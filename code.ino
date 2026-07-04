/*
  Proshash Nebulizer - ESP32 + GC9A01 UI + Supabase QR Connect
  --------------------------------------------------------------
  Board: ESP32 Dev Module
  Display: GC9A01A 240x240 round TFT

  Flow:
  1) QR CONNECT mode: display auto-shows QR with DEVICE_ID.
  2) Website scans the QR and inserts an assigned therapy_sessions row in Supabase.
  3) ESP32 polls Supabase. When assigned row is found, patient/medicine appears on TFT.
  4) User selects time on ESP32, presses START, relay turns ON and countdown starts.
  5) On complete, relay turns OFF and Supabase row is updated to completed.
  6) MODE button switches between QR CONNECT and OFFLINE timer mode.
  7) START on the QR screen also starts manual/offline therapy directly.

  Required Arduino libraries:
  - Adafruit GFX Library
  - Adafruit GC9A01A
  - ArduinoJson
  - No QR library needed; QR for PROSHASH-001 is built in

  Important:
  - Put proshash_logo.h in the same folder as this .ino file.
  - Change WIFI_SSID and WIFI_PASSWORD below.
*/

#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include "proshash_logo.h"

// ---------------- WiFi + Supabase ----------------
const char* WIFI_SSID = "Me";
const char* WIFI_PASSWORD = "mehedi113";

const char* SUPABASE_URL = "https://tormothdvqfzetcrygeb.supabase.co";
const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InRvcm1vdGhkdnFmemV0Y3J5Z2ViIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODMwNTA2NzEsImV4cCI6MjA5ODYyNjY3MX0.QcaxGNdd4MJqxl06u3QfYmHI8U49ipqv17SJL5wL2yM";

const char* DEVICE_ID = "PROSHASH-001";

// ---------------- TFT Pins ----------------
#define TFT_CS   5
#define TFT_DC   16
#define TFT_RST  17
#define TFT_BL   4

#define TFT_SCK  18
#define TFT_MOSI 23

// ---------------- Nebulizer Control Pins ----------------
#define RELAY_PIN 26

#define BTN_NEXT   32
#define BTN_START  33
#define BTN_STOP   25
#define BTN_MODE   27   // New button: connect to GND, uses INPUT_PULLUP

// Relay trigger type:
// true  = relay turns ON when GPIO is LOW (most common relay modules)
// false = relay turns ON when GPIO is HIGH
// If the relay never clicks, change only this value and upload again.
const bool RELAY_ACTIVE_LOW = true;
const uint8_t RELAY_ON  = RELAY_ACTIVE_LOW ? LOW  : HIGH;
const uint8_t RELAY_OFF = RELAY_ACTIVE_LOW ? HIGH : LOW;

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);
WiFiClientSecure secureClient;

// ---------------- Medical UI Palette ----------------
uint16_t C_BG;
uint16_t C_SURFACE;
uint16_t C_INK;
uint16_t C_TEXT;
uint16_t C_MUTED;
uint16_t C_LINE;
uint16_t C_SOFT_BLUE;
uint16_t C_RING_BG;
uint16_t C_BLUE;
uint16_t C_TEAL;
uint16_t C_GREEN;
uint16_t C_RED;
uint16_t C_ORANGE;

// ---------------- Timer Options ----------------
const uint8_t optionCount = 5;
int timeOptions[optionCount] = {5, 10, 15, 20, 30};
int selectedIndex = 1;  // 10 min selected by default

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

// ---------------- Button Debounce ----------------
struct Button {
  uint8_t pin;
  bool lastStable;
  bool lastReading;
  unsigned long lastChange;
};

Button nextBtn  = {BTN_NEXT,  HIGH, HIGH, 0};
Button startBtn = {BTN_START, HIGH, HIGH, 0};
Button stopBtn  = {BTN_STOP,  HIGH, HIGH, 0};
Button modeBtn  = {BTN_MODE,  HIGH, HIGH, 0};


