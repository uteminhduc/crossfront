#include "CrossFrontService.h"

#include <ArduinoJson.h>
#include <BitmapHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <PersistableStore.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <vector>

#include <esp_random.h>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "OpdsServerStore.h"
#include "crossfront/CrossFrontCrypto.h"
#include "crossfront/CrossFrontFileSafety.h"
#include "crossfront/CrossFrontSettings.h"
#include "crossfront/SleepImageRequest.h"
#include "network/HttpDownloader.h"
#include <SecureHttpClient.h>

namespace {
RTC_DATA_ATTR uint8_t sTimerWakeupConsecutiveFailures = 0;

struct OpdsSyncState {
  static const char* filePath() { return "/.crosspoint/cf_opds_sync.json"; }

  bool initialized = false;
  bool downloadPending = false;
  uint32_t syncedHash = 0;

  void load() {
    HalFile file = Storage.open(filePath());
    if (!file || file.fileSize64() > 256) return;
    file.close();
    JsonDocument doc;
    if (!PersistableStoreBase::readDocFromFile(filePath(), doc) ||
        !doc["initialized"].is<bool>() || !doc["downloadPending"].is<bool>() ||
        (doc["initialized"].as<bool>() && !doc["syncedHash"].is<uint32_t>())) {
      return;
    }
    initialized = doc["initialized"].as<bool>();
    downloadPending = doc["downloadPending"].as<bool>();
    syncedHash = doc["syncedHash"] | static_cast<uint32_t>(0);
  }

  bool save() const {
    JsonDocument doc;
    doc["initialized"] = initialized;
    doc["downloadPending"] = downloadPending;
    if (initialized) doc["syncedHash"] = syncedHash;
    return PersistableStoreBase::writeDocToFile(filePath(), doc);
  }
};

bool validOpdsText(JsonVariantConst value, size_t maxLength, bool required) {
  if (value.isNull()) return !required;
  if (!value.is<const char*>()) return false;
  const JsonString text = value.as<JsonString>();
  if (text.size() > maxLength) return false;
  for (size_t index = 0; index < text.size(); ++index) {
    const auto character = static_cast<unsigned char>(text.c_str()[index]);
    if (character < 32 || character == 127) return false;
  }
  return true;
}

bool validLocalOpdsFile() {
  HalFile file = Storage.open(OpdsServerStore::getFilePath());
  if (!file || file.fileSize64() > 32768) return false;
  file.close();

  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(OpdsServerStore::getFilePath(), doc) ||
      !doc["servers"].is<JsonArray>()) return false;
  JsonArray servers = doc["servers"].as<JsonArray>();
  if (servers.size() > 8) return false;
  for (JsonVariant item : servers) {
    if (!item.is<JsonObject>() || !validOpdsText(item["name"], 128, true) ||
        !validOpdsText(item["url"], 1024, true) ||
        !validOpdsText(item["username"], 256, false) ||
        !validOpdsText(item["password_obf"], 512, false) ||
        !validOpdsText(item["password"], 256, false)) {
      return false;
    }
    const char* encodedPassword = item["password_obf"] | "";
    if (encodedPassword[0] != '\0') {
      bool decoded = false;
      bool tooLong = false;
      obfuscation::deobfuscateFromBase64(encodedPassword, 256, &decoded, &tooLong);
      if (!decoded) return false;
    }
  }
  return true;
}

uint32_t opdsConfigHash() {
  JsonDocument doc;
  doc["downloadFolder"] = SETTINGS.opdsDownloadFolder;
  doc["filenameFormat"] = SETTINGS.opdsFilenameFormat;
  JsonArray servers = doc["servers"].to<JsonArray>();
  for (const auto& server : OPDS_STORE.getServers()) {
    JsonObject item = servers.add<JsonObject>();
    item["name"] = server.name;
    item["url"] = server.url;
    item["username"] = server.username;
    item["password"] = server.password;
  }
  std::string serialized;
  serializeJson(doc, serialized);
  uint32_t hash = 2166136261u;
  for (const unsigned char character : serialized) {
    hash = (hash ^ character) * 16777619u;
  }
  return hash;
}

