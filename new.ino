/*
  Proshash Nebulizer - ESP32 + 0.96" I2C OLED (SSD1306) + Buzzer + Supabase QR Connect
  -----------------------------------------------------------------------------------
  Board: ESP32 Dev Module
  Display: SSD1306 128x64 I2C OLED (0.96 inch)

  WIRING (same SDA / SCL as your old GC9A01 module):
    OLED VCC -> 3V3
    OLED GND -> GND
    OLED SDA -> GPIO 23   (this was the old module's SDA pin)
    OLED SCL -> GPIO 18   (this was the old module's SCL pin)

    If you want the classic ESP32 I2C pins instead, change only:
      OLED_SDA = 21, OLED_SCL = 22

  BUZZER:
    Buzzer + -> GPIO 13
    Buzzer - -> GND
    Set BUZZER_IS_ACTIVE below: true = active buzzer module, false = passive piezo.

  Free now (old TFT pins, nothing connected): 5, 16, 17, 4

  Required Arduino libraries:
  - Adafruit GFX Library
  - Adafruit SSD1306
  - ArduinoJson

  Note: proshash_logo.h is NOT needed anymore (it was an RGB565 colour bitmap).
  Supabase table, keys, endpoints and full session flow are unchanged.
*/

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------- WiFi + Supabase (UNCHANGED) ----------------
const char* WIFI_SSID = "Me";
const char* WIFI_PASSWORD = "mehedi113";

const char* SUPABASE_URL = "https://tormothdvqfzetcrygeb.supabase.co";
const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InRvcm1vdGhkdnFmemV0Y3J5Z2ViIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODMwNTA2NzEsImV4cCI6MjA5ODYyNjY3MX0.QcaxGNdd4MJqxl06u3QfYmHI8U49ipqv17SJL5wL2yM";

const char* DEVICE_ID = "PROSHASH-001";

// ---------------- OLED ----------------
#define OLED_SDA     23
#define OLED_SCL     18
#define OLED_W      128
#define OLED_H       64
#define OLED_RESET   -1
uint8_t OLED_ADDR = 0x3C;   // auto-falls back to 0x3D if 0x3C is not found

Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, OLED_RESET);

// ---------------- Buzzer ----------------
#define BUZZER_PIN 13
const bool BUZZER_IS_ACTIVE = false;  // true = active module, false = passive piezo
const bool BUZZER_ENABLED   = true;   // set false to mute the whole device

// ---------------- Nebulizer Control Pins (UNCHANGED) ----------------
#define RELAY_PIN 26

#define BTN_NEXT   32
#define BTN_START  33
#define BTN_STOP   25
#define BTN_MODE   27

const bool RELAY_ACTIVE_LOW = true;
const uint8_t RELAY_ON  = RELAY_ACTIVE_LOW ? LOW  : HIGH;
const uint8_t RELAY_OFF = RELAY_ACTIVE_LOW ? HIGH : LOW;

WiFiClientSecure secureClient;

// ---------------- Timer Options ----------------
const uint8_t optionCount = 5;
int timeOptions[optionCount] = {5, 10, 15, 20, 30};
int selectedIndex = 1;  // 10 min default

unsigned long totalSeconds = 600;
unsigned long remainingSeconds = 600;
unsigned long previousTick = 0;
unsigned long lastDrawnSeconds = 999999UL;
unsigned long lastPollMs = 0;
unsigned long lastWifiCheckMs = 0;

bool onlineMode = true;
bool wifiOk = false;

String activeSessionId = "";
String activeSessionCode = "";
String activePatientName = "";
String activePatientCode = "";
String activeMedicine = "";
String activeDose = "";
String runStartedAt = "";

// ---------------- Device State ----------------
enum DeviceState {
  STATE_QR_WAIT,
  STATE_READY,
  STATE_RUNNING,
  STATE_DONE,
  STATE_OFFLINE_READY,
  STATE_OFFLINE_RUNNING,
  STATE_OFFLINE_DONE
};

DeviceState state = STATE_QR_WAIT;

// ---------------- Forward declarations ----------------
void drawQrWaitScreen();
void drawReadyScreen();
void drawOfflineReadyScreen();
void drawRunningScreen();
void drawDoneScreen(bool uploaded);
void drawWiFiScreen(const String &message);
void renderRunningScreen();
void updateTimerPanel(unsigned long sec, bool running);
bool buttonPressed(uint8_t i);
void syncButtonStates();

