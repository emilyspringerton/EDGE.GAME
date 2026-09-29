# EDGE.GAME

The premiere Windows arcade-cabinet edge environment: a Windows-native binary the founder runs on
the real cabinet hardware, connecting outbound to a server so commands and debug data can flow
both ways — flashing physical lights on the cabinet from an online game, and giving real
USB-serial/hardware debugging access without any remote-control software on the Windows machine
itself.

**Status: one real piece built and tested, the rest is NORTHSTAR.** See
[`NORTHSTAR.md`](NORTHSTAR.md) for the full picture: the founder's own resolved access model (no
Claude Code Remote Control — a binary is built here, downloaded, and run there), the real physical
hardware topology (Windows PC + Adafruit Feather + Raspberry Pi + Arduino Nano, wired through a
logic-level shifter), a capability audit of what already exists to reuse (PARENA's real, working
AVR upload pipeline for the Nano's firmware; SHANKPIT's real controller-input code; DEADWEIGHT_2's
real Windows cross-compile recipe), and a phased build plan.

`PARENA/stdlib/edge_game/traffic_router.prn` — the cabinet's hardware fan-out routing decision
logic — is real, built, and tested (`make test-traffic-router` in `PARENA/`, 4/4 assertions,
strict `-Wall -Wextra -pedantic -Werror` clean). Corrected from an initial pasted-tutorial draft
that had several real PARENA syntax mistakes and would not have compiled as given — see
NORTHSTAR.md's own "The traffic_router module" section for exactly what was wrong and why.

This repo is provisionally named and local-only (no GitHub upstream yet).
