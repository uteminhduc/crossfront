#include "CrossFrontService.h"

#include <ArduinoJson.h>
#include <BitmapHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
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
  std::optional<WifiCredential> cred;
  const std::string lastSsid = store.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    cred = store.findCredential(lastSsid);
  }
  if (!cred && store.getCredentialCount() > 0) {
    cred = store.getCredentialAt(0);
  }
  if (!cred) {
    return false;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start < timeoutMs)) {
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    store.setLastConnectedSsid(cred->ssid);
    return true;
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
  const char* serverUrl = SETTINGS.cfServerUrl;
  if (serverUrl[0] == '\0') {
    return false;
  }

  const unsigned long totalStart = millis();
  const std::string savedEtag = getSavedEtag();

  if (!connectWifiQuick(2500)) {
    LOG_DBG("CF", "Wi-Fi not connected within 2.5s budget, skipping remote fetch");
    disconnectWifi();
    return false;
  }

  char deviceId[32] = {0};
  SETTINGS.getCfDeviceId(deviceId, sizeof(deviceId));
  const char* token = SETTINGS.cfDeviceToken[0] != '\0' ? SETTINGS.cfDeviceToken : deviceId;

  std::string server = std::string(serverUrl);
  if (server.empty()) {
    server = "https://cf-api.pocketgo.org";
  }
  if (!server.empty() && server.back() == '/') {
    server.pop_back();
  }

  const unsigned long elapsed = millis() - totalStart;
  int httpTimeout = (elapsed < maxBudgetMs) ? static_cast<int>(maxBudgetMs - elapsed) : 0;
  if (httpTimeout > 2200) httpTimeout = 2200;

  bool hasNewImage = false;
  if (httpTimeout > 500) {
    std::string url = server + "/api/cf/device/" + token + "/sleep.bmp";
    LOG_INF("CF", "Conditional fetch from %s (etag: %s, timeout: %dms)", url.c_str(), savedEtag.c_str(), httpTimeout);

    const auto err = HttpDownloader::downloadToFile(url, SLEEP_BMP_PATH, nullptr, nullptr, "", "", false, httpTimeout, savedEtag);
    if (err == HttpDownloader::DownloadError::OK) {
      hasNewImage = true;
      LOG_INF("CF", "Downloaded new CrossFront image");
    } else if (err == HttpDownloader::DownloadError::NOT_MODIFIED) {
      LOG_INF("CF", "Image not modified (304), preserving cached image");
    } else {
      LOG_DBG("CF", "Fetch failed/timeout (%d), using cached image", static_cast<int>(err));
    }
  }

  disconnectWifi();
  return hasNewImage;
}

bool CrossFrontService::handleTimerWakeup(HalDisplay& display, GfxRenderer& renderer) {
  LOG_INF("CF", "Handling CrossFront timer wakeup");
  SETTINGS.loadFromFile();

  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::CROSSFRONT || SETTINGS.cfServerUrl[0] == '\0') {
    return false;
  }

  const bool hasNewImage = fetchSleepImageConditional(MAX_BUDGET_MS);

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
  fetchSleepImageConditional(MAX_BUDGET_MS);

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
    const uint32_t intervalSec = SETTINGS.getCfIntervalSeconds();
    if (intervalSec > 0) {
      esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(intervalSec) * 1000000ULL);
      LOG_DBG("CF", "CrossFront sleep timer set to %u seconds", static_cast<unsigned>(intervalSec));
    }
  }
}

CrossFrontService::SyncResult CrossFrontService::syncNow() {
  auto& store = WifiCredentialStore::getInstance();
  store.loadFromFile();

  if (!connectWifiQuick(5000)) {
    LOG_ERR("CF", "syncNow: Failed to connect Wi-Fi within 5s");
    disconnectWifi();
    return SyncResult::WIFI_CONNECT_FAILED;
  }

  const char* serverUrl = SETTINGS.cfServerUrl;
  std::string server = (serverUrl[0] != '\0') ? std::string(serverUrl) : "https://cf-api.pocketgo.org";
  if (!server.empty() && server.back() == '/') {
    server.pop_back();
  }

  char deviceId[32] = {0};
  SETTINGS.getCfDeviceId(deviceId, sizeof(deviceId));
  const char* token = (SETTINGS.cfDeviceToken[0] != '\0') ? SETTINGS.cfDeviceToken : deviceId;

  // 1. Fetch config and update Wi-Fi credentials from web studio
  std::string configUrl = server + "/api/cf/device/" + token + "/config";
  std::string jsonBody;
  bool configOk = false;
  if (HttpDownloader::fetchUrl(configUrl, jsonBody, "", "", 4000)) {
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
        if (strcmp(intervalStr, "on_sleep") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_ON_SLEEP;
        else if (strcmp(intervalStr, "1m") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_1_MIN;
        else if (strcmp(intervalStr, "2m") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_2_MIN;
        else if (strcmp(intervalStr, "5m") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_5_MIN;
        else if (strcmp(intervalStr, "15m") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_15_MIN;
        else if (strcmp(intervalStr, "30m") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_30_MIN;
        else if (strcmp(intervalStr, "1h") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_1_HOUR;
        else if (strcmp(intervalStr, "2h") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_2_HOURS;
        else if (strcmp(intervalStr, "3h") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_3_HOURS;
        else if (strcmp(intervalStr, "6h") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_6_HOURS;
        else if (strcmp(intervalStr, "12h") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_12_HOURS;
        else if (strcmp(intervalStr, "1d") == 0) SETTINGS.cfUpdateInterval = CrossPointSettings::CF_1_DAY;
        SETTINGS.saveToFile();
      }
    }
  }

  // 2. Download latest sleep image
  std::string imageUrl = server + "/api/cf/device/" + token + "/sleep.bmp";
  const auto err = HttpDownloader::downloadToFile(imageUrl, SLEEP_BMP_PATH, nullptr, nullptr, "", "", false, 5000);

  disconnectWifi();

  if (err == HttpDownloader::DownloadError::OK || err == HttpDownloader::DownloadError::NOT_MODIFIED) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CROSSFRONT;
    SETTINGS.saveToFile();
    LOG_INF("CF", "syncNow: Synchronized image and config successfully");
    return SyncResult::OK;
  }

  if (!configOk) {
    return SyncResult::CONFIG_FETCH_FAILED;
  }
  return SyncResult::IMAGE_FETCH_FAILED;
}

