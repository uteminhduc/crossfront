#include "crossfront/CrossFrontSettings.h"

#include <esp_mac.h>

#include <cstdio>
#include <cstring>

namespace {

void copyToField(char* dest, const char* src, const size_t maxLen) {
  strncpy(dest, src, maxLen - 1);
  dest[maxLen - 1] = '\0';
}

uint8_t validInterval(const uint8_t interval) {
  return interval < CrossFrontSettings::UPDATE_INTERVAL_COUNT ? interval : CrossFrontSettings::ON_SLEEP;
}

uint16_t validSleepNetworkTimeout(const uint16_t timeoutMs) {
  switch (timeoutMs) {
    case 3000:
    case 5000:
    case 10000:
    case 15000: return timeoutMs;
    default: return 10000;
  }
}

}  // namespace

void CrossFrontSettings::toJson(JsonDocument& doc) const {
  if (serverUrl[0] != '\0') doc["serverUrl"] = serverUrl;
  if (webUrl[0] != '\0') doc["webUrl"] = webUrl;
  if (deviceToken[0] != '\0') doc["deviceToken"] = deviceToken;
  doc["updateInterval"] = updateInterval;
  doc["sleepNetworkTimeoutMs"] = sleepNetworkTimeoutMs;
}

bool CrossFrontSettings::fromJson(const JsonVariantConst doc) {
  if (doc["serverUrl"].is<const char*>()) {
    copyToField(serverUrl, doc["serverUrl"].as<const char*>(), sizeof(serverUrl));
  }
  if (doc["webUrl"].is<const char*>()) {
    copyToField(webUrl, doc["webUrl"].as<const char*>(), sizeof(webUrl));
  }
  if (doc["deviceToken"].is<const char*>()) {
    copyToField(deviceToken, doc["deviceToken"].as<const char*>(), sizeof(deviceToken));
  }
  if (serverUrl[0] == '\0') copyToField(serverUrl, "https://cf-api.pocketgo.org", sizeof(serverUrl));
  if (webUrl[0] == '\0') copyToField(webUrl, "https://cf.pocketgo.org", sizeof(webUrl));
  updateInterval = validInterval(doc["updateInterval"] | static_cast<uint8_t>(ON_SLEEP));
  sleepNetworkTimeoutMs = validSleepNetworkTimeout(doc["sleepNetworkTimeoutMs"] | static_cast<uint16_t>(10000));
  return true;
}

void CrossFrontSettings::getDeviceId(char* outId, const size_t maxLen) const {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(outId, maxLen, "CF-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

uint32_t CrossFrontSettings::getUpdateIntervalSeconds() const {
  switch (updateInterval) {
    case ONE_MINUTE: return 60;
    case TWO_MINUTES: return 120;
    case FIVE_MINUTES: return 5 * 60;
    case FIFTEEN_MINUTES: return 15 * 60;
    case THIRTY_MINUTES: return 30 * 60;
    case ONE_HOUR: return 3600;
    case TWO_HOURS: return 2 * 3600;
    case THREE_HOURS: return 3 * 3600;
    case SIX_HOURS: return 6 * 3600;
    case TWELVE_HOURS: return 12 * 3600;
    case ONE_DAY: return 24 * 3600;
    case ON_SLEEP:
    default: return 0;
  }
}
