// --- Includes ---
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <BleKeyboard.h>
#include <Preferences.h>
#include "RoboEyesTFT_eSPI.h"

// --- Settings ---
#define TFT_BL_PIN 4
#define BAT_PIN 34
#define DEBUG_BTN_PIN 35

// Inputs
#define ENC_A_PIN 25
#define ENC_B_PIN 26
#define ENC_SW_PIN 32
#define BTN1_PIN 27
#define BTN2_PIN 33
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
Preferences prefs;

// Bluetooth HID keyboard
BleKeyboard bk("RoboEyes Macropad", "YoussefTech", 100);

// --- Rotary encoder ---
volatile int encoderRawTicks = 0;
volatile uint8_t encoderPrevState = 0;

// --- Globals ---
bool isDebugMode = false;
bool isLowBattery = false;
unsigned long lastBatCheck = 0;

void logEvent(const char *msg)
{
  if (isDebugMode)
  {
    Serial.print("[");
    Serial.print(millis());
    Serial.print("] ");
    Serial.println(msg);
  }
}

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
  bool doubleFired = false;

  unsigned long lastChange = 0;
  unsigned long pressStart = 0;
  unsigned long releaseTime = 0;
  bool waitingForDouble = false;

  void begin(uint8_t p)
  {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    rawState = digitalRead(pin);
    stableState = rawState;
    lastChange = millis();
  }

  void update(unsigned long now, unsigned long debounceMs = 25, unsigned long holdMs = 800, unsigned long doubleMs = 100)
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
          doubleFired = true; // Mark that this sequence resulted in a double press
        }
      }
      else // Released
      {
        releasedEvent = true;

        // Only wait for a double press if we haven't already fired a long or double event
        if (isDown && !longFired && !doubleFired)
        {
          waitingForDouble = true;
          releaseTime = now;
        }
        else
        {
          doubleFired = false; // Reset flag for the next sequence
        }

        isDown = false;
        longFired = false;
      }
    }

    // Prevent long press from firing if a double press already fired on this sequence
    if (isDown && !longFired && !doubleFired && (now - pressStart >= holdMs))
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

Button btnEncoder, btnOne, btnTwo, btnThree, btnFour;

// --- Layers ---
enum Layer : uint8_t
{
  LAYER_ONE = 0,
  LAYER_TWO,
  LAYER_THREE,
  LAYER_COUNT
};

Layer layer = LAYER_ONE;

// --- Face customization ---
enum FaceeditMode
{
  FACE_EDIT_MOOD = 0,
  FACE_EDIT_SPACE,
  FACE_EDIT_RADIUS,
  FACE_EDIT_HEIGHT,
  FACE_EDIT_WIDTH,
  FACE_EDIT_COUNT
};
FaceeditMode editMode = FACE_EDIT_MOOD;

int currentSpace;
int currentRadius;
int currentHeight;
int currentWidth;

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

void applyFaceSettings()
{
  eyes.setSpacebetween(currentSpace);
  eyes.setBorderradius(currentRadius, currentRadius);
  eyes.setWidth(currentWidth, currentWidth);
  eyes.setHeight(currentHeight, currentHeight);
  eyes.setMood(faceMoods[faceMoodIdx]);
}

void loadFaceSettings()
{
  prefs.begin("robo_face", true);

  currentSpace = prefs.getInt("space", 24);
  currentRadius = prefs.getInt("radius", 12);
  currentHeight = prefs.getInt("height", 64);
  currentWidth = prefs.getInt("width", 64);
  faceMoodIdx = prefs.getUChar("mood", 0);

  prefs.end();

  logEvent("Loaded face settings from NVS");
}

void saveFaceSettings()
{
  prefs.begin("robo_face", false);

  prefs.putInt("space", currentSpace);
  prefs.putInt("radius", currentRadius);
  prefs.putInt("height", currentHeight);
  prefs.putInt("width", currentWidth);
  prefs.putUChar("mood", faceMoodIdx);

  prefs.end();

  logEvent("Saved face settings to NVS");
}

// --- Macro engine ---
enum ActionType
{
  ACT_NONE = 0,
  ACT_KEY,
  ACT_MEDIA,
  ACT_COMBO,
  ACT_SET_MOOD,
  ACT_FACE_MODE,
  ACT_CYCLE_LAYER,
  ACT_RESET_FACE
};

struct Macro
{
  ActionType type;
  uint16_t key;
  const uint8_t *media;
  uint16_t modifier;
  char comboChar;
  uint8_t mood; // 255 for no reaction mood
  uint16_t moodMs;
  uint8_t param;
};

