// DRO Speed Test — Mitutoyo SPC edition
// Pure frame-timestamp architecture identical to iGaging version.
// Scale input: Mitutoyo Digimatic SPC protocol (REQ-triggered, 13 nibbles).
// Internal position unit: thousandths of mm (mm×1000) — supports both
//   0.001 mm (small scale) and 0.01 mm (big scale) display modes.
// Trigger outputs moved to D6/D7 — D4/D5 now used by Mitutoyo CLOCK/REQ.

#include "esp_timer.h"
#include <math.h>

// -------------------- Pins --------------------
// Mitutoyo SPC
#define PIN_MITO_DATA  D3   // GPIO5  — DATA  (10k pullup to 3V3)
#define PIN_MITO_CLOCK D4   // GPIO6  — CLOCK (10k pullup to 3V3)
#define PIN_MITO_REQ   D5   // GPIO7  — VN2222LL MOSFET gate (pulls REQ low)

// Trigger outputs (moved from D4/D5 to make room for Mitutoyo)
#define OUT_STARTSTOP_PIN D6  // GPIO21
#define OUT_RESET_PIN     D7  // GPIO20

#define TRIGGER_ACTIVE HIGH
const uint32_t OUTPUT_PULSE_MS = 150;
// Mitutoyo is polled at ~8 Hz; declare stale after 1.5 s of silence.
const int64_t  SCALE_STALE_US  = 5000000LL; // 5 s

// ==================== Mitutoyo SPC protocol ====================
// asserting REQ (via MOSFET) causes the scale to clock out 13 nibbles.
// We sample DATA after each falling CLOCK edge.
// Timeout per clock edge: 200 ms — gives the scale plenty of time to begin
// clocking after REQ assertion. Worst-case blocking per failed frame: ~200 ms.

static uint8_t mitoNib[13];

static bool mitoWaitClock(int state) {
  uint32_t t = micros();
  while (digitalRead(PIN_MITO_CLOCK) == state) {
    if ((uint32_t)(micros() - t) > 200000) return false;
  }
  return true;
}

static bool readMitoFrame() {
  memset(mitoNib, 0, sizeof(mitoNib));

  // Assert REQ (MOSFET on → pulls Mitutoyo REQ line low)
  digitalWrite(PIN_MITO_REQ, HIGH);
  delayMicroseconds(300);

  for (int i = 0; i < 13; i++) {
    uint8_t val = 0;
    for (int bit = 0; bit < 4; bit++) {
      if (!mitoWaitClock(LOW))  { digitalWrite(PIN_MITO_REQ, LOW); return false; }
      if (!mitoWaitClock(HIGH)) { digitalWrite(PIN_MITO_REQ, LOW); return false; }
      if (digitalRead(PIN_MITO_DATA)) val |= (1 << bit);
    }
    mitoNib[i] = val;
  }

  digitalWrite(PIN_MITO_REQ, LOW);
  return true;
}

// Returns position in thousandths of mm (raw ÷ 1000 = mm).
// Sign:   bit 3 of nibble 4 = 1 → negative.
// Digits: nibbles 5–10, MSB first, BCD.
// e.g.  raw 600  → 0.600 mm,  raw 1800 → 1.800 mm
static long decodeMitoMm1000() {
  // Header: nibbles 0-3 must all be 0x0F
  for (int i = 0; i < 4; i++) {
    if (mitoNib[i] != 0x0F) return LONG_MIN;
  }
  // Instrument identifiers for this Mitutoyo in INCH mode:
  //   nibble 11 = 0x05  →  decimal position 5  →  raw / 100000 = inches
  //   nibble 12 = 0x01  →  units = inch
  // In mm mode these nibbles have different values; leave gauge in inch mode.
  if (mitoNib[11] != 0x05) return LONG_MIN;
  if (mitoNib[12] != 0x01) return LONG_MIN;
  // Digits: nibbles 5-10 must be valid BCD (0-9)
  for (int i = 5; i <= 10; i++) {
    if (mitoNib[i] > 9) return LONG_MIN;
  }

  // Sign: bit 3 of nibble 4
  bool neg = (mitoNib[4] & 0x8) != 0;

  // S5 forward decode: nib[5] = MSB, nib[10] = LSB
  long raw = 0;
  for (int i = 5; i <= 10; i++) raw = raw * 10 + mitoNib[i];

  // Convert inch reading to mm×1000:
  //   raw / 100000  = inches
  //   × 25.4        = mm
  //   × 1000        = mm×1000
  //   ────────────────────────
  //   raw × 254 / 1000  (+ 500 for rounding)
  //
  // Example: 0.1616 in → raw=16160 → (16160×254+500)/1000 = 4105 → 4.105 mm ✓
  long mm1000 = (raw * 254L + 500L) / 1000L;
  return neg ? -mm1000 : mm1000;
}

