// ==========================================================
// MEDICAL SMARTWATCH UI (FREERTOS VERSION)
// CORE 0 = SENSORS
// CORE 1 = TFT/UI
// ORIGINAL LOGIC PRESERVED
// ==========================================================

#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <math.h>
#include "spo2_algorithm.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <MAX30105.h>
#include "heartRate.h"
#include "time.h"

#define BLYNK_TEMPLATE_ID "TMPL3fuCOPdAe"
#define BLYNK_TEMPLATE_NAME "digital icu"
#define BLYNK_AUTH_TOKEN "tiqmuJ2ZOkiOzO9jfxIg0lK-V4zERlM8"

#include <BlynkSimpleEsp32.h>

#define BUZZER_PIN 27

#include <Preferences.h>
String activeMedicine = "";
Preferences prefs;
bool prescriptionPageLoaded = false;
String doctorPrescription = "";
String medicine1 = "";
String time1 = "";

String medicine2 = "";
String time2 = "";

String medicine3 = "";
String time3 = "";
bool medicineAlert = false;
bool ack1 = false;
bool ack2 = false;
bool ack3 = false;
BLYNK_WRITE(V10)
{
  doctorPrescription = param.asStr();
  prefs.putString("pres", doctorPrescription);

  prescriptionPageLoaded = false;
}

BLYNK_WRITE(V11)
{
  medicine1 = param.asStr();
  prefs.putString("med1", medicine1);
}

BLYNK_WRITE(V12)
{
  time1 = param.asStr();
  prefs.putString("time1", time1);
}

BLYNK_WRITE(V13)
{
  medicine2 = param.asStr();
  prefs.putString("med2", medicine2);
}

BLYNK_WRITE(V14)
{
  time2 = param.asStr();
  prefs.putString("time2", time2);
}

BLYNK_WRITE(V15)
{
  medicine3 = param.asStr();
  prefs.putString("med3", medicine3);
}

BLYNK_WRITE(V16)
{
  time3 = param.asStr();
  prefs.putString("time3", time3);
}
// ================= TFT =================
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);

// ================= TOUCH =================
#define NEXT_TOUCH T3
#define PREV_TOUCH T4

// ================= ECG =================
#define ECG_PIN 35
#define LO_PLUS 25
#define LO_MINUS 26

// ================= MAX30105 =================
MAX30105 particleSensor;

// ==========================================================
// VARIABLES
// ==========================================================
#define NTC_PIN 32

#define NTC_NOMINAL 100000.0
#define SERIES_RESISTOR 100000.0
#define BETA 3950.0

float bodyTemp = 0;      // now from NTC
float ntcEMA = 0;
float alphaNTC = 0.2;

String tempStatus = "NORMAL";
// ================= ECG BPM =================
#define ECG_BUFFER_SIZE 150

int ecgBuffer[ECG_BUFFER_SIZE];
int ecgIndex = 0;

float ecgBPM = 0;
float ecgBPM_Ema = 0;

unsigned long lastBeatTime = 0;
bool peakDetected = false;
String ecgStatus = "NORMAL";
// ================= WIFI =================


const char* ssid = "Sidhu";
const char* password = "12345678";

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800; // IST = UTC +5:30
const int daylightOffset_sec = 0;

bool wifiConnected = false;
int batteryPercent = 0;
int ecgX = 10;
int ecgPrevY = 72;

float filtered = 2000;

int currentPage = 0;
int targetPage = 0;

bool isTransitioning = false;
int transitionX = 0;

unsigned long lastTouch = 0;

bool leadsConnected = true;
bool ecgPageLoaded = false;

bool fingerDetected = false;

// ==========================================================
// MAX30105 VARIABLES
// ==========================================================

#define BUFFER_SIZE 120

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

int32_t spo2;
int8_t validSPO2;

int32_t heartRate;
int8_t validHeartRate;

float bpmEMA = 0;
float spo2EMA = 0;

float alphaBPM = 0.15;
float alphaSpO2 = 0.25;

uint32_t irBaseline = 0;

int bufferIndex = 0;

// ==========================================================
// PAGE SWITCH
// ==========================================================

