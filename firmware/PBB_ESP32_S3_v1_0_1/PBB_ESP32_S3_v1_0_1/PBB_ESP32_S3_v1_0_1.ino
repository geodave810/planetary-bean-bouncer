/*
  Planetary Bean Bouncer - ESP32-S3 Touch Controller
  Integrated roast-controller version
  Date: 2026-08-21
  Board: ESP32S3 Dev Module
| Setting                      | Recommended                               |
| ---------------------------- | ----------------------------------------- |
| **Board**                    | ESP32S3 Dev Module                        |
| **Upload Speed**             | 921600 *(460800 if upload is unreliable)* |
| **USB Mode**                 | Hardware CDC and JTAG                     |
| **USB CDC On Boot**          | **Enabled**                               |
| **USB DFU On Boot**          | Disabled                                  |
| **Upload Mode**              | UART0 / Hardware CDC                      |
| **CPU Frequency**            | 240 MHz                                   |
| **Core Debug Level**         | None                                      |
| **USB Firmware MSC On Boot** | Disabled                                  |
| **Partition Scheme**         | Huge APP (3MB No OTA/1MB SPIFFS)          |
| **Flash Mode**               | QIO                                       |
| **Flash Size**               | **16MB**                                  |
| **PSRAM**                    | **OPI PSRAM**                             |
| **Arduino Runs On**          | Core 1                                    |
| **Events Run On**            | Core 1     
  Combines:
    - Proven PBB roast logic from PlanetaryBeanBouncer_ESP32_v0_2_1
    - Proven 4-inch TFT_eSPI + FT6336 touch + SdFat test sketch

  Included now:
    - DS3231 RTC
    - SHT31 ambient temperature/humidity
    - EC11 rotary encoder with coffee-origin selector
      Last selected origin is remembered across power cycles.
    - 480x320 TFT_eSPI display
    - FT6336 capacitive touch
    - Three touch buttons: MOTOR, RPM/MUTE, TIMER/T-CLR
    - MAX6675 thermocouple
    - TMC2209 standalone STEP/DIR/EN
    - Buzzer alerts
    - RAM roast logging and automatic CSV save to SD
    - Persistent last-selected coffee origin
    - RTC date + time on main display
    - EC11 control selection: MOTOR / DISC RPM / TIMER / COFFEE
    - RPM / gear-ratio configuration loaded from /PBB_CONFIG.TXT on the SD card
    - Disc RPM selections calculate the required motor RPM using the gear ratio
    - Highlighted EC11 selection background
    - Disc RPM and calculated motor RPM display


  Touch controls:
    MOTOR:
      Short press = motor ON/OFF

    RPM/MUTE:
      Short press = next disc RPM
      Long press  = buzzer mute/unmute

    TIMER/T-CLR:
      Short press = start/stop roast timer
      Long press  = clear timer/current roast
                    If a real roast has armed save logic,
                    firmware first attempts a manual SD save.

  NOTE ABOUT TFT_eSPI:
    This sketch assumes the same TFT_eSPI setup that was used by the
    known-good touch/SD test, with:
      
      TFT_MISO 9
      TFT_SCLK 10
      TFT_MOSI 11
      TFT_DC   12
      TFT_RST  13
      TFT_CS   14
*/

#define FW_NAME     "Planetary Bean Bouncer"
#define FW_VERSION  "v1.0.1"

//#define FW_VERSION  "ESP32-S3 Touch v0.4.3"  // Previous setting
#include <TFT_eSPI.h>
#include <Wire.h>
#include <RTClib.h>
#include <SPI.h>
#include <SdFat.h>
#include <max6675.h>
#include <Preferences.h>

// ============================================================
// Pin assignments
// ============================================================

// TFT pins are configured in TFT_eSPI exactly as in the working test:
// MOSI 13, MISO 15, SCLK 14, CS 10, DC 12, RST 11

// FT6336 capacitive touch
#define CTP_SDA   7
#define CTP_SCL   8
#define CTP_RST   15
#define CTP_INT   6
#define FT6336_ADDR 0x38

// SD card shares TFT SPI bus
#define SD_CS 16

// Physical TFT reset pin (matches TFT_eSPI setup).
#define PBB_TFT_RST 13

// MAX6675
#define TC1_SCK 17
#define TC1_CS  18
#define TC1_SO  21

// TMC2209 standalone
#define DIR_PIN  38
#define STEP_PIN 39
#define EN_PIN   40

// Buzzer - newly assigned spare GPIO
#define BUZZER_PIN 47

// DS3231 + SHT31 share a second I2C bus so the FT6336 touch bus
// can remain on Wire at GPIO 7/8.
#define SENSOR_SDA 5
#define SENSOR_SCL 4
#define DS3231_ADDR 0x68
#define SHT31_ADDR  0x44

// EC11 rotary encoder
#define ENC_A  42
#define ENC_B  2
#define ENC_SW 1

TwoWire SensorWire = TwoWire(1);
Preferences pbbPrefs;

// ============================================================
// RTC / SHT31
// ============================================================

RTC_DS3231 rtc;
bool rtcReady = false;
bool sht31Ready = false;

float ambientTempF = NAN;
float ambientHumidity = NAN;

const unsigned long SENSOR_SAMPLE_MS = 2000;
unsigned long lastSensorSampleMs = 0;

// ============================================================
// EC11 / Coffee origin selector
// ============================================================

const char* coffeeOrigins[] = {
  "Blend",
  "Bolivia",
  "Brazil",
  "Burundi",
  "Colombia",
  "Congo",
  "Costa Rica",
  "Dominican Republic",
  "Ecuador",
  "El Salvador",
  "Ethiopia",
  "Guatemala",
  "Haiti",
  "Hawaii - Maui",
  "Honduras",
  "India",
  "Indonesia",
  "Jamaica",
  "Kenya",
  "Malawi",
  "Mexico",
  "Nicaragua",
  "Panama",
  "Papua New Guinea",
  "Peru",
  "Rwanda",
  "Tanzania",
  "Thailand",
  "Uganda",
  "Vietnam",
  "Yemen",
  "Zambia",
  "Zimbabwe",
  "Other"
};

const int NUM_COFFEE_ORIGINS =
  sizeof(coffeeOrigins) / sizeof(coffeeOrigins[0]);

int selectedOriginIndex = 0;
int pendingOriginIndex = 0;

bool originSelectMode = false;
int encoderQuarterSteps = 0;

// EC11 normal-screen selection:
// 0 = MOTOR, 1 = DISC RPM, 2 = TIMER, 3 = COFFEE
uint8_t selectedControl = 0;

int lastEncoderState = 0;

static const int8_t encoderTransitionTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

const int ENC_COUNTS_PER_DETENT = 4;

bool lastEncoderButton = HIGH;
unsigned long lastEncoderButtonMs = 0;
const unsigned long ENC_BUTTON_DEBOUNCE_MS = 35;

// ============================================================
// Persistent coffee-origin selection
// ============================================================

const char* PREF_NAMESPACE = "pbb";
const char* PREF_ORIGIN_KEY = "origin";

void loadSavedOrigin() {
  pbbPrefs.begin(PREF_NAMESPACE, true);  // read-only
  int saved = pbbPrefs.getInt(PREF_ORIGIN_KEY, 0);
  pbbPrefs.end();

  if (saved >= 0 && saved < NUM_COFFEE_ORIGINS) {
    selectedOriginIndex = saved;
  } else {
    selectedOriginIndex = 0;
  }

  pendingOriginIndex = selectedOriginIndex;

  Serial.print("Saved coffee origin: ");
  Serial.println(coffeeOrigins[selectedOriginIndex]);
}

