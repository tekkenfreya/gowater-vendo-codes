#include <ModbusRTUSlave.h>
#include <SoftwareSerial.h>
#include <Preferences.h>

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

const int prices1[] = {0, 100, 500, 1900};
const double prices[] = {0.00, 5.00, 10.00, 20.00, 20.00};

ModbusRTUSlave hmi(Serial1);
SoftwareSerial gsm(SIMRX, SIMTX);
Preferences prefs;

String senderNumber = "";
double currency = 0.00;

// Flow sensor variables
volatile unsigned long pulseCount = 0;
volatile unsigned long lastFlowPulse = 0;

// CALIBRATION: measured 300 pulses per liter
const float PULSES_PER_LITER = 300.0;

// Target volumes in liters for each product
const float targetVolumes[] = {0.0, 1.0, 5.0, 19.4};

// Debounce delay - reduce if missing pulses at high flow rates (try 1-5ms)
const unsigned long flowDebounceDelay = 2;

const unsigned long smsIntervalSecs = 60;
unsigned long smsCheckTimer = 0;
unsigned long lastSMSCheck = 0;

int saleCount1 = 0, saleCount2 = 0, saleCount3 = 0;
int totalLiters = 0, totalRevenue = 0, totalTransactions = 0;

void loadSales() {
  prefs.begin("sales", false);
  saleCount1 = prefs.getInt("s1", 0);
  saleCount2 = prefs.getInt("s2", 0);
  saleCount3 = prefs.getInt("s3", 0);
  prefs.end();

  totalTransactions = saleCount1 + saleCount2 + saleCount3;
  totalLiters = (saleCount1 * 1) + (saleCount2 * 5) + (saleCount3 * 19);
  totalRevenue = (saleCount1 * prices[1]) + (saleCount2 * prices[2]) + (saleCount3 * prices[3]);

  address[16] = saleCount1;
  address[17] = saleCount2;
  address[18] = saleCount3;
  address[19] = totalLiters;
  address[20] = totalRevenue;
  address[21] = totalTransactions;
}

void saveSales() {
  prefs.begin("sales", false);
  prefs.putInt("s1", saleCount1);
  prefs.putInt("s2", saleCount2);
  prefs.putInt("s3", saleCount3);
  prefs.end();
}

volatile bool coinInserted = false;
volatile unsigned long lastDebounceTime = 0;
const unsigned long coinDebounceDelay = 70;
volatile int coinCount = 0;

void IRAM_ATTR coinISR() {
  if (digitalRead(coinslot) == LOW) {
    unsigned long currentTime = millis();
    if (currentTime - lastDebounceTime > coinDebounceDelay) {
      coinInserted = true;
      lastDebounceTime = currentTime;
    }
  }
}

void IRAM_ATTR flowISR() {
  unsigned long currentTime = millis();
  if (currentTime - lastFlowPulse > flowDebounceDelay) {
    pulseCount++;
    lastFlowPulse = currentTime;
  }
}

void recordSale(int product) {
  Serial.print("recordSale() called with product: ");
  Serial.println(product);

  switch(product) {
    case 1: saleCount1++; break;
    case 2: saleCount2++; break;
    case 3: saleCount3++; break;
    default: Serial.println("Invalid product in recordSale()."); return;
  }

  totalTransactions = saleCount1 + saleCount2 + saleCount3;
  totalLiters = (saleCount1 * 1) + (saleCount2 * 5) + (saleCount3 * 19);
  totalRevenue = (saleCount1 * prices[1]) + (saleCount2 * prices[2]) + (saleCount3 * prices[3]);

  address[16] = saleCount1;
  address[17] = saleCount2;
  address[18] = saleCount3;
  address[19] = totalLiters;
  address[20] = totalRevenue;
  address[21] = totalTransactions;

  saveSales();
}

void clearSales() {
  prefs.begin("sales", false);
  prefs.clear();
  prefs.end();
  saleCount1 = saleCount2 = saleCount3 = 0;
  totalLiters = totalRevenue = totalTransactions = 0;
  for (int i = 16; i <= 21; i++) address[i] = 0;
}

