#include <ModbusRTUSlave.h>
#include <SoftwareSerial.h>
#include <Preferences.h>

#define TX 16
#define RX 17
#define relay1 25
#define relay2 26
#define relay3 27
#define relay4 14
#define SIMRX 19
#define SIMTX 18

const int relayPins[] = {0, relay1, relay2, relay3, relay4};

const int regs = 14;
uint16_t address[regs];

// ============ PRODUCT CONFIG (edit here) ============
const int NUM_VALVES = 4;
const unsigned long dispenseDuration = 45000;  // ms - same for all valves
const int litersPerDispense = 1;               // 500ml = 0.5L, tracked as 1 dispense
// ====================================================

ModbusRTUSlave hmi(Serial1);
SoftwareSerial gsm(SIMRX, SIMTX);
Preferences prefs;

String senderNumber = "";
unsigned long lastSMSCheck = 0;

// Valve state tracking (non-blocking, all 4 independent)
// States: 0=idle, 1=countdown, 2=dispensing, 3=complete
int valveState[NUM_VALVES + 1] = {0};
unsigned long valveTimer[NUM_VALVES + 1] = {0};
int countdownStep[NUM_VALVES + 1] = {0};

// HMI registers:
// address[1] = valve trigger (write 1-4 to start that valve)
// address[2] = valve 1 status, address[3] = valve 2, address[4] = valve 3, address[5] = valve 4
// address[7] = clear sales trigger
// address[8-11] = sale counts per valve
// address[12] = total liters, address[13] = total transactions

int saleCounts[NUM_VALVES + 1] = {0};
int totalLiters = 0, totalTransactions = 0;

void updateSalesAddresses() {
  totalTransactions = 0;
  totalLiters = 0;
  for (int i = 1; i <= NUM_VALVES; i++) {
    totalTransactions += saleCounts[i];
    totalLiters += saleCounts[i] * litersPerDispense;
  }
  address[8] = saleCounts[1];
  address[9] = saleCounts[2];
  address[10] = saleCounts[3];
  address[11] = saleCounts[4];
  address[12] = totalLiters;
  address[13] = totalTransactions;
}

void loadSales() {
  prefs.begin("sales", false);
  for (int i = 1; i <= NUM_VALVES; i++) {
    saleCounts[i] = prefs.getInt(("s" + String(i)).c_str(), 0);
  }
  prefs.end();
  updateSalesAddresses();
}

void saveSales() {
  prefs.begin("sales", false);
  for (int i = 1; i <= NUM_VALVES; i++) {
    prefs.putInt(("s" + String(i)).c_str(), saleCounts[i]);
  }
  prefs.end();
}

void recordSale(int valve) {
  if (valve < 1 || valve > NUM_VALVES) return;
  saleCounts[valve]++;
  updateSalesAddresses();
  saveSales();
  Serial.print("Sale recorded for valve ");
  Serial.println(valve);
}

void clearSales() {
  prefs.begin("sales", false);
  prefs.clear();
  prefs.end();
  for (int i = 1; i <= NUM_VALVES; i++) saleCounts[i] = 0;
  totalLiters = totalTransactions = 0;
  updateSalesAddresses();
  Serial.println("Statistics cleared");
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

  loadSales();

  for (int i = 1; i <= NUM_VALVES; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
    valveState[i] = 0;
  }

  Serial.println("System is Ready - Quadflow (4 valve, timer-based)");
}

void loop() {
  // Clear statistics if requested
  if (address[7] == 1) {
    clearSales();
    address[7] = 0;
  }

  // Check for valve trigger from HMI (address[1] = 1-4)
  int triggered = address[1];
  if (triggered >= 1 && triggered <= NUM_VALVES && valveState[triggered] == 0) {
    Serial.print("Valve ");
    Serial.print(triggered);
    Serial.println(" triggered");
    valveState[triggered] = 1;
    countdownStep[triggered] = 3;
    valveTimer[triggered] = millis();
    address[1 + triggered] = 1;  // tell HMI: countdown
    address[1] = 0;              // clear trigger
  } else if (triggered != 0) {
    address[1] = 0;              // clear invalid or busy trigger
  }

  // State machine for each valve
  for (int v = 1; v <= NUM_VALVES; v++) {
    switch (valveState[v]) {
      case 1:  // Countdown (3 seconds)
        if (millis() - valveTimer[v] >= 1000) {
          countdownStep[v]--;
          valveTimer[v] = millis();
          Serial.print("Valve ");
          Serial.print(v);
          Serial.print(" countdown: ");
          Serial.println(countdownStep[v]);
          if (countdownStep[v] <= 0) {
            digitalWrite(relayPins[v], HIGH);
            valveState[v] = 2;
            valveTimer[v] = millis();
            address[1 + v] = 2;  // tell HMI: dispensing
            Serial.print("Valve ");
            Serial.print(v);
            Serial.println(" dispensing...");
          }
        }
        break;

      case 2:  // Dispensing (timer-based)
        if (millis() - valveTimer[v] >= dispenseDuration) {
          digitalWrite(relayPins[v], LOW);
          valveState[v] = 3;
          valveTimer[v] = millis();
          address[1 + v] = 3;  // tell HMI: complete
          recordSale(v);
          Serial.print("Valve ");
          Serial.print(v);
          Serial.println(" dispense complete");
        }
        break;

      case 3:  // Complete screen (5 seconds then back to idle)
        if (millis() - valveTimer[v] >= 5000) {
          valveState[v] = 0;
          address[1 + v] = 0;  // tell HMI: idle
          Serial.print("Valve ");
          Serial.print(v);
          Serial.println(" back to idle");
        }
        break;
    }
  }

  // Check for SMS periodically
  if (millis() - lastSMSCheck > 15000) {
    lastSMSCheck = millis();
    processSMS();
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
  String msg = "GoWater Quadflow Report:\n";
  for (int i = 1; i <= NUM_VALVES; i++) {
    msg += "Valve" + String(i) + ": " + String(saleCounts[i]) + "x" + String(litersPerDispense) + "L\n";
  }
  msg += "Total Liters: " + String(totalLiters) + "L\n" +
    "Transactions: " + String(totalTransactions);

  gsm.print("AT+CMGS=\"" + senderNumber + "\"\r");
  delay(1000);
  gsm.print(msg);
  gsm.write(26);
  Serial.println("Sales SMS sent to: " + senderNumber);
}

void resetSalesSMS() {
  clearSales();
  String msg = "GoWater Quadflow Sales Reset!\nAll counters set to 0.";
  gsm.print("AT+CMGS=\"" + senderNumber + "\"\r");
  delay(1000);
  gsm.print(msg);
  gsm.write(26);
  Serial.println("Sales reset and SMS sent to: " + senderNumber);
}

void processSMS() {
  int availableBytes = gsm.available();
  if (availableBytes > 0) {
    delay(1500);
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
      resetSalesSMS();
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