void saveSelectedOrigin() {
  pbbPrefs.begin(PREF_NAMESPACE, false);
  pbbPrefs.putInt(PREF_ORIGIN_KEY, selectedOriginIndex);
  pbbPrefs.end();

  Serial.print("Coffee origin saved: ");
  Serial.println(coffeeOrigins[selectedOriginIndex]);
}

// ============================================================
// PBB configuration globals
// ============================================================

const char* PBB_CONFIG_FILE = "/PBB_CONFIG.TXT";
const uint8_t MAX_DISC_RPMS = 16;

float discRPMPresets[MAX_DISC_RPMS];
uint8_t discRPMCount = 0;
float defaultDiscRPM = 20.0f;
float planetaryRatio = 5.18f;
float currentDiscRPM = 20.0f;

// ============================================================
// RTC / SHT31 helpers
// ============================================================

uint8_t sht31Crc8(uint8_t *data, uint8_t len) {
  uint8_t crc = 0xFF;

  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];

    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
  }

  return crc;
}

bool readSHT31(float &tempC, float &humidity) {
  SensorWire.beginTransmission(SHT31_ADDR);
  SensorWire.write(0x24);
  SensorWire.write(0x00);

  if (SensorWire.endTransmission() != 0) {
    return false;
  }

  delay(20);

  if (SensorWire.requestFrom(SHT31_ADDR, (uint8_t)6) != 6) {
    return false;
  }

  uint8_t data[6];

  for (uint8_t i = 0; i < 6; i++) {
    data[i] = SensorWire.read();
  }

  if (sht31Crc8(data, 2) != data[2] ||
      sht31Crc8(data + 3, 2) != data[5]) {
    return false;
  }

  uint16_t rawTemp = ((uint16_t)data[0] << 8) | data[1];
  uint16_t rawRH   = ((uint16_t)data[3] << 8) | data[4];

  tempC = -45.0f + 175.0f * ((float)rawTemp / 65535.0f);
  humidity = 100.0f * ((float)rawRH / 65535.0f);

  return true;
}

void updateEnvironmentalSensors() {
  if (rtcReady) {
    // Reading the RTC also confirms the shared sensor bus remains responsive.
    rtc.now();
  }

  if (sht31Ready) {
    float tempC;
    float humidity;

    if (readSHT31(tempC, humidity)) {
      ambientTempF = (tempC * 9.0f / 5.0f) + 32.0f;
      ambientHumidity = humidity;
    }
  }
}

String rtcDateTimeString() {
  if (!rtcReady) return "RTC ERROR";

  DateTime now = rtc.now();

  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           now.year(), now.month(), now.day(),
           now.hour(), now.minute(), now.second());

  return String(buf);
}

String rtcTimeString() {
  if (!rtcReady) return "--:--:--";

  DateTime now = rtc.now();

  char buf[12];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
           now.hour(), now.minute(), now.second());

  return String(buf);
}


TFT_eSPI tft = TFT_eSPI();
SdFat sd;
FsFile roastFile;

bool sdReady = false;

// ============================================================
// Thermocouple
// ============================================================

MAX6675 tc1(TC1_SCK, TC1_CS, TC1_SO);

const float TC1_OFFSET_F = 0.0f;

const unsigned long TEMP_SAMPLE_MS = 200;

// Roast logging interval. Temperature is still sampled every 200 ms,
// but a roast-history point is stored every 15 seconds.
const uint32_t ROAST_LOG_INTERVAL_SEC = 15;
unsigned long lastTempSampleMs = 0;

float lastTempF = NAN;
float lastTempC = NAN;
float maxTempF = NAN;
float maxTempC = NAN;

float TEMP_BEEP_2F = 300.0f;
float TEMP_BEEP_3F = 350.0f;
float TEMP_REARM_HYST_F = 5.0f;

const float SAVE_ARM_TEMP_F = 230.0f;
const float ROAST_END_TEMP_F = 200.0f;

// ============================================================
// Stepper / RPM
// ============================================================

#define FULL_STEPS_PER_REV 200.0f
#define MICROSTEPS         8.0f

void setActiveGearRatio(float ratio) {
  if (ratio > 0.01f) {
    planetaryRatio = ratio;
  }
}

float discRPMToMotorRPM(float discRPM) {
  if (planetaryRatio <= 0.01f) {
    return 0.0f;
  }

  return discRPM * planetaryRatio;
}

void addDiscRPM(float rpm) {
  if (discRPMCount >= MAX_DISC_RPMS || rpm <= 0.0f) {
    return;
  }

  discRPMPresets[discRPMCount++] = rpm;
}

void loadDefaultPBBConfig() {
  discRPMCount = 0;
  planetaryRatio = 5.18f;
  defaultDiscRPM = 20.0f;

  addDiscRPM(10.0f);
  addDiscRPM(15.0f);
  addDiscRPM(20.0f);
  addDiscRPM(25.0f);
  addDiscRPM(30.0f);
  addDiscRPM(35.0f);
  addDiscRPM(40.0f);
  addDiscRPM(45.0f);
}

bool createDefaultPBBConfigFile() {
  if (!sdReady) return false;

  FsFile file = sd.open(PBB_CONFIG_FILE,
                        O_WRONLY | O_CREAT | O_TRUNC);

  if (!file) {
    Serial.println("Could not create PBB_CONFIG.TXT");
    return false;
  }

  file.println("# Planetary Bean Bouncer Configuration");
  file.println("# Gear ratio: motor RPM / disc RPM");
  file.println();
  file.println("GEAR_RATIO=5.18");
  file.println();
  file.println("# Available disc RPM settings");
  file.println("DISC_RPM_1=10");
  file.println("DISC_RPM_2=15");
  file.println("DISC_RPM_3=20");
  file.println("DISC_RPM_4=25");
  file.println("DISC_RPM_5=30");
  file.println("DISC_RPM_6=35");
  file.println("DISC_RPM_7=40");
  file.println("DISC_RPM_8=45");
  file.println();
  file.println("# Default disc RPM at startup");
  file.println("DEFAULT_DISC_RPM=20");
  file.close();

  return true;
}

