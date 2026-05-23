#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "MAX30105.h"
#include "heartRate.h"        // checkForBeat()
#include <time.h>
#include <ArduinoJson.h>
#include <math.h>

// =======================
// ===    SETTINGS     ===
// =======================

// — OLED —
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_ADDR      0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// — Wi-Fi & MQTT —
const char* SSID        = "Bruh";
const char* PASS        = "7a4ee42005";
const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const char* TOPIC_DATA  = "esp32/bruh/data";
const char* TOPIC_STATUS= "esp32/bruh/status";
const char* TOPIC_IDS   = "watch/ids";
const char* TOPIC_ACKS  = "watch/acks";

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// — Watch binding —
const char* WATCH_ID = "1234";
bool bound = false;

// — MPU6050 (steps + temp) —
Adafruit_MPU6050 mpu;
float    baselineG      = 1.0f;
uint32_t stepCount      = 0;
bool     aboveStep      = false;
const float STEP_DELTA  = 0.5f;
const uint32_t DEBOUNCE = 300;
uint32_t lastStepTime   = 0;

// — MAX30105 (HR + SpO₂) —
MAX30105 sensor;
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateIndex = 0;
long lastBeatTime = 0;
int  beatAvg      = 0;

// — IR plot buffer —
const int PLOT_W = SCREEN_WIDTH;
const int PLOT_H = 48;
uint32_t irBuf[PLOT_W];
int      bufHead   = 0;
bool     bufFilled = false;

// — Animation & Pages —
const uint8_t NUM_PAGES     = 4;
uint8_t currentPage         = 0;
const uint8_t BUTTON_PIN    = 14;
const uint32_t BTN_DEBOUNCE = 200;
uint32_t lastBtnTime        = 0;
uint8_t  walkFrame          = 0;
uint32_t lastAnimTime       = 0;
const uint16_t ANIM_INTERVAL = 200;

// — SpO₂ animation —
int   fillLevel = 0;
float spo2      = 0.0f;

// =======================
// ===    HELPERS      ===
// =======================

void drawRightAligned(const char* txt, int y, uint8_t sz=1) {
  int16_t x1,y1; uint16_t w,h;
  display.setTextSize(sz);
  display.getTextBounds(txt, 0, 0, &x1,&y1,&w,&h);
  display.setCursor(SCREEN_WIDTH - w, y);
  display.print(txt);
}

void drawClockHand(int cx, int cy, float angleDeg, int len) {
  float rad = (angleDeg - 90.0f) * (PI/180.0f);
  display.drawLine(cx, cy,
                   cx + cos(rad)*len,
                   cy + sin(rad)*len,
                   SSD1306_WHITE);
}

void drawWalker(uint8_t frame) {
  int cx = SCREEN_WIDTH/2, cy = 12;
  display.drawCircle(cx, cy, 8, SSD1306_WHITE);
  display.drawLine(cx, cy+8, cx, cy+20, SSD1306_WHITE);
  // arms
  if (frame % 2 == 0) {
    display.drawLine(cx,cy+12, cx-8,cy+4, SSD1306_WHITE);
    display.drawLine(cx,cy+12, cx+8,cy+4, SSD1306_WHITE);
  } else {
    display.drawLine(cx,cy+12, cx-8,cy+20, SSD1306_WHITE);
    display.drawLine(cx,cy+12, cx+8,cy+20, SSD1306_WHITE);
  }
  // legs
  switch(frame) {
    case 0:
    case 2:
      display.drawLine(cx,cy+20,cx-6,cy+36, SSD1306_WHITE);
      display.drawLine(cx,cy+20,cx+6,cy+36, SSD1306_WHITE);
      break;
    case 1:
      display.drawLine(cx,cy+20,cx-10,cy+34,SSD1306_WHITE);
      display.drawLine(cx,cy+20,cx+4, cy+36,SSD1306_WHITE);
      break;
    case 3:
      display.drawLine(cx,cy+20,cx-4, cy+36,SSD1306_WHITE);
      display.drawLine(cx,cy+20,cx+10,cy+34,SSD1306_WHITE);
      break;
  }
}

// =======================
// ===   CONNECTORS    ===
// =======================

