#!/usr/bin/env python3
"""
analyze_capture.py — find the Enhanced ShockBurst address hiding in a
promiscuous nRF24 capture.

WHY THIS IS NEEDED
------------------
The Retekess radio is an Si24R1, an nRF24L01+ clone, so the air protocol is
ordinary ESB:

    preamble(1) | address(3-5) | packet-control(9 bits) | payload | CRC(1-2)

To capture without knowing the address we misconfigure the receiver: 2-byte
address width, CRC off, bait address 0x0000 or 0xAAAA.  Ambient noise then
trips the sync detector at an arbitrary bit offset and the chip shifts 32 raw
bytes into the FIFO.

So every captured line is a bit-shifted window onto the air, and the real
address sits somewhere inside at an unknown bit alignment.  This tool undoes
that: it re-aligns every capture eight ways and looks for byte sequences that
recur far more often than chance allows.  A genuine address recurs in every
packet the network sends; noise does not.

USAGE
    python3 tools/analyze_capture.py captures/whatever.log
    python3 tools/analyze_capture.py captures/*.log --width 4 --top 25
"""

import argparse
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

# Matches the firmware's output line:
#   [CH: 35] LEN: 32 | HEX: 1D 7C 36 ... | T:12345
LINE_RE = re.compile(
    r"\[CH:\s*(\d+)\]\s*LEN:\s*(\d+)\s*\|\s*HEX:\s*([0-9A-Fa-f ]+?)\s*"
    r"(?:\|\s*T:(\d+))?(?:\s*RPD:([01]))?\s*$"
)

# Bytes that cannot legally start an ESB address on this silicon.  The Si24R1
# datasheet forbids 0x00, 0xFF, 0xA5, 0x5A, 0xAA, 0x55 as the top address byte,
# which conveniently also describes most of what noise produces.
FORBIDDEN_FIRST = {0x00, 0xFF, 0xA5, 0x5A, 0xAA, 0x55}


def parse(paths, strong_only=False):
    """Read capture files into (channel, bytes) tuples.

    Packets tagged RPD:1 were captured while a signal stronger than -64 dBm was
    present — i.e. on top of a real transmission rather than thermal noise.
    Filtering on that is by far the most effective way to cut a haystack down.
    """
    packets, skipped, strong, tagged = [], 0, 0, 0
    for p in paths:
        for line in Path(p).read_text(errors="replace").splitlines():
            m = LINE_RE.search(line.strip())
            if not m:
                skipped += 1
                continue
            rpd = m.group(5)
            if rpd is not None:
                tagged += 1
                if rpd == "1":
                    strong += 1
                elif strong_only:
                    continue
            ch = int(m.group(1))
            data = bytes(int(b, 16) for b in m.group(3).split())
            packets.append((ch, data))
    return packets, skipped, strong, tagged


def to_bits(data: bytes) -> str:
    return "".join(f"{b:08b}" for b in data)


def bits_to_bytes(bits: str) -> bytes:
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def degenerate(seq: bytes) -> bool:
    """Reject candidates that are structure or noise, not identity."""
    if len(set(seq)) == 1:                       # AA AA AA
        return True
    if seq[0] in FORBIDDEN_FIRST:                # illegal as a top address byte
        return True
    if all(b in (0x55, 0xAA) for b in seq):      # more preamble, not an address
        return True
    bits = to_bits(seq)
    transitions = sum(1 for a, b in zip(bits, bits[1:]) if a != b)
    return transitions < len(bits) // 5          # a run, not an address


# Enhanced ShockBurst emits a one-byte preamble immediately before the address,
# and the chip picks it from the address's first bit: leading 1 -> 0xAA,
# leading 0 -> 0x55.  That gives us a hard anchor.  Without it we would rank the
# true address and its seven bit-shifted aliases identically and have no way to
# tell which is real.
PREAMBLES = {"10101010": "1", "01010101": "0"}