// Helper macros to make the grid readable
#define M_NONE {ACT_NONE, 0, nullptr, 0, 0, 255, 0, 0}

#define M_KEY(k, m, ms) {ACT_KEY, k, nullptr, 0, 0, m, ms, 0}

#define M_MEDIA(mk, m, ms) {ACT_MEDIA, 0, mk, 0, 0, m, ms, 0}

// Combo for standard shortcuts (Ctrl/Cmd)
#define M_COMBO(c, mood, ms) {ACT_COMBO, 0, nullptr, MODIFIER_KEY, c, mood, ms, 0}

// Combo for special shortcuts (Win/Super)
#define M_COMBO_SUPER(c, mood, ms) {ACT_COMBO, 0, nullptr, KEY_LEFT_GUI, c, mood, ms, 0}

// Multi-modifier combo helper (e.g. Ctrl + Shift + Key)
#define M_COMBO_SHIFT(c, mood, ms) {ACT_COMBO, 0, nullptr, (MODIFIER_KEY | KEY_LEFT_SHIFT), c, mood, ms, 0}

#define M_SET_MOOD(idx) {ACT_SET_MOOD, 0, nullptr, 0, 0, 255, 0, idx}

#define M_FACE_MODE(fm) {ACT_FACE_MODE, 0, nullptr, 0, 0, 255, 0, fm}

#define M_CYCLE_LAYER {ACT_CYCLE_LAYER, 0, nullptr, 0, 0, 255, 0, 0}

#define M_RESET_FACE {ACT_RESET_FACE, 0, nullptr, 0, 0, 255, 0, 0}