bool loadPBBConfigFromSD() {
  loadDefaultPBBConfig();

  if (!sdReady) {
    Serial.println("PBB config: SD unavailable, using built-in defaults.");
    return false;
  }

  FsFile file = sd.open(PBB_CONFIG_FILE, O_RDONLY);

  if (!file) {
    createDefaultPBBConfigFile();
    Serial.println("PBB config: created default PBB_CONFIG.TXT");
    return false;
  }

  // Start empty and rebuild from the file.
  discRPMCount = 0;
  planetaryRatio = 5.18f;
  defaultDiscRPM = 20.0f;

  char line[96];

  while (file.available()) {
    int length = file.readBytesUntil('\n', line, sizeof(line) - 1);
    line[length] = '\0';

    String entry = String(line);
    entry.trim();

    if (entry.length() == 0 || entry.startsWith("#")) {
      continue;
    }

    int separator = entry.indexOf('=');
    if (separator <= 0) {
      continue;
    }

    String key = entry.substring(0, separator);
    String value = entry.substring(separator + 1);

    key.trim();
    value.trim();

    float numericValue = value.toFloat();

    if (key.equalsIgnoreCase("GEAR_RATIO")) {
      if (numericValue > 0.01f) {
        planetaryRatio = numericValue;
      }
    }
    else if (key.equalsIgnoreCase("DEFAULT_DISC_RPM")) {
      if (numericValue > 0.0f) {
        defaultDiscRPM = numericValue;
      }
    }
    else if (key.startsWith("DISC_RPM_")) {
      if (numericValue > 0.0f) {
        addDiscRPM(numericValue);
      }
    }
  }

  file.close();

  if (discRPMCount == 0) {
    // Keep the ratio/default that were successfully read, but restore
    // the built-in RPM list if no valid DISC_RPM entries were found.
    addDiscRPM(10.0f);
    addDiscRPM(15.0f);
    addDiscRPM(20.0f);
    addDiscRPM(25.0f);
    addDiscRPM(30.0f);
    addDiscRPM(35.0f);
    addDiscRPM(40.0f);
    addDiscRPM(45.0f);
  }

  Serial.print("PBB config loaded: gear ratio = ");
  Serial.println(planetaryRatio, 3);

  Serial.print("Disc RPM settings loaded: ");
  Serial.println(discRPMCount);

  Serial.print("Default disc RPM: ");
  Serial.println(defaultDiscRPM, 1);

  return true;
}

int findClosestDiscRPMIndex(float rpm) {
  if (discRPMCount == 0) return 0;

  int bestIndex = 0;
  float bestDifference = fabsf(discRPMPresets[0] - rpm);

  for (uint8_t i = 1; i < discRPMCount; i++) {
    float difference = fabsf(discRPMPresets[i] - rpm);

    if (difference < bestDifference) {
      bestDifference = difference;
      bestIndex = i;
    }
  }

  return bestIndex;
}

void printPBBConfig() {
  Serial.println("Available disc RPM settings:");

  for (uint8_t i = 0; i < discRPMCount; i++) {
    Serial.print("  ");
    Serial.print(i + 1);
    Serial.print(": Disc ");
    Serial.print(discRPMPresets[i], 1);
    Serial.print(" RPM -> Motor ");
    Serial.println(discRPMToMotorRPM(discRPMPresets[i]), 1);
  }
}


int currentPreset = 2;
volatile int currentMotorRPM = 104;

bool motorEnabled = false;            // safer startup: motor OFF
bool motorDirectionCCW = false;

constexpr uint8_t STEPPER_PWM_RESOLUTION = 8;
constexpr uint32_t STEPPER_PWM_DUTY = 128;

// ============================================================
// Touch button state
// ============================================================

const unsigned long TOUCH_LONG_MS = 1500;

bool lastTouched = false;
unsigned long touchDownMs = 0;

// 0 = none, 1 = motor, 2 = RPM, 3 = timer
uint8_t activeTouchButton = 0;
bool touchLongFired = false;

// ============================================================
// Button layout - known-good 480 x 320 landscape positions
// ============================================================

#define BTN_Y       220
#define BTN_H        90

#define MOTOR_X      10
#define MOTOR_W     145

#define RPM_X       167
#define RPM_W       145

#define TIMER_X     324
#define TIMER_W     145

// ============================================================
// Buzzer
// ============================================================

bool beepsEnabled = true;

uint16_t BEEP_FREQ_HZ = 2400;
uint16_t BEEP_ON_MS   = 80;
uint16_t BEEP_OFF_MS  = 90;

static uint8_t  beepRemaining = 0;
static bool     beepIsOn = false;
static uint32_t beepNextMs = 0;

static bool beep2F_armed = true;
static bool beep3F_armed = true;

// ============================================================
// Roast timer
// ============================================================

bool timerRunning = false;
unsigned long roastSeconds = 0;
unsigned long lastTickMs = 0;

unsigned long lastMinuteBeepSec = 0;
int lastTenMinBeepMin = 0;

// ============================================================
// RAM roast logging
// ============================================================

struct RoastSample {
  uint32_t elapsedSeconds;
  float tempF;
  float ambientTempF;
  float ambientHumidity;
  float discRPM;
  uint16_t motorRPM;
};

const int MAX_ROAST_SAMPLES = 241;  // 15-second samples for up to 60 minutes
RoastSample roastData[MAX_ROAST_SAMPLES];

int roastSampleCount = 0;
bool roastSaveArmed = false;
bool roastSaved = false;

String lastSaveStatus = "NO ROAST";
String roastStartDateTime = "";
String roastFilename = "";

// ============================================================
// Display timing
// ============================================================

const unsigned long DISPLAY_UPDATE_MS = 500;
unsigned long lastDisplayUpdateMs = 0;

// ============================================================
// General helpers
// ============================================================

float fahrenheitToCelsius(float f) {
  return (f - 32.0f) * 5.0f / 9.0f;
}

float applyTC1OffsetF(float f) {
  return f + TC1_OFFSET_F;
}

float readTC1_F() {
  double c = tc1.readCelsius();

  if (isnan(c)) {
    return NAN;
  }

  float f = (c * 9.0f / 5.0f) + 32.0f;
  return applyTC1OffsetF(f);
}

void deselectSD() {
  digitalWrite(SD_CS, HIGH);
}

// ============================================================
// FT6336 touch
// ============================================================

uint8_t readTouchRegister(uint8_t reg) {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return 0;
  }

  Wire.requestFrom(FT6336_ADDR, (uint8_t)1);

  if (Wire.available()) {
    return Wire.read();
  }

  return 0;
}

bool getTouch(int &screenX, int &screenY) {
  uint8_t touches = readTouchRegister(0x02) & 0x0F;

  if (touches == 0) {
    return false;
  }

  uint8_t xHigh = readTouchRegister(0x03);
  uint8_t xLow  = readTouchRegister(0x04);
  uint8_t yHigh = readTouchRegister(0x05);
  uint8_t yLow  = readTouchRegister(0x06);

  uint16_t rawX = ((xHigh & 0x0F) << 8) | xLow;
  uint16_t rawY = ((yHigh & 0x0F) << 8) | yLow;

  // Mapping verified in the working touch test.
  screenX = rawY;
  screenY = 319 - rawX;

  screenX = constrain(screenX, 0, 479);
  screenY = constrain(screenY, 0, 319);

  return true;
}

uint8_t buttonAt(int x, int y) {
  // COFFEE ORIGIN is above the three touch buttons, so test it first.
  if (x >= 10 && x <= 470 && y >= 136 && y <= 184) return 4;

  if (y < BTN_Y || y > BTN_Y + BTN_H) {
    return 0;
  }

  if (x >= MOTOR_X && x <= MOTOR_X + MOTOR_W) return 1;
  if (x >= RPM_X   && x <= RPM_X   + RPM_W)   return 2;
  if (x >= TIMER_X && x <= TIMER_X + TIMER_W) return 3;

  return 0;
}

// ============================================================
// ESP32 LEDC hardware-timed stepper
// ============================================================

uint32_t rpmToStepFrequency(float rpm) {
  const float stepsPerRev = FULL_STEPS_PER_REV * MICROSTEPS;
  const float stepsPerSecond = rpm * stepsPerRev / 60.0f;
  return static_cast<uint32_t>(stepsPerSecond + 0.5f);
}

