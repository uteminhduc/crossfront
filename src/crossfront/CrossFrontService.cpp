#include "CrossFrontService.h"

#include <ArduinoJson.h>
#include <BitmapHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <algorithm>
#include <ctime>
#include <vector>

#include <esp_random.h>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "crossfront/CrossFrontCrypto.h"
#include "crossfront/CrossFrontSettings.h"
#include "network/HttpDownloader.h"
#include <SecureHttpClient.h>

namespace {
std::vector<std::pair<std::string, std::string>> makeCrossFrontHeaders(const char* deviceId, const char* token) {
  std::vector<std::pair<std::string, std::string>> headers;
  headers.reserve(2);
  headers.emplace_back("X-Device-Id", deviceId);
  if (token && token[0] != '\0') {
    headers.emplace_back("X-Device-Token", token);
  }
  return headers;
}

const char* intervalToString(const uint8_t interval) {
  switch (interval) {
    case CrossFrontSettings::ON_SLEEP: return "on_sleep";
    case CrossFrontSettings::ONE_MINUTE: return "1m";
    case CrossFrontSettings::TWO_MINUTES: return "2m";
    case CrossFrontSettings::FIVE_MINUTES: return "5m";
    case CrossFrontSettings::FIFTEEN_MINUTES: return "15m";
    case CrossFrontSettings::THIRTY_MINUTES: return "30m";
    case CrossFrontSettings::ONE_HOUR: return "1h";
    case CrossFrontSettings::TWO_HOURS: return "2h";
    case CrossFrontSettings::THREE_HOURS: return "3h";
    case CrossFrontSettings::SIX_HOURS: return "6h";
    case CrossFrontSettings::TWELVE_HOURS: return "12h";
    case CrossFrontSettings::ONE_DAY: return "1d";
    default: return "on_sleep";
  }
}
}  // namespace

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

static std::string lastSyncedWifiSsid = "";

const std::string& CrossFrontService::getLastSyncedWifi() {
  return lastSyncedWifiSsid;
}

bool CrossFrontService::connectWifiQuick(unsigned long timeoutMs, ProgressFn onProgress, void* userData) {
  if (WiFi.status() == WL_CONNECTED) {
    auto& store = WifiCredentialStore::getInstance();
    lastSyncedWifiSsid = store.getLastConnectedSsid();
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
    if (onProgress) {
      onProgress(SyncStep::CONNECTING_WIFI, credential.ssid.c_str(), userData);
    }
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
      lastSyncedWifiSsid = credential.ssid;
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
    const char* cleanId = (strncmp(deviceId, "CF-", 3) == 0) ? (deviceId + 3) : deviceId;
    std::string url = server + "/api/cf/device/" + cleanId + "/sleep.bmp";
    LOG_INF("CF", "Conditional fetch from %s (etag: %s, timeout: %dms)", url.c_str(), savedEtag.c_str(), httpTimeout);

    std::string responseEtag;
    uint32_t responsePollInterval = 0xFFFFFFFF;
    auto extraHeaders = makeCrossFrontHeaders(deviceId, token);

    const auto err = HttpDownloader::downloadToFile(url, SLEEP_BMP_PATH, nullptr, nullptr, "", "", false,
                                                    httpTimeout, savedEtag, &responseEtag,
                                                    extraHeaders, &responsePollInterval);
    if (responsePollInterval != 0xFFFFFFFF &&
        responsePollInterval != CROSSFRONT_SETTINGS.serverPollIntervalSeconds) {
      CROSSFRONT_SETTINGS.serverPollIntervalSeconds = responsePollInterval;
      CROSSFRONT_SETTINGS.saveToFile();
      LOG_INF("CF", "Server updated poll interval to %u seconds", static_cast<unsigned>(responsePollInterval));
    }

    if (err == HttpDownloader::DownloadError::OK) {
      hasNewImage = true;
      saveEtag(responseEtag);
      LOG_INF("CF", "Downloaded new CrossFront image");
    } else if (err == HttpDownloader::DownloadError::NOT_MODIFIED) {
      LOG_INF("CF", "Image not modified (304), preserving cached image");
    } else {
      // WiFi connected but server rejected the request (device deleted/blocked
      // returns 4xx). Clear the cache so the fallback screen is shown.
      // No-WiFi and timeout cases return early above, so reaching here means
      // the server was reachable.
      LOG_INF("CF", "Server rejected request, clearing cached image");
      Storage.remove(SLEEP_BMP_PATH);
      Storage.remove(ETAG_FILE_PATH);
    }
  } else {
    LOG_DBG("CF", "Not enough budget left for image fetch (%dms)", httpTimeout);
  }

  disconnectWifi();
  return hasNewImage;
}

