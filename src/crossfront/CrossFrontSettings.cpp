#include "crossfront/CrossFrontSettings.h"

#include <esp_mac.h>

#include <cstdio>
#include <cstring>

namespace {

void copyToField(char* dest, const char* src, const size_t maxLen) {
  if (!dest || maxLen == 0) return;
  if (!src) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, maxLen - 1);
  dest[maxLen - 1] = '\0';
}

uint8_t validInterval(const uint8_t interval) {
  return interval < CrossFrontSettings::UPDATE_INTERVAL_COUNT ? interval : CrossFrontSettings::ON_SLEEP;
}

uint16_t validSleepNetworkTimeout(const uint16_t timeoutMs) {
  switch (timeoutMs) {
    case 15000:
    case 20000:
    case 25000:
    case 30000: return timeoutMs;
    default: return 15000;
  }
}

}  // namespace

CrossFrontSettings::CrossFrontSettings() {
  copyToField(serverUrl, DEFAULT_SERVER_URL, sizeof(serverUrl));
  copyToField(webUrl, DEFAULT_WEB_URL, sizeof(webUrl));
  copyToField(ebookDir, DEFAULT_EBOOK_DIR, sizeof(ebookDir));
}

#include "crossfront/CrossFrontCrypto.h"

void CrossFrontSettings::toJson(JsonDocument& doc) const {
  if (serverUrl[0] != '\0') doc["serverUrl"] = serverUrl;
  if (webUrl[0] != '\0') doc["webUrl"] = webUrl;
  if (deviceToken[0] != '\0') doc["deviceToken"] = deviceToken;
  if (ebookDir[0] != '\0') doc["ebookDir"] = ebookDir;
  doc["updateInterval"] = updateInterval;
  doc["sleepNetworkTimeoutMs"] = sleepNetworkTimeoutMs;
  if (serverPollIntervalSeconds > 0) doc["serverPollIntervalSeconds"] = serverPollIntervalSeconds;
  doc["settingsDirty"] = settingsDirty;
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
  if (doc["ebookDir"].is<const char*>()) {
    setEbookDir(doc["ebookDir"].as<const char*>());
  } else if (ebookDir[0] == '\0') {
    setEbookDir(DEFAULT_EBOOK_DIR);
  }
  if (serverUrl[0] == '\0') copyToField(serverUrl, DEFAULT_SERVER_URL, sizeof(serverUrl));
  if (webUrl[0] == '\0') copyToField(webUrl, DEFAULT_WEB_URL, sizeof(webUrl));
  updateInterval = validInterval(doc["updateInterval"] | static_cast<uint8_t>(ON_SLEEP));
  sleepNetworkTimeoutMs = validSleepNetworkTimeout(doc["sleepNetworkTimeoutMs"] | static_cast<uint16_t>(15000));
  serverPollIntervalSeconds = doc["serverPollIntervalSeconds"] | static_cast<uint32_t>(0);
  settingsDirty = doc["settingsDirty"] | false;
  return true;
}

const char* CrossFrontSettings::getEbookDir() const {
  return (ebookDir[0] != '\0' && strcmp(ebookDir, "/") != 0) ? ebookDir : DEFAULT_EBOOK_DIR;
}

void CrossFrontSettings::setEbookDir(const char* dir) {
  if (!dir || dir[0] == '\0' || strcmp(dir, "/") == 0) {
    copyToField(ebookDir, DEFAULT_EBOOK_DIR, sizeof(ebookDir));
    return;
  }
  char buf[sizeof(ebookDir)];
  size_t idx = 0;
  if (dir[0] != '/') {
    buf[idx++] = '/';
  }
  while (*dir != '\0' && idx < sizeof(buf) - 1) {
    if (*dir == '\\') {
      buf[idx++] = '/';
    } else {
      buf[idx++] = *dir;
    }
    dir++;
  }
  buf[idx] = '\0';
  while (idx > 1 && buf[idx - 1] == '/') {
    buf[--idx] = '\0';
  }
  if (strcmp(buf, "/") == 0) {
    copyToField(ebookDir, DEFAULT_EBOOK_DIR, sizeof(ebookDir));
  } else {
    copyToField(ebookDir, buf, sizeof(ebookDir));
  }
}

void CrossFrontSettings::getDeviceId(char* outId, const size_t maxLen) const {
  crossfront::getDeviceHardwareId(outId, maxLen);
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

uint32_t CrossFrontSettings::getEffectiveUpdateIntervalSeconds() const {
  if (serverPollIntervalSeconds > 0) {
    return serverPollIntervalSeconds;
  }
  return getUpdateIntervalSeconds();
}
