#include "CrossFrontService.h"

#include <ArduinoJson.h>
#include <BitmapHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <algorithm>
#include <vector>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "crossfront/CrossFrontSettings.h"
#include "network/HttpDownloader.h"

std::string CrossFrontService::getSavedEtag() {
  std::string savedEtag = "";
  HalFile tagFile;
  if (Storage.openFileForRead("CF", ETAG_FILE_PATH, tagFile)) {
    char tagBuf[64] = {0};
    const int r = tagFile.read(reinterpret_cast<uint8_t*>(tagBuf), sizeof(tagBuf) - 1);
    if (r > 0) {
      tagBuf[r] = '\0';
      savedEtag = std::string(tagBuf);
    }
    tagFile.close();
  }
  return savedEtag;
}

void CrossFrontService::saveEtag(const std::string& etag) {
  if (etag.empty()) return;
  HalFile tagFile;
  if (Storage.openFileForWrite("CF", ETAG_FILE_PATH, tagFile)) {
    tagFile.write(reinterpret_cast<const uint8_t*>(etag.c_str()), etag.length());
    tagFile.close();
  }
}

bool CrossFrontService::connectWifiQuick(unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  auto& store = WifiCredentialStore::getInstance();
  store.loadFromFile();

  // Keep the most recently successful network first, then fall back to the
  // remaining saved credentials without spending time on a full scan.
  std::vector<WifiCredential> candidates;
  candidates.reserve(store.getCredentialCount() + 1);
  const std::string lastSsid = store.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    const auto lastCredential = store.findCredential(lastSsid);
    if (lastCredential) candidates.push_back(*lastCredential);
  }

  for (size_t index = 0; index < store.getCredentialCount(); ++index) {
    const auto credential = store.getCredentialAt(index);
    if (!credential || credential->ssid == lastSsid) continue;
    candidates.push_back(*credential);
  }

  if (candidates.empty()) {
    return false;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // Fast scan avoids spending the whole fetch budget scanning every channel before auth.
  WiFi.setScanMethod(WIFI_FAST_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  const unsigned long start = millis();

  for (size_t index = 0; index < candidates.size(); ++index) {
    const unsigned long elapsed = millis() - start;
    if (elapsed >= timeoutMs) break;

    const unsigned long remaining = timeoutMs - elapsed;
    if (remaining < 700) break;

    const size_t candidatesRemaining = candidates.size() - index;
    const unsigned long perCandidateBudget = remaining / candidatesRemaining;
    const unsigned long attemptTimeout =
        (candidatesRemaining > 1) ? std::min(remaining, std::max(4000UL, perCandidateBudget)) : remaining;

    const auto& credential = candidates[index];
    LOG_DBG("CF", "Trying saved Wi-Fi %u/%u: %s (%lums)", static_cast<unsigned>(index + 1),
            static_cast<unsigned>(candidates.size()), credential.ssid.c_str(), attemptTimeout);
    WiFi.disconnect(true, false);
    delay(75);
    WiFi.mode(WIFI_STA);
    WiFi.begin(credential.ssid.c_str(), credential.password.c_str());

    const unsigned long attemptStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - attemptStart < attemptTimeout) {
      delay(50);
      const wl_status_t status = WiFi.status();
      if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED) break;
    }

    if (WiFi.status() == WL_CONNECTED) {
      store.setLastConnectedSsid(credential.ssid);
      LOG_DBG("CF", "Connected to saved Wi-Fi: %s", credential.ssid.c_str());
      return true;
    }

    LOG_DBG("CF", "Saved Wi-Fi failed: %s", credential.ssid.c_str());
  }

  return false;
}

void CrossFrontService::disconnectWifi() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

bool CrossFrontService::fetchSleepImageConditional(unsigned long maxBudgetMs) {
  const char* serverUrl = CROSSFRONT_SETTINGS.getServerUrl();
  if (serverUrl[0] == '\0') {
    return false;
  }

  const unsigned long totalStart = millis();
  const std::string savedEtag = getSavedEtag();

  constexpr unsigned long minImageBudgetMs = 1500;
  if (maxBudgetMs <= minImageBudgetMs) {
    return false;
  }
  const unsigned long wifiBudgetMs = maxBudgetMs - minImageBudgetMs;
  if (!connectWifiQuick(wifiBudgetMs)) {
    LOG_DBG("CF", "Wi-Fi not connected within %lums budget, skipping remote fetch", wifiBudgetMs);
    disconnectWifi();
    return false;
  }

  char deviceId[32] = {0};
  CROSSFRONT_SETTINGS.getDeviceId(deviceId, sizeof(deviceId));
  const char* token = CROSSFRONT_SETTINGS.deviceToken[0] != '\0' ? CROSSFRONT_SETTINGS.deviceToken : deviceId;

  std::string server = std::string(serverUrl);
  if (!server.empty() && server.back() == '/') {
    server.pop_back();
  }

  const unsigned long elapsed = millis() - totalStart;
  int httpTimeout = (elapsed < maxBudgetMs) ? static_cast<int>(maxBudgetMs - elapsed) : 0;
  bool hasNewImage = false;
  if (httpTimeout >= 1000) {
    std::string url = server + "/api/cf/device/" + token + "/sleep.bmp";
    LOG_INF("CF", "Conditional fetch from %s (etag: %s, timeout: %dms)", url.c_str(), savedEtag.c_str(), httpTimeout);

    std::string responseEtag;
    const auto err = HttpDownloader::downloadToFile(url, SLEEP_BMP_PATH, nullptr, nullptr, "", "", false,
                                                    httpTimeout, savedEtag, &responseEtag);
    if (err == HttpDownloader::DownloadError::OK) {
      hasNewImage = true;
      saveEtag(responseEtag);
      LOG_INF("CF", "Downloaded new CrossFront image");
    } else if (err == HttpDownloader::DownloadError::NOT_MODIFIED) {
      LOG_INF("CF", "Image not modified (304), preserving cached image");
    } else {
      LOG_DBG("CF", "Fetch failed/timeout (%d), using cached image", static_cast<int>(err));
    }
  } else {
    LOG_DBG("CF", "Not enough budget left for image fetch (%dms)", httpTimeout);
  }

  disconnectWifi();
  return hasNewImage;
}