void changePage(int newPage)
{
  prescriptionPageLoaded = false;

  targetPage = newPage;
  isTransitioning = true;
  transitionX = 0;
}
void checkPageSwitch()
{
  int nextVal = touchRead(NEXT_TOUCH);
  int prevVal = touchRead(PREV_TOUCH);

  bool nextPressed = nextVal < 30;
  bool prevPressed = prevVal < 30;

  if (nextPressed && millis() - lastTouch > 300)
  {
    lastTouch = millis();

    int np = currentPage + 1;
    if (np > 5) np = 0;

    changePage(np);
  }

  if (prevPressed && millis() - lastTouch > 300)
  {
    lastTouch = millis();

    int pp = currentPage - 1;
    if (pp < 0) pp = 5;

    changePage(pp);
  }
}

// ==========================================================
// TRANSITION
// ==========================================================

void handleTransition()
{
  transitionX += 20;

  tft.fillScreen(ST77XX_BLACK);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10 - transitionX, 60);
  tft.print("SWITCHING...");

  if (transitionX >= 160)
  {
    transitionX = 0;
    isTransitioning = false;
    currentPage = targetPage;
  }
}

// ==========================================================
// LEADS CHECK
// ==========================================================

void checkLeads()
{
  int loP = digitalRead(LO_PLUS);
  int loN = digitalRead(LO_MINUS);

  leadsConnected = !(loP || loN);
}

// ==========================================================
// FINGER CHECK
// ==========================================================

bool isFinger(uint32_t ir)
{
  irBaseline = (irBaseline * 9 + ir) / 10;

  if (irBaseline < 10000) return false;

  if (ir < 30000) return false;

  return true;
}
//battery
void readBattery()
{
  int raw = analogRead(34);   // SAFE ADC PIN (no ECG interference)

  float voltage = raw * (3.3 / 4095.0) * 2;  // 10k + 10k divider

  batteryPercent = map(voltage * 100, 300, 420, 0, 100);
  batteryPercent = constrain(batteryPercent, 0, 100);
}
void checkMedicineTouch()
{
  if (!medicineAlert) return;

  int nextVal = touchRead(NEXT_TOUCH);
  int prevVal = touchRead(PREV_TOUCH);

  if (nextVal < 30 || prevVal < 30)
  {
    medicineAlert = false;
    digitalWrite(BUZZER_PIN, LOW);

    // Mark only the active medicine as done
    if(activeMedicine == medicine1) ack1 = true;
    if(activeMedicine == medicine2) ack2 = true;
    if(activeMedicine == medicine3) ack3 = true;
  }
}

void readNTC()
{
  int raw = analogRead(NTC_PIN);
  if (raw <= 0) return;

  float voltage = raw * (3.3 / 4095.0);

  if (voltage <= 0.01 || voltage >= 3.29) return;

  const float R_PULLDOWN = 10000.0; // 10k resistor
  const float VCC = 3.3;

  // ===== correct resistance calculation =====
  float resistance = (VCC * R_PULLDOWN / voltage) - R_PULLDOWN;

  if (resistance <= 0) return;

  // ===== Beta equation for 100K NTC =====
  float steinhart;

  steinhart = resistance / 100000.0;   // ✅ FIXED (100K NTC)
  steinhart = log(steinhart);
  steinhart /= 3950.0;
  steinhart += 1.0 / (25.0 + 273.15);
  steinhart = 1.0 / steinhart;
  steinhart -= 273.15;

  float tempC = steinhart;

  // smoothing
  if (ntcEMA == 0)
    ntcEMA = tempC;
  else
    ntcEMA = (alphaNTC * tempC) + ((1 - alphaNTC) * ntcEMA);

  bodyTemp = ntcEMA;

  if (bodyTemp < -5 || bodyTemp > 80)
    bodyTemp = 0;

  if (bodyTemp < 25)
    tempStatus = "LOW TEMP";
  else if (bodyTemp > 37.5 && bodyTemp <= 39)
    tempStatus = "FEVER";
  else if (bodyTemp > 39)
    tempStatus = "HIGH FEVER";
  else
    tempStatus = "NORMAL";
}
// ==========================================================
// READ MAX30105
// ==========================================================

