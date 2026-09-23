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

  // 1. Kết nối Wi-Fi nhanh tối đa 2.5s
  if (!connectWifiQuick(2500)) {
    LOG_DBG("CF", "Wi-Fi not connected within 2.5s budget, skipping remote fetch");
    disconnectWifi();
    return false;
  }

  char deviceId[32] = {0};
  SETTINGS.getCfDeviceId(deviceId, sizeof(deviceId));
  const char* token = SETTINGS.cfDeviceToken[0] != '\0' ? SETTINGS.cfDeviceToken : deviceId;

  std::string server = std::string(serverUrl);
  if (server.find("192.168.1.3") != std::string::npos) {
    server = "https://cf-api.pocketgo.org";
  }
  if (!server.empty() && server.back() == '/') {
    server.pop_back();
  }

  // 2. Tính thời gian còn lại cho HTTP request
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

  // Thực hiện conditional fetch với timeout <= 4.8s
  const bool hasNewImage = fetchSleepImageConditional(MAX_BUDGET_MS);

  // Chỉ đánh thức màn hình và vẽ lại nếu thực sự có ảnh mới tải về!
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

  // Cài đặt hẹn giờ cho lần thức giấc kế tiếp
  armSleepTimer();
  Storage.prepareForDeepSleep();
  return true;
}

bool CrossFrontService::renderSleepScreen(const GfxRenderer& renderer) {
  // Luôn thực hiện fetch khi vào Sleep (với hard timeout <= 4.8s)
  fetchSleepImageConditional(MAX_BUDGET_MS);

  // Mở file ảnh (mới tải về hoặc ảnh cũ đã lưu từ trước)
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
