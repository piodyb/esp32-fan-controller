#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>

// --- PINY UKŁADU ---
#define MOSFET_PIN      4       
#define FAN_PWM_PIN     10      
#define TEMP_SENSOR_PIN 3       
#define ARGB_PIN        7       
#define NUM_LEDS        12      

U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ 6, /* data=*/ 5);

OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature sensors(&oneWire);

Adafruit_NeoPixel strip(NUM_LEDS, ARGB_PIN, NEO_GRB + NEO_KHZ800);

WebServer server(80);
WiFiManager wm;
Preferences preferences;

float tempMin = 30.0;    
float tempOff = 28.0;    
float tempMax = 60.0;    

const int PWM_FREQ = 25000;     
const int PWM_RESOLUTION = 8;   

float currentTemp = 0.0;
int currentPwmPercent = 0;
bool isMosfetOn = false;

unsigned long pauseEndTime = 0;
bool isPaused = false;

// Tryby ARGB: 0 = Auto (temperatura), 1 = Stały kolor, 2 = Tęcza
int rgbMode = 0;
uint8_t customR = 0;
uint8_t customG = 255;
uint8_t customB = 255;
int rgbBrightness = 150;

// Konfiguracja animacji OLED
bool enableAnimations = true;

// Obsługa wygaszania OLED (1 minuta bezczynności, włączenie na 10 min)
unsigned long oledWakeUntil = 0; 
bool isOledAwake = true;

// --- HISTORIA WYKRESU ---
const int HISTORY_SIZE = 60;
float tempHistory[HISTORY_SIZE];
int historyCount = 0;
unsigned long lastHistorySample = 0;

void processFanControl(float temp);
void updateRgbEffect();
void handleRoot();
void handleUpdate();
void handlePause();
void handleRgbUpdate();
void handleAnimUpdate();
void handleOledWake();
void drawFanIcon(int x, int y, int frame, bool isRunning);
void drawWifiIcon(int x, int y, int frame);
String generateSvgChart();

void setup() {
  Serial.begin(115200);
  delay(500);

  // Wypełnienie historii na start
  for (int i = 0; i < HISTORY_SIZE; i++) {
    tempHistory[i] = -999.0;
  }

  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, LOW);

  strip.begin();
  strip.setBrightness(rgbBrightness);
  strip.show();

  ledcAttach(FAN_PWM_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcWrite(FAN_PWM_PIN, 0);

  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();

  u8g2.begin();
  u8g2.setContrast(100);

  oledWakeUntil = millis() + 60000;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_profont10_tr);
  u8g2.drawStr(0, 9, "STARTING...");
  u8g2.sendBuffer();

  preferences.begin("fan-cfg", false);
  tempMin = preferences.getFloat("t_min", 30.0);
  tempOff = preferences.getFloat("t_off", 28.0);
  tempMax = preferences.getFloat("t_max", 60.0);
  rgbMode = preferences.getInt("rgb_mode", 0);
  customR = preferences.getUChar("rgb_r", 0);
  customG = preferences.getUChar("rgb_g", 255);
  customB = preferences.getUChar("rgb_b", 255);
  rgbBrightness = preferences.getInt("rgb_bright", 150);
  enableAnimations = preferences.getBool("anim_en", true);
  strip.setBrightness(rgbBrightness);
  preferences.end();

  wm.setTimeout(180); 

  if (!wm.autoConnect("ESP32-Fan-Config", "12345678")) {
    Serial.println("Błąd połączenia lub timeout. Restart...");
    delay(3000);
    ESP.restart();
  }

  if (enableAnimations) {
    for (int f = 0; f <= 3; f++) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_profont10_tr);
      u8g2.drawStr(0, 12, "CONNECTED!");
      drawWifiIcon(36, 28, f);
      u8g2.sendBuffer();
      delay(120);
    }
  }
  delay(300);

  Serial.println("\nPołączono z WiFi!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/update", HTTP_POST, handleUpdate);
  server.on("/pause", HTTP_POST, handlePause);
  server.on("/rgb", HTTP_POST, handleRgbUpdate);
  server.on("/animation", HTTP_POST, handleAnimUpdate);
  server.on("/oled_wake", HTTP_POST, handleOledWake);

  server.begin();
}

