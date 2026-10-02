#!/usr/bin/env bash
# scripts/compile_prn_feather.sh <file.prn> <out.hex> -- compile a PARENA program for the Feather 32u4
# on the SERVER (card #477: the server compiles, the cabinet client only flashes). Same recipe as
# PARENA's `make avr-feather-blink-hex`, minus its fixed source path: `parena build` turns the .prn
# into C, avr-gcc links it into PARENA's Feather host (examples/avr/blink_main_feather.c, which calls
# the program's `next-led-state : Bool -> Bool` once a tick -- so the .prn must export that function;
# richer hosts are future work), objcopy writes Intel HEX. Needs ../PARENA built and its AVR toolchain
# (PARENA_ROOT / AVR_TOOLCHAIN_ROOT override the defaults).
set -euo pipefail
SRC="${1:?usage: compile_prn_feather.sh <file.prn> <out.hex>}"
OUT="${2:?usage: compile_prn_feather.sh <file.prn> <out.hex>}"
PARENA_ROOT="${PARENA_ROOT:-$(cd "$(dirname "$0")/../../PARENA" && pwd)}"
AVR="${AVR_TOOLCHAIN_ROOT:-$HOME/.local/opt/avr-toolchain}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
SRC_ABS="$(cd "$(dirname "$SRC")" && pwd)/$(basename "$SRC")"
OUT_ABS="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"

( cd "$PARENA_ROOT" && ./parena build "$SRC_ABS" -o "$TMP/blink_gen.c" ) >&2
grep -q "next_led_state" "$TMP/blink_gen.c" || { echo "compile: the program must define (defn next-led-state [(current : Bool)] : Bool ...)" >&2; exit 3; }
cp "$PARENA_ROOT/examples/avr/blink_main_feather.c" "$PARENA_ROOT/examples/avr/parena_runtime.h" "$TMP/"
LD_LIBRARY_PATH="$AVR/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}" \
  "$AVR/usr/bin/avr-gcc" -Wall -Os -DF_CPU=8000000UL -mmcu=atmega32u4 "$TMP/blink_main_feather.c" -o "$TMP/out.elf" >&2
LD_LIBRARY_PATH="$AVR/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}" \
  "$AVR/usr/bin/avr-objcopy" -O ihex -R .eeprom "$TMP/out.elf" "$OUT_ABS"
echo "compiled $SRC -> $OUT ($(wc -c < "$OUT_ABS") bytes of hex)" >&2