void syncButtonStates() {
  nextBtn.lastStable = digitalRead(BTN_NEXT);
  nextBtn.lastReading = nextBtn.lastStable;
  nextBtn.lastChange = millis();

  startBtn.lastStable = digitalRead(BTN_START);
  startBtn.lastReading = startBtn.lastStable;
  startBtn.lastChange = millis();

  stopBtn.lastStable = digitalRead(BTN_STOP);
  stopBtn.lastReading = stopBtn.lastStable;
  stopBtn.lastChange = millis();

  modeBtn.lastStable = digitalRead(BTN_MODE);
  modeBtn.lastReading = modeBtn.lastStable;
  modeBtn.lastChange = millis();
}

bool buttonPressed(Button &btn) {
  bool reading = digitalRead(btn.pin);

  if (reading != btn.lastReading) {
    btn.lastReading = reading;
    btn.lastChange = millis();
  }

  if ((millis() - btn.lastChange) > 45) {
    if (reading != btn.lastStable) {
      btn.lastStable = reading;
      if (btn.lastStable == LOW) return true;
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

// ---------------- Drawing Helpers ----------------
void initColors() {
  C_BG        = tft.color565(247, 251, 255);
  C_SURFACE   = tft.color565(255, 255, 255);
  C_INK       = tft.color565(7, 42, 92);
  C_TEXT      = tft.color565(72, 90, 112);
  C_MUTED     = tft.color565(136, 158, 180);
  C_LINE      = tft.color565(188, 211, 233);
  C_SOFT_BLUE = tft.color565(234, 245, 252);
  C_RING_BG   = tft.color565(216, 235, 246);
  C_BLUE      = tft.color565(0, 101, 210);
  C_TEAL      = tft.color565(0, 164, 166);
  C_GREEN     = tft.color565(0, 150, 105);
  C_RED       = tft.color565(204, 38, 38);
  C_ORANGE    = tft.color565(230, 130, 20);
}

void centerText(const String &text, int y, uint8_t size, uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(size);
  tft.setTextColor(color, C_BG);
  tft.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  tft.setCursor((240 - w) / 2, y);
  tft.print(text);
}

void centerTextOn(uint16_t bg, const String &text, int y, uint8_t size, uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(size);
  tft.setTextColor(color, bg);
  tft.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  tft.setCursor((240 - w) / 2, y);
  tft.print(text);
}

void drawCheckIcon(int cx, int cy, uint16_t color) {
  tft.drawLine(cx - 5, cy,     cx - 1, cy + 4, color);
  tft.drawLine(cx - 1, cy + 4, cx + 7, cy - 6, color);
}

void drawPlayIcon(int cx, int cy, uint16_t color) {
  tft.fillTriangle(cx - 3, cy - 6, cx - 3, cy + 6, cx + 7, cy, color);
}

void drawStopIcon(int cx, int cy, uint16_t color) {
  tft.fillRect(cx - 5, cy - 5, 10, 10, color);
}

void drawMistIcon(int x, int y, uint16_t color) {
  tft.drawRoundRect(x, y + 12, 18, 24, 4, color);
  tft.drawLine(x + 4, y + 12, x + 4, y + 5, color);
  tft.drawLine(x + 4, y + 5, x + 19, y + 5, color);
  tft.drawRoundRect(x + 18, y + 2, 18, 8, 3, color);
  tft.drawLine(x + 9, y + 20, x + 6, y + 29, color);

  tft.fillCircle(x + 45, y + 7, 2, C_LINE);
  tft.fillCircle(x + 53, y + 13, 2, C_LINE);
  tft.fillCircle(x + 43, y + 18, 2, C_LINE);
  tft.fillCircle(x + 57, y + 23, 2, C_LINE);
}

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
  if (now < 1700000000) {
    return "";
  }

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
  if (out.length() > maxLen) out = out.substring(0, maxLen - 2) + "..";
  return out;
}

// ---------------- UI Sections ----------------
void drawHeader() {
  tft.drawCircle(120, 120, 118, C_LINE);
  tft.drawCircle(120, 120, 116, C_SOFT_BLUE);

  tft.drawRGBBitmap((240 - PROSHASH_LOGO_W) / 2, 5, proshashLogo, proshashLogoMask, PROSHASH_LOGO_W, PROSHASH_LOGO_H);
  centerText("Nebulizer Therapy", 42, 1, C_TEXT);
}

void drawStatusPill(const String &label, uint16_t fillColor) {
  int x = 54;
  int y = 55;
  int w = 132;
  int h = 24;

  tft.fillRoundRect(x, y, w, h, 12, fillColor);
  tft.fillCircle(x + 17, y + 12, 9, C_SURFACE);

  if (label == "Ready" || label == "Done" || label == "Connected") {
    drawCheckIcon(x + 17, y + 12, fillColor);
  } else {
    tft.fillCircle(x + 17, y + 12, 3, fillColor);
  }

  tft.setTextSize(1);
  tft.setTextColor(C_SURFACE, fillColor);
  int16_t bx, by;
  uint16_t bw, bh;
  tft.getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);
  tft.setCursor(x + 34, y + 8);
  tft.print(label);
}

void drawModeTag() {
  tft.setTextSize(1);
  tft.setTextColor(onlineMode ? C_GREEN : C_ORANGE, C_BG);
  tft.setCursor(12, 222);
  tft.print(onlineMode ? "QR MODE" : "OFFLINE");

  tft.setTextColor(wifiOk ? C_GREEN : C_RED, C_BG);
  tft.setCursor(178, 222);
  tft.print(wifiOk ? "WiFi OK" : "No WiFi");
}

void drawSmallFooter() {
  tft.setTextSize(1);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setCursor(53, 207);
  tft.print("MODE NEXT START STOP");
  drawModeTag();
}

void drawTimerTextOnly(unsigned long sec) {
  tft.fillRoundRect(47, 106, 146, 50, 14, C_BG);
  centerText(formatTime(sec), 111, 4, C_INK);
  centerText("Min", 151, 1, C_TEXT);
}

void drawTimerStaticPanel() {
  centerText(" ", 91, 1, C_TEXT);
  lastDrawnSeconds = 999999UL;
}

void updateTimerPanel(unsigned long sec, bool running) {
  if (sec == lastDrawnSeconds && running) return;
  drawTimerTextOnly(sec);
  lastDrawnSeconds = sec;
}

void drawTimerPanel(unsigned long sec, bool running) {
  drawTimerStaticPanel();
  updateTimerPanel(sec, running);
}

void drawOptionButtons() {
  int y = 165;
  int w = 36;
  int h = 24;
  int gap = 6;
  int x0 = 18;

  for (int i = 0; i < optionCount; i++) {
    int x = x0 + i * (w + gap);
    bool selected = (i == selectedIndex);

    if (selected) {
      tft.fillRoundRect(x, y, w, h, 7, C_BLUE);
      tft.drawRoundRect(x - 2, y - 2, w + 4, h + 4, 9, C_BLUE);
      tft.setTextColor(C_SURFACE, C_BLUE);
    } else {
      tft.fillRoundRect(x, y, w, h, 7, C_SURFACE);
      tft.drawRoundRect(x, y, w, h, 7, C_LINE);
      tft.setTextColor(C_INK, C_SURFACE);
    }

    tft.setTextSize(1);
    String label = String(timeOptions[i]);
    int16_t bx, by;
    uint16_t bw, bh;
    tft.getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);
    tft.setCursor(x + (w - bw) / 2, y + 8);
    tft.print(label);
  }
}

