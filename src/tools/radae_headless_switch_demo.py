#!/usr/bin/env python3
"""
radae_headless_switch_demo.py

Demonstrates switching a running radae_headless process between transmit
and receive mode using signals, instead of restarting it.

radae_headless installs handlers for:
  SIGUSR1  -> switch to transmit mode
  SIGUSR2  -> switch to receive mode

Both --frommic/--toradio and --fromradio/--tospeaker must be present in the
running process's configuration for a switch to succeed; otherwise it logs
an error and exits.

Usage:
  radae_headless_switch_demo.py <pid> [--cycles N] [--interval SECONDS]

Example:
  radae_headless -c my.conf &
  radae_headless_switch_demo.py $!
"""

import argparse
import os
import signal
import sys
import time


def switch_to(pid: int, sig: signal.Signals, label: str, interval: float) -> None:
    print(f"Sending {sig.name} to pid {pid} -> switch to {label}")
    os.kill(pid, sig)
    time.sleep(interval)


def process_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # Process exists but is owned by someone else.
        return True
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int, help="PID of a running radae_headless process")
    parser.add_argument("--cycles", type=int, default=3,
                         help="Number of transmit/receive switch cycles (default: 3)")
    parser.add_argument("--interval", type=float, default=5.0,
                         help="Seconds to stay in each mode before switching (default: 5)")
    args = parser.parse_args()

    if not process_alive(args.pid):
        print(f"Error: no process with pid {args.pid}", file=sys.stderr)
        return 1

    try:
        for cycle in range(1, args.cycles + 1):
            print(f"\n=== Cycle {cycle}/{args.cycles} ===")

            if not process_alive(args.pid):
                print(f"Error: pid {args.pid} is no longer running", file=sys.stderr)
                return 1
            switch_to(args.pid, signal.SIGUSR1, "TRANSMIT", args.interval)

            if not process_alive(args.pid):
                print(f"Error: pid {args.pid} is no longer running", file=sys.stderr)
                return 1
            switch_to(args.pid, signal.SIGUSR2, "RECEIVE", args.interval)
    except KeyboardInterrupt:
        print("\nInterrupted.")
        return 130

    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
