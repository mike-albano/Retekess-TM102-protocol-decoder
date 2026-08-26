# Retekess TM102 — Reverse Engineering Findings

Living document. Updated as things are established; superseded claims are
struck through rather than deleted, so the reasoning stays auditable.

**Tags:** **[FACT]** = measured, or from FCC filings / manufacturer docs ·
**[OPEN]** = not yet established · **[REFUTED]** = contradicted by evidence.

This supersedes `Retekess FCC ID RF Analysis.pdf`, which contains material
errors — see *Errors in the pre-existing PDF and README*.

---

## Where we are

| | |
|---|---|
| **Radio** | Si24R1 — an nRF24L01+ clone. Standard ShockBurst, **not** XN297 |
| **Modulation / rate** | GFSK, **1 Mbps** |
| **Channel** | **RF_CH 50 = 2450 MHz** (measured) |
| **Address** | **`18 18 18 18 3A`** on air, after a `0x55` preamble — confirmed by hardware correlator on all 40 bits |
| **CRC** | **Off** — confirmed on air: 5-byte address + CRC-16 receives nothing at any payload length |
| **Auto-ack** | Presumed off; unconfirmed |
| **Payload** | **8 bytes**, laid out — see *The frame* |
| **Controller events** | `81` General/Lock · `8C` Clear · `8F` Random-start · `95` Random-selected |
| **Game state** | Three: idle / armed / answered — in every beacon frame |
| **Buzz-in** | armed → answered (`p3` = `p4` = `00`). **Not** a p6 change |
| **Buzzer identity** | **`p6` = `0x90` + (n XOR 4)** — all 8 handsets, verified 8/8 |
| **Decoder** | `tools/decode_state.py` → JSON event stream |
| **Live readout** | `tools/live.py` — names each button press as it happens |
| **Validation** | Exact on 6 captures — 83 General, 83 Clear, 73 buzz-ins, all 8 handsets |
| **Cadence** | ~30 ms heartbeat; every event sent **twice**, 11 ms apart |

**Goal:** a passive, receive-only decoder beside the controller that emits JSON
of *which button was pressed* and *which buzzer won*. See **Project goal**.

### Timeline

| Date | Established |
|---|---|
| 2026-08-26 | PHY from FCC test report: 1 Mbps, 16 channels, 2420–2465 MHz |
| 2026-08-26 | Radio identified as Si24R1 from host internal photos |
| 2026-08-26 | Channel measured: RF_CH 50 / 2450 MHz |
| 2026-08-26 | Address recovered: `18 18 18 18 3A`; CRC shown to be off |
| 2026-08-26 | Hardware lock-on achieved; 8-byte payload decoded; two controller event codes isolated |
| 2026-08-26 | Full 40-bit address confirmed; legacy ShockBurst (no packet-control field); CRC confirmed off; addressed capture mode `n` adopted |
| 2026-08-26 | `n` mode fixed — stale pipe 0 was flooding the FIFO and dropping 84% of real frames |
| 2026-08-26 | **`81` = General, `8C` = Clear** — settled by timed asymmetric capture |
| 2026-08-26 | Continuous state found in the beacon: `p1` = armed/idle, `p6` = last answerer (`95` = buzzer 1) |
| 2026-08-26 | **`p6` = `0x94` + buzzer number** — the identity field. Working JSON decoder built and validated |
| 2026-08-26 | All 8 handsets mapped: `p6` = `0x90` + (n XOR 4). Linear rule refuted |
| 2026-08-26 | Map verified 8/8 live against declared handset order |
| 2026-08-26 | Voice announcements added for range testing; cold start of both devices verified; `p6` shown to be volatile |
| 2026-08-26 | **Lock decoded**: same `0x81` command as General, no mask transmitted, exclusion enforced at the handset and cumulative |
| 2026-08-26 | **Random mode**: both presses visible (`8F`, `95`); the pick is unobtainable — not transmitted, not inferable from timing, and no buzzing follows to reveal it |
| 2026-08-26 | Live test found a third state (**answered**) and a decoder flaw: repeat wins by the same buzzer were invisible. Fixed, then confirmed live on a run built around repeat wins |

### Open questions

1. Payload layout — which byte carries the buzzer ID, what General / Clear /
   buzz-in look like.
2. ~~Address register byte order~~ — **settled**: on-air `18 18 18 18 3A`
   is written `{0x3A, 0x18, 0x18, 0x18, 0x18}`.
2b. ~~Which of `81` / `8C` is General and which is Clear~~ — **settled**:
   `81` = General, `8C` = Clear.
3. Whether auto-ack is enabled, i.e. whether host ACKs are visible.
3b. **Whether the arbitration result is broadcast** (the project's core
   assumption) — well supported by the buzzers' rank-coloured LEDs, not yet
   proven from the air.
4. How the host's `F4` menu value maps to channel — known to be non-sequential.
5. Whether the two buzzer production batches behave identically.

---

## Project goal

Build a **passive event decoder**: a receiver that sits beside the controller,
listens, and emits a structured stream (JSON) of

1. **which controller button was pressed**, and
2. **which buzzer buzzed in first**.

That mapping then drives whatever software the operator wants to write.

### What this is not

- Not impersonating the controller. Nothing is ever transmitted.
- Not capturing buzzer presses for their own sake — the buzz-in matters only
  as the arbitration *result*.
- Not host emulation, replay, or control of any kind.

**The scope is receive-only, permanently** — not as a cautious first phase.
That simplifies everything: no timing requirements, no ACK deadlines, no risk
of disturbing a live game.

### Operating hypothesis  [OPEN — but well supported]

*The controller arbitrates and broadcasts the result, so every buzzer learns
who won.*

If true, a single listener beside the controller sees everything, and neither
buzzer range nor buzzer count matters.

**Evidence already in hand supports it:**

- The buzzers display rank by colour — steady green for first, flashing green
  second, steady blue third, flashing blue fourth, flashing yellow fifth.
  A buzzer cannot know its own rank; it can only be told. Something must
  broadcast it.
- The controller's `F2` menu sets "answer sorting" from 1 to 5 — the
  controller is explicitly ranking up to five responders.
- The controller holds its own FCC transmitter grant (2A3NOTM101A), so it
  demonstrably transmits.
- Lock, Clear, Rule and Random all change buzzer LED state, which again
  requires a downlink.

**Still to prove**: that the winner's identity is actually *in* the broadcast
rather than, say, each buzzer being addressed individually. Testable — see the
experiment plan.

### What this means for the method

- **The downlink is the primary target**, not the buzzer uplink. Put the
  receiver next to the controller, where its 8 mW transmissions dominate the
  buzzers' 2 mW.
- **The deliverable is a decode table**, not a capture. Success is a JSON
  mapping from observed byte patterns to named events.
- **Labelled differential capture is the fastest route there**: repeat one
  known event many times, capture it in isolation, and diff between event
  types. What stays constant is framing; what changes with the event is the
  payload field we want.

---

## Experiment plan — building the event map

Each row is one capture file, one event repeated 20–30 times, nothing else
touched. Run them in order; later ones only make sense once the earlier ones
have established the baseline framing.

| # | Label | What to do | What it should isolate |
|---|---|---|---|
| 0 | `matched` | `r50` then `m`; press General/Clear throughout the 56 s sweep | **Whether CRC is on and how long the payload is.** Do this first — if it works, every later capture is checksum-clean |
| 1 | ~~`idle`~~ | ~~Leave everything alone ~60 s~~ | **Done, by accident.** The controller beacons continuously at ~30 ms. See *Transmission pattern* |
| 2 | ~~`general`~~ | ~~Timed: General, wait 10 s, Clear, wait 10 s, ×5~~ | **Done.** `81` = General, `8C` = Clear |
| 3 | ~~`buzz-1`~~ | ~~General → buzzer **1** → Clear~~ | **Done.** `p6` = `95`, and it latches |
| 4 | ~~`buzz-2`~~ | ~~General → buzzer **2** → Clear~~ | **Done.** `p6` = `96`. Identity byte found |
| 5 | ~~`buzz-3` … `buzz-8`~~ | ~~all six remaining handsets~~ | **Done.** Linear rule refuted; `p6` = `0x90` + (n XOR 4) |
| 6 | `buzz-1-then-2` | Arm, buzzer 1 first then buzzer 2 | Whether rank (1st/2nd) is broadcast, and how |
| 7 | ~~`lock`~~ / `random` / `rule` | ~~Lock~~; the other mode buttons | **Lock done** — see *Lock*. It sends `0x81`, same as General |
| 8 | `pairing` | Hold a buzzer ~2 s | Enrolment exchange; likely a distinct frame type |

### Order of work, as agreed 2026-08-26

1. ~~Map the remaining six buzzers~~ — **done 2026-08-26**. The linear rule was
   refuted; see *Correction: p6 is not a counter*.