void drawActionBar(bool running) {
  int y = 193;
  int w = 58;
  int h = 26;
  int x1 = 57;
  int x2 = 125;

  if (running) {
    tft.fillRoundRect(x1, y, w, h, 9, C_SOFT_BLUE);
    tft.drawRoundRect(x1, y, w, h, 9, C_LINE);
    tft.setTextColor(C_MUTED, C_SOFT_BLUE);
    drawPlayIcon(x1 + 15, y + 13, C_MUTED);
    tft.setTextSize(1);
    tft.setCursor(x1 + 26, y + 9);
    tft.print("Start");

    tft.fillRoundRect(x2, y, w, h, 9, C_SURFACE);
    tft.drawRoundRect(x2, y, w, h, 9, C_RED);
    drawStopIcon(x2 + 15, y + 13, C_RED);
    tft.setTextColor(C_RED, C_SURFACE);
    tft.setCursor(x2 + 24, y + 9);
    tft.print("Reset");
  } else {
    tft.fillRoundRect(x1, y, w, h, 9, C_TEAL);
    drawPlayIcon(x1 + 15, y + 13, C_SURFACE);
    tft.setTextSize(1);
    tft.setTextColor(C_SURFACE, C_TEAL);
    tft.setCursor(x1 + 26, y + 9);
    tft.print("Start");

    tft.fillRoundRect(x2, y, w, h, 9, C_SURFACE);
    tft.drawRoundRect(x2, y, w, h, 9, C_LINE);
    drawStopIcon(x2 + 15, y + 13, C_RED);
    tft.setTextColor(C_RED, C_SURFACE);
    tft.setCursor(x2 + 24, y + 9);
    tft.print("Reset");
  }
}

