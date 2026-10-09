# Changelog

## 2026-10-09 (cont. 2)
- client + relay: batteries-included browser login (EDGE-599-FOLLOWUP-2). Founder real-time: "the
  oauth isnt in the client yet i still get this [usage: edge_client.exe <host> <port> <token>] on
  v12." Run `edge_client.exe <host> <port>` with no token at all and the client now opens the
  founder's own browser to IDUNA's existing SSO login page, captures the real JWT back via the
  classic desktop-OAuth loopback pattern (`obtain_token_via_browser`: a tiny local HTTP listener
  on 127.0.0.1, since the SSO page hands the token back as a URL FRAGMENT a browser never sends to
  a server — the served callback page's own JS reads `location.hash` and POSTs it back
  same-origin to `/complete`), and uses that real token for the cabinet hello.
  `relay_main.c`'s `jwt_authorized` (previously operator-only) now also gates the CABINET hello —
  one real IDUNA identity covers both `edge_client.exe` (the founder's machine) and `edge_ctl`
  (the operator/me), replacing two separate static shared secrets with one real permission check.
  Live-verified end to end in this sandbox (simulated the browser's own two requests with curl
  against a real running client): correct login-URL construction, correct token capture from the
  simulated redirect, correct use of the captured token in the relay hello (rejected by the old,
  still-deployed pre-JWT relay binary exactly as expected — not a bug, that binary predates this
  change and `make relay` can't be rebuilt end to end in this sandbox, same `llc`-missing gap as
  before). `client-windows` Makefile target now links `-lshell32` for `ShellExecuteA`.
  Native client build (`-DPARENA_NO_GRAPHICS`, warning-clean) succeeded; mingw cross-compile
  relies on CI's own `client-windows` job as before.

## 2026-10-09 (cont.)
- relay: real ES256 JWT verification for operator auth (EDGE-599-FOLLOWUP-1). New
  `common/jwt_verify.{h,c}` (OpenSSL EC/ECDSA, `-lcrypto`) parses a pinned local JWKS file
  (`EDGE_IDUNA_JWKS_FILE`, fetched once via `curl https://iam.okemily.com/.well-known/jwks.json`),
  verifies a presented operator token's ES256 signature, and checks its `permissions` claim for
  `edge.game.operator` -- the permission IDUNA's `PlayerEmailAuthHandler` now grants (see IDUNA's
  own 2026-10-09 entry). Falls back to the existing static `EDGE_OPERATOR_TOKEN` unchanged if the
  presented token doesn't verify or `EDGE_IDUNA_JWKS_FILE` is unset — fully backward compatible,
  no existing usage breaks. `edge_ctl` needed zero changes: it already accepts an arbitrary token
  string, so a real IDUNA JWT works as a drop-in replacement for the shared secret.
  Live-verified against a REAL IDUNA-issued JWT (registered a throwaway test account against the
  live `wotan.okemily.com` API): correct signature verification, correct `permissions` array
  extraction (empty, as expected for an unlisted email), correctly REJECTS a forged token (payload
  tampered to claim `edge.game.operator`, old signature kept) and a garbage string. Named, real
  limitations (see `jwt_verify.h`'s own header comment): no `exp` check, one pinned key (no JWKS
  rotation/multi-`kid` support). `make relay` itself could not be rebuilt end to end in this
  sandbox (missing `llc`, same pre-existing gap as the last entry below) — `jwt_verify.c` and the
  modified `relay_main.c` were each compiled standalone and warning-clean
  (`-Wall -Wextra -pedantic -Werror`, `-Wno-deprecated-declarations` scoped to just the OpenSSL
  EC_KEY calls in `jwt_verify.c`); relying on CI's `relay` build job to confirm the full link.

## 2026-10-09
- client: new `exec` command (card #599, HRIP work) — relay forwards an arbitrary `cmd` string,
  the client runs it (`cmd /C "( ... ) 2>&1"` on Windows, `( ... ) 2>&1` on POSIX, both wrapped in
  a subshell so stderr from every statement in a `;`/`&&` chain is captured, not just the last
  one — found live, fixed before shipping), captures up to 48000 bytes of combined stdout+stderr
  (truncated+flagged past that, never silently dropped), and replies with exit code + output over
  the same existing encrypted (ML-KEM-768 + XChaCha20-Poly1305) transport every other command
  already uses. Deliberately unlike `editor_set`/`editor_get`: no path/extension allowlist — this
  is full generic shell-exec, not a sandboxed command. Founder was presented a narrower,
  HRIP-specific-commands alternative first and explicitly chose full exec instead (logged in
  `EMILY/BACKLOG.md`). No timeout exists yet — a hung command blocks the client's single-threaded
  loop until it exits or the connection is killed and the client restarted; named, not hidden.
  Live-verified end to end (real relay + real client over loopback, `edge_ctl`): a multi-statement
  command with a failing first statement, correct combined stdout+stderr, correct nonzero exit
  code. Native build is `-Wall -Wextra -pedantic -Werror` clean; the mingw cross-compile could not
  be re-verified in this sandbox (mingw gcc unavailable here, same gap the 2026-09-29 entry below
  already names) — relies on CI's own `client-windows` job to confirm it on the next push.

## 2026-10-02
- relay: per-host cabinet connections (windows/android), Feather link reporting, host-aware routing + hosts query (card #478) (sess-20260923-1030-4a526255)
- sec_transport: frame-counter exhaustion refuses/drops (re-handshake), socketpair unit test under ASan+UBSan, CI unit-test step, README for the Claude loop (sess-20260923-1030-4a526255)
- editor_set/editor_get commands, scripts/edge_flash.sh + compile_prn_feather.sh: Claude updates the editor file, compiles on the server, flashes the Feather via the relay (#475,#477); simulator-verified (sess-20260923-1030-4a526255)
- flash_hex: client-side AVR109/Caterina flasher + Intel HEX parser; server compiles, client flashes; simulator-tested, hardware-untested (#477,#474) (sess-20260923-1030-4a526255)
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
