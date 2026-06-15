#include <ModbusRTUSlave.h>
#include <SoftwareSerial.h>
#include <Preferences.h>

#define TX 16
#define RX 17
#define relay 25
#define relay2 27
#define waterSensor 15
#define SIMRX 19
#define SIMTX 18

const int regs = 26;
uint16_t address[regs];

const int durations[] = {0, 4700, 8400, 8000};

ModbusRTUSlave hmi(Serial1);
SoftwareSerial gsm(SIMRX, SIMTX);
Preferences prefs;

String senderNumber = "";

unsigned long lastSMSCheck = 0;
const unsigned long smsIntervalSecs = 60;

int saleCount1 = 0, saleCount2 = 0, saleCount3 = 0;
int totalLiters = 0, totalTransactions = 0;

// ---------------- Function prototypes ----------------
void updateTankLevel();
void sendNoWaterSMS();
void SendATCommand(String command, unsigned long timeout);
void sendSalesSMS();
void checkAndSendSalesSMS();
void processSMS();
void recordSale(int product);
void resetRegisters();
void clearSales();
void loadSales();
void saveSales();
bool dispenseProduct(int product);
void HMICon(void* parameter);
void testCPIN();
void testCFUN();
void testCOPS();
void testCSQ();
void testNetworkRegistration();

// ========================= SETUP =========================
void setup() {
  Serial.begin(9600);
  Serial1.begin(9600, SERIAL_8N1, RX, TX);
  gsm.begin(9600);

  hmi.begin(1, 9600, SERIAL_8N1);
  hmi.configureHoldingRegisters(address, regs);

  Serial.println("Preparing System / GSM");
  delay(1000);
  SendATCommand("AT+CMGF=1\r\n", 1000);
  SendATCommand("AT+CPIN?\r\n", 1000);
  SendATCommand("AT+CNMI=2,2,0,0,0\r\n", 1000);

  testCPIN(); testCFUN(); testCOPS(); testCSQ(); testNetworkRegistration();

  xTaskCreate(HMICon, "HMI task", 4096, NULL, 1, NULL);

  for (int i = 0; i < regs; i++) address[i] = 0;
  loadSales();

  pinMode(waterSensor, INPUT_PULLUP);
  pinMode(relay, OUTPUT);
  pinMode(relay2, OUTPUT);

  digitalWrite(relay, LOW);

  // ---------- Startup priming ----------
  Serial.println("Startup priming...");
  digitalWrite(relay2, LOW);   // pump ON
  delay(1000);                 // short priming burst
  digitalWrite(relay2, HIGH);  // pump OFF
  updateTankLevel();
  Serial.println("Startup complete.");
}

// ========================= LOOP =========================
void loop() {
  // Clear-sales command from HMI
  if (address[7] == 1) { clearSales(); address[7] = 0; }

  // ------------- Transaction flow (Free dispensing) -------------
  if (address[0] == 1) {
    if (address[12] == 1) {
      Serial.println("Transaction blocked: Tank alarm active.");
      address[3] = 14;  // no water warning
      resetRegisters();
    } else if (address[1] >= 1 && address[1] <= 3) {
      // Free dispensing - go directly to dispense
      bool ok = dispenseProduct(address[1]);
      if (ok) recordSale(address[1]);
      resetRegisters();
    }
  }

  // ------------- RETEST BUTTON HANDLER -------------
  if (address[8] == 1) {
    address[8] = 0;
    Serial.println("Retest button pressed → screen 15");
    address[3] = 15; // testing screen

    digitalWrite(relay2, LOW);  // pump ON for priming
    delay(5000);                // 5-second priming test
    digitalWrite(relay2, HIGH); // stop pump

    bool tankDry = (digitalRead(waterSensor) == HIGH);
    if (tankDry) {
      address[12] = 1;
      address[3] = 14; // still dry
      Serial.println("Retest: Still no water → screen 14");
      sendNoWaterSMS();
    } else {
      address[12] = 0;
      Serial.println("Retest: Water detected → screen 0, pump ON");
    }

    // ensure pump + screen updated
    updateTankLevel();
  }

  // ------------- SMS check -------------
  if (millis() - lastSMSCheck > 30000) {
    lastSMSCheck = millis();
    processSMS();
    checkAndSendSalesSMS();
  }
}

// ========================= WATER SENSOR =========================
void updateTankLevel() {
  bool tankDry = (digitalRead(waterSensor) == HIGH); // HIGH = dry

  if (tankDry) {
    address[12] = 1;
    digitalWrite(relay2, HIGH); // pump OFF
    address[3] = 14;            // show warning
    Serial.println("Tank dry → pump OFF → screen 14");
    sendNoWaterSMS();
  } else {
    address[12] = 0;
    digitalWrite(relay2, LOW);  // pump ON
    address[3] = 0;             // home
    Serial.println("Tank OK → pump ON → screen 0");
  }
}

// ========================= DISPENSE =========================
bool dispenseProduct(int product) {
  if (product < 1 || product > 3) return false;
  if (address[12] == 1) { address[3] = 14; return false; }

  unsigned long dispenseMs = durations[product];
  address[3] = 6; delay(200);
  address[3] = 7; delay(200);
  address[3] = 8; delay(200);
  address[3] = 9; delay(200);

  digitalWrite(relay2, LOW); // pump ON
  delay(1500);               // prime
  digitalWrite(relay, HIGH); // valve open
  address[3] = 10;           // dispensing

  unsigned long start = millis();
  while (millis() - start < dispenseMs) delay(10);

  digitalWrite(relay, LOW);
  digitalWrite(relay2, HIGH); // pump OFF
  address[3] = 11;            // complete
  delay(500);
  updateTankLevel();
  return true;
}