// ==================== Frame state ====================
// Internal unit: thousandths of mm (mm×1000).
long    frameMm1000 = 0;
int64_t frameUs     = 0;
bool    haveReading = false;

long    zeroMm1000  = 0;

float   liveSpeedMmMin = 0;

void onFrame(long newMm1000) {
  int64_t now = esp_timer_get_time();

  if (haveReading) {
    int64_t dtUs = now - frameUs;
    if (dtUs > 0) {
      float dtMin = dtUs / 60000000.0f;
      float dMm   = (newMm1000 - frameMm1000) / 1000.0f;
      float inst  = (fabs(dMm / dtMin) < 1.0f) ? 0.0f : dMm / dtMin;
      liveSpeedMmMin = 0.70f * liveSpeedMmMin + 0.30f * inst;
    }
  } else {
    zeroMm1000  = newMm1000;
    haveReading = true;
  }

  frameMm1000 = newMm1000;
  frameUs     = now;
}

// ==================== Test configuration ====================
bool    showInches      = true;
bool    smallScale      = false;   // false = 0.01 mm (big), true = 0.001 mm (small)
uint8_t testDurationMin = 2;
float   targetSpeedMmMin = 0.0f;

bool scaleIsLive() {
  if (!haveReading) return false;
  return (esp_timer_get_time() - frameUs) < SCALE_STALE_US;
}

// ==================== Test state machine ====================
enum TestState { TS_IDLE, TS_RUNNING, TS_HELD };
TestState testState = TS_IDLE;

int64_t testStartUs = 0;   // command clock — display / progress bar only

int64_t startFrameUs    = 0;
long    startFrameMm1000 = 0;

int64_t endFrameUs    = 0;
long    endFrameMm1000 = 0;

float resultSpeedMmMin  = 0.0f;
float resultDistanceMm  = 0.0f;
float resultElapsedSec  = 0.0f;

// -------------------- Trigger output --------------------
bool     startStopOutActive  = false;
bool     resetOutActive      = false;
uint32_t startStopOutOffAtMs = 0;
uint32_t resetOutOffAtMs     = 0;

void pulseStartStop() {
  digitalWrite(OUT_STARTSTOP_PIN, TRIGGER_ACTIVE);
  startStopOutActive  = true;
  startStopOutOffAtMs = millis() + OUTPUT_PULSE_MS;
}

void pulseReset() {
  digitalWrite(OUT_RESET_PIN, TRIGGER_ACTIVE);
  resetOutActive  = true;
  resetOutOffAtMs = millis() + OUTPUT_PULSE_MS;
}

void serviceOutputs() {
  uint32_t now = millis();
  if (startStopOutActive && (int32_t)(now - startStopOutOffAtMs) >= 0) {
    digitalWrite(OUT_STARTSTOP_PIN, !TRIGGER_ACTIVE);
    startStopOutActive = false;
  }
  if (resetOutActive && (int32_t)(now - resetOutOffAtMs) >= 0) {
    digitalWrite(OUT_RESET_PIN, !TRIGGER_ACTIVE);
    resetOutActive = false;
  }
}

// ==================== Test commands ====================

void cmdStart() {
  if (!scaleIsLive())          return;
  if (testState == TS_RUNNING) return;

  startFrameUs     = frameUs;
  startFrameMm1000 = frameMm1000;
  zeroMm1000       = frameMm1000;

  endFrameUs        = 0;
  endFrameMm1000    = 0;
  resultSpeedMmMin  = 0.0f;
  resultDistanceMm  = 0.0f;
  resultElapsedSec  = 0.0f;

  testStartUs = esp_timer_get_time();
  testState   = TS_RUNNING;
  pulseStartStop();
}

