# Phase 1 runbook — find the channel, then the address

Read `FINDINGS.md` for *why* any of this is the way it is. This is just the
sequence of moves. Everything here is **receive-only**.

## 0. Check the wiring first

| nRF24 adapter | ESP32 |
|---|---|
| VCC | **VIN (5 V)** — not 3.3 V |
| GND | GND |
| CE | GPIO 4 |
| CSN | GPIO 5 |
| SCK | GPIO 18 |
| MISO | GPIO 19 |
| MOSI | GPIO 23 |
| IRQ | not connected |

The one that bites: **VCC must not go to the ESP32's 3.3 V pin.** The adapter
has a 3.3 V regulator; feeding it 3.3 V leaves it in dropout at ~2.9 V and it
browns out intermittently, which reads as a flaky radio rather than a wiring
fault.

## 1. Toolchain

```bash
# arduino-cli — brew is the easiest route on macOS
brew install arduino-cli

arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "RF24"

pip3 install pyserial
```

## 2. Build and flash

```bash
cd ~/Documents/web-apps/score-keeperv5

arduino-cli compile --fqbn esp32:esp32:esp32 firmware/tm102_sniffer

arduino-cli board list                      # find the port
arduino-cli upload --fqbn esp32:esp32:esp32 \
  -p /dev/cu.usbserial-0001 firmware/tm102_sniffer
```

If no port appears: this board's USB chip is a **CP2102**, and macOS sometimes
needs Silicon Labs' VCP driver. Check that before suspecting the board.

## 3. Confirm the radio is really in promiscuous mode

```bash
python3 tools/capture.py --label sanity
```

Then type `i`. Every line must read *good*:

```
CONFIG   0x0B   CRC off (good) ...
EN_AA    0x00   auto-ack disabled (good)
SETUP_AW 0x00   address width 2 bytes — exploit ACTIVE (good)
RF_SETUP 0x00   rate 1 Mbps (good)
```

`SETUP_AW` is the one that matters. 2-byte address width is officially an
illegal value; the whole capture technique depends on this clone accepting it.
If it reads anything but `0x00`, stop — captures will be empty and no amount
of listening will help.

## 4. Find the live channel

The host's `F4` menu (00–15) picks the channel, but the mapping from menu
value to frequency is **not confirmed** — don't assume `F4=05` means 2435 MHz.
Measure it:

```
b      <- with buzzers idle and untouched. ~12 s. Records the room.
s      <- press a buzzer over and over throughout. ~12 s.
```

Read the **`vs base`** column, not the raw counts: 2420–2465 MHz sits under
WiFi channels 1–9, so there is plenty of energy that has nothing to do with
the buzzers. The channel that *rises* when you press is the one.

Then lock it: `r35` (raw RF_CH) or `c6` (certified index 1–16).

Worth doing once: change `F4` on the host and re-run. If the live channel
moves as predicted, the menu mapping is confirmed and `FINDINGS.md` §3 can be
promoted from inference to fact.

## 5. Capture

```bash
python3 tools/capture.py --label buzzer1-press
```

`d` to dump, then press buzzer 1 repeatedly for a few minutes. `x` to stop.

Then the same for buzzer 2 in its own labelled run — the diff between the two
is what isolates the node-ID byte.

**Best single target: pairing.** Hold a buzzer's button ~2 s. It is the only
event you can trigger on demand, and it forces traffic in both directions.

### If captures come back empty

In order of likelihood:

1. **Wrong channel.** Redo step 4.
2. **Receiver saturated.** The PA+LNA overloads at close range and simply
   stops decoding — and the nRF24 has no receive gain control. Move the board
   **2–5 m away**. This is the opposite of everyone's instinct and costs
   nothing to try.
3. **Not enough traffic.** Use pairing.
4. **Low hit rate is normal.** The exploit relies on noise coincidentally
   matching the bait pattern. Let it run; try `aa` for the other bait value.

## 6. Find the address

```bash
python3 tools/analyze_capture.py captures/*buzzer1-press.log
```

Anything above ~40% of packets is a real candidate; noise sits near 0%. On
synthetic data with a known answer this ranks the true address first at 64%
while pure noise peaks at 0.4%.

If nothing scores, retry with `--loose` — that drops the preamble anchor and
returns the true address plus seven bit-shifted aliases, and the hardware
decides which is real.

## 7. What comes after

With an address in hand, phase 2 is an ordinary `RF24` receiver: real address,
CRC on, auto-ack off. Packets should then arrive cleanly and constantly, and
the node-ID byte falls out of the buzzer-1 vs buzzer-2 diff.

Still receive-only. Transmit is a separate decision, once we know what we'd
be transmitting.
