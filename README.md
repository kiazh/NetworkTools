# ESP32 Security Research Toolkit

Pocket 2.4 GHz research device built on an ESP32 DevKitC. Navigate four tools from an OLED menu using two buttons: WiFi scanning, deauthentication testing, BLE scanning, and live spectrum analysis via an NRF24L01+ radio module.

> **Legal:** Use only on networks and devices you own or have explicit written permission to test. Unauthorized deauthentication is illegal in Canada and most jurisdictions.

---

## How It Works

The firmware (`src/main.cpp`) is a single-file state machine. Five states exist: `MENU`, `WIFI_SCAN`, `WIFI_DEAUTH`, `BLE_SCAN`, `NRF_SPECTRUM`.

Every iteration of `loop()`:
1. Detects if the state changed.
2. Runs `on_mode_exit()` to cleanly tear down the previous state (frees scan results, powers down radios, deinits BLE).
3. Runs `on_mode_enter()` to initialize the new state.
4. Dispatches to the active state's handler.

Buttons use software debounce with a 220 ms threshold. The OLED renders to an internal buffer each frame and flushes with `display()`.

### WiFi Scan
Calls `WiFi.scanNetworks(true)` (async) so `loop()` stays responsive during the scan. Each iteration polls `WiFi.scanComplete()`. Once done, results are stored in the ESP32 WiFi driver and read via `WiFi.SSID(i)`, `WiFi.channel(i)`, `WiFi.RSSI(i)`. Displays 3 networks at a time; BTN_NEXT scrolls.

### WiFi Deauth
Same async scan as above. After the user selects a target AP, `deauth_send()` constructs a raw 802.11 deauthentication frame (frame control `0xC0`, reason code 7 — "class 3 frame from non-associated STA", destination broadcast `FF:FF:FF:FF:FF:FF`). The AP's BSSID is written into both the SA and BSSID fields at runtime. Transmission uses `esp_wifi_80211_tx()`, which requires promiscuous mode — this is enabled briefly around the send and disabled immediately after. 10 frames are sent per call with 150 µs gaps. This repeats continuously until BTN_SEL is pressed.

### BLE Scan
WiFi radio is disabled first to avoid coexistence conflicts. Runs a 3-second active BLE scan via the ESP32 Arduino BLE library. Each result shows the advertised device name (falls back to MAC address if unnamed) and RSSI. BLE is fully deinited on exit to free the radio.

### NRF Spectrum
Uses the NRF24L01's RPD (Received Power Detector) register — a 1-bit flag that reads `1` if any signal above −64 dBm was detected on the current channel in the last ~128 µs. The firmware sweeps all 126 channels (ch 0 = 2402 MHz … ch 125 = 2527 MHz): for each channel it sets the frequency, starts listening for 128 µs, stops, then reads RPD. A `uint8_t` accumulator per channel rises by 8 on a hit and falls by 8 on a miss, producing smooth decay. Channel pairs are averaged and drawn as 2-pixel-wide vertical bars across the 128-pixel OLED width. Three marker dots at the top indicate WiFi channels 1, 6, and 11.

---

## Hardware

| Component | Spec |
|-----------|------|
| ESP32 DevKitC | 38-pin, CP2102 USB-to-serial |
| SSD1306 OLED | 0.96", I2C, 128×64, I2C address 0x3C |
| NRF24L01+ | 3.3 V RF module — **NOT the 5 V variant** |
| 2× tactile pushbuttons | any momentary type |
| 1× 10 µF electrolytic capacitor | NRF24L01 power rail stabilisation |
| Breadboard + male-to-male jumper wires | |
| Micro-USB cable | to flash and power |

---

## Wiring

**Always connect GND first on every component before any signal wire.**

### OLED SSD1306 — I2C

The OLED communicates over I2C. SDA carries data; SCL carries the clock. The ESP32 hardware I2C peripheral is on GPIO 21/22.

```
ESP32 DevKitC          SSD1306 OLED
─────────────          ────────────
GND          ───────►  GND
3.3V         ───────►  VCC
GPIO 21      ───────►  SDA
GPIO 22      ───────►  SCL
```

> If the OLED stays blank, some modules ship with address 0x3D instead of 0x3C. Change the address in `setup()` → `oled.begin(SSD1306_SWITCHCAPVCC, 0x3D)`.

### NRF24L01+ — SPI

The NRF24L01 uses SPI for data and two GPIO lines (CE and CSN) for chip control. CE enables the radio; CSN is the SPI chip-select (active low).

```
ESP32 DevKitC          NRF24L01+
─────────────          ─────────
GND          ───────►  GND   (pin 1)
3.3V         ───────►  VCC   (pin 2)   ← 3.3V ONLY — 5V destroys the module
GPIO 4       ───────►  CE    (pin 3)
GPIO 5       ───────►  CSN   (pin 4)
GPIO 18      ───────►  SCK   (pin 5)
GPIO 23      ───────►  MOSI  (pin 6)
GPIO 19      ◄───────  MISO  (pin 7)
NC                     IRQ   (pin 8)   ← leave unconnected
```

**Capacitor:** Place the 10 µF electrolytic capacitor across the NRF24L01's VCC and GND pins, as close to the module as possible. Cheap clones have noisy power rails and `radio.begin()` returns `false` without it.

### Buttons

