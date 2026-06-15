#include <ModbusRTUSlave.h>
#include <SoftwareSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>

#define TX 16
#define RX 17
#define relay 26
#define coinslot 23
#define slotActivator 25
#define flowSensor 27
#define SIMRX 19
#define SIMTX 18

const int regs = 26;
uint16_t address[regs];

// ============ PRODUCT CONFIG ============
// Prices, volume labels, and target volumes are all runtime-editable via the web
// admin (persisted in NVS namespace "config"). The values below are first-boot
// defaults only; NVS overrides them on subsequent boots.
const int NUM_PRODUCTS = 4;
double prices[NUM_PRODUCTS + 1] = {0.00, 5.00, 10.00, 20.00, 20.00};
float targetVolumes[NUM_PRODUCTS + 1] = {0.0, 1.0, 5.0, 20.6, 19.4};
int litersLabel[NUM_PRODUCTS + 1] = {0, 1, 5, 19, 19};
// ========================================

// ============ WEB ADMIN CONFIG ============
const char *AP_SSID = "GoWater-Vendo";
const char *DEFAULT_ADMIN_PASSWORD = "gowater2026";
String adminPassword = DEFAULT_ADMIN_PASSWORD;
WebServer server(80);
// ==========================================

ModbusRTUSlave hmi(Serial1);
SoftwareSerial gsm(SIMRX, SIMTX);
Preferences prefs;

String senderNumber = "";
double currency = 0.00;

// Authorized admin numbers for SMS commands (sales/reset)
const String ADMIN_NUMBER_1 = "+639683597760";
const String ADMIN_NUMBER_2 = ""; // TBA — fill in when available

// Flow sensor variables
volatile unsigned long pulseCount = 0;
volatile unsigned long lastFlowPulse = 0;

// CALIBRATION: measured 300 pulses per liter
const float PULSES_PER_LITER = 450.0;

// Debounce delay - reduce if missing pulses at high flow rates (try 1-5ms)
const unsigned long flowDebounceDelay = 2;

const unsigned long smsIntervalSecs = 60;
unsigned long smsCheckTimer = 0;
unsigned long lastSMSCheck = 0;

int saleCounts[NUM_PRODUCTS + 1] = {0}; // index 1 to NUM_PRODUCTS
int totalLiters = 0, totalRevenue = 0, totalTransactions = 0;

void updateSalesAddresses()
{
  totalTransactions = 0;
  totalLiters = 0;
  totalRevenue = 0;
  for (int i = 1; i <= NUM_PRODUCTS; i++)
  {
    totalTransactions += saleCounts[i];
    totalLiters += saleCounts[i] * litersLabel[i];
    totalRevenue += (int)(saleCounts[i] * prices[i]);
    address[15 + i] = saleCounts[i];
  }
  // Combine 20L (idx 3) + 19L (idx 4) sales into address[18] for HMI
  address[18] = saleCounts[3] + saleCounts[4];
  // HMI expects totals at 19/20/21 (3-product layout)
  address[19] = totalLiters;
  address[21] = totalRevenue;
  address[22] = totalTransactions;
}

void loadSales()
{
  prefs.begin("sales", false);
  for (int i = 1; i <= NUM_PRODUCTS; i++)
  {
    saleCounts[i] = prefs.getInt(("s" + String(i)).c_str(), 0);
  }
  prefs.end();
  updateSalesAddresses();
}

void saveSales()
{
  prefs.begin("sales", false);
  for (int i = 1; i <= NUM_PRODUCTS; i++)
  {
    prefs.putInt(("s" + String(i)).c_str(), saleCounts[i]);
  }
  prefs.end();
}

volatile bool coinInserted = false;
volatile unsigned long lastDebounceTime = 0;
const unsigned long coinDebounceDelay = 70;
volatile int coinCount = 0;

void IRAM_ATTR coinISR()
{
  if (digitalRead(coinslot) == LOW)
  {
    unsigned long currentTime = millis();
    if (currentTime - lastDebounceTime > coinDebounceDelay)
    {
      coinInserted = true;
      lastDebounceTime = currentTime;
    }
  }
}

void IRAM_ATTR flowISR()
{
  unsigned long currentTime = millis();
  if (currentTime - lastFlowPulse > flowDebounceDelay)
  {
    pulseCount++;
    lastFlowPulse = currentTime;
  }
}