bool CrossFrontService::handleTimerWakeup(HalDisplay& display, GfxRenderer& renderer) {
  LOG_INF("CF", "Handling CrossFront timer wakeup");
  if (!Storage.ready() && !Storage.begin()) {
    LOG_ERR("CF", "handleTimerWakeup: Failed to initialize storage");
    return false;
  }
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
      file.close();
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
    const uint32_t intervalSec = CROSSFRONT_SETTINGS.getEffectiveUpdateIntervalSeconds();
    if (intervalSec > 0) {
      esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(intervalSec) * 1000000ULL);
      LOG_DBG("CF", "CrossFront sleep timer set to %u seconds (server override: %u)",
              static_cast<unsigned>(intervalSec),
              static_cast<unsigned>(CROSSFRONT_SETTINGS.serverPollIntervalSeconds));
    }
  }
}

CrossFrontService::SyncResult CrossFrontService::syncNow(ProgressFn onProgress, void* userData,
                                                    unsigned long timeoutMs) {
  lastSyncedWifiSsid = "";
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

  constexpr unsigned long minHttpReserveMs = 2000;
  if (totalBudgetMs <= minHttpReserveMs) {
    return SyncResult::TIMEOUT;
  }
  const unsigned long wifiBudgetMs = totalBudgetMs - minHttpReserveMs;

  if (!connectWifiQuick(wifiBudgetMs, onProgress, userData)) {
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
    onProgress(SyncStep::FETCHING_CONFIG, lastSyncedWifiSsid.c_str(), userData);
  }

  const long configRemainingMs = getRemainingMs();
  if (configRemainingMs < 1000) {
    disconnectWifi();
    LOG_ERR("CF", "syncNow: Timed out before config fetch");
    return SyncResult::TIMEOUT;
  }

  const char* cleanId = (strncmp(deviceId, "CF-", 3) == 0) ? (deviceId + 3) : deviceId;
  std::string configUrl = server + "/api/cf/device/" + cleanId + "/config";
  std::string jsonBody;
  bool configOk = false;
  freeink::SecureHttpClient http;
  http.setTimeout(static_cast<int>(configRemainingMs));
  http.setInsecure();
  if (http.begin(configUrl)) {
    http.setUserAgent("CrossPoint-ESP32");
    http.addHeader("Content-Type", "application/json");
    auto extraHeaders = makeCrossFrontHeaders(deviceId, token);
    for (const auto& h : extraHeaders) {
      http.addHeader(h.first.c_str(), h.second.c_str());
    }

    const bool isDirty = CROSSFRONT_SETTINGS.settingsDirty;
    JsonDocument reqDoc;
    if (isDirty) {
      reqDoc["interval"] = intervalToString(CROSSFRONT_SETTINGS.updateInterval);
      reqDoc["sleepNetworkTimeoutMs"] = CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs;
      reqDoc["ebookDir"] = CROSSFRONT_SETTINGS.getEbookDir();
    }
    JsonArray reqWifi = reqDoc["wifiList"].to<JsonArray>();
    const size_t localCredCount = store.getCredentialCount();
    for (size_t i = 0; i < localCredCount; ++i) {
      const auto cred = store.getCredentialAt(i);
      if (cred.has_value() && !cred->ssid.empty()) {
        JsonObject net = reqWifi.add<JsonObject>();
        net["ssid"] = cred->ssid;
        net["password"] = cred->password;
      }
    }
    std::string reqBody;
    serializeJson(reqDoc, reqBody);

    const int httpCode = http.sendRequest("POST",
                                          reinterpret_cast<const uint8_t*>(reqBody.data()),
                                          reqBody.size(),
                                          [&jsonBody](const uint8_t* data, size_t len) {
                                            jsonBody.append(reinterpret_cast<const char*>(data), len);
                                            return true;
                                          });
    http.end();
    if (httpCode == 404 || httpCode == 401) {
      disconnectWifi();
      LOG_ERR("CF", "syncNow: Device not paired on server (HTTP %d)", httpCode);
      return SyncResult::NOT_PAIRED;
    }
    if (httpCode == 200) {
      JsonDocument doc;
      if (deserializeJson(doc, jsonBody) == DeserializationError::Ok) {
        configOk = true;
        if (doc["wifiList"].is<JsonArray>()) {
          bool added = false;
          for (JsonObject net : doc["wifiList"].as<JsonArray>()) {
            const char* ssid = net["ssid"];
            const char* pass = net["password"] | "";
            if (ssid && strlen(ssid) > 0) {
              if (!store.hasSavedCredential(ssid)) {
                store.addCredential(ssid, pass);
                added = true;
              }
            }
          }
          if (added) {
            store.saveToFile();
            LOG_INF("CF", "Wi-Fi list merged from CrossFront Web App");
          }
        }

        if (isDirty) {
          CROSSFRONT_SETTINGS.settingsDirty = false;
          CROSSFRONT_SETTINGS.saveToFile();
          LOG_INF("CF", "Local settings synced to CrossFront server");
        } else {
          JsonObject deviceConfig = doc["config"].is<JsonObject>() ? doc["config"].as<JsonObject>() : doc.as<JsonObject>();
          if (deviceConfig["interval"].is<const char*>()) {
            const char* intervalStr = deviceConfig["interval"].as<const char*>();
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

          if (deviceConfig["sleepNetworkTimeoutMs"].is<uint16_t>()) {
            const uint16_t timeoutMs = deviceConfig["sleepNetworkTimeoutMs"].as<uint16_t>();
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

          if (deviceConfig["ebookDir"].is<const char*>()) {
            CROSSFRONT_SETTINGS.setEbookDir(deviceConfig["ebookDir"].as<const char*>());
            CROSSFRONT_SETTINGS.saveToFile();
          }
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

  disconnectWifi();

  // Successfully synced device configuration and Wi-Fi credentials from Web Studio.
  // Activate CrossFront as the sleep screen mode. Sleep screen image is fetched on demand
  // when the device enters sleep mode.
  SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CROSSFRONT;
  SETTINGS.saveToFile();
  LOG_INF("CF", "syncNow: Synchronized config successfully");
  return SyncResult::OK;
}

CrossFrontService::RotateTokenResult CrossFrontService::rotateToken(const char* newToken, unsigned long timeoutMs) {
  if (!newToken || strlen(newToken) < 6) {
    return RotateTokenResult::SERVER_FAILED;
  }

  auto& store = WifiCredentialStore::getInstance();
  store.loadFromFile();
  if (store.getCredentialCount() == 0) {
    LOG_ERR("CF", "rotateToken: No saved Wi-Fi credentials");
    return RotateTokenResult::NO_WIFI_CONFIGURED;
  }

  if (!connectWifiQuick(timeoutMs)) {
    disconnectWifi();
    return RotateTokenResult::WIFI_CONNECT_FAILED;
  }

  char deviceId[32] = {0};
  CROSSFRONT_SETTINGS.getDeviceId(deviceId, sizeof(deviceId));
  const char* oldToken = CROSSFRONT_SETTINGS.deviceToken;

  const char* serverUrl = CROSSFRONT_SETTINGS.getServerUrl();
  std::string server = std::string(serverUrl);
  if (!server.empty() && server.back() == '/') {
    server.pop_back();
  }
  std::string url = server + "/api/cf/device/rotate-token";

  JsonDocument reqDoc;
  reqDoc["deviceId"] = deviceId;
  reqDoc["oldToken"] = oldToken;
  reqDoc["newToken"] = newToken;
  std::string body;
  serializeJson(reqDoc, body);

  freeink::SecureHttpClient http;
  http.setTimeout(static_cast<int>(timeoutMs));
  http.setInsecure();
  if (!http.begin(url)) {
    disconnectWifi();
    LOG_ERR("CF", "rotateToken: Failed to begin HTTP connection: %s", url.c_str());
    return RotateTokenResult::SERVER_FAILED;
  }

  http.setUserAgent("CrossPoint-ESP32");
  http.addHeader("Content-Type", "application/json");
  auto extraHeaders = makeCrossFrontHeaders(deviceId, oldToken);
  for (const auto& h : extraHeaders) {
    http.addHeader(h.first.c_str(), h.second.c_str());
  }

  const int httpCode = http.sendRequest("POST", body);
  http.end();
  disconnectWifi();

  LOG_INF("CF", "rotateToken response code: %d", httpCode);
  if (httpCode >= 200 && httpCode < 300) {
    snprintf(CROSSFRONT_SETTINGS.deviceToken, sizeof(CROSSFRONT_SETTINGS.deviceToken), "%s", newToken);
    CROSSFRONT_SETTINGS.saveToFile();
    LOG_INF("CF", "rotateToken: Token rotated and saved successfully: %s", newToken);
    return RotateTokenResult::OK;
  }

  LOG_ERR("CF", "rotateToken: Failed with server status: %d", httpCode);
  return RotateTokenResult::SERVER_FAILED;
}