void setMotorRPM(int rpm) {
  currentMotorRPM = rpm;

  const uint32_t requestedFrequency = rpmToStepFrequency(rpm);

  const uint32_t actualFrequency = ledcChangeFrequency(
    STEP_PIN,
    requestedFrequency,
    STEPPER_PWM_RESOLUTION
  );

  ledcWrite(STEP_PIN, STEPPER_PWM_DUTY);

  Serial.print("Motor RPM = ");
  Serial.print(rpm);
  Serial.print("  Requested STEP Hz = ");
  Serial.print(requestedFrequency);
  Serial.print("  Actual STEP Hz = ");
  Serial.println(actualFrequency);
}

bool setupStepperGenerator() {
  const uint32_t startingFrequency = rpmToStepFrequency(currentMotorRPM);

  const bool attached = ledcAttach(
    STEP_PIN,
    startingFrequency,
    STEPPER_PWM_RESOLUTION
  );

  if (!attached) {
    Serial.println("ERROR: Could not attach LEDC to STEP pin.");
    return false;
  }

  ledcWrite(STEP_PIN, STEPPER_PWM_DUTY);
  return true;
}

void setMotorEnabled(bool enabled) {
  motorEnabled = enabled;
  digitalWrite(EN_PIN, enabled ? LOW : HIGH); // active LOW

  Serial.println(enabled ? "MOTOR ON" : "MOTOR OFF");
}

// ============================================================
// Buzzer
// ============================================================

void stopBuzzerNow() {
  noTone(BUZZER_PIN);
  beepRemaining = 0;
  beepIsOn = false;
  beepNextMs = 0;
}

void startBeepPattern(uint8_t count) {
  if (!beepsEnabled) return;
  if (beepRemaining > 0) return;

  beepRemaining = count;
  beepIsOn = false;
  beepNextMs = 0;
}

void serviceBeepPattern() {
  if (beepRemaining == 0) return;

  uint32_t now = millis();

  if (beepNextMs != 0 && (int32_t)(now - beepNextMs) < 0) {
    return;
  }

  if (!beepIsOn) {
    tone(BUZZER_PIN, BEEP_FREQ_HZ);
    beepIsOn = true;
    beepNextMs = now + BEEP_ON_MS;
  } else {
    noTone(BUZZER_PIN);
    beepIsOn = false;
    beepRemaining--;
    beepNextMs = now + BEEP_OFF_MS;
  }
}

bool isBeepPatternActive() {
  return beepRemaining > 0;
}

void beepShort() {
  if (!beepsEnabled) return;
  tone(BUZZER_PIN, 2000, 80);
}

void beepLong() {
  if (!beepsEnabled) return;
  tone(BUZZER_PIN, 1500, 300);
}

void confirmBeep() {
  if (!beepsEnabled) return;
  tone(BUZZER_PIN, 1800, 200);
}

void checkTempAlertsF(float tempF) {
  if (beep2F_armed && tempF >= TEMP_BEEP_2F) {
    startBeepPattern(2);
    beep2F_armed = false;
  }

  if (beep3F_armed && tempF >= TEMP_BEEP_3F) {
    startBeepPattern(3);
    beep3F_armed = false;
  }

  if (!beep2F_armed && tempF <= (TEMP_BEEP_2F - TEMP_REARM_HYST_F)) {
    beep2F_armed = true;
  }

  if (!beep3F_armed && tempF <= (TEMP_BEEP_3F - TEMP_REARM_HYST_F)) {
    beep3F_armed = true;
  }
}

// ============================================================
// SD / roast file helpers - SdFat version
// ============================================================

String nextRoastFilename() {
  // Prefer an RTC timestamp so each roast can be identified without
  // having to remember which sequential number it was.
  if (rtcReady) {
    DateTime now = rtc.now();

    char filename[40];
    snprintf(filename, sizeof(filename),
             "/ROAST_%04d%02d%02d_%02d%02d%02d.CSV",
             now.year(), now.month(), now.day(),
             now.hour(), now.minute(), now.second());

    if (!sd.exists(filename)) {
      return String(filename);
    }

    // If the same timestamp is somehow reused, append a small suffix.
    for (int n = 1; n <= 99; n++) {
      snprintf(filename, sizeof(filename),
               "/ROAST_%04d%02d%02d_%02d%02d%02d_%02d.CSV",
               now.year(), now.month(), now.day(),
               now.hour(), now.minute(), now.second(), n);

      if (!sd.exists(filename)) {
        return String(filename);
      }
    }
  }

  // RTC fallback: retain the old sequential naming scheme if the RTC
  // is unavailable.
  char filename[24];
  for (int n = 1; n <= 9999; n++) {
    snprintf(filename, sizeof(filename), "/ROAST%03d.CSV", n);

    if (!sd.exists(filename)) {
      return String(filename);
    }
  }

  return "/ROAST9999.CSV";
}

void clearRoastData() {
  roastSampleCount = 0;
  roastSaveArmed = false;
  roastSaved = false;
  maxTempF = NAN;
  maxTempC = NAN;
  lastSaveStatus = "LOG EMPTY";
  roastStartDateTime = "";
  roastFilename = "";
}

void addRoastSample(uint32_t elapsedSeconds) {
  if (roastSampleCount >= MAX_ROAST_SAMPLES || isnan(lastTempF)) {
    return;
  }

  RoastSample &s = roastData[roastSampleCount];
  s.elapsedSeconds = elapsedSeconds;
  s.tempF = lastTempF;
  s.ambientTempF = ambientTempF;
  s.ambientHumidity = ambientHumidity;
  s.discRPM = currentDiscRPM;
  s.motorRPM = currentMotorRPM;
  roastSampleCount++;

  Serial.print("RAM log sample ");
  Serial.print(roastSampleCount);
  Serial.print(": ");
  Serial.print(elapsedSeconds);
  Serial.print(" sec, ");
  Serial.print(lastTempF, 1);
  Serial.print(" F, ");
  Serial.print(currentMotorRPM);
  Serial.println(" RPM");
}

bool saveRoastToSD() {
  if (roastSaved) {
    return true;
  }

  if (roastSampleCount == 0) {
    lastSaveStatus = "NO DATA";
    return false;
  }

  if (!sdReady) {
    lastSaveStatus = "SD ERROR";
    Serial.println("Cannot save roast: SD card not ready.");
    return false;
  }

  String filename = roastFilename;
  if (filename.length() == 0) {
    filename = nextRoastFilename();
  }

  // Keep TFT off the shared SPI bus during file activity.
  digitalWrite(TFT_CS, HIGH);

  if (!roastFile.open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC)) {
    lastSaveStatus = "SAVE FAIL";
    Serial.println("Could not open roast CSV for writing.");
    deselectSD();
    return false;
  }

  roastFile.print("CoffeeOrigin,");
  roastFile.println(coffeeOrigins[selectedOriginIndex]);
  roastFile.print("RoastStart,");
  roastFile.println(roastStartDateTime);
  roastFile.print("MaxTempF,");
  if (isnan(maxTempF)) roastFile.println("N/A");
  else roastFile.println(maxTempF, 1);
  roastFile.print("GearRatio,");
  roastFile.println(planetaryRatio, 3);
  roastFile.println("Elapsed,ElapsedSeconds,TempF,TempC,AmbientTempF,HumidityRH,DiscRPM,MotorRPM");

  for (int i = 0; i < roastSampleCount; i++) {
    uint32_t totalSec = roastData[i].elapsedSeconds;
    uint32_t mins = totalSec / 60;
    uint32_t secs = totalSec % 60;

    char elapsed[12];
    snprintf(elapsed, sizeof(elapsed), "%02lu:%02lu",
             (unsigned long)mins,
             (unsigned long)secs);

    float tempC = fahrenheitToCelsius(roastData[i].tempF);

    roastFile.print(elapsed);
    roastFile.print(',');
    roastFile.print(totalSec);
    roastFile.print(',');
    roastFile.print(roastData[i].tempF, 1);
    roastFile.print(',');
    roastFile.print(tempC, 1);
    roastFile.print(',');
    roastFile.print(roastData[i].ambientTempF, 1);
    roastFile.print(',');
    roastFile.print(roastData[i].ambientHumidity, 1);
    roastFile.print(',');
    roastFile.print(roastData[i].discRPM, 1);
    roastFile.print(',');
    roastFile.println(roastData[i].motorRPM);
  }

  roastFile.sync();
  roastFile.close();
  deselectSD();

  roastSaved = true;
  lastSaveStatus = filename;

  Serial.print("Roast saved to ");
  Serial.println(filename);

  return true;
}