void recordSale(int product)
{
  Serial.print("recordSale() called with product: ");
  Serial.println(product);

  if (product < 1 || product > NUM_PRODUCTS)
  {
    Serial.println("Invalid product in recordSale().");
    return;
  }

  saleCounts[product]++;
  updateSalesAddresses();
  saveSales();
}

void clearSales()
{
  prefs.begin("sales", false);
  prefs.clear();
  prefs.end();
  for (int i = 1; i <= NUM_PRODUCTS; i++)
    saleCounts[i] = 0;
  totalLiters = totalRevenue = totalTransactions = 0;
  for (int i = 16; i <= 16 + NUM_PRODUCTS + 2; i++)
    address[i] = 0;
}

// ---------------- Config persistence (prices + admin password) ----------------

// HMI register mapping (verified against Vendo Vigan.skr — Modbus 4x bindings).
// Avoid collision with: 4x0012 ("waterSensor"), 4x0013 (zeroed in setup),
//                       4x0016-19 (sales counts), 4x0021-22 (totals), 4x0025 (login).
//
// Prices are stored as cents (×100); HMI binds with decimal=2 to render "X.XX".
// Range cap: uint16_t max 65535 → max price 655.35 PHP (validated in /save).
const uint8_t PRICE_REGS[NUM_PRODUCTS + 1] = {0, 9, 10, 11, 14};   // p1..p4
const uint8_t VOL_REGS[NUM_PRODUCTS + 1]   = {0, 6, 15, 20, 23};   // l1..l4 (integer liters)

void updatePriceAddresses()
{
  for (int i = 1; i <= NUM_PRODUCTS; i++)
    address[PRICE_REGS[i]] = (uint16_t)(prices[i] * 100.0);
}

void updateLabelAddresses()
{
  for (int i = 1; i <= NUM_PRODUCTS; i++)
    address[VOL_REGS[i]] = (uint16_t)litersLabel[i];
}

void loadConfig()
{
  prefs.begin("config", true);
  for (int i = 1; i <= NUM_PRODUCTS; i++)
  {
    prices[i]        = prefs.getDouble(("p" + String(i)).c_str(), prices[i]);
    litersLabel[i]   = prefs.getInt   (("l" + String(i)).c_str(), litersLabel[i]);
    targetVolumes[i] = prefs.getFloat (("t" + String(i)).c_str(), targetVolumes[i]);
  }
  adminPassword = prefs.getString("adminpw", DEFAULT_ADMIN_PASSWORD);
  prefs.end();
  updatePriceAddresses();
  updateLabelAddresses();
}

void savePrice(int i, double v)
{
  if (i < 1 || i > NUM_PRODUCTS) return;
  prefs.begin("config", false);
  prefs.putDouble(("p" + String(i)).c_str(), v);
  prefs.end();
}

void saveLiter(int i, int v)
{
  if (i < 1 || i > NUM_PRODUCTS) return;
  prefs.begin("config", false);
  prefs.putInt(("l" + String(i)).c_str(), v);
  prefs.end();
}

void saveTargetVolume(int i, float v)
{
  if (i < 1 || i > NUM_PRODUCTS) return;
  prefs.begin("config", false);
  prefs.putFloat(("t" + String(i)).c_str(), v);
  prefs.end();
}

void savePassword(const String &pw)
{
  prefs.begin("config", false);
  prefs.putString("adminpw", pw);
  prefs.end();
}

// ---------------- Web admin handlers ----------------

bool requireAuth()
{
  if (!server.authenticate("admin", adminPassword.c_str()))
  {
    server.requestAuthentication();
    return false;
  }
  return true;
}

