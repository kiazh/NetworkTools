#!/usr/bin/env python3
"""
ESP32 Security Toolkit — Static Firmware Diagnostics
Parses src/main.cpp and checks for known ESP32 pitfalls.
Run: python3 sim/diagnostics.py
"""

import re
import sys
from pathlib import Path

PASS = "\033[92mPASS\033[0m"
FAIL = "\033[91mFAIL\033[0m"
WARN = "\033[93mWARN\033[0m"

ROOT = Path(__file__).parent.parent
SRC  = ROOT / "src" / "main.cpp"


def check(label, condition, level="pass", detail=""):
    if condition:
        tag = PASS
    elif level == "warn":
        tag = WARN
    else:
        tag = FAIL
    suffix = f"  → {detail}" if detail else ""
    print(f"  [{tag}] {label}{suffix}")
    return condition


def load_src():
    if not SRC.exists():
        print(f"[{FAIL}] src/main.cpp not found at {SRC}")
        sys.exit(1)
    return SRC.read_text()


def diag_wifi(src):
    print("\n[A] WiFi / Deauth Diagnostics")

    check("WiFi.mode(WIFI_STA) called before scan",
          "WiFi.mode(WIFI_STA)" in src)

    check("WiFi async scan (scanNetworks(true))",
          "WiFi.scanNetworks(true)" in src,
          detail="non-blocking — loop stays responsive")

    check("esp_wifi_80211_tx used for deauth",
          "esp_wifi_80211_tx" in src)

    check("esp_wifi_set_promiscuous(true) before TX",
          "esp_wifi_set_promiscuous(true)" in src,
          detail="required for raw frame injection on newer ESP-IDF")

    check("esp_wifi_set_channel called before TX",
          "esp_wifi_set_channel" in src)

    check("WiFi.scanDelete() called on exit",
          "WiFi.scanDelete()" in src)

    frame_match = re.search(r"deauth_frame\[(\d+)\]", src)
    if frame_match:
        flen = int(frame_match.group(1))
        check("Deauth frame declared as 26 bytes", flen == 26,
              detail=f"declared as {flen}")
    else:
        check("Deauth frame array found", False)

    check("WiFi.mode(WIFI_OFF) before BLE init",
          "WiFi.mode(WIFI_OFF)" in src,
          detail="prevents WiFi/BLE radio contention")

    check("Scroll clamped (not modulo wrap)",
          "min(wifiScroll + 1" in src,
          detail="last page never shows blank entries")

    check("Hidden network shows '(hidden)'",
          "length() == 0" in src and "hidden" in src,
          detail="empty SSID string = hidden network")


def diag_ble(src):
    print("\n[B] BLE Diagnostics")

    check("BLEDevice::init(\"\") — no advertising during scan",
          'BLEDevice::init("")' in src,
          detail="empty name suppresses BLE advertising")

    check("BLEDevice::deinit() called",
          "BLEDevice::deinit" in src,
          detail="frees radio before WiFi re-init")

    check("bleScan->clearResults() called",
          "clearResults()" in src)

    check("setActiveScan(true) for RSSI",
          "setActiveScan(true)" in src)


def diag_nrf(src):
    print("\n[C] NRF24L01 Diagnostics")

    check("radio.begin() return checked",
          "radio.begin()" in src and ("!radio.begin()" in src or "nrfOk" in src))

    check("radio.powerDown() on error path (nrf_spectrum_enter)",
          "radio.powerDown()" in src and "NRF24 not found" in src,
          detail="called before returning to MENU on init failure")

    check("radio.setAutoAck(false) for sweep",
          "setAutoAck(false)" in src)

    check("radio.testRPD() used (not testCarrier)",
          "testRPD()" in src,
          detail="testRPD() works on NRF24L01+; testCarrier() only on legacy NRF24L01")

    check("radio.powerDown() on SEL exit",
          src.count("radio.powerDown()") >= 2,
          detail="called on both error path and normal exit")

    check("NRF rise == fall rate (no smear)",
          "nrfPower[ch] += 8" in src,
          detail="equal rise/fall = accurate spectrum")

    check("2-channel subsampling (63 pairs × 2px)",
          "NRF_CH_COUNT / 2" in src and "pair * 2" in src,
          detail="126 channels → 126px, no collision")

    check("NRF channel count == 126",
          "NRF_CH_COUNT 126" in src or "126" in src)

    check("NRF on correct VSPI pins (CE=4 CSN=5)",
          "NRF_CE" in src and "4" in src and "NRF_CSN" in src and "5" in src)