// ---------------- QR Drawing ----------------
// No external QR library is used here. This avoids compile errors from different
// Arduino QR libraries having different APIs.
//
// This static QR is for DEVICE_ID = "PROSHASH-001".
// If you change DEVICE_ID, either regenerate this matrix or use manual device-ID
// entry on the website.
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

void drawQRCodeBlock(const String &text) {
  const int scale = 4;
  const int quietModules = 4;
  const int totalModules = QR_STATIC_SIZE + (quietModules * 2);
  const int qrSize = totalModules * scale;
  const int x0 = (240 - qrSize) / 2;
  const int y0 = 82;

  tft.fillRoundRect(x0 - 7, y0 - 7, qrSize + 14, qrSize + 14, 12, C_SURFACE);
  tft.drawRoundRect(x0 - 7, y0 - 7, qrSize + 14, qrSize + 14, 12, C_LINE);

  // White quiet zone for reliable scanning.
  tft.fillRect(x0, y0, qrSize, qrSize, C_SURFACE);

  for (uint8_t y = 0; y < QR_STATIC_SIZE; y++) {
    for (uint8_t x = 0; x < QR_STATIC_SIZE; x++) {
      if (QR_PROSHASH_001[y][x] == '1') {
        tft.fillRect(
          x0 + (x + quietModules) * scale,
          y0 + (y + quietModules) * scale,
          scale,
          scale,
          C_INK
        );
      }
    }
  }

  if (text != "PROSHASH-001") {
    // QR is still PROSHASH-001. This warning prevents silent mismatch.
    tft.fillRoundRect(44, 204, 152, 13, 5, C_ORANGE);
    tft.setTextSize(1);
    tft.setTextColor(C_SURFACE, C_ORANGE);
    tft.setCursor(55, 207);
    tft.print("QR fixed: PROSHASH-001");
  }
}

// ---------------- Screens ----------------
void drawQrWaitScreen() {
  tft.fillScreen(C_BG);
  drawHeader();
  drawStatusPill("QR Connect", onlineMode ? C_TEAL : C_MUTED);
  centerText("Scan this QR", 80, 1, C_INK);
  drawQRCodeBlock(String(DEVICE_ID));
  centerText(String(DEVICE_ID), 194, 1, C_BLUE);
  drawSmallFooter();
}

void drawReadyScreen() {
  tft.fillScreen(C_BG);
  drawHeader();
  drawStatusPill("Connected", C_GREEN);

  totalSeconds = (unsigned long)timeOptions[selectedIndex] * 60UL;
  remainingSeconds = totalSeconds;

  centerText(cleanShort(activePatientName.length() ? activePatientName : "Patient", 22), 82, 1, C_INK);
  centerText(cleanShort(activeMedicine.length() ? activeMedicine : "Medicine assigned", 24), 96, 1, C_BLUE);
  centerText(cleanShort(activeDose, 24), 110, 1, C_MUTED);

  drawTimerPanel(remainingSeconds, false);
  drawOptionButtons();
  drawActionBar(false);
}