void readMAX30105()
{
  uint32_t irValue  = particleSensor.getIR();
  uint32_t redValue = particleSensor.getRed();

  if (!isFinger(irValue))
  {
    fingerDetected = false;

    bpmEMA = 0;
    spo2EMA = 0;

    bufferIndex = 0;

    return;
  }

  fingerDetected = true;

  irBuffer[bufferIndex] = irValue;
  redBuffer[bufferIndex] = redValue;

  bufferIndex++;

  if (bufferIndex < BUFFER_SIZE) return;

  bufferIndex = 0;

  maxim_heart_rate_and_oxygen_saturation(
    irBuffer,
    BUFFER_SIZE,
    redBuffer,
    &spo2,
    &validSPO2,
    &heartRate,
    &validHeartRate
  );

  int hr = heartRate;

  if (!validHeartRate || hr < 40 || hr > 180)
    hr = bpmEMA;

  if (bpmEMA == 0)
    bpmEMA = hr;
  else
    bpmEMA = (alphaBPM * hr) + ((1 - alphaBPM) * bpmEMA);

  if (bpmEMA > 180) bpmEMA = 180;
  if (bpmEMA < 40) bpmEMA = 40;

  int sp = spo2;

  if (!validSPO2 || sp < 85 || sp > 100)
    sp = spo2EMA;

  if (spo2EMA == 0)
    spo2EMA = sp;
  else
    spo2EMA = (alphaSpO2 * sp) + ((1 - alphaSpO2) * spo2EMA);

  if (spo2EMA > 100) spo2EMA = 100;
  if (spo2EMA < 80) spo2EMA = 80;
}
void checkMedicineAlert()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  char currentTime[6];
  sprintf(currentTime, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  String nowTime = String(currentTime);

  // MEDICINE 1
  if(nowTime == time1 && !ack1)
  {
    activeMedicine = medicine1;
    medicineAlert = true;
  }

  // MEDICINE 2
  if(nowTime == time2 && !ack2)
  {
    activeMedicine = medicine2;
    medicineAlert = true;
  }

  // MEDICINE 3
  if(nowTime == time3 && !ack3)
  {
    activeMedicine = medicine3;
    medicineAlert = true;
  }

  // RESET DAILY (MIDNIGHT)
  if(timeinfo.tm_hour == 0 && timeinfo.tm_min == 0)
  {
    ack1 = false;
    ack2 = false;
    ack3 = false;
  }
}
// ==========================================================
// CLOCK PAGE
// ==========================================================