// ---------------- Buzzer Helpers ----------------
void buzzTone(uint16_t freq, uint16_t ms) {
  if (!BUZZER_ENABLED) return;

  if (BUZZER_IS_ACTIVE) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(ms);
    digitalWrite(BUZZER_PIN, LOW);
  } else {
    tone(BUZZER_PIN, freq);
    delay(ms);
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
  }
}

void beepClick()      { buzzTone(2200, 35); }
void beepSelect()     { buzzTone(1800, 45); }
void beepConnected()  { buzzTone(1600, 70); delay(60); buzzTone(2100, 90); }
void beepStart()      { buzzTone(1200, 80); buzzTone(1600, 80); buzzTone(2000, 130); }
void beepStop()       { buzzTone(900, 90); delay(50); buzzTone(600, 160); }
void beepTick()       { buzzTone(2600, 25); }
void beepDone() {
  for (int i = 0; i < 3; i++) {
    buzzTone(2093, 140);
    delay(90);
  }
  buzzTone(2637, 260);
}
void beepBoot()  { buzzTone(1400, 60); delay(40); buzzTone(2000, 60); }
void beepError() { buzzTone(500, 300); }

// ---------------- Button Debounce ----------------
// No custom struct here on purpose: the Arduino IDE auto-generates function
// prototypes near the top of the file, so a user-defined type in a function
// signature causes "'Button' was not declared in this scope". Plain arrays
// avoid that completely.
#define B_NEXT   0
#define B_START  1
#define B_STOP   2
#define B_MODE   3
#define BTN_COUNT 4

const uint8_t btnPin[BTN_COUNT] = {BTN_NEXT, BTN_START, BTN_STOP, BTN_MODE};
bool btnLastStable[BTN_COUNT]   = {HIGH, HIGH, HIGH, HIGH};
bool btnLastReading[BTN_COUNT]  = {HIGH, HIGH, HIGH, HIGH};
unsigned long btnLastChange[BTN_COUNT] = {0, 0, 0, 0};

void syncButtonStates() {
  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    btnLastStable[i]  = digitalRead(btnPin[i]);
    btnLastReading[i] = btnLastStable[i];
    btnLastChange[i]  = millis();
  }
}

bool buttonPressed(uint8_t i) {
  bool reading = digitalRead(btnPin[i]);

  if (reading != btnLastReading[i]) {
    btnLastReading[i] = reading;
    btnLastChange[i] = millis();
  }

  if ((millis() - btnLastChange[i]) > 45) {
    if (reading != btnLastStable[i]) {
      btnLastStable[i] = reading;
      if (btnLastStable[i] == LOW) return true;
    }
  }

  return false;
}

// ---------------- Relay Helper ----------------
void setRelay(bool turnOn) {
  uint8_t level = turnOn ? RELAY_ON : RELAY_OFF;
  digitalWrite(RELAY_PIN, level);

  Serial.print("[RELAY] ");
  Serial.print(turnOn ? "ON" : "OFF");
  Serial.print(" | GPIO ");
  Serial.print(RELAY_PIN);
  Serial.print(" = ");
  Serial.println(level == HIGH ? "HIGH" : "LOW");
}

// ---------------- Text Helpers ----------------
String formatTime(unsigned long sec) {
  int minutes = sec / 60;
  int seconds = sec % 60;

  String out = "";
  if (minutes < 10) out += "0";
  out += String(minutes);
  out += ":";
  if (seconds < 10) out += "0";
  out += String(seconds);
  return out;
}

