# Compile-check stubs

`arduino-cli` in this folder is a macOS x86_64 binary and cannot run inside the
Linux VM the assistant reaches this folder through. These headers are just
enough of the Arduino / SPI / RF24 API — matching the real signatures — to let
`g++ -fsyntax-only` catch syntax errors, typos, wrong argument counts and
missing declarations before the sketch is flashed.

They do **not** replace a real build. `./go.sh build` on the Mac is still the
authority. Run this first, though: it turns a two-minute flash cycle into a
one-second check.

    ./tools/syntax_check.sh
