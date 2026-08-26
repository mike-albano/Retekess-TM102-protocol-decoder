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


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]

    board, tracker, python_ev, raw_n = [], StateTracker(), [], 0
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
    if n_rand:
        print(f"  ({n_rand} random-mode events on the board only — the Python "
              f"decoder has no random support, so they are excluded)")

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
