// --- Includes ---
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <BleKeyboard.h>
#include "RoboEyesTFT_eSPI.h"

// --- Settings ---
// Backlight pin
#define TFT_BL_PIN 4

// Inputs
#define ENC_A_PIN 25
#define ENC_B_PIN 26
#define ENC_SW_PIN 32
#define BTN1_PIN 33
#define BTN2_PIN 27
#define BTN3_PIN 12
#define BTN4_PIN 13

// Encoder direction & resolution config
#define ENCODER_REVERSED false

// Most rotary encoders (like EC11) have 4 state transitions per physical click (detent).
// Set to 4 for 1 action per click, or 2/1 if your encoder feels too sluggish.
#define ENCODER_STEPS_PER_NOTCH 2

// For Windows/Linux use KEY_LEFT_CTRL.
// For macOS, you may prefer KEY_LEFT_GUI.
#define MODIFIER_KEY KEY_LEFT_CTRL

// --- Objects ---
// Screen and robot eyes
TFT_eSPI tft = TFT_eSPI();
TFT_RoboEyes eyes = TFT_RoboEyes(tft, LANDSCAPE);

// Bluetooth HID keyboard
BleKeyboard bk("RoboEyes Macropad", "YoussefTech", 100);

// --- Rotary encoder ---
volatile int encoderRawTicks = 0;
volatile uint8_t encoderPrevState = 0;

// Quadrature state machine table (Gray Code transitions)
// 0 = no change, 1 = Clockwise, -1 = Counter-Clockwise
static const int8_t encoderStates[] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0};

IRAM_ATTR void encoderISR()
{
  uint8_t a = digitalRead(ENC_A_PIN);
  uint8_t b = digitalRead(ENC_B_PIN);

  // Combine previous and current states into a 4-bit index
  encoderPrevState = ((encoderPrevState << 2) | (a << 1) | b) & 0x0F;

  // Update state from transition table
  encoderRawTicks += encoderStates[encoderPrevState];
}

// --- Button handling ---
struct Button
{
  uint8_t pin = 0;

  bool pressedEvent = false;
  bool releasedEvent = false;
  bool longEvent = false;
  bool doubleEvent = false;

  bool rawState = HIGH;
  bool stableState = HIGH;
  bool isDown = false;
  bool longFired = false;
  bool suppressRelease = false;

  unsigned long lastChange = 0;
  unsigned long pressStart = 0;
  unsigned long releaseTime = 0;
  unsigned long waitingForDouble = false;

  void begin(uint8_t p)
  {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    rawState = digitalRead(pin);
    stableState = rawState;
    lastChange = millis();
  }

  void update(unsigned long now, unsigned long debounceMs = 25, unsigned long holdMs = 800, unsigned long doubleMs = 300)
  {
    pressedEvent = false;
    releasedEvent = false;
    longEvent = false;
    doubleEvent = false;

    bool reading = digitalRead(pin);

    if (reading != rawState)
    {
      rawState = reading;
      lastChange = now;
    }

    if ((now - lastChange) > debounceMs && stableState != rawState)
    {
      stableState = rawState;

      if (stableState == LOW) // Pressed
      {
        isDown = true;
        pressStart = now;
        longFired = false;
        if (waitingForDouble)
        {
          doubleEvent = true;
          waitingForDouble = false;
        }
      }
      else // Released
      {
        releasedEvent = true;
        if (isDown && !longFired)
        {
          waitingForDouble = true;
          releaseTime = now;
        }

        isDown = false;
        longFired = false;
        suppressRelease = false;
      }
    }

    if (isDown && !longFired && (now - pressStart >= holdMs))
    {
      longFired = true;
      longEvent = true;
      waitingForDouble = false;
    }

    if (waitingForDouble && (now - releaseTime >= doubleMs))
    {
      pressedEvent = true;
      waitingForDouble = false;
    }
  }
};

Button btnEncoder;
Button btnOne;
Button btnTwo;
Button btnThree;
Button btnFour;

// --- Layers ---
enum Layer : uint8_t
{
  LAYER_MEDIA = 0,
  LAYER_KEYS,
  LAYER_FACE,
  LAYER_COUNT
};

Layer layer = LAYER_MEDIA;

// --- Face states ---
uint8_t faceMoods[] = {DEFAULT, HAPPY, TIRED, ANGRY};
uint8_t faceMoodIdx = 0;

unsigned long lastInteraction = 0;
bool idleEnabled = true;

unsigned long lookUntil = 0;

bool manualBlinkActive = false;
unsigned long manualBlinkOpenAt = 0;

bool tempMoodActive = false;
unsigned long tempMoodUntil = 0;
uint8_t restoreMood = DEFAULT;

