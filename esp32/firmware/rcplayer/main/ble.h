// BLE through the Tab5's ESP32-C6 (esp_hosted HCI over SDIO) with NimBLE on the P4.
// A smoke test for the Muse port: scan 5 s, print named advertisers, then advertise.
#pragma once

namespace ble {

void start();   // console "ble"; needs Wi-Fi/esp_hosted already up (net::start)

}  // namespace ble