void checkAutoSave() {
  if (timerRunning) return;

  if (!roastSaveArmed || roastSaved || roastSampleCount == 0 || isnan(lastTempF)) {
    return;
  }

  if (lastTempF <= ROAST_END_TEMP_F) {
    saveRoastToSD();
  }
}

// ============================================================
// Roast timer + minute logging
// ============================================================

void startStopTimer() {
  if (!timerRunning) {
    originSelectMode = false;

    if (roastSeconds == 0) {
      clearRoastData();
      if (!isnan(lastTempF)) {
        maxTempF = lastTempF;
        maxTempC = lastTempC;
      }
      addRoastSample(0);
    }

    timerRunning = true;
    lastTickMs = millis();
    roastStartDateTime = rtcDateTimeString();

    if (sdReady) {
      roastFilename = nextRoastFilename();
      Serial.print("Roast filename reserved: ");
      Serial.println(roastFilename);
    } else {
      roastFilename = "";
      Serial.println("Roast filename will use fallback if SD becomes available.");
    }

    Serial.println("Roast timer START");
  } else {
    timerRunning = false;
    Serial.println("Roast timer STOP");
  }
}

void clearTimerAndRoast() {
  if (roastSaveArmed && roastSampleCount > 0 && !roastSaved) {
    saveRoastToSD();
  }

  timerRunning = false;
  roastSeconds = 0;
  lastMinuteBeepSec = 0;
  lastTenMinBeepMin = 0;
  clearRoastData();

  confirmBeep();
  Serial.println("Timer/roast cleared");
}

void handleTimerTick() {
  if (!timerRunning) return;

  unsigned long now = millis();

  while (now - lastTickMs >= 1000) {
    lastTickMs += 1000;
    roastSeconds++;

    int mins = roastSeconds / 60;
    int secs = roastSeconds % 60;

    if ((roastSeconds % ROAST_LOG_INTERVAL_SEC) == 0) {
      addRoastSample(roastSeconds);
    }

    bool okToBeep = !isBeepPatternActive();

    if (secs == 0 && roastSeconds > 0) {
      if (mins > 0 && (mins % 10) == 0 && mins != lastTenMinBeepMin) {
        if (okToBeep) beepLong();
        lastTenMinBeepMin = mins;
        lastMinuteBeepSec = roastSeconds;
      } else if (roastSeconds != lastMinuteBeepSec) {
        if (okToBeep) beepShort();
        lastMinuteBeepSec = roastSeconds;
      }
    }
  }
}

// ============================================================
// TFT drawing
// ============================================================

void drawButtonSelectionBorder(int x, int y, int w, int h, bool selected) {
  if (selected) {
    // Strong daylight-visible selection cue:
    // fill the button yellow and use a dark outer border.
    tft.drawRoundRect(x, y, w, h, 10, TFT_BLACK);
    tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 9, TFT_BLACK);
  } else {
    tft.drawRoundRect(x, y, w, h, 10, TFT_WHITE);
  }
}

void drawMotorButton() {
  deselectSD();

  bool selected = (selectedControl == 0);
  uint16_t color = selected ? TFT_YELLOW :
                   (motorEnabled ? TFT_GREEN : TFT_DARKGREY);
  uint16_t textColor = selected ? TFT_BLACK : TFT_WHITE;

  tft.fillRoundRect(MOTOR_X, BTN_Y, MOTOR_W, BTN_H, 10, color);
  drawButtonSelectionBorder(MOTOR_X, BTN_Y, MOTOR_W, BTN_H, selected);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(textColor, color);
  tft.drawString("MOTOR", MOTOR_X + MOTOR_W / 2, BTN_Y + 25, 2);
  tft.drawString(motorEnabled ? "ON" : "OFF",
                 MOTOR_X + MOTOR_W / 2, BTN_Y + 60, 4);
}

void drawRPMButton() {
  deselectSD();

  bool selected = (selectedControl == 1);

  uint16_t color = selected ? TFT_YELLOW : TFT_BLUE;
  uint16_t textColor = selected ? TFT_BLACK : TFT_WHITE;

  tft.fillRoundRect(RPM_X, BTN_Y, RPM_W, BTN_H, 10, color);
  drawButtonSelectionBorder(RPM_X, BTN_Y, RPM_W, BTN_H, selected);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(textColor, color);

  tft.drawString("DISC RPM", RPM_X + RPM_W / 2, BTN_Y + 16, 2);

  char discText[20];
  snprintf(discText, sizeof(discText), "%.1f", currentDiscRPM);
  tft.drawString(discText, RPM_X + RPM_W / 2, BTN_Y + 43, 4);

  char motorText[24];
  snprintf(motorText, sizeof(motorText), "MOTOR %.0f RPM",
           (float)currentMotorRPM);
  tft.drawString(motorText, RPM_X + RPM_W / 2, BTN_Y + 75, 1);
}

void drawTimerButton() {
  deselectSD();

  bool selected = (selectedControl == 2);
  uint16_t color = selected ? TFT_YELLOW :
                   (timerRunning ? TFT_RED : TFT_DARKGREY);
  uint16_t textColor = selected ? TFT_BLACK : TFT_WHITE;

  tft.fillRoundRect(TIMER_X, BTN_Y, TIMER_W, BTN_H, 10, color);
  drawButtonSelectionBorder(TIMER_X, BTN_Y, TIMER_W, BTN_H, selected);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(textColor, color);
  tft.drawString("TIMER", TIMER_X + TIMER_W / 2, BTN_Y + 25, 2);
  tft.drawString(timerRunning ? "RUN" : "STOP",
                 TIMER_X + TIMER_W / 2, BTN_Y + 60, 4);
}

void drawCoffeeSelectionIndicator() {
  // Use the same strong yellow selection cue for the COFFEE control.
  // The entire control area is redrawn so no partial background remains.
  bool selected = (selectedControl == 3);

  if (selected) {
    tft.fillRoundRect(10, 136, 460, 48, 8, TFT_YELLOW);
    tft.drawRoundRect(10, 136, 460, 48, 8, TFT_BLACK);
    tft.drawRoundRect(11, 137, 458, 46, 7, TFT_BLACK);
  } else {
    tft.fillRoundRect(10, 136, 460, 48, 8, TFT_BLACK);
    tft.drawRoundRect(10, 136, 460, 48, 8, TFT_WHITE);
  }
}

