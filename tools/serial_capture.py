#!/usr/bin/env python3
"""Capture ESP32 serial output to standard output.

Example:
    python3 tools/serial_capture.py --port /dev/ttyUSB0 --seconds 120
"""

import argparse
import sys
import time

import serial


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Capture serial logs from an ESP32.")
    parser.add_argument("--port", required=True, help="Serial device, e.g. /dev/ttyUSB0 or COM4")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument(
        "--seconds",
        type=float,
        default=120,
        help="Capture duration in seconds (default: 120)",
    )
    args = parser.parse_args()
    if args.baud <= 0:
        parser.error("--baud must be positive")
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    return args


def main() -> int:
    args = parse_args()
    deadline = time.monotonic() + args.seconds

    try:
        with serial.Serial(args.port, args.baud, timeout=0.2) as device:
            print(
                f"Capturing {args.port} at {args.baud} baud for {args.seconds:g}s...",
                file=sys.stderr,
                flush=True,
            )
            while time.monotonic() < deadline:
                data = device.read(4096)
                if data:
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()
    except serial.SerialException as exc:
        print(f"Unable to open {args.port}: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