String renderAdminPage(const String &message)
{
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>GoWater Admin</title>";
  html += "<style>body{font-family:sans-serif;max-width:480px;margin:1em auto;padding:0 1em}";
  html += "h1{font-size:1.3em}label{display:block;margin:0.6em 0 0.2em}";
  html += "input{width:100%;padding:0.5em;font-size:1em;box-sizing:border-box}";
  html += "button{margin-top:1em;padding:0.7em 1.2em;font-size:1em}";
  html += ".msg{background:#dfd;border:1px solid #6c6;padding:0.6em;margin:0.6em 0}";
  html += "hr{margin:2em 0}</style></head><body>";
  html += "<h1>GoWater Vendo Admin</h1>";
  if (message.length() > 0)
    html += "<div class='msg'>" + message + "</div>";

  html += "<form method='POST' action='/save'><h2>Products</h2>";
  html += "<p style='font-size:0.9em;color:#666'>Label = what the HMI displays. Target Volume = how many liters the machine actually pours (calibrated, may be slightly more/less than the label).</p>";
  for (int i = 1; i <= NUM_PRODUCTS; i++)
  {
    html += "<fieldset style='margin:1em 0;padding:0.6em'><legend>Product " + String(i) + "</legend>";
    html += "<label>Price (PHP)</label>";
    html += "<input type='number' step='0.01' min='0' max='650' name='p" + String(i) +
            "' value='" + String(prices[i], 2) + "' required>";
    html += "<label>Volume Label (L) — shown on HMI</label>";
    html += "<input type='number' step='1' min='0' max='999' name='l" + String(i) +
            "' value='" + String(litersLabel[i]) + "' required>";
    html += "<label>Target Volume (L) — actual dispense</label>";
    html += "<input type='number' step='0.1' min='0' max='999' name='t" + String(i) +
            "' value='" + String(targetVolumes[i], 1) + "' required>";
    html += "</fieldset>";
  }
  html += "<button type='submit'>Save Products</button></form>";

  html += "<hr><form method='POST' action='/changepw'><h2>Change Admin Password</h2>";
  html += "<label>New password (min 6 chars)</label>";
  html += "<input type='password' name='newpw' minlength='6' required>";
  html += "<p style='font-size:0.9em;color:#666'>Note: this is also the WiFi password. After changing, reconnect using the new one.</p>";
  html += "<button type='submit'>Change Password</button></form>";
  html += "</body></html>";
  return html;
}

void handleRoot()
{
  if (!requireAuth())
    return;
  server.send(200, "text/html", renderAdminPage(""));
}

void handleSave()
{
  if (!requireAuth())
    return;

  String message = "Saved: ";
  bool anyPrice = false, anyLabel = false;

  for (int i = 1; i <= NUM_PRODUCTS; i++)
  {
    // Price (cents fit in uint16_t → cap at 650)
    String pk = "p" + String(i);
    if (server.hasArg(pk))
    {
      double v = server.arg(pk).toDouble();
      if (v >= 0 && v <= 650)
      {
        prices[i] = v;
        savePrice(i, v);
        message += pk + "=" + String(v, 2) + " ";
        anyPrice = true;
      }
    }
    // Volume label (integer liters, fits in uint16_t)
    String lk = "l" + String(i);
    if (server.hasArg(lk))
    {
      int v = server.arg(lk).toInt();
      if (v >= 0 && v <= 999)
      {
        litersLabel[i] = v;
        saveLiter(i, v);
        message += lk + "=" + String(v) + " ";
        anyLabel = true;
      }
    }
    // Target volume (float liters, controls actual dispense)
    String tk = "t" + String(i);
    if (server.hasArg(tk))
    {
      float v = server.arg(tk).toFloat();
      if (v >= 0 && v <= 999)
      {
        targetVolumes[i] = v;
        saveTargetVolume(i, v);
        message += tk + "=" + String(v, 1) + " ";
      }
    }
  }

  if (anyPrice) updatePriceAddresses();
  if (anyLabel) updateLabelAddresses();
  if (message == "Saved: ") message = "No valid values submitted.";
  server.send(200, "text/html", renderAdminPage(message));
}

void handleChangePassword()
{
  if (!requireAuth())
    return;

  if (!server.hasArg("newpw"))
  {
    server.send(400, "text/plain", "Missing newpw");
    return;
  }
  String newpw = server.arg("newpw");
  if (newpw.length() < 6)
  {
    server.send(200, "text/html", renderAdminPage("Password must be at least 6 characters."));
    return;
  }
  adminPassword = newpw;
  savePassword(newpw);
  WiFi.softAPdisconnect(false);
  WiFi.softAP(AP_SSID, adminPassword.c_str());
  server.send(200, "text/html", renderAdminPage("Password changed. WiFi restarted with new password."));
}

void handleNotFound()
{
  server.send(404, "text/plain", "Not found");
}