String isoNow() {
  time_t now;
  time(&now);
  if (now < 1700000000) return "";

  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

String cleanShort(const String &s, int maxLen) {
  String out = s;
  out.replace("\n", " ");
  out.replace("\r", " ");
  if ((int)out.length() > maxLen) out = out.substring(0, maxLen - 2) + "..";
  return out;
}

void centerText(const String &text, int y, uint8_t size, uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;
  oled.setTextSize(size);
  oled.setTextColor(color);
  oled.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  int x = (OLED_W - (int)w) / 2;
  if (x < 0) x = 0;
  oled.setCursor(x, y);
  oled.print(text);
}

// ---------------- UI Sections ----------------
void drawTopBar(const String &title) {
  oled.fillRect(0, 0, OLED_W, 11, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_BLACK);
  oled.setCursor(3, 2);
  oled.print(title);

  // right side: mode + wifi indicator
  oled.setCursor(OLED_W - 33, 2);
  if (onlineMode) oled.print(wifiOk ? "ON W" : "ON  ");
  else            oled.print("OFFL");
}

void drawOptionChips(int y) {
  const int w = 22;
  const int gap = 4;
  const int h = 11;
  const int x0 = (OLED_W - (optionCount * w + (optionCount - 1) * gap)) / 2;

  for (int i = 0; i < optionCount; i++) {
    int x = x0 + i * (w + gap);
    bool selected = (i == selectedIndex);

    String label = String(timeOptions[i]);
    int16_t bx, by;
    uint16_t bw, bh;
    oled.setTextSize(1);
    oled.getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);

    if (selected) {
      oled.fillRoundRect(x, y, w, h, 3, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    } else {
      oled.drawRoundRect(x, y, w, h, 3, SSD1306_WHITE);
      oled.setTextColor(SSD1306_WHITE);
    }

    oled.setCursor(x + (w - (int)bw) / 2, y + 2);
    oled.print(label);
  }
}

void drawProgressBar(int y) {
  oled.drawRect(2, y, OLED_W - 4, 6, SSD1306_WHITE);
  if (totalSeconds == 0) return;

  unsigned long elapsed = totalSeconds - remainingSeconds;
  int fill = (int)(((unsigned long)(OLED_W - 8) * elapsed) / totalSeconds);
  if (fill > 0) oled.fillRect(4, y + 2, fill, 2, SSD1306_WHITE);
}

// ---------------- QR Drawing ----------------
// Static QR for DEVICE_ID = "PROSHASH-001".
// Dark modules are drawn BLACK on a WHITE (lit) background so a phone camera
// sees a normal dark-on-light QR.
const uint8_t QR_STATIC_SIZE = 21;
const char QR_PROSHASH_001[QR_STATIC_SIZE][QR_STATIC_SIZE + 1] = {
  "111111100101101111111",
  "100000100111001000001",
  "101110101101101011101",
  "101110100101001011101",
  "101110100010101011101",
  "100000100000101000001",
  "111111101010101111111",
  "000000001101100000000",
  "111011111111011000100",
  "101111010000010101110",
  "001111111000101010101",
  "011100010010010001110",
  "111100110010100110000",
  "000000001101010011110",
  "111111101001001010111",
  "100000101001110010111",
  "101110101001001100111",
  "101110100110001111010",
  "101110101000100101101",
  "100000101010001010111",
  "111111101100101110001"
};

void drawQRCodeBlock(int x0, int y0) {
  const int scale = 2;
  const int quiet = 2;                       // quiet modules on each side
  const int total = QR_STATIC_SIZE + quiet * 2;
  const int px = total * scale;              // 50 px

  oled.fillRect(x0, y0, px, px, SSD1306_WHITE);

  for (uint8_t y = 0; y < QR_STATIC_SIZE; y++) {
    for (uint8_t x = 0; x < QR_STATIC_SIZE; x++) {
      if (QR_PROSHASH_001[y][x] == '1') {
        oled.fillRect(x0 + (x + quiet) * scale,
                      y0 + (y + quiet) * scale,
                      scale, scale, SSD1306_BLACK);
      }
    }
  }
}

// ---------------- Screens ----------------
void drawQrWaitScreen() {
  oled.clearDisplay();
  drawTopBar("QR CONNECT");

  drawQRCodeBlock(2, 13);          // 50x50 block, left side

  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(58, 15);
  oled.print("Scan QR");
  oled.setCursor(58, 26);
  oled.print("or type");
  oled.setCursor(58, 37);
  oled.print("PROSHASH");
  oled.setCursor(58, 48);
  oled.print("-001");

  oled.display();
}

void drawReadyScreen() {
  totalSeconds = (unsigned long)timeOptions[selectedIndex] * 60UL;
  remainingSeconds = totalSeconds;

  oled.clearDisplay();
  drawTopBar("CONNECTED");

  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 13);
  oled.print(cleanShort(activePatientName.length() ? activePatientName : "Patient", 21));
  oled.setCursor(2, 23);
  oled.print(cleanShort(activeMedicine.length() ? activeMedicine : "Medicine", 21));

  centerText(formatTime(remainingSeconds), 33, 2, SSD1306_WHITE);
  drawOptionChips(52);

  oled.display();
}

