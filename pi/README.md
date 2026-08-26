# TM102 events on a Raspberry Pi

Plug the ESP32 into the Pi. Read one JSON object per line. That is the whole
integration surface.

```
$ ./tm102-events
{"t":19823,"ev":"ready","state":"idle","buzzer":null}
{"t":24110,"ev":"general"}
{"t":26902,"ev":"buzz","buzzer":3}
{"t":31447,"ev":"lock"}
{"t":36002,"ev":"buzz","buzzer":5}
{"t":39880,"ev":"clear"}
```

The decoding happens **on the ESP32**, not here. `tm102-events` only finds the
board, keeps it connected, and passes its output through. So a consumer in any
language can skip this program entirely and read the serial port directly —
this is a convenience, not a dependency.

## Setup

```bash
sudo apt install python3-serial          # or: pip3 install pyserial
sudo usermod -aG dialout $USER           # then log out and back in
./tm102-events --pretty                  # check it works
```

Nothing needs to be sent to the board. It starts decoding on power-up, so it
survives the Pi rebooting, your program crashing, or being plugged into a
machine that has never heard of this project.

To run it at boot, see the header of `tm102-events.service`.

## Events

| `ev` | meaning | extra |
|---|---|---|
| `boot` | board started | `fw`, `schema`, `channel` |
| `ready` | state when we joined — **not** something that just happened | `state`, `buzzer` |
| `general` | General pressed; buzzers armed | |
| `buzz` | somebody buzzed in first | `buzzer` 1–32 |
| `lock` | re-armed without a Clear — normally the Lock button | |
| `clear` | Clear pressed; back to idle | |
| `random_start` | Random pressed once; handsets blinking | |
| `random_pick` | Random pressed again; one handset chosen | |
| `random_end` | random mode ended | |
| `winner_changed` | the winner field moved with no state change | `buzzer` |

`t` is the board's uptime in milliseconds. It is a monotonic clock for ordering
and measuring gaps — it is not wall-clock time, and it resets when the board
loses power. Stamp arrival time yourself if you need dates.

## Things worth knowing before you build on this

**`ready` is not an event.** It reports the state the board found on joining. In
particular its `buzzer` is whoever last answered *since the controller was
powered on* — possibly hours ago. Do not treat it as a buzz.

**Which handsets are locked out is not knowable.** The controller never
transmits it; a locked handset simply goes silent, and the exclusion is enforced
inside the handset. If you need that set, track it yourself: everyone who has
buzzed since the last `clear` is locked out. A `clear` is a reliable resync
point — the locked set is provably empty there.

**`random_pick` does not say who was picked.** That is not a limitation of this
decoder. The controller does not transmit it, it cannot be inferred from timing,
and no buzz follows to reveal it. If your software needs a random draw, do the
draw yourself — you know all the handsets and the controller does not need to be
involved.

**`lock` is named for its usual cause, not a certainty.** It is emitted on the
answered → armed transition, which the Lock button produces. Rare spontaneous
ones have been observed.

**Duplicates are already suppressed.** The controller sends every message twice
about 11 ms apart, and repeats mode commands every ~3 s. You get one event per
real occurrence.

**Nothing is ever transmitted.** The radio is receive-only by construction: it
never acknowledges, never replies. It cannot disturb a game in progress.

## If it goes quiet

`tm102-events` writes the board's own output to stderr, so a failure announces
itself rather than looking like silence:

- `SELF-TEST: FAIL` or `HALTED` — the radio did not come up. Check that the
  radio adapter's **VCC goes to VIN, not 3.3V**; fed 3.3V its regulator sits in
  dropout and browns out.
- No events but no errors — the controller may be on a different channel. It is
  set in the host's `F4` menu; the board defaults to RF_CH 50 (2450 MHz). Type
  `f` into the serial port to re-hunt.