bool CrossFrontService::handleTimerWakeup(HalDisplay& display, GfxRenderer& renderer) {
  LOG_INF("CF", "Handling CrossFront timer wakeup");
  CROSSFRONT_SETTINGS.loadFromFile();
  SETTINGS.loadFromFile();

  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::CROSSFRONT ||
      CROSSFRONT_SETTINGS.getServerUrl()[0] == '\0') {
    return false;
  }

  const bool hasNewImage = fetchSleepImageConditional(CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs);

  if (hasNewImage) {
    HalFile file;
    if (Storage.openFileForRead("CF", SLEEP_BMP_PATH, file)) {
      display.begin(true);
      renderer.begin();
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderer.clearScreen();
        renderer.drawBitmap(bitmap, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      display.deepSleep();
    }
  } else {
    LOG_INF("CF", "No new image, skipped e-ink redraw to conserve battery");
  }

  armSleepTimer();
  Storage.prepareForDeepSleep();
  return true;
}

bool CrossFrontService::renderSleepScreen(const GfxRenderer& renderer) {
  fetchSleepImageConditional(CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs);

  HalFile file;
  if (Storage.openFileForRead("CF", SLEEP_BMP_PATH, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("CF", "Rendering CrossFront sleep screen (%dx%d)", bitmap.getWidth(), bitmap.getHeight());
      renderer.clearScreen();
      renderer.drawBitmap(bitmap, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      return true;
    }
  }

  LOG_INF("CF", "No valid CrossFront image found on storage");
  return false;
}

void CrossFrontService::armSleepTimer() {
  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::CROSSFRONT) {
    const uint32_t intervalSec = CROSSFRONT_SETTINGS.getUpdateIntervalSeconds();
    if (intervalSec > 0) {
      esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(intervalSec) * 1000000ULL);
      LOG_DBG("CF", "CrossFront sleep timer set to %u seconds", static_cast<unsigned>(intervalSec));
    }
  }
}

