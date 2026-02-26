/***********************************************************************
 * CodeBlueHalo.AI v2.0 — ESP32 Emergency Sentinel
 
 * PINOUT:
 *  PIR         → GPIO 15
 *  DHT11 DATA  → GPIO 18
 *  OLED SDA    → GPIO 21
 *  OLED SCL    → GPIO 22
 *  Ultrasonic  → TRIG=4, ECHO=5 (optional)
 *  BOOT button → GPIO 0
 *  LED/Buzzer  → GPIO 2
 *
 * ThingSpeak Channel: YOUR CHANNEL ID 
 *  Field 1 = PIR %
 *  Field 2 = Temperature C
 *  Field 3 = Humidity %
 *  Field 4 = Distance cm
 *  Field 5 = Anomaly score
 *  Field 6 = Threat level
 *  Field 7 = Motion energy
 *  Field 8 = Emergency class
 ***********************************************************************/

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <EEPROM.h>
#include <ThingSpeak.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <math.h>
#include <time.h>


// ======================== PINS ========================================
#define PIR_PIN 13
#define DHT_PIN 18
#define ULTRASONIC_TRIG 4
#define ULTRASONIC_ECHO 5
#define BUZZER_PIN 2
#define BUTTON_PIN 0

// ======================== HARDWARE ====================================
#define DHT_TYPE DHT11
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SPEED_OF_SOUND 0.0343f

// ======================== NETWORK =====================================
const char *WIFI_SSID = "YOUR_WIFI";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASS";
const unsigned long TS_CHANNEL = CHANNEL ID;
const char *TS_WRITE_KEY = "YOUR_WRITE_API_KEY";
const char *NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET = 19800;

// ======================== TIMING ======================================
#define SENSOR_SAMPLE_MS 100
#define DHT_READ_MS 3000
#define FEATURE_SLIDE_MS 500
#define THINGSPEAK_MS 20000
#define OLED_REFRESH_MS 300
#define WIFI_RETRY_MS 10000
#define HEARTBEAT_MS 5000
#define BASELINE_LEARN_MS 60000
#define STILLNESS_TIMEOUT_MS 45000
#define ESCALATE_L1_MS 15000
#define ESCALATE_L2_MS 30000
#define DEBOUNCE_MS 200

// ======================== ANOMALY =====================================
#define WINDOW_SIZE 30
#define EMA_ALPHA 0.05f
#define PIR_STILL_THRESH 0.02f
#define FALL_DROP_CM 50.0f
#define FLOOR_MAX_CM 40.0f
#define ZSCORE_FALL 3.5f
#define ZSCORE_ANOMALY 2.5f
#define W_MOT 0.35f
#define W_DST 0.30f
#define W_TMP 0.15f
#define W_HUM 0.10f
#define W_PAT 0.10f

// ======================== STRUCTS =====================================
struct SensorReading {
  float distance_cm;
  bool pir;
  float temp;
  float humid;
  uint32_t ts;
  bool dist_valid;
};

struct Features {
  float d_mean, d_std, d_min, d_max, d_delta;
  float pir_duty;
  int pir_trans, pir_high;
  float temp_mean, temp_delta;
  float humid_mean, humid_delta;
  float motion_energy;
  float env_anomaly;
};

struct Baseline {
  float d_mean_ema, d_std_ema;
  float pir_duty_ema;
  float temp_ema, humid_ema;
  float motion_ema;
  uint32_t samples;
  bool ready;
};

struct Scores {
  float mot_z, dst_z, tmp_z, hum_z, pat_z;
  float fused;
  float contrib[5];
};

enum SysState { SYS_BOOT, SYS_CALIB, SYS_OK, SYS_OFFLINE };
enum Threat { T_NORMAL, T_WATCH, T_ALERT, T_EMERGENCY };
enum EmClass {
  EM_NONE = 0,
  EM_FALL = 1,
  EM_POSTFALL = 2,
  EM_STILL = 3,
  EM_THERMAL = 4,
  EM_OCCUPANCY = 5,
  EM_SENSOR = 6
};
enum Posture { P_UNK, P_EMPTY, P_STAND, P_SEAT, P_BED, P_FLOOR };

struct HSM {
  SysState sys;
  Threat threat;
  EmClass eclass;
  Posture posture;
  uint32_t state_at, last_motion, alert_at, emerg_at;
  bool acked;
  int esc_level;
  float peak_score;
  String reason;
};

struct Event {
  uint32_t ts;
  EmClass ec;
  Threat tl;
  float score;
  String desc;
};

// ======================== GLOBALS =====================================
Adafruit_SSD1306 *oled = NULL;
DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient wifiClient;

#define RAW_BUF 64
#define FEAT_BUF 20
#define EVT_BUF 50

