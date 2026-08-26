#!/usr/bin/env bash
# go.sh — set up, build, flash, and start capturing. One command.
#   ./go.sh            build + flash + open the capture session
#   ./go.sh setup      first time only: install the ESP32 core and RF24 library
#   ./go.sh build      compile only, don't touch the board
#   ./go.sh capture    skip flashing, just start logging
set -u

cd "$(dirname "$0")"
CLI="./arduino-cli"
FQBN="esp32:esp32:esp32"
SKETCH="firmware/tm102_sniffer"
STAMP=".last_flash"

say()  { printf "\n\033[1m>> %s\033[0m\n" "$*"; }
fail() { printf "\n\033[31m!! %s\033[0m\n" "$*"; exit 1; }

[ -x "$CLI" ] || chmod +x "$CLI" 2>/dev/null
[ -x "$CLI" ] || fail "arduino-cli not found next to this script."

find_port() {
  "$CLI" board list 2>/dev/null | awk '/usbserial|SLAB|wchusb|usbmodem/ {print $1; exit}'
}

do_setup() {
  say "Installing the ESP32 board support and the RF24 library (a few minutes)"
  "$CLI" config init --overwrite >/dev/null 2>&1
  "$CLI" config add board_manager.additional_urls \
      https://espressif.github.io/arduino-esp32/package_esp32_index.json >/dev/null 2>&1
  "$CLI" core update-index                || fail "could not reach the Arduino index — check your internet"
  "$CLI" core install esp32:esp32         || fail "ESP32 core install failed"
  "$CLI" lib  install "RF24"              || fail "RF24 library install failed"
  say "Setup done."
}

do_build() {
  say "Compiling"
  "$CLI" compile --fqbn "$FQBN" "$SKETCH" || fail \
"Compile failed.
If it says 'platform not installed' or 'RF24.h: No such file', run:  ./go.sh setup"
  say "Compiled cleanly."
}

do_flash() {
  PORT="$(find_port)"
  FOUND_PORT="$PORT"
  [ -n "$PORT" ] || fail \
"No board found.

  - Is the ESP32 plugged in with a DATA usb-c cable? (charge-only cables are
    the usual culprit - they power the board but carry no data)
  - This board uses a CP2102 chip. macOS sometimes needs Silicon Labs' VCP
    driver before the port shows up.
  - Run  ./arduino-cli board list  to see what the Mac can see."

  say "Flashing $PORT"
  "$CLI" upload --fqbn "$FQBN" -p "$PORT" "$SKETCH" || fail \
"Upload failed. Some boards need the BOOT button held while it says 'Connecting...'.
Try again and hold BOOT."
  touch "$STAMP"
  say "Flashed OK — the board now has the latest firmware."
}

# Has the sketch changed since we last flashed? Capturing with stale firmware
# wastes a whole session and the symptom is confusing (missing commands), so
# check rather than trust the operator to remember.
is_stale() {
  [ ! -f "$STAMP" ] && return 0
  [ -n "$(find "$SKETCH" -name '*.ino' -newer "$STAMP" 2>/dev/null)" ] && return 0
  return 1
}

do_capture() {
  if is_stale; then
    say "Firmware source is newer than the last flash — flashing first"
    do_build
    do_flash
    sleep 2
  fi
  PORT="${FOUND_PORT:-$(find_port)}"
  python3 -c "import serial" 2>/dev/null || {
    say "Installing pyserial"; pip3 install --quiet pyserial || fail "pip3 install pyserial failed"; }
  say "Starting capture. Board commands go straight into this window."
  echo "   r50 = lock to the known channel (2450 MHz)"
  echo "   n   = CAPTURE on the real address. Start here."
  echo "   k   = brute-force the address          d = promiscuous capture"
  echo "   f   = re-hunt the channel   w = watch energy"
  echo "   x   = stop                  ? = full help"
  echo
  echo "   If nothing appears, press the small RESET/EN button on the ESP32 —"
  echo "   that reprints the startup banner so you can confirm what is running."
  echo
  python3 tools/capture.py --label "${LABEL:-session}" ${PORT:+--port "$PORT"}
}

case "${1:-all}" in
  setup)   do_setup ;;
  build)   do_build ;;
  # Flash and stop. Worth having on its own: rolling a firmware change into
  # the next capture means that if something misbehaves later you cannot tell
  # the firmware change from whatever else that session did differently.
  flash)   do_build; do_flash
           say "Done. Check it with:  python3 tools/live.py" ;;
  capture) LABEL="${2:-session}" do_capture ;;
  all)     do_build; do_flash; sleep 2; LABEL="${2:-session}" do_capture ;;
  *)       fail "usage: ./go.sh [setup|build|flash|capture <label>]" ;;
esac