// The Keymap Grid: [Layer] [Button: 0=Enc, 1-4=Btns] [Event: 0=Short, 1=Long, 2=Double]
Macro keyMap[LAYER_COUNT][5][3] = {
    // -------------------------------------------------------------------------
    // LAYER ONE: Media & System Navigation
    // Encoder Dial: Volume Up / Volume Down
    // -------------------------------------------------------------------------
    {
        {
            // Encoder Button
            M_MEDIA(KEY_MEDIA_MUTE, TIRED, 300),      // Short: Mute/Unmute Audio
            M_CYCLE_LAYER,                            // Long:  Cycle to Layer 2
            M_MEDIA(KEY_MEDIA_PLAY_PAUSE, HAPPY, 300) // Double: Play / Pause
        },
        {
            // Button 1: Track Previous
            M_MEDIA(KEY_MEDIA_PREVIOUS_TRACK, DEFAULT, 200),              // Short: Previous Track
            M_MEDIA(KEY_MEDIA_WWW_BACK, DEFAULT, 200),                    // Long:  Browser Back
            M_MEDIA(KEY_MEDIA_CONSUMER_CONTROL_CONFIGURATION, HAPPY, 300) // Double: Reserved / Custom Media
        },
        {
            // Button 2: Play/Pause
            M_MEDIA(KEY_MEDIA_PLAY_PAUSE, HAPPY, 300), // Short: Play / Pause
            M_MEDIA(KEY_MEDIA_CALCULATOR, HAPPY, 400), // Long:  Launch Calculator App
            M_MEDIA(KEY_MEDIA_STOP, ANGRY, 300)        // Double: Stop Playback
        },
        {
            // Button 3: Track Next
            M_MEDIA(KEY_MEDIA_NEXT_TRACK, DEFAULT, 200), // Short: Next Track
            M_MEDIA(KEY_MEDIA_WWW_BACK, DEFAULT, 200),   // Long:  Browser Forward
            M_KEY(KEY_PAGE_DOWN, HAPPY, 500)             // Double: PrintScreen / Screenshot
        },
        {
            // Button 4: System Workspaces / Task View
            M_COMBO(KEY_TAB, HAPPY, 400),     // Short: Task View (Alt/Win+Tab)
            M_KEY(KEY_CAPS_LOCK, TIRED, 300), // Long:  Toggle CapsLock
            M_COMBO_SUPER('m', HAPPY, 400)    // Double: Win+M / Minimuize all windows
        }},

    // -------------------------------------------------------------------------
    // LAYER TWO: Universal Productivity & Editing
    // Encoder Dial: Scroll Up / Scroll Down (Up/Down Arrows)
    // -------------------------------------------------------------------------
    {
        {
            // Encoder Button
            M_KEY(KEY_RETURN, DEFAULT, 200), // Short: Enter
            M_CYCLE_LAYER,                   // Long:  Cycle to Layer 3
            M_KEY(KEY_BACKSPACE, TIRED, 200) // Double: Backspace
        },
        {
            // Button 1: Undo / Redo
            M_COMBO('z', DEFAULT, 200),    // Short: Undo (Ctrl+Z / Cmd+Z)
            M_COMBO('y', HAPPY, 200),      // Long:  Redo (Ctrl+Y / Cmd+Y)
            M_COMBO_SHIFT('z', HAPPY, 200) // Double: Redo alternate (Ctrl+Shift+Z)
        },
        {
            // Button 2: Copy / Cut
            M_COMBO('c', HAPPY, 200),  // Short: Copy (Ctrl+C)
            M_COMBO('x', ANGRY, 200),  // Long:  Cut  (Ctrl+X)
            M_COMBO('a', DEFAULT, 200) // Double: Select All (Ctrl+A)
        },
        {
            // Button 3: Paste
            M_COMBO('v', HAPPY, 200),       // Short: Paste (Ctrl+V)
            M_COMBO_SHIFT('v', HAPPY, 200), // Long:  Paste Plain Text (Ctrl+Shift+V)
            M_COMBO('f', DEFAULT, 200)      // Double: Find (Ctrl+F)
        },
        {
            // Button 4: Save / Quick Actions
            M_COMBO('s', HAPPY, 300), // Short: Quick Save (Ctrl+S)
            M_COMBO('w', ANGRY, 300), // Long:  Close Window/Tab (Ctrl+W)
            M_COMBO('t', HAPPY, 300)  // Double: New Tab (Ctrl+T)
        }},

    // -------------------------------------------------------------------------
    // LAYER THREE: Face Customization Tweak Engine
    // Encoder Dial: Adjust selected parameter value
    // -------------------------------------------------------------------------
    {
        {
            // Encoder Button
            M_NONE,        // Short:  No action
            M_CYCLE_LAYER, // Long:   Return to Layer 1
            M_RESET_FACE   // Double: Reset Eyes Geometry
        },
        {
            // Button 1: Mood Selector
            M_FACE_MODE(FACE_EDIT_MOOD), // Short: Select Mood mode
            M_SET_MOOD(0),               // Long:  Force DEFAULT Mood
            M_SET_MOOD(1)                // Double: Force HAPPY Mood
        },
        {
            // Button 2: Eye Spacing
            M_FACE_MODE(FACE_EDIT_SPACE), // Short: Select Eye Gap mode
            M_NONE,                       // Long:  Unassigned
            M_NONE                        // Double: Unassigned
        },
        {
            // Button 3: Corner Radius
            M_FACE_MODE(FACE_EDIT_RADIUS), // Short: Select Corner Radius mode
            M_NONE,                        // Long:  Unassigned
            M_NONE                         // Double: Unassigned
        },
        {
            // Button 4: Eye Dimensions
            M_FACE_MODE(FACE_EDIT_HEIGHT), // Short: Adjust Eye Height
            M_FACE_MODE(FACE_EDIT_WIDTH),  // Long:  Adjust Eye Width
            M_SET_MOOD(3)                  // Double: Force ANGRY Mood
        }}};

// --- Layer colors ---
void updateLayerColors()
{
  if (isLowBattery)
  {
    eyes.setColors(TFT_RED, TFT_BLACK);
    return;
  }
  switch (layer)
  {
  case LAYER_ONE:
    eyes.setColors(TFT_CYAN, TFT_BLACK);
    break;

  case LAYER_TWO:
    eyes.setColors(TFT_GREEN, TFT_BLACK);
    break;

  case LAYER_THREE:
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
  if (isLowBattery)
    return;

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
  if (isDebugMode)
  {
    Serial.print(millis());
    Serial.print(" - Layer Changed to: ");
    Serial.println(layer);
  }

  layer = static_cast<Layer>((layer + 1) % LAYER_COUNT);

  uint8_t currentMood = faceMoods[faceMoodIdx];
  tempMoodActive = false;
  restoreMood = currentMood;

  eyes.setMood(isLowBattery ? TIRED : currentMood);
  eyes.setPosition(DEFAULT);
  eyes.setCuriosity(false);

  updateLayerColors();

  if (!isLowBattery)
    eyes.anim_laugh();

  interaction();
}

// --- HID & Execution engine ---
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

  // Split modifier mask if bitwise combinations are passed
  if (modifier & KEY_LEFT_CTRL)
    bk.press(KEY_LEFT_CTRL);
  if (modifier & KEY_LEFT_SHIFT)
    bk.press(KEY_LEFT_SHIFT);
  if (modifier & KEY_LEFT_ALT)
    bk.press(KEY_LEFT_ALT);
  if (modifier & KEY_LEFT_GUI)
    bk.press(KEY_LEFT_GUI);

  bk.press((uint8_t)key);
  delay(15);
  bk.releaseAll();
}

