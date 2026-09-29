#!/usr/bin/env bash
# pi/boot_announce.sh -- real, minimal "tell me if the pi booted" script (founder real-time,
# 2026-09-29: "i need you to be able to tell me if the pi booted"). Runs once on boot (see
# edge-boot-announce.service below) on each Raspberry Pi, opens a plain TCP connection to the
# relay's operator port, authenticates as a device (EDGE_PI_TOKEN, not the operator token -- a Pi
# is a real, separate device class from an operator/Windows-cabinet client, see
# server/relay_main.c's own header comment on the three connection roles), and dispatches one
# REFLUX_ACTION_PI_BOOTED event.
#
# Deliberately bash + /dev/tcp, no curl/python/nc dependency: every real Raspberry Pi OS image
# ships bash; this needs nothing else installed to work on a freshly-flashed board. NDJSON, not
# HTTP -- matches every other connection in this stack (the relay's own operator port dropped HTTP
# entirely, see Makefile's own "relay" target header comment).
set -euo pipefail

: "${EDGE_RELAY_HOST:?set to the relay's real hostname/IP}"
: "${EDGE_RELAY_OPERATOR_PORT:?set to the relay's operator port, e.g. 8092}"
: "${EDGE_PI_TOKEN:?set to this Pi's shared device token}"
: "${EDGE_PI_ID:?set to this Pi's own small integer id, e.g. 1 or 2}"

exec 3<>"/dev/tcp/${EDGE_RELAY_HOST}/${EDGE_RELAY_OPERATOR_PORT}"

printf '{"type":"hello","token":"%s"}\n' "$EDGE_PI_TOKEN" >&3
read -r -t 5 hello_resp <&3 || { echo "boot_announce: no hello_ok from relay, giving up" >&2; exit 1; }
case "$hello_resp" in
  *'"role":"device"'*) ;;
  *) echo "boot_announce: relay did not grant device role -- got: $hello_resp" >&2; exit 1 ;;
esac

printf '{"type":"event","event":"boot","pi_id":%s}\n' "$EDGE_PI_ID" >&3
exec 3<&-
exec 3>&-
echo "boot_announce: reported boot for pi_id=$EDGE_PI_ID to ${EDGE_RELAY_HOST}:${EDGE_RELAY_OPERATOR_PORT}"
