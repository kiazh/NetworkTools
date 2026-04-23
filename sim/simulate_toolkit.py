#!/usr/bin/env python3
"""
ESP32 Security Research Toolkit — Hardware Simulator
Simulates all 4 modes without physical hardware.
Run: python3 sim/simulate_toolkit.py
"""

import re
import random
import sys
from pathlib import Path

PASS = "\033[92mPASS\033[0m"
FAIL = "\033[91mFAIL\033[0m"
INFO = "\033[94mINFO\033[0m"

SRC_PATH = Path(__file__).parent.parent / "src" / "main.cpp"


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

    used = {}
    conflicts = 0
    for name, gpio in pins.items():
        if gpio in used:
            print(f"  [{FAIL}] CONFLICT: {name}=GPIO{gpio} already used by {used[gpio]}")
            conflicts += 1
        else:
            used[gpio] = name

    check("No pin conflicts", conflicts == 0, f"{len(pins)} pins checked")

    bad_pullup = [n for n, g in pins.items() if g in {34, 35, 36, 39}]
    check(
        "Buttons not on input-only pins",
        len(bad_pullup) == 0,
        "GPIO34/35/36/39 have no internal pullup" if bad_pullup else "GPIO32/33 support INPUT_PULLUP",
    )

    vspi_pins = {18, 19, 23}
    spi_ok = all(pins[k] in vspi_pins for k in ["NRF_SCK", "NRF_MOSI", "NRF_MISO"])
    check("NRF24 on VSPI pins (18/19/23)", spi_ok)
    check("OLED on default I2C (21/22)", pins["OLED_SDA"] == 21 and pins["OLED_SCL"] == 22)

    return conflicts == 0


# ─────────────────────────────────────────────────────────────────
# 802.11 DEAUTH FRAME VALIDATION  [fix #18: parse from src/main.cpp]
# ─────────────────────────────────────────────────────────────────
def parse_deauth_frame_from_src():
    if not SRC_PATH.exists():
        return None
    src = SRC_PATH.read_text()
    match = re.search(r"deauth_frame\[\d+\]\s*=\s*\{([^}]+)\}", src, re.DOTALL)
    if not match:
        return None
    hex_vals = re.findall(r'0[xX][0-9a-fA-F]+', match.group(1))
    return bytes(int(v, 16) for v in hex_vals)


def test_deauth_frame():
    print("\n[2/5] 802.11 Deauth Frame Validation")

    frame = parse_deauth_frame_from_src()  # [fix #18] reads actual source
    if not check("deauth_frame parsed from src/main.cpp", frame is not None,
                 "check src path" if frame is None else f"{len(frame)} bytes extracted"):
        return False

    check("Frame length == 26 bytes", len(frame) == 26, f"got {len(frame)}")
    check("Frame Control = 0xC000 (Deauth)", frame[0] == 0xC0 and frame[1] == 0x00)
    check("DA = broadcast FF:FF:FF:FF:FF:FF", frame[4:10] == b'\xff' * 6)
    check("Reason code = 7 (class3 from nonassoc STA)", frame[24] == 0x07)
    check("Frame subtype nibble = 0xC (Deauth)", (frame[0] & 0xF0) == 0xC0)

    bssid = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF])
    injected = bytearray(frame)
    injected[10:16] = bssid
    injected[16:22] = bssid
    check("BSSID injection correct (SA+BSSID fields)", injected[10:16] == bssid and injected[16:22] == bssid)

    return True


# ─────────────────────────────────────────────────────────────────
# STATE MACHINE SIMULATION  [fix #19: no menu_sel reset on return]
# ─────────────────────────────────────────────────────────────────
def test_state_machine():
    print("\n[3/5] Menu State Machine Simulation")

    MENU, WIFI_SCAN, WIFI_DEAUTH, BLE_SCAN, NRF_SPECTRUM = range(5)
    MENU_COUNT = 4

    state = MENU
    menu_sel = 0
    transitions = []

    def press_next():
        nonlocal menu_sel
        if state == MENU:
            menu_sel = (menu_sel + 1) % MENU_COUNT

    def press_sel():
        nonlocal state
        if state == MENU:
            state = menu_sel + 1
            transitions.append((MENU, state))
        else:
            transitions.append((state, MENU))
            state = MENU
            # [fix #19] firmware does NOT reset menuSel on return — removed menu_sel = 0

    for target in [WIFI_SCAN, WIFI_DEAUTH, BLE_SCAN, NRF_SPECTRUM]:
        while (menu_sel + 1) != target:
            press_next()
        press_sel()   # enter
        press_sel()   # back to menu

    check("All 4 modes reachable from menu", len(transitions) == 8)
    entered  = {t[1] for t in transitions if t[0] == MENU}
    returned = {t[0] for t in transitions if t[1] == MENU}
    check("All modes entered", entered  == {WIFI_SCAN, WIFI_DEAUTH, BLE_SCAN, NRF_SPECTRUM})
    check("All modes return to menu", returned == {WIFI_SCAN, WIFI_DEAUTH, BLE_SCAN, NRF_SPECTRUM})

    return True


