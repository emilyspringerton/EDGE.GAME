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
6. After Phase 1 shipped, pasted a second AI-generated tutorial (same reviewed-critically-not-
   transcribed treatment as message 3's own pasted design) about Android-tablet kiosk payment
   processing (Stripe/Square/PayPal, Tap-to-Pay/QR, USB-MIDI-triggered relays on a successful
   charge, a charity-donation-cabinet framing). Asked directly whether this was part of EDGE.GAME
   or a separate project — founder's answer, verbatim: "its a separate component of EDGE.GAME we
   have an android tablet that is actually talks to the whole system too - so its like kubernetes
   with like 2 raspi bs an edge node raspi w or 2 a ada feather roting all of the hardware and
   maybe an arduino or 2 as a slave for more peripherals - like the android is going to handle the
   main brain of the actual kiosk - the windows environment is our dev platform the android
   platform is our execution platform the feather will be the interface to either either usb to
   the windows computer or usb OTG adapter to the android tablet." This is real, resolved, and adds
   a materially new component — see "Production topology: Android as the execution platform"
   below for the full, consolidated design this resolves into.
7. Asked for the actual IDE feedback loop to be built (top pane: compile/upload/files/save
   buttons "like shankpit"; bottom pane: the code editor; a non-blocking socket open to editor
   events; the server can send events to the editor and buttons too; a debug mode for direct pin
   access via an uploaded harness once hardware is attached, explicitly named "a little bit dog
   foody"; v0 acceptance bar is blinking a light from the server/operator side end to end; skip a
   simulator for v0), then, mid-build, corrected the relay's own implementation language three
   times in a row: "we can run sarena from one of the raspberry pis but we need the serial to work
   first" (confirms a Pi runs PARENA-editor/notebook tooling, gated on serial) → "oh you need
   server streaming events for that too like buffered obviously" + "i need you to be able to tell
   me if the pi booted" (the real, concrete events-channel ask) → "make sure we are using REFLUX
   for the pub sub" → "also the spotlight bar should be included too this is a real IDE" → "write
   it in PARENA in what world are we using node for any part of this stack?" → "all of the node
   stuff gets ported to PARENA" → "why is the server JS? PARENA emits TS what the fuck is
   happening" → "the backend is written in PARENA and you can dog food burrow into golang if you
   really need to but i think C on the server is ok i dont know" → "parena really needs to just
   emit the fucking LLVM code for the server I think i think we eat that tech debt and we get
   parena binaries going for the server obviously the windows client is C." Resolved: the relay
   (originally Node.js, a real mistake corrected here) is now a native PARENA+C binary compiled
   through PARENA's own LLVM target — see "Phase 1.5" below. The IDE feedback loop itself (the
   embedded editor widget + button bar + socket) is still Phase 2, not yet built — see the real,
   found fork-divergence and the Spotlight-bar requirement named in "EDGE.GAME as an IDE" below.

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

## Production topology: Android as the execution platform (2026-09-29 clarification)

Message 6 above resolves a real, two-platform split that didn't exist in the original scoping —
not a bigger version of the same Windows-only plan, a second, parallel arm:

- **Windows = dev/debug platform.** Unchanged from everything above — this is where I (Claude, via
  the server relay) help debug the USB upload code, the serial protocol, etc. Phases 0-7 below are
  entirely this arm.
- **Android tablet = the kiosk's actual production "brain."** The tablet, not the Windows PC, is
  what runs live in the field — it's the real HMI (screen, payment UI, game trigger) and the real
  decision-maker once the cabinet is deployed. The Windows arm exists to develop and debug the
  hardware/firmware side; it does not run in production.
- **The Feather is dual-mode, not simultaneous.** One physical USB link, switched between two
  hosts depending on which platform is currently driving the cabinet: USB directly to the Windows
  PC during dev, or through a USB-OTG adapter to the Android tablet in production. Whichever host
  is attached owns the serial link; there's no scoped case (yet) for both being attached and
  arbitrating at once.
- **The Raspberry Pis are a real edge-node layer, not a single companion box.** Founder's own
  words: "like kubernetes with like 2 raspi bs an edge node." Real, honest, NOT yet resolved:
  whether this means literal container orchestration (k3s/k8s across 2 Pis) or an informal way of
  saying "2 Pi nodes splitting hardware-routing duties" — see open question 5 below; nothing here
  assumes either answer.
- **1-2 additional Arduino boards as peripheral slaves**, beyond the Nano already scoped in
  "Real physical topology" above — same multi-profile avrdude problem already solved there
  applies again; no new toolchain work, just more boards to flash with the same pipeline.
