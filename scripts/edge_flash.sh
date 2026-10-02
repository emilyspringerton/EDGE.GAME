#!/usr/bin/env bash
# scripts/edge_flash.sh <source> -- the "Claude updates the code, compiles it, flashes the Feather" loop
# (card #477), run on the SERVER box. <source> is a PARENA program (.prn, must define next-led-state --
# see compile_prn_feather.sh) or a prebuilt .hex. It (1) optionally pushes the .prn into the editor
# window (editor_set, unless EDGE_NO_EDITOR_SYNC=1), (2) compiles here, (3) sends flash_hex through the
# relay to the cabinet client, which does the 1200-baud touch + AVR109 flash + verify. Prints the
# client's flash_result line. Env: EDGE_RELAY_HOST (default 127.0.0.1), EDGE_RELAY_OPERATOR_PORT
# (8092), EDGE_OPERATOR_TOKEN (required), EDGE_SERVER_PIN (recommended), EDGE_CTL (build/edge_ctl),
# EDGE_BOOTLOADER_PORT (optional: flash that port directly, skipping the reset dance).
set -euo pipefail
SRC="${1:?usage: edge_flash.sh <file.prn|file.hex>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CTL="${EDGE_CTL:-$HERE/../build/edge_ctl}"
HOST="${EDGE_RELAY_HOST:-127.0.0.1}"; PORT="${EDGE_RELAY_OPERATOR_PORT:-8092}"
: "${EDGE_OPERATOR_TOKEN:?set EDGE_OPERATOR_TOKEN}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

case "$SRC" in
  *.prn)
    if [ "${EDGE_NO_EDITOR_SYNC:-0}" != "1" ]; then
      python3 - "$SRC" > "$TMP/editor.json" <<'PY'
import json, os, sys
print(json.dumps({"id": "ed1", "type": "editor_set", "payload": {}, "name": os.path.basename(sys.argv[1]), "text": open(sys.argv[1]).read()}, separators=(",", ":")))
PY
      "$CTL" "$HOST" "$PORT" "$EDGE_OPERATOR_TOKEN" --wait 3 --until-id ed1 "$(cat "$TMP/editor.json")" | sed -n 's/^\({"id":"ed1".*\)$/editor_set -> \1/p'
    fi
    "$HERE/compile_prn_feather.sh" "$SRC" "$TMP/out.hex"
    HEX="$TMP/out.hex" ;;
  *.hex) HEX="$SRC" ;;
  *) echo "edge_flash: unsupported source (want .prn or .hex)" >&2; exit 2 ;;
esac

BL="${EDGE_BOOTLOADER_PORT:-}" python3 - "$HEX" > "$TMP/flash.json" <<'PY'
import json, os, sys
p = {"hex": open(sys.argv[1]).read()}
if os.environ.get("BL"): p["bootloader_port"] = os.environ["BL"]
print(json.dumps({"id": "fl1", "type": "flash_hex", "payload": p}, separators=(",", ":")))
PY
"$CTL" "$HOST" "$PORT" "$EDGE_OPERATOR_TOKEN" --wait 75 --until-id fl1 "$(cat "$TMP/flash.json")" | grep '"id":"fl1"' || { echo "edge_flash: no flash_result (cabinet offline?)" >&2; exit 1; }
