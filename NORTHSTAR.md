# EDGE.GAME — NORTHSTAR

## The real ask (founder real-time, 2026-09-29, three messages)

1. "we are building an arcade cabinet the idea is its an online game and the server can flash
   lights on the cabinet we need help to develop and debug so you need a windows native interface
   with the real hardware attached therefore we will build EDGE.GAME the premiere windows arcade
   cabinet edge environment we need full control of my windows computer so you can help debug the
   parena USB upload code etc help with the serial debugging etc there will be a server obviously
   that there will be an API to the api needs to be able to give arbitrary write commands to the
   native windows platform also it will need to be 2 way communication obviosly so we can actually
   debug"
2. Asked how to actually get access to the Windows machine (Claude Code Remote Control was one
   option). Founder's answer, verbatim: "no remote control we make the binary here package it as a
   game for the interface - rip the buttons out of shankpit or whatever / write a parena i dont
   care / i run the game and then the game gives you access / the game connects to the server you
   run commands through the server - if you need a new tool we need to ship a new binary and i
   will download install and run it." This is the real, resolved access model — see "Access model"
   below.
3. Pasted a Gemini-style tutorial (`Use code with caution` boilerplate — an AI chat transcript,
   same real pattern MIXFORGE's `legacy.txt`/LO's captured spec already established, reviewed
   critically here rather than transcribed) naming the real physical topology: a Windows PC, a
   Raspberry Pi, an Arduino Nano, and an Adafruit Feather, wired through a logic-level shifter, and
   a `traffic_router` PARENA module sketch for routing messages between them. Its actual `.prn`
   code did not compile as written — see "The traffic_router module" below for what was wrong and
   what's real.
4. Confirmed the Feather is a **32u4** (ATmega32u4, AVR family) and named a third real board ("a
   regular arduino... around here somewhere too"), plus a real, concrete gotcha: "the different
   usb speeds needs to be accounted for" — see "Real physical topology" and the AVR profile work
   below.
5. A major scope expansion in three follow-up messages: (a) "integrate the PARENA EDITOR (check
   EDITOR.GAME and PARENA examples)... shankpit style emily OS affordances... IDUNA color
   pallette... solarized... but the buttons need to be high contrast... use the shankpit menu
   system... i need a basic IDE to start with - a code thingy a compile an upload - its already
   there but it doesnt work - thats the first task... make the full pipeline for edge.game work...
   live take control of my coding window like we are a pair programming partner... assume it needs
   to work with a pair programming setup but you will just use the server as an affordance"; (b)
   "it needs git affordances like the source code will be on the windows computer but the compiler
   is actually built into the game"; (c) "think of it like vs code but its a game." See "EDGE.GAME
   as an IDE" below for the full, consolidated design this resolves into.

## Access model (resolved, not the default assumption)

**No Claude Code Remote Control on the founder's Windows machine.** The founder explicitly ruled
this out. Instead:

- The binary is **built here** (in this Linux sandbox/monorepo) and the founder downloads,
  installs, and runs it on the real Windows machine with the cabinet hardware attached.
