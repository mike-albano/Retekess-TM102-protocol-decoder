#!/usr/bin/env python3
"""
Turn a TM102 capture into a stream of controller events.

Handles both capture styles:

  * promiscuous / lock-on  (LEN 32, frame starts 18 3A) — the two leading bytes
    are the tail of the address, so the payload is bytes 2..9 and the 2-byte
    trailer is bytes 10..11. Everything past that is post-packet noise.
  * matched receive        (LEN <= 12) — the radio has already stripped the
    address and validated the checksum, so the whole frame is payload.

Payload layout (see FINDINGS.md, "The frame"):

    p0 p1 p2 p3 p4 p5 p6 p7
       |     |  |  |
       |     |  |  +-- 0x73 constant
       |     |  +----- event code: 0x81 / 0x8C on a button press
       |     +-------- 0x00 on a button press, a counter otherwise
       +-------------- message class

Usage:
    tools/decode_events.py captures/xxx.log            # human readable
    tools/decode_events.py captures/xxx.log --json     # one JSON object per line
    tools/decode_events.py captures/xxx.log --summary  # histogram of codes
"""
import re, sys, json, os
from collections import Counter

FRAME = re.compile(r"HEX: ([0-9A-F ]+) \| T:(\d+)")
MAPFILE = os.path.join(os.path.dirname(__file__), "..", "event_map.json")

# Each message is sent twice ~11 ms apart. Anything closer than this is the
# same press, not a new one.
DUP_WINDOW_MS = 25


def load_map():
    try:
        with open(MAPFILE) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def parse(path):
    out = []
    for line in open(path, errors="ignore"):
        m = FRAME.search(line)
        if not m:
            continue
        b = bytes(int(x, 16) for x in m.group(1).split())
        t = int(m.group(2))
        if len(b) >= 12 and b[0] == 0x18 and b[1] == 0x3A:
            p, trailer, kind = b[2:10], b[10:12], "locked"
        elif len(b) <= 12:
            p, trailer, kind = b[:8], b[8:10], "addressed"
        else:
            continue        # 32-byte frame not starting 18 3A: promiscuous junk
        # Sanity gate. p1 and p5 are 0x7x in every real frame ever captured;
        # noise that slips past the address correlator does not respect that.
        # p1 and p5 are 0x7x in every real frame. Requiring EITHER (not both)
        # is enough to reject noise — which matches neither — while tolerating
        # a corrupted byte in a real frame, and real frames are scarce.
        if len(p) < 6 or ((p[1] & 0xF0) != 0x70 and (p[5] & 0xF0) != 0x70):
            continue
        out.append((t, p, trailer, kind))
    return out


def events(frames):
    ev, last = [], {}
    for t, p, trailer, kind in frames:
        if len(p) < 6 or p[3] != 0x00:
            continue
        code = p[4]
        if code in (0x00, 0xFF):
            continue
        prev = last.get(code)
        dup = prev is not None and t - prev <= DUP_WINDOW_MS
        last[code] = t
        if dup:
            # The second copy of a message sent twice. Not a new press — but
            # its existence is evidence the first one was real, so record it.
            if ev:
                ev[-1]["confirmed"] = True
            continue
        ev.append({
            "t_ms": t,
            "code": f"{code:02X}",
            "class": f"{p[1]:02X}",
            "trailer": trailer.hex().upper(),
            "payload": p.hex().upper(),
            "source": kind,
            "confirmed": False,   # True once the 11 ms repeat is seen
        })
    return ev


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    if not args:
        print(__doc__)
        return 1
    frames = parse(args[0])
    ev = events(frames)
    labels = load_map()
    for e in ev:
        e["label"] = labels.get(e["code"], {}).get("label", f"unknown-{e['code']}")

    if "--summary" in flags:
        print(f"frames: {len(frames)}   events: {len(ev)}")
        if ev:
            span = (ev[-1]["t_ms"] - ev[0]["t_ms"]) / 1000
            print(f"span: {span:.1f} s")
        for code, n in Counter(e["code"] for e in ev).most_common():
            lab = labels.get(code, {}).get("label", "UNLABELLED")
            cls = Counter(e["class"] for e in ev if e["code"] == code).most_common(1)[0][0]
            tr = Counter(e["trailer"] for e in ev if e["code"] == code).most_common(1)[0][0]
            conf = sum(1 for e in ev if e["code"] == code and e["confirmed"])
            print(f"  {code}  x{n:<4} {conf:>3} confirmed   class {cls}  "
                  f"trailer {tr}   {lab}")
        return 0

    if "--json" in flags:
        for e in ev:
            print(json.dumps(e))
        return 0

    t0 = ev[0]["t_ms"] if ev else 0
    for e in ev:
        mark = "**" if e["confirmed"] else "  "
        print(f"{(e['t_ms']-t0)/1000:8.3f}s {mark} {e['code']}  class {e['class']}  "
              f"trailer {e['trailer']}  {e['label']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
