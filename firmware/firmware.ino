#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EncButton.h>
#include <ButtonDebounce.h>

// encoder
#define ENC_A 2
#define ENC_B 3
#define ENC_KEY 4
// LCD
#define LCD_ADDR 0x27
#define LCD_LINES 4
#define LCD_CHARS 20
// buttons
#define BTN_MODE 5
#define BTN_FIRE 6
#define BTN_FOCUS 7
#define RELAY_PIN 8
#define debounceDelay 80

// Software
#define linearMode 0
#define testStripMode 1
#define printingMode 2

EncButton<EB_CALLBACK, ENC_A, ENC_B, ENC_KEY> enc;
// TODO(nikonov1101): use 4bit connection instead.
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_CHARS, LCD_LINES);

ButtonDebounce modeButton(BTN_MODE, debounceDelay);
ButtonDebounce fireButton(BTN_FIRE, debounceDelay);
ButtonDebounce focusButton(BTN_FOCUS, debounceDelay);

// the state
static uint8_t mode = testStripMode;
static uint8_t submode = 0;

static float mainTimer = 8.0;         // 4 byte
static float savedTimer = mainTimer;  // 4 more byte
static float tmpTimer;                // a crutch for a fstop auto advancing
static uint8_t deltaStops = 1;        // 1 / n-th stop
static uint8_t runCounter = 0;        // increments on each test run
// static uint8_t burnSteps = 1;

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
  lcd.init();
  lcd.backlight();

  // setup the encoder
  enc.attach(LEFT_HANDLER, encPlus);
  enc.attach(LEFT_H_HANDLER, encPlus);
  enc.attach(RIGHT_HANDLER, encMinus);
  enc.attach(RIGHT_H_HANDLER, encMinus);
  enc.attach(CLICK_HANDLER, encClick);

  // encoder handled via hardware interrupts
  attachInterrupt(0, isr, CHANGE);
  attachInterrupt(1, isr, CHANGE);

  // show the initial screen
  printModeInitials();
}

ISR(TIMER1_COMPA_vect) {
  TCNT1 = 0;  // First, set the timer back to 0 so it resets for next interrupt
  // Serial.println(millis());
  // timerUpdate();
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
    case printingMode:
      printPrintingMode();
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

void printPrintingMode() {
  // TODO(nikonov1101): don't clear the display on each timer tick.
  lcd.clear();

  switch (submode) {
    case 0:
      lcd.setCursor(0, 0);
      lcd.print("Print: base time");

      printTimerValue();
      break;
    case 1:
      lcd.setCursor(0, 0);
      lcd.print("Print: burn step");

      lcd.setCursor(0, 1);
      lcd.print("d=1");
      if (deltaStops > 1) {
        lcd.print("/");
        lcd.print(deltaStops);
      }
      lcd.print(" fs");
      break;

    case 2:
      lcd.setCursor(0, 0);
      lcd.print("Print: base");
      printTimerValue();
      break;

    case 3:
      lcd.setCursor(0, 0);
      lcd.print("Burn +");
      if (deltaStops == 1) {
        lcd.print(runCounter);
        lcd.print(" fs");
      } else {
        lcd.print(runCounter);
        lcd.print("/");
        lcd.print(deltaStops);
        lcd.print(" fs");
      }

      printTimerValue();
      break;
  }
}

void setMode() {
  // TODO(@nikonov1101): reset timers as well?
  submode = 0;
  runCounter = 0;
  tmpTimer = 0;
  mainTimer = 8.0;
  mode++;
  if (mode > printingMode) {
    mode = linearMode;
  }

  allowedToRUN = mode == linearMode;

  // switching to printing mode ->
  // round up the timer value
  if (printingMode) {
    mainTimer = (int)mainTimer;
  }

  printModeInitials();
}

void encPlus() {
  if (relayMode != RELAY_MODE_OFF) {
    // lock interface if running
    return;
  }

  switch (mode) {
    case linearMode:
      mainTimer = encInc(mainTimer);
      printLinearMode();
      break;

    case testStripMode:
      switch (submode) {
        case 0:
          mainTimer = encInc(mainTimer);
          break;
        case 1:
          deltaStops = nextFstopFraction(deltaStops);
          break;
        default:
          return;
      }

      printTestMode();
      break;

    case printingMode:
      switch (submode) {
        case 0:
          mainTimer = encInc(mainTimer);
          break;
        case 1:
          deltaStops = nextFstopFraction(deltaStops);
          break;
        case 3:
          {
            if (runCounter == 99) {
              return;
            }

            runCounter++;
            float x = mulFstop(savedTimer, runCounter, deltaStops);
            mainTimer = x - savedTimer;
            break;
          }  // case 3
        default:
          return;
      }

      printPrintingMode();
      break;
  }
}

void encMinus() {
  if (relayMode != RELAY_MODE_OFF) {
    // lock interface if running
    return;
  }

  switch (mode) {
    case linearMode:
      mainTimer = encDec(mainTimer);
      printLinearMode();
      break;

    case testStripMode:
      switch (submode) {
        case 0:
          mainTimer = encDec(mainTimer);
          break;
        case 1:
          deltaStops = prevFstopFraction(deltaStops);
          break;
        default:
          return;
      }

      printTestMode();
      break;

    case printingMode:
      switch (submode) {
        case 0:
          mainTimer = encDec(mainTimer);
          break;
        case 1:
          deltaStops = prevFstopFraction(deltaStops);
          break;
        case 3:
          {
            if (runCounter == 1) {
              break;
            }

            runCounter--;
            float x = mulFstop(savedTimer, runCounter, deltaStops);
            mainTimer = x - savedTimer;
            break;
          }
        default:
          return;
      }

      printPrintingMode();
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

    case printingMode:
      submode++;
      if (submode > 3) {
        submode = 0;
      }

      if (submode == 1) {
        // stash the timer for further fstop calculations
        savedTimer = mainTimer;
      }

      allowedToRUN = submode > 1;  // 2 and 3 for base and burn

      // BURN MODE
      if (submode == 3) {
        runCounter = 1;
        float x = addFstop(savedTimer, deltaStops);
        mainTimer = x - savedTimer;
      }

      printPrintingMode();
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

float encInc(float v) {
  if (v == 999.9) {
    return v;
  }

  if (v > 9.51) {
    v = v + 1.0;
  } else {
    v = v + 0.5;
  }

  return v;
}

float encDec(float v) {
  if (v == 0) {
    return 0;
  }
  if (v > 10) {
    v = v - 1.0;
  } else {
    v = v - 0.5;
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
    case 8:
      return 12;
    case 12:
      return 16;
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
    case 12:
      return 8;
    case 16:
      return 12;
    default:
      return v;
  }
}
