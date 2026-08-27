# Retekess TM102 — passive protocol decoder

A receive-only decoder for the Retekess TM102 wireless quiz-buzzer system.
Plug an ESP32 into anything that can read a serial port and get the game as
JSON:

```json
{"t":24110,"ev":"general"}
{"t":26902,"ev":"buzz","buzzer":3}
{"t":31447,"ev":"lock"}
{"t":39880,"ev":"clear"}
```

The TM102 has no API, no serial port and no documentation beyond a one-page
manual. This project works out what its controller transmits, and turns it into
an event stream you can build a scoreboard, sound effects or a stream overlay
on top of.

**The radio never transmits.** Auto-acknowledgement is disabled in hardware; it
cannot acknowledge, reply, or disturb a game in progress.

---

## What it decodes

| | |
|---|---|
| **General** pressed | buzzers armed |
| **Clear** pressed | back to idle |
| **Lock** pressed | re-armed, excluding whoever just answered |
| **Buzz-in** | *which* handset answered first, 1–32 |
| **Random** | the mode starting and resolving |

Verified against known ground truth: 8 of 8 handsets identified in a test where
the handset order was declared in advance, and 83 General / 83 Clear / 73
buzz-ins decoded across six capture sessions with no errors.

### What it cannot decode, and why

Two things are **not knowable to any passive receiver**, and it is worth
knowing that up front rather than hunting for them:

- **Which handsets are locked out.** No mask is transmitted. A locked handset
  simply goes silent; the exclusion is enforced inside the handset itself.
  Reconstruct it by tracking who has buzzed since the last `clear`.
- **Which handset a Random draw picked.** Not transmitted, not inferable from
  press timing, and no later event reveals it.

The pattern behind both: **this controller broadcasts state transitions, not
decisions.** Anything a handset decides locally stays local.

---

## The protocol, briefly

| | |
|---|---|
| Radio | Si24R1 — an nRF24L01+ clone, standard ShockBurst |
| Modulation | GFSK, 1 Mbps |
| Channel | RF_CH 50 (2450 MHz) by default; the host's `F4` menu picks one of 16 |
| Address | `18 18 18 18 3A`, 5 bytes |
| CRC | off |
| Payload | 8 bytes |

```
on air:  55 | 18 18 18 18 3A | p0 p1 p2 p3 p4 p5 p6 p7
          preamble   address           payload
```

`p1` carries the game state, `p6` the handset that last answered, and `p3`/`p4`
mark a standing answer or a button command. Full derivation, including the
measurements behind every claim, is in **[FINDINGS.md](FINDINGS.md)**.

---

## Hardware

About $15. An **ESP32** (this build: ESP-WROOM-32, CP2102, USB-C) and an
**nRF24L01+PA+LNA** module on a regulated socket adapter.

| Module pin | ESP32 pin |
|---|---|
| VCC | **VIN** (see note) |
| GND | GND |
| CE | GPIO 4 |
| CSN | GPIO 5 |
| SCK | GPIO 18 |
| MISO | GPIO 19 |
| MOSI | GPIO 23 |

> **VCC goes to VIN, not 3.3V.** That is correct for *this* build only. The
> PA+LNA module sits on an adapter board with its own regulator; feeding that
> regulator 3.3 V leaves it in dropout at ~2.9 V and the radio browns out. A
> **bare** nRF24 module is the opposite — 3.3 V only, never 5 V.

---

## Quickstart

```bash
./go.sh setup     # one-time: arduino-cli, ESP32 core, RF24 library
./go.sh flash     # compile and upload
```

Then either read the serial port yourself, or:

```bash
python3 tools/live.py           # live readout, speaks each event aloud
python3 tools/live.py --verify  # walk through handsets and check each one
```

The board starts decoding on power-up. Nothing has to be sent to it.

### On a Raspberry Pi

See **[pi/README.md](pi/README.md)**. Copy `pi/`, plug in the board, and read
lines. The decoding happens on the ESP32, so consumers need no library and no
Python.

---

## Layout

| | |
|---|---|
| `FINDINGS.md` | the full technical record — every measurement, and every wrong turn, struck through rather than deleted |
| `firmware/` | the ESP32 sketch: capture modes and the on-board decoder |
| `tools/` | capture, decode, verification and analysis scripts |
| `pi/` | deployment package: event streamer, systemd unit, event schema |
| `captures/` | every raw capture session — the evidence behind the findings |
| `pics/`, `docs/` | FCC exhibits, hardware photos, and the original pre-research README |
| `event_map.json` | the decode map; measured values are the authority, the formula is a fallback |

`START-HERE.md` is the plain-language runbook; `QUICKSTART.md` the terse one.

---

## How this was worked out

The short version: FCC filings first, then a $15 capture rig, then a loop of
*write the prediction down, then measure*. That loop caught several confident,
plausible, wrong conclusions — a handset-ID formula that fit four of eight
cases, ambient WiFi mistaken for a discovery, a bug that only surfaced when a
person pressed two buttons at once.

`FINDINGS.md` keeps all of them, struck through rather than deleted, so the
reasoning stays auditable.

## Legal

Receive-only, on hardware I own, for interoperability with equipment I bought.
The FCC exhibits in `pics/` are public records retrieved from the Commission's
filing database.

Everything original here is MIT licensed — see [LICENSE](LICENSE). The Retekess
user manual included for reference remains the property of its publisher.
