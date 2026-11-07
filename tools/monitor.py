#!/usr/bin/env python3
"""Show the board's live readings and save them to logs/.

Run from the project root, with the board on USB:

    uv run --no-project --with pyserial tools/monitor.py

Stop with Ctrl-C. Connecting can restart the board, which is harmless.
"""
import datetime
import glob
import pathlib
import sys

import serial


def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("No board found. Check the USB cable, or pass the port as an argument.")
    return ports[0]


def main():
    port = find_port()
    logs = pathlib.Path("logs")
    logs.mkdir(exist_ok=True)
    path = logs / datetime.datetime.now().strftime("run_%Y%m%d_%H%M%S.csv")
    print(f"Reading {port}, saving to {path}")

    with serial.Serial(port, 115200, timeout=1) as link, open(path, "w", buffering=1) as out:
        try:
            while True:
                line = link.readline().decode("utf-8", "replace").strip()
                if not line:
                    continue
                if not line.startswith("DOA,"):
                    print(line)
                    continue
                out.write(line[4:] + "\n")
                fields = line.split(",")
                if fields[1] == "ms":
                    continue
                _, ms, sound, _, bearing, valid, conf, tau, left, right, dsp, _ = fields
                seen = f"{float(bearing):+6.1f} deg  conf {conf}  {float(tau):+6.1f} us" if valid == "1" else "listening"
                mark = "*" if sound == "1" else " "
                print(f"\r{mark} {seen:38s} L {left:>6} dB  R {right:>6} dB  dsp {dsp} us   ", end="", flush=True)
        except KeyboardInterrupt:
            print(f"\nSaved {path}")


if __name__ == "__main__":
    main()