SensorReading rawBuf[RAW_BUF];
volatile int rawHead = 0, rawCnt = 0;

Features featHist[FEAT_BUF];
int featHead = 0, featCnt = 0;

Event evtLog[EVT_BUF];
int evtHead = 0, evtCnt = 0;

Baseline bl;
Scores sc;
HSM hsm;
Features curF, prevF;

// DHT cached — starts at 0, only real data after first successful read
float dhtTemp = 0.0f;
float dhtHumid = 0.0f;
uint32_t lastDHTRead = 0;

uint32_t lastTSUp = 0, lastOLED = 0, lastWiFi = 0;
uint32_t lastHB = 0, bootTime = 0, lastBtn = 0;
uint32_t lastPageSwap = 0;

bool wifiOK = false, oledOK = false, usonicOK = false, tsOK = false;

uint32_t totalSamples = 0, totalUploads = 0;
uint32_t totalAlerts = 0, uptimeSec = 0;

int dispPage = 0;
bool buzzerOn = false, buzzerSt = false;
uint32_t lastBuzz = 0;

TaskHandle_t sensorTaskH = NULL;
SemaphoreHandle_t mtx;

// ======================== FORWARD DECL ================================
float readUltrasonic();
Features extractFeatures();
float calcStd(float *d, int n, float m);
float clampf(float v, float lo, float hi);
void initBaseline();
void updateBaseline(Features &f);
Scores computeScores(Features &f);
Posture estPosture(Features &f);
void initHSM();
void runHSM(Features &f, Scores &s);
EmClass classify(Features &f, Scores &s);
void changeThreat(Threat t, EmClass ec, String reason);
void logEvt(EmClass ec, Threat tl, float score, String desc);
String timeStr();
String threatStr(Threat t);
String classStr(EmClass e);
String postureStr(Posture p);

// ======================== OLED INIT ===================================
bool tryOLED(int sda, int scl, uint8_t addr) {
  Serial.print(F("  Try SDA="));
  Serial.print(sda);
  Serial.print(F(" SCL="));
  Serial.print(scl);
  Serial.print(F(" 0x"));
  Serial.print(addr, HEX);
  Serial.print(F("... "));

  Wire.end();
  delay(50);
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  delay(100);

  Wire.beginTransmission(addr);
  if (Wire.endTransmission() != 0) {
    Serial.println(F("no device"));
    return false;
  }
  Serial.println(F("found!"));

  if (oled != NULL)
    delete oled;
  oled = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

  if (oled->begin(SSD1306_SWITCHCAPVCC, addr)) {
    Serial.println(F("  >>> OLED SUCCESS <<<"));
    return true;
  }
  Serial.println(F("  begin() failed"));
  delete oled;
  oled = NULL;
  return false;
}

void initOLED() {
  Serial.println(F("\n[OLED] Scanning..."));
  if (tryOLED(21, 22, 0x3C)) {
    oledOK = true;
    return;
  }
  if (tryOLED(21, 22, 0x3D)) {
    oledOK = true;
    return;
  }
  if (tryOLED(22, 21, 0x3C)) {
    oledOK = true;
    return;
  }
  if (tryOLED(22, 21, 0x3D)) {
    oledOK = true;
    return;
  }
  if (tryOLED(21, 23, 0x3C)) {
    oledOK = true;
    return;
  }
  if (tryOLED(22, 23, 0x3C)) {
    oledOK = true;
    return;
  }
  if (tryOLED(23, 22, 0x3C)) {
    oledOK = true;
    return;
  }
  Serial.println(F("[OLED] ALL FAILED — check wiring"));
  oledOK = false;
}