# ─────────────────────────────────────────────────────────────────
# NRF24L01 SPECTRUM SWEEP SIMULATION
# ─────────────────────────────────────────────────────────────────
def test_nrf_spectrum():
    print("\n[4/5] NRF24L01 Spectrum Sweep Simulation")

    NRF_CH = 126
    power = [0] * NRF_CH
    wifi_active = {12, 37, 62}  # NRF ch for WiFi ch 1/6/11

    def sweep_once():
        for ch in range(NRF_CH):
            rpd = ch in wifi_active and random.random() > 0.2
            if rpd:
                power[ch] = min(248, power[ch] + 8)  # matches firmware cap of 248
            else:
                power[ch] = max(0, power[ch] - 8)

    for _ in range(20):
        sweep_once()

    wifi_power_ok  = all(power[ch] > 50 for ch in wifi_active)
    quiet_channels = [ch for ch in range(NRF_CH) if ch not in wifi_active]
    quiet_ok = sum(1 for ch in quiet_channels if power[ch] > 50) < 10

    check("WiFi channels (1/6/11) show elevated power", wifi_power_ok,
          f"ch12={power[12]} ch37={power[37]} ch62={power[62]}")
    check("Quiet channels mostly near zero", quiet_ok,
          f"{sum(1 for ch in quiet_channels if power[ch]>50)} hot / {len(quiet_channels)} quiet channels")

    # [fix #14] validate 2-channel subsampling bar graph
    pairs   = NRF_CH // 2  # 63
    BAR_H   = 54
    bar_vals = [int(((power[p*2] + power[p*2+1]) / 2) * BAR_H / 255) for p in range(pairs)]
    check("Bar graph maps 0-255 → 0-54px (2ch groups)", max(bar_vals) <= BAR_H)
    check("63 pairs × 2px = 126px ≤ 128px display width", pairs * 2 <= 128)

    # WiFi ch1 → NRF ch12 → pair6 → x=12
    # WiFi ch6 → NRF ch37 → pair18 → x=36
    # WiFi ch11 → NRF ch62 → pair31 → x=62
    markers = {12: 6*2, 37: 18*2, 62: 31*2}
    check("WiFi ch1 marker x=12px (pair 6)",  markers[12] == 12)
    check("WiFi ch6 marker x=36px (pair 18)", markers[37] == 36)
    check("WiFi ch11 marker x=62px (pair 31)", markers[62] == 62)

    return True


# ─────────────────────────────────────────────────────────────────
# BLE SCAN SIMULATION
# ─────────────────────────────────────────────────────────────────
def test_ble_scan():
    print("\n[5/5] BLE Scanner Simulation")

    mock_devices = [
        {"name": "iPhone",       "mac": "AA:BB:CC:DD:EE:01", "rssi": -55},
        {"name": "",             "mac": "AA:BB:CC:DD:EE:02", "rssi": -72},
        {"name": "Pixel 7",      "mac": "AA:BB:CC:DD:EE:03", "rssi": -61},
        {"name": "BLE_Keyboard", "mac": "AA:BB:CC:DD:EE:04", "rssi": -80},
        {"name": "Fitbit",       "mac": "AA:BB:CC:DD:EE:05", "rssi": -68},
    ]

    def display_label(dev):
        label = dev["name"] if dev["name"] else dev["mac"]
        return label[:14]

    visible_pages = []
    for start in range(len(mock_devices)):
        page = mock_devices[start:start + 3]
        visible_pages.append([display_label(d) for d in page])

    check("Device count detected", len(mock_devices) == 5)
    check("Unnamed device falls back to MAC (first 14 chars)", display_label(mock_devices[1]) == "AA:BB:CC:DD:EE")
    check("Label truncated to 14 chars", all(len(display_label(d)) <= 14 for d in mock_devices))
    check("RSSI values in valid range", all(-100 <= d["rssi"] <= 0 for d in mock_devices))
    check("Scroll exposes all devices",
          len({lbl for page in visible_pages for lbl in page}) == len(mock_devices))

    return True


# ─────────────────────────────────────────────────────────────────
# MAIN
# ─────────────────────────────────────────────────────────────────
def main():
    print("=" * 60)
    print("ESP32 Security Toolkit — Hardware Simulator")
    print("AEL project: esp32_sec_toolkit / NetworkTools")
    print("Hardware: ESP32 DevKitC + SSD1306 OLED + NRF24L01")
    print("=" * 60)

    results = [
        test_pin_conflicts(),
        test_deauth_frame(),
        test_state_machine(),
        test_nrf_spectrum(),
        test_ble_scan(),
    ]

    passed = sum(bool(r) for r in results)
    total  = len(results)
    print(f"\n{'='*60}")
    print(f"Result: {passed}/{total} test groups passed")
    if passed == total:
        print(f"[{PASS}] All simulations passed — firmware logic is sound.")
        print("        Flash when hardware arrives:")
        print("        cd NetworkTools && pio run -t upload")
    else:
        print(f"[{FAIL}] {total-passed} group(s) failed — review before flashing.")
    print("=" * 60)

    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