void loop() {
  server.handleClient();

  static unsigned long lastDisplayUpdate = 0;
  static unsigned long lastTempRequest = 0;
  static uint8_t animFrame = 0;
  unsigned long currentMillis = millis();

  if (isPaused && currentMillis >= pauseEndTime) {
    isPaused = false;
  }

  // Pomiary temperatury co 1s
  if (currentMillis - lastTempRequest >= 1000) {
    currentTemp = sensors.getTempCByIndex(0);
    sensors.requestTemperatures();
    lastTempRequest = currentMillis;
    processFanControl(currentTemp);
    updateRgbEffect();
  }

  // Rejestracja historii wykresu co 60 sekund
  if (currentMillis - lastHistorySample >= 60000 || historyCount == 0) {
    lastHistorySample = currentMillis;
    if (currentTemp > -50.0 && currentTemp < 125.0) { // Omijamy odczyty -127.0 (błąd czujnika)
      if (historyCount < HISTORY_SIZE) {
        tempHistory[historyCount++] = currentTemp;
      } else {
        for (int i = 0; i < HISTORY_SIZE - 1; i++) {
          tempHistory[i] = tempHistory[i + 1];
        }
        tempHistory[HISTORY_SIZE - 1] = currentTemp;
      }
    }
  }

  if (currentMillis < oledWakeUntil) {
    if (!isOledAwake) {
      u8g2.setPowerSave(0); 
      isOledAwake = true;
    }
  } else {
    if (isOledAwake) {
      u8g2.setPowerSave(1); 
      isOledAwake = false;
    }
  }

  if (isOledAwake && (currentMillis - lastDisplayUpdate >= 250)) {
    lastDisplayUpdate = currentMillis;

    bool running = (isMosfetOn && !isPaused);
    if (enableAnimations && running) {
      animFrame = (animFrame + 1) % 4; 
    } else {
      animFrame = 0; 
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_profont10_tr);

    drawFanIcon(14, 14, animFrame, running && enableAnimations);

    if (isPaused) {
      char pBuf[15];
      snprintf(pBuf, sizeof(pBuf), "PAUSE %lds", (pauseEndTime - currentMillis) / 1000);
      u8g2.drawStr(28, 12, pBuf);
    } else if (running) {
      char pBuf[15];
      snprintf(pBuf, sizeof(pBuf), "RUN %d%%", currentPwmPercent);
      u8g2.drawStr(28, 12, pBuf);
    } else {
      u8g2.drawStr(28, 12, "STANDBY");
    }

    char tempBuf[20];
    if (currentTemp <= 0 || currentTemp == -127.0) {
      snprintf(tempBuf, sizeof(tempBuf), "TMP: ERR");
    } else {
      snprintf(tempBuf, sizeof(tempBuf), "Temp: %.1f C", currentTemp);
    }
    u8g2.drawStr(0, 36, tempBuf);

    u8g2.sendBuffer();
  }
}

void drawFanIcon(int x, int y, int frame, bool isRunning) {
  u8g2.drawCircle(x, y, 9); 
  
  float angle = isRunning ? (frame * (PI / 4)) : 0.785; 
  for (int i = 0; i < 4; i++) {
    float a = angle + i * (PI / 2);
    int x2 = x + (int)(cos(a) * 8);
    int y2 = y + (int)(sin(a) * 8);
    u8g2.drawLine(x, y, x2, y2);
  }
  u8g2.drawDisc(x, y, 2); 
}

void drawWifiIcon(int x, int y, int frame) {
  u8g2.drawDisc(x, y, 1); 
  if (frame >= 1) {
    u8g2.drawPixel(x - 3, y - 3);
    u8g2.drawPixel(x + 3, y - 3);
  }
  if (frame >= 2) {
    u8g2.drawHLine(x - 5, y - 6, 11);
  }
  if (frame >= 3) {
    u8g2.drawHLine(x - 8, y - 9, 17);
  }
}