void drawClockPage()
{
  tft.fillScreen(ST77XX_BLACK);

  // ================= TOP BAR =================
  tft.fillRect(0, 0, 160, 20, 0x018C);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(5, 6);
  tft.print("MEDICAL WATCH");

  // ================= WIFI ICON =================
  int wx = 120;
  int wy = 10;

  if (WiFi.status() == WL_CONNECTED)
  {
    tft.drawCircle(wx, wy, 6, ST77XX_BLUE);
    tft.drawCircle(wx, wy, 4, ST77XX_BLUE);
    tft.fillCircle(wx, wy, 1, ST77XX_BLUE);
  }
  else
  {
    tft.drawCircle(wx, wy, 6, ST77XX_RED);
    tft.drawLine(wx - 6, wy - 6, wx + 6, wy + 6, ST77XX_RED);
  }

  // ================= BATTERY =================
  int bx = 140;
  int by = 4;

  tft.drawRect(bx, by, 16, 8, ST77XX_WHITE);
  tft.fillRect(bx + 16, by + 2, 2, 4, ST77XX_WHITE);

  uint16_t color = ST77XX_GREEN;
  if (batteryPercent < 30) color = ST77XX_RED;
  else if (batteryPercent < 60) color = ST77XX_YELLOW;

  int fillW = map(batteryPercent, 0, 100, 0, 12);
  tft.fillRect(bx + 2, by + 2, fillW, 4, color);

  // ================= TIME =================
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return;

  int sec = timeinfo.tm_sec;
  int min = timeinfo.tm_min;
  int hr  = timeinfo.tm_hour % 12;
  if (hr == 0) hr = 12;

  // ================= CLOCK FACE =================
  int cx = 80;
  int cy = 70;

  // 3 rings
  tft.drawCircle(cx, cy, 45, ST77XX_BLUE);
  tft.drawCircle(cx, cy, 44, ST77XX_CYAN);
  tft.drawCircle(cx, cy, 43, ST77XX_WHITE);

  // markers
  for (int i = 0; i < 12; i++)
  {
    float a = (i * 30 - 90) * 0.0174533;

    int x1 = cx + cos(a) * 35;
    int y1 = cy + sin(a) * 35;

    int x2 = cx + cos(a) * 42;
    int y2 = cy + sin(a) * 42;

    if (i % 3 == 0)
    {
      tft.drawLine(x1, y1, x2, y2, ST77XX_WHITE);
      tft.drawLine(x1 + 1, y1, x2 + 1, y2, ST77XX_WHITE);
    }
    else
    {
      tft.drawLine(x1, y1, x2, y2, ST77XX_CYAN);
    }
  }

  // inside numbers
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  tft.setCursor(74, 32);  tft.print("12");
  tft.setCursor(112, 64); tft.print("3");
  tft.setCursor(78, 96);  tft.print("6");
  tft.setCursor(40, 64);  tft.print("9");

  // ================= HANDS =================
  float sa = (sec * 6 - 90) * 0.0174533;
  float ma = (min * 6 - 90) * 0.0174533;
  float ha = ((hr * 30) + (min * 0.5) - 90) * 0.0174533;

  // hour
  tft.drawLine(cx, cy,
               cx + cos(ha) * 22,
               cy + sin(ha) * 22,
               ST77XX_CYAN);

  // minute
  tft.drawLine(cx, cy,
               cx + cos(ma) * 30,
               cy + sin(ma) * 30,
               ST77XX_WHITE);

  // second
  tft.drawLine(cx, cy,
               cx + cos(sa) * 38,
               cy + sin(sa) * 38,
               ST77XX_RED);

  // center
  tft.fillCircle(cx, cy, 5, ST77XX_BLUE);
  tft.drawCircle(cx, cy, 5, ST77XX_WHITE);
  tft.fillCircle(cx, cy, 2, ST77XX_WHITE);

  // ================= DATE =================
  char dateBuffer[32];

  strftime(dateBuffer,
           sizeof(dateBuffer),
           "%a | %d %b %Y",
           &timeinfo);

  tft.fillRoundRect(10, 108, 140, 16, 5, 0x31A6);

  tft.setCursor(16, 113);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(dateBuffer);
}

// ==========================================================
// HEALTH PAGE
// ==========================================================

void drawHealthPage()
{
  tft.fillScreen(ST77XX_BLACK);

  tft.fillRect(0, 0, 160, 22, ST77XX_BLUE);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(55, 6);
  tft.print("HEALTH");

  // BPM BOX
  tft.fillRoundRect(10, 35, 65, 50, 8, 0x0208);

  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(20, 40);
  tft.print("BPM");

  tft.setTextSize(2);
  tft.setCursor(18, 55);

  if (fingerDetected && validHeartRate)
    tft.print((int)bpmEMA);
  else
    tft.print(0);

  // SpO2 BOX
  tft.fillRoundRect(85, 35, 65, 50, 8, 0x0180);

  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(1);
  tft.setCursor(95, 40);
  tft.print("SpO2");

  tft.setTextSize(2);
  tft.setCursor(95, 55);

  if (fingerDetected && validSPO2)
    tft.print((int)spo2EMA);
  else
    tft.print(0);

  // ECG STATUS
  tft.setTextSize(1);
  tft.setCursor(20, 100);

  if (leadsConnected)
  {
    tft.setTextColor(ST77XX_GREEN);
    tft.print("ECG: SIGNAL OK");
  }
  else
  {
    tft.setTextColor(ST77XX_RED);
    tft.print("LEADS OFF!");
  }

  // FINGER STATUS
  tft.setCursor(10, 115);

  if (fingerDetected)
  {
    tft.setTextColor(ST77XX_GREEN);
    tft.print("FINGER: OK");
  }
  else
  {
    tft.setTextColor(ST77XX_RED);
    tft.print("PUT FINGER");
  }
}