def find_candidates(packets, width, top, loose=False):
    """Rank address candidates by how many DISTINCT packets contain them.

    Counting distinct packets rather than raw occurrences matters: a sequence
    repeated 20 times inside one noisy capture is far weaker evidence than one
    appearing once in each of 20 separate captures.
    """
    packet_hits = Counter()
    where = defaultdict(set)
    need = width * 8

    for ch, data in packets:
        bits = to_bits(data)
        seen_here = set()

        for off in range(len(bits) - need - 8 + 1):
            if not loose:
                pre = bits[off:off + 8]
                lead = PREAMBLES.get(pre)
                if lead is None:
                    continue
                addr_bits = bits[off + 8:off + 8 + need]
                # The preamble is chosen from the address's first bit; if they
                # disagree this is a coincidental 0xAA, not a real frame start.
                if addr_bits[0] != lead:
                    continue
            else:
                addr_bits = bits[off:off + need]

            seq = bits_to_bytes(addr_bits)
            if degenerate(seq):
                continue
            seen_here.add(seq)
            where[seq].add((ch, off % 8))

        for seq in seen_here:
            packet_hits[seq] += 1

    total = len(packets) or 1
    rows = []
    for seq, hits in packet_hits.most_common(top * 8):
        frac = hits / total
        rows.append((frac, seq, hits, frac,
                     {c for c, _ in where[seq]}, {b for _, b in where[seq]}))
    rows.sort(key=lambda r: -r[0])
    return rows[:top], total


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="capture log file(s)")
    ap.add_argument("--width", type=int, default=0,
                    help="address width in bytes (3,4,5). 0 = try all")
    ap.add_argument("--top", type=int, default=15, help="candidates to show")
    ap.add_argument("--strong", action="store_true",
                    help="use only packets tagged RPD:1 (captured on real signal)")
    ap.add_argument("--loose", action="store_true",
                    help="skip the preamble anchor (use if the false sync ate "
                         "the preamble byte); returns bit-shifted aliases too")
    args = ap.parse_args()

    packets, skipped, strong, tagged = parse(args.files, strong_only=args.strong)
    if not packets:
        print("No capture lines found.")
        print("Expected format:  [CH: 35] LEN: 32 | HEX: 1D 7C .. | T:123")
        sys.exit(1)

    chans = Counter(ch for ch, _ in packets)
    print(f"\nParsed {len(packets)} packets ({skipped} non-matching lines ignored)")
    if tagged:
        print(f"Signal-tagged: {strong} of {tagged} captured on real signal "
              f"({strong*100/tagged:.1f}%)"
              + ("  [using only these]" if args.strong else
                 "  [add --strong to use only these]"))
    print("Channels present: " + ", ".join(
        f"RF_CH {c} ({2400+c} MHz): {n}" for c, n in sorted(chans.items())))

    if len(packets) < 40:
        print("\n  NOTE: fewer than 40 packets. Address detection leans on a")
        print("  sequence recurring across many captures — collect more first.")

    widths = [args.width] if args.width else [5, 4, 3]
    for w in widths:
        rows, total = find_candidates(packets, w, args.top, loose=args.loose)
        print(f"\n=== {w}-byte address candidates ===")
        if not rows:
            print("  none survived filtering")
            continue
        print(f"{'address':<18}{'packets':>8}{'  share':>9}  channels")
        for score, seq, hits, frac, cs, al in rows:
            addr = " ".join(f"{b:02X}" for b in seq)
            flag = "  <== STRONG" if frac > 0.40 else ("  <- worth testing" if frac > 0.15 else "")
            print(f"{addr:<18}{hits:>8}{frac*100:>8.1f}%  {sorted(cs)!s}{flag}")

    print("""
HOW TO READ THIS
  A real address should appear in a LARGE SHARE of packets at ONE OR TWO bit
  alignments. Something in 5% of packets across all eight alignments is noise.

  Nothing strong?  In order of likelihood:
    1. Wrong channel — run the firmware's 's' scan and compare against baseline.
    2. Receiver saturated — the PA+LNA overloads up close. Move 2-5 m AWAY.
    3. Not enough traffic — force it: buzzer pairing (hold button ~2 s) is
       the one event you can trigger on demand.
    4. Too few packets — the exploit's hit rate is low by design. Let it run.
    5. Preamble byte lost to the false sync — retry with --loose. That drops
       the anchor, so expect the true address plus 7 bit-shifted aliases;
       the real one is whichever the hardware accepts.

  Got a candidate?  Set it as a real 3-5 byte address with CRC enabled and
  auto-ack off. If it is right, packets arrive cleanly and constantly.
""")


if __name__ == "__main__":
    main()
