# EDGE.GAME

Mostly a scoping repo right now, a growing set of real pieces built. Read
[`NORTHSTAR.md`](NORTHSTAR.md) before doing any work here — it has the founder's own resolved
access model (no Remote Control), the real physical hardware topology, a capability audit of what
to reuse vs. what's genuinely new, and the phased plan. Open questions are named at its end (which
Adafruit Feather chip is now confirmed — 32u4 — transport was resolved 2026-09-29, see below — but
what the Pi(s) actually run, the "kubernetes" framing, and the payment processor are still open) —
check with the founder before guessing past them.

**The relay is a native PARENA+C binary, not Node.js (2026-09-29 rewrite).** Founder real-time,
corrected twice: "write it in PARENA in what world are we using node for any part of this stack?"
→ "parena really needs to just emit the fucking llvm code for the server... we eat that tech
debt." `build/edge_relay` links real object code compiled from `stdlib/reflux/reflux.prn` and
`stdlib/net/tcp_llvm.prn` through PARENA's new `src/emit_llvm.c` `#target {:llvm ...}` FFI hatch
(a genuinely new compiler capability — that target had zero FFI mechanism before this), lowered to
x86_64 via `llc`, linked against a thin hand-written C host (`server/relay_main.c`) for the
select()/NDJSON plumbing PARENA's scalar-only LLVM v0 can't express. Both the cabinet port and the
operator port speak plain TCP + NDJSON now — the old HTTP operator API is gone. A new events
channel (buffered + live-streaming, backed by the real REFLUX ring-buffer log) answers "tell me if
the pi booted" via `pi/boot_announce.sh`. See NORTHSTAR.md's own "Phase 1.5" section for the full
detail. `make test-e2e` (12/12 checks) is the real, reproducible, no-hardware proof.

**Two parallel platform arms, not one bigger plan**: a Windows dev/debug arm (Phases 0-7) and an
Android production/execution arm (Phases A1-A4, added 2026-09-29) — the tablet is the kiosk's real
"brain" in the field; Windows is where I help debug hardware/firmware. See NORTHSTAR.md's own
"Production topology" section before touching anything Android-shaped. `MJOLNIR` is the real
Android project-skeleton precedent to template from — but has no Android SDK/`gradlew` available
in this sandbox, so Android work here is written, not built/run, until the founder's own machine
or a real CI Android runner is in the loop. Payment processing (Phase A3) is explicitly gated on
its own founder go-ahead — do not write payment code without that.

Follow the root `/home/fatbaby/CLAUDE.md`'s "The Emily Way" protocol (backlog-first, Apple before
mark-done, changelog, commit format) same as every other repo in the monorepo.

## Related repos

- `PARENA` — `stdlib/edge_game/traffic_router.prn` is this repo's own real routing decision logic,
  already built and tested there (`make test-traffic-router`). `docs/AVR_ARDUINO_NORTHSTAR.md` +
  the `avr-blink-hex`/`avr-blink-upload` Makefile targets are the real, existing AVR compile/upload
  pipeline the Arduino Nano's firmware should extend, not reinvent.
- `SHANKPIT` — `apps/lobby/src/main.c`'s real `SDL_GameController` input/button-grid code is the
  real precedent for this repo's own cabinet button UI ("rip the buttons out of shankpit").
- `DEADWEIGHT_2` — `.github/workflows/ci.yml`'s `windows` job is the real, proven mingw+SDL2
  Windows cross-compile recipe this repo's own CI should copy, not re-derive.
- `IDUNA` — `docs/NORTHSTAR_INVENTORY.md` names the founder's own real hardware (Raspberry Pis, an
  Adafruit Feather) this repo actually targets.

## Core Deps Are PARENA-First (standing, monorepo-wide)

Founder real-time, 2026-10-01: *"always implement core deps in PARENA — when core deps are missing
always implement the core deps in PARENA first."*

- **When a core dependency is missing** (a codec, a protocol client, an inference engine, a
  parser, a data structure — anything this repo's own functionality stands on), implement it in
  PARENA (`PARENA/stdlib/...`) **first**, before building the feature that needs it. Deps first,
  feature second.
- **If PARENA itself can't express the dep yet**, that gap is the real first task: fix or extend
  PARENA (compiler, emitter, or stdlib), with tests, then build the dep on top. Don't route around it.
- **Third-party tools/binaries are stopgaps, not the answer.** Shelling out to or FFI-binding an
  existing tool is allowed only to unblock a demo, and must be labeled as a stopgap in the code and
  in `EMILY/BACKLOG.md` with a PARENA replacement item. (Example: Piper via subprocess for
  MODE_TYLER TTS, 2026-10-01 — stopgap; the PARENA-native synthesis stack is the real work.)
- **Not a license to reimplement the OS.** Core deps = what the product's own behavior depends on.
  Compilers, kernels, system libraries and the like stay as-is; a repo's own CLAUDE.md may record a
  considered, specific exception (same standard as the LZ4 compression convention).
