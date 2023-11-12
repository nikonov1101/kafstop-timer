#include <Wire.h>
#include <LiquidCrystal.h>
#include <EncButton.h>
#include <ButtonDebounce.h>

// encoder
#define ENC_A 2
#define ENC_B 3
#define ENC_KEY 4
// buttons
#define BTN_MODE 8
#define BTN_FIRE 7
#define BTN_FOCUS 6
#define RELAY_PIN 5
#define debounceDelay 80
// LCD
#define LCD_RS 14
#define LCD_EN 15
#define LCD_D4 16
#define LCD_D5 17
#define LCD_D6 18
#define LCD_D7 19

// Software
#define linearMode 0
#define testStripMode 1
#define timerInitialValue 8.0

EncButton enc(ENC_A, ENC_B, ENC_KEY);
LiquidCrystal lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

ButtonDebounce modeButton(BTN_MODE, debounceDelay);
ButtonDebounce fireButton(BTN_FIRE, debounceDelay);
ButtonDebounce focusButton(BTN_FOCUS, debounceDelay);

// the state
static uint8_t mode = testStripMode;
static uint8_t submode = 0;

static float mainTimer = timerInitialValue;         // 4 byte
static float savedTimer = timerInitialValue;  // 4 more byte
static float tmpTimer;                // a crutch for a fstop auto advancing
static uint8_t deltaStops = 1;        // 1 / n-th stop
static uint8_t runCounter = 0;        // increments on each test run

// relay output modes
#define RELAY_MODE_OFF 0
#define RELAY_MODE_ON 1
#define RELAY_MODE_FOCUS 2
#define RELAY_MODE_BURN 3
static uint8_t relayMode = RELAY_MODE_OFF;
static uint8_t allowedToRUN = 0;  // allowed to run the timer only in several (modes+submodes)

// timer control
static uint32_t startedAt = 0;  // millis when started
static uint32_t endAt = 0;      // millis when should stop
static uint8_t tickHappen = 0;

void setup() {
  // debug
  // Serial.begin(9600);

  // setup the timer
  cli();
  TCCR1A = 0;           // Reset entire TCCR1A to 0
  TCCR1B = 0;           // Reset entire TCCR1B to 0
  TCCR1B |= B00000100;  //Set CS12 to 1 so we get prescalar 256
  TIMSK1 |= B00000010;  //Set OCIE1A to 1 so we enable compare match A
  // OCR1A = 3125; // ~50ms
  OCR1A = 6250;  // ~100ms
  sei();

  // initialize the IO
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  fireButton.setCallback(onFire);
  focusButton.setCallback(onFocus);
  modeButton.setCallback(onMode);

  // initialize the lcd
  lcd.begin(16, 2);

  // setup the encoder
  enc.attach(encCallback);

  // encoder handled via hardware interrupts
  attachInterrupt(0, isr, CHANGE);
  attachInterrupt(1, isr, CHANGE);

  // show the initial screen
  printModeInitials();
}

void encCallback() {
  switch (enc.action()) {
    case EB_CLICK:
      encClick();
      break;

    case EB_TURN:
      auto dir = enc.dir();
      uint8_t delta = enc.fast() ? 1 : 0;

      if (dir > 0) {
        encMinus(delta);
      } else {
        encPlus(delta);
      }

      break;
  }
}

ISR(TIMER1_COMPA_vect) {
  TCNT1 = 0;  // First, set the timer back to 0 so it resets for next interrupt
  tickHappen = 1;
}

void isr() {
  enc.tickISR();  // hanle the encoder state
}

void printModeInitials() {
  switch (mode) {
    case linearMode:
      printLinearMode();
      break;
    case testStripMode:
      printTestMode();
      break;
  }
}

void printTimerValue() {
  lcd.setCursor(0, 1);
  lcd.print("t=");

  if (mainTimer < 10) {
    lcd.print(mainTimer, 1);
  } else {
    // TODO(nikonov1101): don't. Print decimal part only if value is less than 10.
    auto x1 = (uint16_t)mainTimer * 100;
    auto x2 = mainTimer * 100.0;

    if (x1 == (uint16_t)x2) {
      lcd.print(mainTimer, 0);
    } else {
      lcd.print(mainTimer, 1);
    }
  }

  lcd.print("s");
}

void printLinearMode() {
  // TODO(nikonov1101): avoid if possible
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Linear: run #");
  lcd.print(runCounter);

  printTimerValue();
}

void printTestMode() {
  // TODO(nikonov1101): don't clear the display on each timer tick.
  lcd.clear();

  switch (submode) {
    case 0:
      lcd.setCursor(0, 0);
      lcd.print("Test: init time");

      printTimerValue();
      break;

    case 1:
      lcd.setCursor(0, 0);
      lcd.print("Test: set step");

      lcd.setCursor(0, 1);
      if (deltaStops == 1) {
        lcd.print("d=1 fs");
      } else {
        lcd.print("d=1/");
        lcd.print(deltaStops);
        lcd.print(" fs");
      }

      break;
    case 2:
      lcd.setCursor(0, 0);
      lcd.print("Test #");
      lcd.print(runCounter);
      if (runCounter > 0) {
        lcd.print(" T=");
        lcd.print(tmpTimer - mainTimer, 1);
      }

      lcd.setCursor(0, 1);
      printTimerValue();
      lcd.print(" d=1");

      if (deltaStops > 1) {
        lcd.print("/");
        lcd.print(deltaStops);
      }
      lcd.print("fs");
      break;
  }
}