- **Payment processing lives on the Android tablet**, gating a paid game/kiosk session before the
  Feather is told to do anything (Stripe/Square/PayPal/Tap-to-Pay/QR per the founder's own pasted
  material, none chosen yet). Named here but deliberately **not scoped into a phase with code
  yet** — this is real financial infrastructure (actual charges, not a toy), a materially
  different risk class from everything else in this doc, and gets its own explicit go-ahead and
  processor choice before any implementation, not folded silently into the general Android phase.

This does not change or invalidate anything in "Access model," "Real physical topology," "The
traffic_router module," or "EDGE.GAME as an IDE" above — those all still describe the real
Windows/dev arm exactly as scoped. This section is additive: a second, parallel production arm.

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
- **MJOLNIR is the real Android precedent in this monorepo** (Kotlin 2.0/Jetpack Compose/Hilt,
  Retrofit → IDUNA, FCM push) — the real template for an EDGE.GAME Android client's project
  skeleton and IDUNA auth wiring, not invented from scratch. Real, honest, checked directly: this
  sandbox has **no Android SDK and no `gradlew` wrapper checked out even for MJOLNIR itself** —
  the same exact gap `SPIDERBEETLE/NORTHSTAR.md` already found and named. An Android client here
  can be scoped and its source written, but not built or run in this sandbox — needs the founder's
  own machine or a real CI Android-SDK runner, same as every other Android work in this monorepo.
- **No USB-OTG-serial code existed anywhere in this monorepo as of 2026-09-29** — checked directly
  (grepped for `UsbManager`/`UsbSerialPort`/OTG across every `.kt`/`.java`/`.md` file, zero real
  hits at the time). Real, first step now shipped in PARENA: see Phase A2 below — a new PARENA
  compiler capability (`:java` FFI) plus a first real module (`hw/usb_serial.prn`). Still
  genuinely missing: the actual Android app, and the real `UsbManager` device-permission dance
  itself (stays hand-written Kotlin — this v0 Java emitter cannot express stateful callback-driven
  platform ceremony at all, only stateless scalar calls against a static field).
