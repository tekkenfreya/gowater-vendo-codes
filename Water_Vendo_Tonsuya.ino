#include <ModbusRTUSlave.h>
#include <SoftwareSerial.h>
#include <Preferences.h>

#define TX 16
#define RX 17
#define relay 26
#define coinslot 23
#define slotActivator 25
#define SIMRX 19
#define SIMTX 18

const int regs = 26;
uint16_t address[regs];

const int durations[] = {0, 7500, 45000, 168000, 156000};
const double prices[] = {0.00, 5.00, 10.00, 20.00, 20.00};

ModbusRTUSlave hmi(Serial1);
SoftwareSerial gsm(SIMRX, SIMTX);
Preferences preferences;

String senderNumber = "";
unsigned long lastSMSCheck = 0;
double currency = 0.00;

volatile bool coinInserted = false;
volatile unsigned long lastDebounceTime = 0;
const unsigned long coinDebounceDelay = 70;
volatile int coinCount = 0;

int totalLiters = 0, totalTransactions = 0;
int count500ml = 0, count5L = 0, count1Gallon = 0;

void IRAM_ATTR coinISR() {
  if (digitalRead(coinslot) == LOW) {
    unsigned long currentTime = millis();
    if (currentTime - lastDebounceTime > coinDebounceDelay) {
      coinInserted = true;
      lastDebounceTime = currentTime;
    }
  }
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

  xTaskCreate(HMICon, "HMI Task", 5000, NULL, 1, NULL);

  for (int i = 0; i < regs; i++) address[i] = 0;

  // Load saved sales data from flash
  preferences.begin("sales", false);
  totalLiters = preferences.getInt("liters", 0);
  totalTransactions = preferences.getInt("trans", 0);
  count500ml = preferences.getInt("c500ml", 0);
  count5L = preferences.getInt("c5L", 0);
  count1Gallon = preferences.getInt("c1Gal", 0);
  preferences.end();

  address[19] = totalLiters;
  address[21] = totalTransactions;
  address[16] = count500ml;
  address[17] = count5L;
  address[18] = count1Gallon;

  Serial.println("Sales data loaded from flash");

  delay(100);

  pinMode(relay, OUTPUT);
  pinMode(coinslot, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(coinslot), coinISR, FALLING);
  pinMode(slotActivator, OUTPUT);
}

void loop() {
  // Clear statistics if requested
  if (address[7] == 1) {
    totalLiters = 0;
    totalTransactions = 0;
    count500ml = 0;
    count5L = 0;
    count1Gallon = 0;
    address[19] = 0;
    address[21] = 0;
    address[7] = 0;

    // Clear saved sales from flash
    preferences.begin("sales", false);
    preferences.clear();
    preferences.end();

    Serial.println("Statistics cleared");
  }

  if (address[0] == 1 && address[1] >= 1 && address[1] <= 4) {
    address[4] = prices[address[1]];
    if (address[2] == 1) {
      Serial.print("Cash payment for product: ");
      Serial.println(address[1]);
      cashPay();
      resetRegisters();
    }
    digitalWrite(relay, LOW);
  }

  digitalWrite(relay, LOW);

  // Check for SMS periodically
  if (millis() - lastSMSCheck > 15000) {
    lastSMSCheck = millis();
    processSMS();
  }
}

void dispenseProduct(int product) {
  Serial.print("Starting dispense for product ");
  Serial.println(product);

  // Countdown screens
  address[3] = 7;
  delay(1000);
  address[3] = 8;
  delay(1000);
  address[3] = 9;
  delay(1000);

  // Dispense
  Serial.println("=== RELAY ACTIVATION ===");
  Serial.print("Relay pin: ");
  Serial.println(relay);
  Serial.print("Duration: ");
  Serial.print(durations[product]);
  Serial.println(" ms");
  Serial.print("Current relay state before: ");
  Serial.println(digitalRead(relay));

  digitalWrite(relay, HIGH);
  Serial.print("Relay set to HIGH - Current state: ");
  Serial.println(digitalRead(relay));

  address[3] = 10;
  delay(durations[product]);

  digitalWrite(relay, LOW);
  Serial.print("Relay set to LOW - Current state: ");
  Serial.println(digitalRead(relay));
  Serial.println("=== RELAY DEACTIVATED ===");

  // Complete screen
  address[3] = 11;
  delay(5000);

  Serial.println("Dispensing complete");
}