void setup() {
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

  xTaskCreate(HMICon, "Address refresh all time", 5000, NULL, 1, NULL);

  for (int i = 0; i < 7; i++) address[i] = 0;
  address[13] = 0;
  delay(100);

  loadSales();

  pinMode(coinslot, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(coinslot), coinISR, FALLING);

  pinMode(flowSensor, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(flowSensor), flowISR, RISING);

  pinMode(slotActivator, OUTPUT);
  pinMode(relay, OUTPUT);

  Serial.println("System is Ready");
}

void loop() {
  if (address[7] == 1) {
    clearSales();
    address[7] = 0;
  }

  if (address[0] == 1) {
    Serial.print("Transaction triggered. address[1] = ");
    Serial.println(address[1]);
    address[4] = prices[address[1]];
    if (address[2] == 1) {
      cashPay();
      recordSale(address[1]);
      resetRegisters();
    }
    if (address[2] == 2) {
      gcashPay();
      recordSale(address[1]);
      resetRegisters();
    }
    digitalWrite(relay, LOW);
  }

  digitalWrite(relay, LOW);

  if (millis() - lastSMSCheck > 15000) {
    lastSMSCheck = millis();
    processSMS();
  }
}

// The rest of your existing functions stay the same


void resetRegisters() {
  // Clear control registers (0-6); leave sales registers unchanged.
  address[0] = 0;
  address[2] = 0;
  address[3] = 0;
  address[4] = 0;
  address[5] = 0;
  address[6] = 0;
  digitalWrite(relay, LOW);
  currency = 0;
}

