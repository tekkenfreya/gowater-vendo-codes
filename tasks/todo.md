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

---

# Make PULSES_PER_LITER editable from HMI (water_vendo_vigan.ino)

## Goal
Operator changes flow calibration (pulses per liter) from a spare HMI **numeric input**
bound to the 4x register mapping to firmware slot `address[24]`. No reflash. Survives reboot via NVS.
User does the HMI side; ESP32 side only here. Minimal changes.

## Tasks
- [x] 1. Drop `const` on `PULSES_PER_LITER` (keep the name → all 6 usages unchanged)
- [x] 2. Add `savePulsesPerLiter()` NVS helper (namespace `"config"`, key `ppl`)
- [x] 3. `loadConfig()`: load saved value + push into `address[24]` so HMI shows current calibration on boot
- [x] 4. Add `syncPulsesPerLiter()` — apply HMI-entered value, persist; ignore 0 (guards calibration + divide-by-zero)
- [x] 5. Call `syncPulsesPerLiter()` in the `HMICon` poll loop

## Notes
- Value is integer `uint16_t` (450 fits). `!= 0` guard so idle/boot never zeroes calibration.
- `regs` unchanged — slot 24 was the only free index.

---

# Admin free-dispense mode + 1 reserved slot (water_vendo_vigan.ino)

## Goal
Expand register array to add 2 slots. address[26] = admin free-dispense toggle
(HMI: 1=on, 0=normal). When on, choosing a product dispenses free (no coins/GCash).
address[27] = reserved for the next feature (spec pending). User does HMI side.

## Tasks
- [x] 1. Bump `regs` 26 -> 28 (slots 26, 27)
- [x] 2. Default `address[26] = 0` in setup() (admin always OFF on boot — safety)
- [x] 3. Dispatch: if `address[26]==1`, pre-credit `currency = prices[...]` so cashPay/gcashPay dispense immediately
- [x] 4. Skip `recordSale()` when admin mode on (free pour not logged as a sale — keeps revenue accurate)
- [ ] 5. address[27] feature — WAITING on user spec

## Notes
- One pre-credit covers both cash + GCash (both dispense once `currency >= price`).
- `resetRegisters()` only clears 0-6, so admin toggle persists until HMI sets it back to 0.
- HMI must still trigger the transaction (address[0]=1, address[2]=1 or 2) for the dispense to run.

---

# Water level sensor: pause dispense + "Refilling..." (water_vendo_v3.ino)

## Wiring (new setup, confirmed 18 Aug 2026)
GSM 16/17 · RS232/HMI 18/19 · coinslot 23 · slot relay 25 · booster relay 26 ·
flow 27 · **water level LOW = 32, HIGH = 33** (new). Code pins already match except 32/33.

## Behaviour (user spec, REVISED 18 Aug after bench test)
- Pins 32/33 are water INDICATORS: read HIGH when on (= no water at that level).
- LOW indicator on -> `refilling` = true -> HMI shows "Refilling..." (address[27]=1)
- While refilling: running transaction is ABORTED (pump off, resetRegisters, no
  sale), coin wait / payment wait exit, no new transaction can start.
- HIGH indicator off -> `refilling` = false -> back to normal, customer triggers again.
- (First version paused-and-resumed the pour; that froze the HMI flow -> replaced.)

## Tasks
- [x] 1. `#define levelLow 32`, `#define levelHigh 33`; `pinMode(..., INPUT_PULLUP)` in setup()
- [x] 2. `#define LEVEL_DRY LOW` — one-line flip if float switches read inverted on hardware
- [x] 3. `bool refilling` global + `checkTankLevel()`:
       LOW dry -> refilling=true; HIGH wet -> refilling=false; mirror to `address[27]` (1 = refilling)
- [x] 4. `unsigned long waitForRefill()` — relay LOW, loop until !refilling (calls checkTankLevel, delay 100), relay HIGH; returns paused ms
- [x] 5. Call at top of the dispense `while` in cashPay() AND gcashPay():
       if refilling -> pause; add paused ms to `dispenseStartTime` + reset `lastPulseTime` so stall / no-water timeouts don't fire after resume
- [x] 6. Call `checkTankLevel()` once per `loop()` so HMI shows "Refilling..." while idle too
- [ ] 7. HMI side (user): bind `4x0027` — show "Refilling..." when value == 1

## Notes
- address[27] was the reserved slot -> now the tank status flag. `regs = 28` already.
- Uses HMICon task for polling, so HMI keeps updating during the wait.
- Coin insertion is untouched: paying still works while low; pour just waits for refill.
- Not doing: blocking a NEW transaction start while low (say if you want that).

---

# Code scan fixes (water_vendo_v3.ino) — 18 Aug 2026

- [x] 1. REMOVED PayMongo SMS crediting (`extractAmount`, `testExtractAmount`, PAYMONGO branch). GCash credit will come from an API method later; `gcashPay()` kept as the placeholder.
- [x] 2. Cancelled / failed transaction no longer recorded as a sale — `cashPay()` / `gcashPay()` now return `bool dispensed`; `loop()` records only when true.
- [x] 3. `clearSales()` restores the 19L volume label (`address[20]`) after zeroing sales regs.
- [ ] 4. Ghost credit — moot after #1 (no SMS credit source). Skipped.
- [x] 5. Overpay shows 65535 on HMI — added `remainingBalance()` helper (clamps at 0), used at both `address[4]` sites in `cashPay()`.
- [x] 6. Bounds-check `address[1]` at transaction start in `loop()` — invalid index → `resetRegisters()`, return.
- [x] 7. `syncPulsesPerLiter()` moved from HMICon task to `loop()` (idle only) — single-task NVS access.
- [ ] 8. GSM → HardwareSerial `Serial2` — DEFERRED (no benefit until GSM carries payments/API).

> **18 Aug 2026:** all of the above (pin remap, editable PPL, admin mode, scan fixes,
> tank level) now lives in **`water_vendo_v3.ino`** — the NEW vendo build.
> `water_vendo_vigan.ino` was restored to the committed version (old vendo, HMI on 16/17).

> **18 Aug bench (ESP32 on USB, flashed from arduino-cli):** built with **ESP32 core 2.0.17**
> (3.3.11 gave the "no WiFi light" report; unverified whether 3.x was actually the cause — the
> 2.0.17 build boots with `AP started: GoWater-Vendo @ 192.168.4.1`). `LEVEL_DRY` is **LOW**:
> indicators pull the pin low when on; HIGH blocked the machine at boot with pins idle.
> GSM module not answering on the bench (no SIM / not wired) — expected.
> Flash: `arduino-cli upload -p /dev/cu.usbserial-0001 --fqbn esp32:esp32:esp32 water_vendo_v3`
> or the GoWater Flasher app.
