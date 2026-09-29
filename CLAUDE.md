# EDGE.GAME

Mostly a scoping repo right now, one real piece built. Read [`NORTHSTAR.md`](NORTHSTAR.md) before
doing any work here — it has the founder's own resolved access model (no Remote Control), the real
physical hardware topology, a capability audit of what to reuse vs. what's genuinely new, and the
phased plan. Six open questions are named at its end (which Adafruit Feather chip is now
confirmed — 32u4 — but transport choice, what the Pi(s) actually run, the "kubernetes" framing,
and the payment processor are still open) — check with the founder before guessing past them.

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