void executeMacro(Layer l, uint8_t btnIdx, uint8_t evtIdx)
{
  Macro m = keyMap[l][btnIdx][evtIdx];
  interaction();

  switch (m.type)
  {
  case ACT_KEY:
    sendKey(m.key);
    break;

  case ACT_MEDIA:
    sendMediaKey(m.media);
    break;

  case ACT_COMBO:
    sendCombo(m.modifier, m.comboChar);
    break;

  case ACT_SET_MOOD:
    faceMoodIdx = m.param;
    eyes.setMood(faceMoods[faceMoodIdx]);
    break;

  case ACT_FACE_MODE:
    editMode = (FaceeditMode)m.param;
    if (isDebugMode)
    {
      Serial.print("Face Edit Mode: ");
      Serial.println(editMode);
    }
    flashMood(faceMoods[editMode % 4], 300);
    break;

  case ACT_CYCLE_LAYER:
    cycleLayer();
    return;

  case ACT_RESET_FACE:
    currentSpace = 24;
    currentRadius = 12;
    currentHeight = 64;
    currentWidth = 64;
    faceMoodIdx = 0;

    applyFaceSettings();
    saveFaceSettings();

    flashMood(HAPPY, 500);
    break;

  case ACT_NONE:
  default:
    break;
  }

  if (m.mood != 255 && m.type != ACT_NONE)
    flashMood(m.mood, m.moodMs);
}

void logButtonPress(uint8_t btnIdx, uint8_t evtIdx)
{
  if (!isDebugMode)
    return;
  const char *btnNames[] = {"EncBtn", "Btn1", "Btn2", "Btn3", "Btn4"};
  const char *evtNames[] = {"Short", "Long", "Double"};
  char buf[32];
  sprintf(buf, "%s: %s", btnNames[btnIdx], evtNames[evtIdx]);
  logEvent(buf);
}

// --- Encoder logic ---
void handleEncoder(int delta)
{
  interaction();
  bool cw = (delta > 0);
  logEvent(cw ? "Encoder: CW" : "Encoder: CCW");
  int steps = abs(delta);
  if (steps > 3)
    steps = 3;

  if (layer == LAYER_ONE)
  {
    for (int i = 0; i < steps; i++)
      sendKey(cw ? KEY_MEDIA_VOLUME_UP : KEY_MEDIA_VOLUME_DOWN);
    look(cw ? E : W, 500);
  }
  else if (layer == LAYER_TWO)
  {
    for (int i = 0; i < steps; i++)
      sendKey(cw ? KEY_UP_ARROW : KEY_DOWN_ARROW);
    look(cw ? E : W, 500);
  }
  else if (layer == LAYER_THREE)
  {
    int dir = cw ? 1 : -1;
    switch (editMode)
    {
    case FACE_EDIT_MOOD:
    {
      int n = sizeof(faceMoods) / sizeof(faceMoods[0]);
      int idx = (int)faceMoodIdx + (dir * steps);
      idx %= n;
      if (idx < 0)
        idx += n;
      faceMoodIdx = (uint8_t)idx;
      tempMoodActive = false;
      eyes.setMood(faceMoods[faceMoodIdx]);
      break;
    }
    case FACE_EDIT_SPACE:
      currentSpace = constrain(currentSpace + (dir * steps * 2), 0, 100);
      eyes.setSpacebetween(currentSpace);
      break;
    case FACE_EDIT_RADIUS:
      currentRadius = constrain(currentRadius + (dir * steps), 0, 32);
      eyes.setBorderradius(currentRadius, currentRadius);
      break;
    case FACE_EDIT_HEIGHT:
      currentHeight = constrain(currentHeight + (dir * steps * 2), 10, 100);
      eyes.setHeight(currentHeight, currentHeight);
      break;
    case FACE_EDIT_WIDTH:
      currentWidth = constrain(currentWidth + (dir * steps * 2), 10, 100);
      eyes.setWidth(currentWidth, currentWidth);
      break;
    }

    saveFaceSettings();

    eyes.setPosition(DEFAULT);
    eyes.setCuriosity(false);
    lookUntil = 0;
  }
}

