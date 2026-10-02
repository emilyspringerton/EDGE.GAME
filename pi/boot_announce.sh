#!/usr/bin/env bash
# pi/boot_announce.sh -- "tell me if the pi booted" (founder real-time, 2026-09-29). Runs once on boot
# (edge-boot-announce.service) on each Raspberry Pi and pushes one REFLUX_ACTION_PI_BOOTED event.
#
# Since card #491 the relay speaks ONLY the encrypted transport (ML-KEM-768 + LZ4 + XChaCha20-Poly1305),
# so this no longer uses bash /dev/tcp: it drives `edge_ctl` (build it on the Pi with `make edge-ctl`,
# or copy a prebuilt aarch64 binary; set EDGE_CTL to its path). Server authentication: EDGE_SERVER_PIN
# (the 64-hex fingerprint the relay prints at startup) or, if unset, trust-on-first-use recorded next
# to the script in edge_known_servers.txt.
set -euo pipefail

: "${EDGE_RELAY_HOST:?set to the relay's real hostname/IP (dotted IPv4)}"
: "${EDGE_RELAY_OPERATOR_PORT:?set to the relay's operator port, e.g. 8092}"
: "${EDGE_PI_TOKEN:?set to this Pi's shared device token}"
: "${EDGE_PI_ID:?set to this Pi's own small integer id, e.g. 1 or 2}"
EDGE_CTL="${EDGE_CTL:-$(dirname "$0")/edge_ctl}"
[ -x "$EDGE_CTL" ] || { echo "boot_announce: edge_ctl not found at $EDGE_CTL (make edge-ctl, set EDGE_CTL)" >&2; exit 1; }

cd "$(dirname "$0")"
"$EDGE_CTL" "$EDGE_RELAY_HOST" "$EDGE_RELAY_OPERATOR_PORT" "$EDGE_PI_TOKEN" --wait 0.5 \
  "{\"type\":\"event\",\"event\":\"boot\",\"pi_id\":${EDGE_PI_ID}}" >/dev/null \
  || { echo "boot_announce: relay refused or unreachable (edge_ctl exit $?)" >&2; exit 1; }
echo "boot_announce: reported boot for pi_id=$EDGE_PI_ID to ${EDGE_RELAY_HOST}:${EDGE_RELAY_OPERATOR_PORT}"