std::vector<std::pair<std::string, std::string>> makeCrossFrontHeaders(const char* deviceId, const char* token) {
  std::vector<std::pair<std::string, std::string>> headers;
  headers.reserve(3);
  headers.emplace_back("X-Device-Id", deviceId ? deviceId : "");
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

HalDisplay::GrayscaleMode sleepGrayscaleMode(const GfxRenderer& renderer) {
  return renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Direct).supported()
             ? HalDisplay::GrayscaleMode::Direct
             : HalDisplay::GrayscaleMode::Absolute;
}

bool renderSleepBitmap(GfxRenderer& renderer, Bitmap& bitmap) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const bool hasGreyscale =
      bitmap.hasGreyscale() &&
      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.clearScreen();
  if (!renderer.drawBitmap(bitmap, 0, 0, pageWidth, pageHeight)) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return true;
  }

  if (SETTINGS.sleepScreenCoverFilter ==
      CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  const auto grayscaleMode = sleepGrayscaleMode(renderer);
  const bool absolute = hasGreyscale && renderer.grayscaleCapabilities(grayscaleMode).supported();
  if (absolute) {
    if (!renderer.displayGrayscaleBase(grayscaleMode)) return true;
  } else if (hasGreyscale) {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  if (!hasGreyscale) return true;

  bool ready = true;
  for (const auto plane : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    if (bitmap.rewindToData() != BmpReaderError::Ok) {
      ready = false;
      break;
    }
    renderer.clearScreen(absolute ? 0xFF : 0x00);
    renderer.setRenderMode(plane);
    if (!renderer.drawBitmap(bitmap, 0, 0, pageWidth, pageHeight)) {
      ready = false;
      break;
    }
    if (plane == GfxRenderer::GRAYSCALE_LSB)
      renderer.copyGrayscaleLsbBuffers();
    else
      renderer.copyGrayscaleMsbBuffers();
  }

  if (ready)
    renderer.displayGrayBuffer();
  else
    LOG_ERR("CF", "Incomplete grayscale sleep image; keeping the current display");
  renderer.setRenderMode(GfxRenderer::BW);
  return true;
}
}  // namespace

static std::string lastSyncedWifiSsid = "";

const std::string& CrossFrontService::getLastSyncedWifi() {
  return lastSyncedWifiSsid;
}

bool CrossFrontService::connectWifiQuick(unsigned long timeoutMs, ProgressFn onProgress, void* userData,
                                         CancelFn shouldCancel) {
  const auto isCancelled = [shouldCancel, userData]() { return shouldCancel && shouldCancel(userData); };
  if (isCancelled()) return false;
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
    LOG_ERR("CF", "connectWifiQuick: No saved Wi-Fi credentials in store");
    return false;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // Fast scan avoids spending the whole fetch budget scanning every channel before auth.
  WiFi.setScanMethod(WIFI_FAST_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  const unsigned long start = millis();

  for (size_t index = 0; index < candidates.size(); ++index) {
    if (isCancelled()) return false;
    const unsigned long elapsed = millis() - start;
    if (elapsed >= timeoutMs) break;

    const unsigned long remaining = timeoutMs - elapsed;
    if (remaining < 700) break;

    const size_t candidatesRemaining = candidates.size() - index;
    const unsigned long perCandidateBudget = remaining / candidatesRemaining;
    const unsigned long attemptTimeout =
        (candidatesRemaining > 1) ? std::min(remaining, std::max(4000UL, perCandidateBudget)) : remaining;

    const auto& credential = candidates[index];
    LOG_INF("CF", "Trying saved Wi-Fi %u/%u: %s (%lums)", static_cast<unsigned>(index + 1),
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
      if (isCancelled()) return false;
      delay(50);
      const wl_status_t status = WiFi.status();
      if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED) break;
    }

    if (isCancelled()) return false;
    if (WiFi.status() == WL_CONNECTED) {
      store.setLastConnectedSsid(credential.ssid);
      lastSyncedWifiSsid = credential.ssid;
      LOG_INF("CF", "Connected to saved Wi-Fi: %s", credential.ssid.c_str());
      return true;
    }

    LOG_ERR("CF", "Saved Wi-Fi '%s' failed to connect (status %d)", credential.ssid.c_str(),
            static_cast<int>(WiFi.status()));
  }

  return false;
}

void CrossFrontService::disconnectWifi() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

