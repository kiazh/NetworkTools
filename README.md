# ESP32 Security Research Toolkit

ESP32-based 2.4 GHz security research device. WiFi scanning, deauthentication testing, BLE scanning, and live spectrum analysis — all from a pocket device with an OLED menu.

> **Legal:** Use only on networks and devices you own or have explicit written permission to test. Unauthorized deauthentication is illegal in Canada and most jurisdictions.

---

## Prerequisites

### Hardware

| Component | Details |
|-----------|---------|
| ESP32 DevKitC | 38-pin, CP2102 USB-to-serial chip |
| SSD1306 OLED | 0.96", I2C, 128×64, white, address 0x3C |
| NRF24L01+ | 3.3 V RF module — **NOT the 5 V variant** |
| Tactile buttons | 2× momentary pushbutton (any size) |
| Capacitor | 1× 10 µF electrolytic |
| Breadboard + jumper wires | Male-to-male |
| USB cable | Micro-USB or USB-C (depends on your DevKitC revision) |

### Software

| Tool | Version | Install |
|------|---------|---------|
| Python | 3.8+ | [python.org](https://www.python.org/downloads/) |
| PlatformIO CLI | 6.x | `pip install platformio` |
| Git | any | [git-scm.com](https://git-scm.com/) |

**macOS** — install Homebrew first, then:
```bash
brew install python git
pip install platformio
```

**Windows** — install Python from python.org (check "Add to PATH"), then:
```cmd
pip install platformio
```

**Linux (Debian/Ubuntu)**:
```bash
sudo apt install python3 python3-pip git
pip3 install platformio
```

After installing PlatformIO, add udev rules on Linux so it can access USB:
```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
  | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

---

## Get the Code

```bash
git clone https://github.com/kiazh/NetworkTools.git
cd NetworkTools
```

---

## Wiring

Wire GND first on every component before connecting signal lines.

### OLED SSD1306 (I2C, address 0x3C)

| OLED pin | ESP32 pin |
|----------|----------|
| GND | GND |
| VCC | 3.3 V |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

### NRF24L01+ (SPI)

| NRF24L01 pin | ESP32 pin | Note |
|--------------|----------|------|
| GND | GND | Wire first |
| VCC | 3.3 V | **NOT 5 V — will destroy the module** |
| CE | GPIO 4 | |
| CSN | GPIO 5 | |
| SCK | GPIO 18 | |
| MOSI | GPIO 23 | |
| MISO | GPIO 19 | |
| IRQ | NC | Leave unconnected |

> Put the 10 µF capacitor across the NRF24L01 VCC and GND pins (as close to the module as possible). Cheap NRF24L01 clones brown out without it and `radio.begin()` returns false.

### Buttons

Each button connects between the GPIO pin and GND. No resistors needed — internal pullups are enabled in firmware.

| Button | Function | ESP32 pin |
|--------|---------|----------|
| BTN_NEXT | Scroll / next item | GPIO 32 |
| BTN_SEL | Select / back | GPIO 33 |

---

## Flash

```bash
# Inside the NetworkTools directory:
pio run -t upload
```

PlatformIO downloads all dependencies automatically on first run (toolchain, Arduino framework, libraries). This takes a few minutes the first time.

**Find your port** if upload fails:

| OS | Command | Example result |
|----|---------|---------------|
| macOS | `ls /dev/cu.*` | `/dev/cu.usbserial-0001` |
| Linux | `ls /dev/ttyUSB*` | `/dev/ttyUSB0` |
| Windows | Device Manager → Ports (COM & LPT) | `COM3` |

Set it in `platformio.ini`:
```ini
upload_port = /dev/cu.usbserial-0001   ; macOS
; upload_port = /dev/ttyUSB0           ; Linux
; upload_port = COM3                   ; Windows
```

**Monitor serial output** (115200 baud):
```bash
pio device monitor
```

---

## Usage

Power on → 2-second splash → main menu.

```
> 1 WiFi Scan
  2 WiFi Deauth
  3 BLE Scan
  4 NRF Spectrum
```

| Button | In menu | In mode |
|--------|---------|---------|
| BTN_NEXT | Move cursor down | Scroll list |
| BTN_SEL | Enter selected mode | Exit back to menu |

### WiFi Scan
Scans all channels. Shows SSID (or `(hidden)` for hidden networks), channel number, and RSSI. BTN_NEXT scrolls, BTN_SEL exits.

### WiFi Deauth
Scans for APs. BTN_NEXT scrolls targets. BTN_SEL starts deauth — holds BTN_SEL again to stop and return to menu.
**Use only on your own AP or with explicit written permission.**

### BLE Scan
3-second passive scan. Displays device name (falls back to MAC address if unnamed) and RSSI. BTN_NEXT scrolls through results.

### NRF Spectrum
Live 2.4 GHz bar graph across all 126 NRF24L01 channels. Three dots at the top mark WiFi channels 1, 6, and 11. Updates continuously. BTN_SEL exits.

---

## First-Boot Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| OLED blank | Wrong I2C address or loose wire | Check SDA/SCL wiring; some modules use 0x3D — edit `0x3C` in `setup()` |
| "NRF24 not found!" on screen | Module not initialising | Add/replace 10 µF cap; check all 6 SPI wires; try swapping module |
| Upload fails: "port not found" | Wrong port | Set `upload_port` in `platformio.ini` |
| Upload fails: permission denied (Linux) | Missing udev rules | Run the udev commands in Prerequisites |
| BLE scan returns 0 devices | Radio busy | Exit and re-enter BLE Scan mode once |
| WiFi scan hangs | Cancelled mid-scan | Return to menu with BTN_SEL; re-enter WiFi Scan |

---

## Simulate Without Hardware

Run logic validation without any hardware connected:

```bash
# Full hardware simulator (state machine, frame validation, spectrum, BLE)
python3 sim/simulate_toolkit.py

# Static firmware diagnostics (parses src/main.cpp for known ESP32 pitfalls)
python3 sim/diagnostics.py
```

Both scripts exit with code 0 on success, 1 on failure — usable in CI.

---

## Libraries Used

| Library | Purpose |
|---------|---------|
| Adafruit SSD1306 | OLED display driver |
| Adafruit GFX | Graphics primitives |
| nrf24/RF24 | NRF24L01 SPI driver |
| ESP32 BLE Arduino | BLE scan (built into framework) |
| ESP32 WiFi | WiFi scan + raw 802.11 TX (built into framework) |

---

## License

For educational and research purposes. See `LICENSE` for terms.
