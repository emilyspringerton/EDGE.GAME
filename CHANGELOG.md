# Changelog

## 2026-10-02
- feather/pi_bridge firmware (USB<->UART bridge, !ping/!pi?) compiled with arduino-cli + Pi serial responder; not hardware-tested (#474) (sess-20260923-1030-4a526255)
- serial/terminal capture: client serial_open/write/close (auto-find Feather), relay log buffer + log_since/log_subscribe so Claude reads device output (#492,#474) (sess-20260923-1030-4a526255)
- encrypted transport on every socket: ML-KEM-768 + LZ4 + XChaCha20-Poly1305 (PARENA secure_channel), edge_ctl CLI, pinned/TOFU fingerprint, handshake reaper, Windows build (#491) (sess-20260923-1030-4a526255)

- client: USB/COM probe (edge_client probe / usb_probe command) finds the Feather 32u4 by VID/PID on Windows; make test-usb-probe (#493) (sess-20260923-1030-4a526255)


## 2026-09-29

- feat(ci): `.github/workflows/ci.yml` — auto-release pipeline for the game client, ported from
  `DEADWEIGHT_2`'s own proven recipe (founder real-time: "get CICD auto releases set up for the
  game client ensure we have windows zip files with sdl and play.bat and the exe"). `version` job
  auto-bumps a MINOR tag on green `main` pushes; `build` job builds + tests `make client`/`make
  relay`/`make test-e2e` on Linux (checking out a sibling PARENA + a no-sudo LLVM toolchain); new
  `windows` job cross-compiles `client/edge_client.c` via a new `client-windows` Makefile target
  (`x86_64-w64-mingw32-gcc -DPARENA_NO_GRAPHICS`) and bundles a flat `edge_client_windows.zip`
  (exe + `PLAY.bat`); `release` job tags and publishes both plus `edge_relay`/`EDGE_GAME_
  CONSTRUCT.txt`. New `scripts/generate_construct.sh` (Principle 21, git-based, verified
  byte-for-byte deterministic locally). Real, honest scope decision (via AskUserQuestion): the
  client has no SDL2 dependency yet (Phase 1 headless test client, SDL2 UI is Phase 2+), so the
  Windows zip has no `SDL2.dll` — shipping one would bundle a library nothing calls.
  `-DPARENA_NO_GRAPHICS` is a narrow CI-only mingw flag (skips the shared runtime header's
  unconditional SDL2 include for this one cross-compile target); native `client` is unchanged.
  Live-verified locally before committing: the mingw cross-compile is `-Wall -Wextra -pedantic
  -Werror` clean and produces a real `PE32+ executable ... for MS Windows`. Real, named blocker:
  this repo has no GitHub remote yet and the available `GITHUB_TOKEN` can't create one (HTTP 403,
  read-only scope) — the workflow is written and locally verified but has not run on GitHub
  Actions; creating the upstream is the founder's own step. See `NORTHSTAR.md`'s updated Phase 7.