// --- System monitors ---
void handleConnection()
{
  bool connected = bk.isConnected();

  if (connected && !lastConnected)
  {
    logEvent("BLE: Connected");
    eyes.open();
    flashMood(HAPPY, 1500);
  }

  if (!connected && lastConnected)
  {
    logEvent("BLE: Disconnected");
    eyes.setMood(TIRED);
  }

  lastConnected = connected;
}

void checkBattery()
{
  if (millis() - lastBatCheck < 2000)
    return;

  lastBatCheck = millis();

  int raw = 0;
  for (int i = 0; i < 8; i++)
    raw += analogRead(BAT_PIN);
  raw /= 8;

  // Estimate voltage. TTGO T-Display usually has a voltage divider.
  // Adjust the multiplier (2.0) based on your specific board's divider.
  float voltage = (raw / 4095.0) * 3.3 * 2.0;

  // Low battery threshold: ~3.3V.
  // > 2.5V prevents false triggers if no battery is connected and pin reads near 0.
  bool low = (voltage < 3.3 && voltage > 2.5);

  if (low && !isLowBattery)
  {
    isLowBattery = true;
    eyes.setColors(TFT_RED, TFT_BLACK);
    eyes.setMood(TIRED);
    tempMoodActive = false;
    logEvent("BATTERY LOW");
  }
  else if (!low && isLowBattery)
  {
    isLowBattery = false;
    updateLayerColors();
    eyes.setMood(faceMoods[faceMoodIdx]);
    logEvent("BATTERY OK");
  }
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
    eyes.setMood(isLowBattery ? TIRED : restoreMood);
  }
}

// --- Setup ---
void setup()
{
  Serial.begin(9600);

  pinMode(DEBUG_BTN_PIN, INPUT_PULLUP);
  delay(50);

  if (digitalRead(DEBUG_BTN_PIN) == LOW)
  {
    isDebugMode = true;
    Serial.println("\n=================================");
    Serial.println("   DEBUG MODE ENABLED (Pin 35)");
    Serial.println("=================================\n");
  }

  analogReadResolution(12);

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

  eyes.setAutoblinker(true, 4, 3);
  eyes.setIdleMode(true, 4, 3);

  loadFaceSettings();
  applyFaceSettings();

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
  {
    logButtonPress(0, 0);
    executeMacro(layer, 0, 0);
  }
  if (btnEncoder.longEvent)
  {
    logButtonPress(0, 1);
    executeMacro(layer, 0, 1);
  }
  if (btnEncoder.doubleEvent)
  {
    logButtonPress(0, 2);
    executeMacro(layer, 0, 2);
  }

  if (btnOne.pressedEvent)
  {
    logButtonPress(1, 0);
    executeMacro(layer, 1, 0);
  }
  if (btnOne.longEvent)
  {
    logButtonPress(1, 1);
    executeMacro(layer, 1, 1);
  }
  if (btnOne.doubleEvent)
  {
    logButtonPress(1, 2);
    executeMacro(layer, 1, 2);
  }

  if (btnTwo.pressedEvent)
  {
    logButtonPress(2, 0);
    executeMacro(layer, 2, 0);
  }
  if (btnTwo.longEvent)
  {
    logButtonPress(2, 1);
    executeMacro(layer, 2, 1);
  }
  if (btnTwo.doubleEvent)
  {
    logButtonPress(2, 2);
    executeMacro(layer, 2, 2);
  }

  if (btnThree.pressedEvent)
  {
    logButtonPress(3, 0);
    executeMacro(layer, 3, 0);
  }
  if (btnThree.longEvent)
  {
    logButtonPress(3, 1);
    executeMacro(layer, 3, 1);
  }
  if (btnThree.doubleEvent)
  {
    logButtonPress(3, 2);
    executeMacro(layer, 3, 2);
  }

  if (btnFour.pressedEvent)
  {
    logButtonPress(4, 0);
    executeMacro(layer, 4, 0);
  }
  if (btnFour.longEvent)
  {
    logButtonPress(4, 1);
    executeMacro(layer, 4, 1);
  }
  if (btnFour.doubleEvent)
  {
    logButtonPress(4, 2);
    executeMacro(layer, 4, 2);
  }

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

  checkBattery();
  handleConnection();
  handleFaceTimers();

  eyes.update();
}