// ==========================================================
// ECG PAGE
// ==========================================================

void drawECGPage()
{
  tft.fillScreen(ST77XX_BLACK);

  tft.fillRect(0, 0, 160, 22, ST77XX_RED);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(70, 6);
  tft.print("ECG");

  tft.drawRoundRect(5, 28, 150, 80, 8, ST77XX_GREEN);

  tft.fillRoundRect(5, 112, 150, 14, 4, 0x2104);

// ================= ECG BPM =================
tft.fillRect(10, 112, 60, 14, ST77XX_BLACK);
tft.setCursor(10, 115);
tft.setTextColor(ST77XX_CYAN);
tft.setTextSize(1);
tft.print("BPM:");
tft.print((int)ecgBPM_Ema);

// ================= STATUS =================
tft.setCursor(80, 115);

if (ecgStatus == "NORMAL")
  tft.setTextColor(ST77XX_GREEN);
else
  tft.setTextColor(ST77XX_RED);

tft.print(ecgStatus);

  ecgX = 10;
  ecgPrevY = 72;
}


// ==========================================================
// ECG DRAW
// ==========================================================

void drawECG()
{
  int raw = analogRead(ECG_PIN);
  calculateECGBPM(raw);
  filtered = (filtered * 0.90) + (raw * 0.10);

  int y = map(filtered, 1200, 3000, 100, 40);

  if (y < 45) y = 45;
  if (y > 100) y = 100;

  tft.drawFastVLine(ecgX, 45, 55, ST77XX_BLACK);

  tft.drawLine(ecgX - 1, ecgPrevY, ecgX, y, ST77XX_GREEN);

  ecgPrevY = y;

  ecgX++;

  if (ecgX > 145)
  {
    ecgX = 10;
    ecgPrevY = 72;

    tft.fillRect(6, 45, 148, 55, ST77XX_BLACK);

    tft.drawRoundRect(5, 28, 150, 80, 8, ST77XX_GREEN);
  }
}
void calculateECGBPM(int raw)
{
  static float filtered = 2000;
  static float prev = 2000;

  static unsigned long lastBeat = 0;

  // smooth ECG
  filtered = (filtered * 0.92) + (raw * 0.08);

  ecgBuffer[ecgIndex] = filtered;
  ecgIndex++;
  if (ecgIndex >= ECG_BUFFER_SIZE) ecgIndex = 0;

  // detect R-wave (slope method)
  float diff = filtered - prev;

  // strong upward slope = heartbeat start
  if (diff > 60)
  {
    unsigned long now = millis();

    if (lastBeat > 0)
    {
      unsigned long interval = now - lastBeat;

      float bpm = 60000.0 / interval;

      if (bpm > 35 && bpm < 180)
      {
        ecgBPM_Ema = (0.30 * bpm) + (0.70 * ecgBPM_Ema);
      }
    }

    lastBeat = now;
  }

  prev = filtered;

  // ================= STATUS =================
 if (!leadsConnected)
{
  ecgStatus = "LEADS OFF";
}
else if (ecgBPM_Ema < 50 || ecgBPM_Ema > 120 || ecgBPM_Ema == 0)
{
  ecgStatus = "ABNORMAL";
}
else
{
  ecgStatus = "NORMAL";
}
}
void drawTempPage()
{
  tft.fillScreen(ST77XX_BLACK);

  tft.fillRect(0, 0, 160, 22, ST77XX_MAGENTA);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(50, 6);
  tft.print("PATIENT TEMP");

  // Temperature box
  tft.fillRoundRect(20, 40, 120, 60, 10, 0x4208);

  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(35, 55);
  tft.print(bodyTemp, 1);
  tft.print(" C");

  // Status
  tft.setTextSize(1);
  tft.setCursor(30, 95);

  if (tempStatus == "NORMAL")
    tft.setTextColor(ST77XX_GREEN);
  else if (tempStatus == "FEVER")
    tft.setTextColor(ST77XX_RED);
  else
    tft.setTextColor(ST77XX_RED);

  tft.print(tempStatus);

  // Alert banner
  if (tempStatus != "NORMAL")
  {
    tft.fillRect(0, 110, 160, 18, ST77XX_RED);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(25, 114);
    tft.print("ALERT! CHECK PATIENT");
  }
}
void drawPrescriptionPage()
{
  tft.fillScreen(ST77XX_BLACK);

  tft.fillRect(0, 0, 160, 20, ST77XX_BLUE);

  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(20, 6);
  tft.print("PRESCRIPTION");

  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(1);

  int x = 2;
  int y = 30;
  int lineHeight = 10;
  int maxWidth = 155;

  String word = "";

  for (int i = 0; i <= doctorPrescription.length(); i++)
  {
    char c = (i < doctorPrescription.length()) ? doctorPrescription[i] : ' ';

    if (c == ' ' || c == '\n')
    {
      int wordWidth = word.length() * 6;

      if (x + wordWidth > maxWidth)
      {
        x = 2;
        y += lineHeight;
      }

      tft.setCursor(x, y);
      tft.print(word);

      x += wordWidth + 6;
      word = "";

      if (c == '\n')
      {
        x = 2;
        y += lineHeight;
      }

      if (y > 120)
        break;
    }
    else
    {
      word += c;
    }
  }
}
void drawMedicineSchedulePage()
{
  tft.fillScreen(ST77XX_BLACK);

  tft.fillRect(0,0,160,20,ST77XX_RED);

  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(35,6);
  tft.print("MEDICINES");

  tft.setTextColor(ST77XX_YELLOW);

  tft.setCursor(5,35);
  tft.print("1. ");
  tft.print(medicine1);
  tft.print(" ");
  tft.print(time1);

  tft.setCursor(5,60);
  tft.print("2. ");
  tft.print(medicine2);
  tft.print(" ");
  tft.print(time2);

  tft.setCursor(5,85);
  tft.print("3. ");
  tft.print(medicine3);
  tft.print(" ");
  tft.print(time3);
}
// ==========================================================
// ================= FREERTOS TASKS =========================
// ==========================================================