2. **Then** the two harder use cases below, deliberately saved for last because
   both need the identity map to be solid before their results can be read.

#### Use case A — placings beyond first

`buzz-1-then-2`, and then three and four handsets in a known order. The host's
F2 menu ranks up to **five** places, so the ranking exists somewhere; p6 holds
only the winner. Either there are further identity bytes we have not attributed
(p0, p2 and p7 are still unexplained), or placings are sent as a sequence of
messages rather than a single field.

#### Use case B — Lock

The **Lock** button re-arms every buzzer *except* the one that just answered —
for when the first team gets the question wrong. This is the most informative
single test left, because it forces the protocol to express something our
current model cannot: a per-buzzer enable mask, distinct from both "armed" and
"answered". Whatever byte changes under Lock is a field we have not yet found,
and it may be the same field that carries placings.

Note it also predicts a state our decoder has no name for: armed, with a
standing previous answerer, and one handset excluded. Expect `decode_state.py`
to need a fourth state.


Captures 3 and 4 are the ones that matter most: **the diff between them is the
buzzer identity**, and that is the core of the JSON mapping.

Consistency matters more than volume. Twenty clean repetitions of exactly one
event beat two hundred of a muddle.

### Why capture 2 has to be timed

The lock-on capture already showed the two event codes `81` and `8C`
alternating strictly, 48 times. That is exactly what a General/Clear cycle
produces — and exactly what a Clear/General cycle produces. Alternation alone
cannot break the symmetry no matter how many presses are recorded. Only an
asymmetric capture can: press one button, leave a gap far longer than the
cycle, press the other. The first event after a long silence is then known.

**Leaning (not established):** in the lock-on capture `81` → `8C` averaged
0.85 s and `8C` → `81` averaged 1.4 s. If the operator pressed General, paused
a beat, pressed Clear, then paused longer before the next round — which is how
the described game flow feels — that makes **`81` = General, `8C` = Clear**.
Plausible, not proven. Capture 2 decides.

### A caveat about captures 3–6

Our matched receiver locks to `18 18 18 18 3A`, which is the address the
**controller** transmits on. If a buzz-in produces no new event code there, it
means the buzzers transmit on a different address and the controller does not
rebroadcast the result — which would refute the operating hypothesis. That is a
real possible outcome and worth naming in advance, so we read the result
honestly instead of hunting for the answer we want.

---

# Radio and protocol

## The radio: Si24R1, an nRF24L01+ clone  [FACT]

**This is the single most important finding.** The host's internal photos
(exhibit 6072498, Fig. 5) show the 2.4 GHz radio is not soldered to the main
board — it is a **plug-in daughter module** (`U5`, silkscreen `10383-V1.0`)
with a u.FL connector and an onboard PCB antenna. At that scale the chip
markings are legible:

| Component | Marking |
|---|---|
| **Radio IC** | **`Si24R1`** / `552WA11` |
| Crystal | `SJK 16.000` — 16.000 MHz |
| Second IC | `82D A1H` (small SOP — PA or LDO) |

**Si24R1** = Nanjing Zhongke Microelectronics, a 2.4 GHz GFSK transceiver
built as an **nRF24L01+ architectural clone**: same register map (CONFIG,
EN_AA, EN_RXADDR, SETUP_AW, RF_CH, RF_SETUP, STATUS, FIFO_STATUS), same
Enhanced ShockBurst packet engine, 3–5 byte addresses, 1 or 2 byte CRC,
0–32 byte payloads, `F = 2400 + RF_CH` MHz at 1 MHz steps, and 250 k / 1 M /
2 Mbps rates. Differences from a genuine Nordic part are minor and irrelevant
to sniffing: an extra RF_PWR bit (8 levels to +7 dBm) and lower idle current.

### Why this changes everything

**The protocol is standard nRF24 Enhanced ShockBurst — not XN297.**

