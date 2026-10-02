#!/usr/bin/env bash
# pi/serial_responder.sh -- answers over the Pi's UART so the Feather bridge (feather/pi_bridge) and
# the EDGE.GAME serial capture can prove the Pi is alive end to end (card #474). Line protocol on
# /dev/serial0 (115200 8N1):
#   ping   -> "pong <hostname> up=<seconds> load=<1m>"
#   uname  -> "uname <kernel> <arch>"
# Anything else is echoed as "echo: <line>". Needs the Pi's serial console DISABLED and the UART
# enabled (raspi-config: Interface Options > Serial Port: login shell = No, hardware = Yes), otherwise
# the kernel console and this script fight over the port. Run via pi/edge-serial-responder.service.
set -euo pipefail
DEV="${EDGE_SERIAL_DEV:-/dev/serial0}"
stty -F "$DEV" 115200 raw -echo
exec 3<>"$DEV"
while IFS= read -r line <&3; do
  line="${line%$'\r'}"
  case "$line" in
    ping)  printf 'pong %s up=%s load=%s\n' "$(hostname)" "$(cut -d. -f1 /proc/uptime)" "$(cut -d' ' -f1 /proc/loadavg)" >&3 ;;
    uname) printf 'uname %s %s\n' "$(uname -r)" "$(uname -m)" >&3 ;;
    "")    ;;
    *)     printf 'echo: %s\n' "$line" >&3 ;;
  esac
done
