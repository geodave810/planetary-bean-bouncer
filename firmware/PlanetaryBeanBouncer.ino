/*
 * Planetary Bean Bouncer Coffee Roaster Controller Firmware
 *
 * Author: David Bunch
 * Date: 01/08/2026
 *
 * License: MIT
 * Portions of this code were developed with the assistance of AI-based
 * code generation tools and subsequently reviewed, tested, and modified
 * by the author.
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <max6675.h>   // Thermocouple module (MAX6675)

// ==============================
// LCD setup
// ==============================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ==============================
// Stepper / TMC2209 (X slot on CNC shield)
// ==============================
#define STEP_PIN 2
#define DIR_PIN  5
#define EN_PIN   8

#define FULL_STEPS_PER_REV 400.0 // 0.9 Degree Motor
#define MICROSTEPS         8.0   // 1/8 step

#define SHOW_STARTUP_BANNER 1
#define BANNER_TIME_MS 3000

bool showingBanner = SHOW_STARTUP_BANNER;
unsigned long bannerStartTime = 0;
// ==============================
// Buttons (active LOW, INPUT_PULLUP)
// ==============================
#define BTN_MOTOR    10   // CW/CCW toggle + long-press Buzzer Mute
#define BTN_PROFILE  11   // Change to different RPM range + long-press (LCD reset)
#define BTN_TIMER    12   // start/stop + long-press reset timer to 0
#define BTN_RPM      13   // Change to different RPM in current set array + long-press to toggle F/C

// ==============================
// Single status LED + buzzer
// ==============================
#define LED_STATUS   7
#define BUZZER_PIN   9    // active buzzer on D9 (tone() is non-blocking)

// ==============================
// Thermocouple #1 pins (use A0/A1/A2 as DIGITAL pins)
// SCK/CLK = output, CS = output, SO/DO = input
// ==============================
#define TC1_SCK  A0
#define TC1_CS   A1
#define TC1_SO   A2

MAX6675 tc1(TC1_SCK, TC1_CS, TC1_SO);

// ==============================
// Thermocouple #2 pins (FUTURE, commented out)
// ==============================
// #define TC2_SCK  3
// #define TC2_CS   4
// #define TC2_SO   6
// MAX6675 tc2(TC2_SCK, TC2_CS, TC2_SO);

// ==============================
// TEMP ALERTS (Fahrenheit thresholds)
// ==============================
float TEMP_BEEP_2F = 250.0;   // double beep
float TEMP_BEEP_3F = 275.0;   // triple beep
float TEMP_REARM_HYST_F = 5.0;  // must cool this much below threshold to re-arm

unsigned long lastTempSampleMs = 0;
const unsigned long TEMP_SAMPLE_MS = 200; // 5 Hz (adjust 100–500ms)
float lastTempF = NAN;

// ==============================
// Buzzer pattern timing
// ==============================
uint16_t BEEP_FREQ_HZ = 2400;   // tone frequency
uint16_t BEEP_ON_MS   = 80;
uint16_t BEEP_OFF_MS  = 90;

// Set to 1 if you want tone(), 0 if you just switch an "active buzzer" HIGH/LOW
#define USE_TONE 1

// ==============================
// RPM Profiles
// ==============================
#define NUM_PROFILES 4
#define NUM_PRESETS  6
//
// Speed tops out at 240rpm for me, so I set that at maximum
// Profile P0 is for 1:1 ratio which starts at 15 rpm
// Profile P1 is for 4:1 ratio which starts with 60 rpm and 15 rpm at the wobble disc
// Profile P2 is for 5:1 ratio which starts with 75 rpm and 15 rpm at the wobble disc (This is what I am using)
int profiles[NUM_PROFILES][NUM_PRESETS] = {
  { 15,  20,  30,  40,  50,  60 },
  { 60,  80, 120, 160, 200, 240 },
  { 75, 100, 150, 200, 220, 240 },
  { 90,  120, 180, 200, 220, 240 }
};
int currentProfile = 2;  // Range 0 - 3 since NUM_PROFILES = 4
int currentPreset  = 2;  // Range 0 - 5 since NUM_PRESETS = 6

bool tempUnitsF = true;  // true=F, false=C

// RPM long-press
bool rpmBtnPrev = false;
bool rpmLongFired = false;
unsigned long rpmHoldStartMs = 0;
const unsigned long RPM_LONG_MS = 1200;  // hold to toggle F/C

// PROFILE long-press (LCD reset)
bool profileBtnPrev = false;
bool profileLongFired = false;
unsigned long profileHoldStartMs = 0;
const unsigned long PROFILE_LONG_MS = 1500;

// ==============================
// Motor state / step timing
// ==============================
bool motorEnabled = true;           // driver enabled
bool motorDirectionCCW = false;     // true=CCW (DIR LOW), false=CW (DIR HIGH)

bool beepsEnabled = true;

// Motor button long-press (Beep On/Off for possible quiet mode)
bool motorBtnPrev = false;
bool motorLongFired = false;
unsigned long motorHoldStartMs = 0;
const unsigned long MOTOR_LONG_MS = 1500;  // 1.5s

unsigned long stepDelayMicros = 0;
unsigned long lastStepMicros  = 0;

// ==============================
// Timer state
// ==============================
bool timerRunning          = false;
unsigned long roastSeconds = 0;
unsigned long lastTickMs   = 0;

// Beep tracking (minute / 10-min beeps)
unsigned long lastMinuteBeepSec = 0;
int           lastTenMinBeepMin = 0;

// Timer button long-press
bool timerBtnPrev              = false;
bool timerLongFired            = false;
unsigned long timerHoldStartMs = 0;
const unsigned long TIMER_LONG_MS = 1500;  // hold to reset

// Other button edge tracking
bool lastMotorPressed   = false;

const unsigned long DEBOUNCE_MS = 35;  // try 35–60ms if needed

struct DebounceBtn {
  uint8_t pin;
  bool stable;          // debounced state: true = pressed
  bool lastRaw;
  unsigned long lastChangeMs;
};

DebounceBtn dbMotor   = { BTN_MOTOR,   false, false, 0 };
DebounceBtn dbProfile = { BTN_PROFILE, false, false, 0 };
DebounceBtn dbTimer   = { BTN_TIMER,   false, false, 0 };
DebounceBtn dbRpm     = { BTN_RPM,     false, false, 0 };

bool readPressedRaw(uint8_t pin) {
  return digitalRead(pin) == LOW;   // active LOW buttons
}

bool debounceRead(DebounceBtn &b) {
  bool raw = readPressedRaw(b.pin);

  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.lastChangeMs = millis();
  }

  if ((millis() - b.lastChangeMs) >= DEBOUNCE_MS) {
    b.stable = raw;
  }

  return b.stable; // true = pressed
}

// ==============================
// BEEP PATTERN (non-blocking)
// ==============================
static uint8_t  beepRemaining = 0;
static bool     beepIsOn = false;
static uint32_t beepNextMs = 0;

void startBeepPattern(uint8_t count) {
  // Don’t interrupt an in-progress pattern
  if (!beepsEnabled) return;
  if (beepRemaining > 0) return;
  beepRemaining = count;
  beepIsOn = false;
  beepNextMs = 0;
}

void serviceBeepPattern(uint8_t buzzerPin) {
  if (beepRemaining == 0) return;

  uint32_t now = millis();
  if (beepNextMs != 0 && (int32_t)(now - beepNextMs) < 0) return;

  if (!beepIsOn) {
#if USE_TONE
    tone(buzzerPin, BEEP_FREQ_HZ);
#else
    digitalWrite(buzzerPin, HIGH);
#endif
    beepIsOn = true;
    beepNextMs = now + BEEP_ON_MS;
  } else {
#if USE_TONE
    noTone(buzzerPin);
#else
    digitalWrite(buzzerPin, LOW);
#endif
    beepIsOn = false;
    beepRemaining--;
    beepNextMs = now + BEEP_OFF_MS;
  }
}

// Threshold one-shot logic
static bool beep2F_armed = true;
static bool beep3F_armed = true;

// tempF must be Fahrenheit here
void checkTempAlertsF(float tempF, uint8_t buzzerPin) {
  if (beep2F_armed && tempF >= TEMP_BEEP_2F) {
    startBeepPattern(2); // double beep
    beep2F_armed = false;
  }
  if (beep3F_armed && tempF >= TEMP_BEEP_3F) {
    startBeepPattern(3); // triple beep
    beep3F_armed = false;
  }

  // Re-arm only after cooling below threshold (hysteresis)
  if (!beep2F_armed && tempF <= (TEMP_BEEP_2F - TEMP_REARM_HYST_F)) beep2F_armed = true;
  if (!beep3F_armed && tempF <= (TEMP_BEEP_3F - TEMP_REARM_HYST_F)) beep3F_armed = true;

  serviceBeepPattern(buzzerPin);
}

bool isBeepPatternActive() {
  return beepRemaining > 0;
}

// ==============================
// Helpers
// ==============================
unsigned long rpmToDelayMicros(int rpm) {
  float stepsPerRev    = FULL_STEPS_PER_REV * MICROSTEPS; // 400 * 8 = 3200
  float stepsPerSecond = (rpm * stepsPerRev) / 60.0;
  return (unsigned long)(1000000.0 / stepsPerSecond);
}

// ==============================
// Buzzer helpers (tone duration is non-blocking)
// ==============================
// 80ms
void beepShort() {
  if (!beepsEnabled) return;
  tone(BUZZER_PIN, 2000, 80);
}
// 300ms
void beepLong() {
  if (!beepsEnabled) return;
  tone(BUZZER_PIN, 1500, 300);
}
// 200ms
void confirmBeep() {
  if (!beepsEnabled) return;
  tone(BUZZER_PIN, 1800, 200);
}

// ==============================
// Thermocouple reading
// ==============================
// Read TC1 in Fahrenheit (for alerts). Returns NAN if not valid.
float readTC1_F() {
  double c = tc1.readCelsius();
  if (isnan(c)) return NAN;
  return (float)((c * 9.0 / 5.0) + 32.0);
}

// Read TC1 in current display units for LCD (int). -999 = invalid
int readTC1_displayUnits() {
  double c = tc1.readCelsius();
  if (isnan(c)) return -999;

  if (tempUnitsF) {
    double f = (c * 9.0 / 5.0) + 32.0;
    return (int)(f + 0.5);
  } else {
    return (int)(c + 0.5);
  }
}

// ==============================
// LCD update (NO lcd.clear(); overwrite full lines)
// Line 1: P# R### Tmm:ss RUN/STOP
// Line 2: TC1:###F CCW/CW
// ==============================
void updateLCD() {
  char line0[17];
  char line1[17];

  int rpm  = profiles[currentProfile][currentPreset];
  int mins = roastSeconds / 60;
  int secs = roastSeconds % 60;

  // Line 0
  snprintf(line0, sizeof(line0), "P%d R%d T%02d:%02d %s",
           currentProfile, rpm, mins, secs, timerRunning ? "RUN" : "STP");
  int len0 = strlen(line0);
  for (int i = len0; i < 16; i++) line0[i] = ' ';
  line0[16] = '\0';

  // Line 1
  int t = readTC1_displayUnits();
  char unitChar = tempUnitsF ? 'F' : 'C';

const char* dirStr = motorDirectionCCW ? "CCW" : "CW ";
const char  muteChar = beepsEnabled ? ' ' : 'M';  // M = muted

if (t == -999) {
  snprintf(line1, sizeof(line1),
           "TC1:---%c %s%c",
           unitChar, dirStr, muteChar);
} else {
  snprintf(line1, sizeof(line1),
           "TC1:%3d%c %s%c",
           t, unitChar, dirStr, muteChar);
}


  int len1 = strlen(line1);
  for (int i = len1; i < 16; i++) line1[i] = ' ';
  line1[16] = '\0';

  lcd.setCursor(0, 0);
  lcd.print(line0);
  lcd.setCursor(0, 1);
  lcd.print(line1);
}

// ==============================
// LCD soft reset
// ==============================
void resetLCDSoft() {
  Wire.begin();
  Wire.setClock(100000);

  delay(20);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  delay(10);
  updateLCD();
  confirmBeep();
}

// ==============================
// Setup
// ==============================
void setup() {
  lcd.init();
  lcd.backlight();

  updateLCD();

  #if SHOW_STARTUP_BANNER
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Planetary Bean");
    lcd.setCursor(0, 1);
    lcd.print("Bouncer v0.1.0");

    bannerStartTime = millis();
  #endif

  // Stepper pins
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN,  OUTPUT);
  pinMode(EN_PIN,   OUTPUT);

  // Default direction
  motorDirectionCCW = false;
  digitalWrite(DIR_PIN, LOW);

  // Enable driver (LOW active on CNC shield)
  motorEnabled = true;
  digitalWrite(EN_PIN, motorEnabled ? LOW : HIGH);

  // Buttons
  pinMode(BTN_MOTOR,   INPUT_PULLUP);
  pinMode(BTN_RPM,     INPUT_PULLUP);
  pinMode(BTN_PROFILE, INPUT_PULLUP);
  pinMode(BTN_TIMER,   INPUT_PULLUP);

  // Status LED + buzzer
  pinMode(LED_STATUS, OUTPUT);
  digitalWrite(LED_STATUS, LOW);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Initial RPM
  int startRPM = profiles[currentProfile][currentPreset];
  stepDelayMicros = rpmToDelayMicros(startRPM);

  // MAX6675 needs a brief settling time
  delay(250);

}

// ==============================
// Buttons
// ==============================

// Motor button: toggle CW/CCW
void handleMotorButton(bool pressed) {
  unsigned long now = millis();

  // Just pressed
  if (pressed && !motorBtnPrev) {
    motorHoldStartMs = now;
    motorLongFired = false;
  }

  // Long press → toggle beeps ON/OFF
  if (pressed && !motorLongFired && (now - motorHoldStartMs) >= MOTOR_LONG_MS) {
    beepsEnabled = !beepsEnabled;
    motorLongFired = true;

    // Feedback
    if (beepsEnabled) {
      tone(BUZZER_PIN, 2000, 120);   // short confirm beep
    } else {
      tone(BUZZER_PIN, 1200, 300);   // lower / longer = "muted"
    }

    updateLCD();   // <-- add this so M updates immediately
  }

  // Short press → toggle CW/CCW
  if (!pressed && motorBtnPrev) {
    if (!motorLongFired) {
      motorDirectionCCW = !motorDirectionCCW;
      digitalWrite(DIR_PIN, motorDirectionCCW ? LOW : HIGH);
      updateLCD();
    }
  }

  motorBtnPrev = pressed;
}


// RPM button:
//  - Short press: Change RPM in current Profile
//  - Long press: Toggle F/C
void handleRpmButton(bool pressed) {
  unsigned long now = millis();

  // Just pressed
  if (pressed && !rpmBtnPrev) {
    rpmHoldStartMs = now;
    rpmLongFired = false;
  }

  // Held long enough -> toggle F/C (fires once)
  if (pressed && !rpmLongFired && (now - rpmHoldStartMs) >= RPM_LONG_MS) {
    tempUnitsF = !tempUnitsF;
    rpmLongFired = true;
    confirmBeep();
    updateLCD();
  }

  // Just released
  if (!pressed && rpmBtnPrev) {
    if (!rpmLongFired) {
      currentPreset++;
      if (currentPreset >= NUM_PRESETS) currentPreset = 0;

      int rpm = profiles[currentProfile][currentPreset];
      stepDelayMicros = rpmToDelayMicros(rpm);
      updateLCD();
    }
  }

  rpmBtnPrev = pressed;
}

// Profile button: cycle profile, long press LCD reset
void handleProfileButton(bool pressed) {
  unsigned long now = millis();

  // Just pressed
  if (pressed && !profileBtnPrev) {
    profileHoldStartMs = now;
    profileLongFired = false;
  }

  // Held long enough -> LCD soft reset
  if (pressed && !profileLongFired && (now - profileHoldStartMs) >= PROFILE_LONG_MS) {
    profileLongFired = true;
    resetLCDSoft();
  }

  // Just released
  if (!pressed && profileBtnPrev) {
    if (!profileLongFired) {
      currentProfile++;
      if (currentProfile >= NUM_PROFILES) currentProfile = 0;

      int rpm = profiles[currentProfile][currentPreset];
      stepDelayMicros = rpmToDelayMicros(rpm);
      updateLCD();
    }
  }

  profileBtnPrev = pressed;
}

// Timer button:
//  - Short press: toggle start/stop
//  - Long press: reset + confirm beep
void handleTimerButton(bool pressed) {
  unsigned long now = millis();

  // Just pressed
  if (pressed && !timerBtnPrev) {
    timerHoldStartMs = now;
    timerLongFired = false;
  }

  // Long press reset
  if (pressed && !timerLongFired && (now - timerHoldStartMs) >= TIMER_LONG_MS) {
    timerRunning = false;
    roastSeconds = 0;
    lastMinuteBeepSec = 0;
    lastTenMinBeepMin = 0;
    timerLongFired = true;
    confirmBeep();
    updateLCD();
  }

  // Just released
  if (!pressed && timerBtnPrev) {
    if (!timerLongFired) {
      timerRunning = !timerRunning;
      if (timerRunning) {
        lastTickMs = millis();
        lastMinuteBeepSec = 0;
        lastTenMinBeepMin = 0;
      }
      updateLCD();
    }
  }

  timerBtnPrev = pressed;
}

// ==============================
// Timer tick + beeps + status LED
// ==============================
void handleTimerTickAndStatusLED() {
  static bool ledState = false;

  if (!timerRunning) {
    if (ledState) {
      ledState = false;
      digitalWrite(LED_STATUS, LOW);
    }
    return;
  }

  unsigned long now = millis();

  if (now - lastTickMs >= 1000) {
    lastTickMs += 1000;
    roastSeconds++;

    // Blink LED each second
    ledState = !ledState;
    digitalWrite(LED_STATUS, ledState ? HIGH : LOW);

    int mins = roastSeconds / 60;
    int secs = roastSeconds % 60;

    // Avoid colliding with temp alert beeps if you want
    bool okToBeep = !isBeepPatternActive();

    // Short beep every full minute (except t=0)
    if (okToBeep && secs == 0 && roastSeconds > 0 && roastSeconds != lastMinuteBeepSec) {
      beepShort();
      lastMinuteBeepSec = roastSeconds;
    }

    // Long beep every 10 minutes
    if (okToBeep && secs == 0 && mins > 0 && (mins % 10) == 0 && mins != lastTenMinBeepMin) {
      beepLong();
      lastTenMinBeepMin = mins;
    }

    updateLCD();
  }
}

// ==============================
// Stepper motion (non-blocking)
// ==============================
void handleStepper() {
  if (!motorEnabled) return;

  unsigned long nowMicros = micros();
  if (nowMicros - lastStepMicros >= stepDelayMicros) {
    lastStepMicros = nowMicros;
    digitalWrite(STEP_PIN, HIGH);
    digitalWrite(STEP_PIN, LOW);
  }
}

// ==============================
// Main loop
// ==============================
void loop() {

#if SHOW_STARTUP_BANNER
  if (showingBanner) {
    if (millis() - bannerStartTime >= BANNER_TIME_MS) {
      showingBanner = false;
      lcd.clear();

      updateLCD();          // <-- force an immediate redraw
      // lastLcdUpdate = 0;  // <-- optional: reset your LCD refresh timer variable if you have one
    } else {
      return;
    }

  }
#endif

  bool motorPressed   = debounceRead(dbMotor);
  bool rpmPressed     = debounceRead(dbRpm);
  bool profilePressed = debounceRead(dbProfile);
  bool timerPressed   = debounceRead(dbTimer);

  handleMotorButton(motorPressed);
  handleRpmButton(rpmPressed);
  handleProfileButton(profilePressed);
  handleTimerButton(timerPressed);

// Temperature alerts (sample TC at a sane rate; still service beep engine every loop)
unsigned long now = millis();
if (now - lastTempSampleMs >= TEMP_SAMPLE_MS) {
  lastTempSampleMs = now;
  lastTempF = readTC1_F(); // read MAX6675 only 5x/sec
}

if (!isnan(lastTempF)) {
  checkTempAlertsF(lastTempF, BUZZER_PIN);
} else {
  serviceBeepPattern(BUZZER_PIN);
}


  handleTimerTickAndStatusLED();
  handleStepper();
}