// ======================== SETUP =======================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n================================"));
  Serial.println(F("  CodeBlueHalo.AI v2.0 FINAL"));
  Serial.println(F("  ALL BUGS FIXED"));
  Serial.println(F("================================\n"));

  bootTime = millis();
  mtx = xSemaphoreCreateMutex();

  // --- Pins ---
  // PIR: INPUT_PULLDOWN fixes GPIO 15 stuck-HIGH bug
  pinMode(PIR_PIN, INPUT_PULLDOWN);
  pinMode(ULTRASONIC_TRIG, OUTPUT);
  pinMode(ULTRASONIC_ECHO, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);

  // --- DHT11 warmup ---
  dht.begin();
  Serial.println(F("[DHT] Warming up 3s..."));
  delay(3000);
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  Serial.print(F("[DHT] First read: T="));
  Serial.print(t);
  Serial.print(F("  H="));
  Serial.println(h);
  if (!isnan(t) && t > -40 && t < 80)
    dhtTemp = t;
  if (!isnan(h) && h > 0 && h <= 100)
    dhtHumid = h;
  lastDHTRead = millis();

  // --- OLED ---
  initOLED();
  if (oledOK && oled != NULL) {
    oled->clearDisplay();
    oled->setTextSize(2);
    oled->setTextColor(SSD1306_WHITE);
    oled->setCursor(5, 0);
    oled->println(F("CodeBlue"));
    oled->println(F("Halo.AI"));
    oled->setTextSize(1);
    oled->setCursor(5, 45);
    oled->println(F("v2.0 ALL FIXED"));
    oled->display();
    delay(2000);
  }

  // --- Ultrasonic ---
  float td = readUltrasonic();
  usonicOK = (td > 0 && td < 400);
  Serial.println(usonicOK ? F("[US] OK") : F("[US] Not found"));

  // --- WiFi ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("[WiFi] Connecting"));
  int att = 0;
  while (WiFi.status() != WL_CONNECTED && att < 30) {
    delay(500);
    Serial.print(F("."));
    att++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    wifiOK = true;
    Serial.print(F("[WiFi] IP: "));
    Serial.println(WiFi.localIP());
    configTime(GMT_OFFSET, 0, NTP_SERVER);
    ThingSpeak.begin(wifiClient);
  } else {
    wifiOK = false;
    Serial.println(F("[WiFi] Failed, offline. Starting AP Mode"));
    WiFi.mode(WIFI_AP);
    WiFi.softAP("CodeBlueHalo_AP", "12345678");
  }

  // --- State ---
  initHSM();
  initBaseline();

  // --- Sensor task on Core 0 ---
  xTaskCreatePinnedToCore(
      [](void *p) {
        uint32_t lastSamp = 0, lastFeat = 0;
        for (;;) {
          uint32_t now = millis();

          // DHT11 read every 3s
          if (now - lastDHTRead >= DHT_READ_MS) {
            float t = dht.readTemperature();
            float h = dht.readHumidity();
            Serial.print(F("[DHT] T="));
            Serial.print(isnan(t) ? -999.0f : t, 1);
            Serial.print(F(" H="));
            Serial.println(isnan(h) ? -999.0f : h, 1);
            if (!isnan(t) && t > -40 && t < 80)
              dhtTemp = t;
            if (!isnan(h) && h > 0 && h <= 100)
              dhtHumid = h;
            lastDHTRead = now;
          }

          // PIR + ultrasonic at 10 Hz
          if (now - lastSamp >= SENSOR_SAMPLE_MS) {
            SensorReading r;
            r.ts = now;
            r.pir = (digitalRead(PIR_PIN) == HIGH);
            r.temp = dhtTemp;
            r.humid = dhtHumid;
            if (usonicOK) {
              r.distance_cm = readUltrasonic();
              r.dist_valid = (r.distance_cm > 2 && r.distance_cm < 400);
            } else {
              r.distance_cm = 0;
              r.dist_valid = false;
            }
            if (xSemaphoreTake(mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
              rawBuf[rawHead] = r;
              rawHead = (rawHead + 1) % RAW_BUF;
              if (rawCnt < RAW_BUF)
                rawCnt++;
              totalSamples++;
              xSemaphoreGive(mtx);
            }
            lastSamp = now;
          }

          // Feature extraction every 500ms
          if (now - lastFeat >= FEATURE_SLIDE_MS) {
            if (rawCnt >= 5) {
              if (xSemaphoreTake(mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
                prevF = curF;
                curF = extractFeatures();
                if (hsm.sys == SYS_CALIB || hsm.threat == T_NORMAL)
                  updateBaseline(curF);
                sc = computeScores(curF);
                featHist[featHead] = curF;
                featHead = (featHead + 1) % FEAT_BUF;
                if (featCnt < FEAT_BUF)
                  featCnt++;
                if (bl.ready)
                  runHSM(curF, sc);
                xSemaphoreGive(mtx);
              }
            }
            lastFeat = now;
          }
          vTaskDelay(pdMS_TO_TICKS(5));
        }
      },
      "Sensor", 8192, NULL, 2, &sensorTaskH, 0);

  hsm.sys = SYS_CALIB;
  Serial.println(F("\n[BOOT] Running. Calibrating 60s...\n"));
}

// ======================== LOOP ========================================
void loop() {
  uint32_t now = millis();

  // WiFi reconnect
  if (now - lastWiFi >= WIFI_RETRY_MS) {
    if (WiFi.status() == WL_CONNECTED)
      wifiOK = true;
    else {
      wifiOK = false;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    lastWiFi = now;
  }

  // Button ack
  if (digitalRead(BUTTON_PIN) == LOW && (now - lastBtn > DEBOUNCE_MS)) {
    lastBtn = now;
    if (hsm.threat >= T_WATCH) {
      hsm.acked = true;
      Serial.println(F("[ACK] Button pressed"));
    }
  }

  // Escalation
  if (hsm.threat == T_ALERT && !hsm.acked &&
      (now - hsm.alert_at) > ESCALATE_L2_MS) {
    changeThreat(T_EMERGENCY, hsm.eclass, "Code Blue — no response");
  }

  // Buzzer
  if (!buzzerOn) {
    digitalWrite(BUZZER_PIN, LOW);
  } else {
    uint32_t iv = (hsm.threat == T_EMERGENCY) ? 150 : 500;
    if (now - lastBuzz > iv) {
      buzzerSt = !buzzerSt;
      digitalWrite(BUZZER_PIN, buzzerSt ? HIGH : LOW);
      lastBuzz = now;
    }
  }

  // ===================================================================
  // THINGSPEAK UPLOAD
  //   Field 1 = PIR %       (0-100)
  //   Field 2 = Temperature (Celsius)
  //   Field 3 = Humidity    (%)
  //   Field 4 = Distance    (cm)
  //   Field 5 = Anomaly score
  //   Field 6 = Threat level
  //   Field 7 = Motion energy
  //   Field 8 = Emergency class
  // ===================================================================
  if (now - lastTSUp >= THINGSPEAK_MS) {
    if (wifiOK) {
      float upPIR, upTemp, upHumid, upDist, upScore, upEnergy;
      int upThreat, upClass;

      if (xSemaphoreTake(mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        upPIR = curF.pir_duty;
        upTemp = curF.temp_mean;
        upHumid = curF.humid_mean;
        upDist = curF.d_mean;
        upScore = sc.fused;
        upThreat = (int)hsm.threat;
        upEnergy = curF.motion_energy;
        upClass = (int)hsm.eclass;
        xSemaphoreGive(mtx);
      }

      // Skip if no real data yet
      if (upTemp < 0.1f && upHumid < 0.1f) {
        Serial.println(F("[TS] Skip — no real DHT data yet"));
      } else {
        Serial.println(F("--- ThingSpeak Upload ---"));
        Serial.print(F("  F1 PIR%   = "));
        Serial.println(upPIR * 100.0f, 1);
        Serial.print(F("  F2 TEMP   = "));
        Serial.println(upTemp, 1);
        Serial.print(F("  F3 HUMID  = "));
        Serial.println(upHumid, 1);
        Serial.print(F("  F4 DIST   = "));
        Serial.println(upDist, 1);
        Serial.print(F("  F5 SCORE  = "));
        Serial.println(upScore, 3);
        Serial.print(F("  F6 THREAT = "));
        Serial.println(upThreat);

        ThingSpeak.setField(1, upPIR * 100.0f); // Field 1 = PIR %
        ThingSpeak.setField(2, upTemp);         // Field 2 = Temperature
        ThingSpeak.setField(3, upHumid);        // Field 3 = Humidity
        ThingSpeak.setField(4, upDist);         // Field 4 = Distance
        ThingSpeak.setField(5, upScore);        // Field 5 = Anomaly
        ThingSpeak.setField(6, upThreat);       // Field 6 = Threat
        ThingSpeak.setField(7, upEnergy);       // Field 7 = Energy
        ThingSpeak.setField(8, upClass);        // Field 8 = Class

        String st = threatStr(hsm.threat) + "|" + classStr(hsm.eclass) +
                    "|S=" + String(upScore, 2);
        ThingSpeak.setStatus(st);

        int code = ThingSpeak.writeFields(TS_CHANNEL, TS_WRITE_KEY);
        tsOK = (code == 200);
        if (tsOK) {
          totalUploads++;
          Serial.println(F("[TS] OK"));
        } else {
          Serial.print(F("[TS] FAIL code="));
          Serial.println(code);
        }
      }
    }
    lastTSUp = now;
  }

  // ===================================================================
  // OLED UPDATE
  // ===================================================================
  if (now - lastOLED >= OLED_REFRESH_MS) {
    if (oledOK && oled != NULL) {
      oled->clearDisplay();
      oled->setTextSize(1);
      oled->setTextColor(SSD1306_WHITE);

      if (hsm.threat >= T_ALERT) {
        // --- ALERT / CODE BLUE overlay ---
        static bool fl = false;
        static uint32_t lf = 0;
        uint32_t iv = (hsm.threat == T_EMERGENCY) ? 300 : 600;
        if (now - lf > iv) {
          fl = !fl;
          lf = now;
        }
        if (fl) {
          oled->drawRect(0, 0, 128, 64, SSD1306_WHITE);
          oled->drawRect(2, 2, 124, 60, SSD1306_WHITE);
        }
        oled->setTextSize(2);
        oled->setCursor(5, 5);
        if (hsm.threat == T_EMERGENCY)
          oled->println(F("CODE BLUE"));
        else
          oled->println(F("!! ALERT"));
        oled->setTextSize(1);
        oled->setCursor(5, 30);
        oled->println(classStr(hsm.eclass));
        oled->setCursor(5, 42);
        oled->print(F("Score: "));
        oled->println(hsm.peak_score, 2);
        oled->setCursor(5, 54);
        oled->println(F("BOOT = I'm OK"));
      } else {
        // --- Status bar ---
        oled->setCursor(0, 0);
        oled->print(wifiOK ? F("W ") : F("- "));
        oled->print(threatStr(hsm.threat));
        oled->setCursor(75, 0);
        oled->print(timeStr());
        oled->drawLine(0, 10, 127, 10, SSD1306_WHITE);

        // Page auto-rotate every 4 seconds
        if (now - lastPageSwap > 4000) {
          dispPage = (dispPage + 1) % 5;
          lastPageSwap = now;
        }

        switch (dispPage) {
        case 0: // STATUS
          oled->setCursor(0, 13);
          oled->print(F("Posture: "));
          oled->println(postureStr(hsm.posture));
          oled->print(F("Score:   "));
          oled->println(sc.fused, 3);
          oled->print(F("Temp:    "));
          oled->print(curF.temp_mean, 1);
          oled->println(F(" C"));
          oled->print(F("Humid:   "));
          oled->print(curF.humid_mean, 1);
          oled->println(F(" %"));
          oled->print(F("Alerts:  "));
          oled->println(totalAlerts);
          break;

        case 1: // SENSORS
          oled->setCursor(0, 13);
          oled->print(F("PIR:     "));
          oled->print(curF.pir_duty * 100.0f, 1);
          oled->println(F(" %"));
          oled->print(F("Trans:   "));
          oled->println(curF.pir_trans);
          oled->print(F("Energy:  "));
          oled->println(curF.motion_energy, 2);
          oled->print(F("Dist:    "));
          if (usonicOK) {
            oled->print(curF.d_mean, 1);
            oled->println(F(" cm"));
          } else {
            oled->println(F("N/A"));
          }
          oled->print(F("EnvAnm:  "));
          oled->println(curF.env_anomaly, 2);
          break;

        case 2: // Z-SCORES
          oled->setCursor(0, 13);
          oled->println(F("Z-Scores:"));
          oled->print(F(" Motion:   "));
          oled->println(sc.mot_z, 1);
          oled->print(F(" Distance: "));
          oled->println(sc.dst_z, 1);
          oled->print(F(" Temp:     "));
          oled->println(sc.tmp_z, 1);
          oled->print(F(" Humid:    "));
          oled->println(sc.hum_z, 1);
          oled->print(F(" Pattern:  "));
          oled->println(sc.pat_z, 1);
          break;

        case 3: // SYSTEM
          oled->setCursor(0, 13);
          oled->print(F("Up: "));
          oled->print(uptimeSec / 3600);
          oled->print(F("h "));
          oled->print((uptimeSec % 3600) / 60);
          oled->print(F("m "));
          oled->println(uptimeSec % 60);
          oled->print(F("Samples: "));
          oled->println(totalSamples);
          oled->print(F("Uploads: "));
          oled->print(totalUploads);
          oled->println(tsOK ? " OK" : " ERR");
          oled->print(F("RSSI: "));
          if (wifiOK) {
            oled->print(WiFi.RSSI());
            oled->println(F(" dBm"));
          } else {
            oled->println(F("OFF"));
          }
          break;

        case 4: // RAW DHT + EVENT
          oled->setCursor(0, 13);
          oled->print(F("DHT Temp:  "));
          oled->print(dhtTemp, 1);
          oled->println(F(" C"));
          oled->print(F("DHT Humid: "));
          oled->print(dhtHumid, 1);
          oled->println(F(" %"));
          oled->println();
          oled->println(F("Last event:"));
          if (evtCnt > 0) {
            int idx = evtHead - 1;
            if (idx < 0)
              idx += EVT_BUF;
            uint32_t ago = (millis() - evtLog[idx].ts) / 1000;
            oled->print(ago);
            oled->print(F("s ago "));
            oled->println(evtLog[idx].desc);
          } else {
            oled->println(F("  (none)"));
          }
          break;
        }
      }
      oled->display();
    }
    lastOLED = now;
  }

  // Heartbeat
  if (now - lastHB >= HEARTBEAT_MS) {
    uptimeSec = (now - bootTime) / 1000;
    if (hsm.sys == SYS_CALIB && (now - bootTime) > BASELINE_LEARN_MS) {
      hsm.sys = SYS_OK;
      bl.ready = true;
      Serial.println(F("[SYS] Calibration done → HEALTHY"));
      logEvt(EM_NONE, T_NORMAL, 0, "Cal done");
    }
    lastHB = now;
  }

  delay(10);
}

// ======================== ULTRASONIC ==================================
float readUltrasonic() {
  digitalWrite(ULTRASONIC_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(ULTRASONIC_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRASONIC_TRIG, LOW);
  long dur = pulseIn(ULTRASONIC_ECHO, HIGH, 30000);
  if (dur == 0)
    return -1.0f;
  return (dur * SPEED_OF_SOUND) / 2.0f;
}

// ======================== FEATURES ====================================
Features extractFeatures() {
  Features f;
  memset(&f, 0, sizeof(Features));
  int cnt = (int)rawCnt;
  if (cnt > WINDOW_SIZE)
    cnt = WINDOW_SIZE;
  if (cnt <= 0)
    return f;

  float dist[WINDOW_SIZE];
  int pirHi = 0, pirTr = 0, dCnt = 0;
  bool lp = false;
  float tS = 0, hS = 0, dS = 0, dMn = 9999, dMx = 0;

  for (int i = 0; i < cnt; i++) {
    int idx = (int)rawHead - cnt + i;
    if (idx < 0)
      idx += RAW_BUF;
    SensorReading &s = rawBuf[idx];
    if (s.pir)
      pirHi++;
    if (i > 0 && s.pir != lp)
      pirTr++;
    lp = s.pir;
    if (usonicOK && s.dist_valid) {
      dist[dCnt] = s.distance_cm;
      dS += s.distance_cm;
      if (s.distance_cm < dMn)
        dMn = s.distance_cm;
      if (s.distance_cm > dMx)
        dMx = s.distance_cm;
      dCnt++;
    }
    tS += s.temp;
    hS += s.humid;
  }

  if (dCnt > 0) {
    f.d_mean = dS / (float)dCnt;
    f.d_min = dMn;
    f.d_max = dMx;
    f.d_std = calcStd(dist, dCnt, f.d_mean);
  }
  f.d_delta = f.d_mean - prevF.d_mean;
  f.pir_duty = (float)pirHi / (float)cnt;
  f.pir_trans = pirTr;
  f.pir_high = pirHi;
  f.temp_mean = tS / (float)cnt;
  f.humid_mean = hS / (float)cnt;
  f.temp_delta = f.temp_mean - prevF.temp_mean;
  f.humid_delta = f.humid_mean - prevF.humid_mean;

  float p = (float)f.pir_trans / 10.0f;
  if (p > 1.0f)
    p = 1.0f;
  float d = f.d_std / 20.0f;
  if (d > 1.0f)
    d = 1.0f;
  f.motion_energy = f.pir_duty * 0.5f + p * 0.3f + d * 0.2f;

  f.env_anomaly = 0.0f;
  if (fabs(f.temp_delta) > 3.0f)
    f.env_anomaly += 0.6f;
  if (fabs(f.humid_delta) > 10.0f)
    f.env_anomaly += 0.4f;
  return f;
}

float calcStd(float *data, int n, float m) {
  if (n < 2)
    return 0.0f;
  float s = 0;
  for (int i = 0; i < n; i++) {
    float d = data[i] - m;
    s += d * d;
  }
  return sqrt(s / (float)(n - 1));
}

float clampf(float v, float lo, float hi) {
  if (v < lo)
    return lo;
  if (v > hi)
    return hi;
  return v;
}

// ======================== BASELINE ====================================
void initBaseline() {
  memset(&bl, 0, sizeof(Baseline));
  bl.d_mean_ema = 150.0f;
  bl.d_std_ema = 5.0f;
  bl.pir_duty_ema = 0.3f;
  bl.temp_ema = 0.0f;
  bl.humid_ema = 0.0f;
  bl.motion_ema = 0.3f;
}

void updateBaseline(Features &f) {
  bl.samples++;
  bl.d_mean_ema = EMA_ALPHA * f.d_mean + (1 - EMA_ALPHA) * bl.d_mean_ema;
  bl.d_std_ema = EMA_ALPHA * f.d_std + (1 - EMA_ALPHA) * bl.d_std_ema;
  bl.pir_duty_ema = EMA_ALPHA * f.pir_duty + (1 - EMA_ALPHA) * bl.pir_duty_ema;
  bl.temp_ema = EMA_ALPHA * f.temp_mean + (1 - EMA_ALPHA) * bl.temp_ema;
  bl.humid_ema = EMA_ALPHA * f.humid_mean + (1 - EMA_ALPHA) * bl.humid_ema;
  bl.motion_ema = EMA_ALPHA * f.motion_energy + (1 - EMA_ALPHA) * bl.motion_ema;
  if (bl.samples >= 100 && !bl.ready) {
    bl.ready = true;
    Serial.println(F("[BL] Baseline locked."));
  }
}

// ======================== ANOMALY =====================================
Scores computeScores(Features &f) {
  Scores s;
  memset(&s, 0, sizeof(Scores));
  if (!bl.ready)
    return s;

  float ps = bl.pir_duty_ema * 0.3f;
  if (ps < 0.05f)
    ps = 0.05f;
  s.mot_z = fabs(f.pir_duty - bl.pir_duty_ema) / ps;
  if (f.pir_duty < PIR_STILL_THRESH && bl.pir_duty_ema > 0.15f &&
      s.mot_z < 3.0f)
    s.mot_z = 3.0f;

  if (usonicOK) {
    float ds = bl.d_std_ema;
    if (ds < 3.0f)
      ds = 3.0f;
    s.dst_z = fabs(f.d_mean - bl.d_mean_ema) / ds;
    if (f.d_delta < -FALL_DROP_CM && s.dst_z < ZSCORE_FALL)
      s.dst_z = ZSCORE_FALL;
    if (f.d_mean > 0 && f.d_mean < FLOOR_MAX_CM && bl.d_mean_ema > 80.0f)
      s.dst_z += 1.5f;
  }

  s.tmp_z = fabs(f.temp_mean - bl.temp_ema) / 1.5f;
  s.hum_z = fabs(f.humid_mean - bl.humid_ema) / 5.0f;

  s.pat_z = 0.0f;
  if (featCnt >= 5) {
    int still = 0;
    float mb = 0;
    int lim = featCnt;
    if (lim > 10)
      lim = 10;
    for (int i = 0; i < lim; i++) {
      int idx = featHead - 1 - i;
      if (idx < 0)
        idx += FEAT_BUF;
      if (i < 3) {
        if (featHist[idx].pir_duty < PIR_STILL_THRESH)
          still++;
      } else {
        mb += featHist[idx].motion_energy;
      }
    }
    if (still >= 2 && mb > 1.5f) {
      s.pat_z = mb / 2.0f;
      if (s.pat_z > 4.0f)
        s.pat_z = 4.0f;
    }
  }

  float c0 = clampf(s.mot_z / ZSCORE_FALL, 0, 1) * W_MOT;
  float c1 = clampf(s.dst_z / ZSCORE_FALL, 0, 1) * W_DST;
  float c2 = clampf(s.tmp_z / ZSCORE_ANOMALY, 0, 1) * W_TMP;
  float c3 = clampf(s.hum_z / ZSCORE_ANOMALY, 0, 1) * W_HUM;
  float c4 = clampf(s.pat_z / 3.0f, 0, 1) * W_PAT;
  if (!usonicOK) {
    c0 += c1 * 0.6f;
    c4 += c1 * 0.4f;
    c1 = 0;
  }

  s.fused = c0 + c1 + c2 + c3 + c4;
  s.contrib[0] = c0;
  s.contrib[1] = c1;
  s.contrib[2] = c2;
  s.contrib[3] = c3;
  s.contrib[4] = c4;
  return s;
}

// ======================== POSTURE =====================================
Posture estPosture(Features &f) {
  if (!usonicOK) {
    if (f.pir_duty < 0.01f)
      return P_EMPTY;
    if (f.pir_duty < PIR_STILL_THRESH)
      return P_BED;
    if (f.pir_duty < 0.3f)
      return P_SEAT;
    return P_STAND;
  }
  if (f.pir_duty < 0.01f && f.d_std < 1.0f)
    return P_EMPTY;
  if (f.d_mean < FLOOR_MAX_CM)
    return P_FLOOR;
  if (f.d_mean < 80.0f)
    return P_BED;
  if (f.d_mean < 150.0f)
    return P_SEAT;
  if (f.d_mean < 250.0f)
    return P_STAND;
  return P_EMPTY;
}

// ======================== HSM =========================================
void initHSM() {
  memset(&hsm, 0, sizeof(HSM));
  hsm.sys = SYS_BOOT;
  hsm.threat = T_NORMAL;
  hsm.eclass = EM_NONE;
  hsm.posture = P_UNK;
  hsm.state_at = millis();
  hsm.last_motion = millis();
}

EmClass classify(Features &f, Scores &s) {
  if (usonicOK && f.d_delta < -FALL_DROP_CM && s.pat_z > 1.5f)
    return EM_FALL;
  if (estPosture(f) == P_FLOOR && f.pir_duty < PIR_STILL_THRESH)
    return EM_POSTFALL;
  if (f.pir_duty < PIR_STILL_THRESH && s.mot_z > ZSCORE_ANOMALY)
    return EM_STILL;
  if (f.env_anomaly > 0.5f)
    return EM_THERMAL;
  if (f.pir_duty < 0.01f && bl.pir_duty_ema > 0.2f)
    return EM_OCCUPANCY;
  return EM_STILL;
}

void changeThreat(Threat t, EmClass ec, String reason) {
  Threat old = hsm.threat;
  if (old == t && hsm.eclass == ec)
    return;

  hsm.threat = t;
  hsm.eclass = ec;
  hsm.state_at = millis();
  hsm.reason = reason;
  hsm.acked = false;

  if (t == T_WATCH) {
    hsm.esc_level = 1;
  } else if (t == T_ALERT) {
    hsm.esc_level = 2;
    hsm.alert_at = millis();
    totalAlerts++;
  } else if (t == T_EMERGENCY) {
    hsm.esc_level = 3;
    hsm.emerg_at = millis();
  } else {
    hsm.esc_level = 0;
    hsm.peak_score = 0;
  }

  if (sc.fused > hsm.peak_score)
    hsm.peak_score = sc.fused;
  buzzerOn = (t >= T_ALERT);
  logEvt(ec, t, sc.fused, reason);

  Serial.print(F("[HSM] "));
  Serial.print(threatStr(old));
  Serial.print(F(" -> "));
  Serial.print(threatStr(t));
  Serial.print(F(" | "));
  Serial.println(reason);
}

void runHSM(Features &f, Scores &s) {
  uint32_t now = millis();
  hsm.posture = estPosture(f);
  if (f.pir_duty > PIR_STILL_THRESH)
    hsm.last_motion = now;

  switch (hsm.threat) {
  case T_NORMAL:
    if (s.fused >= 0.7f) {
      changeThreat(T_ALERT, classify(f, s), "High anomaly");
    } else if (s.fused >= 0.45f) {
      changeThreat(T_WATCH, EM_NONE, "Elevated");
    } else if ((now - hsm.last_motion) > STILLNESS_TIMEOUT_MS &&
               bl.pir_duty_ema > 0.1f) {
      changeThreat(T_WATCH, EM_STILL, "Prolonged stillness");
    }
    break;

  case T_WATCH:
    if (s.fused >= 0.7f) {
      changeThreat(T_ALERT, classify(f, s), "Watch -> Alert");
    } else if (s.fused < 0.2f) {
      changeThreat(T_NORMAL, EM_NONE, "Cleared");
    } else if ((now - hsm.state_at) > ESCALATE_L1_MS) {
      changeThreat(T_ALERT, classify(f, s), "Watch timeout");
    }
    break;

  case T_ALERT:
    if (hsm.acked) {
      changeThreat(T_NORMAL, EM_NONE, "Acknowledged");
      break;
    }
    if (f.pir_duty > 0.3f && f.motion_energy > 0.4f &&
        (now - hsm.alert_at) > 5000) {
      changeThreat(T_NORMAL, EM_NONE, "Movement resumed");
    }
    break;

  case T_EMERGENCY:
    if (hsm.acked) {
      changeThreat(T_NORMAL, EM_NONE, "Emergency acked");
    }
    break;
  }
}

// ======================== EVENT LOG ===================================
void logEvt(EmClass ec, Threat tl, float score, String desc) {
  Event e;
  e.ts = millis();
  e.ec = ec;
  e.tl = tl;
  e.score = score;
  e.desc = desc;
  evtLog[evtHead] = e;
  evtHead = (evtHead + 1) % EVT_BUF;
  if (evtCnt < EVT_BUF)
    evtCnt++;
}

// ======================== STRINGS =====================================
String timeStr() {
  struct tm ti;
  if (!getLocalTime(&ti))
    return String("--:--");
  char b[10];
  strftime(b, sizeof(b), "%H:%M", &ti);
  return String(b);
}

String threatStr(Threat t) {
  switch (t) {
  case T_NORMAL:
    return "NORM";
  case T_WATCH:
    return "WATCH";
  case T_ALERT:
    return "ALRT";
  case T_EMERGENCY:
    return "EMRG";
  }
  return "?";
}

String classStr(EmClass e) {
  switch (e) {
  case EM_NONE:
    return "NONE";
  case EM_FALL:
    return "FALL";
  case EM_POSTFALL:
    return "POSTF";
  case EM_STILL:
    return "STILL";
  case EM_THERMAL:
    return "THERM";
  case EM_OCCUPANCY:
    return "OCC";
  case EM_SENSOR:
    return "SENS";
  }
  return "?";
}

String postureStr(Posture p) {
  switch (p) {
  case P_UNK:
    return "UNK";
  case P_EMPTY:
    return "EMPTY";
  case P_STAND:
    return "STAND";
  case P_SEAT:
    return "SEAT";
  case P_BED:
    return "BED";
  case P_FLOOR:
    return "FLOOR";
  }
  return "?";
}
