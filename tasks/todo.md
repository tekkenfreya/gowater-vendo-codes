# Add Sales Retrieval SMS to Water_Vendo_Tonsuya.ino

## Plan
- [ ] Add `sendSalesSMS()` function to send sales report via SMS
- [ ] Update `processSMS()` to detect "doit" command and call `sendSalesSMS()`

---

# Water Flow Sensor Implementation Plan

## Overview
Replace time-based water dispensing with accurate pulse-counting using the YF-S201 flow sensor.

**Flow Sensor Specifications:**
- **450 pulses = 1 liter of water** ⭐ Key metric
- Frequency: F = 7.5 × Q(L/min)
- Flow range: 1-30 L/min
- Operating voltage: DC 3.5-24V
- Pin: 27 (already defined as `flowSensor`)

## Current State Analysis
- ✅ Flow sensor pin defined on line 10: `#define flowSensor 27`
- ❌ Pin is NOT being used in the code
- ❌ Dispensing uses time-based delays from `durations[]` array
- Product sizes: 1L, 5L, 19L
- Current durations: {0, 5500, 29500, 190000, 175000} ms

## Implementation Tasks

### [ ] 1. Add Flow Sensor Variables (after line 26)
- `volatile unsigned long pulseCount = 0;` - ISR pulse counter
- `const int targetPulses[] = {0, 450, 2250, 8550};` - 1L, 5L, 19L targets
- `volatile unsigned long lastFlowPulse = 0;` - Debounce timing

### [ ] 2. Create Flow Sensor ISR
- Add `void IRAM_ATTR flowISR()` function
- Count pulses on RISING edge
- Add 10ms debounce to prevent false counts
- Place near coinISR() function (after line 75)

### [ ] 3. Configure Flow Sensor in setup()
- Set `pinMode(flowSensor, INPUT_PULLUP);`
- Attach interrupt: `attachInterrupt(digitalPinToInterrupt(flowSensor), flowISR, RISING);`
- Add in setup() after coinslot interrupt (after line 141)

### [ ] 4. Update cashPay() Function
- Replace `delay(durations[address[1]]);` on line 273
- Reset pulseCount before dispensing
- Count pulses during dispensing
- Stop when targetPulses reached
- Add timeout safety (use durations as max time)

### [ ] 5. Update gcashPay() Function
- Replace `delay(durations[address[1]]);` on line 309
- Same pulse-counting logic as cashPay
- Reset counter, count pulses, stop at target

### [ ] 6. Add Safety Features
- Maximum timeout (fallback if sensor fails)
- Pulse validation (ensure pulses are incrementing)
- Reset pulse counter before each dispense
- Stop if no pulses detected for 5 seconds (pump issue)

## Implementation Details

### Pulse Calculations
- Product 1 (1L): 450 pulses
- Product 2 (5L): 2,250 pulses
- Product 3 (19L): 8,550 pulses

### Dispensing Logic (Pseudo-code)
```cpp
pulseCount = 0;
digitalWrite(relay, HIGH);
unsigned long startTime = millis();
while (pulseCount < targetPulses[product]) {
  // Safety timeout
  if (millis() - startTime > durations[product]) break;
  // Check for stalled flow
  if (pulseCount > 0 && millis() - lastFlowPulse > 5000) break;
  delay(10);
}
digitalWrite(relay, LOW);
```

## Testing Checklist
- [ ] Test 1L dispense (should be ~450 pulses)
- [ ] Test 5L dispense (should be ~2250 pulses)
- [ ] Test 19L dispense (should be ~8550 pulses)
- [ ] Test sensor failure fallback (disconnect sensor)
- [ ] Verify accurate volume with measuring container

## Notes
- Keep existing code structure - minimal changes only
- Use interrupt-based counting for accuracy
- Keep `durations[]` array as safety timeout
- Flow sensor MUST be installed in-line with water flow
- Sensor outputs pulses when water flows through it

---

# Add Coinslot to Water_Vendo_Tonsuya.ino

## Plan

### [ ] 1. Update pin definitions
- Change `#define relay 25` → `#define relay 26` (pin 25 is now slotActivator)
- Add `#define coinslot 23`
- Add `#define slotActivator 25`

### [ ] 2. Add coin-related global variables
- `double currency = 0.00;`
- `volatile bool coinInserted = false;`
- `volatile unsigned long lastDebounceTime = 0;`
- `const unsigned long coinDebounceDelay = 70;`
- `volatile int coinCount = 0;`

### [ ] 3. Add product prices array
- `const double prices[] = {0.00, 5.00, 10.00, 20.00, 20.00};`

### [ ] 4. Add coinISR() function (copied from vigan)

### [ ] 5. Update setup()
- Add `pinMode(coinslot, INPUT_PULLUP);`
- Add `attachInterrupt(digitalPinToInterrupt(coinslot), coinISR, FALLING);`
- Add `pinMode(slotActivator, OUTPUT);`

