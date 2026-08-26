# Start here

Three commands, then two letters. That's the whole thing.

---

## Before you plug anything in

Check one wire. On the little adapter board the radio sits in, the pin marked
**VCC** must go to the ESP32 pin marked **VIN**.

**Not** the pin marked 3.3V. That one wire is the difference between "works"
and "works erratically in a way that's miserable to diagnose" — the adapter
has its own voltage regulator, and feeding it 3.3V starves it.

The rest, if you want to double-check:

```
VCC  -> VIN          CE   -> 4          SCK  -> 18
GND  -> GND          CSN  -> 5          MISO -> 19
                     IRQ  -> nothing    MOSI -> 23
```

---

## Step 1 — one-time setup

Open Terminal and run:

```bash
cd ~/Documents/web-apps/score-keeperv5
./go.sh setup
```

This downloads the ESP32 board support and the radio library. Takes a few
minutes. You only ever do this once.

## Step 2 — plug in the ESP32

Use a **data** USB-C cable, not a charge-only one. That catches people out:
a charge-only cable powers the board so its light comes on, but the Mac never
sees it.

## Step 3 — build, flash, and start listening

```bash
./go.sh
```

That compiles the firmware, puts it on the board, and opens a live window.

---

## What you'll see

The board runs its own check and tells you in plain English:

```
============================================================
  SELF-TEST: PASS
  The radio is configured and listening correctly.
  ...
============================================================
```

**If it says PASS**, carry on to the next section.

**If it says FAIL**, it lists what to check, in order. It's almost always the
VCC/VIN wire. Fix it, press the small RESET button on the ESP32, and it will
re-run the check.

You don't need to read the hex numbers above the banner. They're there so
that if something is wrong, I can see exactly what — paste them to me.

---

## Finding the channel — skip unless F4 changed

The buzzers use one of 16 frequencies. We don't know which, so we hunt for it.

**Put the ESP32 about 1 metre from the controller** for this step. (Later,
for capturing, you'll want it further away — but energy detection needs a
strong signal.)

Type **`f`** and press Enter.

Now here's the important part: **keep pressing General, then Clear, over and
over, without stopping**, for about 45 seconds. Every General press makes the
controller transmit, and that's what we're hunting for. If you stop pressing,
there's nothing to find.

You'll see it walk the channels live:

```
listening RF_CH 20  (2420 MHz) ... 0 hits
listening RF_CH 23  (2423 MHz) ... 0 hits
listening RF_CH 35  (2435 MHz) ... 14 hits   <=== SOMETHING HERE
```

That's your channel. **Tell me the number and I'll take it from there.**

If a full pass finds nothing anywhere, move **closer** (half a metre) and run
`f` again. Energy detection is the one step where closer is better.

---

## Step 3 — addressed capture   (channel is already known: RF_CH 50)

The channel is **50 (2450 MHz)**, measured on 2026-08-26. You don't need to
hunt again unless someone changes `F4` on the controller.

```bash
./go.sh capture addressed
```

Then type:

```
r50      <- lock to 2450 MHz
n        <- addressed capture
```

`n` tunes the radio to the controller's real address, all 40 bits of it,
checked in hardware. Nothing else on 2.4 GHz can get through that, so
everything you see is genuinely from your controller — no static to filter, and
the first byte of every line is the first byte of the message.

Lines appear about thirty times a second, because the controller beacons
constantly whether or not anyone touches it. That is normal. Press `x` when
you're done, then Ctrl-C.

## Watch it live

This is the quickest way to see whether the decoding actually works. Plug the
board in and run:

```bash
python3 tools/live.py
```

Nothing to type — it drives the board itself. Press buttons and it tells you
what it sees:

```
  19:42:03   GENERAL pressed              -> buzzers armed
  19:42:05   BUZZER 3 buzzed in first
  19:42:07   CLEAR pressed                -> idle
```

Every five seconds it prints a status line with the frame rate, so you can
tell the difference between "nothing is happening" and "nothing is being
received". Ctrl-C to stop. The session is logged to `captures/` like any other
run, so a live session is still a capture you can re-analyse afterwards.

To try it without the hardware, replay a capture through the same decoder:

```bash
python3 tools/live.py --replay captures/20260826-133344_buzz-2.log
```

## Step 4 — the one capture that answers "which button is which"

I can see two distinct controller messages, and I know one is General and one
is Clear. I cannot tell which, because they always alternate — and alternation
looks identical whichever way round it is.

This capture breaks the tie. It is short, and the timing is the whole point:

```bash
./go.sh capture general-timed
```

```
r50
n
```

Then, slowly and deliberately, five times:

1. Press **General**
2. Count to **ten**
3. Press **Clear**
4. Count to **ten**

Nothing else. No buzzing in. The long gaps are what make the order readable.

## Step 5 — the buzzer captures

These are the ones that build the actual mapping. Same setup each time, one
buzzer per file, about 25 rounds each, unhurried:

```bash
./go.sh capture buzz-1        # General -> buzzer 1 -> Clear, x25
./go.sh capture buzz-2        # General -> buzzer 2 -> Clear, x25
./go.sh capture buzz-3        # General -> buzzer 3 -> Clear, x25
./go.sh capture buzz-1-then-2 # General -> buzzer 1, THEN buzzer 2 -> Clear, x25
```

The difference between `buzz-1` and `buzz-2` is the byte that carries the
buzzer's identity. That byte is the heart of the JSON file you want.

`buzz-1-then-2` answers the other half: whether the controller broadcasts the
*ranking* — who was first, who was second — or only the winner.

After any capture:

```bash
python3 tools/decode_events.py captures/*buzz-1.log --summary
```

---

## Command reference

| Type | What it does |
|---|---|
| `r50` | Lock to 2450 MHz — the known channel |
| `n` | **Addressed capture** — real 5-byte address, CRC off. Use this one |
| `m` | CRC sweep — already run, answer was "no CRC" |
| `k` | Lock onto the address by brute force (fallback if `m` finds nothing) |
| `f` | Re-hunt the channel (only if `F4` changed) |
| `w` | Watch one channel live for activity |
| `b` / `s` | Statistical scan (weak for this system — prefer `f`) |
| `r35` | Listen on a specific frequency |
| `d` | Start capturing (quiet by design — static is filtered out) |
| `g` | Turn the static filter off/on |
| `x` | Stop capturing |
| `i` | Re-run the self-test |
| `?` | Help |

Nothing here transmits. The board only listens.