bool lastConnected = false;

// --- Backlight ---
void initBacklight()
{
  ledcSetup(1, 5000, 8);
  ledcAttachPin(TFT_BL_PIN, 1);
}

void setBacklight(uint8_t brightness)
{
  ledcWrite(1, brightness);
}

// --- Layer colors ---
void updateLayerColors()
{
  switch (layer)
  {
  case LAYER_MEDIA:
    eyes.setColors(TFT_CYAN, TFT_BLACK);
    break;

  case LAYER_KEYS:
    eyes.setColors(TFT_GREEN, TFT_BLACK);
    break;

  case LAYER_FACE:
    eyes.setColors(TFT_YELLOW, TFT_BLACK);
    break;
  }
}

// --- Face interaction helpers ---
void interaction()
{
  lastInteraction = millis();

  if (idleEnabled)
  {
    eyes.setIdleMode(false);
    idleEnabled = false;
  }
}

void maybeIdle()
{
  if (!idleEnabled && (millis() - lastInteraction > 8000))
  {
    eyes.setIdleMode(true, 4, 2);
    idleEnabled = true;
    lookUntil = 0;
  }
}

void look(uint8_t pos, uint16_t ms = 450)
{
  eyes.setCuriosity(true);
  eyes.setPosition(pos);
  lookUntil = ms ? (millis() + ms) : 0;
}

void flashMood(uint8_t mood, uint16_t ms = 450)
{
  restoreMood = faceMoods[faceMoodIdx];

  eyes.setMood(mood);
  tempMoodActive = true;
  tempMoodUntil = millis() + ms;
}

void triggerBlink()
{
  if (manualBlinkActive)
    return;

  eyes.close();
  manualBlinkActive = true;
  manualBlinkOpenAt = millis() + 130;
}

void triggerWinkRight()
{
  if (manualBlinkActive)
    return;

  eyes.close(true, false);
  manualBlinkActive = true;
  manualBlinkOpenAt = millis() + 130;
}

void triggerWinkLeft()
{
  if (manualBlinkActive)
    return;

  eyes.close(false, true);
  manualBlinkActive = true;
  manualBlinkOpenAt = millis() + 130;
}

void cycleLayer()
{
  layer = static_cast<Layer>((layer + 1) % LAYER_COUNT);

  uint8_t currentMood = faceMoods[faceMoodIdx];
  tempMoodActive = false;
  restoreMood = currentMood;

  eyes.setMood(currentMood);
  eyes.setPosition(DEFAULT);
  eyes.setCuriosity(false);

  updateLayerColors();

  eyes.anim_laugh();

  interaction();
}

// --- HID Helpers ---
void sendKey(uint16_t key)
{
  if (bk.isConnected())
  {
    bk.write(key);
  }
}

void sendMediaKey(const uint8_t *key)
{
  if (!bk.isConnected())
    return;

  bk.write(const_cast<uint8_t *>(key));
}

void sendKey(const uint8_t *key)
{
  sendMediaKey(key);
}

void sendCombo(uint16_t modifier, char key)
{
  if (!bk.isConnected())
    return;

  bk.press(modifier);
  bk.press((uint8_t)key);
  delay(12);
  bk.release(modifier);
  bk.release((uint8_t)key);
}

// --- Input actions ---
void handleEncoder(int delta)
{
  interaction();

  bool cw = (delta > 0);

  int steps = abs(delta);
  if (steps > 3)
    steps = 3;

  switch (layer)
  {
  case LAYER_MEDIA:
    for (int i = 0; i < steps; i++)
    {
      sendKey(cw ? KEY_MEDIA_VOLUME_UP : KEY_MEDIA_VOLUME_DOWN);
    }

    look(cw ? E : W, 500);
    break;

  case LAYER_KEYS:
    for (int i = 0; i < steps; i++)
    {
      sendKey(cw ? KEY_UP_ARROW : KEY_DOWN_ARROW);
    }

    look(cw ? N : S, 500);
    break;

  case LAYER_FACE:
  {
    int n = sizeof(faceMoods) / sizeof(faceMoods[0]);
    int idx = (int)faceMoodIdx + (cw ? steps : -steps);

    idx %= n;
    if (idx < 0)
      idx += n;

    faceMoodIdx = (uint8_t)idx;

    tempMoodActive = false;
    eyes.setMood(faceMoods[faceMoodIdx]);
    eyes.setPosition(DEFAULT);
    eyes.setCuriosity(false);
    lookUntil = 0;
    break;
  }
  }
}

void handleEncoderBtnPressed()
{
  interaction();
}

