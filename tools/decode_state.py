#!/usr/bin/env python3
"""
Decode a TM102 capture into the event stream the project is for:
which controller button was pressed, and which buzzer answered.

This reads STATE from the beacon rather than watching for event frames.
The controller restates its full state ~30 times a second, whereas a button
event is transmitted only twice and is easy to miss — several were lost in
every capture so far. Reading the beacon makes detection robust.

Payload bytes used (see FINDINGS.md, "The frame"):

    p1  0x76 = idle,  0x77 = armed
    p3  \ both 0x00 while a buzz-in stands unanswered; anything else means
    p4  / the round is still open
    p6  0xFF = nobody has answered
        otherwise  p6 = 0x90 + (n XOR 4)  — see below, it is not a plain
        counter and assuming it was cost us four wrong predictions

Three states, not two: idle -> armed -> answered -> idle. The "answered" state
must be read from p3/p4, NOT from p6 changing. p6 latches the last winner, so
if the same buzzer wins twice in a row it never changes and a p6-only decoder
silently misses the second win. That is a real failure that showed up in live
use, not a hypothetical.

The controller also changes how often it transmits, which corroborates the
state: ~35 frames/s idle, ~148 armed and waiting, ~6 once someone has answered.

Bit errors are real, and the dangerous one is cheap: armed (0x77) becomes idle
(0x76) by dropping a single bit, which is the dominant failure mode. So a
change is only accepted once CONFIRM frames in a row agree.

CONFIRM was chosen by sweeping it against three captures with known ground
truth (5/5, 25/25, 25/25 button presses). 2 and 3 produce phantom transitions;
4 and above are exact on all three. 5 is used, for margin. The cost is about
55 ms of latency at the observed ~90 frames/s.

Usage:
    tools/decode_state.py captures/xxx.log            # readable timeline
    tools/decode_state.py captures/xxx.log --json     # one JSON object per line
"""
import os, re, sys, json

FRAME = re.compile(r"HEX: ([0-9A-F ]+) \| T:(\d+)")
CONFIRM = 5          # frames needed to accept an idle <-> armed change
CONFIRM_ANSWERED = 3 # ... and to accept a buzz-in
# Buzzer identity in p6.
#
# The obvious reading of the first three handsets was p6 = 0x94 + n, and it
# held for buzzers 1, 2, 3 and 8. It is wrong. Measured across all eight:
#
#     n   1     2     3     4     5     6     7     8
#     p6  0x95  0x96  0x97  0x90  0x91  0x92  0x93  0x9C
#
# The low nibble is n with bit 2 inverted — n XOR 4 — which fits all eight
# exactly. Handsets 1, 2, 3 and 8 have bit 2 clear, so for them XOR 4 and + 4
# give the same answer; that coincidence is what made the linear rule look
# right on the sample we happened to test first.
#
# Why bit 2 is stored inverted is unknown. The rule is measured for n = 1-8
# only; buzzer numbers go to 32 via the host's F2 menu, and nothing here has
# been checked above 8.
BUZZER_BASE = 0x90
BUZZER_XOR = 0x04
P6_MAX = 0xB4               # n = 32 -> 0x90 + (32 XOR 4) = 0xB4

MAPFILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "event_map.json")


def _load_measured():
    """Measured p6 -> handset number, from event_map.json.

    The table is the authority and the formula is only an extrapolation for
    handsets nobody has pressed yet. That ordering matters: the formula has
    already been wrong once, and a measurement should never be overridden by
    a rule inferred from it.
    """
    try:
        with open(MAPFILE) as fh:
            m = json.load(fh)["winner"]["_measured"]
        return {int(v, 16): int(n) for n, v in m.items()}
    except Exception:
        return {}


MEASURED = _load_measured()

def classify(b):
    """Payload -> game state, or None if this frame does not say."""
    if b[1] == 0x76:
        return "idle"
    if b[1] == 0x77:
        # p3 and p4 both zero marks a standing answer. A 16-bit all-zero
        # pattern is not something the slicer produces by accident: its errors
        # clear single bits, and 0xFE FE would need fifteen of them.
        return "answered" if (b[3] == 0x00 and b[4] == 0x00) else "armed"
    return None


def parse_line(line):
    """One serial line -> (t_ms, payload) for a real controller frame, else None."""
    m = FRAME.search(line)
    if not m:
        return None
    b = bytes(int(x, 16) for x in m.group(1).split())
    if len(b) < 8:
        return None
    if len(b) >= 12 and b[0] == 0x18 and b[1] == 0x3A:
        b = b[2:]                       # strip address tail from lock-on captures
    if (b[1] & 0xF0) != 0x70 and (b[5] & 0xF0) != 0x70:
        return None                     # not a controller frame
    return int(m.group(2)), b


