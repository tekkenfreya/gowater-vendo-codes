#include <ModbusRTUSlave.h>
#include <SoftwareSerial.h>

#define TX 16
#define RX 17
#define relay 25
#define SIMRX 19
#define SIMTX 18

const int regs = 26;
uint16_t address[regs];

const int durations[] = {0, 7500, 45000, 168000, 156000};

ModbusRTUSlave hmi(Serial1);
SoftwareSerial gsm(SIMRX, SIMTX);

String senderNumber = "";
unsigned long lastSMSCheck = 0;

int totalLiters = 0, totalTransactions = 0;

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
  delay(100);

  pinMode(relay, OUTPUT);

  // Test relay hardware
  
}

void loop() {
  // Clear statistics if requested
  if (address[7] == 1) {
    totalLiters = 0;
    totalTransactions = 0;
    address[19] = 0;
    address[21] = 0;
    address[7] = 0;
    Serial.println("Statistics cleared");
  }

  // Free dispensing - no payment required
  if (address[0] == 1 && address[1] >= 1 && address[1] <= 4) {
    Serial.print("Dispensing product: ");
    Serial.println(address[1]);
    dispenseProduct(address[1]);
    recordUsage(address[1]);

    resetRegisters();
  }

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
    case 1: totalLiters += 1; break;
    case 2: totalLiters += 5; break;
    case 3: totalLiters += 19; break;
  }

  address[19] = totalLiters;
  address[21] = totalTransactions;

  Serial.print("Total liters: ");
  Serial.println(totalLiters);
  Serial.print("Total transactions: ");
  Serial.println(totalTransactions);
}

void resetRegisters() {
  address[0] = 0;
  address[1] = 0;  // Reset product selection to prevent auto-dispense
  address[2] = 0;
  address[3] = 0;
  address[4] = 0;
  address[5] = 0;
  address[6] = 0;
  digitalWrite(relay, LOW);
  Serial.println("Registers reset - ready for next transaction");
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

void processSMS() {
  int availableBytes = gsm.available();
  if (availableBytes > 0) {
    String sms = gsm.readString();
    Serial.println("Received SMS: ");
    Serial.println(sms);
    extractSenderInfo(sms);
    sms.toLowerCase();

    // SMS processing placeholder - can be used for future features
    // (alerts, reports, remote commands, etc.)
    Serial.println("SMS received but no action configured.");
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