// ==========================================================
// SENSOR TASK -> CORE 0
// ==========================================================
void BlynkTask(void *pvParameters)
{
  while (1)
  {
    Blynk.run();

    Blynk.virtualWrite(V0, (int)bpmEMA);
    Blynk.virtualWrite(V1, (int)spo2EMA);
    Blynk.virtualWrite(V2, bodyTemp);
    Blynk.virtualWrite(V3, batteryPercent);
    Blynk.virtualWrite(V4, (int)ecgBPM_Ema);
    Blynk.virtualWrite(V5, ecgStatus);

    // ===== ALERTS =====

    if(bodyTemp > 39)
    {
      Blynk.logEvent("high_fever",
                     "Patient temperature above 39C");
    }

    if(spo2EMA < 90 && fingerDetected)
    {
      Blynk.logEvent("low_spo2",
                     "SpO2 below 90%");
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void SensorTask(void *pvParameters)
{
  while (1)
  {
    checkLeads();

    readMAX30105();
    readNTC();
    readBattery();

    checkMedicineAlert();

    if (medicineAlert)
      digitalWrite(BUZZER_PIN, HIGH);
    else
      digitalWrite(BUZZER_PIN, LOW);

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
// ==========================================================
// TOUCH TASK -> CORE 0
// ==========================================================

void TouchTask(void *pvParameters)
{
  while (1)
  {
    checkMedicineTouch();

    if (!medicineAlert)
    {
      checkPageSwitch();
    }

    vTaskDelay(40 / portTICK_PERIOD_MS);
  }
}

// ==========================================================
// UI TASK -> CORE 1
// ==========================================================

void UITask(void *pvParameters)
{
  while (1)
  {
    // ===== Medicine Alert Overlay =====
    if (medicineAlert)
{
     drawMedicineAlert();

  vTaskDelay(100 / portTICK_PERIOD_MS);
  continue;
}

    // ===== Page Transition =====
    if (isTransitioning)
    {
      handleTransition();
      vTaskDelay(20 / portTICK_PERIOD_MS);
      continue;
    }

    // ===== CLOCK PAGE =====
    if (currentPage == 0)
    {
      ecgPageLoaded = false;
      drawClockPage();
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    // ===== HEALTH PAGE =====
    else if (currentPage == 1)
    {
      ecgPageLoaded = false;
      drawHealthPage();
      vTaskDelay(250 / portTICK_PERIOD_MS);
    }

    // ===== ECG PAGE =====
    else if (currentPage == 2)
    {
      if (!ecgPageLoaded)
      {
        drawECGPage();
        ecgPageLoaded = true;

        ecgX = 10;
        ecgPrevY = 72;
      }

      drawECG();
      ets_delay_us(4000);
    }

    // ===== TEMPERATURE PAGE =====
    else if (currentPage == 3)
    {
      ecgPageLoaded = false;
      drawTempPage();
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    // ===== PRESCRIPTION PAGE =====
 else if(currentPage == 4)
{
  if(!prescriptionPageLoaded)
  {
    drawPrescriptionPage();
    prescriptionPageLoaded = true;
  }

  vTaskDelay(100 / portTICK_PERIOD_MS);
}

    // ===== MEDICINE SCHEDULE PAGE =====
    else if (currentPage == 5)
    {
      ecgPageLoaded = false;
      drawMedicineSchedulePage();
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    vTaskDelay(1);
  }
}

void drawMedicineAlert()
{
  tft.fillScreen(ST77XX_RED);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);

  tft.setCursor(25,10);
  tft.print("MEDICINE");

  tft.setCursor(40,35);
  tft.print("TIME!");

  // Medicine Name Box
  tft.drawRect(5,60,150,30,ST77XX_WHITE);

  tft.setTextSize(1);
  tft.setCursor(10,72);
  tft.print("TAB: ");
  tft.print(activeMedicine);

  tft.drawRect(5,95,150,20,ST77XX_WHITE);

  tft.setCursor(10,102);
  tft.print("PLEASE TAKE NOW");

  tft.setCursor(15,120);
  tft.print("TOUCH T3/T4 TO STOP");
}
// ==========================================================
// SETUP
// ==========================================================

void setup()
{
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  prefs.begin("medical", false);

   doctorPrescription = prefs.getString("pres", "");

medicine1 = prefs.getString("med1", "");
time1 = prefs.getString("time1", "");

medicine2 = prefs.getString("med2", "");
time2 = prefs.getString("time2", "");

medicine3 = prefs.getString("med3", "");
time3 = prefs.getString("time3", "");
  Serial.begin(115200);
  ecgBPM_Ema = 75;   // default resting value
  analogReadResolution(12);
  analogSetPinAttenuation(ECG_PIN, ADC_11db);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);

  Wire.begin(21, 22);

  pinMode(LO_PLUS, INPUT);
  pinMode(LO_MINUS, INPUT);

  // ================= MAX30105 =================
  if (particleSensor.begin(Wire, I2C_SPEED_FAST))
  {
    particleSensor.setup(0x1F, 4, 2, 100, 411, 4096);
    particleSensor.setPulseAmplitudeRed(0x1F);
    particleSensor.setPulseAmplitudeIR(0x1F);
  }

  // ================= WIFI CONNECT (MOVE HERE) =================
 Blynk.begin(
  BLYNK_AUTH_TOKEN,
  ssid,
  password
);

  wifiConnected = true;

  // ================= NTP TIME =================
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // ================= FREERTOS TASKS =================
  xTaskCreatePinnedToCore(SensorTask, "SensorTask", 6000, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TouchTask, "TouchTask", 3000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(UITask, "UITask", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(
  BlynkTask,
  "BlynkTask",
  6000,
  NULL,
  1,
  NULL,
  1
);
}
// ==========================================================
// LOOP
// ==========================================================

void loop()
{
  // EMPTY
}
