# BroCommand - ESP8266 ARGB Controller

BroCommand is a custom ESP8266-based ARGB lighting controller designed for PC builds that do not have motherboard ARGB support. It controls WS2812 addressable LEDs with multiple lighting effects and a full web-based UI control panel.

It is lightweight, fast, and fully DIY friendly.

---

## Features

### Lighting Modes
- Solid Mode
  - Controls all LEDs with a single color
  - Adjustable brightness

- Chaser Mode
  - Runs animation on fan LEDs only (first 6 LEDs)
  - Two sub modes:
    - Single color chase
    - Dual color half-ring chase
  - Direction control (CW / CCW)
  - Adjustable speed and brightness

- Mirage Mode
  - Strobe / flicker effect for full LED strip
  - Adjustable frequency control
  - Works on all LEDs

---

## Web UI Features

Access the control panel using:
http://192.168.10.50

UI includes:
- Mode selection (Solid, Chaser, Mirage)
- Color picker for each mode
- Brightness slider
- Speed control for chaser animation
- Direction toggle (CW / CCW)
- Dual / Single chaser mode switch
- Mirage frequency control
- Real-time updates without page refresh
- Save configuration button

---

## Hardware Requirements

- ESP-01 / ESP8266 module
- WS2812 ARGB LED strip
- 5V external power supply (recommended)
- Common ground connection

---

## Wiring Instructions

### ESP8266 (ESP-01)

GPIO2 (D4) -> Data In (DIN) of WS2812 strip  
GND -> LED power supply GND  
VCC -> 3.3V (ESP only)

---

### WS2812 LED Strip

DIN -> GPIO2 (ESP-01)  
+5V -> External 5V power supply  
GND -> Shared ground with ESP8266

---

## Important Notes

- Always connect ESP and LED power GND together
- Do NOT power LEDs from ESP 3.3V pin
- Use external 5V power supply for LEDs
- Recommended: 330Ω resistor on data line
- Recommended: 1000µF capacitor across 5V and GND

---

## Endpoints

/        -> Web UI  
/set     -> Live control updates  
/state   -> JSON status data  
/save    -> Save configuration to EEPROM  

---

## Configuration Storage

Stored in EEPROM:
- Selected mode
- Colors
- Brightness values
- Animation speed
- Direction
- Mirage frequency

Settings persist after reboot.

---

## Use Cases

- PC ARGB lighting without motherboard support
- DIY gaming setups
- Custom desk lighting
- ESP8266 smart lighting projects

---

## Author

Developed by: Saimum (BroCommand)  
Email: saimumhassan26@gmail.com

---

## License

© 2025 Saimum. All rights reserved.
Unauthorized copying or commercial use is not allowed without permission.
