#pragma once

#include <cstdint>

constexpr int ESP_MAC_WIFI_STA = 0;

extern "C" int esp_read_mac(uint8_t* mac, int type);
