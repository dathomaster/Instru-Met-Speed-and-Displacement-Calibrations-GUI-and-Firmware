// DRO Speed Test — Mitutoyo SPC edition
// Pure frame-timestamp architecture identical to iGaging version.
// Scale input: Mitutoyo Digimatic SPC protocol (REQ-triggered, 13 nibbles).
// Internal position unit: thousandths of mm (mm×1000) — supports both
//   0.001 mm (small scale) and 0.01 mm (big scale) display modes.
// Trigger outputs moved to D6/D7 — D4/D5 now used by Mitutoyo CLOCK/REQ.

#include <WiFi.h>
#include <WebServer.h>
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

// -------------------- Wi-Fi --------------------
const char *AP_SSID = "DRO_ESP32";
const char *AP_PASS = "12345678";

WebServer server(80);

// ==================== Mitutoyo SPC protocol ====================
// asserting REQ (via MOSFET) causes the scale to clock out 13 nibbles.
// We sample DATA after each falling CLOCK edge.
// Timeout per clock edge: 2 ms — generous for Mitutoyo (~100–500 kHz clock)
// but keeps worst-case blocking to ~208 ms if scale is unresponsive.

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

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>DRO</title>
<style>
:root {
  --bg: #f6f7fb;
  --card: #ffffff;
  --text: #111827;
  --muted: #6b7280;
  --line: #e5e7eb;
  --accent: #111827;
  --green: #16a34a;
  --red: #dc2626;
  --shadow: 0 14px 40px rgba(15,23,42,0.08);
  --radius: 24px;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  font-family: system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif;
  background: radial-gradient(circle at top left,#ffffff 0,#f6f7fb 50%,#eef2ff 100%);
  color: var(--text);
}
.wrap { max-width: 1050px; margin: 0 auto; padding: 22px; }
.top  { display:flex; justify-content:flex-end; align-items:center; margin-bottom:18px; }
.pill {
  border:1px solid var(--line); background:rgba(255,255,255,0.85);
  border-radius:999px; padding:9px 12px; color:var(--muted); font-size:13px; white-space:nowrap;
}
.pill.running { background:#dcfce7; color:#166534; border-color:#86efac; font-weight:850; }
.grid { display:grid; grid-template-columns:1.2fr 1fr; gap:20px; align-items:start; }
.card {
  background:rgba(255,255,255,0.94); border:1px solid rgba(229,231,235,0.95);
  border-radius:var(--radius); box-shadow:var(--shadow); padding:24px; backdrop-filter:blur(12px);
}
.card.runningCard { border-color:#86efac; box-shadow:0 14px 44px rgba(22,163,74,0.16); }
.positionCard { text-align:center; padding-top:30px; padding-bottom:30px; }
.positionCard.simplified { min-height:420px; }
.positionCard.simplified .liveSpeedBox { display:none; }
.label { font-size:12px; color:var(--muted); text-transform:uppercase; letter-spacing:0.09em; font-weight:800; }
.mainValue {
  font-variant-numeric:tabular-nums;
  font-size:clamp(60px,13vw,118px); font-weight:900; letter-spacing:-0.03em;
  line-height:0.95; margin:10px 0 14px;
}
.unit { font-size:24px; color:var(--muted); font-weight:750; margin-left:8px; }
.copyBtn {
  padding:2px 6px; border-radius:7px; font-size:11px; margin-left:4px;
  vertical-align:middle; background:#ffffff; color:#6b7280;
  border:1px solid #d1d5db; min-width:auto; line-height:1;
}
.liveSpeedBox { margin-top:28px; padding-top:20px; border-top:1px solid var(--line); text-align:center; }
.liveSpeedValue {
  font-variant-numeric:tabular-nums;
  font-size:clamp(30px,6vw,52px); font-weight:850; letter-spacing:-0.02em; margin-top:6px;
}
.liveSpeedValue .unit { font-size:18px; }
.row { display:grid; grid-template-columns:1fr 1fr; gap:10px; margin-top:28px; }
button,select,input {
  appearance:none; border-radius:18px; padding:15px 16px; font-weight:850; font-size:15px;
}
button { border:0; background:var(--accent); color:white; cursor:pointer; }
select,input { background:#ffffff; color:var(--text); border:1px solid var(--line); width:100%; }
input { cursor:text; font-variant-numeric:tabular-nums; }
button:focus-visible,select:focus,input:focus {
  outline:2px solid rgba(30,58,138,0.22); outline-offset:2px;
}
.secondary { background:#eef2ff; color:#1e3a8a; }
.green { background:var(--green); }
.red   { background:var(--red); }
.light { background:#ffffff; color:var(--text); border:1px solid var(--line); }
.iconBtn { font-size:21px; line-height:1; padding:10px; }
.testHeader { display:flex; justify-content:space-between; align-items:flex-start; gap:14px; margin-bottom:8px; }
.testTitle  { font-size:22px; font-weight:800; }
.headerMeta { width:100%; max-width:170px; display:flex; flex-direction:column; align-items:stretch; }
.headerProgressTrack { height:4px; background:#e9edf5; border-radius:999px; overflow:hidden; }
.headerProgressFill  {
  height:100%; width:0%; background:var(--accent); border-radius:999px; transition:width 0.12s linear;
}
.runningCard .headerProgressFill { background:var(--green); }
.targetBox  { display:block; margin-top:12px; }
.testButtons { display:grid; grid-template-columns:minmax(0,1fr) 72px; gap:8px; margin-top:12px; }
.testButtons button { padding:13px 12px; font-size:14px; border-radius:15px; }
.testButtons .iconBtn { font-size:19px; }
.actionBtn { width:100%; }
.miniStatus { color:var(--muted); font-size:13px; margin:0 0 6px; font-weight:700; text-align:right; }
.resultBlock { margin-top:16px; padding-top:16px; border-top:1px solid var(--line); }
.speedRow { display:flex; justify-content:space-between; align-items:flex-start; gap:18px; margin-top:8px; }
.resultSpeed {
  font-variant-numeric:tabular-nums;
  font-size:clamp(34px,8vw,62px); font-weight:850; letter-spacing:-0.05em; line-height:0.95;
}
.inlineError {
  text-align:right; color:var(--muted); font-size:13px;
  line-height:1.25; white-space:nowrap; font-variant-numeric:tabular-nums; padding-top:8px;
}
.inlineErrorValue { display:block; margin-top:3px; color:#4b5563; font-size:20px; font-weight:800; }
.inlineErrorValue.missing { color:#9ca3af; font-weight:700; }
.metricGrid { display:grid; grid-template-columns:1fr 1fr; gap:16px; margin-top:18px; }
.metricValue {
  font-size:clamp(24px,5vw,42px); font-weight:850; letter-spacing:-0.03em;
  line-height:1; margin-top:8px; font-variant-numeric:tabular-nums;
}
.valueWrap { display:flex; flex-direction:column; align-items:flex-start; }
.copyUnder { margin-top:10px; }
.tiny { color:var(--muted); font-size:13px; margin-top:6px; line-height:1.35; font-variant-numeric:tabular-nums; }
.footer { margin-top:20px; color:var(--muted); font-size:12px; text-align:center; line-height:1.45; }
@media (max-width:780px) {
  .grid,.row,.metricGrid { grid-template-columns:1fr; }
  .testHeader { flex-wrap:wrap; }
  .headerMeta { width:100%; max-width:none; flex-basis:100%; }
  .speedRow { flex-direction:column; }
  .miniStatus,.inlineError { text-align:left; padding-top:0; }
}
</style>
</head>
<body>
<div class="wrap">

<div class="top">
  <div class="pill" id="status">connecting...</div>
</div>

<div class="grid">

<div class="card positionCard" id="positionCard">
  <div>
    <span class="mainValue" id="pos">0.0000</span>
    <span class="unit" id="unit">in</span>
    <button class="copyBtn" onclick="copyVal('pos')">⧉</button>
  </div>
  <div class="row">
    <button onclick="cmd('/zero')">ZERO</button>
    <button class="secondary" onclick="toggleUnits()">Toggle in / mm</button>
  </div>
  
<div style="margin-top:10px">
  <button class="secondary" id="scaleBtn" onclick="toggleScale()"
    style="width:100%;display:flex;align-items:center;justify-content:center;gap:9px;padding:12px 16px">
    <span id="scaleSmBox" style="display:inline-block;width:8px;height:12px;background:currentColor;border-radius:2px;opacity:0.3;flex-shrink:0"></span>
    <span id="scaleLgBox" style="display:inline-block;width:18px;height:12px;background:currentColor;border-radius:2px;flex-shrink:0"></span>
    <span id="scaleLabel">Big &middot; 0.01mm</span>
  </button>
</div>
<div class="liveSpeedBox">
    <div class="label">Live Speed</div>
    <div class="liveSpeedValue">
      <span id="liveSpeed">0.0000</span>
      <span class="unit" id="liveSpeedUnit">in/min</span>
    </div>
  </div>
</div>

<div class="card testCard" id="testCard">

  <div class="testHeader">
    <div class="testTitle">Speed Test</div>
    <div class="headerMeta">
      <div class="miniStatus" id="testState">Idle</div>
      <div aria-label="Speed test progress">
        <div class="headerProgressTrack">
          <div class="headerProgressFill" id="testProgressFill"></div>
        </div>
      </div>
    </div>
  </div>

  <div class="tiny">Duration</div>
  <select id="duration" onchange="setDuration()">
    <option value="1">1 minute</option>
    <option value="2" selected>2 minutes</option>
    <option value="3">3 minutes</option>
    <option value="4">4 minutes</option>
    <option value="5">5 minutes</option>
    <option value="6">6 minutes</option>
    <option value="7">7 minutes</option>
    <option value="8">8 minutes</option>
    <option value="9">9 minutes</option>
    <option value="10">10 minutes</option>
  </select>

  <div class="targetBox">
    <div class="tiny">Target speed</div>
    <input id="targetSpeed" type="number" min="0" step="1.000"
           placeholder="optional"
           onchange="setTargetSpeed()" onblur="setTargetSpeed()"
           onkeydown="if(event.key==='Enter'){event.preventDefault();event.target.blur()}">
  </div>

  <div class="testButtons">
    <button class="green actionBtn" id="actionBtn" onclick="toggleTest()">Start</button>
    <button class="light iconBtn" title="Reset" onclick="cmd('/test?cmd=reset')">↻</button>
  </div>

  <div class="resultBlock">
    <div class="label">Test Speed</div>
    <div class="speedRow">
      <div class="valueWrap">
        <div>
          <span class="resultSpeed" id="resultSpeed">--</span>
          <span class="unit" id="resultUnit">in/min</span>
        </div>
        <button class="copyBtn copyUnder" onclick="copyVal('resultSpeed')">⧉</button>
      </div>
      <div class="inlineError">
        <span>Percent error</span>
        <span class="inlineErrorValue missing" id="percentError">--</span>
      </div>
    </div>

    <div class="metricGrid">
      <div>
        <div class="label">Distance</div>
        <div class="valueWrap">
          <div class="metricValue">
            <span id="testDist">--</span>
            <span class="unit" id="testDistUnit">in</span>
          </div>
          <button class="copyBtn copyUnder" onclick="copyVal('testDist')">⧉</button>
        </div>
      </div>
      <div>
        <div class="label">Elapsed</div>
        <div class="valueWrap">
          <div class="metricValue">
            <span id="testTime">--</span>
            <span class="unit">s</span>
          </div>
          <button class="copyBtn copyUnder" onclick="copyVal('testTime')">⧉</button>
        </div>
      </div>
    </div>
  </div>

</div>
</div>

<div class="footer">
  <b>External Outputs</b> · START/STOP: <b>D4 / GPIO6</b> · RESET: <b>D5 / GPIO7</b>
</div>
</div>

<script>
"use strict";
let currentUnit = "in";

// ---- Request serializer -----------------------------------------------
// One request in-flight at a time. Prevents concurrent HTTP requests from
// overwhelming the ESP32's single-threaded WebServer (root cause of the
// intermittent "sometimes it works" behaviour).
let busy = false;

// ---- Audio -----------------------------------------------
// Web Audio API — generates tones directly in the browser.
// No external files needed, works offline on the ESP32 AP.
//
// Why the original version stopped working:
//   Browsers suspend AudioContext after a period of silence. The old
//   ensureAudio() called resume() but didn't await it, so the context
//   was still suspended when tones were scheduled — they silently dropped.
//
// Fix: playDoneSound() is async and awaits resume() before scheduling
// anything. ensureAudio() is still called on every button press (user
// gesture) to keep the context as warm as possible, but playDoneSound()
// handles the suspended case reliably on its own.
let audioCtx = null;

function ensureAudio() {
  if (audioCtx && audioCtx.state === "closed") audioCtx = null;
  if (!audioCtx) {
    try { audioCtx = new (window.AudioContext || window.webkitAudioContext)(); }
    catch(e) {}
  }
  // Best-effort resume during a user gesture — does NOT need to be awaited here
  if (audioCtx && audioCtx.state === "suspended") audioCtx.resume();
}

async function playDoneSound() {
  try {
    // Recreate if the context was closed
    if (audioCtx && audioCtx.state === "closed") audioCtx = null;
    if (!audioCtx) {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    }
    // AWAIT the resume so we know the context is actually running
    // before scheduling any nodes. This is the fix for the "sound stops
    // working after a while" bug — the context gets suspended during the
    // 2-minute test and the old code fired tones while it was still asleep.
    if (audioCtx.state === "suspended") await audioCtx.resume();
    if (audioCtx.state !== "running") return;

    const ctx = audioCtx;
    const t   = ctx.currentTime;

    // Arrow function avoids the 'function' keyword that the Arduino IDE
    // preprocessor picks up inside raw string literals when generating
    // C++ prototypes, which caused the compile error.
    const tone = (freq, start, dur, vol) => {
      const osc = ctx.createOscillator();
      const g   = ctx.createGain();
      osc.type = "sine";
      osc.frequency.value = freq;
      osc.connect(g);
      g.connect(ctx.destination);
      g.gain.setValueAtTime(0, start);
      g.gain.linearRampToValueAtTime(vol, start + 0.012);
      g.gain.exponentialRampToValueAtTime(0.0001, start + dur);
      osc.start(start);
      osc.stop(start + dur + 0.02);
    };

    tone(660, t,        0.18, 0.25);
    tone(880, t + 0.21, 0.32, 0.20);
  } catch(e) {}
}

function withTimeout(p, ms = 2500) {
  return new Promise((res, rej) => {
    const t = setTimeout(() => rej(new Error("timeout")), ms);
    p.then(v => { clearTimeout(t); res(v); },
           e => { clearTimeout(t); rej(e); });
  });
}

// ---- Formatters -------------------------------------------------------
function fmt(v, d)    { return isFinite(v) ? Number(v).toFixed(d) : "--"; }
function fmtAbs(v, d) { return isFinite(v) ? Math.abs(Number(v)).toFixed(d) : "--"; }

function fmtInches(v, decimals = 4) {
  let x = Number(v);
  if (!isFinite(x)) return "--";
  const sign = x < 0 ? "-" : "";
  x = Math.abs(x);

  // Inch displays should always land on a value whose last decimal digit is
  // 0 or 5.  That means nearest 0.0005" on the big/long scale (4 places)
  // and nearest 0.00005" on the small/short scale (5 places).
  const scale = Math.pow(10, decimals);
  const steps = Math.round(x * scale / 5);
  const whole = Math.floor((steps * 5) / scale);
  const frac  = (steps * 5) % scale;
  return sign + whole + "." + String(frac).padStart(decimals, "0");
}

function fmtInchesAbs(v, decimals = 4) {
  let x = Math.abs(Number(v));
  return fmtInches(x, decimals);
}

function fmtAbsPercent(v) {
  const x = Math.abs(Number(v));
  return isFinite(x) ? x.toFixed(2) + "%" : "--";
}

function clamp01(v) {
  const x = Number(v);
  return isFinite(x) ? Math.max(0, Math.min(1, x)) : 0;
}

function targetDec() { return currentUnit === "in" ? 4 : 2; }
function fmtTarget(v) {
  const x = Number(v);
  return (isFinite(x) && Math.abs(x) > 1e-7) ? x.toFixed(targetDec()) : "";
}

function copyVal(id) {
  const t = document.createElement("textarea");
  t.value = document.getElementById(id).textContent;
  t.style.cssText = "position:fixed;left:-9999px";
  document.body.appendChild(t);
  t.focus(); t.select();
  try { document.execCommand("copy"); } catch(e) {}
  document.body.removeChild(t);
}

// ---- UI update ---------------------------------------------------------
// Track previous running state so we can detect the RUNNING → HELD
// transition and fire the done sound exactly once per completed test.
let prevRunning = false;
let smallScale   = false;

function applyData(d) {
  // Fire chime the moment the test locks into HELD.
  // Guard with prevRunning so a browser reconnect after an already-held
  // test doesn't re-trigger the sound.
  if (d.test_held && prevRunning) playDoneSound();
  prevRunning = d.test_running;

  // Scale type selector
  smallScale = d.small_scale;
  document.getElementById("scaleSmBox").style.opacity = smallScale ? "1"   : "0.3";
  document.getElementById("scaleLgBox").style.opacity  = smallScale ? "0.3" : "1";
  document.getElementById("scaleLabel").textContent    = smallScale ? "Small \u00b7 0.001mm" : "Big \u00b7 0.01mm";
  currentUnit = d.unit;

  const running = d.test_running;
  const held    = d.test_held;
  const active  = running || held;

  // Status pill
  const pill = document.getElementById("status");
  pill.className = running ? "pill running" : "pill";
  pill.textContent = !d.haveReading ? "waiting for scale"
                   : running        ? "TEST RUNNING"
                   : held           ? "result held"
                   : "live";

  // Cards
  document.getElementById("testCard").className =
    running ? "card testCard runningCard" : "card testCard";
  document.getElementById("positionCard").className =
    active ? "card positionCard simplified" : "card positionCard";

  // Test state badge + action button
  const btn = document.getElementById("actionBtn");
  if (running) {
    document.getElementById("testState").textContent = "Running";
    btn.textContent = "Stop";
    btn.className   = "red actionBtn";
    btn.dataset.mode = "stop";
  } else if (held) {
    document.getElementById("testState").textContent = "Complete";
    btn.textContent = "Start";
    btn.className   = "green actionBtn";
    btn.dataset.mode = "start";
  } else {
    document.getElementById("testState").textContent = "Idle";
    btn.textContent = "Start";
    btn.className   = "green actionBtn";
    btn.dataset.mode = "start";
  }

  // Progress bar
  document.getElementById("testProgressFill").style.width =
    fmt(clamp01(d.test_progress) * 100, 1) + "%";

  // Duration selector
  document.getElementById("duration").value = String(d.duration_min);

  // Target speed input (don't overwrite while user is typing)
  const ti = document.getElementById("targetSpeed");
  if (document.activeElement !== ti) {
    ti.value = d.has_target_speed
      ? fmtTarget(d.unit === "in" ? d.target_speed_in_min : d.target_speed_mm_min)
      : "";
  }

  // Percent error
  const pe = document.getElementById("percentError");
  if (d.has_target_speed && active) {
    pe.textContent = fmtAbsPercent(d.test_percent_error);
    pe.className   = "inlineErrorValue";
  } else {
    pe.textContent = "--";
    pe.className   = "inlineErrorValue missing";
  }

  // Live position (left card)
  if (d.unit === "in") {
    document.getElementById("pos").textContent          = fmtInches(d.rel_in, smallScale ? 5 : 4);
    document.getElementById("unit").textContent         = "in";
    document.getElementById("liveSpeed").textContent    = fmt(d.live_speed_in_min, 4);
    document.getElementById("liveSpeedUnit").textContent = "in/min";
  } else {
    document.getElementById("pos").textContent          = fmt(d.rel_mm, smallScale ? 3 : 2);
    document.getElementById("unit").textContent         = "mm";
    document.getElementById("liveSpeed").textContent    = fmt(d.live_speed_mm_min, 2);
    document.getElementById("liveSpeedUnit").textContent = "mm/min";
  }

  // Test result panel — show "--" in idle, real values during/after
  if (!active) {
    document.getElementById("resultSpeed").textContent = "--";
    document.getElementById("testDist").textContent    = "--";
    document.getElementById("testTime").textContent    = "--";
  } else if (d.unit === "in") {
    document.getElementById("resultSpeed").textContent = fmtAbs(d.test_speed_in_min, 4);
    document.getElementById("resultUnit").textContent  = "in/min";
    document.getElementById("testDist").textContent    = fmtInchesAbs(d.test_distance_in, smallScale ? 5 : 4);
    document.getElementById("testDistUnit").textContent = "in";
    // Elapsed: 4 decimal places so the frame-window value is distinguishable
    // from a round configured duration (e.g. 59.9872 s vs 60.0000 s)
    document.getElementById("testTime").textContent    = fmt(d.test_elapsed_s, 4);
  } else {
    document.getElementById("resultSpeed").textContent = fmtAbs(d.test_speed_mm_min, 3);
    document.getElementById("resultUnit").textContent  = "mm/min";
    document.getElementById("testDist").textContent    = fmtAbs(d.test_distance_mm, smallScale ? 3 : 2);
    document.getElementById("testDistUnit").textContent = "mm";
    document.getElementById("testTime").textContent    = fmt(d.test_elapsed_s, 4);
  }
}

// ---- Poll / command ----------------------------------------------------
async function poll() {
  if (busy) return;
  busy = true;
  try {
    const r = await withTimeout(fetch("/api", { cache: "no-store" }));
    applyData(await r.json());
  } catch(e) {
    document.getElementById("status").textContent = "disconnected";
  } finally {
    busy = false;
  }
}

async function cmd(path) {
  ensureAudio();   // keep AudioContext warm on every user gesture
  if (busy) return;
  busy = true;
  try {
    await withTimeout(fetch(path, { method: "POST" }));
  } catch(e) { /* will recover on next poll */ }
  busy = false;
  await poll();
}

async function toggleTest() {
  const mode = document.getElementById("actionBtn").dataset.mode;
  await cmd(mode === "stop" ? "/test?cmd=stop" : "/test?cmd=start");
}


async function toggleScale() {
  ensureAudio();
  await cmd('/scale?s=' + (smallScale ? '0' : '1'));
}

async function toggleUnits() {
  await cmd("/units?u=" + (currentUnit === "in" ? "mm" : "in"));
}

async function setDuration() {
  await cmd("/duration?min=" + document.getElementById("duration").value);
}

async function setTargetSpeed() {
  const input = document.getElementById("targetSpeed");
  let v = parseFloat(input.value);
  if (!isFinite(v) || v < 0) v = 0;
  await cmd("/target?v=" + encodeURIComponent(v) + "&u=" + encodeURIComponent(currentUnit));
}

// ---- Keyboard shortcut ----------------------------------------
// Alt+S (Windows/Linux) or Option+S (Mac) — start or stop the test.
// Uses event.code (physical key position) so it works on all keyboard
// layouts regardless of what character Option/Alt+S produces.
document.addEventListener("keydown", e => {
  if (e.altKey && e.code === "KeyS") {
    e.preventDefault();
    ensureAudio();   // counts as a user gesture — keeps audio context warm
    toggleTest();
  }
});

setInterval(poll, 200);
poll();
</script>
</body>
</html>
)rawliteral";

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

// ==================== HTTP handlers ====================
void handleScale() {
  if (server.hasArg("s")) smallScale = (server.arg("s") == "1");
  server.send(200, "text/plain", "OK");
}

void handleRoot()  { server.send_P(200, "text/html", INDEX_HTML); }
void handleApi()   { server.send(200, "application/json", makeApiJson()); }

void handleZero()  { cmdZero(); server.send(200, "text/plain", "OK"); }

void handleUnits() {
  if (server.hasArg("u")) {
    showInches = (server.arg("u") == "in");
  }
  server.send(200, "text/plain", "OK");
}

void handleDuration() {
  if (server.hasArg("min")) {
    int m = server.arg("min").toInt();
    if (m < 1)  m = 1;
    if (m > 10) m = 10;
    testDurationMin = (uint8_t)m;
  }
  server.send(200, "text/plain", "OK");
}

void handleTarget() {
  if (server.hasArg("v")) {
    float v = server.arg("v").toFloat();
    if (v < 0.0f) v = 0.0f;
    String u = server.hasArg("u") ? server.arg("u") : (showInches ? "in" : "mm");
    targetSpeedMmMin = (u == "in") ? v * 25.4f : v;
  }
  server.send(200, "text/plain", "OK");
}

void handleTest() {
  if (server.hasArg("cmd")) {
    String c = server.arg("cmd");
    if (c == "start") cmdStart();
    if (c == "stop")  cmdStop();
    if (c == "reset") cmdReset();
  }
  server.send(200, "text/plain", "OK");
}

// ==================== Setup / Loop ====================
void setupWeb() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/api",     HTTP_GET,  handleApi);
  server.on("/zero",    HTTP_GET,  handleZero);
  server.on("/zero",    HTTP_POST, handleZero);
  server.on("/units",   HTTP_GET,  handleUnits);
  server.on("/units",   HTTP_POST, handleUnits);
  server.on("/duration",HTTP_GET,  handleDuration);
  server.on("/duration",HTTP_POST, handleDuration);
  server.on("/target",  HTTP_GET,  handleTarget);
  server.on("/target",  HTTP_POST, handleTarget);
  server.on("/test",    HTTP_GET,  handleTest);
  server.on("/test",    HTTP_POST, handleTest);
  server.on("/scale",   HTTP_GET,  handleScale);
  server.on("/scale",   HTTP_POST, handleScale);

  server.begin();
}

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

  setupWeb();

  Serial.println("\nDRO Speed Test — Mitutoyo SPC edition");
  Serial.println("Wi-Fi SSID: " + String(AP_SSID));
  Serial.println("Mitutoyo: DATA=D3  CLOCK=D4  REQ=D5(MOSFET)");
  Serial.println("OUTPUT:   START/STOP=D6  RESET=D7");
}

void loop() {
  server.handleClient();
  serviceOutputs();
  serviceAutoStop();

  // Poll Mitutoyo at ~8 Hz.  readMitoFrame() is blocking (~few ms when
  // scale is live; up to ~200 ms worst-case if scale is unresponsive).
  static uint32_t lastFrameMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastFrameMs >= 125) {    // 8 Hz
    lastFrameMs = nowMs;
    if (readMitoFrame()) {
      long val = decodeMitoMm1000();
      if (val != LONG_MIN) onFrame(val);
    }
  }
}
