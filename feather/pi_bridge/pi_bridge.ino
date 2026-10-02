// feather/pi_bridge/pi_bridge.ino -- Adafruit Feather 32u4 <-> Raspberry Pi serial bridge (cards #474/#477).
//
// The Feather's USB serial (to the Windows PC / Android tablet running the EDGE.GAME client) is
// bridged byte-for-byte to Serial1 (hardware UART: Feather TX/RX pins -> Pi GPIO15/GPIO14, 3.3V logic,
// shared GND). Two lines of local commands, handled on the Feather itself, make "is the Pi alive?"
// answerable without any Pi software at all:
//
//   !ping          -> "!pong feather32u4 pi-bridge v1"            (proves USB + sketch are alive)
//   !pi?           -> sends "\n" to the Pi, waits 600ms, then "!pi alive N bytes" or "!pi silent"
//                     (a Pi with a serial console prints a login prompt on newline; or run
//                      pi/serial_responder.sh which answers explicitly)
//   !baud <N>      -> re-opens Serial1 at N baud (default 115200)
//
// Every other line is forwarded to the Pi verbatim, and everything the Pi sends comes back over USB, so
// the EDGE.GAME relay's log stream shows the Pi's own words. The built-in LED (pin 13) blinks on traffic.
//
// Build: arduino-cli compile --fqbn adafruit:avr:feather32u4 feather/pi_bridge   (see ../README.md)
// Status: compiles; NOT yet run on real hardware -- needs the founder's Feather + Pi wiring.

static const unsigned long PI_PROBE_MS = 600;
static char cmd[40];
static uint8_t cmdLen = 0;
static bool inCmd = false;

static void blinkLed() { digitalWrite(13, !digitalRead(13)); }

static void probePi() {
  while (Serial1.available()) Serial1.read();       // drop stale bytes so we measure only the reply
  Serial1.write('\n');
  unsigned long start = millis();
  unsigned int got = 0;
  while (millis() - start < PI_PROBE_MS) {
    while (Serial1.available()) { Serial1.read(); got++; }
  }
  if (got) { Serial.print(F("!pi alive ")); Serial.print(got); Serial.println(F(" bytes")); }
  else Serial.println(F("!pi silent"));
}

static void runCommand() {
  cmd[cmdLen] = 0;
  if (!strcmp(cmd, "!ping")) Serial.println(F("!pong feather32u4 pi-bridge v1"));
  else if (!strcmp(cmd, "!pi?")) probePi();
  else if (!strncmp(cmd, "!baud ", 6)) {
    long b = atol(cmd + 6);
    if (b >= 1200 && b <= 230400) { Serial1.end(); Serial1.begin(b); Serial.print(F("!baud ")); Serial.println(b); }
    else Serial.println(F("!error bad baud"));
  } else { Serial.print(F("!error unknown command ")); Serial.println(cmd); }
}

void setup() {
  pinMode(13, OUTPUT);
  Serial.begin(115200);    // USB CDC (baud is ignored by CDC)
  Serial1.begin(115200);   // hardware UART to the Pi
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    blinkLed();
    if (!inCmd && cmdLen == 0 && c == '!') { inCmd = true; }
    if (inCmd) {
      if (c == '\n' || c == '\r') {
        if (cmdLen) { runCommand(); }
        cmdLen = 0; inCmd = false;
      } else if (cmdLen < sizeof(cmd) - 1) {
        cmd[cmdLen++] = c;
      }
    } else {
      Serial1.write(c);
    }
  }
  while (Serial1.available()) {
    Serial.write((char)Serial1.read());
    blinkLed();
  }
}