### [ ] 6. Add cashPay() function
- Time-based dispensing (using existing `durations[]` array)
- Coin accumulation loop with slotActivator control
- Cancel support via address[0]

### [ ] 7. Update loop()
- Add `address[4] = prices[address[1]];` to show price
- Check `address[2] == 1` for cash payment, call `cashPay()`
- Reset `currency = 0;` in `resetRegisters()`

---

# Editable Prices via Local Web Admin (water_vendo_vigan.ino) — FINAL

## Goal
Operator changes product prices on the vendo without Arduino IDE / reflashing.
Edit via a password-protected page on the vendo's own WiFi hotspot. Prices survive reboot via NVS.

## Final Design (confirmed)
- **WiFi mode:** ESP runs as **AP** permanently. SSID `GoWater-Vendo`. Operator joins that WiFi to access admin.
- **Single password** stored in NVS — protects the WiFi AP *and* the admin HTTP basic auth login. Default `gowater2026` on first boot. Changeable from admin page.
- **NVS namespace** `"config"` (separate from `"sales"`). Keys: `p1,p2,p3,p4,adminpw`.
- **All 4 prices** editable (`p1..p4`).
- **Web server runs only when idle** (`server.handleClient()` gated on `address[0] == 0`).
- **No new libraries** — uses built-in `WiFi.h` + `WebServer.h`.

## Implementation Tasks

### [x] 1. Make `prices[]` mutable + add NVS load/save
### [x] 2. Add WiFi AP + WebServer
### [x] 3. Admin routes (`/`, `/save`, `/changepw`, 404)
### [x] 4. Wire into `loop()` (gated on `address[0] == 0`)
### [x] 5. Push prices to HMI holding registers (in cents)
- New helper `updatePriceAddresses()`; called from `loadConfig()` (boot) and `handleSave()` (web edit).
- Web validation cap lowered from 10000 → 650 PHP (so cents fit in uint16_t).
- Mapping verified against `Vendo Vigan.skr` (decoded from zlib SKTOOL project):
  - p1 (1L)  → `address[9]`  (`4x0009`)
  - p2 (5L)  → `address[10]` (`4x0010`)
  - p3 (20L) → `address[11]` (`4x0011`)
  - p4 (19L) → `address[14]` (`4x0014`)
  - Skipped `4x0012` ("waterSensor" in HMI Address Library) and `4x0013` (zeroed in setup).

### [x] 6. Make volume labels + target dispense volumes editable from web admin
- Dropped `const` on `litersLabel[]` and `targetVolumes[]`.
- NVS keys: `l1..l4` (int liters), `t1..t4` (float target volume), in same `"config"` namespace.
- New helper `updateLabelAddresses()` writes integer labels to `address[6, 15, 20, 23]`.
- Web admin form now has Price + Volume Label + Target Volume per product.
- `handleSave()` validates and persists all three.
- Existing dispense (`targetVolumes[address[1]]`) and sales SMS (`litersLabel[i]`) automatically use new values.

### [ ] 7. HMI side — bind dynamic widgets (in SKTOOL, on EVERY page that shows them)
Decoded from `Vendo Vigan.skr`: prices and volume labels appear as **static text on 3 pages** (likely Sales, Order, Order2). Replace each static text with a Number-display widget bound to:

| Field | Register | Format |
|-------|----------|--------|
| 1L price | `4x0009` | 16-bit unsigned, **decimal=2** |
| 5L price | `4x0010` | 16-bit unsigned, **decimal=2** |
| 19L price | `4x0011` | 16-bit unsigned, **decimal=2** |
| 1L volume label | `4x0006` | 16-bit unsigned, decimal=0 |
| 5L volume label | `4x0015` | 16-bit unsigned, decimal=0 |
| 19L volume label | `4x0020` | 16-bit unsigned, decimal=0 |

(Add a separate static "L" or "Liter" suffix next to each volume number.)

Save / compile / upload to HMI.

### [ ] 8. Smoke test on hardware (your turn)
- Power on → phone joins `GoWater-Vendo` (pass `gowater2026`) → open `http://192.168.4.1` → log in (`admin` / `gowater2026`).
- Change a price → confirm HMI updates within ~1s (no reboot needed).
- Change a volume label from 5 → 10 AND target volume from 5.0 → 10.0 → confirm HMI shows "10" and machine actually pours ~10L on next purchase.
- Reboot → confirm everything persists.
- Change password → reconnect with new password → confirm.

## Constraints / Notes
- Only `water_vendo_vigan.ino` is touched.
- NVS namespace `"sales"` is untouched. New namespace `"config"`.
- No changes to HMI register layout, dispense logic, SMS code.
- Web server is dormant during transactions to avoid SoftwareSerial/GSM timing glitches.
