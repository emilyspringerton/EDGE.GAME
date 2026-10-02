# EDGE.GAME

The premiere Windows arcade-cabinet edge environment: a Windows-native binary the founder runs on
the real cabinet hardware, connecting outbound to a server so commands and debug data can flow
both ways — flashing physical lights on the cabinet from an online game, and giving real
USB-serial/hardware debugging access without any remote-control software on the Windows machine
itself.

**Status: several real pieces built and tested, the rest is NORTHSTAR.** See
[`NORTHSTAR.md`](NORTHSTAR.md) for the full picture: the founder's own resolved access model (no
Claude Code Remote Control — a binary is built here, downloaded, and run there), the real physical
hardware topology (Windows PC + Adafruit Feather + Raspberry Pi + Arduino Nano, wired through a
logic-level shifter), a capability audit of what already exists to reuse (PARENA's real, working
AVR upload pipeline for the Nano's firmware; SHANKPIT's real controller-input code; DEADWEIGHT_2's
real Windows cross-compile recipe), and a phased build plan.

**The relay server (`build/edge_relay`) is a real, native PARENA+C binary — no Node.js anywhere in
this stack.** It links genuine object code compiled from `PARENA/stdlib/reflux/reflux.prn` (the
event pub/sub log) and `PARENA/stdlib/net/tcp_llvm.prn` (raw socket lifecycle) through PARENA's own
LLVM emission target, plus a thin hand-written C host for the parts that target's v0 can't express
(the select() event loop, NDJSON line framing). Both the cabinet connection and the operator API
are plain TCP + NDJSON. A live events channel (buffered + streaming, backed by the real REFLUX
ring-buffer) lets a Raspberry Pi announce its own boot over the network (`pi/boot_announce.sh`).
`make test-e2e` (12/12 checks, no hardware needed) is the real, reproducible proof.

`PARENA/stdlib/edge_game/traffic_router.prn` — the cabinet's hardware fan-out routing decision
logic — is real, built, and tested (`make test-traffic-router` in `PARENA/`, 4/4 assertions,
strict `-Wall -Wextra -pedantic -Werror` clean). Corrected from an initial pasted-tutorial draft
that had several real PARENA syntax mistakes and would not have compiled as given — see
NORTHSTAR.md's own "The traffic_router module" section for exactly what was wrong and why.

This repo is provisionally named and local-only (no GitHub upstream yet).

**CI/auto-release is written and locally verified, not yet running for real.** `.github/workflows/
ci.yml` builds + tests the client and relay on Linux (`make client`, `make relay`, `make
test-e2e`), cross-compiles `edge_client.c` for Windows via mingw (`make client-windows` —
real-verified: a clean `-Wall -Wextra -pedantic -Werror` PE32+ binary, since that file was already
written portable against `_WIN32`/winsock2 but this was the first time that path was actually
exercised), bundles a flat `edge_client_windows.zip` (exe + `PLAY.bat`), and auto-tags/releases on
every green `main` push — same pattern `DEADWEIGHT_2/.github/workflows/ci.yml` already proves live.
**USB/COM probe (card #493):** `edge_client probe` (or the relay's `usb_probe` operator command)
finds the Feather 32u4 by USB VID/PID, reports its COM port and whether it is running a sketch
(`239A:800C`) or in the Caterina bootloader (`239A:000C`), also classifies Nano (CH340/FTDI) and Pi
gadget devices, and gives a plain-language hint when nothing is found (e.g. charge-only cable).
Windows backend reads the registry (`advapi32` only); classification, JSON and the POSIX backend are
unit-tested (`make test-usb-probe`, 16 checks) and the command round-trips in `make test-e2e`.
**Untested:** the Windows registry backend has been compiled by mingw but never run on a real
Windows machine yet -- the first `edge_client.exe probe` on the founder's PC is the real test.

Honest scope note: `edge_client.c` is still Phase 1's headless NDJSON test client, not the real
SDL2 game window (that's still Phase 2+, see `NORTHSTAR.md`) — so the Windows zip has no
`SDL2.dll` yet, because nothing links it. Because this repo has no GitHub remote yet, the workflow
cannot actually run until one exists — created and pushed, not run in CI, until then.
