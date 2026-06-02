# DRO Speed Test — Mitutoyo SPC Edition

<img width="2526" height="1392" alt="image" src="https://github.com/user-attachments/assets/7ebda552-5850-4c2e-b267-479db627e364" />

ESP32-based crosshead speed calibration system for tensile testers. Reads a Mitutoyo Digimatic linear scale over the SPC protocol, computes average crosshead speed over a timed test window, and streams live data to a browser via USB Web Serial. Built to meet **ASTM E2658** Class A accuracy requirements (±0.5% speed, ±5.0% repeatability) without commercial DAQ hardware.

> **Current release:** Inch mode (gauge set to inches).  
> **Coming soon:** MM mode support, full run log with CSV export.

---

## What It Does

- Displays live crosshead position and speed in real time
- Runs a timed speed test (1–10 minutes, configurable)
- Reports final average speed, total displacement, and actual elapsed time at test end
- Computes percent error against a target speed
- Fires 150 ms trigger pulses to a tensile tester's START/STOP and RESET inputs
- Plays a done chime when the test completes
- Alt+S keyboard shortcut to start/stop

Results are computed entirely on the ESP32 from matched frame-timestamp pairs, so accuracy is independent of USB latency or browser timing.

---

## Hardware

| Part | Notes |
|---|---|
| Seeed Studio XIAO ESP32-C3 | Main controller |
| Mitutoyo Digimatic linear scale | SPC output, set to **inch mode** |
| VN2222LL NPN MOSFET | Pulls Mitutoyo REQ line low |
| 10 kΩ resistor × 2 | Pullups on DATA and CLOCK to 3.3 V |

### Wiring

```
Mitutoyo SPC connector
  DATA  ──── 10k ──── 3V3
  DATA  ──────────── D3 / GPIO5   (ESP32 input)

  CLOCK ──── 10k ──── 3V3
  CLOCK ──────────── D4 / GPIO6   (ESP32 input)

  REQ   ──── DRAIN of VN2222LL
             SOURCE → GND
             GATE  ─── D5 / GPIO7  (ESP32 output)
             (ESP32 HIGH → MOSFET on → pulls REQ low → scale sends data)

Trigger outputs (to tensile tester)
  D6 / GPIO21  →  START/STOP input
  D7 / GPIO20  →  RESET input
```

---

## Software Setup

1. Install **Arduino IDE** and add the ESP32-C3 board package
2. Open `DRO_WebSerial_ESP32.ino` and upload to the XIAO ESP32-C3
3. Open `DRO_WebSerial_GUI.html` in **Chrome or Edge** (Web Serial API required — Firefox not supported)
4. Click **Connect USB**, select the ESP32 serial port
5. Set your Mitutoyo gauge to **inch mode** before connecting

No Wi-Fi, no server, no install. The HTML file runs directly from disk.

---

## How It Works

### Frame-Timestamp Architecture

Speed measurement accuracy depends entirely on one principle: **the position measurement and the time measurement must come from the same physical event.**

A naive approach — polling the scale every N milliseconds and dividing distance by N — accumulates error from the mismatch between command time and scale update time. This system avoids that entirely.

Every Mitutoyo frame read produces a matched pair:

```
(position, timestamp) = (frameMm1000, frameUs)
```

`frameUs` is captured by `esp_timer_get_time()` as the very first line of `onFrame()`, before any computation. At test start and test end, both values from the same pair are latched:

```
start: (startFrameMm1000, startFrameUs)
stop:  (endFrameMm1000,   endFrameUs)

speed = (endPos − startPos) / (endTime − startTime)
```

Because numerator and denominator come from the same two frame events, any systematic lag in the reading pipeline cancels exactly. At constant crosshead velocity this gives zero systematic error regardless of frame rate. The time base is the ESP32's crystal-backed `esp_timer` at ±20–50 ppm.

This is the same architecture used in the iGaging version of this tool, validated against NVLAP-accredited Instron and MTS calibration systems.

---

### Mitutoyo SPC Protocol (Inch Mode)

The Mitutoyo Digimatic SPC interface is a simple synchronous serial protocol. The gauge does not transmit continuously — it waits for the controller to assert the REQ line, then clocks out one 52-bit frame.

#### REQ Assertion

The ESP32 cannot drive the Mitutoyo REQ line directly because the gauge operates at 5 V logic while the XIAO is 3.3 V. A **VN2222LL NPN MOSFET** solves this: ESP32 GPIO HIGH turns the MOSFET on, which pulls the gauge's REQ line to GND (active low). GPIO LOW releases it.