That conclusion holds for the whole system, not just the host: the buzzers
must interoperate with an Si24R1 on the other end, so their PHY is
nRF24-compatible whatever silicon they use. (The buzzer's own `U4` is a
QFN-20 beside a crystal and PCB antenna — the Si24R1's exact footprint.)

Consequences:

1. **No scrambling, no 3-byte fixed sync word.** The `XN297Dump` approach
   in §8 below is the *wrong tool* — discard it. Set against the earlier
   plan, this is the correction that matters most.
2. **The Goodspeed promiscuous exploit is valid here** — it was written for
   exactly this silicon. The README's instinct was right; it was the channel
   and data rate that were wrong.
3. **The `RF24` library drives this natively.** No custom descrambling, no
   CRC lookup tables, no bit-reversal.
4. **Auto-ack is probably in use.** ESB acknowledges in hardware, so a buzzer
   press likely draws an immediate ACK from the host. The PDF's "ACK"
   guess is accidentally right, though its CSMA/CA and randomised-backoff
   story remains invention.
5. **Address discovery is the real task.** Not demodulation — the hard part
   is finding the 3–5 byte address, after which everything decodes cleanly
   with CRC on.

Useful datasheet note: Si24R1 states the top address byte must not be
`0xFF, 0x00, 0xA5, 0x5A, 0xAA, 0x55`. So the system's real address cannot
begin with `0x00` or `0xAA` — meaning the README's plan of *listening* on
`0x00`/`0xAA` will only ever catch noise, never a framed packet. Those two
values are the right bait for the Goodspeed trick precisely because no real
device uses them, but the captures must then be searched for a repeating
address, not read directly.

### One resolved red herring

Host internal photo Fig. 4 carries a lab annotation **"Only Receive"** with
arrows pointing at two meander antennas on the left and right board edges —
*not* at the Si24R1 module. The TM101's separate remote control runs on
2×12V/23A cells, the signature of a 433 MHz key-fob transmitter, so those
edge antennas are a receive-only 433 MHz path for that remote, declared to
the FCC as an unintentional radiator. It does **not** mean the host is
receive-only on 2.4 GHz — the Si24R1 module holds a transmitter grant.

---

## PHY parameters  [FACT]

Everything here is from the FCC test report (LCSA072222029EA, Shenzhen LCS,
2022-08-01) filed under FCC ID 2A3NOTM101.

| Parameter | Value |
|---|---|
| **Data rate** | **1 Mbps** GFSK |
| **Channels** | **16**, spaced **3 MHz** |
| **Range** | 2420–2465 MHz |
| **20 dB bandwidth** | 1141 / 1151 / 1149 kHz (measured at 2420 / 2444 / 2465) |
| Antenna | PCB trace, 0 dBi max |
| Rule part | FCC Part 15C §15.249 |

The ~1.15 MHz 20 dB bandwidth is the classic nRF24-family **1 Mbps** GFSK
signature (2 Mbps would measure ~2 MHz, 250 kbps ~0.6 MHz). Combined with the
report's own text — *"data rates can be up to 1 Mb/s by modulating the RF
carrier using GFSK"* — the data rate is settled. **README's "we think 2 Mbps"
is wrong; `skills.md`'s "start at 1 Mbps" is right.**

---

## Channel map  [FACT]

From the test report's channel list. `RF_CH` is the nRF24 register value
(`f_MHz − 2400`) to program directly.

| Idx | MHz | RF_CH | | Idx | MHz | RF_CH |
|---|---|---|---|---|---|---|
| 1 | 2420 | 20 | | 9  | 2444 | 44 |
| 2 | 2423 | 23 | | 10 | 2447 | 47 |
| 3 | 2426 | 26 | | 11 | 2450 | 50 |
| 4 | 2429 | 29 | | 12 | 2453 | 53 |
| 5 | 2432 | 32 | | 13 | 2456 | 56 |
| 6 | 2435 | 35 | | 14 | 2459 | 59 |
| 7 | 2438 | 38 | | 15 | 2462 | 62 |
| 8 | 2441 | 41 | | 16 | 2465 | 65 |

*The report prints channel 6 as "2433 MHz". That breaks the otherwise exact
3 MHz spacing and is a typo for 2435 — but verify empirically before relying
on it.*

At 3 MHz spacing with ~1.15 MHz occupied bandwidth there is no channel
overlap, so a sniffer parked on the wrong channel hears nothing at all.
Getting the channel right is not optional.

---

## The address  [FACT — measured]

Capture 2026-08-26, RF_CH 50, buzzer #1, operator cycling General/Clear:
29,341 promiscuous captures, of which **1,523 arrived on real signal** (the
RPD gate). Of those, **1,324 (87%)** contain the same byte run once aligned on
the Enhanced ShockBurst preamble:

```
    55  |  18 18 18 18 3A  |  <varies>
 preamble     CONSTANT         payload
```

The three candidates the analyser first reported — `18 18 18 18 3A`,
`8C 0C 0C 0C 1D`, `46 06 06 06 0E` — are the same bit pattern at three
different bit offsets. Aligning on the `0x55` preamble picks out the real one:
ESB uses preamble `0x55` when the address's first bit is 0, and `0x18` begins
`00011000`. Consistent.

### Independent corroboration

**The README's own example output starts with these exact bytes:**

```
HEX: 8C 0C 0C 0C 1D 79 3B 7F 77 31 DD 4A FF 4E B9 A0 ...
HEX: 46 06 06 06 0E BC 1D BF BB 98 EE 25 7F A7 5C D0 ...
```

Whoever produced that capture was receiving the real signal all along and did
not recognise it. Two independent sessions, different days, same pattern —
this is not an artefact of one run.

### CRC is disabled on this system

Six CRC configurations were tested offline against 1,317 aligned packets:
CRC-16 (poly 0x1021, init 0xFFFF and 0x0000) and CRC-8 (polys 0x07 and 0x31,
inits 0xFF and 0x00), across address widths 3/4/5, both Enhanced ShockBurst
(with packet-control field) and legacy ShockBurst (fixed payload). Best result
was 9 of 1,324 — indistinguishable from chance.

**Conclusion: the system runs with CRC switched off.** Legal, and common in
low-cost designs. Practically it means the receiver must be configured
`disableCRC()` to match, and that packet integrity has to be judged by
plausibility rather than checksum.


## The frame — payload structure  [FACT — measured 2026-08-26]

Source: `captures/20260826-072140_lockon.log` — 82 s on RF_CH 50 with the
correlator hardware-locked to the first three address bytes `18 18 18`,
operator pressing **General → Clear** on repeat, no buzz-ins. 4,068 frames,
every one beginning `18 3A`.

### Layout

Locking on three address bytes leaves the other two address bytes sitting in
the payload window. Everything after them is the real payload:

```
on air:   55 | 18 18 18 18 3A | p0 p1 p2 p3 p4 p5 p6 p7 | c0 c1 |  (dead air)
                \__ matched __/  \_______ payload ______/ \trailer/
capture:                  18 3A   [2][3][4][5][6][7][8][9] [10][11]  [12…]
```

**The payload is 8 bytes.** Evidence: bit values are deterministic through
capture byte 12, then the ones-fraction decays smoothly from 0.88 at byte 13 to
0.63 at byte 31 — the signature of a receiver still shifting in demodulator
output after the transmitter has stopped. That is where the packet ends.

| Capture byte | Payload | Content |
|---|---|---|
| `[0] [1]` | — | tail of the address, `18 3A` |
| `[2]` | p0 | varies — sits inside a corrupted run, see *the noise model* |
| `[3]` | p1 | **message class** — `76` / `77` / `7B` |
| `[4]` | p2 | `FF` |
| `[5]` | p3 | `00` on a button event; a **decrementing counter** otherwise |
| `[6]` | p4 | **event code** — `81` or `8C` |
| `[7]` | p5 | `73`, sometimes `70` |
| `[8] [9]` | p6 p7 | `FF FF` — padding |
| `[10] [11]` | — | **2-byte trailer, fixed per message type** — almost certainly the CRC |
| `[12] …` | — | post-packet noise |

### The two controller messages  [FACT]

Reconstructed by per-bit majority vote over all frames of each kind:

| | p1 | p3 p4 | trailer | frames |
|---|---|---|---|---|
| **General** | `7B` | `00 81` | `72 15` | 59 |
| **Clear** | `76` | `00 8C` | `A1 99` | 38 |
| heartbeat | `77` | varies | varies | 3,971 |

```
Event A   18 3A  F2 7B FF 00 81 73 FF FF  72 15
Event B   18 3A  FA 76 FF 00 8C 73 FF FF  A1 99
```

Across 78 s the two alternate **strictly** — 97 event frames, about 48 button
presses.

### Which is which  [FACT — settled 2026-08-26]

`captures/20260826-132002_general-timed.log`. Deliberately asymmetric: press
General, count to ten, press Clear, count to ten, five times. Long gaps make
the order readable in a way that fast alternation never can.

```
   0.000s   81      16.128s   81      35.765s   81      57.120s   81      77.914s   81
   7.836s   8C      23.248s   8C      46.890s   8C      67.824s   8C      87.973s   8C
```

Exactly ten events, exactly five of each, strictly alternating, gaps of 7–12 s
throughout. 3.7 s of quiet capture precedes the first and 3.3 s follows the
last, so nothing was clipped at either end and the first event really is the
first press.

| Code | Button |
|---|---|
| **`81`** | **General** — arms the buzzers |
| **`8C`** | **Clear** — clears the result, must precede the next General |

Recorded in `event_map.json`. The earlier timing hunch — that the shorter
0.85 s gap was General → Clear — turned out to be right, but it was a guess
about operator habit and could easily have gone the other way. It is now
measured instead of inferred.

### Capture quality after the pipe-0 fix  [FACT]

Same capture, for comparison against the 84%-loss run that preceded it:

| | before (v7) | after (v8/v9) |
|---|---|---|
| frames delivered | 15,868 | 8,655 |
| of which real | 575 (3.6%) | 8,647 (**100%**) |
| button events caught | 0 | 10 of 10 |

Every frame the radio now hands us is genuine. Rate is ~91 frames/s: the
~30 ms beacon plus the doubled event transmissions, all of it arriving rather
than being pushed out of the FIFO by junk.

### State lives in the beacon, not only in the events  [FACT — 2026-08-26]

Source: `captures/20260826-132616_buzz-1.log` — 25 rounds of General → buzzer 1
→ Clear, 98 s, 4,670 frames. Comparing it against `general-timed` (identical
except nobody buzzed) isolates what a buzz-in changes.

Two payload bytes turn out to carry **continuous state**, repeated in every
beacon frame rather than announced once:

| Byte | Value | Meaning |
|---|---|---|
| **p1** | `0x76` | idle — buzzers not armed |
| | `0x77` | **armed** — General has been pressed, buzz-ins accepted |
| **p6** | `0xFF` | no buzzer has answered since power-on |
| | `0x95` | **buzzer 1 has answered** |

p1 tracks the game state second by second, flipping to `77` on General and back
to `76` on Clear, and it does so in *every* frame. That matters practically:
the momentary event frames are sent only twice and are easy to miss — this
capture dropped several — whereas the state is restated ~30 times a second.
**A decoder should read state from the beacon and use the event frames only as
a timestamp for the transition.**

### p6 latches, and Clear does not reset it  [FACT]

p6 was `FF`-ish for the first six seconds of the capture. At the first buzz-in
it became `0x95` and **stayed `0x95` for the remaining 92 seconds**, across
more than twenty subsequent General/Clear cycles.

So p6 is not "who won this round". It is closer to a **last-answered register**
that survives Clear — most likely what drives the winner display on the host.
Whether it also survives a power cycle is untested.

This is good news for the project: it means the winner is readable from any
beacon frame at any time, not only in the instant after a buzz.

It also sets up the decisive test. If p6 changes to a different value when
buzzer **2** answers, p6 is the buzzer identity and the JSON map is essentially
done. If it stays `0x95`, it means something else — "someone answered" rather
than "who" — and the identity is elsewhere.

### p6 is the buzzer identity  [FACT — 2026-08-26]

`captures/20260826-133344_buzz-2.log`, 25 rounds with buzzer **2**. p6 opened at
`0x95` — still latched from the buzzer-1 session — and flipped at the first
buzz-in, then held for the remaining 73 s.

| Buzzer | p6 |
|---|---|
| 1 | `0x95` |
| 2 | `0x96` |

~~**`p6 = 0x94 + buzzer number.`**~~ — **wrong**, see the correction below.
p6 *is* the field the project was after; the arithmetic was not.

Buzzer numbers run 1–32 (the host's F2 *learning number* range), so a winner
occupies `0x95`–`0xB4` exactly and anything above that is `0xFF` with bits
knocked out of it. That gives the decoder a clean, non-arbitrary boundary.

### Correction: p6 is not a counter  [FACT — all 8 handsets, 2026-08-26]

`captures/20260826-142007_live.log`. Every handset, two or three rounds each:

| n | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| **p6** | `95` | `96` | `97` | **`90`** | **`91`** | **`92`** | **`93`** | `9C` |

The linear rule predicted `98 99 9A 9B` for handsets 4–7. It got `90 91 92 93`
— and 4, 5, 6, 7 were rejected outright as out of range, because the decoder
had been told nothing below `0x95` could be a buzzer.

**The low nibble is `n` with bit 2 inverted:**

```
    p6 = 0x90 + (n XOR 4)          n = (p6 - 0x90) XOR 4
```

| n | binary | XOR 4 | p6 |
|---|---|---|---|
| 1 | `0001` | `0101` | `0x95` |
| 4 | `0100` | `0000` | `0x90` |
| 8 | `1000` | `1100` | `0x9C` |

This fits all eight measurements exactly. Handsets 1, 2, 3 and 8 have bit 2
**clear**, and for those `XOR 4` and `+ 4` give the same answer — which is
precisely why the linear rule survived the first sample. We tested 1, 2, then
predicted from two points that shared the property that made the wrong rule
look right.

**Why bit 2 is stored inverted is unknown**, and the rule is measured for
n = 1–8 only. Handset numbers go to 32 through the host's F2 menu; nothing
above 8 has been checked, and the same trap applies — a rule that fits eight
points can still be the wrong rule. Renumbering one handset to, say, 20 would
be the decisive test if it ever matters.

**Method note.** The prediction was recorded before the capture, which is why
this was caught in one run instead of becoming a latent bug in the decoder. It
also argues for testing the *unlike* cases first: had we done handset 4 second
instead of handset 2, the linear rule would never have been written down.

### Confirmed end to end  [FACT — 2026-08-26]

`captures/20260826-145842_live.log`, `tools/live.py --verify`. The operator
declares the handset order up front, so the check can fail — unlike a summary
of what was observed, which cannot disagree with itself.

| handset | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| p6 | `95` | `96` | `97` | `90` | `91` | `92` | `93` | `9C` |
| | PASS | PASS | PASS | PASS | PASS | PASS | PASS | PASS |

**8 of 8**, in 35 seconds. The mapping lives in `event_map.json` and
`decode_state.py` loads that table as the authority, falling back to the
formula only for handsets nobody has pressed. A measurement is never
overridden by a rule inferred from it.

~~**Also untested:** whether p6 survives a power cycle~~ — **settled**: it does
not. See *Power cycling*.

### Three states, not two — and how the live test found it  [FACT — 2026-08-26]

The first live run (`captures/20260826-134629_live.log`) read every button
press correctly except one round, where the operator pressed two buzzers with
**buzzer 2** a fraction earlier. The host showed the result; the decoder said
nothing. Buzzer 2 had also won the *previous* round — so p6 was already `0x96`
and **did not change**. A decoder watching p6 for changes cannot see a repeat
win by the same buzzer.

This was invisible in the recorded captures, and worse than it looked there:
`buzz-1` reported one buzz across 25 rounds, and that seemed plausible because
p6 latches. It was not plausible — 24 real buzz-ins were being silently
dropped, and the latch explanation made the wrong number look right.

The fix is to stop inferring buzzes from p6 at all. The controller has a
distinct **answered** state, marked in the payload:

| State | p1 | p3 | p4 | frames/s |
|---|---|---|---|---|
| **idle** | `0x76` | counter | `0x70` | ~35 |
| **armed**, waiting | `0x77` | `0xFE` | `0xFE` | **~148** |
| **answered** | `0x77` | **`0x00`** | **`0x00`** | **~6** |

A buzz-in is the armed → answered transition; p6 then says *who*. p6 remains
the identity, but it is no longer the trigger.

**The transmit rate corroborates the state independently** — and is a finding
in its own right. The controller beacons hardest while a question is open
(~148/s, presumably arbitrating), drops to a trickle once someone has answered
(~6/s), and idles in between (~35/s). It was the collapse from 148 to 6 in the
missed round that showed something had happened there at all.

### Two confirmation thresholds  [method]

A state change is accepted only after N agreeing frames. One N does not work,
because the two transitions fail in opposite directions:

- **idle ↔ armed** rides on one bit of p1 — `0x77` → `0x76` is a *single
  dropped bit*, the dominant error mode. It needs several frames to confirm.
- **armed → answered** is marked by p3 and p4 both zero: sixteen bits that
  1 → 0 corruption cannot fabricate. But the controller sends only ~6 frames/s
  in that state, so demanding five would miss short rounds.

Strong marker, low threshold; weak marker, high threshold. Swept jointly
against four captures with known ground truth:

| | general-timed | buzz-1 | buzz-2 | live #1 | live #2 |
|---|---|---|---|---|---|
| **expected** | 5G 5C 0B | 25G 25C 25B | 25G 25C 25B | 6G 6C 5B | 7G 7C 7B |
| **decoded** | 5G 5C 0B | 25G 25C 25B | 25G 25C 25B | 6G 6C 5B | 7G 7C 7B |

Live run #2 (`captures/20260826-140054_live.log`) was the confirming test,
recorded after the fix and deliberately including repeat wins. Seven rounds,
decoded as `GBCGBCGBCGBCGBCGBCGBC` — perfectly formed, nothing spurious,
nothing missing. The buzz sequence was **1, 2, 2, 2, 1, 2, 2**: three of the
six consecutive pairs are repeat wins by the same handset, so the old
p6-change decoder would have reported 4 of 7. It now reports 7 of 7.

Exact on all four, with every buzz attributed to the right handset (25/25
buzzer 1, 25/25 buzzer 2, and 1,2,2,1,2 live). The chosen values, 5 and 3, sit
in the middle of a plateau — 4–6 and 2–3 all give identical results — rather
than on the edge of one, so they are not fitted to these particular captures.

### A caution this leaves behind  [method]

The earlier validation looked convincing and was wrong. "25 general, 25 clear"
matched the operator's count exactly while the buzz count was off by 24, and
nothing in the numbers announced it. Ground truth on *one* field is not
validation of the others.

### Power cycling  [FACT — 2026-08-26]

**The controller clears its state.** Buzz handset 5, confirm `p6 = 0x91`
latched, power cycle the controller, reconnect:

```
captures/…154512_live.log  last event:  buzz buzzer 5 (p6 0x91), then clear
captures/…154800_live.log  opened:      idle, last winner: nobody
```

So p6 lives in RAM, not in non-volatile memory, and the round returns to
cleared. A decoder must therefore **not** trust p6 at startup as "who won
last" — until the first buzz of a session it means only "nobody since the
controller last powered up". This closes an open question and matters for any
software built on this: the latch is per power-on, not permanent.

**The ESP32 tolerates a cold start.** Unplugged at the USB, replugged, run
again: no reconfiguration needed, everything decoded normally. This was worth
testing specifically because the nRF24 module has a documented brownout mode
on this hardware (see *Hardware*), and a cold power-up is the only thing that
exercises the module's own regulator from zero.

### Correction: the serial port does not reset the board  [REFUTED]

~~Opening the serial port resets the ESP32 via DTR, so every `live.py` run
starts from a fresh boot~~ — **wrong.** Board uptime across six consecutive
sessions:

| session | 13:46 | 14:00 | 14:20 | 14:58 | 15:39 | 15:45 | 15:48 |
|---|---|---|---|---|---|---|---|
| uptime | 1600 s | 2467 s | 3616 s | 5922 s | 8379 s | 8712 s | **19.5 s** |

Monotonic for 2.4 hours, then a physical unplug. The board was **never**
rebooted by opening the port. The clue was there the whole time and was
explained away: no boot banner ever appeared in any log, which was put down to
a buffer flush rather than to there being no boot.

Two consequences. The operator's power cycle was the *first* ESP32 reboot
since flashing, which makes it a real test rather than a redundant one. And
`live.py` cannot rely on a banner for a health check — it now sends `i`
explicitly and prints the self-test result.

**A trap this immediately created.** `verifyRegisters()` required
`SETUP_AW == 0`, the 2-byte promiscuous exploit. Addressed capture uses a real
5-byte address, so `SETUP_AW == 3`, and the newly-added self-test would have
printed **"SELF-TEST: FAIL — stop here"** at the start of every ordinary
session. The check now accepts either width and names which mode it is in.
A health check that cries wolf is worse than none: it trains you to ignore it.

## Lock  [FACT — 2026-08-26]

Sources: `captures/20260826-155751_lock-basic.log`,
`captures/20260826-160451_lock-excluded.log`.

**Lock is not a distinct command.** It transmits the same event code as
General, `p4 = 0x81` — 16 such frames for 4 Generals plus 4 Locks, splitting
8/8. There is no separate "lock" message.

**The armed state after Lock is bit-for-bit identical to the armed state after
General.** 956 armed frames following General against 1,413 following Lock,
compared per bit:

```
 p0 ........   p1 ........   p2 ........   p3 ........
 p4 ........   p5 ........   p6 .....**.   p7 ........
```

Only p6 differs, and only because it holds the handset that just answered —
the latch already understood. **No exclusion mask is transmitted.** p0, p2 and
p7 remain unattributed but are not carrying it.

**And "armed, excluding whoever is in p6" is not the rule either.** In
`buzz-2`, handset 2 won 25 consecutive rounds with p6 pinned at `0x96`
throughout and was never excluded. So the same air traffic produces different
behaviour depending on history.

### The exclusion is enforced at the handset  [FACT]

During a 9.8-second locked window with handset 3 pressed twice, deliberately
and firmly:

- frame rate held flat at **132–150/s**, the normal armed rate. A real buzz-in
  collapses it to ~6/s. The controller never changed state.
- `p1 = 0x6D` frames, which are strongly buzz-associated, showed **no
  elevation above the normal armed baseline**.

That second point needs stating carefully, because the first version of it
here was wrong. `0x6D` frames are not absent from armed windows — measured
across all five captures, by the state they fall in:

| state | `0x6D` share of frames |
|---|---|
| answered (just after a buzz) | **35%** |
| armed, after General | 1.01% |
| armed, after Lock | 0.87% |
| idle, after Clear | 0.00% |

So the claim is not "zero traffic" but the stronger, controlled one: **an armed
window with the locked handset being pressed is indistinguishable from an armed
window with nobody pressing anything** — 0.87% against 1.01%, on 4,827 and
11,451 frames. Had the handset transmitted and been rejected, the answered-state
signature at 35% would have appeared. It does not.

A locked-out handset therefore produces no detectable air traffic. The
controller is not receiving and discarding — there is nothing to discard.

### How the handset knows  [OPEN — hypothesis]

The handset must infer its exclusion from the same broadcast everyone hears.
The only thing distinguishing a post-Lock arm from a post-Clear arm, from the
handset's point of view, is that **no Clear intervened**. Each handset knows
whether *it* was the last to answer.

Proposed rule: *on arm, stay out if I answered since the last Clear.* That
fits every observation, needs no hidden field, and explains why the armed
state is bit-identical.

### Exclusions accumulate  [FACT — 2026-08-26]

`captures/20260826-160932_lock-twice.log`. General → handset 3 → Lock →
handset 5 → Lock → handset 3 pressed twice → handset 7 → Clear.

During the second locked window, **p6 held `0x91` — handset 5** — and handset 3
was pressed twice with no reaction: 1,360 frames at 135/s, flat, no state
change, `0x6D` at baseline.

This settles the rule. Handset 3 stayed out while p6 named a *different*
handset, so the exclusion cannot be "p6 names me". It is **"I answered since
the last Clear"**, held by each handset about itself, and it **accumulates**:
lock out one team, then another, and both stay out until Clear.

Practically, for the quiz: successive wrong answers can be locked out one after
another, and only Clear brings anyone back.

For a decoder this is the awkward case. The set of locked-out handsets is never
transmitted and now cannot even be derived from p6 — it has to be reconstructed
by tracking every buzz since the last Clear. That is doable from our event
stream, since we see every buzz and every Clear, but it is inference, not
observation, and it will drift if a buzz is ever missed.

This has consequences for the decoder. A passive receiver **cannot** see which
handsets are locked out — the information is never transmitted. It can only be
inferred by tracking the same history the handsets track: who answered, and
whether a Clear has happened since.

## Random mode  [FACT — 2026-08-26]

Source: `captures/20260826-161522_random-mode.log`. Eight rounds: Random →
blink → Random → one handset lit → Clear, with the lit handset typed into the
log each time (3, 7, 5, 2, 7, 6, 5, 7).

### The two button presses are visible  [FACT]

Two command codes, alongside the known `0x81` General and `0x8C` Clear:

| p4 | meaning | count |
|---|---|---|
| **`0x8F`** | **Random pressed once** — all handsets blink | 21 |
| **`0x95`** | **Random pressed again** — selection locked in | 30 |

Both repeat every ~3 s for as long as the mode is active, so they are states as
well as events, and either can be picked up by a receiver joining mid-round.

### The selected handset is NOT transmitted  [FACT — negative result]

Nothing distinguishes a round where handset 2 was chosen from one where
handset 7 was. Checked exhaustively:

- **p6 never moved.** It held `0x93` — stale from the previous session — in
  **1,628 of 1,630 frames**, across eight rounds selecting five different
  handsets. The winner field is untouched by Random.
- Every byte of the 30 `0x95` selection frames, grouped by the handset that
  actually lit: no byte takes a consistent per-handset value.
- Same for the beacon frames within each round.

### Transmission nearly stops while blinking  [FACT]

| phase | frames/s |
|---|---|
| idle | 30.4 |
| **blinking** | **1.5** |
| selected | 8.1 |
| (armed, for comparison) | ~148 |

The controller goes almost silent during the blink — twenty times quieter than
idle. Eight handsets blinking in unison need coordination from somewhere, and
it is not arriving on this channel.

### Two hypotheses  [OPEN]

**(a) The handsets decide, as with Lock.** `0x8F` starts a synchronised
rotation that the handsets free-run; `0x95` freezes it; whoever's turn it is
stays lit. No identity is ever transmitted, and the silence is explained
because nothing needs saying. This would make the selection **unknowable** to a
passive receiver on principle, not by accident.

**(b) The coordination happens off-channel.** The controller hops elsewhere to
run the blink, which would explain the silence equally well and would mean
there is a whole conversation we have never seen. **[REFUTED — see below]**

These make opposite predictions and one cheap test separated them.

### The channel hunt, and the control that saved it  [FACT]

Hunting all 16 channels while cycling Random produced an alarming result —
three channels that had been **completely dead** the day before were now the
loudest in the band, while our channel 50 had gone quiet:

| RF_CH | MHz | during Random | earlier, normal use |
|---|---|---|---|
| 50 | 2450 | 3 | **277** |
| 59 | 2459 | **286** | 0 |
| 62 | 2462 | **282** | 0 |
| 65 | 2465 | **243** | 0 |

Read alone that says "the controller hops during Random". It does not.

**Control run** (`captures/20260826-162727_hunt-control.log`): same sweep, same
room, **controller switched off**, nothing touched:

| RF_CH | 50 | 56 | 59 | **62** | 65 |
|---|---|---|---|---|---|
| energy | 0 | 19 | 269 | **462** | 362 |

Stronger with the controller off than with it on. The energy peaks at
**2462 MHz — exactly the centre of WiFi channel 11** — and falls off
symmetrically either side, which is the shape of a 20 MHz WiFi carrier and not
of a hopping 1 MHz GFSK link. It was simply someone's afternoon WiFi; the day
before, at 22:42, it was idle.

**So hypothesis (b) is refuted and (a) stands.** The controller does not leave
channel 50. The silence during blinking is real silence, and the identity of
the randomly selected handset is **never transmitted at all**.

That is a permanent limit, not a gap in our method: a passive receiver cannot
know which handset was chosen, because nothing on the air says so.

**Why the control mattered.** The uncontrolled result was suggestive, internally
consistent, and wrong. Two channels dead yesterday and loud today looks like
causation when it is a time of day. Any future claim about channel energy needs
the controller-off run beside it.

**Incidental:** our channel 50 sits 12 MHz below that WiFi carrier, far enough
that it has never interfered. The offer to drive somewhere quieter was never
needed — but this is what it would have been for.

### Not inferable from timing either  [FACT — negative result]

If the blink were a rotation frozen by the second press, the outcome would
follow from the gap between the two presses. It does not:

| gap | handset |
|---|---|
| 3.38 s | 5 |
| 3.40 s | **7** |

Twenty milliseconds apart, different handsets. Handset 7 also came from gaps of
3.20 s, 3.40 s and 4.34 s; handset 5 from 1.05 s and 3.38 s. There is no
function from timing to outcome at any resolution we can measure. It is a real
random draw inside the controller, not a frozen rotation.

### And no downstream event reveals it  [FACT — from use]

Confirmed by the operator: **nobody buzzes during Random.** It is purely a way
to choose a person, shown by lighting that handset green. So there is no later
buzz-in to give the answer away either — the one indirect route is closed
because the situation it needed never arises.

### Conclusion: the pick is unobtainable

| | |
|---|---|
| Random pressed (blink starts) | `0x8F` — **yes** |
| Random pressed again (pick made) | `0x95` — **yes** |
| **Which handset was picked** | **no. Not transmitted, not inferable, no downstream tell** |

The start and end of a draw can be detected and timed precisely; the outcome
exists only in the handsets' own logic and on their LEDs.

**Recommended workaround:** have the scorekeeping software do the draw itself.
It knows all eight handsets and can display the result with full knowledge. The
controller's Random button then serves no purpose the software needs.

Worth noting for the project either way: **the first Random press is fully
capturable, the outcome is not** — which is the reverse of the buzz-in case,
where the outcome is broadcast continuously.

### A third frame class  [OPEN]

Frames with **p1 = `0x6D`** appear only after a buzz-in — 202 of them here,
**zero** in the no-buzzing capture — arriving in pairs a few hundred ms apart:

```
F4 6D FF 00 FE EE 95 F5 E0 9C BE FF
```

They differ from the controller's normal frames in p1, p5 and p8/p9. Candidates:
a display-update message, or the **buzzer itself transmitting** on the shared
address. Not yet distinguishable. `0x6D` could also be a doubly-corrupted
`0x7F`, though 202 occurrences is a lot for that.

### Correction: the trailer is not constant per message  [REFUTED]

~~Bytes p8/p9 are fixed per message type (`72 15` for General, `A1 99` for
Clear)~~ — **wrong**, and wrong because it was inferred from captures that all
happened to share one game state. In this capture General carries `4F FD` and
Clear `3C 73`; idle beacons alternate `25 1C` / `92 9E`, and post-buzz frames
carry `C9 06` / `49 06`. p8/p9 **track state** like p1 and p6 do. The values
recorded earlier were true of those captures only.

### Transmission pattern  [FACT]

Inter-frame gaps cluster at three values and nothing else:

| Gap | Count | Meaning |
|---|---|---|
| 11–12 ms | 2,461 | **every message is sent twice**, 11 ms apart |
| 22–23 ms | 189 | one half of a pair lost |
| 30 ms | 1,124 | idle heartbeat cadence |

Two consequences. The controller **beacons continuously at ~30 ms** even when
nobody touches it — that answers experiment #1 before we run it, and it is why
the earlier statistical channel sweep should have worked and the duty-cycle
worry was overblown. And because every message is sent twice, the decoder gets
a free second copy to cross-check against.

### The noise model — why the capture looks dirty  [FACT]

Some byte positions look like garbage (`FD`, `FA`, `DF`, `BB`, `ED` …) and
some are rock solid. It is not random corruption, and it is not two
interleaved fields:

- The dirty bytes **never contain two adjacent zero bits** (0.00 of 4,068
  frames at `[8]` and `[9]`), and neither does the known post-packet noise.
  Real data bytes `[10]` and `[11]` contain adjacent zeros 88–90% of the time.
- Every observed value of a dirty byte is the majority value **with bits
  cleared**, never set. Errors run 1 → 0, almost never 0 → 1.

Both point at the same cause: **long runs of `1` bits desynchronise the
receiver's data slicer.** Payload bytes p2, p6, p7 are `FF`, so the frame
contains a 16-bit run of ones; the demodulator's DC restoration drifts across
it and drops isolated bits. Positions carrying mixed data never drift and come
through clean. Dead air after the packet reads the same way — mostly ones with
isolated zeros, decaying toward random.

This is a defect of *our* capture, not of the protocol. It is fixable — see
below.

### The trailer: reopened, then re-closed  [FACT]

The "CRC is disabled" conclusion above was briefly reopened and is now
**confirmed** — by a much better test than the one that first produced it.

Bytes `[10] [11]` are **constant for a given message and different between
messages** (`72 15` vs `A1 99`), and they *vary* across heartbeat frames, whose
payload carries a changing counter. That is exactly how a checksum behaves and
nothing else behaves that way. Frame length also fits: 5 address + 8 payload +
**2 CRC**.

The earlier six-variant CRC test failed for a reason that now looks obvious:
it was run against frames carrying ~15% bit errors in the `FF` runs. A CRC
never validates over corrupted data, so the test could only ever return
"chance". It proved nothing either way.

**Test run 2026-08-26** (`captures/20260826-125930_matched.log`): real 5-byte
address, sweeping payload length 6–12 against CRC-16 and CRC-8, 4 s each,
controller beaconing throughout.

| CRC | Result |
|---|---|
| CRC-16 | **0 frames at every length** |
| CRC-8 | 2, 5, 1, 0, 3, 0, 3 — scattered across five mutually incompatible lengths |

**Verdict: CRC is off. The original finding stands.** The CRC-8 hits are
chance: a CRC-8 passes one time in 256, the correlator triggers constantly, and
a real match would have delivered ~130 frames per 4 s window at the measured
30 ms beacon rate — not five. Five different "winning" lengths cannot all be
right; none of them is.

So bytes `[10] [11]` are **not** a hardware CRC. They are still a per-message
constant that changes with the message, so they are most likely a *software*
checksum or a message-type constant the controller computes itself. Recorded as
[OPEN], but no longer as a CRC hypothesis.

### What the failed test proved instead  [FACT]

The chance-passed frames came out **byte-aligned at p0** — `F9 77 …`,
`FA 76 FE .. .. 73 …` — matching the layout derived independently from the
3-byte lock. Three things follow, and they are worth more than the CRC answer:

1. The on-air address is **exactly** `18 18 18 18 3A`, all 40 bits, confirmed by
   a hardware correlator rather than by statistics.
2. The register byte order `{0x3A, 0x18, 0x18, 0x18, 0x18}` is right.
3. There is **no packet-control field** between address and payload — the
   transmitter uses legacy ShockBurst, not Enhanced ShockBurst. If a 9-bit PCF
   were present every byte would have come out shifted by one bit.

### The capture mode to use from here  [method]

Real 5-byte address, **CRC off**, 12-byte payload window — firmware command
`n`. This is strictly better than both earlier modes: the promiscuous mode
delivers mostly static and needs software bit-alignment, and the 3-byte lock
wastes two payload bytes on address tail. With the full address matched in
hardware there are no false positives at all, and byte 0 is p0.

It does not fix the slicer bit-errors — nothing at the receiver can, since the
corruption is caused by the `FF` runs in the transmitted data. The answer to
that is redundancy: every message is sent twice, and majority-voting across
repeats of the same event recovers the true bytes.

#### One bug worth recording: the pipe-0 leak

The first `n` run (`captures/20260826-130842_general-timed.log`) kept only
**575 real frames out of an expected ~3,500**, and caught zero button events.
Cause: `openReadingPipe(1, …)` enables pipe 1 but leaves **pipe 0 enabled**
from the earlier promiscuous configuration, and pipe 0's address register still
held the 2-byte bait padded out to five bytes — close enough to all-zeros that
idle noise matched it constantly. Those false frames filled the 3-deep RX FIFO
and pushed the real ones out. 15,868 frames arrived; 15,293 of them were junk.

Fixed in v8 with `closeReadingPipe(0)`. The lesson generalises: on the nRF24,
opening a pipe is not the same as being the only pipe open, and a stale address
in a still-enabled pipe is indistinguishable from a working receiver until you
count what you should have got.

### Would a quieter RF environment help?  [OPEN — evidence says no]

Worth settling, because the operator has offered to drive somewhere with no
ambient 2.4 GHz traffic, and that is a real cost.

**The corruption does not look like interference.** Three reasons:

1. It is **asymmetric** — bits go 1 → 0, essentially never 0 → 1. Interference
   is symmetric; it has no reason to prefer one direction.
2. It is **position-dependent** — payload bytes p2, p6, p7 (`FF`) are shredded
   while p3, p4, p5 in the same packet come through perfectly. Interference
   corrupts whatever it happens to overlap, not specific byte offsets.
3. It is **not bursty**. Collisions with WiFi wipe out whole packets for the
   duration of a frame. What we see is isolated single bits, never two adjacent.

All three point at the receiver's own data slicer losing its DC reference
across a long run of identical symbols — see *The noise model*. That happens at
any signal strength, in any environment, and no amount of quiet fixes it.

**Cheaper things to rule out first, in order:**

1. **The ESP32's own radios.** Its WiFi/BT antenna is centimetres from the
   nRF24; anything it transmits arrives tens of dB stronger than any external
   source. Turned off in v9 — the one interferer we fully control.
2. **A distance A/B test.** Same capture at 30 cm and at 5 m. If the error rate
   is unchanged, it is not signal-to-noise, and the woods would prove nothing.
   If it scales with distance, SNR matters after all and the picture changes.

**When the drive would actually be worth it:** if we ever need to hear the
*buzzers* rather than the controller. They are battery-powered handsets with
PCB antennas transmitting far less often — a much harder target than a
mains-adjacent controller beaconing every 30 ms. That case is not open yet.

Address byte order for `openReadingPipe`, now settled by the lock-on test: the
nRF24 transmits the **high** register byte first, so on-air `18 18 18 18 3A`
is written as `{0x3A, 0x18, 0x18, 0x18, 0x18}`.


#
# The system itself

## Observed game flow  [FACT — from live use]

Reported by the operator, 2026-08-26:

1. Press **General** on the controller → the buzzers become armed.
2. A player buzzes in → that buzzer wins; the rest lock out.
3. Press **Clear** on the controller → the round resets.
4. Repeat from 1.

### Why this matters for capture

**Every round involves at least two controller transmissions**, not just a
buzzer one. "General" has to reach and arm up to 32 buzzers, and "Clear" has
to release them. Both are host downlinks.

That makes the **controller the better capture target**, for three reasons:

- It transmits at **8 mW** versus the buzzers' 2 mW — 6 dB louder.
- Arming a broadcast to 32 nodes is likely repeated or acknowledged, so one
  press probably puts several frames on the air rather than one.
- It fires **on demand**, whenever a button is pressed — no waiting for a
  player, no dependence on game state.

So the operator pressing General/Clear repeatedly is the cheapest way to
generate traffic, and it should be the standard method for every capture from
here on.

### Correction to the capture method  [supersedes §8's tooling]

The first channel-finder used statistical RPD sweeps (`b` then `s`). It
returned **zero hits on all 17 channels**, twice — including the baseline,
which a normal room never produces. Two faults, both mine:

1. **The sampling loop restarted RX every 140 µs.** Each restart costs ~130 µs
   of settling during which the receiver detects nothing, so it was deaf about
   half the time — and RPD *latches*, so constant resetting discarded the very
   property that makes it useful. Fixed: stay in RX for the whole dwell and
   clear the latch only *after* a detection.

2. **Statistical sweeping is the wrong instrument for this signal.** §1 records
   a duty cycle under 1%. Eight button presses is on the order of 8 ms of
   transmission; spread over 17 channels, a sweep will almost never coincide
   with a burst. This was predictable from data already in this document.

Replaced with **`f` (hunt)**: 2.5 s of continuous listening per channel,
cycling, printing live, while the operator presses General/Clear without
stopping. Interactive detection beats averaging when the signal is a handful
of short bursts. **`w` (watch)** does the same on a single locked channel.

### Note on range, which cuts both ways

- **Energy detection (`f`, `w`) wants the board CLOSE** (~0.5–1 m). RPD only
  trips above roughly −64 dBm.
- **Packet capture (`d`) wants it FAR** (2–5 m). The PA+LNA saturates at close
  range and the nRF24 has no receive gain control.

These pull in opposite directions and it is worth stating plainly, because
following the wrong one produces silence with no error message.


---

---

## The host menu — and what F4 means  [FACT]

The host display photo reads `F4 (00-15)` with value `05`.

**TM102 menu** (per manufacturer manual):

| Item | Function | Range |
|---|---|---|
| F1 | Code settings (pairing) | — |
| F2 | Answer sorting — how many places are ranked | 1–5 |
| F3 | Foul penalty rounds | — |
| **F4** | **Channel setting** — "frequency channel assignment and synchronization with extension buttons" | **00–15** |

16 menu values ↔ 16 certified channels. This is the channel selector.

**⇒ Primary capture target: F4=05. If the menu is 0-based, that is
2435 MHz — `RF_CH = 35`.** If 1-based, 2432 MHz / `RF_CH = 32`. Try 35
first, then 32, then sweep the other 14.

Useful cross-check: the README guessed "channel 50 (2450 MHz)". That is a
real channel in this system (index 11) — so if F4 is changed to 10 or 11,
traffic should appear at RF_CH 50. Changing F4 and watching the capture move
is the cleanest confirmation available and costs nothing.

**TM101 menu** (older sibling, from its FCC user manual) — different layout,
but it reveals the underlying data model:

| Item | Function | Range |
|---|---|---|
| F1 | Answer time | 1–999 s |
| F2 | Learning number — assigns each responder its node number; `00` pairs the remote | 1–32 |
| F3 | Local ID — *"you need to relearn the number after changing the ID"* | 1–15 |
| F4 | Answer mode | 1–3 |

So each buzzer carries a **node number 1–32** assigned by the host at pairing,
and the system carries a **local/network ID**. Expect a node-ID byte in the
payload with values `0x01`–`0x20`.

---

## Pairing procedure  [FACT]

Hold the buzzer's button ~2 s → blue light flashes (entering pairing) → red
flash on success. The host assigns the node number. This is a deliberate,
observable RF event and a **good first capture target** — it is the one time
you can force traffic on demand without guessing game state.

---

## Does the host transmit? Yes  [FACT]

Previously open. Settled by finding the **second FCC grant**:

| FCC ID | Equipment class | Device |
|---|---|---|
| 2A3NOTM101 | Intelligent Responder-**Buttons** | the buzzers |
| **2A3NOTM101A** | Intelligent Responder-**Displayer** | **the host** |

Both are granted as *"Part 15 Low Power Communication Device **Transmitter**"*
over 2420–2465 MHz. The host has its own transmitter authorisation, so
Retekess's spec-page line *"receiving 2.4GHz, no transmission"* is simply
wrong — as its own listed 8 mW transmit power already implied.

**⇒ Expect two directions of traffic on the same channel**: buzzer uplinks
(press events) and host downlinks (LED colour, lock, clear, mode, pairing).
Do not assume a lone packet is a button press.

---

# Hardware

## The capture rig — confirmed hardware  [FACT]

| Part | Item |
|---|---|
| MCU board | **ShillehTek ESP-WROOM-32 (ESP-32S) dev board**, CP2102 USB-serial, USB-C, pre-soldered headers |
| Radio | **nRF24L01+PA+LNA** with SMA antenna |
| Carrier | **Socket adapter board with on-board 3.3V regulator** |

### This resolves the VCC contradiction — `skills.md` was right

`README.md` said *"VCC → 3.3V, do NOT connect to 5V"*; `skills.md` said
*"VCC → VIN (5V routed through external adapter board regulator)"*.

**`skills.md` is correct for this build.** The README's warning applies to a
*bare* nRF24 module, whose 3.0–3.6 V supply pin is destroyed by 5 V. This rig
has the module seated on a **regulated socket adapter**, which takes **5–12 V
in (5 V typical)** and produces the 3.3 V the module needs.

**⇒ Adapter VCC → ESP32 `VIN` (5 V). Do NOT wire it to the ESP32's 3.3 V pin.**
Feeding 3.3 V into a 3.3 V LDO leaves it in dropout, producing roughly 2.9 V —
below spec, and it will brown out on transmit bursts. The failure mode is
intermittent and looks like a flaky radio, which makes it expensive to debug.

Logic levels need no attention: the ESP32 is a 3.3 V part and the adapter
passes CE/CSN/SCK/MOSI/MISO straight through. Nothing to level-shift.

### Pinout (both source documents agree, and all pins are free on a 38-pin board)

| nRF24L01+ | ESP32 GPIO |
|---|---|
| CE | 4 |
| CSN | 5 |
| SCK | 18 |
| MISO | 19 |
| MOSI | 23 |
| IRQ | not connected |
| VCC | **VIN (5 V)** |
| GND | GND |

GPIO 5 is an ESP32 strapping pin that must read HIGH at boot — harmless here,
since SPI chip-select idles HIGH by design.

### PA+LNA implications for *sniffing* specifically

The PA+LNA variant is built for range, which cuts both ways for capture:

- **Good:** the LNA adds roughly 20 dB of receive gain, so weak buzzer bursts
  are far more likely to trip the receiver.
- **Bad:** at close range it will **saturate**. The nRF24 has no receive gain
  control, so an overloaded front end simply stops decoding. If captures are
  empty with the buzzer sitting next to the antenna, **move further away**
  (2–5 m) rather than closer. This is a genuinely counter-intuitive failure
  mode and worth trying early.
- PA+LNA modules are sensitive to supply noise. If capture is unstable, a
  10–100 µF electrolytic across the module's VCC/GND is the standard fix.

### Toolchain notes

- `arduino-cli` 1.5.2 is already downloaded (`~/Downloads`).
- Board FQBN: `esp32:esp32:esp32` (ESP32 Dev Module).
- Serial port on macOS will appear as `/dev/cu.usbserial-*` or
  `/dev/cu.SLAB_USBtoUART`. **CP2102** boards sometimes need Silicon Labs'
  VCP driver on macOS — if no port appears, that is the first thing to check,
  not the board.
- Baud 115200.

---

---

## The buzzer PCB  [FACT / OPEN]

From internal photos (exhibit 6072181, board silkscreen `JTQ1_V28 2021/11/13`).
**Caveat: the photographed unit is the TM101-generation buzzer** (blue, 2×AAA),
not the grey Li-ion TM102 puck. Retekess ships the newer product under the
older 2022 grant.

- Radio is a bare **QFN-20** (`U4`) beside crystal `X1`, feeding a meander PCB
  trace antenna. No module, no shield can.
- MCU is a discrete **TSSOP-28** with a 6-pin programming header `P1`.
  MCU and radio are separate parts on SPI — so an **SPI bus tap is viable**
  as a fallback if over-the-air capture stalls.
- 6× RGB LEDs, one microswitch.

The buzzer's own chip marking is unreadable (~60×60 px in a 1069×802 photo),
but §6 identifies the family from the host side. Schematics, block diagram
and operational description are **permanently withheld** under granted
long-term confidentiality on both grants.

---

# Method

## Capture strategy  [method]

~~XN297 sync-word approach~~ — **withdrawn.** §6 establishes the radio is an
nRF24L01+-compatible Si24R1, so there is no XN297 sync word and no
scrambling. `XN297Dump_nrf24l01.ino` is the wrong tool for this device.

The correct two-phase approach:

**Phase 1 — address discovery (Goodspeed promiscuous).**
Address width 2 bytes (`SETUP_AW = 0b00`, the illegal-but-functional value),
CRC off, auto-ack off, payload 32 bytes, RX address `0x00 0x00` or
`0xAA 0xAA`. Ambient noise trips the receiver and the chip dumps raw on-air
bits. Park on the one likely channel rather than sweeping: **`RF_CH = 35`**
(F4=05, 0-based). 1 Mbps only. Then search the dumps for a byte sequence that
recurs across many captures — that is the real address, shifted by whatever
bit offset the false sync introduced.

Force traffic during this phase rather than waiting: the pairing sequence
(§4) is triggerable on demand.

**Phase 2 — locked capture.**
Once the address is known: standard `RF24` configuration, real address, CRC
on, dynamic payloads, and the packets decode normally. From there the node-ID
byte falls out by pressing buzzer 1 ten times, then buzzer 2 ten times, and
diffing — which is the one piece of the original PDF's methodology worth
keeping.

The search space is now **16 channels × 1 data rate**, with one prime
suspect — down from the 85 × 3 = 255 combinations a blind sweep would need.

---

## Finding the channel: RF_CH 50 / 2450 MHz  [FACT — measured]

Hunt run, 2026-08-26, board ~1 m from the controller, operator cycling
General/Clear for 85 s:

| RF_CH | MHz | energy hits |
|---|---|---|
| **50** | **2450** | **277** |
| 26 | 2426 | 8 |
| 47 | 2447 | 4 |
| *all other 14* | — | **0** |

A 35x separation over the next-strongest channel. Not ambiguous.

Packet counts were flat across all channels (2000-3300 each), exactly as
expected: those are noise-triggered captures and carry no channel information.
The energy column is what found it.

### Two things this changes

**1. The README's guess was right.** It said *"might operate on Channel 50
(2450 MHz)"* — and it does. Section 7 lists several things that document got
wrong; this was not one of them. Worth recording, because the same file's
2 Mbps claim was wrong and it would be easy to dismiss the whole thing.

**2. The F4 menu mapping is NOT sequential — my inference in §3 was wrong.**

2450 MHz is entry **11 of 16** in the certified channel list. The host display
showed `F4 = 05`. So F4 maps to neither index 5 nor index 6, and the
assumption that menu value N selects the Nth channel in the FCC report's order
is dead.

Possible explanations, none yet tested:
- The device's internal channel ordering differs from the order the test lab
  happened to tabulate.
- F4 was changed between the photograph and this measurement.
- The mapping is deliberately non-obvious.

**This does not matter for capture** — we have the frequency empirically. It
matters only if we later want to predict the channel from the menu, or follow
the system when someone changes F4. Cheap experiment when needed: set F4 to a
few known values and re-run the hunt.

The wider lesson holds: the operator was right to question the 0-based
assumption, and measuring rather than assuming is what caught it.

### Corrections to earlier method notes

- Zero energy on 14 channels while packets flowed steadily confirms the **RPD
  detector works fine on this module** — the earlier all-zero result was
  entirely the restart-every-140us bug, not stubbed hardware.
- Saturation worry was overstated for this geometry: at ~1 m from an 8 mW
  transmitter the received level is roughly -31 dBm, about -11 dBm after LNA
  gain, comfortably under the nRF24's 0 dBm limit. **~1 m is fine for capture**;
  the "move 2-5 m away" advice applies only if captures come back empty.


---

---

# Record

## Questions closed so far

Kept as a record of what was once unknown and how it was settled. Live
questions are listed under *Where we are* at the top.

| Question | Outcome |
|---|---|
| ~~Is the radio XN297, BK2425 or nRF24?~~ | **Si24R1** (nRF24L01+ clone), read off the host's RF module in FCC internal photos |
| ~~1 Mbps or 2 Mbps?~~ | **1 Mbps** — stated in the FCC test report and confirmed by its measured 1.15 MHz 20 dB bandwidth |
| ~~How many channels, and where?~~ | **16**, 2420–2465 MHz at 3 MHz spacing, listed in the test report |
| ~~Which channel is in use?~~ | **RF_CH 50 / 2450 MHz**, measured — 277 energy hits vs 0–8 elsewhere |
| ~~Does the host transmit at all?~~ | **Yes** — it holds its own transmitter grant, FCC ID 2A3NOTM101A |
| ~~What is the ESB address?~~ | **`18 18 18 18 3A`** on air, in 87% of signal-carrying captures |
| ~~Is CRC 1 byte or 2?~~ | **Neither — CRC is off.** Six variants tested offline, all at chance |
| ~~Is `F4` 0-based, i.e. is the channel 2435 or 2432?~~ | **Neither.** 2450 MHz is entry 11 of 16 while `F4` read 05, so the mapping is not sequential |
| ~~Does the host's own test report differ from the buzzer's?~~ | **No** — identical PHY, same 16-channel list, same 1 Mbps |

---

## Errors in the pre-existing PDF and README  [REFUTED]

It cites "image 1, image 3" for claims the photos contradict, and its Works
Cited are six hobbyist blog links — it never cites an FCC exhibit despite
claiming to synthesise OET filings.

1. **Power sources swapped.** PDF: host USB-C, responders 3×AAA. Reality:
   host is 3×AAA (~8 h); TM102 buzzers have a built-in 500 mAh Li-ion
   (2.7–4.2 V), USB-C charged, ~25 h.
2. **The anti-polling argument rests on the swapped specs** — §2.1 leans on
   "25 hours on AAA alkaline", but 25 h is the Li-ion buzzer figure. The
   conclusion may hold; the argument doesn't support it.
3. **Link budget wrong.** Derives "~0 to +2.5 dBm" by hand; manufacturer
   states 8 mW host / 2 mW buzzer.
4. **45 channels — wrong.** It reasons "1 MHz bandwidth over 2420–2465 gives
   up to 45 discrete channels." The certified answer is **16 channels at
   3 MHz spacing.** Sweeping 45 or 85 channels wastes ~⅔ of dwell time on
   frequencies the system never uses.
5. **CSMA/CA, randomised backoff, ACK arbitration, 4-field packet framing —
   invention.** Plausible, conventional, entirely uncited. Hypotheses only.
6. **TM101 ≠ TM102** — the PDF treats them as one system. Different products
   sharing a family grant.
7. ~~2420–2465 MHz unsourced~~ — **the PDF was right here and I was wrong to
   doubt it.** It is on the grant itself.

### Also corrected
- `skills.md`: nRF24L01+ minimum address width is **2** bytes, not 3 — the
  illegal-but-functional setting is the whole basis of the Goodspeed exploit.
- `skills.md`: sweeping channels 5–85 is wrong for this device; sweep only
  the 16 certified channels above.
- `README.md`: 2 Mbps is wrong; it is 1 Mbps.

---

## Constraints

Cloud sandbox with the project folder mounted; **no access to the Mac's USB
serial ports**. Firmware and analysis tooling are written here; flashing and
capture happen on the Mac (arduino-cli), with logs saved back into this folder.

Agreed scope: **RX only** until decoded. Capture rig first, then decide.

---

---

## Sources

- FCC ID 2A3NOTM101 (buzzers) — test report LCSA072222029EA, internal photos,
  user manual, label. https://fccid.io/2A3NOTM101
- FCC ID 2A3NOTM101A (host) — https://fccid.io/2A3NOTM101A
- Retekess TM102 product + host spec pages, TM102 manual (ManualsLib 3718877)
- Si24R1 datasheet, Nanjing Zhongke Microelectronics (via LCSC C14436)
- keszoox.com — nRF24L01+PA+LNA with socket adapter board (this rig's radio)
- shillehtek.com — ESP-WROOM-32 / CP2102 / USB-C dev board (this rig's MCU)
- ProtoSupplies — nRF24L01 breakout adapter with regulator, 5–12 V in / 3.3 V out
- Travis Goodspeed, "Promiscuity is the nRF24L01+'s Duty" (2011)


---

---


---

# Keeping this file current

This document is the project record. When something new is established:

- Add it under the right heading, tagged **[FACT]** / **[OPEN]** / **[REFUTED]**.
- Update **Where we are** and the **Timeline** at the top.
- **Strike through superseded claims rather than deleting them**, and say what
  replaced them. Several conclusions here have already been overturned —
  XN297 vs Si24R1, the F4 mapping, the energy-scan method — and the reasoning
  is only auditable if the wrong turns stay visible.
- Record negative results too. "Six CRC variants tested, none validated" is a
  finding, and without it someone will spend a day re-testing them.