- **PARENA's Java emitter is real but v0/scalar-only** (no structs/loops/collections — confirmed
  directly in `DEADWEIGHT/NORTHSTAR.md`'s own capability audit, and `SPIDERBEETLE`'s real,
  shipped `battery_ui.prn`→`BatteryUi.java` is still its only precedent: two standalone scalar
  helper functions, not a whole app). The same boundary applies here — PARENA can supply small,
  scalar decision helpers into the Android client (same shape as SPIDERBEETLE's), not the client
  itself.
- **IDUNA's own inventory scoping doc independently confirms real hardware on hand**
  (`IDUNA/docs/NORTHSTAR_INVENTORY.md`, `II-001`, 2026-09-03, founder's own words): "2 rasbberyy
  pi b2 and 2 zero i think? also i have at least 1 ada feather with some kinda packetmodule" — a
  real, pre-existing record matching "2 raspi's" named here, not a new or hypothetical claim.

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
  `internal/games.Registry`). **Rewritten 2026-09-29 (see "Phase 1.5" below): a real native
  PARENA+C binary (`build/edge_relay`), not Node.js.** Holds the one live connection from the
  running client; both the cabinet port AND the operator port speak plain TCP + NDJSON (the
  original HTTP operator API is gone — PARENA has no HTTP-server stdlib, and unifying transports
  is simpler than inventing one). Three separate secrets: an operator token (Claude/CLI side), a
  client token (the embedded Windows binary's own credential), and a Pi token (each Raspberry
  Pi's own boot-announce script) — so a stray port-scanner can't pose as any of the three.
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
spawning the editor as a separate process or forking it a second time.

**Real, found-live correction (2026-09-29): the two forks have diverged, and this section's own
earlier claim was wrong.** Checked directly (grepped for the actual function DEFINITIONS, not just
usage): the widget lifecycle functions above exist ONLY in `EDITOR.GAME/examples/editor_main.c` —
NOT in PARENA's own copy, despite this section previously claiming otherwise. PARENA's own
`examples/editor_main.c` has the AVR multi-board upload fix (current-file-aware,
multi-board-profile-aware `compile_and_upload_avr`) but NOT the widget lifecycle refactor;
`EDITOR.GAME`'s own copy has the widget lifecycle but NOT the AVR fix (confirmed:
`EDITOR.GAME`'s own `Makefile` has zero `avr-*` targets at all). Neither fork has both. Phase 2's
own real first step, not yet done, is merging these two divergent copies into one — pulling
`EDITOR.GAME`'s widget-lifecycle structure as the base and porting PARENA's AVR fix onto it —
before EDGE.GAME can embed a version with both real capabilities at once.

**Also new, from the founder directly (2026-09-29): "also the spotlight bar should be included too
this is a real IDE."** The embedded widget must carry PARENA's real Spotlight overlay
(`stdlib/editor/spotlight.prn`'s own fuzzy file/command search, already real and shipped in the
editor demo) along with syntax highlighting and the file tree — not a stripped-down shell with only
Save/Upload/Compile buttons. Named here as a firm Phase 2 requirement, not an optional nice-to-have.

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
- **Phase 1 — done.** `server/relay.js` (plain TCP + NDJSON, holds one cabinet connection, an
  authenticated HTTP API for the operator) + `client/edge_client.c` (connects, authenticates, and
  answers a real `route` command by calling into the actual compiled `route_for_source()` from
  Phase 0's `traffic_router.prn` — not just an echo, proving the two phases genuinely fit
  together). `make test-e2e`: a real, reproducible, no-hardware local proof — 6/6 checks pass
  (cabinet-connected status, all three real routing decisions, a generic ack for an unrelated
  command type, and operator-token rejection), `-Wall -Wextra -pedantic -Werror` clean. One real
  gotcha found and fixed: the shared PARENA runtime needs to be the very first `#include` in any
  file that pulls it in (it defines `_POSIX_C_SOURCE` internally, which only works if set before
  glibc's own headers are first touched) — `edge_client.c`'s original include order broke this.
- **Phase 1.5 — done (2026-09-29). The relay rewritten in PARENA + C; Node.js removed entirely.**
  Founder real-time, real-time-corrected twice: "write it in PARENA in what world are we using node
  for any part of this stack?" → "all of the node stuff gets ported to PARENA" → "make sure we are
  using REFLUX for the pub sub" → "parena really needs to just emit the fucking llvm code for the
  server... i think we eat that tech debt... obviously the windows client is C." Real, new PARENA
  compiler capability added to get there: `src/emit_llvm.c` had **zero FFI mechanism at all**
  before this (confirmed directly in its own error text: "v0 has no external FFI/math-primitive
  table yet") — a new `#target {:llvm (inline-llvm "...")}` hatch was added, mirroring the C/Java
  emitters' own `:c`/`:java` hatches but adapted for LLVM's SSA return convention (see
  `PARENA/docs/LLVM_BACKEND_NORTHSTAR.md`'s own new dated section for the full compiler-side
  detail; 19 new tests, 71/71 pass; `make test`: 347/347, zero regressions). A new top-level
  `(llvm-extern "declare ...")` form was added alongside it for external C-ABI declarations.
  `stdlib/reflux/reflux.prn` (PARENA's real, existing, multi-repo cross-mod pub/sub log — same
  API/ABI as SHANKPIT's and IDUNA.GAME's own ports) got a second `:llvm` key added to its existing
  `#target` maps (zero redesign needed — the file was already pure I32/Unit scalar); a new, narrow,
  LLVM-target-only module `stdlib/net/tcp_llvm.prn` provides raw `tcp-listen-raw`/`tcp-accept-raw`/
  `tcp-close-raw` (net/tcp.prn's own higher-level String/Result/Arena-typed wrappers can't reach
  this target — no struct/Region/String support in LLVM v0 — so this is a deliberately separate,
  minimal file, not a fork of that one). Real, live, end-to-end build chain, not just unit-tested in
  isolation: `parena build *.prn -o *.ll` → real `llc -mtriple=x86_64-pc-linux-gnu` → real object
  code → linked (via plain `gcc`) against a hand-written C host (`server/relay_main.c`, the
  select()/NDJSON plumbing PARENA's scalar-only v0 genuinely can't express — same "PARENA owns the
  decision/log, hand-written C owns raw syscalls/buffers" split every other PARENA-mod-island in
  this monorepo already uses) — one real, standalone `build/edge_relay` binary, zero Node anywhere.
  New, real events channel on the operator port answering "you need server streaming events for
  that too like buffered obviously": `events_since`/`events_subscribe` read the SAME real REFLUX
  ring-buffer log a device's `event` push writes into — buffered (a subscriber that wasn't
  listening yet still sees it on its next `events_since`) AND live (a subscriber gets pushed new
  entries as they're dispatched, no polling). `pi/boot_announce.sh` + `pi/
  edge-boot-announce.service` directly answer "i need you to be able to tell me if the pi booted" —
  a plain bash `/dev/tcp` one-shot (no curl/python dependency needed on a fresh Pi image),
  dispatching `REFLUX_ACTION_PI_BOOTED`, live-verified against the real relay binary. `make
  test-e2e`: rewritten for the new TCP+NDJSON-everywhere protocol, 12/12 checks pass (the original
  3 routing decisions + generic ack + wrong-token rejection, plus 6 new checks for the events
  channel). Real, honest, not yet done: no physical Pi exists in this sandbox to verify
  `edge-boot-announce.service` itself booting for real (only the script's own wire behavior against
  a live relay is verified) — named, not hidden.
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

**Production/Android arm (parallel to Phases 0-7 above, not sequenced after them — separate
platform, separate concern):**

- **Phase A1 — Android client skeleton.** A new Kotlin/Compose project templated on MJOLNIR's own
  structure (Hilt DI, Retrofit → IDUNA for auth), no payment or serial code yet. Real, honest:
  scoped and written here, not buildable/runnable in this sandbox (no Android SDK) — same gap
  SPIDERBEETLE already named.
- **Phase A2 — USB-OTG serial to the Feather. Partially done.** Founder real-time: "usb to serial
  code goes in parena" — resolved: PARENA's Java emitter had **no FFI mechanism of any kind**
  before today (checked directly, only a hardcoded `java.lang.Math` table), so a new
  `#target {:java (inline-java "...")}` escape hatch was added to `src/emit_java.c`, mirroring
  the C emitter's own long-standing `:c` hatch exactly (8 new tests, 43/43 total pass). First real
  consumer: `PARENA/stdlib/hw/usb_serial.prn` (`usb-serial-is-connected`/`-write-byte`/
  `-read-byte`/`-baud-for-board`), verified via the real `parena build` CLI + a real `javac`
  compiling the emitted Java against a hand-written stub standing in for the real
  `usb-serial-for-android` library (no Android SDK in this sandbox — same gap SPIDERBEETLE/
  MJOLNIR already name), 8/8 runtime assertions pass. Real, honest, still not done: the actual
  Android app calling this (Phase A1 doesn't exist yet), the real permission/open ceremony
  (genuinely inexpressible in this scalar-only v0 — stays hand-written Kotlin by design, same
  split as Windows `hardware/win_serial.c`), and verification against the real library/SDK. See
  `PARENA/STDLIB.md`'s own "Java emitter's new `:java` FFI escape hatch" section for full detail.
- **Phase A3 — Payment processing.** Explicitly gated, not started: real money changes hands here,
  a materially different risk class from the rest of this doc. Needs its own founder go-ahead and
  processor choice (Stripe Terminal / Square / native Android NFC Tap-to-Pay were named in the
  founder's own pasted material, none chosen) before any code is written.
- **Phase A4 — Pi edge-node layer.** Blocked on open question 5 below (literal k3s/k8s vs. an
  informal "2 Pi nodes splitting duties") — real infrastructure scope differs a lot between the two
  answers, not guessed at here.

## Open questions for the founder

1. **Is "a regular arduino" an Uno?** Assumed so (no new toolchain work either way vs. a
   new-bootloader Nano) — flag if it's something else (Mega, Due, etc.).
2. **What does the Pi actually run?** A companion Linux program (PARENA-compiled, using the real
   `hw/serial.prn`) the same way EDGE.GAME's Windows client does, or something else entirely
   (an existing off-the-shelf light-controller stack)?
3. ~~**Transport for client↔server**~~ — **resolved 2026-09-29 (Phase 1.5).** Plain TCP + NDJSON
   for both the cabinet port AND the operator port (the original HTTP operator API was dropped
   when the relay itself was rewritten in PARENA+C — no external framing library needed on the C
   side, and PARENA has no HTTP-server stdlib to lean on either). Still flag if a browser-facing
   debug console is wanted later — that would tip the balance back toward WebSocket for that one
   consumer specifically, not a reason to revisit the cabinet/operator wire format itself.
4. **Git remote for the pair-programming sync**: does the founder want their local EDGE.GAME
   working copy pushing to/pulling from a repo Claude can already reach (e.g. a real GitHub repo
   once EDGE.GAME has an upstream), or something more local/direct (the relay server itself hosting
   a bare repo)? Affects Phase 4's real design, not guessed at here.
5. **Is the "kubernetes" framing for the 2 Raspberry Pis literal or informal?** Real container
   orchestration (k3s/k8s) across 2 Pis is a materially bigger infrastructure lift than 2 Pi nodes
   each just running a plain PARENA/Linux companion program that splits hardware-routing duties.
   Affects Phase A4 directly — not guessed at here.
6. **Payment processor/SDK choice, and is this real production payment handling from day one?**
   The founder's own pasted material names Stripe Terminal, Square, and native Android NFC
   Tap-to-Pay as options, none chosen. Given real money is involved, this needs an explicit
   go-ahead and choice before Phase A3 gets any code — not assumed or defaulted to one option.