void drawOfflineReadyScreen() {
  totalSeconds = (unsigned long)timeOptions[selectedIndex] * 60UL;
  remainingSeconds = totalSeconds;

  oled.clearDisplay();
  drawTopBar("OFFLINE");

  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 14);
  oled.print("Manual therapy");

  centerText(formatTime(remainingSeconds), 26, 2, SSD1306_WHITE);

  oled.setTextSize(1);
  oled.setCursor(2, 44);
  oled.print("NEXT=time START=go");

  drawOptionChips(52);
  oled.display();
}

void renderRunningScreen() {
  oled.clearDisplay();
  drawTopBar(onlineMode ? "RUNNING" : "RUN OFFLINE");

  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  if (onlineMode && activePatientName.length()) {
    oled.setCursor(2, 13);
    oled.print(cleanShort(activePatientName, 21));
  } else {
    oled.setCursor(2, 13);
    oled.print("Nebulizer active");
  }

  centerText(formatTime(remainingSeconds), 25, 3, SSD1306_WHITE);

  drawProgressBar(52);

  oled.display();
}

void drawRunningScreen() {
  lastDrawnSeconds = 999999UL;
  renderRunningScreen();
  lastDrawnSeconds = remainingSeconds;
}

void updateTimerPanel(unsigned long sec, bool running) {
  if (sec == lastDrawnSeconds && running) return;
  renderRunningScreen();
  lastDrawnSeconds = sec;
}

void drawDoneScreen(bool uploaded) {
  oled.clearDisplay();
  drawTopBar("DONE");

  centerText("THERAPY", 15, 2, SSD1306_WHITE);
  centerText("COMPLETE", 33, 2, SSD1306_WHITE);

  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  if (onlineMode) {
    centerText(uploaded ? "Saved to cloud" : "Upload failed", 54, 1, SSD1306_WHITE);
  } else {
    centerText("Offline, not saved", 54, 1, SSD1306_WHITE);
  }

  oled.display();
}

void drawWiFiScreen(const String &message) {
  oled.clearDisplay();
  drawTopBar("PROSHASH");

  centerText("WiFi", 18, 2, SSD1306_WHITE);
  centerText(message, 38, 1, SSD1306_WHITE);
  centerText(cleanShort(String(WIFI_SSID), 21), 50, 1, SSD1306_WHITE);

  oled.display();
}

// ---------------- WiFi + Supabase (UNCHANGED LOGIC) ----------------
bool connectWiFi(uint16_t timeoutMs = 12000) {
  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
  }

  wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  }
  return wifiOk;
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    return true;
  }
  wifiOk = false;
  return connectWiFi(4500);
}

String supabaseRequest(const String &method, const String &path, const String &body = "") {
  if (!ensureWiFi()) return "";

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/" + path;

  secureClient.setInsecure();
  if (!http.begin(secureClient, url)) return "";

  http.setTimeout(8000);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=representation");

  int code = 0;
  if (method == "GET") code = http.GET();
  else if (method == "PATCH") code = http.PATCH(body);
  else if (method == "POST") code = http.POST(body);
  else {
    http.end();
    return "";
  }

  String payload = http.getString();
  http.end();

  if (code < 200 || code >= 300) return "";
  return payload;
}

void clearActiveSession() {
  activeSessionId = "";
  activeSessionCode = "";
  activePatientName = "";
  activePatientCode = "";
  activeMedicine = "";
  activeDose = "";
  runStartedAt = "";
}

bool pollAssignedSession() {
  String path = "therapy_sessions?device_id=eq." + String(DEVICE_ID) + "&status=eq.assigned&select=*&order=assigned_at.desc&limit=1";
  String res = supabaseRequest("GET", path);
  if (res.length() < 5) return false;

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, res);
  if (err || !doc.is<JsonArray>() || doc.size() == 0) return false;

  JsonObject row = doc[0];
  activeSessionId = row["id"].as<String>();
  activeSessionCode = row["session_code"].as<String>();
  activePatientName = row["patient_name"].as<String>();
  activePatientCode = row["patient_code"].as<String>();
  activeMedicine = row["medicine_name"].as<String>();
  activeDose = row["dose"].as<String>();

  return activeSessionId.length() > 0;
}

bool patchSessionRunning() {
  if (!activeSessionId.length()) return false;

  StaticJsonDocument<512> doc;
  doc["status"] = "running";
  doc["selected_duration_min"] = timeOptions[selectedIndex];
  runStartedAt = isoNow();
  if (runStartedAt.length()) doc["started_at"] = runStartedAt;

  String body;
  serializeJson(doc, body);
  String path = "therapy_sessions?id=eq." + activeSessionId;
  String res = supabaseRequest("PATCH", path, body);
  return res.length() > 0;
}