void handleEncoderBtnLong()
{
  cycleLayer();
}

void handleEncoderBtnDouble()
{
  interaction();
  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_PLAY_PAUSE);
    flashMood(HAPPY, 200);
    break;
  case LAYER_KEYS:
    sendKey(' ');
    flashMood(HAPPY, 200);
    break; // Spacebar
  case LAYER_FACE:
    eyes.anim_laugh();
    flashMood(HAPPY, 500);
    break;
  }
}

void handleEncoderBtnReleased()
{
  interaction();

  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_MUTE);
    flashMood(HAPPY, 500);
    break;
  case LAYER_KEYS:
    sendKey(KEY_RETURN);
    flashMood(HAPPY, 500);
    break;

  case LAYER_FACE:
    triggerWinkRight();
    break;

  default:
    break;
  }
}

void handleBtn1Pressed()
{
  interaction();

  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_PLAY_PAUSE);
    flashMood(HAPPY, 250);
    break;

  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'c');
    flashMood(HAPPY, 250);
    break;

  case LAYER_FACE:
    eyes.anim_laugh();
    flashMood(TIRED, 500);
    break;

  default:
    break;
  }
}

void handleBtn1Long()
{
  interaction();
  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_STOP);
    flashMood(ANGRY, 250);
    break;
  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'x');
    flashMood(ANGRY, 250);
    break; // Cut
  case LAYER_FACE:
    faceMoodIdx = 3;
    eyes.setMood(faceMoods[faceMoodIdx]);
    break; // Angry
  }
}
void handleBtn1Double()
{
  interaction();
  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_NEXT_TRACK);
    flashMood(HAPPY, 250);
    break;
  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'v');
    flashMood(HAPPY, 250);
    break; // Paste
  case LAYER_FACE:
    faceMoodIdx = 1;
    eyes.setMood(faceMoods[faceMoodIdx]);
    break; // Happy
  }
}

void handleBtn2Pressed()
{
  interaction();

  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_NEXT_TRACK);
    flashMood(HAPPY, 250);
    break;

  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'v');
    flashMood(HAPPY, 250);
    break;

  case LAYER_FACE:
    eyes.anim_confused();
    flashMood(HAPPY, 500);
    break;

  default:
    break;
  }
}

void handleBtn2Long()
{
  interaction();
  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_PREVIOUS_TRACK);
    flashMood(TIRED, 250);
    break;
  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'z');
    flashMood(TIRED, 250);
    break; // Undo
  case LAYER_FACE:
    faceMoodIdx = 2;
    eyes.setMood(faceMoods[faceMoodIdx]);
    break; // Tired
  }
}

void handleBtn2Double()
{
  interaction();
  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_STOP);
    flashMood(DEFAULT, 250);
    break;
  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'y');
    flashMood(DEFAULT, 250);
    break; // Redo
  case LAYER_FACE:
    faceMoodIdx = 0;
    eyes.setMood(faceMoods[faceMoodIdx]);
    break; // Default
  }
}

void handleBtn3Pressed()
{
  interaction();

  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_PREVIOUS_TRACK);
    flashMood(HAPPY, 250);
    break;

  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'z');
    flashMood(HAPPY, 250);
    break;

  case LAYER_FACE:
    eyes.anim_laugh();
    flashMood(HAPPY, 500);
    break;

  default:
    break;
  }
}

void handleBtn3Long()
{
  interaction();
  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_PLAY_PAUSE);
    flashMood(TIRED, 250);
    break;
  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'c');
    flashMood(TIRED, 250);
    break;
  case LAYER_FACE:
    faceMoodIdx = 2;
    eyes.setMood(faceMoods[faceMoodIdx]);
    break;
  }
}

void handleBtn3Double()
{
  interaction();
  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_MUTE);
    flashMood(HAPPY, 250);
    break;
  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'x');
    flashMood(HAPPY, 250);
    break;
  case LAYER_FACE:
    faceMoodIdx = 1;
    eyes.setMood(faceMoods[faceMoodIdx]);
    break;
  }
}

void handleBtn4Pressed()
{
  interaction();

  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_STOP);
    flashMood(HAPPY, 250);
    break;

  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'y');
    flashMood(HAPPY, 250);
    break;

  case LAYER_FACE:
    triggerBlink();
    break;

  default:
    break;
  }
}

void handleBtn4Long()
{
  interaction();
  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_NEXT_TRACK);
    flashMood(ANGRY, 250);
    break;
  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'v');
    flashMood(ANGRY, 250);
    break;
  case LAYER_FACE:
    faceMoodIdx = 3;
    eyes.setMood(faceMoods[faceMoodIdx]);
    break;
  }
}

