#!/usr/bin/env python3
"""
test_radae_headless_eoo.py

End-to-end test of radae_headless: verifies it can decode a RADE V1
modulated signal (sync + SNR) and correctly decode a callsign carried in
the End-of-Over (EOO) frame.

Rather than feeding rade_decoder/rade_demod internals directly, this drives
the actual radae_headless binary over real PulseAudio devices, the same way
it is used in practice:

  1. rade_modulate encodes a speech WAV into a RADE V1 OFDM signal, with a
     known callsign encoded into the trailing EOO frame.
  2. Two temporary PulseAudio null sinks are created: one to inject the
     modulated audio ("radio in"), one to swallow decoded speech
     ("speaker out").
  3. radae_headless is launched in receive mode pointed at those devices.
  4. The modulated WAV is played into the "radio in" sink.
  5. radae_headless's stderr is captured and checked for a SYNC line and
     an "EOO callsign received: <CALLSIGN>" line matching what was encoded.

Requires: pactl, paplay (PulseAudio or a compatible server, e.g. PipeWire's
pulse module) and a built build/tools/{radae_headless,rade_modulate}.

Usage:
  tests/test_radae_headless_eoo.py [--callsign VK3TPM] [--build-dir ../build]
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser.add_argument("--build-dir", default=os.path.join(repo_root, "build"),
                         help="Path to the cmake build directory (default: ../build)")
    parser.add_argument("--input-wav", default=os.path.join(repo_root, "voice.wav"),
                         help="Speech WAV to modulate (default: voice.wav)")
    parser.add_argument("--callsign", default="VK3TPM",
                         help="Callsign to encode in the EOO frame (default: VK3TPM)")
    parser.add_argument("--play-margin", type=float, default=6.0,
                         help="Seconds to wait after playback for decode/EOO to complete")
    args = parser.parse_args()

    headless_bin = os.path.join(args.build_dir, "tools", "radae_headless")
    modulate_bin = os.path.join(args.build_dir, "tools", "rade_modulate")

    for path in (headless_bin, modulate_bin, args.input_wav):
        if not os.path.isfile(path):
            print(f"Error: required file not found: {path}", file=sys.stderr)
            return 1

    for tool in ("pactl", "paplay"):
        if subprocess.run(["which", tool], stdout=subprocess.DEVNULL).returncode != 0:
            print(f"Error: required tool not found on PATH: {tool}", file=sys.stderr)
            return 1

    tag = uuid.uuid4().hex[:8]
    src_sink = f"radae_test_src_{tag}"
    out_sink = f"radae_test_out_{tag}"
    src_module = None
    out_module = None
    headless_proc = None

    with tempfile.TemporaryDirectory(prefix="radae_headless_eoo_test_") as tmpdir:
        modulated_wav = os.path.join(tmpdir, "modulated.wav")
        conf_file = os.path.join(tmpdir, "test.conf")
        log_file = os.path.join(tmpdir, "radae_headless.log")

        try:
            print(f"Modulating {args.input_wav} with callsign {args.callsign}...")
            run([modulate_bin, "--callsign", args.callsign, args.input_wav, modulated_wav],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            print(f"Creating PulseAudio null sinks {src_sink}, {out_sink}...")
            src_module = load_null_sink(src_sink)
            out_module = load_null_sink(out_sink)

            print("Starting radae_headless in receive mode...")
            with open(log_file, "wb") as log:
                headless_proc = subprocess.Popen(
                    [headless_bin, "-c", conf_file,
                     "--fromradio", f"{src_sink}.monitor",
                     "--tospeaker", out_sink],
                    stdout=log, stderr=subprocess.STDOUT)

            time.sleep(2)  # let it open the audio devices

            print("Playing modulated signal into radio-in sink...")
            run(["paplay", f"--device={src_sink}", modulated_wav])

            time.sleep(args.play_margin)

            headless_proc.send_signal(signal.SIGTERM)
            try:
                headless_proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                headless_proc.kill()
                headless_proc.wait()
            headless_proc = None

            with open(log_file, "r", errors="replace") as f:
                output = f.read()

        finally:
            if headless_proc is not None:
                headless_proc.kill()
                headless_proc.wait()
            if src_module is not None:
                unload_module(src_module)
            if out_module is not None:
                unload_module(out_module)

    synced = "SYNC" in output
    snr_values = [float(v) for v in re.findall(r"SNR:\s*(-?\d+\.\d+)\s*dB", output)]
    max_snr = max(snr_values) if snr_values else None
    callsign_match = re.search(r"EOO callsign received:\s*(\S+)", output)
    decoded_callsign = callsign_match.group(1) if callsign_match else None

    print()
    print("=== Result ===")
    print(f"Sync achieved:       {synced}")
    print(f"Max SNR:             {max_snr if max_snr is not None else 'n/a'} dB")
    print(f"EOO callsign wanted: {args.callsign}")
    print(f"EOO callsign got:    {decoded_callsign if decoded_callsign else 'none'}")

    ok = synced and max_snr is not None and max_snr > 10.0 and decoded_callsign == args.callsign

    if ok:
        print("\nPASS: radae_headless decoded RADE V1 and the EOO callsign correctly.")
        return 0

    print("\nFAIL: radae_headless did not decode as expected.")
    print("\n--- captured output ---")
    print(output)
    return 1


if __name__ == "__main__":
    sys.exit(main())