bool patchSessionCompleted() {
  if (!activeSessionId.length()) return false;

  StaticJsonDocument<512> doc;
  doc["status"] = "completed";
  doc["selected_duration_min"] = timeOptions[selectedIndex];
  doc["actual_duration_sec"] = (int)totalSeconds;

  String completed = isoNow();
  if (completed.length()) doc["completed_at"] = completed;
  if (runStartedAt.length()) doc["started_at"] = runStartedAt;

  String body;
  serializeJson(doc, body);
  String path = "therapy_sessions?id=eq." + activeSessionId;
  String res = supabaseRequest("PATCH", path, body);
  return res.length() > 0;
}

// ---------------- Device Actions ----------------
void startTherapy() {
  totalSeconds = (unsigned long)timeOptions[selectedIndex] * 60UL;
  remainingSeconds = totalSeconds;
  previousTick = millis();
  lastDrawnSeconds = 999999UL;

  Serial.print("[THERAPY] Start, duration = ");
  Serial.print(timeOptions[selectedIndex]);
  Serial.println(" minute(s)");

  beepStart();

  if (onlineMode && activeSessionId.length()) {
    bool updated = patchSessionRunning();
    Serial.println(updated ? "[SUPABASE] Session marked running"
                           : "[SUPABASE] Running update failed");
  }

  setRelay(true);
  state = onlineMode ? STATE_RUNNING : STATE_OFFLINE_RUNNING;

  previousTick = millis();   // re-sync after network call
  drawRunningScreen();
}

void stopTherapy() {
  Serial.println("[THERAPY] Stopped/reset by user");
  setRelay(false);
  beepStop();

  if (onlineMode) {
    clearActiveSession();
    state = STATE_QR_WAIT;
    drawQrWaitScreen();
  } else {
    state = STATE_OFFLINE_READY;
    drawOfflineReadyScreen();
  }
}

void finishTherapy() {
  Serial.println("[THERAPY] Countdown complete");
  setRelay(false);

  bool uploaded = false;
  if (onlineMode && activeSessionId.length()) {
    uploaded = patchSessionCompleted();
  }

  state = onlineMode ? STATE_DONE : STATE_OFFLINE_DONE;
  drawDoneScreen(uploaded);
  beepDone();
}

void toggleMode() {
  if (state == STATE_RUNNING || state == STATE_OFFLINE_RUNNING) return;

  setRelay(false);
  onlineMode = !onlineMode;
  clearActiveSession();
  lastDrawnSeconds = 999999UL;
  beepSelect();

  if (onlineMode) {
    state = STATE_QR_WAIT;
    drawQrWaitScreen();
  } else {
    state = STATE_OFFLINE_READY;
    drawOfflineReadyScreen();
  }
}

// ---------------- Arduino Setup/Loop ----------------
void setup() {
  // Force relay OFF as early as possible at boot.
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println("=== Proshash Nebulizer boot (OLED build) ===");
  Serial.print("Relay trigger mode: ");
  Serial.println(RELAY_ACTIVE_LOW ? "ACTIVE LOW" : "ACTIVE HIGH");
  setRelay(false);

  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_START, INPUT_PULLUP);
  pinMode(BTN_STOP, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);
  syncButtonStates();

  // I2C on the same physical wires as the old display module
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    OLED_ADDR = 0x3D;
    if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      Serial.println("[OLED] Not found on 0x3C or 0x3D. Check SDA/SCL wiring.");
      beepError();
      while (true) delay(1000);
    }
  }

  oled.clearDisplay();
  oled.setTextWrap(false);
  oled.display();

  beepBoot();

  centerText("PROSHASH", 18, 2, SSD1306_WHITE);
  centerText("Nebulizer Therapy", 42, 1, SSD1306_WHITE);
  oled.display();
  delay(1200);

  drawWiFiScreen("Connecting");
  connectWiFi();
  if (!wifiOk) beepError();

  onlineMode = true;
  state = STATE_QR_WAIT;
  drawQrWaitScreen();
}