- That running binary is itself the access point: it opens an **outbound** connection to a server
  (client-initiated, so no port-forwarding/NAT setup needed on the founder's network).
- Debugging happens **through the server**: an operator (Claude, via curl/a CLI) sends a command
  down to the connected client and reads back whatever it reports — a real two-way channel, but
  mediated by a server this environment controls, never literal remote control of the Windows
  desktop itself.
- **Iteration loop is real but slow, on purpose**: any new debug capability has to be built into
  the binary and shipped as a new download — "if you need a new tool we need to ship a new binary
  and i will download install and run it." No live code-push, no auto-update assumed.

## Real physical topology (from the founder's own pasted design)

```
        USB                    Serial1 (3.3V, direct)         GPIO14/15 UART
Windows PC  <----------->  Adafruit Feather  <---------------------------->  Raspberry Pi
(EDGE.GAME                                    \
 binary)                                       \ SoftwareSerial pins 10/11 (5V, via level shifter)
                                                 \-------------------------->  Arduino Nano
```

- **Shared ground**: GND of the Feather, Pi, and Nano all tie into the level shifter's ground rail.
- **3.3V direct channel**: Feather `Serial1` (its real hardware UART) straight to the Pi's own UART
  pins (GPIO 14/15) — both sides are native 3.3V logic, no shifting needed.
- **5V shunted channel**: Feather `SoftwareSerial` digital pins 10/11 through the level shifter's
  LV side, stepped up to HV, into the Nano's RX/TX — the Nano's ATmega328P runs at 5V logic, the
  Feather doesn't, hence the shifter.
- **Confirmed: the Feather is a 32u4** (ATmega32u4, AVR family) — the existing `avr-gcc`/`avrdude`
  pipeline applies directly, no new ARM/Xtensa toolchain needed. A real, third board is also in
  play: "a regular arduino... around here somewhere too" (assumed Uno pending confirmation — same
  `atmega328p`/115200/`arduino` profile as a new-bootloader Nano, so no new toolchain work either
  way).
- **Real, confirmed gotcha, not a single baud-rate tweak**: these three AVR boards do NOT share one
  avrdude profile. Checked directly against avrdude's own `-c '?'`/`-p '?'` listings and a real
  `avr-gcc -mmcu=atmega32u4` compile (not assumed) — see `PARENA/docs/AVR_ARDUINO_NORTHSTAR.md`'s
  own "Real, named per-board profiles" section for the full detail:
  - **Uno / Nano (new bootloader)**: `atmega328p`, `arduino` (STK500v1), 115200 baud.
  - **Nano (OLD bootloader)**: same chip, 57600 baud — a real, common gotcha (flashing at 115200
    against an old-bootloader Nano just times out, easy to mistake for a wiring problem).
  - **Feather 32u4 (Caterina bootloader)**: `atmega32u4`, `avr109`, 57600 baud, 8MHz clock (half
    the Uno/Nano's 16MHz — matters for AVR-libc delay timing), and genuinely REQUIRES a real
    "1200-baud touch" reset first (open the port at 1200 baud, close it — the same thing the
    Arduino IDE does invisibly; there's no avrdude flag for this).
  All three now have real, named, tested Makefile targets in `PARENA/Makefile`
  (`avr-blink-upload`/`avr-nano-old-bootloader-upload`/`avr-feather-blink-upload`) plus a real
  touch-reset helper (`PARENA/tools/avr_touch_reset.py`, plain termios, verified against a real
  pty). A real, distinct bug was caught before shipping: naively reusing the Uno/Nano's own
  `blink_main.c` for the Feather would have toggled the WRONG pin (PB5 vs. the 32u4's real PC7,
  confirmed via `avr-objdump` disassembly) at the wrong speed — fixed with a dedicated
  `examples/avr/blink_main_feather.c`.

## Real, existing capability audit — checked directly, not assumed

- **The Nano is directly covered today.** `PARENA/docs/AVR_ARDUINO_NORTHSTAR.md` + the real,
  live-verified `make avr-blink-hex`/`avr-blink-upload` Makefile targets are exactly "the PARENA
  USB upload code" the founder's very first message named wanting help debugging — this already
  exists: a real, no-sudo `avr-gcc`/`avrdude` toolchain (extracted via `apt-get download` +
  `dpkg -x`, no root needed), a real AVR-safe runtime stub (`examples/avr/parena_runtime.h`,
  deliberately NOT the syscall-heavy shared `runtime/parena_runtime.h`), and a working
  compile→hex→flash pipeline already proven against an `atmega328p` target — the Arduino Nano's
  own real chip. Nothing new needs inventing for the Nano's firmware side; it's the same recipe as
  the existing `blink.prn` demo, extended with real cabinet-light logic.
- **The Pi is a Linux target, already fully supported.** `stdlib/hw/serial.prn` is real, shipped,
  POSIX-based (termios/`/dev/ttyUSB0`-style paths) — directly usable ON the Pi itself (a real Linux
  box), no Windows-COM-port gap applies there at all (that gap is Windows-specific, see below).
- **The Windows side has the one real, genuinely new gap.** `stdlib/hw/serial.prn` is POSIX-only —
  no Win32 COM-port (`CreateFileW`/`SetCommState`/`SetCommTimeouts`) path exists anywhere in this
  stdlib or runtime. EDGE.GAME's own Windows client routes around this the same way every other
  PARENA-mod-island repo in this monorepo does (REDGARDEN/ECOWAR/PAPERCRAFT/DEADWEIGHT): a
  hand-written C host owns the raw Win32 serial I/O, PARENA owns the decision logic that doesn't
  need direct syscalls.
- **SHANKPIT's lobby app has real, working `SDL_GameController` input** (`apps/lobby/src/main.c`:
  `SDL_Init(SDL_INIT_GAMECONTROLLER)`, axis/button reads, a button-grid UI) — the real precedent
  for "rip the buttons out of shankpit," confirmed to actually exist and work, not assumed.
- **DEADWEIGHT_2's `.github/workflows/ci.yml` windows job** is a real, proven mingw + SDL2-mingw
  cross-compile recipe (already produces a working `dw2_client.exe` + `SDL2.dll` + `PLAY.bat` flat
  zip) — directly reusable as EDGE.GAME's own Windows build pipeline.

## The traffic_router module (real, built, tested — Phase 0, done)

`PARENA/stdlib/edge_game/traffic_router.prn` — pure routing DECISION logic only (which board a
message should go to: `RouteTarget` = `ToWindows`/`ToPi`/`ToNano`/`ToPiAndNano`), no I/O of its
own. Deliberately kept pure and Region-carrying (`Message.payload : String @ Region`) so it's a
Linux/Windows-host-side decision helper, NOT something meant to cross-compile onto the AVR targets
themselves (the AVR runtime doesn't support Arena/Region machinery at all — see the AVR doc above;
the Nano/Feather firmware side stays plain scalar logic, same shape as the existing `blink.prn`).

The founder's own pasted tutorial's `.prn` sketch did not compile as written. Real, concrete
mistakes found and fixed before anything shipped: a comma-separated flat parameter list (real
syntax needs one paren pair per parameter, no commas — confirmed against `stdlib/awk.prn`/`io.prn`
directly), `Void` as a return type (`Unit` is the real no-value type — `Void` appears zero times
anywhere in this stdlib), `(get msg source)` for struct field access (the real accessor is
`(get-field msg :source)` — `get` is the unrelated Map/BSTree lookup function), `==` for equality
(the real operator is bare `=` — 1245 real uses vs. 1 for `==`), and `io/write-string` treated as a
console-print call (its real signature writes to an open `FileHandle` with an explicit
`dest : Arena @ Region` — real file I/O, not console output; this module does no I/O of its own on
purpose, see above). One more found only by actually running the compiler: a zero-field `defenum`
variant constructs as a bare symbol (`ToWindows`), never a zero-arg call form (`(ToWindows)`) —
`src/emit.c`'s call-form dispatch has no `field_count == 0` branch, so the call form silently falls
through to generic function-call emission and fails late with a confusing C error instead of a
clear PARENA one. Named as a real, minor compiler DX gap, not fixed (out of scope here — PARENA
still correctly refuses the bad program, just unclearly).

4/4 real assertions pass (`make test-traffic-router` in `PARENA/`), strict-clean
(`-Wall -Wextra -pedantic -Werror`) on the emitted C. See `PARENA/STDLIB.md`'s own
"edge_game/traffic_router" section for the full detail.

## Architecture

- **`client/`** — the actual Windows-native binary the founder runs (EDGE.GAME), C/SDL2, built via
  the DEADWEIGHT_2 mingw+SDL2 recipe. Three real responsibilities, deliberately split:
  - `net/`: a persistent **outbound** connection to the server, opened once at launch (plain TCP +
    newline-delimited JSON — not WebSocket; avoids needing a WS handshake/framing library in C,
    especially once cross-compiled for Windows, the same reasoning behind DEADWEIGHT_2's own
    plain-HTTP-only `http.h`, no mbedTLS).
  - `hardware/win_serial.c`: hand-written Win32 serial I/O against the Feather over USB-serial
    (the one genuinely new gap named above).
  - The compiled `traffic_router_gen.c` (from `PARENA/stdlib/edge_game/traffic_router.prn`) linked
    in directly for routing decisions, called from the hand-written host exactly like every other
    PARENA-mod-island repo already does.
  - `ui/`: SHANKPIT-lobby-style `SDL_GameController` input + a button-grid UI, ported from
    `apps/lobby/src/main.c`'s own pattern, not the whole lobby app.
- **`server/`** — a small, new relay (its own process, not folded into IDUNA — this is one
  operator and one physical cabinet, not a many-player game needing per-player identity via
  `internal/games.Registry`). Holds the one live connection from the running client; exposes an
  authenticated HTTP API for the operator to send a command down and read the response back.
  Two separate secrets: an operator token (Claude/CLI side) and a client token (the embedded
  binary's own credential, so a stray port-scanner can't pose as the cabinet).
- **Firmware** (Nano, and the Feather once its chip is confirmed) — plain PARENA scalar decision
  logic (e.g. "what light pattern for event X"), same shape as `examples/avr/blink.prn`, compiled
  via the real, existing `avr-gcc`/`avrdude` pipeline.

## On "arbitrary write commands"

Read literally: the command channel needs to carry a raw, flexible byte payload to the serial
link, because you cannot debug a hardware protocol against a fixed menu of canned effects — you
need to send exactly what you want and see exactly what comes back. Built as exactly that: a debug
primitive that writes an arbitrary byte payload to the serial port and streams back whatever the
port returns, scoped strictly to the serial link. This is **not** a remote-shell / arbitrary-
process-execution surface on the founder's Windows machine — that would be a materially different,
much higher-risk feature nobody asked for, and it is deliberately not built here. If real shell
access is later wanted, that's its own explicit ask with its own explicit risk conversation.

## EDGE.GAME as an IDE ("think of it like VS Code but it's a game")

The founder's own mental model, stated directly: EDGE.GAME's core is a real code editor with git
and a build/flash pipeline, presented as a SHANKPIT-OS-style game app rather than a normal desktop
window — not a separate, smaller "game" bolted onto an unrelated dev tool.

**The IDE itself is not new work — it's real, existing PARENA machinery, embedded, not forked
again.** `EDITOR.GAME` already did the hard part: `examples/editor_main.c`'s `main()` was
refactored (2026-09-25, "deeply integrate it as a widget") into a real, embeddable lifecycle —
`editor_widget_create`/`_dispatch_event`/`_render_frame`/`_present`/`_tick_autosave`/`_shutdown` —
callable by a host process instead of only from one big blocking loop, specifically so it could be
hosted inside SHANKPIT's own lobby compositor. EDGE.GAME reuses that exact widget API rather than
spawning the editor as a separate process or forking it a second time. The AVR compile+upload fix
above (current-file-aware, multi-board-profile-aware) lives in PARENA's own `examples/
editor_main.c` — EDGE.GAME's fork/embed should pull the FIXED version, not `EDITOR.GAME`'s own
copy, which still has the older, more broken (zero AVR Makefile targets at all) code.

**"the compiler is actually built into the game."** The source `.prn` files live on the founder's
Windows machine as ordinary local files — but `parena.exe` and a full Windows-native AVR toolchain
(`avr-gcc`/`avr-objcopy`/`avrdude`, real, redistributable Windows builds — WinAVR-style, or
extracted from the Arduino IDE's own bundled toolchain, the same no-install-needed spirit
`docs/AVR_ARDUINO_NORTHSTAR.md`'s own no-sudo Linux recipe already has) ship INSIDE the EDGE.GAME
distribution itself, at fixed paths relative to the `.exe` — matching DEADWEIGHT_2's own real,
proven "flat zip, everything needed, nothing to separately install" Windows release shape, just
carrying more than an `SDL2.dll` this time. Real, honest, not-yet-done: acquiring and packaging a
verified Windows-native `avr-gcc`/`avrdude` build is real work this Linux sandbox cannot itself
produce or test (no way to run a Windows binary here) — named as its own phase below, not silently
assumed solved by "we already have avr-gcc on Linux."

**Git affordances.** The founder's own source tree on the Windows machine is a real git working
copy — this is also the real, concrete answer to "how does the pair-programming/server-only access
model actually let you read and change code": git IS the code-sync channel (I read/propose changes
via commits — a `git log`/`git diff`-shaped view of a repo the founder's own EDGE.GAME instance can
push, or that I can push a branch to for the founder's local copy to pull), and the earlier
NDJRelay/server channel is purely the COMMAND channel (trigger compile, trigger upload, read back
build/upload output) — two separate, already-well-understood mechanisms working together, not one
new custom live-buffer-patching protocol invented from scratch. A minimal real "Source Control"
panel (status/diff/stage/commit, mirroring VS Code's own) is the concrete UI surface for this,
matching the founder's own "think of it like VS Code" framing directly — not a full git GUI, a
narrow, real slice of one.

**Theme: SHANKPIT-OS affordances, IDUNA's Solarized palette, high-contrast buttons.** All three
pieces are real, already-written elsewhere — this is application, not invention:
- **Palette**: `IDUNA.GAME/cmd/idunagame/main.go`'s own exact Solarized Light values — bg `base3`
  `#fdf6e3`, fg `base00` `#657b83`, accent `yellow` `#b58900` ("IDUNA's own real gold's nearest
  Solarized swatch," per that file's own comment) — reused verbatim, not re-derived.
- **"High contrast buttons" is already the spec, not a deviation from it.** `EmilyOS/docs/
  legacy-archive/gui-v0.1-design-capture.md` (the real "EmilyOS affordances guide," confirmed via
  `SHANKPIT/docs/SHANKPIT_OS_NORTHSTAR.md`'s own citation of it) already draws exactly this
  distinction: directory/container tiles are near-white (`EGSHELL`), but colored BUTTON tiles are
  "always darker than EGSHELL, a fixed non-white palette" — i.e. real contrast against the
  background is already a named rule, not something EDGE.GAME invents on top of a flatter
  Solarized scheme.
- **Menu structure**: `SHANKPIT/apps/lobby/src/main.c`'s own real, working button-grid code
  (`draw_lobby_buttons`/`lobby_button_pos`/`LobbyLayout`, `SDL_GameController` axis/button input) —
  the literal "shankpit menu system," confirmed to exist and work, reused structurally.

**Pair-programming command surface (extends the Phase 1 wire protocol).** Concrete new operations
for the server relay, once Phase 1's basic NDJSON channel exists: `compile {file}`, `upload {file,
board_profile}`, and their respective log/output readback — the same actions the embedded IDE's
own UI buttons trigger locally, exposed identically over the relay so an operator (me) can drive
them without ever touching the Windows desktop directly. Git handles seeing/proposing the actual
code changes (above); the relay handles making the embedded toolchain DO something with them.

## Phased plan

- **Phase 0 — done.** `traffic_router.prn`, real, tested, strict-clean; the 3-board AVR upload
  profile work + the Upload-button current-file/board-profile fix in PARENA's own editor (see
  above) — both real, tested, and land as direct, immediate improvements to PARENA/EDITOR.GAME
  regardless of EDGE.GAME's own timeline.
- **Phase 1 — the wire protocol + a minimal, fully verifiable slice with no real hardware.**
  `server/relay.js` (plain TCP + NDJSON, mint/hold one client connection, an HTTP API for the
  operator) + `client/edge_client.c` (connects, authenticates, acks commands) — proves the whole
  pipeline end to end with a real local test before any physical board is involved. Not started.
- **Phase 2 — embed the IDE widget + bundle the toolchain.** Pull EDITOR.GAME's real widget-
  lifecycle API + PARENA's now-fixed `editor_main.c` compile/upload logic into EDGE.GAME's own
  Windows client; acquire and package a verified Windows-native `parena.exe` +
  `avr-gcc`/`avr-objcopy`/`avrdude` build at fixed relative paths ("the compiler is actually built
  into the game"). Real, honest blocker: this sandbox cannot build or run a Windows binary, so the
  toolchain acquisition/packaging step itself needs verifying on a real Windows machine or in CI
  with a Windows runner, not claimed done from Linux alone.
- **Phase 3 — the theme + menu shell.** Apply IDUNA.GAME's Solarized palette + EmilyOS's own
  already-written button-contrast rule to SHANKPIT lobby's real button-grid/controller-input code,
  producing the actual SHANKPIT-OS-style shell EDGE.GAME runs inside.
- **Phase 4 — git affordances + the pair-programming command surface.** A minimal Source Control
  panel (status/diff/stage/commit) in the embedded IDE, plus the relay's own `compile`/`upload`
  operations (extending Phase 1's wire protocol) — the two together are the real mechanism behind
  "live pair programming... through the server."
- **Phase 5 — real Win32 serial I/O** against the founder's actual Feather/Nano, genuinely blocked
  on the founder's real hardware + a Windows machine to build/run on; cannot be done or verified in
  this sandbox.
- **Phase 6 — real cabinet-light firmware** for the Nano/Feather (beyond the existing `blink.prn`
  demo), using `traffic_router.prn`'s own routing decisions. Compile-verifiable in this sandbox
  today (no physical board needed to prove it builds); flashing needs the founder's real hardware.
- **Phase 7 — CI Windows cross-compile + release artifact** (same D2 flat-zip pattern, extended to
  carry the bundled toolchain too), so "ship a new binary, founder downloads and runs it" is a real
  one-command release, not a manual build.

## Open questions for the founder

1. **Is "a regular arduino" an Uno?** Assumed so (no new toolchain work either way vs. a
   new-bootloader Nano) — flag if it's something else (Mega, Due, etc.).
2. **What does the Pi actually run?** A companion Linux program (PARENA-compiled, using the real
   `hw/serial.prn`) the same way EDGE.GAME's Windows client does, or something else entirely
   (an existing off-the-shelf light-controller stack)?
3. **Transport for client↔server**: this doc defaults to plain TCP + NDJSON (simpler than
   WebSocket for a C client, no framing library needed) — flag if a browser-facing debug console
   is wanted soon, which would tip the balance back toward WebSocket.
4. **Git remote for the pair-programming sync**: does the founder want their local EDGE.GAME
   working copy pushing to/pulling from a repo Claude can already reach (e.g. a real GitHub repo
   once EDGE.GAME has an upstream), or something more local/direct (the relay server itself hosting
   a bare repo)? Affects Phase 4's real design, not guessed at here.
