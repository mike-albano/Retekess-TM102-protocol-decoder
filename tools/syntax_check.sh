#!/usr/bin/env bash
# Syntax-check the sketch without a toolchain. See tools/stubs/README.md.
set -e
cd "$(dirname "$0")/.."
SRC=firmware/tm102_sniffer/tm102_sniffer.ino
TMP=$(mktemp -d)
{ echo '#include "Arduino.h"'; cat "$SRC"; } > "$TMP/sketch.cpp"
g++ -fsyntax-only -std=c++17 -I tools/stubs "$TMP/sketch.cpp" && echo "syntax OK: $SRC"
rm -rf "$TMP"