void loop() {
  // MODE changes between QR/online and local/offline mode.
  if (buttonPressed(B_MODE)) {
    Serial.println("[BUTTON] MODE");
    toggleMode();
    delay(150);
    return;
  }

  if (millis() - lastWifiCheckMs > 10000) {
    lastWifiCheckMs = millis();
    bool prev = wifiOk;
    wifiOk = (WiFi.status() == WL_CONNECTED);
    if (prev != wifiOk && state == STATE_QR_WAIT) drawQrWaitScreen();
  }

  // ------------------------------------------------------------
  // QR WAIT
  // ------------------------------------------------------------
  if (state == STATE_QR_WAIT) {
    if (buttonPressed(B_START)) {
      Serial.println("[BUTTON] START in QR screen -> manual/offline start");
      beepClick();
      onlineMode = false;
      clearActiveSession();
      state = STATE_OFFLINE_READY;
      startTherapy();
      return;
    }

    if (buttonPressed(B_NEXT)) {
      Serial.println("[BUTTON] NEXT in QR screen -> select manual time");
      beepSelect();
      selectedIndex++;
      if (selectedIndex >= optionCount) selectedIndex = 0;

      onlineMode = false;
      clearActiveSession();
      state = STATE_OFFLINE_READY;
      drawOfflineReadyScreen();
      return;
    }

    if (buttonPressed(B_STOP)) {
      Serial.println("[BUTTON] STOP in QR screen");
      beepClick();
      setRelay(false);
      drawQrWaitScreen();
      return;
    }

    if (millis() - lastPollMs > 5000) {
      lastPollMs = millis();
      if (pollAssignedSession()) {
        Serial.println("[SUPABASE] Assigned session found");
        state = STATE_READY;
        drawReadyScreen();
        beepConnected();
      }
    }
  }

  // ------------------------------------------------------------
  // READY states
  // ------------------------------------------------------------
  else if (state == STATE_READY || state == STATE_OFFLINE_READY) {
    if (buttonPressed(B_NEXT)) {
      Serial.println("[BUTTON] NEXT");
      beepSelect();
      selectedIndex++;
      if (selectedIndex >= optionCount) selectedIndex = 0;

      Serial.print("[TIMER] Selected ");
      Serial.print(timeOptions[selectedIndex]);
      Serial.println(" minute(s)");

      if (onlineMode) drawReadyScreen();
      else drawOfflineReadyScreen();
      return;
    }

    if (buttonPressed(B_START)) {
      Serial.println("[BUTTON] START");
      startTherapy();
      return;
    }

    if (buttonPressed(B_STOP)) {
      Serial.println("[BUTTON] STOP/RESET");
      beepStop();
      setRelay(false);

      if (onlineMode) {
        clearActiveSession();
        state = STATE_QR_WAIT;
        drawQrWaitScreen();
      } else {
        state = STATE_OFFLINE_READY;
        drawOfflineReadyScreen();
      }
      return;
    }
  }

  // ------------------------------------------------------------
  // RUNNING states
  // ------------------------------------------------------------
  else if (state == STATE_RUNNING || state == STATE_OFFLINE_RUNNING) {
    if (buttonPressed(B_STOP)) {
      Serial.println("[BUTTON] STOP while running");
      stopTherapy();
      return;
    }

    if (buttonPressed(B_NEXT)) {
      Serial.println("[BUTTON] NEXT ignored while running");
    }
    if (buttonPressed(B_START)) {
      Serial.println("[BUTTON] START ignored; already running");
    }

    unsigned long nowMs = millis();
    bool ticked = false;
    while (remainingSeconds > 0 && nowMs - previousTick >= 1000) {
      previousTick += 1000;
      remainingSeconds--;
      ticked = true;
    }

    updateTimerPanel(remainingSeconds, true);

    // Last 10 seconds warning beeps
    if (ticked && remainingSeconds > 0 && remainingSeconds <= 10) {
      beepTick();
    }

    if (remainingSeconds == 0) {
      finishTherapy();
      return;
    }
  }

  // ------------------------------------------------------------
  // DONE states
  // ------------------------------------------------------------
  else if (state == STATE_DONE || state == STATE_OFFLINE_DONE) {
    if (buttonPressed(B_START)) {
      Serial.println("[BUTTON] START after completion");
      beepClick();

      if (onlineMode) {
        clearActiveSession();
        state = STATE_QR_WAIT;
        drawQrWaitScreen();
      } else {
        state = STATE_OFFLINE_READY;
        startTherapy();
      }
      return;
    }

    if (buttonPressed(B_STOP)) {
      Serial.println("[BUTTON] STOP after completion");
      beepClick();
      setRelay(false);

      if (onlineMode) {
        clearActiveSession();
        state = STATE_QR_WAIT;
        drawQrWaitScreen();
      } else {
        state = STATE_OFFLINE_READY;
        drawOfflineReadyScreen();
      }
      return;
    }
  }
}