void recordUsage(int product) {
  totalTransactions++;

  switch(product) {
    case 1: totalLiters += 1; count500ml++; break;
    case 2: totalLiters += 5; count5L++; break;
    case 3: totalLiters += 19; count1Gallon++; break;
  }

  address[19] = totalLiters;
  address[21] = totalTransactions;
  address[16] = count500ml;
  address[17] = count5L;
  address[18] = count1Gallon;

  // Save sales data to flash
  preferences.begin("sales", false);
  preferences.putInt("liters", totalLiters);
  preferences.putInt("trans", totalTransactions);
  preferences.putInt("c500ml", count500ml);
  preferences.putInt("c5L", count5L);
  preferences.putInt("c1Gal", count1Gallon);
  preferences.end();

  Serial.print("Total liters: ");
  Serial.println(totalLiters);
  Serial.print("Total transactions: ");
  Serial.println(totalTransactions);
}

void resetRegisters() {
  address[0] = 0;
  address[1] = 0;
  address[2] = 0;
  address[3] = 0;
  address[4] = 0;
  address[5] = 0;
  address[6] = 0;
  digitalWrite(relay, LOW);
  digitalWrite(slotActivator, LOW);
  currency = 0;
  Serial.println("Registers reset - ready for next transaction");
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

  digitalWrite(slotActivator, HIGH);
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
        Serial.print("Coin pulse detected. coinCount: ");
        Serial.println(coinCount);
        Serial.print("Currency: ");
        Serial.println(currency);
        finalUpdated = false;
      }
    }

    if (coinCount > 0 && !finalUpdated && (millis() - lastCoinPulseTime > finalUpdateDelay)) {
      address[4] = (uint16_t)(prices[address[1]] - currency);
      finalUpdated = true;
      Serial.print("Remaining balance: ");
      Serial.println(address[4]);
    }

    if (currency >= prices[address[1]]) {
      address[4] = 0;
      digitalWrite(slotActivator, LOW);
      address[3] = 6;
      delay(3000);
      address[3] = 7;
      delay(1000);
      address[3] = 8;
      delay(1000);
      address[3] = 9;
      delay(1000);

      // Time-based dispensing
      digitalWrite(relay, HIGH);
      address[3] = 10;
      delay(durations[address[1]]);
      digitalWrite(relay, LOW);

      address[3] = 11;
      delay(5000);
      recordUsage(address[1]);
      break;
    }

    delay(10);
  }

  if (cancelled) {
    digitalWrite(slotActivator, LOW);
    Serial.println("Coin slot deactivated after cancel.");
  }
}

void HMICon(void* parameter) {
  for (;;) {
    hmi.poll();
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

// ============= GSM/SMS Functions =============

void SendATCommand(String command, unsigned long timeout) {
  gsm.print(command);
  unsigned long startTime = millis();
  while (millis() - startTime < timeout) {
    while (gsm.available()) {
      char c = gsm.read();
      Serial.print(c);
    }
  }
  Serial.println();
}

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

void sendSalesSMS() {
  String msg = "GoWater Usage Report:\n"
               "500ml: " + String(count500ml) + "\n"
               "5L: " + String(count5L) + "\n"
               "1 Gallon: " + String(count1Gallon) + "\n"
               "Total Liters: " + String(totalLiters) + "L\n"
               "Transactions: " + String(totalTransactions);
  gsm.print("AT+CMGS=\"" + senderNumber + "\"\r");
  delay(1000);
  gsm.print(msg);
  gsm.write(26);
  Serial.println("Sales SMS sent to: " + senderNumber);
}

void resetSales() {
  totalLiters = 0;
  totalTransactions = 0;
  count500ml = 0;
  count5L = 0;
  count1Gallon = 0;
  address[19] = 0;
  address[21] = 0;
  address[16] = 0;
  address[17] = 0;
  address[18] = 0;

  // Clear saved sales from flash
  preferences.begin("sales", false);
  preferences.clear();
  preferences.end();

  String msg = "GoWater Sales Reset!\n"
               "All counters set to 0.";
  gsm.print("AT+CMGS=\"" + senderNumber + "\"\r");
  delay(1000);
  gsm.print(msg);
  gsm.write(26);
  Serial.println("Sales reset and confirmation SMS sent to: " + senderNumber);
}

void processSMS() {
  int availableBytes = gsm.available();
  Serial.print("SMS buffer bytes: ");
  Serial.println(availableBytes);
  if (availableBytes > 0) {
    delay(1500);  // Wait for full SMS (header + body) to arrive
    String sms = gsm.readString();
    Serial.println("--- SMS Received ---");
    Serial.println(sms);
    Serial.println("--- End SMS ---");
    extractSenderInfo(sms);
    sms.toLowerCase();

    if (sms.indexOf("doit") != -1) {
      Serial.println("Keyword 'doit' detected. Sending sales SMS...");
      sendSalesSMS();
    } else if (sms.indexOf("alohomora") != -1) {
      Serial.println("Keyword 'alohomora' detected. Resetting sales...");
      resetSales();
    } else {
      Serial.println("No recognized keyword in SMS.");
    }
  }
}

// ============= GSM Test Functions =============

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
    Serial.println("Network registered. GSM ready for use.");
  }
}