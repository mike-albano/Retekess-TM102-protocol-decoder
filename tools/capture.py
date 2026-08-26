#!/usr/bin/env python3
"""
capture.py — log the sniffer's serial output to a labelled file.

Every capture is an experiment, and an unlabelled log is nearly useless a day
later. This names each run for what you were doing ("buzzer1-press",
"pairing", "idle-baseline") so analyze_capture.py can be pointed at exactly
the right subset.

    pip3 install pyserial

    python3 tools/capture.py --list
    python3 tools/capture.py --label buzzer1-press
    python3 tools/capture.py --label pairing --port /dev/cu.usbserial-0001

Type commands (s, b, d, h, x, i, r35 ...) straight into the terminal; they are
forwarded to the board. Ctrl-C ends the capture.
"""

import argparse
import datetime as dt
import sys
import threading
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial missing.  Install it with:  pip3 install pyserial")

ROOT = Path(__file__).resolve().parent.parent
CAPTURES = ROOT / "captures"


def pick_port(explicit=None):
    if explicit:
        return explicit
    ports = list(list_ports.comports())
    # CP2102 on this board enumerates as usbserial / SLAB_USBtoUART on macOS.
    likely = [p for p in ports
              if any(k in p.device for k in ("usbserial", "SLAB", "wchusb", "ttyUSB"))]
    if len(likely) == 1:
        return likely[0].device
    if not ports:
        sys.exit("No serial ports found.\n"
                 "This board uses a CP2102; macOS sometimes needs Silicon Labs'\n"
                 "VCP driver before a port appears. Check that before the board.")
    print("Multiple ports — choose one with --port:")
    for p in ports:
        print(f"  {p.device:<28} {p.description}")
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--label", default="capture",
                    help="what this run is (e.g. buzzer1-press, pairing, idle)")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    args = ap.parse_args()

    if args.list:
        for p in list_ports.comports():
            print(f"{p.device:<28} {p.description}")
        return

    port = pick_port(args.port)
    CAPTURES.mkdir(exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = CAPTURES / f"{stamp}_{args.label}.log"

    print(f"port  {port} @ {args.baud}")
    print(f"log   {path}")
    print("Type board commands here (? for help). Ctrl-C to stop.\n")

    ser = serial.Serial(port, args.baud, timeout=0.2)
    lines = 0

    def writer():
        for line in sys.stdin:
            ser.write(line.strip().encode() + b"\n")

    threading.Thread(target=writer, daemon=True).start()

    with path.open("w") as fh:
        fh.write(f"# TM102 capture — label={args.label} port={port} started={stamp}\n")
        try:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                text = raw.decode(errors="replace").rstrip()
                print(text)
                fh.write(text + "\n")
                fh.flush()
                lines += 1
        except KeyboardInterrupt:
            pass

    print(f"\n{lines} lines -> {path}")
    print(f"Analyse with:  python3 tools/analyze_capture.py {path}")


if __name__ == "__main__":
    main()