void drawOfflineReadyScreen() {
  tft.fillScreen(C_BG);
  drawHeader();
  drawStatusPill("Offline Mode", C_ORANGE);

  totalSeconds = (unsigned long)timeOptions[selectedIndex] * 60UL;
  remainingSeconds = totalSeconds;

  centerText(" ", 90, 2, C_INK);
  drawTimerPanel(remainingSeconds, false);
  drawOptionButtons();
  drawActionBar(false);
}

void drawRunningScreen() {
  tft.fillScreen(C_BG);
  drawHeader();
  drawStatusPill(onlineMode ? "Running" : "Offline Run", C_TEAL);

  if (onlineMode) {
    centerText(cleanShort(activePatientName, 24), 82, 1, C_INK);
    centerText(cleanShort(activeMedicine, 24), 96, 1, C_BLUE);
  } else {
    centerText(" ", 90, 1, C_INK);
  }

  drawTimerPanel(remainingSeconds, true);
  drawOptionButtons();
  drawActionBar(true);
}

void drawDoneScreen(bool uploaded) {
  tft.fillScreen(C_BG);
  drawHeader();
  drawStatusPill("Done", uploaded || !onlineMode ? C_GREEN : C_RED);

  centerText("THERAPY", 92, 2, C_INK);
  centerText("COMPLETE", 116, 2, C_GREEN);
  if (onlineMode) {
    centerText(uploaded ? "Saved to Supabase" : "Upload failed", 146, 1, uploaded ? C_GREEN : C_RED);
    centerText(cleanShort(activeSessionCode, 26), 162, 1, C_BLUE);
  } else {
    centerText("Offline log not saved", 150, 1, C_ORANGE);
  }
  centerText("START/STOP = New", 190, 1, C_MUTED);
  drawSmallFooter();
}

void drawWiFiScreen(const String &message) {
  tft.fillScreen(C_BG);
  drawHeader();
  drawStatusPill("WiFi", C_BLUE);
  centerText(message, 112, 2, C_INK);
  centerText(String(WIFI_SSID), 143, 1, C_MUTED);
}

// ---------------- WiFi + Supabase ----------------
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

  secureClient.setInsecure();  // Simple prototype TLS handling.
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

  if (code < 200 || code >= 300) {
    return "";
  }
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

  if (onlineMode && activeSessionId.length()) {
    bool updated = patchSessionRunning();
    Serial.println(updated ? "[SUPABASE] Session marked running"
                           : "[SUPABASE] Running update failed");
  }

  setRelay(true);
  state = onlineMode ? STATE_RUNNING : STATE_OFFLINE_RUNNING;
  drawRunningScreen();
}

void stopTherapy() {
  Serial.println("[THERAPY] Stopped/reset by user");
  setRelay(false);

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
}

void toggleMode() {
  if (state == STATE_RUNNING || state == STATE_OFFLINE_RUNNING) return;

  setRelay(false);
  onlineMode = !onlineMode;
  clearActiveSession();
  lastDrawnSeconds = 999999UL;

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
  // For active-low relay modules, RELAY_OFF = HIGH.
  // Writing before pinMode enables the output latch before the pin becomes OUTPUT.
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  Serial.begin(115200);
  delay(50);
  Serial.println();
  Serial.println("=== Proshash Nebulizer boot ===");
  Serial.print("Relay trigger mode: ");
  Serial.println(RELAY_ACTIVE_LOW ? "ACTIVE LOW" : "ACTIVE HIGH");
  setRelay(false);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_START, INPUT_PULLUP);
  pinMode(BTN_STOP, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);
  syncButtonStates();

  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);

  tft.begin();
  tft.setSPISpeed(10000000);
  tft.setRotation(0);
  tft.setTextWrap(false);
  initColors();

  drawWiFiScreen("Connecting");
  connectWiFi();

  onlineMode = true;
  state = STATE_QR_WAIT;
  drawQrWaitScreen();
}