void cmdStop() {
  if (testState != TS_RUNNING) return;

  endFrameUs     = frameUs;
  endFrameMm1000 = frameMm1000;

  int64_t windowUs = endFrameUs - startFrameUs;

  if (windowUs > 0) {
    resultDistanceMm = (endFrameMm1000 - startFrameMm1000) / 1000.0f;
    resultElapsedSec = windowUs / 1000000.0f;
    float windowMin  = windowUs / 60000000.0f;
    resultSpeedMmMin = (windowMin > 0.0f) ? resultDistanceMm / windowMin : 0.0f;
  }

  testState = TS_HELD;
  pulseStartStop();
}

void cmdReset() {
  testState        = TS_IDLE;
  startFrameUs     = 0;
  startFrameMm1000 = 0;
  endFrameUs       = 0;
  endFrameMm1000   = 0;
  resultSpeedMmMin = 0.0f;
  resultDistanceMm = 0.0f;
  resultElapsedSec = 0.0f;
  testStartUs      = 0;
  liveSpeedMmMin   = 0.0f;   // clear EMA so live speed display resets immediately
  pulseReset();
}

void cmdZero() {
  if (!haveReading) return;
  zeroMm1000 = frameMm1000;
}

// ==================== Auto-stop ====================
void serviceAutoStop() {
  if (testState != TS_RUNNING) return;
  int64_t duration = (int64_t)testDurationMin * 60LL * 1000000LL;

  if (frameUs - startFrameUs >= duration) { cmdStop(); return; }

  if (!scaleIsLive()) {
    if ((esp_timer_get_time() - testStartUs) >= duration + 2000000LL) cmdStop();
  }
}

// ==================== Result accessors ====================

float getDisplayedSpeedMmMin() {
  switch (testState) {
    case TS_RUNNING: {
      int64_t windowUs = frameUs - startFrameUs;
      if (windowUs <= 0) return 0.0f;
      float distMm    = (frameMm1000 - startFrameMm1000) / 1000.0f;
      float windowMin = windowUs / 60000000.0f;
      return distMm / windowMin;
    }
    case TS_HELD: return resultSpeedMmMin;
    default:      return 0.0f;
  }
}

float getDisplayedDistanceMm() {
  switch (testState) {
    case TS_RUNNING: return (frameMm1000 - startFrameMm1000) / 1000.0f;
    case TS_HELD:    return resultDistanceMm;
    default:         return 0.0f;
  }
}

float getDisplayedElapsedSec() {
  switch (testState) {
    case TS_RUNNING: return (esp_timer_get_time() - testStartUs) / 1000000.0f;
    case TS_HELD:    return resultElapsedSec;
    default:         return 0.0f;
  }
}

float getTestProgress() {
  if (testState == TS_IDLE) return 0.0f;
  if (testState == TS_HELD) return 1.0f;
  float p = getDisplayedElapsedSec() / ((float)testDurationMin * 60.0f);
  return (p < 0.0f) ? 0.0f : (p > 1.0f) ? 1.0f : p;
}

float getPercentError() {
  if (targetSpeedMmMin <= 0.0001f) return 0.0f;
  return ((fabs(getDisplayedSpeedMmMin()) - targetSpeedMmMin) / targetSpeedMmMin) * 100.0f;
}

// ==================== JSON builder ====================
String makeApiJson() {
  float relMm = (frameMm1000 - zeroMm1000) / 1000.0f;
  float relIn = relMm / 25.4f;

  float liveSpeedInMin = liveSpeedMmMin / 25.4f;

  float elapsed = getDisplayedElapsedSec();

  float distMm = getDisplayedDistanceMm();
  float distIn = distMm / 25.4f;

  float speedMmMin = getDisplayedSpeedMmMin();
  float speedInMin = speedMmMin / 25.4f;

  float progress = getTestProgress();
  float pctError = getPercentError();

  bool running = (testState == TS_RUNNING);
  bool held    = (testState == TS_HELD);

  String j = "{";

  j += "\"haveReading\":";      j += scaleIsLive() ? "true" : "false";
  j += ",\"unit\":\"";          j += showInches ? "in" : "mm"; j += "\"";

  j += ",\"rel_mm\":";          j += String(relMm, 3);
  j += ",\"rel_in\":";          j += String(relIn, 6);

  j += ",\"live_speed_mm_min\":"; j += String(liveSpeedMmMin, 3);
  j += ",\"live_speed_in_min\":"; j += String(liveSpeedInMin, 5);

  j += ",\"duration_min\":";    j += String(testDurationMin);

  j += ",\"target_speed_mm_min\":"; j += String(targetSpeedMmMin, 3);
  j += ",\"target_speed_in_min\":"; j += String(targetSpeedMmMin / 25.4f, 5);
  j += ",\"has_target_speed\":";    j += (targetSpeedMmMin > 0.0001f) ? "true" : "false";

  j += ",\"test_running\":";    j += running ? "true" : "false";
  j += ",\"test_held\":";       j += held    ? "true" : "false";

  j += ",\"test_elapsed_s\":";  j += String(elapsed, 6);
  j += ",\"test_distance_mm\":"; j += String(distMm, 3);
  j += ",\"test_distance_in\":"; j += String(distIn, 6);
  j += ",\"test_speed_mm_min\":"; j += String(speedMmMin, 3);
  j += ",\"test_speed_in_min\":"; j += String(speedInMin, 5);
  j += ",\"test_progress\":";    j += String(progress, 4);
  j += ",\"test_percent_error\":"; j += String(pctError, 3);
  j += ",\"small_scale\":";         j += smallScale ? "true" : "false";

  j += "}";
  return j;
}