void mqttCallback(char* topic, byte* payload, unsigned len) {
  if (strcmp(topic, TOPIC_ACKS) == 0) {
    String msg;
    for (unsigned i = 0; i < len; i++) msg += (char)payload[i];
    msg.trim();
    if (msg == String("Ok ") + WATCH_ID) {
      bound = true;
    }
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

void connectMQTT() {
  while (!mqttClient.connected()) {
    String cid = "ESP32-" + String(random(0xFFFF), HEX);
    if (mqttClient.connect(cid.c_str())) {
      mqttClient.publish(TOPIC_STATUS, "online");
      mqttClient.subscribe(TOPIC_ACKS);
      mqttClient.publish(TOPIC_IDS, WATCH_ID);
    } else {
      delay(5000);
    }
  }
}

// =======================
// ===     SETUP       ===
// =======================

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // OLED
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.println("Booting...");
  display.display();

  // MPU6050
  mpu.begin();
  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
  mpu.setFilterBandwidth(MPU6050_BAND_5_HZ);
  // calibrate gravity
  float sumG = 0;
  for (int i = 0; i < 100; i++) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    sumG += sqrt(sq(a.acceleration.x) + sq(a.acceleration.y) + sq(a.acceleration.z)) / 9.80665f;
    delay(10);
  }
  baselineG = sumG / 100.0f;

  // MAX30105
  sensor.begin(Wire, I2C_SPEED_FAST);
  sensor.setup();
  sensor.setPulseAmplitudeRed(0x0A);
  sensor.setPulseAmplitudeGreen(0);

  // initialize buffers
  for (byte i = 0; i < RATE_SIZE; i++) rates[i] = 0;
  uint32_t ir0 = sensor.getIR();
  for (int i = 0; i < PLOT_W; i++) irBuf[i] = ir0;
  bufFilled = true;

  // time sync (non-blocking in loop via timeout)
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  // Wi-Fi & MQTT
  connectWiFi();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

// =======================
// ===      LOOP       ===
// =======================

void loop() {
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  uint32_t now = millis();

  // — binding screen —
  if (!bound) {
    mqttClient.publish(TOPIC_IDS, WATCH_ID);
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print("Bind your watch:");
    display.setTextSize(3);
    display.setCursor((SCREEN_WIDTH - 6 * 3 * strlen(WATCH_ID)) / 2, 40);
    display.print(WATCH_ID);
    display.display();
    delay(1000);
    return;
  }

  // — page button —
  if (digitalRead(BUTTON_PIN) == LOW && now - lastBtnTime > BTN_DEBOUNCE) {
    lastBtnTime = now;
    currentPage = (currentPage + 1) % NUM_PAGES;
    if (currentPage == 3) fillLevel = 0;
  }

  // — read sensors —
  uint32_t ir  = sensor.getIR();
  uint32_t red = sensor.getRed();
  irBuf[bufHead] = ir;
  bufHead = (bufHead + 1) % PLOT_W;
  if (!bufFilled && bufHead == 0) bufFilled = true;

  // — heart rate (SparkFun PBA) —
  if (checkForBeat(ir)) {
    long delta = now - lastBeatTime;
    lastBeatTime = now;
    float bpm = 60000.0f / delta;
    if (bpm > 20 && bpm < 255) {
      rates[rateIndex++] = (byte)bpm;
      rateIndex %= RATE_SIZE;
      long sum = 0;
      for (byte i = 0; i < RATE_SIZE; i++) sum += rates[i];
      beatAvg = sum / RATE_SIZE;
    }
  }

  // — steps & temp —
  sensors_event_t aEvt, gEvt, tEvt;
  mpu.getEvent(&aEvt, &gEvt, &tEvt);
  float magG = sqrt(sq(aEvt.acceleration.x) + sq(aEvt.acceleration.y) + sq(aEvt.acceleration.z)) / 9.80665f;
  float dG   = fabs(magG - baselineG);
  if (!aboveStep && dG > STEP_DELTA && now - lastStepTime > DEBOUNCE) {
    stepCount++; lastStepTime = now; aboveStep = true;
  } else if (aboveStep && dG < STEP_DELTA * 0.8f) {
    aboveStep = false;
  }

  // — walker animation —
  if (now - lastAnimTime > ANIM_INTERVAL) {
    lastAnimTime = now;
    walkFrame = (walkFrame + 1) % 4;
  }

  // — SpO₂ crude ratio —
  if (ir + red > 0) {
    spo2 = 100.0f * float(ir) / float(ir + red);
    spo2 = constrain(spo2, 0, 100);
  }

  // — draw pages —
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (currentPage == 0) {
    // Clock + Temp, but only wait up to 1s for time sync each loop
    struct tm ti;
    if (getLocalTime(&ti, 1000)) {
      int cx = 64, cy = 32, r = 20;
      display.drawCircle(cx, cy, r, SSD1306_WHITE);
      float hA = (((ti.tm_hour % 12) + ti.tm_min / 60.0f) / 12.0f) * 360.0f;
      float mA = (ti.tm_min / 60.0f) * 360.0f;
      drawClockHand(cx, cy, hA, r - 7);
      drawClockHand(cx, cy, mA, r - 3);
      char buf[9];
      sprintf(buf, "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print(buf);
      display.setCursor(0, SCREEN_HEIGHT - 8);
      display.printf("Temp: %.1fC", tEvt.temperature);
    } else {
      display.setTextSize(1);
      display.setCursor((SCREEN_WIDTH - 6 * strlen("Syncing time...")) / 2,
                        SCREEN_HEIGHT / 2 - 4);
      display.print("Syncing time...");
    }
  }
  else if (currentPage == 1) {
    // IR plot + BPM
    uint32_t mn = UINT32_MAX, mx = 0;
    int cnt = bufFilled ? PLOT_W : bufHead;
    for (int i = 0; i < cnt; i++) {
      uint32_t v = irBuf[(bufHead - cnt + i + PLOT_W) % PLOT_W];
      mn = min(mn, v); mx = max(mx, v);
    }
    if (mx == mn) mx = mn + 1;
    for (int x = 0; x < PLOT_W - 1; x++) {
      int i1 = (bufHead + x) % PLOT_W, i2 = (bufHead + x + 1) % PLOT_W;
      float n1 = (irBuf[i1] - mn) / float(mx - mn);
      float n2 = (irBuf[i2] - mn) / float(mx - mn);
      int y1 = PLOT_H - int(n1 * PLOT_H), y2 = PLOT_H - int(n2 * PLOT_H);
      display.drawLine(x, y1, x + 1, y2, SSD1306_WHITE);
    }
    display.setTextSize(1);
    display.setCursor(0, PLOT_H + 2);
    display.printf("BPM:%d IR:%lu", beatAvg, ir);
    if (ir < 50000 && beatAvg == 0) display.print(" NoFinger");
  }
  else if (currentPage == 2) {
    // Walker + Steps
    drawWalker(walkFrame);
    char buf[16];
    sprintf(buf, "Steps:%lu", stepCount);
    display.setTextSize(2);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, SCREEN_HEIGHT - h - 2);
    display.print(buf);
  }
  else {
    // SpO₂ Beaker
    int tgt = int(spo2 + 0.5f);
    if      (fillLevel < tgt) fillLevel++;
    else if (fillLevel > tgt) fillLevel--;
    int bx = 44, by = 10, bw = 40, bh = SCREEN_HEIGHT - 20;
    display.drawRect(bx, by, bw, bh, SSD1306_WHITE);
    int fh = (bh * fillLevel) / 100;
    display.fillRect(bx + 1, by + (bh - fh), bw - 2, fh, SSD1306_WHITE);
    char buf[8];
    sprintf(buf, "%d%%", fillLevel);
    display.setTextSize(2);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(SCREEN_WIDTH - w - 2, SCREEN_HEIGHT - h - 2);
    display.print(buf);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("SpO2 (est)");
  }

  display.display();

  // — publish JSON every 5s —
  static uint32_t lastPub = 0;
  if (now - lastPub > 5000) {
    lastPub = now;
    StaticJsonDocument<192> doc;
    doc["id"]     = WATCH_ID;
    doc["time"]   = (unsigned long)time(NULL);
    doc["steps"]  = stepCount;
    doc["hr"]     = beatAvg;
    doc["temp"]   = tEvt.temperature;
    doc["oxygen"] = spo2;
    char out[192];
    serializeJson(doc, out);
    mqttClient.publish(TOPIC_DATA, out);
  }
}
