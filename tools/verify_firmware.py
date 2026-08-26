#!/usr/bin/env python3
"""
Check the on-board decoder against the Python one, on the same stream.

The firmware's event logic is a port of decode_state.py, and a port can drift
from its original in ways that only show up in the case you did not think to
test. This removes the guesswork: run the board with raw echo on (`v`), so a
session contains BOTH the board's JSON events and the raw frames they were
derived from. Feed the raw frames to the Python decoder and compare.

Same input, two independent implementations, exact comparison.

    ./go.sh capture fwcheck        # then type:  e   v
    ...play a few rounds...
    python3 tools/verify_firmware.py captures/<that file>.log

Name differences, which are deliberate: the Python calls the answered->armed
transition `rearmed` (neutral, since it is occasionally spontaneous) while the
firmware calls it `lock` (the button that normally causes it). Likewise
`initial_state` and `ready`.
"""
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from decode_state import StateTracker, parse_line          # noqa: E402

ALIAS = {"rearmed": "lock", "initial_state": "ready"}

# Random mode has no state signature, so decode_state.py does not handle it and
# cannot be used to check it. This is a second, independent implementation of
# the same spec — the latch behaviour the firmware applies to the repeating
# 0x8F / 0x95 / 0x8C command codes — written from the FINDINGS description
# rather than from the C++, so that agreement means something.
RANDOM_CMDS = {0x8F: "random_start", 0x95: "random_pick", 0x8C: "random_end"}


def expected_random(frames):
    out, in_random, picked = [], False, False
    for b in frames:
        if b[3] != 0x00:
            continue
        c = b[4]
        if c == 0x8F and not in_random:
            out.append("random_start"); in_random, picked = True, False
        elif c == 0x95 and in_random and not picked:
            out.append("random_pick"); picked = True
        elif c == 0x8C and in_random:
            out.append("random_end"); in_random, picked = False, False
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]

    board, tracker, python_ev, raw_n, raw_frames = [], StateTracker(), [], 0, []
    for line in open(path, errors="ignore"):
        line = line.strip()
        if line.startswith("{"):
            try:
                o = json.loads(line)
            except ValueError:
                continue
            if o.get("ev") not in (None, "boot"):
                board.append(o)
            continue
        r = parse_line(line)
        if r:
            raw_n += 1
            raw_frames.append(r[1])
            for e in tracker.feed(*r):
                python_ev.append(e)

    if not raw_n:
        print("No raw frames in this log. Did you type 'v' to turn on raw echo?")
        return 1
    if not board:
        print("No board events in this log. Did you type 'e'?")
        return 1

    a = [ALIAS.get(e["event"], e["event"]) for e in python_ev]
    b = [e["ev"] for e in board]
    ab = [e.get("buzzer") for e in python_ev]
    bb = [e.get("buzzer") for e in board]

    print(f"raw frames        : {raw_n}")
    print(f"python events     : {len(a)}")
    print(f"board events      : {len(b)}")

    # Board events the Python cannot produce: it has no random handling.
    RANDOM = {"random_start", "random_pick", "random_end"}
    b_core = [(x, y) for x, y in zip(b, bb) if x not in RANDOM]
    n_rand = len(b) - len(b_core)
    board_rand = [e["ev"] for e in board if e["ev"] in RANDOM]
    want_rand = expected_random(raw_frames)
    if board_rand or want_rand:
        rand_ok = board_rand == want_rand
        print(f"\nrandom-mode events   : board {board_rand}")
        print(f"                       expected {want_rand}")
        print("  " + ("random events MATCH" if rand_ok else "  <<< RANDOM MISMATCH"))
    else:
        print("\n  no random-mode events in this session — that path is UNVERIFIED.")
        print("  To cover it: press Random twice, then Clear, a few times.")
    if n_rand:
        pass

    ok = len(a) == len(b_core) and all(
        x == y and p == q for (x, p), (y, q) in zip(zip(a, ab), b_core))
    print()
    if ok:
        print(f"MATCH — {len(a)} events, identical in order, type and handset.")
        return 0

    print("MISMATCH:")
    for i in range(max(len(a), len(b_core))):
        pa = f"{a[i]}({ab[i]})" if i < len(a) else "-"
        pb = f"{b_core[i][0]}({b_core[i][1]})" if i < len(b_core) else "-"
        print(f"  {i:3d}  python {pa:22s}  board {pb:22s}"
              + ("" if pa == pb else "   <<<"))
    return 1


if __name__ == "__main__":
    sys.exit(main())