def frames(path):
    for line in open(path, errors="ignore"):
        r = parse_line(line)
        if r:
            yield r


def winner(p6):
    # Buzzer numbers run 1-32 (the host's F2 "learning number" range), so a
    # winner occupies 0x90-0xB4. Anything above that is 0xFF - nobody has
    # answered - with bits knocked out of it by the slicer.
    if p6 in MEASURED:
        return MEASURED[p6]
    if not BUZZER_BASE <= p6 <= P6_MAX:
        return None
    n = (p6 - BUZZER_BASE) ^ BUZZER_XOR
    return n if 1 <= n <= 32 else None


class StateTracker:
    """Feed frames in, get events out. Used by both the log decoder and the
    live monitor, so the thing running live is the thing that was validated."""

    def __init__(self, confirm=None, confirm_answered=None):
        self.confirm = CONFIRM if confirm is None else confirm
        self.confirm_answered = (CONFIRM_ANSWERED if confirm_answered is None
                                 else confirm_answered)
        self.cur = None
        self.pending = None
        self.n_same = 0

    def feed(self, t, b):
        obs = {"state": classify(b), "winner": winner(b[6])}
        if obs["state"] is None:
            return []
        if obs == self.pending:
            self.n_same += 1
        else:
            self.pending, self.n_same = obs, 1
        # Two thresholds, because the two transitions fail in opposite ways.
        # idle <-> armed rides on one bit of p1 (0x77 -> 0x76 is a single
        # dropped bit, the dominant error), so it needs several frames to
        # confirm. A buzz-in is marked by p3 and p4 both zero — sixteen bits
        # that corruption cannot fabricate — and the controller sends only
        # ~6 frames/s in that state, so waiting for five would miss short
        # rounds. Strong marker, low threshold; weak marker, high threshold.
        need = (self.confirm_answered if obs["state"] == "answered"
                else self.confirm)
        if self.n_same < need or obs == self.cur:
            return []
        if self.cur is None:
            # First confirmed reading. This is the state we joined mid-game,
            # not something that just happened - do not report it as an event.
            self.cur = dict(obs)
            return [{"t_ms": t, "event": "initial_state",
                     "state": obs["state"], "buzzer": obs["winner"]}]
        out, was = [], self.cur["state"]
        now_ = obs["state"]
        if now_ != was:
            if now_ == "armed" and was == "idle":
                out.append({"t_ms": t, "event": "general", "state": "armed"})
            elif now_ == "answered":
                # Carry the raw p6 as well. If a handset ever reports a value
                # outside 0x95-0xB4 the decoded number is None, and without the
                # raw byte the event would say nothing useful about why.
                out.append({"t_ms": t, "event": "buzz",
                            "buzzer": obs["winner"], "p6": f"{b[6]:02X}"})
            elif now_ == "idle":
                out.append({"t_ms": t, "event": "clear", "state": "idle"})
            elif now_ == "armed" and was == "answered":
                # Re-armed without a Clear. Not part of the documented game
                # flow; surfaced rather than swallowed.
                out.append({"t_ms": t, "event": "rearmed", "state": "armed"})
        elif obs["winner"] != self.cur["winner"] and obs["winner"] is not None:
            # Winner changed without a state change — should not happen now
            # that buzzes are detected from p3/p4, so say so if it does.
            out.append({"t_ms": t, "event": "winner_changed",
                        "buzzer": obs["winner"]})
        self.cur = dict(obs)
        return out


def decode(path, confirm=None, confirm_answered=None):
    tracker = StateTracker(confirm, confirm_answered)
    out = []
    for t, b in frames(path):
        out.extend(tracker.feed(t, b))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 1
    ev = decode(args[0])
    if "--json" in sys.argv:
        for e in ev:
            print(json.dumps(e))
        return 0
    t0 = ev[0]["t_ms"] if ev else 0
    for e in ev:
        if e["event"] == "buzz":
            extra = f"buzzer {e['buzzer']}"
        elif e["event"] == "initial_state":
            extra = f"{e['state']}, winner {e['buzzer'] or 'none'}"
        else:
            extra = e.get("state", "")
        print(f"{(e['t_ms']-t0)/1000:8.3f}s  {e['event']:<8} {extra}")
    print(f"\n{len(ev)} events")
    return 0


if __name__ == "__main__":
    sys.exit(main())