void drawStaticScreen() {
  deselectSD();

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  char title[64];
  snprintf(title, sizeof(title), "%s %s", FW_NAME, FW_VERSION);

  tft.drawString(title, 240, 16, 2);
  //tft.drawString("PLANETARY BEAN BOUNCER v0.4.3", 240, 16, 2);

  // Roaster temperature
  tft.drawRoundRect(10, 36, 220, 92, 8, TFT_WHITE);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("ROASTER TEMP", 120, 51, 2);

  // Ambient conditions
  tft.drawRoundRect(240, 36, 230, 92, 8, TFT_WHITE);
  tft.drawString("AMBIENT", 355, 51, 2);

  // Origin
  tft.drawRoundRect(10, 136, 460, 48, 8, TFT_WHITE);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("COFFEE ORIGIN", 85, 150, 2);

  // Timer
  tft.drawRoundRect(120, 188, 240, 26, 8, TFT_WHITE);
  tft.drawString("ROAST TIMER", 165, 201, 2);

  drawMotorButton();
  drawRPMButton();
  drawTimerButton();
}

void updateTemperatureDisplay() {
  deselectSD();

  tft.fillRect(22, 66, 196, 55, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  if (isnan(lastTempF)) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("TC ERROR", 120, 94, 4);
    return;
  }

  char tempText[20];
  snprintf(tempText, sizeof(tempText), "%.1f F", lastTempF);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(tempText, 120, 80, 4);

  if (isnan(maxTempF)) {
    snprintf(tempText, sizeof(tempText), "MAX --.- F");
  } else {
    snprintf(tempText, sizeof(tempText), "MAX %.1f F", maxTempF);
  }
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString(tempText, 120, 108, 2);
}

void updateAmbientDisplay() {
  deselectSD();

  tft.fillRect(250, 66, 210, 55, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  if (!sht31Ready || isnan(ambientTempF) || isnan(ambientHumidity)) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("SHT31 ERROR", 355, 94, 2);
    return;
  }

  char text[32];

  // Show both ambient temperature units on the same line.
  float ambientTempC = fahrenheitToCelsius(ambientTempF);

  snprintf(text, sizeof(text), "%.1f F / %.1f C",
           ambientTempF, ambientTempC);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(text, 315, 80, 2);

  snprintf(text, sizeof(text), "%.0f%% RH", ambientHumidity);
  tft.drawString(text, 425, 80, 2);

  if (rtcReady) {
    DateTime now = rtc.now();

    char dateText[12];
    snprintf(dateText, sizeof(dateText), "%02d/%02d/%02d",
             now.month(), now.day(), now.year() % 100);

    char timeText[12];
    snprintf(timeText, sizeof(timeText), "%02d:%02d:%02d",
             now.hour(), now.minute(), now.second());

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(dateText, 305, 108, 2);
    tft.drawString(timeText, 405, 108, 2);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("RTC ERROR", 355, 108, 2);
  }
}


// ============================================================
// Dynamic coffee-origin font sizing
// ============================================================
// Use the largest TFT_eSPI font that fits the available width.
// This measures the actual rendered pixel width rather than
// relying only on the number of characters.
int coffeeOriginFontForWidth(const char* text, int maxWidth) {
  for (int font = 4; font >= 2; font--) {
    if (tft.textWidth(text, font) <= maxWidth) {
      return font;
    }
  }
  return 2;
}

void drawCenteredCoffeeOrigin(const char* text, int centerX,
                              int centerY, int maxWidth,
                              uint16_t color, uint16_t background) {
  int font = coffeeOriginFontForWidth(text, maxWidth);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, background);
  tft.drawString(text, centerX, centerY, font);
}

void updateOriginDisplay() {
  deselectSD();

  const char* origin = coffeeOrigins[
    originSelectMode ? pendingOriginIndex : selectedOriginIndex
  ];

  bool ec11Selected = (selectedControl == 3);

  // Keep the active coffee origin visible on the normal screen.
  // COFFEE is a display/control area rather than one of the three
  // large touch buttons, so use the existing indicator/border without
  // covering the origin text with yellow.
  drawCoffeeSelectionIndicator();

  uint16_t background = TFT_BLACK;
  uint16_t originColor = originSelectMode ? TFT_YELLOW :
                         (ec11Selected ? TFT_YELLOW : TFT_WHITE);

  drawCenteredCoffeeOrigin(origin, 287, 160, 350,
                           originColor, background);
}