// ========================= SALES =========================
void recordSale(int product) {
  if (product == 1) saleCount1++;
  else if (product == 2) saleCount2++;
  else if (product == 3) saleCount3++;

  totalTransactions = saleCount1 + saleCount2 + saleCount3;
  totalLiters = (saleCount1 * 1) + (saleCount2 * 5) + (saleCount3 * 19);

  address[16] = saleCount1;
  address[17] = saleCount2;
  address[18] = saleCount3;
  address[19] = totalLiters;
  address[21] = totalTransactions;
  saveSales();
}

void clearSales() {
  prefs.begin("sales", false); prefs.clear(); prefs.end();
  saleCount1 = saleCount2 = saleCount3 = 0;
  totalLiters = totalTransactions = 0;
  for (int i = 16; i <= 21; i++) address[i] = 0;
}

void loadSales() {
  prefs.begin("sales", false);
  saleCount1 = prefs.getInt("s1", 0);
  saleCount2 = prefs.getInt("s2", 0);
  saleCount3 = prefs.getInt("s3", 0);
  prefs.end();
  totalTransactions = saleCount1 + saleCount2 + saleCount3;
  totalLiters = (saleCount1 * 1) + (saleCount2 * 5) + (saleCount3 * 19);
  address[16] = saleCount1; address[17] = saleCount2; address[18] = saleCount3;
  address[19] = totalLiters; address[21] = totalTransactions;
}

void saveSales() {
  prefs.begin("sales", false);
  prefs.putInt("s1", saleCount1);
  prefs.putInt("s2", saleCount2);
  prefs.putInt("s3", saleCount3);
  prefs.end();
}

// ========================= RESET REGISTERS =========================
void resetRegisters() {
  // Preserve alarm state and screen if tank is dry
  bool tankAlarmActive = (address[12] == 1);

  for (int i = 0; i <= 6; i++) address[i] = 0;
  digitalWrite(relay, LOW);

  // If alarm is active, restore screen 14
  if (tankAlarmActive) {
    address[3] = 14;
    address[12] = 1;  // Restore alarm flag
  }
}

// ========================= GSM / SMS =========================
void sendNoWaterSMS() {
  gsm.print("AT+CMGS=\"09620650416\"\r");
  delay(1000);
  gsm.print("ALERT: Tank is empty. Pump stopped.");
  gsm.write(26);
}

void sendSalesSMS() {
  String msg = "GoWater Usage Report:\n" +
               String(saleCount1) + "x1L\n" +
               String(saleCount2) + "x5L\n" +
               String(saleCount3) + "x19L\n" +
               "Total Liters:" + String(totalLiters) + "L\n" +
               "Transactions:" + String(totalTransactions);
  gsm.print("AT+CMGS=\"09620650416\"\r");
  delay(1000); gsm.print(msg); gsm.write(26);
}

void checkAndSendSalesSMS() {
  unsigned long uptimeSecs = millis() / 1000;
  prefs.begin("sms", true);
  unsigned long lastSent = prefs.getULong("lastSent", 0);
  prefs.end();
  if ((uptimeSecs >= lastSent) && ((uptimeSecs - lastSent) >= smsIntervalSecs)) {
    sendSalesSMS();
    prefs.begin("sms", false); prefs.putULong("lastSent", uptimeSecs); prefs.end();
  }
}

void SendATCommand(String cmd, unsigned long timeout) {
  gsm.print(cmd);
  unsigned long start = millis();
  while (millis() - start < timeout)
    while (gsm.available()) Serial.print((char)gsm.read());
  Serial.println();
}

// ---------------- SMS Processing ----------------
void extractSenderInfo(String sms) {
  senderNumber = "";
  int start = sms.indexOf("+CMT: \"");
  if (start != -1) {
    int end = sms.indexOf("\"", start + 7);
    if (end != -1) senderNumber = sms.substring(start + 7, end);
  }
  if (senderNumber.length() > 0) Serial.println("Sender:" + senderNumber);
}

void processSMS() {
  int avail = gsm.available();
  if (avail > 0) {
    String sms = gsm.readString();
    extractSenderInfo(sms);
    sms.toLowerCase();
    // Only handle sales report request, no payment processing
    if (senderNumber == "+639213926986" && sms.indexOf("doit") != -1) sendSalesSMS();
  }
}

// ---------------- GSM Diagnostics ----------------
void HMICon(void* p) { for(;;){ hmi.poll(); vTaskDelay(5/portTICK_PERIOD_MS);} }
void testCPIN() { gsm.println("AT+CPIN?\r\n"); delay(500); }
void testCFUN() { gsm.println("AT+CFUN?\r\n"); delay(500); }
void testCOPS() { gsm.println("AT+COPS?\r\n"); delay(500); }
void testCSQ()  { gsm.println("AT+CSQ\r\n");  delay(500); }
void testNetworkRegistration() {
  gsm.println("AT+CREG=2\r\n"); delay(500);
  gsm.println("AT+CREG?\r\n"); delay(500);
}