void setup()
{
  Serial.begin(9600);
  Serial1.begin(9600, SERIAL_8N1, RX, TX);
  gsm.begin(9600);
  hmi.begin(1, 9600, SERIAL_8N1);
  hmi.configureHoldingRegisters(address, regs);

  Serial.println("Preparing System");
  Serial.println("Preparing GSM");
  delay(1000);

  SendATCommand("AT+CMGF=1\r\n", 1000);
  SendATCommand("AT+CPIN?\r\n", 1000);
  SendATCommand("AT+CNMI=2,2,0,0,0\r\n", 1000);

  testCPIN();
  testCFUN();
  testCOPS();
  testCSQ();
  testNetworkRegistration();

  for (int i = 0; i < 9; i++)
    address[i] = 0;
  address[13] = 0;

  loadConfig();
  loadSales();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, adminPassword.c_str());
  Serial.print("AP started: ");
  Serial.print(AP_SSID);
  Serial.print(" @ ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/changepw", HTTP_POST, handleChangePassword);
  server.onNotFound(handleNotFound);
  server.begin();

  xTaskCreate(HMICon, "Address refresh all time", 5000, NULL, 1, NULL);
  delay(100);

  pinMode(coinslot, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(coinslot), coinISR, FALLING);

  pinMode(flowSensor, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(flowSensor), flowISR, RISING);

  pinMode(slotActivator, OUTPUT);
  pinMode(relay, OUTPUT);

  Serial.println("System is Ready");
}

void loop()
{
  // Web admin only runs when no transaction is active, to avoid disturbing
  // the SoftwareSerial GSM timing during coin/SMS/dispense flow.
  if (address[0] == 0)
    server.handleClient();

  if (address[7] == 1)
  {
    resetRegisters();
    address[7] = 0;
  }

  if (address[8] == 1)
  {
    clearSales();
    address[8] = 0;
  }

  if (address[0] == 1)
  {
    Serial.print("Transaction triggered. address[1] = ");
    Serial.println(address[1]);
    address[4] = prices[address[1]];
    if (address[2] == 1)
    {
      cashPay();
      recordSale(address[1]);
      resetRegisters();
    }
    if (address[2] == 2)
    {
      gcashPay();
      recordSale(address[1]);
      resetRegisters();
    }
    digitalWrite(relay, LOW);
  }

  digitalWrite(relay, LOW);

  if (millis() - lastSMSCheck > 15000)
  {
    lastSMSCheck = millis();
    processSMS();
  }
}

// The rest of your existing functions stay the same

void resetRegisters()
{
  // Clear control registers (0-6); leave sales registers unchanged.
  address[0] = 0;
  address[1] = 0;
  address[2] = 0;
  address[3] = 0;
  address[4] = 0;
  address[5] = 0;
  address[6] = 0;
  digitalWrite(relay, LOW);
  currency = 0;
}