void HMICon(void* parameter) {
  for (;;) {
    hmi.poll();
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

void SendATCommand(String command, unsigned long timout) {
  gsm.print(command);
  unsigned long startTime = millis();
  while (millis() - startTime < timout) {
    while (gsm.available()) {
      char c = gsm.read();
      Serial.print(c);
    }
  }
  Serial.println();
}

 void cashPay() {
  if (prices[address[1]] <= 0) {
    Serial.println("Invalid product selection. Aborting cashPay.");
    return;
  }

  noInterrupts();
  coinCount = 0;
  coinInserted = false;
  interrupts();

  digitalWrite(slotActivator, HIGH);  // Enable coin slot
  unsigned long lastCoinPulseTime = 0;
  const unsigned long finalUpdateDelay = 300;
  bool finalUpdated = false;
  bool cancelled = false;

  while (true) {
    if (address[0] == 0) {
      Serial.println("Transaction cancelled. User pressed back.");
      cancelled = true;
      break;
    }

    if (coinInserted) {
      noInterrupts();
      coinInserted = false;
      interrupts();
      delay(15);
      if (digitalRead(coinslot) == LOW) {
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

    if (coinCount > 0 && !finalUpdated && (millis() - lastCoinPulseTime > finalUpdateDelay)) {
      address[4] = (uint16_t)(prices[address[1]] - currency);
      finalUpdated = true;
      Serial.print("Final update: Remaining balance: ");
      Serial.println(address[4]);
    }

    if (currency >= prices[address[1]]) {
      address[4] = (uint16_t)(prices[address[1]] - currency);
      digitalWrite(slotActivator, LOW);  // Turn off coin slot
      address[3] = 6;
      delay(3000);
      address[3] = 7;
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
      int lastPinState = digitalRead(flowSensor);
      unsigned long lastEdgeTime = 0;
      unsigned long lastPulseTime = millis();

      Serial.println("========== DISPENSE START (CASH) ==========");
      Serial.print("Target: ");
      Serial.print(targetLiters, 2);
      Serial.print(" L (");
      Serial.print(targetPulses);
      Serial.println(" pulses)");

      while (localPulseCount < targetPulses) {
        int pinState = digitalRead(flowSensor);

        if (pinState == HIGH && lastPinState == LOW) {
          unsigned long now = millis();
          if (now - lastEdgeTime > 5) {
            localPulseCount++;
            lastEdgeTime = now;
            lastPulseTime = now;
          }
        }
        lastPinState = pinState;

        if (localPulseCount > 10 && (millis() - lastPulseTime > 5000)) {
          Serial.println("ERROR: Flow stalled - no pulses for 5 seconds");
          break;
        }

        if (millis() - lastLogTime >= 500) {
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
      break;
    }

    delay(10);
  }

  if (cancelled) {
    digitalWrite(slotActivator, LOW);
    Serial.println("Coin slot deactivated after cancel.");
  }
}




void gcashPay() {
  for (;;) {
    if (address[0] == 0) break;
    processSMS();
    Serial.print("Current credited currency: PHP ");
    Serial.println(currency, 2);
    if (address[0] == 0) break;
    if (currency >= prices[address[1]]) {
      address[3] = 6;
      delay(3000);
      address[3] = 7;
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
      int lastPinState = digitalRead(flowSensor);
      unsigned long lastEdgeTime = 0;
      unsigned long lastPulseTime = millis();

      Serial.println("========== DISPENSE START (GCASH) ==========");
      Serial.print("Target: ");
      Serial.print(targetLiters, 2);
      Serial.print(" L (");
      Serial.print(targetPulses);
      Serial.println(" pulses)");

      while (localPulseCount < targetPulses) {
        int pinState = digitalRead(flowSensor);

        if (pinState == HIGH && lastPinState == LOW) {
          unsigned long now = millis();
          if (now - lastEdgeTime > 5) {
            localPulseCount++;
            lastEdgeTime = now;
            lastPulseTime = now;
          }
        }
        lastPinState = pinState;

        if (localPulseCount > 10 && (millis() - lastPulseTime > 5000)) {
          Serial.println("ERROR: Flow stalled - no pulses for 5 seconds");
          break;
        }

        if (millis() - lastLogTime >= 500) {
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
      break;
    }
    delay(100);
  }
}

void sendSalesSMS() {
  String msg =
    "GoWater Sales Report:\n" +
    String(saleCount1) + "x1L\n" +
    String(saleCount2) + "x5L\n" +
    String(saleCount3) + "x19L\n" +
    "Liters: " + String(totalLiters) + "L\n" +
    "Revenue: PHP " + String(totalRevenue) + "\n" +
    "Txns: " + String(totalTransactions);

  Serial.println("Sending sales SMS...");
  gsm.print("AT+CMGS=\"09620650416\"\r");  // Replace with your number
  delay(1000);
  gsm.print(msg);
  gsm.write(26); // CTRL+Z to send
  Serial.println("Sales SMS sent.");
}


void checkAndSendSalesSMS() {
  unsigned long uptimeSecs = millis() / 1000;

  prefs.begin("sms", true);
  unsigned long lastSent = prefs.getULong("lastSent", 0);
  prefs.end();

  if ((uptimeSecs >= lastSent) && ((uptimeSecs - lastSent) >= smsIntervalSecs)) {
    sendSalesSMS();
    prefs.begin("sms", false);
    prefs.putULong("lastSent", uptimeSecs);
    prefs.end();
  }
}



// ---------------- SMS Processing Functions ----------------

void extractSenderInfo(String sms) {
  senderNumber = "";
  int startIndex = sms.indexOf("+CMT: \"");
  if (startIndex != -1) {
    int endIndex = sms.indexOf("\"", startIndex + 7);
    if (endIndex != -1) {
      senderNumber = sms.substring(startIndex + 7, endIndex);
    }
  }
  if (senderNumber.length() > 0) {
    Serial.print("Sender: ");
    Serial.println(senderNumber);
  } else {
    Serial.println("Failed to extract sender information.");
  }
}

void extractAmount(String sms) {
  int idx = sms.indexOf("PHP");
  if (idx < 0) {
    Serial.println("No amount found in the message.");
    return;
  }

  int start = idx + 3;
  // Skip whitespace
  while (start < sms.length() && isspace(sms.charAt(start))) start++;

  int end = start;
  // Consume digits and decimal point only
  while (end < sms.length() && (isdigit(sms.charAt(end)) || sms.charAt(end) == '.')) {
    end++;
  }

  String amt = sms.substring(start, end);
  amt.trim();
  Serial.print("Raw extracted amount: '"); Serial.print(amt); Serial.println("'");

  float val = amt.toFloat();
  if (val > 0.0) {
    currency += val;
    Serial.print("Credited amount: PHP "); Serial.println(val, 2);
    Serial.print("Total currency: PHP "); Serial.println(currency, 2);
  } else {
    Serial.println("Failed to parse a valid numeric amount.");
  }
}


bool isNumeric(String str) {
  for (int i = 0; i < str.length(); i++) {
    if (!isdigit(str.charAt(i)) && str.charAt(i) != '.') {
      return false;
    }
  }
  return true;
}

void processSMS() {
  int availableBytes = gsm.available();
  Serial.print("Bytes available in SMS buffer: ");
  Serial.println(availableBytes);
  if (availableBytes > 0) {
    String sms = gsm.readString();
    Serial.println("Received SMS: ");
    Serial.println(sms);
    extractSenderInfo(sms);
    sms.toLowerCase(); // normalize to lowercase

    // Only accept SMS from PAYMONGO or trusted number
    if (senderNumber == "PAYMONGO") {
      extractAmount(sms);
    } else if (senderNumber == "+639213926986" && sms.indexOf("doit") != -1) {  // Replace with your actual number
      sendSalesSMS();
    } else {
      Serial.print("Ignoring SMS from unauthorized sender: ");
      Serial.println(senderNumber);
    }
  } else {
    Serial.println("No new SMS data available.");
  }
}


// ---------------- Test and Troubleshooting Functions ----------------

void testExtractAmount() {
  Serial.println("Testing extractAmount with a simulated SMS message...");
  String testSMS = "+CMT: \"PAYMONGO\",\"...\"\r\nYour payment of PHP20.00 has been received.";
  Serial.println("Simulated SMS: ");
  Serial.println(testSMS);
  extractAmount(testSMS);
}

void readSMSInbox() {
  Serial.println("Reading SMS Inbox using AT+CMGL command...");
  gsm.println("AT+CMGL=\"ALL\"\r\n");
  delay(3000);
  String inboxResponse = "";
  while (gsm.available()) {
    inboxResponse += (char)gsm.read();
  }
  Serial.println("SMS Inbox:");
  Serial.println(inboxResponse);
}

// ---------------- Additional GSM Debug Functions ----------------

void testCPIN() {
  Serial.println("Testing SIM PIN status...");
  gsm.println("AT+CPIN?\r\n");
  delay(2000);
  String response = "";
  while (gsm.available()) {
    response += (char)gsm.read();
  }
  if (response.indexOf("READY") != -1) {
    Serial.println("SIM is ready.");
  } else {
    Serial.println("Error: SIM card may not be inserted or is locked.");
  }
}

void testCFUN() {
  Serial.println("Testing full functionality (CFUN)...");
  gsm.println("AT+CFUN?\r\n");
  delay(2000);
  String response = "";
  while (gsm.available()) {
    response += (char)gsm.read();
  }
  if (response.indexOf("+CFUN: 1") != -1) {
    Serial.println("Module is in full functionality mode.");
  } else {
    Serial.println("Warning: Module may not be in full functionality mode.");
  }
}

void testCOPS() {
  Serial.println("Testing operator selection (COPS)...");
  gsm.println("AT+COPS?\r\n");
  delay(2000);
  String response = "";
  while (gsm.available()) {
    response += (char)gsm.read();
  }
  if (response.indexOf("+COPS:") != -1) {
    Serial.println("Operator information received: " + response);
  } else {
    Serial.println("Error: Could not retrieve operator information.");
  }
}

void testCSQ() {
  Serial.println("Testing signal quality (CSQ)...");
  gsm.println("AT+CSQ\r\n");
  delay(2000);
  String response = "";
  while (gsm.available()) {
    response += (char)gsm.read();
  }
  Serial.print("Signal response: ");
  Serial.println(response);
}

void testNetworkRegistration() {
  Serial.println("Setting verbose network registration mode...");
  gsm.println("AT+CREG=2\r\n");
  delay(2000);
  
  Serial.println("Testing network registration...");
  gsm.println("AT+CREG?\r\n");
  delay(5000);
  String response = "";
  while (gsm.available()) {
    response += (char)gsm.read();
  }
  Serial.print("Network registration response: ");
  Serial.println(response);

  bool registered = false;
  if (response.indexOf(",1") != -1 || response.indexOf(",5") != -1) {
    Serial.println("Registered on network.");
    registered = true;
  } else {
    Serial.println("Error: Not registered on network.");
  }
  
  if (registered) {
    Serial.println("Network registered. (SMS sending disabled)");
  }
}