void handleBtn4Double()
{
  interaction();
  switch (layer)
  {
  case LAYER_MEDIA:
    sendKey(KEY_MEDIA_PREVIOUS_TRACK);
    flashMood(DEFAULT, 250);
    break;
  case LAYER_KEYS:
    sendCombo(MODIFIER_KEY, 'z');
    flashMood(DEFAULT, 250);
    break;
  case LAYER_FACE:
    faceMoodIdx = 0;
    eyes.setMood(faceMoods[faceMoodIdx]);
    break;
  }
}

// --- BLE connection state ---
void handleConnection()
{
  bool connected = bk.isConnected();

  if (connected && !lastConnected)
  {
    eyes.open();
    flashMood(HAPPY, 1500);
  }

  if (!connected && lastConnected)
  {
    eyes.setMood(TIRED);
  }

  lastConnected = connected;
}

// Face timers
void handleFaceTimers()
{
  maybeIdle();

  if (lookUntil != 0 && millis() >= lookUntil)
  {
    eyes.setPosition(DEFAULT);
    eyes.setCuriosity(false);
    lookUntil = 0;
  }

  if (manualBlinkActive && millis() >= manualBlinkOpenAt)
  {
    eyes.open();
    manualBlinkActive = false;
  }

  if (tempMoodActive && millis() >= tempMoodUntil)
  {
    tempMoodActive = false;
    eyes.setMood(restoreMood);
  }
}

// --- Setup ---
void setup()
{
  Serial.begin(9600);

  analogReadResolution(12);

  initBacklight();
  setBacklight(200);

  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);

  // Initialize state variable with starting pin reads
  uint8_t a = digitalRead(ENC_A_PIN);
  uint8_t b = digitalRead(ENC_B_PIN);
  encoderPrevState = (a << 1) | b;

  // Interrupts attached to BOTH pins on CHANGE for full quadrature decoding
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), encoderISR, CHANGE);

  btnEncoder.begin(ENC_SW_PIN);
  btnOne.begin(BTN1_PIN);
  btnTwo.begin(BTN2_PIN);
  btnThree.begin(BTN3_PIN);
  btnFour.begin(BTN4_PIN);

  tft.init();
  eyes.begin(60);

  eyes.setColors(TFT_WHITE, TFT_BLACK);

  eyes.setAutoblinker(true, 2, 1);
  eyes.setIdleMode(true, 4, 0);

  eyes.setWidth(64, 64);
  eyes.setHeight(64, 64);
  eyes.setBorderradius(12, 12);
  eyes.setSpacebetween(24);

  eyes.setPosition(DEFAULT);
  eyes.setCuriosity(false);
  eyes.open();

  updateLayerColors();

  bk.begin();

  lastInteraction = millis();
}

// --- Loop ---
void loop()
{
  unsigned long now = millis();

  btnEncoder.update(now);
  btnOne.update(now);
  btnTwo.update(now);
  btnThree.update(now);
  btnFour.update(now);

  if (btnEncoder.pressedEvent)
    handleEncoderBtnPressed();
  if (btnEncoder.longEvent)
    handleEncoderBtnLong();
  if (btnEncoder.doubleEvent)
    handleEncoderBtnDouble();

  if (btnOne.pressedEvent)
    handleBtn1Pressed();
  if (btnOne.longEvent)
    handleBtn1Long();
  if (btnOne.doubleEvent)
    handleBtn1Double();

  if (btnTwo.pressedEvent)
    handleBtn2Pressed();
  if (btnTwo.longEvent)
    handleBtn2Long();
  if (btnTwo.doubleEvent)
    handleBtn2Double();

  if (btnThree.pressedEvent)
    handleBtn3Pressed();
  if (btnThree.longEvent)
    handleBtn3Long();
  if (btnThree.doubleEvent)
    handleBtn3Double();

  if (btnFour.pressedEvent)
    handleBtn4Pressed();
  if (btnFour.longEvent)
    handleBtn4Long();
  if (btnFour.doubleEvent)
    handleBtn4Double();

  noInterrupts();
  int rawTicks = encoderRawTicks;
  // Retain leftover ticks that haven't formed a full click step yet
  encoderRawTicks %= ENCODER_STEPS_PER_NOTCH;
  interrupts();

  // Convert raw quadrature steps into physical clicks/detents
  int delta = rawTicks / ENCODER_STEPS_PER_NOTCH;

  if (ENCODER_REVERSED)
  {
    delta = -delta;
  }

  if (delta != 0)
  {
    handleEncoder(delta);
  }

  handleConnection();
  handleFaceTimers();

  eyes.update();
}