void HMICon(void *parameter)
{
  for (;;)
  {
    hmi.poll();
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

void SendATCommand(String command, unsigned long timout)
{
  gsm.print(command);
  unsigned long startTime = millis();
  while (millis() - startTime < timout)
  {
    while (gsm.available())
    {
      char c = gsm.read();
      Serial.print(c);
    }
  }
  Serial.println();
}

void cashPay()
{
  if (prices[address[1]] <= 0)
  {
    Serial.println("Invalid product selection. Aborting cashPay.");
    return;
  }

  noInterrupts();
  coinCount = 0;
  coinInserted = false;
  interrupts();

  digitalWrite(slotActivator, HIGH); // Enable coin slot
  unsigned long lastCoinPulseTime = 0;
  const unsigned long finalUpdateDelay = 300;
  bool finalUpdated = false;
  bool cancelled = false;

  while (true)
  {
    if (address[0] == 0)
    {
      Serial.println("Transaction cancelled. User pressed back.");
      cancelled = true;
      break;
    }

    if (coinInserted)
    {
      noInterrupts();
      coinInserted = false;
      interrupts();
      delay(15);
      if (digitalRead(coinslot) == LOW)
      {
        coinCount++;
        currency += 1.0;
        lastCoinPulseTime = millis();
        Serial.print("Detected coin pulse. Total coinCount: ");
        Serial.println(coinCount);
        Serial.print("Currency: ");
        Serial.println(currency);
        finalUpdated = false;
      }
    }

    if (coinCount > 0 && !finalUpdated && (millis() - lastCoinPulseTime > finalUpdateDelay))
    {
      address[4] = (uint16_t)(prices[address[1]] - currency);
      finalUpdated = true;
      Serial.print("Final update: Remaining balance: ");
      Serial.println(address[4]);
    }

    if (currency >= prices[address[1]])
    {
      address[4] = (uint16_t)(prices[address[1]] - currency);
      digitalWrite(slotActivator, LOW); // Turn off coin slot
      address[3] = 6;
      delay(3000);
      address[3] = 7;
      delay(1000);
      address[3] = 8;
      delay(1000);
      address[3] = 9;
      delay(1000);

      // Volume-based dispensing using polled pulse counting (no ISR)
      detachInterrupt(digitalPinToInterrupt(flowSensor));
      digitalWrite(relay, HIGH);
      address[3] = 10;
      delay(500);

      unsigned long dispenseStartTime = millis();
      unsigned long lastLogTime = dispenseStartTime;
      float targetLiters = targetVolumes[address[1]];
      unsigned long targetPulses = (unsigned long)(targetLiters * PULSES_PER_LITER);
      unsigned long localPulseCount = 0;
      bool dispenseFailed = false;
      int lastPinState = digitalRead(flowSensor);
      unsigned long lastEdgeTime = 0;
      unsigned long lastPulseTime = millis();

      Serial.println("========== DISPENSE START (CASH) ==========");
      Serial.print("Target: ");
      Serial.print(targetLiters, 2);
      Serial.print(" L (");
      Serial.print(targetPulses);
      Serial.println(" pulses)");

      while (localPulseCount < targetPulses)
      {
        int pinState = digitalRead(flowSensor);

        if (pinState == HIGH && lastPinState == LOW)
        {
          unsigned long now = millis();
          if (now - lastEdgeTime > 5)
          {
            localPulseCount++;
            lastEdgeTime = now;
            lastPulseTime = now;
          }
        }
        lastPinState = pinState;

        if (localPulseCount > 10 && (millis() - lastPulseTime > 5000))
        {
          Serial.println("ERROR: Flow stalled - no pulses for 5 seconds");
          break;
        }

        if (localPulseCount == 0 && (millis() - dispenseStartTime > 8000))
        {
          Serial.println("ERROR: No water detected - timeout after 8 seconds");
          dispenseFailed = true;
          break;
        }

        if (millis() - lastLogTime >= 500)
        {
          float elapsedSec = (millis() - dispenseStartTime) / 1000.0;
          float dispensedMl = (localPulseCount / PULSES_PER_LITER) * 1000.0;
          float percentComplete = ((float)localPulseCount / targetPulses) * 100.0;

          Serial.print("[");
          Serial.print(elapsedSec, 1);
          Serial.print("s] Pulses: ");
          Serial.print(localPulseCount);
          Serial.print("/");
          Serial.print(targetPulses);
          Serial.print(" | Vol: ");
          Serial.print(dispensedMl, 0);
          Serial.print("mL | ");
          Serial.print(percentComplete, 1);
          Serial.println("%");

          lastLogTime = millis();
        }

        delayMicroseconds(500);
      }

      digitalWrite(relay, LOW);
      unsigned long finalPulses = localPulseCount;
      unsigned long totalDispenseTime = millis() - dispenseStartTime;
      float finalVolume = finalPulses / PULSES_PER_LITER;

      Serial.println("========== DISPENSE COMPLETE ==========");
      Serial.print("Final pulses: ");
      Serial.println(finalPulses);
      Serial.print("Volume dispensed: ");
      Serial.print(finalVolume, 3);
      Serial.print(" L (");
      Serial.print(finalVolume * 1000.0, 0);
      Serial.println(" mL)");
      Serial.print("Total time: ");
      Serial.print(totalDispenseTime / 1000.0, 1);
      Serial.println(" seconds");
      Serial.println("========================================");

      attachInterrupt(digitalPinToInterrupt(flowSensor), flowISR, RISING);

      if (dispenseFailed)
      {
        resetRegisters();
      }
      else
      {
        address[3] = 17;
        delay(1000);
        address[3] = 18;
        delay(1000);
        address[3] = 19;
        delay(1000);
        address[3] = 20;
        delay(1000);
        address[3] = 21;
        delay(1000);
      }
      break;
    }

    delay(10);
  }

  if (cancelled)
  {
    digitalWrite(slotActivator, LOW);
    Serial.println("Coin slot deactivated after cancel.");
  }
}

void gcashPay()
{
  for (;;)
  {
    if (address[0] == 0)
      break;
    processSMS();
    Serial.print("Current credited currency: PHP ");
    Serial.println(currency, 2);
    if (address[0] == 0)
      break;
    if (currency >= prices[address[1]])
    {
      address[3] = 6;
      delay(3000);
      address[3] = 7;
      delay(1000);
      address[3] = 8;
      delay(1000);
      address[3] = 9;
      delay(1000);

      // Volume-based dispensing using polled pulse counting (no ISR)
      detachInterrupt(digitalPinToInterrupt(flowSensor));
      digitalWrite(relay, HIGH);
      address[3] = 10;
      delay(500);

      unsigned long dispenseStartTime = millis();
      unsigned long lastLogTime = dispenseStartTime;
      float targetLiters = targetVolumes[address[1]];
      unsigned long targetPulses = (unsigned long)(targetLiters * PULSES_PER_LITER);
      unsigned long localPulseCount = 0;
      bool dispenseFailed = false;
      int lastPinState = digitalRead(flowSensor);
      unsigned long lastEdgeTime = 0;
      unsigned long lastPulseTime = millis();

      Serial.println("========== DISPENSE START (GCASH) ==========");
      Serial.print("Target: ");
      Serial.print(targetLiters, 2);
      Serial.print(" L (");
      Serial.print(targetPulses);
      Serial.println(" pulses)");

      while (localPulseCount < targetPulses)
      {
        int pinState = digitalRead(flowSensor);

        if (pinState == HIGH && lastPinState == LOW)
        {
          unsigned long now = millis();
          if (now - lastEdgeTime > 5)
          {
            localPulseCount++;
            lastEdgeTime = now;
            lastPulseTime = now;
          }
        }
        lastPinState = pinState;

        if (localPulseCount > 10 && (millis() - lastPulseTime > 5000))
        {
          Serial.println("ERROR: Flow stalled - no pulses for 5 seconds");
          break;
        }

        if (localPulseCount == 0 && (millis() - dispenseStartTime > 8000))
        {
          Serial.println("ERROR: No water detected - timeout after 8 seconds");
          dispenseFailed = true;
          break;
        }

        if (millis() - lastLogTime >= 500)
        {
          float elapsedSec = (millis() - dispenseStartTime) / 1000.0;
          float dispensedMl = (localPulseCount / PULSES_PER_LITER) * 1000.0;
          float percentComplete = ((float)localPulseCount / targetPulses) * 100.0;

          Serial.print("[");
          Serial.print(elapsedSec, 1);
          Serial.print("s] Pulses: ");
          Serial.print(localPulseCount);
          Serial.print("/");
          Serial.print(targetPulses);
          Serial.print(" | Vol: ");
          Serial.print(dispensedMl, 0);
          Serial.print("mL | ");
          Serial.print(percentComplete, 1);
          Serial.println("%");

          lastLogTime = millis();
        }

        delayMicroseconds(500);
      }

      digitalWrite(relay, LOW);
      unsigned long finalPulses = localPulseCount;
      unsigned long totalDispenseTime = millis() - dispenseStartTime;
      float finalVolume = finalPulses / PULSES_PER_LITER;

      Serial.println("========== DISPENSE COMPLETE ==========");
      Serial.print("Final pulses: ");
      Serial.println(finalPulses);
      Serial.print("Volume dispensed: ");
      Serial.print(finalVolume, 3);
      Serial.print(" L (");
      Serial.print(finalVolume * 1000.0, 0);
      Serial.println(" mL)");
      Serial.print("Total time: ");
      Serial.print(totalDispenseTime / 1000.0, 1);
      Serial.println(" seconds");
      Serial.println("========================================");

      attachInterrupt(digitalPinToInterrupt(flowSensor), flowISR, RISING);

      if (dispenseFailed)
      {
        resetRegisters();
      }
      else
      {
        address[3] = 17;
        delay(1000);
        address[3] = 18;
        delay(1000);
        address[3] = 19;
        delay(1000);
        address[3] = 20;
        delay(1000);
        address[3] = 21;
        delay(1000);
      }
      break;
    }
    delay(100);
  }
}

void sendSalesSMS(String number)
{
  String msg = "GoWater Sales Report\n";
  msg += "--------------------\n";
  for (int i = 1; i <= NUM_PRODUCTS; i++)
  {
    int rev = (int)(saleCounts[i] * prices[i]);
    msg += String(litersLabel[i]) + "L x" + String(saleCounts[i]) +
           " = PHP " + String(rev) + "\n";
  }
  msg += "--------------------\n";
  msg += "Total Liters: " + String(totalLiters) + "L\n";
  msg += "Total Sales: PHP " + String(totalRevenue) + "\n";
  msg += "Transactions: " + String(totalTransactions);

  Serial.println("Sending sales SMS to: " + number);
  gsm.print("AT+CMGS=\"" + number + "\"\r");
  delay(1000);
  gsm.print(msg);
  gsm.write(26); // CTRL+Z to send
  Serial.println("Sales SMS sent.");
}

void resetSalesSMS(String number)
{
  clearSales();
  String msg = "GoWater Sales Reset\n";
  msg += "--------------------\n";
  msg += "All sales data cleared.";

  Serial.println("Sending reset SMS to: " + number);
  gsm.print("AT+CMGS=\"" + number + "\"\r");
  delay(1000);
  gsm.print(msg);
  gsm.write(26); // CTRL+Z to send
  Serial.println("Reset SMS sent.");
}

// ---------------- SMS Processing Functions ----------------

bool isAuthorizedSender(String number)
{
  // Normalize local format (09xxxxxxxxx) to international (+639xxxxxxxxx)
  String normalized = number;
  if (normalized.startsWith("09"))
  {
    normalized = "+63" + normalized.substring(1);
  }
  if (ADMIN_NUMBER_1.length() > 0 && normalized == ADMIN_NUMBER_1)
    return true;
  if (ADMIN_NUMBER_2.length() > 0 && normalized == ADMIN_NUMBER_2)
    return true;
  return false;
}

void extractSenderInfo(String sms)
{
  senderNumber = "";
  int startIndex = sms.indexOf("+CMT: \"");
  if (startIndex != -1)
  {
    int endIndex = sms.indexOf("\"", startIndex + 7);
    if (endIndex != -1)
    {
      senderNumber = sms.substring(startIndex + 7, endIndex);
    }
  }
  if (senderNumber.length() > 0)
  {
    Serial.print("Sender: ");
    Serial.println(senderNumber);
  }
  else
  {
    Serial.println("Failed to extract sender information.");
  }
}

void extractAmount(String sms)
{
  int idx = sms.indexOf("PHP");
  if (idx < 0)
  {
    Serial.println("No amount found in the message.");
    return;
  }

  int start = idx + 3;
  // Skip whitespace
  while (start < sms.length() && isspace(sms.charAt(start)))
    start++;

  int end = start;
  // Consume digits and decimal point only
  while (end < sms.length() && (isdigit(sms.charAt(end)) || sms.charAt(end) == '.'))
  {
    end++;
  }

  String amt = sms.substring(start, end);
  amt.trim();
  Serial.print("Raw extracted amount: '");
  Serial.print(amt);
  Serial.println("'");

  float val = amt.toFloat();
  if (val > 0.0)
  {
    currency += val;
    Serial.print("Credited amount: PHP ");
    Serial.println(val, 2);
    Serial.print("Total currency: PHP ");
    Serial.println(currency, 2);
  }
  else
  {
    Serial.println("Failed to parse a valid numeric amount.");
  }
}

bool isNumeric(String str)
{
  for (int i = 0; i < str.length(); i++)
  {
    if (!isdigit(str.charAt(i)) && str.charAt(i) != '.')
    {
      return false;
    }
  }
  return true;
}

void processSMS()
{
  int availableBytes = gsm.available();
  Serial.print("Bytes available in SMS buffer: ");
  Serial.println(availableBytes);
  if (availableBytes > 0)
  {
    String sms = gsm.readString();
    Serial.println("Received SMS: ");
    Serial.println(sms);
    extractSenderInfo(sms);
    sms.toLowerCase(); // normalize to lowercase

    // Only accept SMS from PAYMONGO or trusted number
    if (senderNumber == "PAYMONGO")
    {
      extractAmount(sms);
    }
    else if (sms.indexOf("sales") != -1)
    {
      if (isAuthorizedSender(senderNumber))
      {
        sendSalesSMS(senderNumber);
      }
      else
      {
        Serial.println("Unauthorized sales request from: " + senderNumber);
      }
    }
    else if (sms.indexOf("reset") != -1)
    {
      if (isAuthorizedSender(senderNumber))
      {
        resetSalesSMS(senderNumber);
      }
      else
      {
        Serial.println("Unauthorized reset request from: " + senderNumber);
      }
    }
    else
    {
      Serial.print("Ignoring SMS from unauthorized sender: ");
      Serial.println(senderNumber);
    }
  }
  else
  {
    Serial.println("No new SMS data available.");
  }
}

// ---------------- Test and Troubleshooting Functions ----------------

void testExtractAmount()
{
  Serial.println("Testing extractAmount with a simulated SMS message...");
  String testSMS = "+CMT: \"PAYMONGO\",\"...\"\r\nYour payment of PHP20.00 has been received.";
  Serial.println("Simulated SMS: ");
  Serial.println(testSMS);
  extractAmount(testSMS);
}

void readSMSInbox()
{
  Serial.println("Reading SMS Inbox using AT+CMGL command...");
  gsm.println("AT+CMGL=\"ALL\"\r\n");
  delay(3000);
  String inboxResponse = "";
  while (gsm.available())
  {
    inboxResponse += (char)gsm.read();
  }
  Serial.println("SMS Inbox:");
  Serial.println(inboxResponse);
}

// ---------------- Additional GSM Debug Functions ----------------

void testCPIN()
{
  Serial.println("Testing SIM PIN status...");
  gsm.println("AT+CPIN?\r\n");
  delay(2000);
  String response = "";
  while (gsm.available())
  {
    response += (char)gsm.read();
  }
  if (response.indexOf("READY") != -1)
  {
    Serial.println("SIM is ready.");
  }
  else
  {
    Serial.println("Error: SIM card may not be inserted or is locked.");
  }
}

void testCFUN()
{
  Serial.println("Testing full functionality (CFUN)...");
  gsm.println("AT+CFUN?\r\n");
  delay(2000);
  String response = "";
  while (gsm.available())
  {
    response += (char)gsm.read();
  }
  if (response.indexOf("+CFUN: 1") != -1)
  {
    Serial.println("Module is in full functionality mode.");
  }
  else
  {
    Serial.println("Warning: Module may not be in full functionality mode.");
  }
}

void testCOPS()
{
  Serial.println("Testing operator selection (COPS)...");
  gsm.println("AT+COPS?\r\n");
  delay(2000);
  String response = "";
  while (gsm.available())
  {
    response += (char)gsm.read();
  }
  if (response.indexOf("+COPS:") != -1)
  {
    Serial.println("Operator information received: " + response);
  }
  else
  {
    Serial.println("Error: Could not retrieve operator information.");
  }
}

void testCSQ()
{
  Serial.println("Testing signal quality (CSQ)...");
  gsm.println("AT+CSQ\r\n");
  delay(2000);
  String response = "";
  while (gsm.available())
  {
    response += (char)gsm.read();
  }
  Serial.print("Signal response: ");
  Serial.println(response);
}

void testNetworkRegistration()
{
  Serial.println("Setting verbose network registration mode...");
  gsm.println("AT+CREG=2\r\n");
  delay(2000);

  Serial.println("Testing network registration...");
  gsm.println("AT+CREG?\r\n");
  delay(5000);
  String response = "";
  while (gsm.available())
  {
    response += (char)gsm.read();
  }
  Serial.print("Network registration response: ");
  Serial.println(response);

  bool registered = false;
  if (response.indexOf(",1") != -1 || response.indexOf(",5") != -1)
  {
    Serial.println("Registered on network.");
    registered = true;
  }
  else
  {
    Serial.println("Error: Not registered on network.");
  }

  if (registered)
  {
    Serial.println("Network registered. (SMS sending disabled)");
  }
}