void setMode() {
  submode = 0;
  runCounter = 0;
  tmpTimer = 0;
  mainTimer = timerInitialValue;

  mode = (mode == linearMode) ? testStripMode : linearMode;

  allowedToRUN = mode == linearMode;

  printModeInitials();
}

void encPlus(uint8_t isFast) {
  if (relayMode != RELAY_MODE_OFF) {
    // lock interface if running
    return;
  }

  switch (mode) {
    case linearMode:
      mainTimer = encInc(mainTimer, isFast);
      printLinearMode();
      break;

    case testStripMode:
      switch (submode) {
        case 0:
          mainTimer = encInc(mainTimer, isFast);
          break;
        case 1:
          deltaStops = nextFstopFraction(deltaStops);
          break;
        default:
          return;
      }

      printTestMode();
      break;
  }
}

void encMinus(uint8_t isFast) {
  if (relayMode != RELAY_MODE_OFF) {
    // lock interface if running
    return;
  }

  switch (mode) {
    case linearMode:
      mainTimer = encDec(mainTimer, isFast);
      printLinearMode();
      break;

    case testStripMode:
      switch (submode) {
        case 0:
          mainTimer = encDec(mainTimer, isFast);
          break;
        case 1:
          deltaStops = prevFstopFraction(deltaStops);
          break;
        default:
          return;
      }

      printTestMode();
      break;
  }
}

void encClick() {
  if (relayMode != RELAY_MODE_OFF) {
    // lock interface if running
    return;
  }

  // click changes sub-modes
  switch (mode) {
    case testStripMode:
      submode++;
      if (submode > 2) {
        submode = 0;
      }

      if (submode == 1) {
        runCounter = 0;
        savedTimer = mainTimer;
      }

      if (submode == 2) {
        tmpTimer = mainTimer;
      }

      allowedToRUN = submode == 2 ? 1 : 0;

      printTestMode();
      break;

    case linearMode:
      allowedToRUN = 1;
      break;
  }
}

void onFire(const int state) {
  if (state && allowedToRUN) {
    switch (relayMode) {
      case RELAY_MODE_OFF:  // start the timer
        savedTimer = mainTimer;
        relayMode = RELAY_MODE_ON;
        digitalWrite(RELAY_PIN, LOW);  // note: inverted
        startedAt = millis();
        endAt = mainTimer * 1000 + startedAt;
        break;
      case RELAY_MODE_ON:
        return;
      case RELAY_MODE_FOCUS:
        return;
      case RELAY_MODE_BURN:
        break;
    }
  }
}

void onFocus(const int state) {
  if (state) {
    switch (relayMode) {
      case RELAY_MODE_OFF:
        relayMode = RELAY_MODE_FOCUS;
        digitalWrite(RELAY_PIN, LOW);  // note: inverted
        break;
      case RELAY_MODE_FOCUS:
        relayMode = RELAY_MODE_OFF;
        digitalWrite(RELAY_PIN, HIGH);  // note: inverted
        break;
      case RELAY_MODE_BURN:
        break;
      case RELAY_MODE_ON:
        break;
    }
  }
}

void onMode(const int state) {
  if (relayMode != RELAY_MODE_OFF) {
    // lock interface if running
    return;
  }

  if (state) {
    setMode();
  }
}

void timerUpdate() {
  // timer is running
  if (relayMode == RELAY_MODE_ON) {
    auto now = millis();
    if (now >= endAt) {
      // immediately off
      digitalWrite(RELAY_PIN, HIGH);
      relayMode = RELAY_MODE_OFF;

      //
      runCounter++;
      mainTimer = savedTimer;
      if (mode == testStripMode) {
        // TODO:
        auto x = addFstop(tmpTimer, deltaStops);
        mainTimer = x - tmpTimer;
        tmpTimer = x;
      }

      printModeInitials();
      return;
    }

    auto delta = now - startedAt;
    auto tmp = (float)delta / 1000.0;
    mainTimer = savedTimer - tmp;
    printModeInitials();
  }
}

void loop() {
  enc.tick();
  fireButton.update();
  focusButton.update();
  modeButton.update();

  if (tickHappen) {
    timerUpdate();
    tickHappen = 0;
  }
}

/// aux

float addFstop(float v, uint8_t dividee) {
  float tmp = 1.0 / dividee;
  return v * pow(2, tmp);
}

float mulFstop(float v, float nth, uint8_t dividee) {
  float tmp = nth / dividee;
  return v * pow(2, tmp);
}

float subFstop(float v, uint8_t dividee) {
  float tmp = 1.0 / dividee;
  return v / pow(2, tmp);
}

float encInc(float v, uint8_t isFast) {
  if (v >= 999.1) {
    return 999;
  }

  if (v > 9.51) {
    v += isFast ? 3.0 : 1.0;
  } else {
    v += 0.5;
  }

  return v;
}

float encDec(float v, uint8_t isFast) {
  if (v <= 0) {
    return 0;
  }

  if (v > 10) {
    v -= isFast ? 3.0 : 1.0;
  } else {
    v -= 0.5;
  }

  return v;
}

uint8_t nextFstopFraction(uint8_t v) {
  switch (v) {
    case 1:
      return 2;
    case 2:
      return 3;
    case 3:
      return 4;
    case 4:
      return 6;
    case 6:
      return 8;
    default:
      return v;
  }
}

uint8_t prevFstopFraction(uint8_t v) {
  switch (v) {
    case 2:
      return 1;
    case 3:
      return 2;
    case 4:
      return 3;
    case 6:
      return 4;
    case 8:
      return 6;
    default:
      return v;
  }
}