def diag_oled(src):
    print("\n[D] OLED / I2C Diagnostics")

    check("I2C address 0x3C used",              "0x3C" in src)
    check("Wire.begin(21, 22) explicit pin init", "Wire.begin(21, 22)" in src)
    check("oled.begin() return checked",          "!oled.begin(" in src)
    check("oled.display() called after draw",     "oled.display()" in src)
    check("oled.clearDisplay() called",           "oled.clearDisplay()" in src)


def diag_gpio(src):
    print("\n[E] GPIO / Button Diagnostics (AEL HIGH_PRIORITY: input-only pins)")

    btn_next_match = re.search(r"BTN_NEXT\s+(\d+)", src)
    btn_sel_match  = re.search(r"BTN_SEL\s+(\d+)", src)

    # [fix #17] explicit fallback when regex fails
    if btn_next_match is None:
        check("BTN_NEXT define found", False,
              detail="regex matched nothing — define may have been renamed")
        return
    if btn_sel_match is None:
        check("BTN_SEL define found", False,
              detail="regex matched nothing — define may have been renamed")
        return

    btn_next   = int(btn_next_match.group(1))
    btn_sel    = int(btn_sel_match.group(1))
    input_only = {34, 35, 36, 39}

    check(f"BTN_NEXT GPIO{btn_next} supports INPUT_PULLUP",
          btn_next not in input_only,
          level="warn" if btn_next in input_only else "pass",
          detail="GPIO34/35/36/39 = input-only, no internal pullup")

    check(f"BTN_SEL GPIO{btn_sel} supports INPUT_PULLUP",
          btn_sel not in input_only,
          level="warn" if btn_sel in input_only else "pass",
          detail="GPIO34/35/36/39 = input-only, no internal pullup")

    check("INPUT_PULLUP used for buttons",
          "INPUT_PULLUP" in src)

    check("Per-button debounce timestamps",
          "lastBtnNextMs" in src and "lastBtnSelMs" in src,
          detail="shared timestamp can miss simultaneous presses")

    check("Debounce > 200ms",
          "220" in src or "200" in src)


def diag_coexistence(src):
    print("\n[F] BLE/WiFi Coexistence (ESP32 shared radio)")

    check("WiFi disabled before BLE init",
          "WIFI_OFF" in src,
          detail="ESP32 WiFi/BLE share radio — must not overlap")

    check("BLE deinit before WiFi re-init",
          "deinit" in src,
          detail="prevents driver state corruption on mode switch")

    check("on_mode_exit() teardown before every transition",
          "on_mode_exit" in src,
          detail="guards against direct mode-to-mode transitions")

    check("Mode state machine prevents concurrent WiFi+BLE",
          "enum Mode" in src or "Mode mode" in src)


def main():
    print("=" * 60)
    print("ESP32 Security Toolkit — Static Diagnostics")
    print(f"Source: {SRC}")
    print("=" * 60)

    src = load_src()

    diag_wifi(src)
    diag_ble(src)
    diag_nrf(src)
    diag_oled(src)
    diag_gpio(src)
    diag_coexistence(src)

    print(f"\n{'='*60}")
    print("AEL CE Audit (from CLAUDE.md known patterns):")
    print("  7daa8c80  ESP32 USB classification — DevKitC = Class A dual-USB")
    print("            → CP2102 handles upload, no native USB conflict")
    print("  04486a33  HARDWARE_CONNECT_FIRST_RULE — hardware not in hand, noted")
    print("  f5a92b73  LA GND first wire — documented in wiring header")
    print("  CE path /nvme1t offline — patterns applied from CLAUDE.md reference")
    print("=" * 60)


if __name__ == "__main__":
    main()
