# RoboEyes MacroPad

<p align="center">
  <img alt="RoboEyes MacroPad prototype" src="imgs/prototype.jpeg" width="300"><br>
  <i>A TTGO T-Display macropad with an animated robot display.</i>
</p>

RoboEyes MacroPad is a BLE macro keypad built on the ESP32. Powered by the [`RoboEyesTFT`](https://github.com/yousseftechdev/RoboEyesTFT) library, the display acts as a desk companion that reacts to button presses, layer switches, and battery status.

---

## Features

- **3 Function Layers:**
  - **Layer 1 (Cyan):** Media & volume dial, track switching, window controls.
  - **Layer 2 (Green):** Standard shortcuts (Copy, Paste, Plain Text Paste, Undo, Redo, Save).
  - **Layer 3 (Yellow):** Live eye editor (adjust eye size, radius, spacing, and mood via the encoder).
- **Animated Eyes:** Real-time expressions (Happy, Tired, Angry, Default), idle blinking, and turn-direction animations.
- **Multi-Input Support:** Short press, long press, and double press per button (15 actions per layer).

> **Hardware Note:** Standard ESP32 chips (ESP32-D0WD / ESP32-WROOM) do not have native USB HID support. The onboard USB port is strictly for serial flashing and power. Keypresses are sent exclusively over Bluetooth.

---

## Hardware & Pinout

### Required Components
- ESP32 dev board (TTGO T-Display or any ESP32 with SPI TFT)
- TFT Display compatible with `TFT_eSPI`
- 1x EC11 Rotary Encoder with push switch
- 4x Tactile buttons
- Voltage divider circuit (connected to Pin 34 for battery monitoring)

### Pin Mapping

| Peripheral | Component Pin | ESP32 GPIO |
| :--- | :--- | :--- |
| **Rotary Encoder** | Channel A | `GPIO 25` |
| | Channel B | `GPIO 26` |
| | Switch | `GPIO 32` |
| **Buttons** | Button 1 | `GPIO 27` |
| | Button 2 | `GPIO 33` |
| | Button 3 | `GPIO 12` |
| | Button 4 | `GPIO 13` |
| **System** | TFT Backlight | `GPIO 4` |
| | Battery Sense | `GPIO 34` |
| | Debug Toggle | `GPIO 35` (Hold LOW on boot for logs) |

---

## Software Dependencies

Install the following libraries before building:

- [`TFT_eSPI`](https://github.com/Bodmer/TFT_eSPI)
- [`RoboEyesTFT`](https://github.com/yousseftechdev/RoboEyesTFT)
- `ESP32-BLE-Keyboard`
- `Preferences` (built into ESP32 Arduino Core)

---

## Keymaps & Customization

Bindings live in a 3D array in the code: `keyMap[LAYER][BUTTON_INDEX][EVENT_TYPE]`.

- **Layers:** `0` (Layer 1), `1` (Layer 2), `2` (Layer 3)
- **Buttons:** `0` (Encoder Switch), `1` (Btn 1), `2` (Btn 2), `3` (Btn 3), `4` (Btn 4)
- **Events:** `0` (Short Press), `1` (Long Press), `2` (Double Press)

### Macro Helper Definitions

You can easily assign custom actions using the built-in helper macros:

```cpp
// Send a single key press with an optional mood reaction
M_KEY(key, mood, duration_ms)

// Send a media key (e.g., KEY_MEDIA_PLAY_PAUSE, KEY_MEDIA_VOLUME_UP)
M_MEDIA(media_key, mood, duration_ms)

// Send a modifier shortcut (defaults to Ctrl on Win/Linux or Cmd on macOS)
M_COMBO('c', mood, duration_ms) // Ctrl + C

// Send a Windows/Super key shortcut (Win + Key)
M_COMBO_SUPER('d', mood, duration_ms) // Win + D

// Send a multi-modifier shortcut (Ctrl + Shift + Key)
M_COMBO_SHIFT('z', mood, duration_ms) // Ctrl + Shift + Z
```

### Examples

**Change Button 1 (Short Press) on Layer 2 to Lock System (`Win + L`):**
```cpp
// Change this:
M_COMBO('z', DEFAULT, 200)

// To this:
M_COMBO_SUPER('l', TIRED, 400)
```

**Set Modifier Key to Command for macOS:**
Change line 27:
```cpp
#define MODIFIER_KEY KEY_LEFT_GUI
```