// ==================== USB Serial command interface ====================
String serialLine;

void handleSerialCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line == "START") { cmdStart(); return; }
  if (line == "STOP")  { cmdStop();  return; }
  if (line == "RESET") { cmdReset(); return; }
  if (line == "ZERO")  { cmdZero();  return; }

  if (line.startsWith("UNIT ")) {
    String u = line.substring(5);
    u.trim();
    showInches = (u == "in");
    return;
  }

  if (line.startsWith("SCALE ")) {
    String s = line.substring(6);
    s.trim();
    smallScale = (s == "1");
    return;
  }

  if (line.startsWith("DURATION ")) {
    int m = line.substring(9).toInt();
    if (m < 1)  m = 1;
    if (m > 10) m = 10;
    testDurationMin = (uint8_t)m;
    return;
  }

  if (line.startsWith("TARGET ")) {
    String rest = line.substring(7);
    rest.trim();
    int sp = rest.indexOf(' ');
    String vStr = (sp >= 0) ? rest.substring(0, sp) : rest;
    String uStr = (sp >= 0) ? rest.substring(sp + 1) : (showInches ? "in" : "mm");
    vStr.trim();
    uStr.trim();
    float v = vStr.toFloat();
    if (v < 0.0f) v = 0.0f;
    targetSpeedMmMin = (uStr == "in") ? v * 25.4f : v;
    return;
  }
}

void serviceSerial() {
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      handleSerialCommand(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 160) {
      serialLine += ch;
    } else {
      serialLine = ""; // drop overlong garbage line
    }
  }
}

void sendStatusEvery200ms() {
  static uint32_t lastStatusMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastStatusMs >= 200) {
    lastStatusMs = nowMs;
    Serial.println(makeApiJson());
  }
}

// ==================== Setup / Loop ====================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_MITO_DATA,  INPUT_PULLUP);
  pinMode(PIN_MITO_CLOCK, INPUT_PULLUP);
  pinMode(PIN_MITO_REQ,   OUTPUT);
  digitalWrite(PIN_MITO_REQ, LOW);   // MOSFET off — REQ released

  pinMode(OUT_STARTSTOP_PIN, OUTPUT);
  pinMode(OUT_RESET_PIN,     OUTPUT);
  digitalWrite(OUT_STARTSTOP_PIN, !TRIGGER_ACTIVE);
  digitalWrite(OUT_RESET_PIN,     !TRIGGER_ACTIVE);

  Serial.println("DRO Speed Test — Mitutoyo SPC USB Web Serial edition");
  Serial.println("Open DRO_WebSerial_GUI.html in Chrome/Edge and click Connect USB.");
  Serial.println("Mitutoyo: DATA=D3  CLOCK=D4  REQ=D5(MOSFET)");
  Serial.println("OUTPUT:   START/STOP=D6  RESET=D7");
}

void loop() {
  serviceSerial();
  serviceOutputs();
  serviceAutoStop();
  sendStatusEvery200ms();

  // Poll Mitutoyo at ~8 Hz. readMitoFrame() is blocking when the scale is unresponsive.
  static uint32_t lastFrameMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastFrameMs >= 125) {
    lastFrameMs = nowMs;
    if (readMitoFrame()) {
      long val = decodeMitoMm1000();
      if (val != LONG_MIN) onFrame(val);
    }
  }
}