void updateStatusDisplay() {
  deselectSD();

  tft.fillRect(15, 189, 100, 22, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  String line1;

  if (roastSaved) {
    line1 = "SAVED";
  } else if (roastSaveArmed) {
    line1 = "SAVE ARMED";
  } else if (timerRunning) {
    line1 = "ROASTING";
  } else {
    line1 = sdReady ? "SD READY" : "SD ERROR";
  }

  tft.setTextColor(roastSaved ? TFT_GREEN :
                   roastSaveArmed ? TFT_YELLOW :
                   timerRunning ? TFT_CYAN :
                   sdReady ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.drawString(line1, 65, 200, 2);
}

void updateTimerDisplay() {
  deselectSD();

  unsigned long mins = roastSeconds / 60;
  unsigned long secs = roastSeconds % 60;

  char timerText[16];
  snprintf(timerText, sizeof(timerText), "%02lu:%02lu", mins, secs);

  tft.fillRect(215, 188, 120, 27, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(timerRunning ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
  tft.drawString(timerText, 275, 201, 4);
}

void updateDisplay() {
  // Full-screen selectors own the display while active.
  if (originSelectMode) return;

  updateTemperatureDisplay();
  updateAmbientDisplay();
  updateOriginDisplay();
  updateStatusDisplay();
  updateTimerDisplay();
}

void drawOriginSelector() {
  deselectSD();

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("SELECT COFFEE ORIGIN", 240, 35, 3);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("TURN ENCODER TO SCROLL", 240, 75, 2);
  tft.drawString("PRESS TO SELECT", 240, 100, 2);

  tft.drawRoundRect(30, 125, 420, 90, 10, TFT_WHITE);
  drawCenteredCoffeeOrigin(coffeeOrigins[pendingOriginIndex],
                           240, 170, 380, TFT_YELLOW, TFT_BLACK);

  char countText[24];
  snprintf(countText, sizeof(countText), "%d / %d",
           pendingOriginIndex + 1, NUM_COFFEE_ORIGINS);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(countText, 240, 205, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("PRESS ENCODER TO CONFIRM", 240, 250, 2);
}

void enterOriginSelector() {
  if (timerRunning) return;

  pendingOriginIndex = selectedOriginIndex;
  originSelectMode = true;
  drawOriginSelector();
}

void moveOriginSelection(int direction) {
  if (timerRunning) return;

  if (!originSelectMode) {
    enterOriginSelector();
  }

  pendingOriginIndex += direction;

  if (pendingOriginIndex < 0) {
    pendingOriginIndex = NUM_COFFEE_ORIGINS - 1;
  }

  if (pendingOriginIndex >= NUM_COFFEE_ORIGINS) {
    pendingOriginIndex = 0;
  }

  drawOriginSelector();
}

void confirmOriginSelection() {
  if (!originSelectMode) {
    enterOriginSelector();
    return;
  }

  selectedOriginIndex = pendingOriginIndex;
  originSelectMode = false;

  saveSelectedOrigin();
  confirmBeep();

  Serial.print("Coffee origin selected: ");
  Serial.println(coffeeOrigins[selectedOriginIndex]);

  drawStaticScreen();
  updateDisplay();
}

void moveControlSelection(int direction) {
  if (timerRunning) return;

  int next = (int)selectedControl + direction;

  if (next < 0) next = 3;
  if (next > 3) next = 0;

  selectedControl = (uint8_t)next;

  Serial.print("EC11 selection: ");
  if (selectedControl == 0) Serial.println("MOTOR");
  else if (selectedControl == 1) Serial.println("DISC RPM");
  else if (selectedControl == 2) Serial.println("TIMER");
  else Serial.println("COFFEE");

  drawMotorButton();
  drawRPMButton();
  drawTimerButton();
  updateOriginDisplay();
}

void encoderSelectAction() {
  if (timerRunning) return;

  switch (selectedControl) {
    case 0:
      motorShortPress();
      break;

    case 1:
      rpmShortPress();
      break;

    case 2:
      timerShortPress();
      break;

    case 3:
      enterOriginSelector();
      break;
  }
}

void serviceEncoder() {
  int a = digitalRead(ENC_A);
  int b = digitalRead(ENC_B);
  int encoded = (a << 1) | b;

  int transitionIndex = (lastEncoderState << 2) | encoded;
  int movement = encoderTransitionTable[transitionIndex];

  if (timerRunning) {
    encoderQuarterSteps = 0;
  }

  if (movement != 0 && !timerRunning) {
    encoderQuarterSteps += movement;

    if (encoderQuarterSteps >= ENC_COUNTS_PER_DETENT) {
      encoderQuarterSteps = 0;

      if (originSelectMode) {
        moveOriginSelection(1);
      } else {
        moveControlSelection(1);
      }
    } else if (encoderQuarterSteps <= -ENC_COUNTS_PER_DETENT) {
      encoderQuarterSteps = 0;

      if (originSelectMode) {
        moveOriginSelection(-1);
      } else {
        moveControlSelection(-1);
      }
    }
  }

  lastEncoderState = encoded;

  bool buttonState = digitalRead(ENC_SW);
  unsigned long now = millis();

  if (buttonState != lastEncoderButton &&
      now - lastEncoderButtonMs >= ENC_BUTTON_DEBOUNCE_MS) {

    lastEncoderButtonMs = now;
    lastEncoderButton = buttonState;

    if (buttonState == LOW) {
      if (originSelectMode) {
        confirmOriginSelection();
      } else {
        encoderSelectAction();
      }
    }
  }
}

// ============================================================
// Touch actions
// ============================================================

void redrawTouchButtons() {
  // Touch actions can change selectedControl. Redraw all three buttons so
  // the previous yellow selection is cleared immediately.
  drawMotorButton();
  drawRPMButton();
  drawTimerButton();
}

void motorShortPress() {
  selectedControl = 0;
  setMotorEnabled(!motorEnabled);
  redrawTouchButtons();
}

void setDiscRPM(float discRPM) {
  currentDiscRPM = discRPM;

  float motorRPM = discRPMToMotorRPM(discRPM);
  int roundedMotorRPM = (int)lroundf(motorRPM);

  setMotorRPM(roundedMotorRPM);

  Serial.print("Disc RPM = ");
  Serial.print(currentDiscRPM, 1);
  Serial.print("  Motor RPM = ");
  Serial.println(currentMotorRPM);
}

void rpmShortPress() {
  selectedControl = 1;

  currentPreset++;
  if (currentPreset >= discRPMCount) currentPreset = 0;

  setDiscRPM(discRPMPresets[currentPreset]);
  redrawTouchButtons();
}

void rpmLongPress() {
  selectedControl = 1;
  bool wasEnabled = beepsEnabled;
  beepsEnabled = !beepsEnabled;

  if (!beepsEnabled) {
    if (wasEnabled) tone(BUZZER_PIN, 1200, 300);
    beepRemaining = 0;
    beepIsOn = false;
    beepNextMs = 0;
  } else {
    tone(BUZZER_PIN, 2000, 120);
  }

  Serial.println(beepsEnabled ? "BUZZER ON" : "BUZZER MUTED");
  redrawTouchButtons();
  updateStatusDisplay();
}

void timerShortPress() {
  selectedControl = 2;
  startStopTimer();
  redrawTouchButtons();
  updateTimerDisplay();
  updateStatusDisplay();
}

void timerLongPress() {
  selectedControl = 2;
  clearTimerAndRoast();
  redrawTouchButtons();
  updateTimerDisplay();
  updateStatusDisplay();
}

// ============================================================
// Touch confirmation / debounce
// ============================================================
// A single FT6336 touch report is not enough to activate a
// control. The same button must be reported for several
// consecutive reads and remain there for a minimum time.
//
// This is intentionally conservative because MOTOR and RPM
// are machine-control functions.
const uint8_t TOUCH_CONFIRM_COUNT = 3;
const unsigned long TOUCH_CONFIRM_MS = 35;

uint8_t touchCandidateButton = 0;
uint8_t touchCandidateCount = 0;
unsigned long touchCandidateSince = 0;

void serviceTouch() {
  int x = 0;
  int y = 0;
  bool touched = getTouch(x, y);
  unsigned long now = millis();

  // ----------------------------------------------------------
  // No touch
  // ----------------------------------------------------------
  if (!touched) {
    // If a confirmed touch was released, perform the short press.
    if (lastTouched) {
      if (!touchLongFired) {
        if (activeTouchButton == 1) motorShortPress();
        if (activeTouchButton == 2) rpmShortPress();
        if (activeTouchButton == 3) timerShortPress();
        if (activeTouchButton == 4) {
          selectedControl = 3;
          enterOriginSelector();
        }
      }

      activeTouchButton = 0;
      touchLongFired = false;
    }

    lastTouched = false;

    // Clear an unconfirmed/partial touch.
    touchCandidateButton = 0;
    touchCandidateCount = 0;
    touchCandidateSince = 0;

    return;
  }

  uint8_t currentButton = buttonAt(x, y);

  // Ignore touches outside the three control buttons.
  if (currentButton == 0) {
    if (!lastTouched) {
      touchCandidateButton = 0;
      touchCandidateCount = 0;
      touchCandidateSince = 0;
    }
    return;
  }

  // ----------------------------------------------------------
  // New candidate touch
  // ----------------------------------------------------------
  if (!lastTouched) {
    if (currentButton != touchCandidateButton) {
      touchCandidateButton = currentButton;
      touchCandidateCount = 1;
      touchCandidateSince = now;
      return;
    }

    if (touchCandidateCount < TOUCH_CONFIRM_COUNT) {
      touchCandidateCount++;
    }

    // Do not accept until both requirements are met.
    if (touchCandidateCount >= TOUCH_CONFIRM_COUNT &&
        now - touchCandidateSince >= TOUCH_CONFIRM_MS) {

      activeTouchButton = currentButton;
      touchDownMs = now;
      touchLongFired = false;
      lastTouched = true;

      Serial.print("TOUCH ACCEPTED: button=");
      if (currentButton == 1) Serial.println("MOTOR");
      else if (currentButton == 2) Serial.println("RPM");
      else if (currentButton == 3) Serial.println("TIMER");
      else if (currentButton == 4) Serial.println("COFFEE");

      return;
    }

    return;
  }

  // ----------------------------------------------------------
  // Already have a confirmed touch
  // ----------------------------------------------------------
  if (currentButton != activeTouchButton) {
    // Finger moved off the original button. Do not interpret
    // the new location as another button press.
    return;
  }

  // Held touch: long functions only on RPM and TIMER.
  if (!touchLongFired &&
      (activeTouchButton == 2 || activeTouchButton == 3) &&
      now - touchDownMs >= TOUCH_LONG_MS) {

    touchLongFired = true;

    if (activeTouchButton == 2) rpmLongPress();
    if (activeTouchButton == 3) timerLongPress();
  }
}

// ============================================================
// Setup
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println(FW_NAME);
  Serial.println(FW_VERSION);
  Serial.println("----------------------------");
  Serial.println("Touch confirmation: 3 matching reads / 35 ms");
  Serial.println("Roast log interval: 15 seconds");
  Serial.println("Temperature alerts: 300F / 350F");

  // GPIO
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(DIR_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);

  motorDirectionCCW = false;
  digitalWrite(DIR_PIN, HIGH);
  digitalWrite(EN_PIN, HIGH); // disabled at startup
  motorEnabled = false;

  pinMode(TFT_CS, OUTPUT);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(SD_CS, HIGH);

  // TFT - explicit hardware reset before initialization.
  // This helps avoid the occasional white-screen first boot seen with
  // the ST7796 when its power-up reset timing does not line up with ESP32.
  Serial.println("Resetting TFT...");
  pinMode(PBB_TFT_RST, OUTPUT);
  digitalWrite(PBB_TFT_RST, LOW);
  delay(250);
  digitalWrite(PBB_TFT_RST, HIGH);

  // Give the TFT controller plenty of time to come out of reset
  // before sending the initialization sequence.
  delay(1000);

  Serial.println("Initializing TFT...");
  tft.init();

  // Give the display a moment to settle after initialization.
  delay(250);
  tft.setRotation(1);
  Serial.println("TFT PASSED");

  // Draw an early startup screen immediately after TFT initialization.
  // This confirms the display is alive before the SD card/configuration
  // loading begins. The final startup banner is drawn again later.
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("PLANETARY", 240, 92, 4);
  tft.drawString("BEAN BOUNCER", 240, 137, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(FW_VERSION, 240, 185, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("INITIALIZING...", 240, 245, 2);
  delay(100);

  // Touch - same initialization as working touch test
  Serial.println("Initializing touch...");
  pinMode(CTP_RST, OUTPUT);
  digitalWrite(CTP_RST, LOW);
  delay(20);
  digitalWrite(CTP_RST, HIGH);
  delay(300);

  pinMode(CTP_INT, INPUT);
  Wire.begin(CTP_SDA, CTP_SCL);
  Wire.setClock(100000);
  Serial.println("TOUCH PASSED");

  // ----------------------------------------------------------
  // DS3231 + SHT31 on second I2C bus
  // ----------------------------------------------------------
  Serial.println("Initializing RTC + SHT31...");
  SensorWire.begin(SENSOR_SDA, SENSOR_SCL, 100000);

  rtcReady = rtc.begin(&SensorWire);

  if (rtcReady) {
    Serial.println("DS3231 PASSED");

    if (rtc.lostPower()) {
      Serial.println("WARNING: RTC reports lost power");
    }

    Serial.print("RTC: ");
    Serial.println(rtcDateTimeString());
  } else {
    Serial.println("DS3231 FAILED");
  }

  // Verify SHT31 by sending its measurement command.
  float shtTempC;
  float shtHumidity;

  sht31Ready = readSHT31(shtTempC, shtHumidity);

  if (sht31Ready) {
    ambientTempF = shtTempC * 9.0f / 5.0f + 32.0f;
    ambientHumidity = shtHumidity;
    Serial.println("SHT31 PASSED");
  } else {
    Serial.println("SHT31 FAILED");
  }

  // Load the last-selected coffee origin before starting the encoder.
  loadSavedOrigin();

  // ----------------------------------------------------------
  // EC11
  // ----------------------------------------------------------
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);

  lastEncoderState = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  lastEncoderButton = digitalRead(ENC_SW);

  Serial.println("EC11 READY");

  // SdFat using the exact TFT_eSPI SPI instance from the working test
  SPIClass* sharedSPI = &TFT_eSPI::getSPIinstance();

  Serial.println("Initializing SD...");

  SdSpiConfig sdConfig(
    SD_CS,
    SHARED_SPI,
    SD_SCK_MHZ(4),
    sharedSPI
  );

  sdReady = sd.begin(sdConfig);

  // Update the already-initialized TFT while SD/configuration work continues.
  tft.fillRect(80, 225, 320, 32, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("INITIALIZING SD...", 240, 245, 2);

  if (sdReady) {
    Serial.println("SD PASSED");
    Serial.print("Card size: ");
    Serial.print(sd.card()->sectorCount() / 2048);
    Serial.println(" MB");
    lastSaveStatus = "SD READY";
  } else {
    Serial.println("SD FAILED");
    sd.initErrorPrint(&Serial);
    lastSaveStatus = "SD ERROR";
  }

  deselectSD();

  // Load the gear ratio and disc-RPM choices from PBB_CONFIG.TXT.
  // The firmware calculates motor RPM from the requested disc RPM.
  tft.fillRect(60, 225, 360, 32, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("LOADING PBB_CONFIG.TXT...", 240, 245, 2);
  loadPBBConfigFromSD();
  currentPreset = findClosestDiscRPMIndex(defaultDiscRPM);
  currentDiscRPM = discRPMPresets[currentPreset];
  currentMotorRPM = (int)lroundf(discRPMToMotorRPM(currentDiscRPM));

  printPBBConfig();

  // Stepper generator
  if (!setupStepperGenerator()) {
    Serial.println("FATAL: stepper LEDC setup failed.");
    while (true) delay(1000);
  }

  setMotorRPM(currentMotorRPM);
  setMotorEnabled(false);

  // MAX6675
  delay(250);
  lastTempF = readTC1_F();
  if (!isnan(lastTempF)) {
    lastTempC = fahrenheitToCelsius(lastTempF);
  }
  maxTempF = NAN;
  maxTempC = NAN;

  // Startup banner
  deselectSD();
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("PLANETARY", 240, 105, 4);
  tft.drawString("BEAN BOUNCER", 240, 150, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(FW_VERSION, 240, 205, 2);
  delay(1500);

  drawStaticScreen();
  updateDisplay();

  Serial.println("Roaster controller ready");
}

// ============================================================
// Main loop
// ============================================================

void loop() {
  serviceTouch();
  serviceEncoder();

  unsigned long now = millis();

  // RTC + SHT31 sample
  if (now - lastSensorSampleMs >= SENSOR_SAMPLE_MS) {
    lastSensorSampleMs = now;
    updateEnvironmentalSensors();
  }

  // Temperature sample
  if (now - lastTempSampleMs >= TEMP_SAMPLE_MS) {
    lastTempSampleMs = now;

    lastTempF = readTC1_F();

    if (!isnan(lastTempF)) {
      lastTempC = fahrenheitToCelsius(lastTempF);

      if (timerRunning && (isnan(maxTempF) || lastTempF > maxTempF)) {
        maxTempF = lastTempF;
        maxTempC = lastTempC;
      }

      if (timerRunning && lastTempF >= SAVE_ARM_TEMP_F) {
        roastSaveArmed = true;
      }

      checkTempAlertsF(lastTempF);
    } else {
      lastTempC = NAN;
    }
  }

  serviceBeepPattern();
  handleTimerTick();
  checkAutoSave();

  if (now - lastDisplayUpdateMs >= DISPLAY_UPDATE_MS) {
    lastDisplayUpdateMs = now;
    updateDisplay();
  }
}