CrossFrontService::SyncResult CrossFrontService::syncNow(ProgressFn onProgress, void* userData,
                                                    unsigned long timeoutMs) {
  CROSSFRONT_SETTINGS.loadFromFile();
  auto& store = WifiCredentialStore::getInstance();
  store.loadFromFile();

  if (store.getCredentialCount() == 0) {
    LOG_ERR("CF", "syncNow: No saved Wi-Fi credentials");
    return SyncResult::NO_WIFI_CONFIGURED;
  }

  const unsigned long totalBudgetMs = timeoutMs;
  const unsigned long startTime = millis();

  auto getRemainingMs = [startTime, totalBudgetMs]() -> long {
    return static_cast<long>(totalBudgetMs) - static_cast<long>(millis() - startTime);
  };

  if (onProgress) {
    onProgress(SyncStep::CONNECTING_WIFI, userData);
  }

  constexpr unsigned long minHttpReserveMs = 2000;
  if (totalBudgetMs <= minHttpReserveMs) {
    return SyncResult::TIMEOUT;
  }
  const unsigned long wifiBudgetMs = totalBudgetMs - minHttpReserveMs;

  if (!connectWifiQuick(wifiBudgetMs)) {
    disconnectWifi();
    if (getRemainingMs() <= 0) {
      LOG_ERR("CF", "syncNow: Timed out during Wi-Fi connect");
      return SyncResult::TIMEOUT;
    }
    LOG_ERR("CF", "syncNow: Failed to connect Wi-Fi");
    return SyncResult::WIFI_CONNECT_FAILED;
  }

  const char* serverUrl = CROSSFRONT_SETTINGS.getServerUrl();
  std::string server = std::string(serverUrl);
  if (!server.empty() && server.back() == '/') {
    server.pop_back();
  }

  char deviceId[32] = {0};
  CROSSFRONT_SETTINGS.getDeviceId(deviceId, sizeof(deviceId));
  const char* token = (CROSSFRONT_SETTINGS.deviceToken[0] != '\0') ? CROSSFRONT_SETTINGS.deviceToken : deviceId;

  // 1. Fetch config and update Wi-Fi credentials from web studio
  if (onProgress) {
    onProgress(SyncStep::FETCHING_CONFIG, userData);
  }

  const long configRemainingMs = getRemainingMs();
  if (configRemainingMs < 1000) {
    disconnectWifi();
    LOG_ERR("CF", "syncNow: Timed out before config fetch");
    return SyncResult::TIMEOUT;
  }

  std::string configUrl = server + "/api/cf/device/" + token + "/config";
  std::string jsonBody;
  bool configOk = false;
  if (HttpDownloader::fetchUrl(configUrl, jsonBody, "", "", static_cast<int>(configRemainingMs))) {
    JsonDocument doc;
    if (deserializeJson(doc, jsonBody) == DeserializationError::Ok) {
      configOk = true;
      if (doc["wifi_list"].is<JsonArray>()) {
        bool added = false;
        for (JsonObject net : doc["wifi_list"].as<JsonArray>()) {
          const char* ssid = net["ssid"];
          const char* pass = net["password"] | "";
          if (ssid && strlen(ssid) > 0) {
            store.addCredential(ssid, pass);
            added = true;
          }
        }
        if (added) {
          store.saveToFile();
          LOG_INF("CF", "Wi-Fi list updated from CrossFront Web App");
        }
      }

      if (doc["interval"].is<const char*>()) {
        const char* intervalStr = doc["interval"].as<const char*>();
        if (strcmp(intervalStr, "on_sleep") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::ON_SLEEP;
        else if (strcmp(intervalStr, "1m") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::ONE_MINUTE;
        else if (strcmp(intervalStr, "2m") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::TWO_MINUTES;
        else if (strcmp(intervalStr, "5m") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::FIVE_MINUTES;
        else if (strcmp(intervalStr, "15m") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::FIFTEEN_MINUTES;
        else if (strcmp(intervalStr, "30m") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::THIRTY_MINUTES;
        else if (strcmp(intervalStr, "1h") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::ONE_HOUR;
        else if (strcmp(intervalStr, "2h") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::TWO_HOURS;
        else if (strcmp(intervalStr, "3h") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::THREE_HOURS;
        else if (strcmp(intervalStr, "6h") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::SIX_HOURS;
        else if (strcmp(intervalStr, "12h") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::TWELVE_HOURS;
        else if (strcmp(intervalStr, "1d") == 0 || strcmp(intervalStr, "24h") == 0) CROSSFRONT_SETTINGS.updateInterval = CrossFrontSettings::ONE_DAY;
        CROSSFRONT_SETTINGS.saveToFile();
      }

      if (doc["sleep_network_timeout_ms"].is<uint16_t>()) {
        const uint16_t timeoutMs = doc["sleep_network_timeout_ms"].as<uint16_t>();
        switch (timeoutMs) {
          case 15000:
          case 20000:
          case 25000:
          case 30000:
            CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs = timeoutMs;
            CROSSFRONT_SETTINGS.saveToFile();
            break;
          default: break;
        }
      }
    }
  }

  if (!configOk) {
    disconnectWifi();
    if (getRemainingMs() <= 0) {
      LOG_ERR("CF", "syncNow: Timed out during config fetch");
      return SyncResult::TIMEOUT;
    }
    LOG_ERR("CF", "syncNow: Failed to fetch device configuration");
    return SyncResult::CONFIG_FETCH_FAILED;
  }

  // 2. Download latest sleep image
  if (onProgress) {
    onProgress(SyncStep::FETCHING_IMAGE, userData);
  }

  const long imageRemainingMs = getRemainingMs();
  if (imageRemainingMs < 1000) {
    disconnectWifi();
    LOG_ERR("CF", "syncNow: Timed out before image fetch");
    return SyncResult::TIMEOUT;
  }

  std::string imageUrl = server + "/api/cf/device/" + token + "/sleep.bmp";
  std::string responseEtag;
  const auto err = HttpDownloader::downloadToFile(imageUrl, SLEEP_BMP_PATH, nullptr, nullptr, "", "", false,
                                                  static_cast<int>(imageRemainingMs), "", &responseEtag);
  disconnectWifi();

  if (err == HttpDownloader::DownloadError::OK) {
    saveEtag(responseEtag);
  }

  if (err == HttpDownloader::DownloadError::OK || err == HttpDownloader::DownloadError::NOT_MODIFIED) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CROSSFRONT;
    SETTINGS.saveToFile();
    LOG_INF("CF", "syncNow: Synchronized image and config successfully");
    return SyncResult::OK;
  }

  if (getRemainingMs() <= 0) {
    LOG_ERR("CF", "syncNow: Timed out during image download");
    return SyncResult::TIMEOUT;
  }

  LOG_ERR("CF", "syncNow: Failed to download sleep image (%d)", static_cast<int>(err));
  return SyncResult::IMAGE_FETCH_FAILED;
}

