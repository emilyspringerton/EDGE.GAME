# feather/ -- firmware for the Adafruit Feather 32u4 (cards #474/#477)

`pi_bridge/pi_bridge.ino` bridges the Feather's USB serial (to the Windows PC / Android tablet) to its
hardware UART `Serial1` (to the Raspberry Pi), with three local commands (`!ping`, `!pi?`, `!baud N`) that
answer "is the Pi alive?" from the Feather alone. Wiring: Feather TX -> Pi GPIO15 (RXD), Feather RX -> Pi
GPIO14 (TXD), GND <-> GND, both at 3.3V logic (no level shifter needed on this link).

**Status: compiles (arduino-cli, `adafruit:avr:feather32u4`, 6106 bytes / 21%), not yet run on real hardware.**
`pi_bridge.hex` is that build (sha256 `a94f09c978d5cbdb72a374d311be36538fb7736d557919afb576096c6910f974`).

## Flash it (double-tap RESET first so the red LED pulses; the Feather then enumerates as the Caterina bootloader, `239A:000C`)
```
avrdude -p atmega32u4 -c avr109 -P COMx -b 57600 -U flash:w:pi_bridge.hex:i
```
(`COMx` = the bootloader's port -- `edge_client probe` reports it; avrdude ships with the Arduino IDE.)

## Rebuild
```
arduino-cli core install arduino:avr adafruit:avr   # adafruit index: https://adafruit.github.io/arduino-board-index/package_adafruit_index.json
arduino-cli compile --fqbn adafruit:avr:feather32u4 feather/pi_bridge --output-dir out
```

## Use it through EDGE.GAME
`serial_open` (port `"auto"` finds the Feather) then `serial_write {"data":"!ping","newline":1}`; every line the Feather or the Pi
prints arrives via `log_since` / `log_subscribe` on the relay. On the Pi, `pi/serial_responder.sh` answers `ping`
with `pong <hostname> up=.. load=..` (disable the serial login console first; see its header).
