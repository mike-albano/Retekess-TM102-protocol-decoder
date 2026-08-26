#!/usr/bin/env python3
"""
live.py — watch the TM102 controller in real time and say what it is doing.

    python3 tools/live.py

Plug in the board, run this, and press buttons. It prints a line every time
the controller changes state:

    19:42:03   GENERAL pressed    -> buzzers armed
    19:42:05   BUZZER 3 buzzed in first
    19:42:07   CLEAR pressed      -> idle

It drives the board itself (locks the channel, starts the addressed capture),
so there is nothing to type. Everything received is also written to a log in
captures/, so a live session is still a capture you can re-analyse later.

Ctrl-C to stop.

The decoding is done by StateTracker from decode_state.py — the same code,
unmodified, that reproduces the button presses in the recorded captures
exactly. This tool is a front end on it, not a second implementation.
"""

import argparse
import datetime as dt
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from decode_state import StateTracker, parse_line          # noqa: E402
from capture import pick_port                              # noqa: E402

try:
    import serial
except ImportError:
    sys.exit("pyserial missing.  Install it with:  pip3 install pyserial")

ROOT = Path(__file__).resolve().parent.parent
CAPTURES = ROOT / "captures"

BAR = "-" * 62


def describe(e):
    if e["event"] == "general":
        return "GENERAL pressed", "-> buzzers armed"
    if e["event"] == "clear":
        return "CLEAR pressed", "-> idle"
    if e["event"] == "buzz":
        return f"BUZZER {e['buzzer']} buzzed in first", ""
    if e["event"] == "initial_state":
        who = f"buzzer {e['buzzer']}" if e["buzzer"] else "nobody yet"
        return "now listening", f"({e['state']}, last winner: {who})"
    return e["event"], ""


def replay(path):
    """Push a recorded log through the live decoder, line by line, exactly as
    the serial reader would. Proves the readout without needing the hardware."""
    tracker, n = StateTracker(), 0
    print(BAR)
    for line in open(path, errors="ignore"):
        r = parse_line(line)
        if not r:
            continue
        n += 1
        for e in tracker.feed(*r):
            what, detail = describe(e)
            print(f"  +{r[0]/1000:9.3f}s   {what:<28} {detail}")
    print(BAR)
    print(f"{n} frames replayed from {path}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--channel", default="50", help="RF_CH to lock (default 50)")
    ap.add_argument("--quiet", action="store_true", help="no periodic status line")
    ap.add_argument("--replay", metavar="LOG",
                    help="replay a recorded capture instead of reading the board. "
                         "Same code path, no hardware needed — use it to check "
                         "the readout against a capture whose answer you know.")
    args = ap.parse_args()

    if args.replay:
        return replay(args.replay)

    port = pick_port(args.port)
    CAPTURES.mkdir(exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = CAPTURES / f"{stamp}_live.log"

    print(f"port {port}   log {path}")
    print("Starting the board. Give it a couple of seconds.\n")

    ser = serial.Serial(port, args.baud, timeout=0.2)
    time.sleep(2.5)                      # opening the port resets the ESP32
    ser.reset_input_buffer()
    for cmd in (f"r{args.channel}", "n"):
        ser.write(cmd.encode() + b"\n")
        time.sleep(0.4)

    tracker = StateTracker()
    frames = last_frames = 0
    started = last_status = time.time()
    warned = False

    print(BAR)
    fh = path.open("w")
    fh.write(f"# TM102 live session port={port} started={stamp}\n")
    try:
        while True:
            raw = ser.readline()
            now = time.time()
            if raw:
                text = raw.decode(errors="replace").rstrip()
                fh.write(text + "\n")
                r = parse_line(text)
                if r:
                    frames += 1
                    for e in tracker.feed(*r):
                        what, detail = describe(e)
                        ts = dt.datetime.now().strftime("%H:%M:%S")
                        print(f"  {ts}   {what:<28} {detail}")

            if now - last_status >= 5:
                fh.flush()
                rate = (frames - last_frames) / (now - last_status)
                last_frames, last_status = frames, now
                if rate < 1:
                    print(f"  ...no frames. Is the controller on? "
                          f"Is it on channel {args.channel}?")
                    warned = True
                elif warned or not args.quiet:
                    st = tracker.cur or {}
                    who = st.get("winner")
                    print(f"  [listening: {rate:4.0f} frames/s  state: "
                          f"{st.get('state','?')}  last winner: "
                          f"{('buzzer %s' % who) if who else 'none'}]")
                    warned = False
    except KeyboardInterrupt:
        pass
    finally:
        fh.close()
        ser.close()

    mins = (time.time() - started) / 60
    print(BAR)
    print(f"{frames} frames over {mins:.1f} min -> {path}")


if __name__ == "__main__":
    main()
