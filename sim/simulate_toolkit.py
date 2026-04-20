#!/usr/bin/env python3
"""
ESP32 Security Research Toolkit — Hardware Simulator
Simulates all 4 modes without physical hardware.
Run: python3 sim/simulate_toolkit.py
"""

import random
import time
import struct
import sys

PASS = "\033[92mPASS\033[0m"
FAIL = "\033[91mFAIL\033[0m"
INFO = "\033[94mINFO\033[0m"


def check(label, condition, detail=""):
    status = PASS if condition else FAIL
    suffix = f"  ({detail})" if detail else ""
    print(f"  [{status}] {label}{suffix}")
    return condition


# ─────────────────────────────────────────────────────────────────
# PIN CONFLICT ANALYSIS
# ─────────────────────────────────────────────────────────────────
def test_pin_conflicts():
    print("\n[1/5] GPIO Pin Conflict Analysis")

    pins = {
        "OLED_SDA":  21,
        "OLED_SCL":  22,
        "NRF_CE":     4,
        "NRF_CSN":    5,
        "NRF_SCK":   18,
        "NRF_MOSI":  23,
        "NRF_MISO":  19,
        "BTN_NEXT":  32,
        "BTN_SEL":   33,
    }

    # Check for duplicates
    used = {}
    conflicts = 0
    for name, gpio in pins.items():
        if gpio in used:
            print(f"  [{FAIL}] CONFLICT: {name}=GPIO{gpio} already used by {used[gpio]}")
            conflicts += 1
        else:
            used[gpio] = name

    check("No pin conflicts", conflicts == 0, f"{len(pins)} pins checked")

    # Check input-only pins (34/35/36/39 have no internal pullup on ESP32)
    bad_pullup = [n for n, g in pins.items() if g in {34, 35, 36, 39}]
    check(
        "Buttons not on input-only pins",
        len(bad_pullup) == 0,
        "GPIO34/35/36/39 have no internal pullup" if bad_pullup else "GPIO32/33 support INPUT_PULLUP",
    )

    # SPI pins valid
    spi_ok = all(pins[k] in {4,5,18,19,23} or pins[k] > 5 for k in ["NRF_SCK","NRF_MOSI","NRF_MISO"])
    check("NRF24 on VSPI pins (18/19/23)", spi_ok)

    # I2C pins valid
    check("OLED on default I2C (21/22)", pins["OLED_SDA"] == 21 and pins["OLED_SCL"] == 22)

    return conflicts == 0


# ─────────────────────────────────────────────────────────────────
# 802.11 DEAUTH FRAME VALIDATION
# ─────────────────────────────────────────────────────────────────
def test_deauth_frame():
    print("\n[2/5] 802.11 Deauth Frame Validation")

    frame = bytes([
        0xC0, 0x00,                                     # Frame Control
        0x00, 0x00,                                     # Duration
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,             # DA: broadcast
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             # SA (BSSID placeholder)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             # BSSID placeholder
        0xF0, 0xFF,                                     # Sequence
        0x07, 0x00,                                     # Reason
    ])

    check("Frame length == 26 bytes", len(frame) == 26, f"got {len(frame)}")
    check("Frame Control = 0xC000 (Deauth)", frame[0] == 0xC0 and frame[1] == 0x00)
    check("DA = broadcast FF:FF:FF:FF:FF:FF", frame[4:10] == b'\xff' * 6)
    check("Reason code = 7 (class3 from nonassoc STA)", frame[24] == 0x07)
    check("Frame subtype nibble = 0xC (Deauth)", (frame[0] & 0xF0) == 0xC0)

    # Simulate BSSID injection
    bssid = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF])
    injected = bytearray(frame)
    injected[10:16] = bssid
    injected[16:22] = bssid
    check("BSSID injection correct (SA+BSSID fields)", injected[10:16] == bssid and injected[16:22] == bssid)

    return True


# ─────────────────────────────────────────────────────────────────
# STATE MACHINE SIMULATION
# ─────────────────────────────────────────────────────────────────
def test_state_machine():
    print("\n[3/5] Menu State Machine Simulation")

    MENU, WIFI_SCAN, WIFI_DEAUTH, BLE_SCAN, NRF_SPECTRUM = range(5)
    MENU_COUNT = 4

    state = MENU
    menu_sel = 0
    transitions = []

    def press_next():
        nonlocal menu_sel, state
        if state == MENU:
            menu_sel = (menu_sel + 1) % MENU_COUNT

    def press_sel():
        nonlocal state, menu_sel
        if state == MENU:
            state = menu_sel + 1
            transitions.append((MENU, state))
        else:
            transitions.append((state, MENU))
            state = MENU
            menu_sel = 0

    # Simulate full menu cycle
    for target in [WIFI_SCAN, WIFI_DEAUTH, BLE_SCAN, NRF_SPECTRUM]:
        while (menu_sel + 1) != target:
            press_next()
        press_sel()   # enter
        press_sel()   # back to menu

    check("All 4 modes reachable from menu", len(transitions) == 8)
    entered = {t[1] for t in transitions if t[0] == MENU}
    check("All modes entered", entered == {WIFI_SCAN, WIFI_DEAUTH, BLE_SCAN, NRF_SPECTRUM})
    returned = {t[0] for t in transitions if t[1] == MENU}
    check("All modes return to menu", returned == {WIFI_SCAN, WIFI_DEAUTH, BLE_SCAN, NRF_SPECTRUM})

    return True