bool CrossFrontService::fetchSleepImage(unsigned long maxBudgetMs, bool* outNetworkOk) {
  if (outNetworkOk) *outNetworkOk = false;
  crossfront::discardSleepImageCache(Storage, SLEEP_BMP_PATH, ETAG_FILE_PATH, "/.crosspoint/cf_sleep.bmp.bak");
  const char* serverUrl = CROSSFRONT_SETTINGS.getServerUrl();
  LOG_INF("CF", "Starting sleep image fetch (server: %s, budget: %lums)", serverUrl, maxBudgetMs);
  if (serverUrl[0] == '\0') {
    LOG_ERR("CF", "fetchSleepImage: serverUrl is empty");
    return false;
  }

  const unsigned long totalStart = millis();
  constexpr unsigned long minImageBudgetMs = 1500;
  if (maxBudgetMs <= minImageBudgetMs) {
    LOG_ERR("CF", "fetchSleepImage: maxBudgetMs (%lums) <= minImageBudgetMs (%lums)", maxBudgetMs,
            minImageBudgetMs);
    return false;
  }
  const unsigned long wifiBudgetMs = maxBudgetMs - minImageBudgetMs;
  if (!connectWifiQuick(wifiBudgetMs)) {
    LOG_ERR("CF", "Wi-Fi not connected within %lums budget, skipping remote fetch", wifiBudgetMs);
    disconnectWifi();
    return false;
  }

  char deviceId[32] = {0};
  CROSSFRONT_SETTINGS.getDeviceId(deviceId, sizeof(deviceId));
  const char* token = CROSSFRONT_SETTINGS.deviceToken;

  std::string server = std::string(serverUrl);
  if (!server.empty() && server.back() == '/') {
    server.pop_back();
  }

  const unsigned long elapsed = millis() - totalStart;
  int httpTimeout = (elapsed < maxBudgetMs) ? static_cast<int>(maxBudgetMs - elapsed) : 0;
  if (httpTimeout < 5000) {
    httpTimeout = 5000;
  }
  bool hasNewImage = false;
  std::string url = crossfront::sleepImageRequestUrl(server, deviceId);
  LOG_INF("CF", "Fetching fresh sleep image from %s (timeout: %dms)", url.c_str(), httpTimeout);

  uint32_t responsePollInterval = 0xFFFFFFFF;
  auto extraHeaders = makeCrossFrontHeaders(deviceId, token);
  extraHeaders.emplace_back("Cache-Control", "no-store");

  const auto err = HttpDownloader::downloadToFile(url, SLEEP_BMP_PATH, nullptr, nullptr, "", "", false,
                                                  httpTimeout, "", nullptr,
                                                  extraHeaders, &responsePollInterval);
  if (responsePollInterval != 0xFFFFFFFF &&
      responsePollInterval != CROSSFRONT_SETTINGS.serverPollIntervalSeconds) {
    CROSSFRONT_SETTINGS.serverPollIntervalSeconds = responsePollInterval;
    CROSSFRONT_SETTINGS.saveToFile();
    LOG_INF("CF", "Server updated poll interval to %u seconds", static_cast<unsigned>(responsePollInterval));
  }

  if (err == HttpDownloader::DownloadError::OK) {
    if (outNetworkOk) *outNetworkOk = true;
    hasNewImage = true;
    LOG_INF("CF", "Downloaded new CrossFront image");
  } else {
    LOG_ERR("CF", "Sleep image fetch failed (err=%d)", static_cast<int>(err));
    Storage.remove(SLEEP_BMP_PATH);
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

  bool networkOk = false;
  const bool hasNewImage = fetchSleepImage(CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs, &networkOk);

  if (hasNewImage) {
    HalFile file;
    if (Storage.openFileForRead("CF", SLEEP_BMP_PATH, file)) {
      display.begin(true);
      renderer.begin();
      Bitmap bitmap(file, true,
                    renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported() &&
                        display.getController() == HalDisplay::Controller::SSD1677 &&
                        SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderSleepBitmap(renderer, bitmap);
      }
      file.close();
      display.deepSleep();
    }
  } else {
    LOG_INF("CF", "No new image, skipped e-ink redraw to conserve battery");
  }

  if (networkOk) {
    sTimerWakeupConsecutiveFailures = 0;
    armSleepTimer();
  } else {
    sTimerWakeupConsecutiveFailures++;
    const uint8_t maxFailures = CROSSFRONT_SETTINGS.maxSleepFailures;
    LOG_ERR("CF", "Periodic sleep refresh failed to connect (%u)",
            static_cast<unsigned>(sTimerWakeupConsecutiveFailures));
    if (maxFailures > 0 && sTimerWakeupConsecutiveFailures >= maxFailures) {
      LOG_ERR("CF", "Periodic sleep refresh reached failure limit (%u). Suspending periodic refresh for this sleep session.",
              static_cast<unsigned>(maxFailures));
      // Do not re-arm timer: periodic refresh is suspended until the next manual sleep
    } else {
      armSleepTimer();
    }
  }

  Storage.prepareForDeepSleep();
  return true;
}

bool CrossFrontService::renderSleepScreen(GfxRenderer& renderer) {
  sTimerWakeupConsecutiveFailures = 0;
  CROSSFRONT_SETTINGS.loadFromFile();
  const unsigned long networkTimeoutMs = CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs;
  LOG_INF("CF", "Sleep screen entered; requesting remote image (timeout: %lums)", networkTimeoutMs);
  const bool hasNewImage = fetchSleepImage(networkTimeoutMs);
  LOG_INF("CF", "Sleep image fetch finished (new image: %s)", hasNewImage ? "yes" : "no");

  if (!hasNewImage) return false;

  HalFile file;
  if (Storage.openFileForRead("CF", SLEEP_BMP_PATH, file)) {
    Bitmap bitmap(file, true,
                  renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported() &&
                      display.getController() == HalDisplay::Controller::SSD1677 &&
                      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("CF", "Rendering CrossFront sleep screen (%dx%d)", bitmap.getWidth(), bitmap.getHeight());
      return renderSleepBitmap(renderer, bitmap);
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
  sTimerWakeupConsecutiveFailures = 0;
  lastSyncedWifiSsid = "";
  CROSSFRONT_SETTINGS.loadFromFile();
  OpdsSyncState opdsSync;
  opdsSync.load();
  auto& store = WifiCredentialStore::getInstance();
  store.loadFromFile();
  const bool opdsStoreMissing = !Storage.exists(OpdsServerStore::getFilePath());
  const bool opdsStoreLoaded = !opdsStoreMissing && validLocalOpdsFile() && OPDS_STORE.loadFromFile();

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
  const char* token = CROSSFRONT_SETTINGS.deviceToken;

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

  std::string configUrl = server + "/api/cf/device/" + deviceId + "/config";
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
    if (!lastSyncedWifiSsid.empty()) {
      reqDoc["currentSsid"] = lastSyncedWifiSsid;
    }
    if (isDirty) {
      reqDoc["interval"] = intervalToString(CROSSFRONT_SETTINGS.updateInterval);
      reqDoc["sleepNetworkTimeoutMs"] = CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs;
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
    const bool opdsChanged = (opdsStoreLoaded || (!opdsSync.initialized && opdsStoreMissing)) &&
                             !opdsSync.downloadPending &&
                             (!opdsSync.initialized || opdsConfigHash() != opdsSync.syncedHash);
    const std::string opdsFolder = SETTINGS.opdsDownloadFolder;
    const bool opdsValid = (opdsFolder.empty() ||
                            (opdsFolder.size() <= 63 && crossfront::isSafeTargetFolder(opdsFolder))) &&
                           SETTINGS.opdsFilenameFormat <= 2;
    if (opdsChanged && opdsValid) {
      JsonObject opds = reqDoc["opds"].to<JsonObject>();
      opds["bootstrap"] = !opdsSync.initialized;
      opds["downloadFolder"] = opdsFolder;
      opds["filenameFormat"] = SETTINGS.opdsFilenameFormat;
      JsonArray servers = opds["servers"].to<JsonArray>();
      for (const auto& entry : OPDS_STORE.getServers()) {
        JsonObject item = servers.add<JsonObject>();
        item["name"] = entry.name;
        item["url"] = entry.url;
        item["username"] = entry.username;
        item["password"] = entry.password;
      }
    } else if (opdsChanged) {
      LOG_ERR("CF", "OPDS local download settings invalid; keeping local configuration");
    } else if (!opdsStoreLoaded && !opdsStoreMissing) {
      LOG_ERR("CF", "OPDS local file unreadable; keeping cloud configuration");
    }
    std::string reqBody;
    serializeJson(reqDoc, reqBody);

    constexpr size_t maxConfigResponseBytes = 65536;
    const int httpCode = http.sendRequest("POST",
                                          reinterpret_cast<const uint8_t*>(reqBody.data()),
                                          reqBody.size(),
                                          [&jsonBody, maxConfigResponseBytes](const uint8_t* data, size_t len) {
                                            if (len > maxConfigResponseBytes - jsonBody.size()) return false;
                                            jsonBody.append(reinterpret_cast<const char*>(data), len);
                                            return true;
                                          });
    const bool responseComplete = http.responseComplete();
    http.end();
    if (httpCode == 404 || httpCode == 401) {
      disconnectWifi();
      LOG_ERR("CF", "syncNow: Device not paired on server (HTTP %d)", httpCode);
      return SyncResult::NOT_PAIRED;
    }
    if (httpCode == 200 && responseComplete) {
      JsonDocument doc;
      if (deserializeJson(doc, jsonBody) == DeserializationError::Ok) {
        configOk = true;
        if (doc["wifiList"].is<JsonArray>()) {
          bool changed = false;
          std::vector<std::string> serverSsids;
          for (JsonObject net : doc["wifiList"].as<JsonArray>()) {
            const char* ssid = net["ssid"];
            const char* pass = net["password"] | "";
            if (ssid && strlen(ssid) > 0) {
              serverSsids.emplace_back(ssid);
              if (!store.hasSavedCredential(ssid)) {
                store.addCredential(ssid, pass);
                changed = true;
              }
            }
          }

          // Remove credentials that were deleted on Web App, but preserve the currently active Wi-Fi.
          const size_t currentCount = store.getCredentialCount();
          std::vector<std::string> toRemove;
          for (size_t i = 0; i < currentCount; ++i) {
            const auto cred = store.getCredentialAt(i);
            if (cred.has_value() && !cred->ssid.empty()) {
              if (cred->ssid != lastSyncedWifiSsid) {
                const bool existsOnServer = std::find(serverSsids.begin(), serverSsids.end(), cred->ssid) != serverSsids.end();
                if (!existsOnServer) {
                  toRemove.push_back(cred->ssid);
                }
              }
            }
          }
          for (const auto& ssid : toRemove) {
            if (store.removeCredential(ssid)) {
              changed = true;
              LOG_INF("CF", "Removed deleted Wi-Fi: %s", ssid.c_str());
            }
          }

          if (changed) {
            store.saveToFile();
            LOG_INF("CF", "Wi-Fi list synchronized with CrossFront Web App");
          }
        }

        if (doc["config"]["opdsServers"].is<JsonArray>()) {
          JsonArray cloudServers = doc["config"]["opdsServers"].as<JsonArray>();
          JsonObject deviceConfig = doc["config"].as<JsonObject>();
          JsonVariant cloudFolderValue = deviceConfig["opdsDownloadFolder"];
          JsonVariant cloudFormatValue = deviceConfig["opdsFilenameFormat"];
          std::string cloudFolder = opdsValid ? opdsFolder : "";
          if (cloudFolderValue.is<const char*>()) {
            const JsonString text = cloudFolderValue.as<JsonString>();
            cloudFolder.assign(text.c_str(), text.size());
          }
          const int cloudFormat = cloudFormatValue.is<int>()
                                      ? cloudFormatValue.as<int>()
                                      : (SETTINGS.opdsFilenameFormat <= 2 ? SETTINGS.opdsFilenameFormat : 0);
          bool validCloudServers = true;
          for (JsonVariant item : cloudServers) {
            if (!item.is<JsonObject>() || !validOpdsText(item["name"], 128, true) ||
                !validOpdsText(item["url"], 1024, true) ||
                !validOpdsText(item["username"], 256, false) ||
                !validOpdsText(item["password"], 256, false)) {
              validCloudServers = false;
              break;
            }
          }
          if (!validCloudServers ||
              (!cloudFolderValue.isNull() && !cloudFolderValue.is<const char*>()) ||
              (!cloudFormatValue.isNull() && !cloudFormatValue.is<int>()) ||
              cloudServers.size() > 8 || cloudFolder.size() > 63 ||
              (!cloudFolder.empty() && !crossfront::isSafeTargetFolder(cloudFolder)) ||
              cloudFormat < 0 || cloudFormat > 2) {
            LOG_ERR("CF", "OPDS server settings invalid on cloud");
            configOk = false;
          } else {
            bool needsDownload = !opdsStoreLoaded || cloudFolder != SETTINGS.opdsDownloadFolder ||
                                 cloudFormat != SETTINGS.opdsFilenameFormat ||
                                 cloudServers.size() != OPDS_STORE.getCount();
            size_t compareIndex = 0;
            for (JsonObject item : cloudServers) {
              const auto* existing = OPDS_STORE.getServer(compareIndex++);
              if (!existing || existing->name != (item["name"] | "") ||
                  existing->url != (item["url"] | "") ||
                  existing->username != (item["username"] | "") ||
                  existing->password != (item["password"] | "")) {
                needsDownload = true;
              }
            }
            bool opdsSynced = true;
            if (needsDownload && !opdsSync.downloadPending) {
              opdsSync.downloadPending = true;
              opdsSynced = opdsSync.save();
            }
            size_t index = 0;
            for (JsonObject item : cloudServers) {
              if (!opdsSynced) break;
              OpdsServer server;
              server.name = item["name"] | "";
              server.url = item["url"] | "";
              server.username = item["username"] | "";
              server.password = item["password"] | "";
              const auto* existing = OPDS_STORE.getServer(index);
              if (existing) {
                if (existing->name != server.name || existing->url != server.url ||
                    existing->username != server.username || existing->password != server.password) {
                  opdsSynced = OPDS_STORE.updateServer(index, server);
                }
              } else {
                opdsSynced = OPDS_STORE.addServer(server);
              }
              if (!opdsSynced) break;
              ++index;
            }
            while (opdsSynced && OPDS_STORE.getCount() > index) {
              opdsSynced = OPDS_STORE.removeServer(OPDS_STORE.getCount() - 1);
            }
            if (opdsSynced && !opdsStoreLoaded) {
              opdsSynced = OPDS_STORE.saveToFile();
            }
            if (opdsSynced && (cloudFolder != SETTINGS.opdsDownloadFolder ||
                               cloudFormat != SETTINGS.opdsFilenameFormat)) {
              strncpy(SETTINGS.opdsDownloadFolder, cloudFolder.c_str(), sizeof(SETTINGS.opdsDownloadFolder) - 1);
              SETTINGS.opdsDownloadFolder[sizeof(SETTINGS.opdsDownloadFolder) - 1] = '\0';
              SETTINGS.opdsFilenameFormat = static_cast<uint8_t>(cloudFormat);
              opdsSynced = SETTINGS.saveToFile();
            }
            if (opdsSynced) {
              const uint32_t syncedHash = opdsConfigHash();
              if (opdsSync.downloadPending || !opdsSync.initialized ||
                  opdsSync.syncedHash != syncedHash) {
                opdsSync.initialized = true;
                opdsSync.syncedHash = syncedHash;
                opdsSync.downloadPending = false;
                opdsSynced = opdsSync.save();
              }
            }
            if (!opdsSynced) {
              configOk = false;
              LOG_ERR("CF", "Failed to synchronize OPDS server settings");
            }
          }
        } else if (opdsSync.downloadPending ||
                   (!opdsStoreLoaded && (opdsSync.initialized || !opdsStoreMissing))) {
          configOk = false;
          LOG_ERR("CF", "OPDS cloud settings missing during recovery");
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

          if (deviceConfig["maxSleepFailures"].is<uint8_t>()) {
            CROSSFRONT_SETTINGS.maxSleepFailures = deviceConfig["maxSleepFailures"].as<uint8_t>();
            CROSSFRONT_SETTINGS.saveToFile();
          }

          if (deviceConfig["ebookDir"].is<const char*>()) {
            const char* configuredEbookDir = deviceConfig["ebookDir"].as<const char*>();
            if (configuredEbookDir && std::strcmp(configuredEbookDir, CROSSFRONT_SETTINGS.getEbookDir()) != 0) {
              CROSSFRONT_SETTINGS.setEbookDir(configuredEbookDir);
              CROSSFRONT_SETTINGS.saveToFile();
            }
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
    LOG_INF("CF", "rotateToken: Token rotated and saved successfully");
    return RotateTokenResult::OK;
  }

  LOG_ERR("CF", "rotateToken: Failed with server status: %d", httpCode);
  return RotateTokenResult::SERVER_FAILED;
}
