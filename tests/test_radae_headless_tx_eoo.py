#!/usr/bin/env python3
"""
test_radae_headless_tx_eoo.py

End-to-end test that radae_headless correctly transmits the callsign given
via --call in the End-of-Over (EOO) frame it sends when a transmission
stops.

Drives the actual radae_headless binary over real PulseAudio devices, the
same way it is used in practice:

  1. Two temporary PulseAudio null sinks are created: one to feed silence
     as the "microphone" input, one to capture whatever radae_headless
     plays out as "radio out".
  2. parecord captures the radio-out sink's monitor to a WAV file for the
     whole run.
  3. radae_headless is launched in transmit mode with --call <CALLSIGN>,
     left running briefly, then sent SIGTERM - which makes it flush the
     EOO frame (carrying the callsign) before exiting.
  4. The captured WAV is decoded with rade_demod (a file-based RADE
     demodulator that is independent of radae_headless and audio devices),
     and its stderr is checked for "Callsign = '<CALLSIGN>'".

Requires: pactl, parecord (PulseAudio or a compatible server, e.g.
PipeWire's pulse module) and a built
build/tools/{radae_headless,rade_demod}.

Usage:
  tests/test_radae_headless_tx_eoo.py [--callsign VK3TPM] [--build-dir ../build]
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
import uuid


def run(cmd, **kwargs):
    return subprocess.run(cmd, check=True, **kwargs)


def unload_module(module_id):
    subprocess.run(["pactl", "unload-module", str(module_id)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def load_null_sink(sink_name):
    result = run(["pactl", "load-module", "module-null-sink",
                  f"sink_name={sink_name}",
                  f"sink_properties=device.description={sink_name}"],
                 stdout=subprocess.PIPE, text=True)
    return int(result.stdout.strip())


def stop_process(proc, term_timeout=10):
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=term_timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser.add_argument("--build-dir", default=os.path.join(repo_root, "build"),
                         help="Path to the cmake build directory (default: ../build)")
    parser.add_argument("--callsign", default="VK3TPM",
                         help="Callsign to pass to radae_headless --call (default: VK3TPM)")
    parser.add_argument("--tx-seconds", type=float, default=5.0,
                         help="Seconds to let radae_headless transmit before stopping it")
    parser.add_argument("--drain-margin", type=float, default=2.0,
                         help="Seconds to keep capturing after radae_headless exits")
    args = parser.parse_args()

    headless_bin = os.path.join(args.build_dir, "tools", "radae_headless")
    demod_bin = os.path.join(args.build_dir, "tools", "rade_demod")

    for path in (headless_bin, demod_bin):
        if not os.path.isfile(path):
            print(f"Error: required file not found: {path}", file=sys.stderr)
            return 1

    for tool in ("pactl", "parecord"):
        if subprocess.run(["which", tool], stdout=subprocess.DEVNULL).returncode != 0:
            print(f"Error: required tool not found on PATH: {tool}", file=sys.stderr)
            return 1

    tag = uuid.uuid4().hex[:8]
    mic_sink = f"radae_tx_mic_{tag}"
    radio_sink = f"radae_tx_radio_{tag}"
    mic_module = None
    radio_module = None
    headless_proc = None
    parecord_proc = None

    with tempfile.TemporaryDirectory(prefix="radae_headless_tx_eoo_test_") as tmpdir:
        conf_file = os.path.join(tmpdir, "test.conf")
        headless_log = os.path.join(tmpdir, "radae_headless.log")
        captured_wav = os.path.join(tmpdir, "captured.wav")
        decoded_wav = os.path.join(tmpdir, "decoded.wav")

        try:
            print(f"Creating PulseAudio null sinks {mic_sink}, {radio_sink}...")
            mic_module = load_null_sink(mic_sink)
            radio_module = load_null_sink(radio_sink)

            print("Starting capture of radio-out sink...")
            parecord_proc = subprocess.Popen(
                ["parecord", f"--device={radio_sink}.monitor",
                 "--file-format=wav", captured_wav],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1)  # let capture start before any audio is produced

            print(f"Starting radae_headless in transmit mode with --call {args.callsign}...")
            with open(headless_log, "wb") as log:
                headless_proc = subprocess.Popen(
                    [headless_bin, "-c", conf_file,
                     "--frommic", f"{mic_sink}.monitor",
                     "--toradio", radio_sink,
                     "-t", "--call", args.callsign],
                    stdout=log, stderr=subprocess.STDOUT)

            time.sleep(args.tx_seconds)

            print("Stopping radae_headless (should send EOO frame)...")
            stop_process(headless_proc)
            headless_proc = None

            time.sleep(args.drain_margin)

            print("Stopping capture...")
            parecord_proc.send_signal(signal.SIGINT)
            try:
                parecord_proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                parecord_proc.kill()
                parecord_proc.wait()
            parecord_proc = None

            with open(headless_log, "r", errors="replace") as f:
                headless_output = f.read()

            print(f"Decoding captured signal with rade_demod...")
            demod_result = subprocess.run(
                [demod_bin, captured_wav, decoded_wav],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            demod_output = demod_result.stdout

        finally:
            if headless_proc is not None:
                headless_proc.kill()
                headless_proc.wait()
            if parecord_proc is not None:
                parecord_proc.kill()
                parecord_proc.wait()
            if mic_module is not None:
                unload_module(mic_module)
            if radio_module is not None:
                unload_module(radio_module)

    eoo_sent = "sending eoo frame" in headless_output.lower()
    callsign_match = re.search(r"Callsign = '([^']*)'", demod_output)
    decoded_callsign = callsign_match.group(1) if callsign_match else None

    print()
    print("=== Result ===")
    decoded_display = repr(decoded_callsign) if decoded_callsign is not None else "not decoded"
    print(f"EOO frame sent by TX:  {eoo_sent}")
    print(f"Callsign wanted:       {args.callsign!r}")
    print(f"Callsign decoded:      {decoded_display}")

    ok = eoo_sent and decoded_callsign == args.callsign

    if ok:
        print("\nPASS: radae_headless transmitted the given EOO callsign correctly.")
        return 0

    print("\nFAIL: radae_headless did not transmit the expected EOO callsign.")
    print("\n--- radae_headless output ---")
    print(headless_output)
    print("\n--- rade_demod output ---")
    print(demod_output)
    return 1


if __name__ == "__main__":
    sys.exit(main())