void processFanControl(float temp) {
  if (isPaused) {
    digitalWrite(MOSFET_PIN, LOW);
    ledcWrite(FAN_PWM_PIN, 0);
    isMosfetOn = false;
    currentPwmPercent = 0;
    return;
  }

  if (temp <= 0 || temp == -127.0) {
    digitalWrite(MOSFET_PIN, HIGH);
    ledcWrite(FAN_PWM_PIN, 255);
    isMosfetOn = true;
    currentPwmPercent = 100;
    return;
  }

  if (isMosfetOn) {
    if (temp < tempOff) {
      digitalWrite(MOSFET_PIN, LOW);
      ledcWrite(FAN_PWM_PIN, 0);
      isMosfetOn = false;
      currentPwmPercent = 0;
    } else {
      if (temp >= tempMax) {
        ledcWrite(FAN_PWM_PIN, 255);
        currentPwmPercent = 100;
      } else {
        int pwmValue = map((int)(temp * 10), (int)(tempMin * 10), (int)(tempMax * 10), 50, 255);
        ledcWrite(FAN_PWM_PIN, pwmValue);
        currentPwmPercent = map(pwmValue, 0, 255, 0, 100);
      }
    }
  } else {
    if (temp >= tempMin) {
      digitalWrite(MOSFET_PIN, HIGH);
      isMosfetOn = true;
      
      ledcWrite(FAN_PWM_PIN, 255);
      delay(150); 

      int pwmValue = map((int)(temp * 10), (int)(tempMin * 10), (int)(tempMax * 10), 50, 255);
      ledcWrite(FAN_PWM_PIN, pwmValue);
      currentPwmPercent = map(pwmValue, 0, 255, 0, 100);
    }
  }
}