```cpp
requestOn()  → digitalWrite(PIN_MITO_REQ, HIGH)  // MOSFET on → REQ pulled low
requestOff() → digitalWrite(PIN_MITO_REQ, LOW)   // MOSFET off → REQ released
```

#### Frame Structure

After REQ is asserted, the gauge clocks out **13 nibbles (4 bits each = 52 bits total)** on the CLOCK and DATA lines. Data is sampled after each falling clock edge.

```
Nibble  Field           Value (this instrument, inch mode)
──────  ──────────────  ────────────────────────────────────
 0–3    Header          0x0F, 0x0F, 0x0F, 0x0F  (always)
  4     Sign            0x0 = positive,  0x8 = negative
 5–10   BCD digits      6 decimal digits, MSB first
 11     Format ID       0x05  (inch mode, this instrument)
 12     Format ID       0x01  (inch mode, this instrument)
```

All four nibble validation checks must pass before a frame is accepted. A single failed check discards the whole frame — the previous good reading is kept on screen and the measurement window is unaffected.

#### Decode (Inch Mode)

The six BCD digit nibbles encode the reading as an integer. For a gauge at 0.00001 inch resolution with decimal position 5:

```
raw = nib[5]×100000 + nib[6]×10000 + nib[7]×1000
    + nib[8]×100   + nib[9]×10    + nib[10]×1

raw / 100000 = inches
```

Example: reading 0.1616 in → nibbles 5–10 = {0,1,6,1,6,0} → raw = 16160 → 16160 / 100000 = 0.1616 in ✓

Internally everything is stored in **mm × 1000** (integer, no float rounding):

```
mm×1000 = (raw × 254 + 500) / 1000
```

The `×254/1000` is exact integer arithmetic for `×25.4/100` (converting inches to mm, then scaling to mm×1000). The `+500` gives correct rounding. Integer overflow is impossible for this gauge: max raw = 999,999 → 999,999 × 254 + 500 = 254,000,246, well within int32.

#### Why Inch Mode Only (For Now)

In mm mode, nibbles 11 and 12 take different values than in inch mode. The current firmware validates against the confirmed inch-mode identifiers (0x05, 0x01). MM mode support — with its own nibble identifiers and a different decimal position — is planned for the next release. Keep the gauge set to inches.

---

## Timing Details

| Parameter | Value |
|---|---|
| Frame rate (polled) | ~8 Hz (125 ms gate) |
| Mitutoyo frame duration | ~88–94 ms |
| Time base | `esp_timer_get_time()`, crystal-backed, ±20–50 ppm |
| Scale staleness threshold | 5 seconds (≈ 40 missed frames) |
| Status JSON rate | 200 ms over USB Serial at 115200 baud |
| Trigger pulse width | 150 ms |

The auto-stop fires on a frame boundary, so the reported elapsed time is always slightly more than the configured duration (typically 50–100 ms over). This is intentional and correct — the displayed elapsed time is the actual measurement window used in the speed calculation, matching what your reference timer (e.g. Laurel L7041) would show.

---

## ASTM E2658 Notes

ASTM E2658 defines crosshead speed as average displacement divided by elapsed time over the test interval. This is exactly what this system computes. Class A requires ±0.5% speed accuracy and ±5.0% repeatability. In testing against NVLAP-accredited reference systems:

- Average speed error: **< 0.05%**
- Run-to-run repeatability: **< 0.06% std dev**

This places the system at the same accuracy level as commercial Instron and MTS crosshead speed calibrators.

---

## Serial Commands

The GUI sends these automatically, but they can also be typed in any serial terminal at 115200 baud.

| Command | Action |
|---|---|
| `START` | Start a test (scale must be live) |
| `STOP` | Stop and latch the result |
| `RESET` | Clear result and return to idle |
| `ZERO` | Zero the position display |
| `UNIT in` / `UNIT mm` | Switch display units |
| `SCALE 0` / `SCALE 1` | Big scale (0.01 mm) / Small scale (0.001 mm) |
| `DURATION N` | Set test duration in minutes (1–10) |
| `TARGET V in` / `TARGET V mm` | Set target speed for percent error display |

---

## Roadmap

- [ ] **MM mode** — detect inch vs mm from nibble identifiers, convert automatically
- [ ] **Run log** — persistent table of all completed runs with speed, distance, elapsed, and percent error; CSV export for ASTM E2658 documentation

---

## References

- ASTM E2658 — *Standard Practices for Verification of Speed for Universal Testing Machines*
- Mitutoyo Digimatic / SPC output specification
- sspence SPC Arduino decoder (original protocol reference)
- [Web Serial API](https://developer.chrome.com/docs/capabilities/serial) — Chrome / Edge only