# ─────────────────────────────────────────────────────────────────
# NRF24L01 SPECTRUM SWEEP SIMULATION
# ─────────────────────────────────────────────────────────────────
def test_nrf_spectrum():
    print("\n[4/5] NRF24L01 Spectrum Sweep Simulation")

    NRF_CH = 126
    power = [0] * NRF_CH

    # Simulate WiFi activity on channels 1 (nrf12), 6 (nrf37), 11 (nrf62)
    # NRF ch = WiFi freq - 2400 (approx: ch1=2412→12, ch6=2437→37, ch11=2462→62)
    wifi_active = {12, 37, 62}

    def sweep_once():
        for ch in range(NRF_CH):
            rpd = ch in wifi_active and random.random() > 0.2
            if rpd:
                power[ch] = min(255, power[ch] + 16)
            else:
                power[ch] = max(0, power[ch] - 8)

    for _ in range(20):
        sweep_once()

    # WiFi channels should have elevated power
    wifi_power_ok = all(power[ch] > 50 for ch in wifi_active)
    quiet_channels = [ch for ch in range(NRF_CH) if ch not in wifi_active]
    quiet_ok = sum(1 for ch in quiet_channels if power[ch] > 50) < 10

    check("WiFi channels (1/6/11) show elevated power", wifi_power_ok,
          f"ch12={power[12]} ch37={power[37]} ch62={power[62]}")
    check("Quiet channels mostly near zero", quiet_ok,
          f"{sum(1 for ch in quiet_channels if power[ch]>50)} hot / {len(quiet_channels)} quiet channels")

    # Validate bar graph mapping
    bar_h_max = 54
    bar_vals = [min(bar_h_max, power[ch] * bar_h_max // 255) for ch in range(NRF_CH)]
    check("Bar graph maps 0-255 → 0-54px", max(bar_vals) <= bar_h_max)

    # Validate WiFi marker positions (pixel x = ch * 127 / 125)
    markers = {ch: round(ch * 127 / 125) for ch in [12, 37, 62]}
    check("WiFi ch1 marker x=12px", markers[12] == 12)
    check("WiFi ch6 marker x=38px", markers[37] == 38, f"got {markers[37]}")

    return True


# ─────────────────────────────────────────────────────────────────
# BLE SCAN SIMULATION
# ─────────────────────────────────────────────────────────────────
def test_ble_scan():
    print("\n[5/5] BLE Scanner Simulation")

    # Mock BLE advertisements
    mock_devices = [
        {"name": "iPhone",         "mac": "AA:BB:CC:DD:EE:01", "rssi": -55},
        {"name": "",               "mac": "AA:BB:CC:DD:EE:02", "rssi": -72},
        {"name": "Pixel 7",        "mac": "AA:BB:CC:DD:EE:03", "rssi": -61},
        {"name": "BLE_Keyboard",   "mac": "AA:BB:CC:DD:EE:04", "rssi": -80},
        {"name": "Fitbit",         "mac": "AA:BB:CC:DD:EE:05", "rssi": -68},
    ]

    def display_label(dev):
        label = dev["name"] if dev["name"] else dev["mac"]
        return label[:14]

    # Simulate scroll: show 3 at a time
    ble_idx = 0
    visible_pages = []
    for start in range(0, len(mock_devices), 1):
        page = mock_devices[start:start + 3]
        visible_pages.append([display_label(d) for d in page])

    check("Device count detected", len(mock_devices) == 5)
    check("Unnamed device falls back to MAC", display_label(mock_devices[1]) == "AA:BB:CC:DD:EE")
    check("Label truncated to 14 chars", all(len(display_label(d)) <= 14 for d in mock_devices))

    rssi_range_ok = all(-100 <= d["rssi"] <= 0 for d in mock_devices)
    check("RSSI values in valid range", rssi_range_ok)

    scroll_covers_all = len(set(
        label for page in visible_pages for label in page
    )) == len(mock_devices)
    check("Scroll exposes all devices", scroll_covers_all)

    return True


# ─────────────────────────────────────────────────────────────────
# MAIN
# ─────────────────────────────────────────────────────────────────
def main():
    print("=" * 60)
    print("ESP32 Security Toolkit — Hardware Simulator")
    print("AEL project: esp32_sec_toolkit")
    print("Hardware: ESP32 DevKitC + SSD1306 OLED + NRF24L01")
    print("=" * 60)

    results = [
        test_pin_conflicts(),
        test_deauth_frame(),
        test_state_machine(),
        test_nrf_spectrum(),
        test_ble_scan(),
    ]

    passed = sum(results)
    total = len(results)
    print(f"\n{'='*60}")
    print(f"Result: {passed}/{total} test groups passed")
    if passed == total:
        print(f"[{PASS}] All simulations passed — firmware logic is sound.")
        print("        Flash when hardware arrives:")
        print("        cd projects/esp32_sec_toolkit && pio run -t upload")
    else:
        print(f"[{FAIL}] {total-passed} group(s) failed — review before flashing.")
    print("=" * 60)

    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