void updateRgbEffect() {
  uint32_t color = strip.Color(0, 0, 0);

  if (rgbMode == 1) {
    color = strip.Color(customR, customG, customB);
    for(int i=0; i<strip.numPixels(); i++) {
      strip.setPixelColor(i, color);
    }
    strip.show();
    return;
  }

  if (rgbMode == 2) {
    static uint16_t j = 0;
    for(int i=0; i<strip.numPixels(); i++) {
      strip.setPixelColor(i, strip.ColorHSV((i * 65536L / strip.numPixels() + j) & 65535, 255, 255));
    }
    strip.show();
    j += 256;
    return;
  }

  if (isPaused) {
    color = strip.Color(128, 0, 128);
  } else if (currentTemp <= 0 || currentTemp == -127.0) {
    color = strip.Color(255, 0, 0);
  } else if (!isMosfetOn) {
    color = strip.Color(0, 100, 255);
  } else {
    float ratio = (currentTemp - tempMin) / (tempMax - tempMin);
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    
    int red = (int)(255 * ratio);
    int green = (int)(255 * (1.0 - ratio));
    color = strip.Color(red, green, 0);
  }

  for(int i=0; i<strip.numPixels(); i++) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

// Funkcja generująca lekki wykres SVG
String generateSvgChart() {
  if (historyCount < 2) {
    return "<div style='text-align:center;padding:30px;color:#777;font-size:14px;'>Zbieranie danych (potrzeba min. 2 minut)...</div>";
  }

  float minT = 100.0;
  float maxT = -50.0;
  for (int i = 0; i < historyCount; i++) {
    if (tempHistory[i] < minT) minT = tempHistory[i];
    if (tempHistory[i] > maxT) maxT = tempHistory[i];
  }

  minT = floor(minT - 1.0);
  maxT = ceil(maxT + 1.0);
  if (maxT - minT < 4.0) maxT = minT + 4.0; 

  int w = 500;
  int h = 160;
  int padL = 40;
  int padR = 15;
  int padT = 15;
  int padB = 25;

  int plotW = w - padL - padR;
  int plotH = h - padT - padB;

  String svg = "<svg viewBox='0 0 " + String(w) + " " + String(h) + "' style='width:100%; height:auto; display:block;'>";
  
  for (int step = 0; step <= 4; step++) {
    int y = padT + (plotH * step / 4);
    float val = maxT - ((maxT - minT) * step / 4.0);
    svg += "<line x1='" + String(padL) + "' y1='" + String(y) + "' x2='" + String(w - padR) + "' y2='" + String(y) + "' stroke='#2a3449' stroke-dasharray='3'/>";
    svg += "<text x='" + String(padL - 6) + "' y='" + String(y + 4) + "' fill='#8fa0bc' font-size='11' text-anchor='end'>" + String(val, 0) + "°</text>";
  }

  String polyline = "";
  for (int i = 0; i < historyCount; i++) {
    float x = padL + ((float)i / (HISTORY_SIZE - 1)) * plotW;
    float norm = (tempHistory[i] - minT) / (maxT - minT);
    float y = (padT + plotH) - (norm * plotH);
    polyline += String(x, 1) + "," + String(y, 1) + " ";
  }

  svg += "<polyline fill='none' stroke='#0ea5e9' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round' points='" + polyline + "'/>";

  svg += "<text x='" + String(padL) + "' y='" + String(h - 4) + "' fill='#64748b' font-size='11'>-60 min</text>";
  svg += "<text x='" + String(padL + plotW / 2) + "' y='" + String(h - 4) + "' fill='#64748b' font-size='11' text-anchor='middle'>-30 min</text>";
  svg += "<text x='" + String(w - padR) + "' y='" + String(h - 4) + "' fill='#64748b' font-size='11' text-anchor='end'>teraz</text>";
  svg += "</svg>";
  
  return svg;
}

// Nowy, kafelkowy Dashboard
void handleRoot() {
  String html = "<!DOCTYPE html><html lang='pl'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  
  // Automatyczne odświeżanie podczas pauzy, aby widzieć licznik
  if (isPaused) {
    html += "<meta http-equiv='refresh' content='2'>";
  }

  html += "<title>Sterownik Wentylatora</title>";
  html += "<style>";
  html += "* { box-sizing: border-box; margin: 0; padding: 0; }";
  html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0f1117; color: #e1e7ef; padding: 20px; }";
  html += ".container { max-width: 1100px; margin: 0 auto; }";
  html += "header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 25px; padding-bottom: 15px; border-bottom: 1px solid #1e2638; }";
  html += "header h1 { font-size: 22px; font-weight: 600; color: #fff; }";
  html += ".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 20px; }";
  html += ".card { background: #161c2d; border: 1px solid #20293d; border-radius: 12px; padding: 20px; box-shadow: 0 4px 20px rgba(0,0,0,0.25); }";
  html += ".card.full { grid-column: 1 / -1; }";
  html += ".c-head { display: flex; justify-content: space-between; align-items: center; margin-bottom: 15px; }";
  html += ".c-title { font-size: 14px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; color: #8fa0bc; }";
  html += ".metric { font-size: 34px; font-weight: 700; color: #fff; margin: 5px 0 10px 0; }";
  html += ".badge { padding: 4px 10px; border-radius: 20px; font-size: 12px; font-weight: 600; }";
  html += ".b-on { background: rgba(34,197,94,0.15); color: #22c55e; border: 1px solid rgba(34,197,94,0.3); }";
  html += ".b-off { background: rgba(239,68,68,0.15); color: #ef4444; border: 1px solid rgba(239,68,68,0.3); }";
  html += ".b-pause { background: rgba(245,158,11,0.15); color: #f59e0b; border: 1px solid rgba(245,158,11,0.3); }";
  html += "label { display: block; font-size: 13px; color: #94a3b8; margin: 12px 0 6px; }";
  html += "input, select { width: 100%; padding: 10px; background: #0d121f; border: 1px solid #2a3449; color: #fff; border-radius: 6px; font-size: 14px; }";
  html += "input[type='checkbox'] { width: auto; margin-right: 8px; vertical-align: middle; }";
  html += "button { width: 100%; padding: 12px; border: none; border-radius: 6px; font-weight: 600; cursor: pointer; margin-top: 16px; font-size: 14px; transition: 0.2s; }";
  html += ".btn-blue { background: #0284c7; color: white; } .btn-blue:hover { background: #0369a1; }";
  html += ".btn-orange { background: #ea580c; color: white; } .btn-orange:hover { background: #c2410c; }";
  html += ".btn-purple { background: #7c3aed; color: white; } .btn-purple:hover { background: #6d28d9; }";
  html += ".btn-gray { background: #334155; color: white; } .btn-gray:hover { background: #475569; }";
  html += "@media (max-width: 600px) { .grid { grid-template-columns: 1fr; } }";
  html += "</style></head><body>";

  html += "<div class='container'>";
  html += "<header><h1>Sterownik Wentylatora</h1>";
  html += "<a href='/' style='color:#0284c7;text-decoration:none;font-weight:600;font-size:14px;'>Odśwież</a></header>";

  html += "<div class='grid'>";

  // KARTA: Wykres
  html += "<div class='card full'>";
  html += "<div class='c-head'><span class='c-title'>Historia Temperatury (60 min)</span>";
  html += "<span style='font-size:12px;color:#64748b;'>Próbki co 1 min</span></div>";
  html += generateSvgChart();
  html += "</div>";

  // KARTA: Status Główny
  html += "<div class='card'>";
  html += "<div class='c-head'><span class='c-title'>Stan Bieżący</span>";
  if (isPaused) {
    html += "<span class='badge b-pause'>PAUZA</span>";
  } else if (isMosfetOn) {
    html += "<span class='badge b-on'>PRACA</span>";
  } else {
    html += "<span class='badge b-off'>CZUWANIE</span>";
  }
  html += "</div>";
  html += "<div class='metric'>" + (currentTemp > -50 ? String(currentTemp, 1) + " °C" : "Błąd czujnika") + "</div>";
  html += "<p style='color:#94a3b8;font-size:14px;'>Obroty PWM: <b>" + String(currentPwmPercent) + "%</b></p>";
  
  if (isPaused) {
    long sec = (pauseEndTime - millis()) / 1000;
    if (sec < 0) sec = 0;
    html += "<p style='color:#f59e0b;font-size:14px;margin-top:8px;'>Wznowienie za: <b>" + String(sec) + "s</b></p>";
  } else {
    html += "<form action='/pause' method='POST' style='margin-top:15px;'>";
    html += "<label>Wyłączenie czasowe (sekundy):</label>";
    html += "<div style='display:flex;gap:10px;'>";
    html += "<input type='number' name='duration' value='60' style='margin-top:16px;'>";
    html += "<button type='submit' class='btn-orange'>Zatrzymaj</button></div></form>";
  }
  html += "</div>";

  // KARTA: Krzywa
  html += "<div class='card'>";
  html += "<div class='c-head'><span class='c-title'>Krzywa Wentylatora</span></div>";
  html += "<form action='/update' method='POST'>";
  html += "<label>Start MOSFET (°C):</label><input type='number' step='0.5' name='t_min' value='" + String(tempMin) + "'>";
  html += "<label>Stop MOSFET (°C):</label><input type='number' step='0.5' name='t_off' value='" + String(tempOff) + "'>";
  html += "<label>Max 100% PWM (°C):</label><input type='number' step='0.5' name='t_max' value='" + String(tempMax) + "'>";
  html += "<button type='submit' class='btn-blue'>Zapisz progi</button></form>";
  html += "</div>";

  // KARTA: ARGB
  html += "<div class='card'>";
  html += "<div class='c-head'><span class='c-title'>Podświetlenie ARGB</span></div>";
  html += "<form action='/rgb' method='POST'>";
  html += "<label>Tryb świecenia:</label><select name='rgb_mode'>";
  html += "<option value='0' " + String(rgbMode == 0 ? "selected" : "") + ">Auto (wg temperatury)</option>";
  html += "<option value='1' " + String(rgbMode == 1 ? "selected" : "") + ">Stały kolor (Hex)</option>";
  html += "<option value='2' " + String(rgbMode == 2 ? "selected" : "") + ">Tęcza</option></select>";
  
  char hexColor[8];
  sprintf(hexColor, "#%02X%02X%02X", customR, customG, customB);
  html += "<label>Wybór barwy stałej:</label><input type='color' name='rgb_color' value='" + String(hexColor) + "'>";
  html += "<label>Jasność (0-255):</label><input type='number' min='0' max='255' name='rgb_bright' value='" + String(rgbBrightness) + "'>";
  html += "<button type='submit' class='btn-purple'>Zastosuj ARGB</button></form>";
  html += "</div>";

  // KARTA: OLED
  html += "<div class='card'>";
  html += "<div class='c-head'><span class='c-title'>Ekran OLED</span>";
  html += "<span class='badge " + String(isOledAwake ? "b-on" : "b-off") + "'>" + String(isOledAwake ? "WŁĄCZONY" : "UŚPIONY") + "</span></div>";
  html += "<form action='/oled_wake' method='POST'>";
  html += "<button type='submit' class='btn-gray'>Wybudź ekran (10 min)</button></form>";
  html += "<form action='/animation' method='POST' style='margin-top:20px; border-top:1px solid #1e2638; padding-top:15px;'>";
  html += "<label style='cursor:pointer;'><input type='checkbox' name='anim_en' value='1' " + String(enableAnimations ? "checked" : "") + "> Pokaż animację obrotów</label>";
  html += "<button type='submit' class='btn-blue'>Zapisz ustawienie</button></form>";
  html += "</div>";

  html += "</div></div></body></html>";
  server.send(200, "text/html", html);
}

void handleUpdate() {
  if (server.hasArg("t_min")) tempMin = server.arg("t_min").toFloat();
  if (server.hasArg("t_off")) tempOff = server.arg("t_off").toFloat();
  if (server.hasArg("t_max")) tempMax = server.arg("t_max").toFloat();

  preferences.begin("fan-cfg", false);
  preferences.putFloat("t_min", tempMin);
  preferences.putFloat("t_off", tempOff);
  preferences.putFloat("t_max", tempMax);
  preferences.end();

  server.sendHeader("Location", "/", true);
  server.send(303);
}

void handlePause() {
  if (server.hasArg("duration")) {
    long durationSec = server.arg("duration").toInt();
    if (durationSec > 0) {
      isPaused = true;
      pauseEndTime = millis() + (durationSec * 1000);
    }
  }

  server.sendHeader("Location", "/", true);
  server.send(303);
}

void handleOledWake() {
  oledWakeUntil = millis() + 600000; 
  server.sendHeader("Location", "/", true);
  server.send(303);
}

void handleAnimUpdate() {
  enableAnimations = server.hasArg("anim_en");

  preferences.begin("fan-cfg", false);
  preferences.putBool("anim_en", enableAnimations);
  preferences.end();

  server.sendHeader("Location", "/", true);
  server.send(303);
}

void handleRgbUpdate() {
  preferences.begin("fan-cfg", false);
  if (server.hasArg("rgb_mode")) {
    rgbMode = server.arg("rgb_mode").toInt();
    preferences.putInt("rgb_mode", rgbMode);
  }
  if (server.hasArg("rgb_bright")) {
    rgbBrightness = server.arg("rgb_bright").toInt();
    if (rgbBrightness < 0) rgbBrightness = 0;
    if (rgbBrightness > 255) rgbBrightness = 255;
    preferences.putInt("rgb_bright", rgbBrightness);
    strip.setBrightness(rgbBrightness);
  }
  if (server.hasArg("rgb_color")) {
    String hex = server.arg("rgb_color"); 
    if (hex.length() == 7) {
      long number = strtol(&hex[1], NULL, 16);
      customR = number >> 16;
      customG = (number >> 8) & 0xFF;
      customB = number & 0xFF;
      
      preferences.putUChar("rgb_r", customR);
      preferences.putUChar("rgb_g", customG);
      preferences.putUChar("rgb_b", customB);
    }
  }
  preferences.end();

  server.sendHeader("Location", "/", true);
  server.send(303);
}