# ESP32 Security Research Toolkit

ESP32-based 2.4 GHz security research device for WiFi scanning, deauthentication testing, BLE scanning, and spectrum analysis.

**Legal:** Use only on networks and devices you own or have explicit written permission to test.

---

## Hardware Requirements

| Component | Part |
|-----------|------|
| Microcontroller | ESP32 DevKitC (38-pin, CP2102 USB) |
| Display | 0.96" SSD1306 OLED, I2C, 128×64 white |
| RF module | NRF24L01+ (3.3 V) |
| Buttons | 2× tactile pushbutton |
| Capacitor | 10 µF electrolytic (NRF24L01 VCC decoupling) |

---

## Wiring

### OLED SSD1306 (I2C address 0x3C)

| OLED pin | ESP32 GPIO |
|----------|-----------|
| VCC | 3.3 V |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

### NRF24L01+ (SPI)

| NRF24L01 pin | ESP32 GPIO |
|--------------|-----------|
| VCC | 3.3 V — **NOT 5 V** |
| GND | GND |
| CE | GPIO 4 |
| CSN | GPIO 5 |
| SCK | GPIO 18 |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| IRQ | NC |

> Add a 10 µF capacitor between VCC and GND at the NRF24L01 module to prevent voltage drops during TX.

### Buttons (active-low, internal pullup)

| Button | ESP32 GPIO |
|--------|-----------|
| BTN_NEXT | GPIO 32 |
| BTN_SEL | GPIO 33 |

Connect each button between the GPIO pin and GND.

---

## Features

| Menu item | Function |
|-----------|----------|
| WiFi Scan | Lists nearby APs with SSID, channel, RSSI. NEXT scrolls, SEL exits. |
| WiFi Deauth | Sends 802.11 deauth frames to a selected AP. **Authorized networks only.** |
| BLE Scan | 3-second passive BLE scan. Lists device name (or MAC) and RSSI. |
| NRF Spectrum | Live 2.4 GHz spectrum bar graph. WiFi channels 1, 6, 11 marked. |

---

## Flash with PlatformIO

```bash
# Install PlatformIO CLI if needed
pip install platformio

# Build and upload
cd NetworkTools
pio run -t upload

# Monitor serial output
pio device monitor
```

> Set your port in `platformio.ini` if auto-detect fails:
> ```ini
> upload_port = /dev/cu.usbserial-XXXX   # macOS
> upload_port = COM3                       # Windows
> ```

---

## Run Simulator (no hardware needed)

```bash
python3 sim/simulate_toolkit.py
```

Simulates all 4 modes, validates deauth frame structure against the actual source, checks GPIO conflicts, and verifies state machine transitions.

## Run Static Diagnostics

```bash
python3 sim/diagnostics.py
```

Parses `src/main.cpp` and checks for known ESP32 pitfalls: promiscuous mode, async scan, BLE/WiFi coexistence, per-button debounce, NRF subsampling, and more.

---

## AEL Integration

This project lives in `ai-embedded-lab/projects/esp32_sec_toolkit/NetworkTools/`.
CE audit on file in `sim/diagnostics.py` output. Hardware validation pending until parts arrive.