void loop() {
  // MODE changes between QR/online and local/offline mode.
  if (buttonPressed(modeBtn)) {
    Serial.println("[BUTTON] MODE");
    toggleMode();
    delay(150);
    return;
  }

  if (millis() - lastWifiCheckMs > 10000) {
    lastWifiCheckMs = millis();
    wifiOk = (WiFi.status() == WL_CONNECTED);
  }

  // ------------------------------------------------------------
  // QR WAIT:
  // START now works as a direct manual start. It automatically
  // switches to offline mode and energizes the relay.
  // ------------------------------------------------------------
  if (state == STATE_QR_WAIT) {
    if (buttonPressed(startBtn)) {
      Serial.println("[BUTTON] START in QR screen -> manual/offline start");
      onlineMode = false;
      clearActiveSession();
      state = STATE_OFFLINE_READY;
      startTherapy();
      return;
    }

    if (buttonPressed(nextBtn)) {
      Serial.println("[BUTTON] NEXT in QR screen -> select manual time");
      selectedIndex++;
      if (selectedIndex >= optionCount) selectedIndex = 0;

      onlineMode = false;
      clearActiveSession();
      state = STATE_OFFLINE_READY;
      drawOfflineReadyScreen();
      return;
    }

    if (buttonPressed(stopBtn)) {
      Serial.println("[BUTTON] STOP in QR screen");
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
      }
    }
  }

  // ------------------------------------------------------------
  // READY states: select duration, start, or reset.
  // ------------------------------------------------------------
  else if (state == STATE_READY || state == STATE_OFFLINE_READY) {
    if (buttonPressed(nextBtn)) {
      Serial.println("[BUTTON] NEXT");
      selectedIndex++;
      if (selectedIndex >= optionCount) selectedIndex = 0;

      Serial.print("[TIMER] Selected ");
      Serial.print(timeOptions[selectedIndex]);
      Serial.println(" minute(s)");

      if (onlineMode) drawReadyScreen();
      else drawOfflineReadyScreen();
      return;
    }

    if (buttonPressed(startBtn)) {
      Serial.println("[BUTTON] START");
      startTherapy();
      return;
    }

    if (buttonPressed(stopBtn)) {
      Serial.println("[BUTTON] STOP/RESET");
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
  // RUNNING states: STOP immediately de-energizes the relay.
  // Countdown also de-energizes the relay when it reaches zero.
  // ------------------------------------------------------------
  else if (state == STATE_RUNNING || state == STATE_OFFLINE_RUNNING) {
    if (buttonPressed(stopBtn)) {
      Serial.println("[BUTTON] STOP while running");
      stopTherapy();
      return;
    }

    // Ignore NEXT/START while already running, but report it.
    if (buttonPressed(nextBtn)) {
      Serial.println("[BUTTON] NEXT ignored while running");
    }
    if (buttonPressed(startBtn)) {
      Serial.println("[BUTTON] START ignored; already running");
    }

    // Catch up correctly even if another operation briefly blocks the loop.
    unsigned long nowMs = millis();
    while (remainingSeconds > 0 && nowMs - previousTick >= 1000) {
      previousTick += 1000;
      remainingSeconds--;
    }

    updateTimerPanel(remainingSeconds, true);

    if (remainingSeconds == 0) {
      finishTherapy();
      return;
    }
  }

  // ------------------------------------------------------------
  // DONE states:
  // Offline START immediately begins another run.
  // STOP returns to the relevant ready/QR screen.
  // ------------------------------------------------------------
  else if (state == STATE_DONE || state == STATE_OFFLINE_DONE) {
    if (buttonPressed(startBtn)) {
      Serial.println("[BUTTON] START after completion");

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

    if (buttonPressed(stopBtn)) {
      Serial.println("[BUTTON] STOP after completion");
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