Each button connects between the GPIO and GND. No external resistors — the firmware enables internal pullups, so the pin reads HIGH at rest and LOW when pressed.

```
ESP32 DevKitC          Button
─────────────          ──────
GPIO 32      ───────►  BTN_NEXT terminal A
GND          ───────►  BTN_NEXT terminal B

GPIO 33      ───────►  BTN_SEL terminal A
GND          ───────►  BTN_SEL terminal B
```

| Button | GPIO | Function |
|--------|------|----------|
| BTN_NEXT | 32 | Scroll / move cursor down |
| BTN_SEL | 33 | Select / confirm / exit back to menu |

---

## Software Requirements

| Tool | Version | Install |
|------|---------|---------|
| Python | 3.8+ | [python.org](https://www.python.org/downloads/) |
| PlatformIO CLI | 6.x | `pip install platformio` |
| Git | any | [git-scm.com](https://git-scm.com/) |

**Linux only** — grant USB access after installing PlatformIO:
```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
  | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

---

## Build and Flash

```bash
git clone https://github.com/kiazh/NetworkTools.git
cd NetworkTools
pio run -t upload
```

PlatformIO downloads the toolchain, Arduino framework, and all libraries on first run (a few minutes). Subsequent builds are fast. The firmware uses the `huge_app` partition scheme (3 MB app region) — required to fit BLE + WiFi + NRF stacks simultaneously.

**Find your serial port if upload fails:**

| OS | Command | Example output |
|----|---------|----------------|
| macOS | `ls /dev/cu.*` | `/dev/cu.usbserial-0001` |
| Linux | `ls /dev/ttyUSB*` | `/dev/ttyUSB0` |
| Windows | Device Manager → Ports (COM & LPT) | `COM3` |

Set your port in `platformio.ini`:
```ini
upload_port = /dev/cu.usbserial-0001   ; macOS example
```

**Monitor serial output** (115200 baud):
```bash
pio device monitor
```

---

## Using the Device

Power on → 2-second splash screen → main menu.

```
> 1 WiFi Scan
  2 WiFi Deauth
  3 BLE Scan
  4 NRF Spectrum
```

| Button | In menu | Inside any mode |
|--------|---------|-----------------|
| BTN_NEXT | Move `>` cursor down | Scroll list |
| BTN_SEL | Enter selected mode | Exit back to menu |

### WiFi Scan
Async scan across all channels. Shows SSID (or `(hidden)` for hidden networks), channel number, and RSSI in dBm. BTN_NEXT scrolls 3 results at a time. BTN_SEL during scan cancels it; BTN_SEL on results returns to menu.

### WiFi Deauth
Scans for APs first. BTN_NEXT scrolls through discovered targets showing SSID, channel, and RSSI. BTN_SEL on a target starts continuous deauth — the screen shows `DEAUTHING:` and the target SSID. BTN_SEL again stops and returns to menu.
**Authorized networks only.**

### BLE Scan
Runs a 3-second passive BLE scan. Results show device name (or MAC address if the device has no name) and RSSI. BTN_NEXT scrolls. BTN_SEL exits and fully releases the BLE radio.

### NRF Spectrum
Live 2.4 GHz bar graph. The NRF24L01 sweeps all 126 channels continuously, each bar decaying smoothly when signal disappears. Three dots at the top of the display mark WiFi channels 1, 6, and 11 so you can spot WiFi congestion instantly. BTN_SEL exits and powers down the NRF radio.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| OLED blank after boot | Wrong I2C address or loose SDA/SCL wire | Check GPIO 21/22 connections; try address 0x3D in `setup()` |
| `NRF24 not found!` on screen | Module not initialising | Check all 6 SPI wires; add or replace 10 µF cap across VCC/GND at module; swap module |
| Upload fails: `port not found` | Wrong or missing port | Run port discovery command for your OS; set `upload_port` in `platformio.ini` |
| Upload fails: permission denied (Linux) | Missing udev rules | Run the udev commands in the Software Requirements section |
| BLE scan returns 0 devices | Radio busy from previous run | Exit to menu with BTN_SEL; re-enter BLE Scan |
| WiFi scan hangs on "Scanning..." | Interrupted mid-scan | Press BTN_SEL to cancel; re-enter WiFi Scan |
| Deauth has no effect | Target AP may require directed deauth (not broadcast) | This firmware sends broadcast deauth only |

---

## Simulate Without Hardware

Two Python scripts validate logic without any physical hardware:

```bash
# Simulates the full state machine: mode transitions, frame construction,
# spectrum accumulator logic, BLE result handling
python3 sim/simulate_toolkit.py

# Static analysis: parses src/main.cpp and flags known ESP32 pitfalls
python3 sim/diagnostics.py
```

Both exit with code `0` on pass and `1` on failure — usable in CI pipelines.

---

## Libraries

| Library | Purpose |
|---------|---------|
| Adafruit SSD1306 | OLED display driver |
| Adafruit GFX | Graphics primitives (text, lines, pixels) |
| nrf24/RF24 | NRF24L01+ SPI driver |
| ESP32 BLE Arduino | BLE scan (built into ESP32 Arduino framework) |
| ESP32 WiFi | WiFi scan + raw 802.11 TX (built into ESP32 Arduino framework) |

---

## License

Educational and research use only. See `LICENSE` for terms.
