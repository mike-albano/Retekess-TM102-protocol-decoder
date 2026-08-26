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
from decode_state import StateTracker, parse_line, BUZZER_BASE   # noqa: E402
from capture import pick_port                              # noqa: E402

try:
    import serial
except ImportError:
    sys.exit("pyserial missing.  Install it with:  pip3 install pyserial")

ROOT = Path(__file__).resolve().parent.parent
CAPTURES = ROOT / "captures"

BAR = "-" * 62


def parse_expect(spec):
    """'1-8' or '3,4,7' or '1-3,8' -> [1,2,3,...]"""
    out = []
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            a, b = part.split("-")
            out.extend(range(int(a), int(b) + 1))
        elif part:
            out.append(int(part))
    return out


class Verifier:
    """Checks each buzz against the handset the operator says they pressed.

    The operator declares the order up front, so unlike the plain session
    summary this can actually fail. One buzz is taken per round - the first -
    and the expectation only advances on Clear, so a handset pressed twice
    inside one round (which happens; the controller re-arms on its own) does
    not skip an entry.
    """

    def __init__(self, expect):
        self.expect = expect
        self.i = 0
        self.round_seen = None
        self.results = []

    def done(self):
        return self.i >= len(self.expect)

    def prompt(self):
        if self.done():
            return None
        n = self.expect[self.i]
        return (f"  >>> round {self.i+1} of {len(self.expect)}:  "
                f"General, then HANDSET {n}, then Clear")

    def on_buzz(self, e):
        if self.done() or self.round_seen is not None:
            return None
        want = self.expect[self.i]
        got = e["buzzer"]
        self.round_seen = (want, got, e["p6"])
        ok = got == want
        mark = "PASS" if ok else "FAIL"
        detail = (f"handset {want} -> reads as {got}" if not ok
                  else f"handset {want}, p6 = 0x{e['p6']}")
        return f"  [{mark}] {detail}"

    def on_clear(self):
        if self.round_seen is None:
            return None
        self.results.append(self.round_seen)
        self.round_seen = None
        self.i += 1
        return self.prompt()

    def report(self):
        if not self.results:
            return ["  nothing verified"]
        out = [BAR, "  VERIFICATION", ""]
        bad = 0
        for want, got, p6 in self.results:
            ok = want == got
            bad += not ok
            out.append(f"    handset {want:<3} p6 = 0x{p6}   "
                       f"{'PASS' if ok else 'FAIL  (decoded as %s)' % got}")
        out.append("")
        n = len(self.results)
        out.append(f"  {n - bad} of {n} correct"
                   + ("" if bad else "  — the map is confirmed end to end"))
        untested = self.expect[self.i:]
        if untested:
            out.append(f"  not reached: {', '.join(str(x) for x in untested)}")
        return out


def describe(e):
    if e["event"] == "general":
        return "GENERAL pressed", "-> buzzers armed"
    if e["event"] == "clear":
        return "CLEAR pressed", "-> idle"
    if e["event"] == "buzz":
        if e["buzzer"] is None:
            return "BUZZER ? buzzed in first", f"(unrecognised p6 = 0x{e['p6']})"
        return f"BUZZER {e['buzzer']} buzzed in first", f"(p6 = 0x{e['p6']})"
    if e["event"] == "initial_state":
        who = f"buzzer {e['buzzer']}" if e["buzzer"] else "nobody yet"
        return "now listening", f"({e['state']}, last winner: {who})"
    return e["event"], ""


def replay(path):
    """Push a recorded log through the live decoder, line by line, exactly as
    the serial reader would. Proves the readout without needing the hardware."""
    tracker, n, seen = StateTracker(), 0, {}
    print(BAR)
    for line in open(path, errors="ignore"):
        r = parse_line(line)
        if not r:
            continue
        n += 1
        for e in tracker.feed(*r):
            if e["event"] == "buzz":
                k = int(e["p6"], 16)
                b_, c = seen.get(k, (e["buzzer"], 0))
                seen[k] = (b_, c + 1)
            what, detail = describe(e)
            print(f"  +{r[0]/1000:9.3f}s   {what:<28} {detail}")
    print(BAR)
    print(f"{n} frames replayed from {path}")
    for p6 in sorted(seen):
        b_, c = seen[p6]
        print(f"  p6 = 0x{p6:02X}  ->  {('buzzer %s' % b_) if b_ else 'unrecognised':<14} x{c}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--channel", default="50", help="RF_CH to lock (default 50)")
    ap.add_argument("--quiet", action="store_true", help="no periodic status line")
    ap.add_argument("--verify", nargs="?", const="1-8", metavar="LIST",
                    help="walk through handsets and check each one against the "
                         "map. Default 1-8; or give a list like 3,4,7.")
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
    verifier = Verifier(parse_expect(args.verify)) if args.verify else None
    seen = {}
    frames = last_frames = 0
    started = last_status = time.time()
    warned = False

    print(BAR)
    if verifier:
        print(verifier.prompt())
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
                        if e["event"] == "buzz":
                            k = int(e["p6"], 16)
                            n, c = seen.get(k, (e["buzzer"], 0))
                            seen[k] = (n, c + 1)
                        what, detail = describe(e)
                        ts = dt.datetime.now().strftime("%H:%M:%S")
                        print(f"  {ts}   {what:<28} {detail}")
                        if verifier:
                            line = (verifier.on_buzz(e) if e["event"] == "buzz"
                                    else verifier.on_clear()
                                    if e["event"] == "clear" else None)
                            if line:
                                print(line)
                            if verifier.done() and e["event"] == "clear":
                                raise KeyboardInterrupt

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
    if verifier:
        for line in verifier.report():
            print(line)
    print(BAR)
    print(f"{frames} frames over {mins:.1f} min -> {path}")
    if seen:
        # Only the operator knows which handset was actually pressed, so this
        # cannot self-verify — it lists what was observed, in p6 order, for
        # comparison against the order the handsets were pressed in.
        print("\nHandsets seen this session, in p6 order:")
        for p6 in sorted(seen):
            n, count = seen[p6]
            who = f"buzzer {n}" if n else "UNRECOGNISED"
            print(f"  p6 = 0x{p6:02X}  ->  {who:<14} x{count}")


if __name__ == "__main__":
    main()
