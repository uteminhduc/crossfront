#include "CrossFrontSetupActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_random.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/QrUtils.h"

CrossFrontSetupActivity::CrossFrontSetupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("CrossFrontSetup", renderer, mappedInput) {}

void CrossFrontSetupActivity::ensureTokenGenerated() {
  SETTINGS.getCfDeviceId(deviceId, sizeof(deviceId));

  if (SETTINGS.cfDeviceToken[0] == '\0') {
    uint32_t r1 = esp_random();
    snprintf(SETTINGS.cfDeviceToken, sizeof(SETTINGS.cfDeviceToken), "%08X", (unsigned int)r1);
    SETTINGS.saveToFile();
  }
}

std::string CrossFrontSetupActivity::getPairingUrl() const {
  std::string base = SETTINGS.cfWebUrl[0] != '\0' ? SETTINGS.cfWebUrl : "https://cf.pocketgo.org";
  if (base.find("192.168.1.3") != std::string::npos) {
    base = "https://cf.pocketgo.org";
  }
  if (!base.empty() && base.back() == '/') {
    base.pop_back();
  }

  // Đọc thông tin model phần cứng và độ phân giải màn hình
  std::string model = BoardConfig::ACTIVE.name;
  for (char& c : model) {
    if (c == ' ') c = '+';
  }

  return base + "/connect?dev=" + deviceId + "&token=" + SETTINGS.cfDeviceToken +
         "&model=" + model +
         "&w=" + std::to_string(BoardConfig::ACTIVE.displayWidth) +
         "&h=" + std::to_string(BoardConfig::ACTIVE.displayHeight);
}

const char* CrossFrontSetupActivity::getIntervalLabel() const {
  switch (SETTINGS.cfUpdateInterval) {
    case CrossPointSettings::CF_1_MIN: return "1 phut";
    case CrossPointSettings::CF_2_MIN: return "2 phut";
    case CrossPointSettings::CF_5_MIN: return "5 phut";
    case CrossPointSettings::CF_15_MIN: return "15 phut";
    case CrossPointSettings::CF_30_MIN: return "30 phut";
    case CrossPointSettings::CF_1_HOUR: return "1 gio";
    case CrossPointSettings::CF_2_HOURS: return "2 gio";
    case CrossPointSettings::CF_3_HOURS: return "3 gio";
    case CrossPointSettings::CF_6_HOURS: return "6 gio";
    case CrossPointSettings::CF_12_HOURS: return "12 gio";
    case CrossPointSettings::CF_1_DAY: return "1 ngay";
    case CrossPointSettings::CF_ON_SLEEP:
    default:
      return "Chi khi Sleep";
  }
}

void CrossFrontSetupActivity::onEnter() {
  Activity::onEnter();
  ensureTokenGenerated();
  statusMessage = "";
  isSyncing = false;
  selectedMenu = MenuSelection::SYNC_NOW;
  requestUpdate();
}

void CrossFrontSetupActivity::onExit() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  Activity::onExit();
}

void CrossFrontSetupActivity::performManualSync() {
  isSyncing = true;
  statusMessage = tr(STR_CROSSFRONT_CONNECTING);
  requestUpdateAndWait();

  if (WiFi.status() != WL_CONNECTED) {
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
    if (cred) {
      WiFi.persistent(false);
      WiFi.mode(WIFI_STA);
      WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
      WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
      const unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 4000) {
        delay(50);
      }
      if (WiFi.status() == WL_CONNECTED) {
        store.setLastConnectedSsid(cred->ssid);
      }
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    std::string base = SETTINGS.cfServerUrl[0] != '\0' ? SETTINGS.cfServerUrl : "https://cf-api.pocketgo.org";
    if (base.find("192.168.1.3") != std::string::npos) {
      base = "https://cf-api.pocketgo.org";
    }
    if (!base.empty() && base.back() == '/') base.pop_back();

    // 1. Đồng bộ cấu hình và danh sách Wi-Fi
    std::string configUrl = base + "/api/cf/device/" + SETTINGS.cfDeviceToken + "/config";
    std::string jsonBody;
    if (HttpDownloader::fetchUrl(configUrl, jsonBody, "", "", 3000)) {
      JsonDocument doc;
      if (deserializeJson(doc, jsonBody) == DeserializationError::Ok) {
        if (doc["wifi_list"].is<JsonArray>()) {
          auto& store = WifiCredentialStore::getInstance();
          store.loadFromFile();
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
            LOG_INF("CF", "Wi-Fi credentials updated from CrossFront");
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

    // 2. Ép tải ảnh Sleep screen (với timeout tối đa 4000ms)
    std::string url = base + "/api/cf/device/" + SETTINGS.cfDeviceToken + "/sleep.bmp";

    const auto err = HttpDownloader::downloadToFile(url, "/.crosspoint/cf_sleep.bmp", nullptr, nullptr, "", "", false, 4000);
    if (err == HttpDownloader::DownloadError::OK || err == HttpDownloader::DownloadError::NOT_MODIFIED) {
      statusMessage = "Synced successfully!";
      SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CROSSFRONT;
      SETTINGS.saveToFile();
    } else {
      statusMessage = "Download failed (" + std::to_string(static_cast<int>(err)) + ")";
    }
  } else {
    statusMessage = tr(STR_CONNECTION_FAILED);
  }

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  isSyncing = false;
  requestUpdate();
}

void CrossFrontSetupActivity::loop() {
  if (isSyncing) return;

  int x = 0;
  int y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
    return;
  }

  // Chuyển đổi giữa 2 mục menu: Up / Down
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) || mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    selectedMenu = (selectedMenu == MenuSelection::SYNC_NOW) ? MenuSelection::INTERVAL : MenuSelection::SYNC_NOW;
    requestUpdate();
    return;
  }

  // Điều chỉnh mốc thời gian: Left / Right
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (SETTINGS.cfUpdateInterval > 0) {
      SETTINGS.cfUpdateInterval--;
    } else {
      SETTINGS.cfUpdateInterval = CrossPointSettings::CROSSFRONT_INTERVAL::CF_INTERVAL_COUNT - 1;
    }
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    SETTINGS.cfUpdateInterval = (SETTINGS.cfUpdateInterval + 1) % CrossPointSettings::CROSSFRONT_INTERVAL::CF_INTERVAL_COUNT;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  // Nút Confirm: Hành động theo mục menu đang chọn
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedMenu == MenuSelection::SYNC_NOW) {
      performManualSync();
    } else {
      // Khi chọn dòng interval, bấm Confirm sẽ chuyển sang mốc kế tiếp
      SETTINGS.cfUpdateInterval = (SETTINGS.cfUpdateInterval + 1) % CrossPointSettings::CROSSFRONT_INTERVAL::CF_INTERVAL_COUNT;
      SETTINGS.saveToFile();
      requestUpdate();
    }
    return;
  }
}

void CrossFrontSetupActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CROSSFRONT_SETUP), nullptr);

  int curY = metrics.topPadding + metrics.headerHeight + 10;

  // Device Info Line: Model, ID, Token
  char infoBuf[128];
  snprintf(infoBuf, sizeof(infoBuf), "%s  |  %s", BoardConfig::ACTIVE.name, SETTINGS.cfDeviceToken);
  renderer.drawCenteredText(UI_10_FONT_ID, curY, infoBuf, true, EpdFontFamily::BOLD);
  curY += 22;

  // Hint
  renderer.drawCenteredText(SMALL_FONT_ID, curY, tr(STR_CROSSFRONT_PAIR_HINT));
  curY += 16;

  // QR Code Box - Co nhỏ kích thước xuống ~150px
  const int qrSize = std::min(pageWidth - 80, 150);
  const int qrX = (pageWidth - qrSize) / 2;
  const Rect qrBounds(qrX, curY, qrSize, qrSize);
  QrUtils::drawQrCode(renderer, qrBounds, getPairingUrl());
  curY += qrSize + 18;

  // Menu bên dưới QR:
  const int menuBoxW = std::min(pageWidth - 32, 280);
  const int menuBoxX = (pageWidth - menuBoxW) / 2;
  const int itemHeight = 30;

  // 1. Menu Item: Sync Now
  const bool isSyncSelected = (selectedMenu == MenuSelection::SYNC_NOW);
  char syncBuf[128];
  snprintf(syncBuf, sizeof(syncBuf), "%s 1. Dong bo ngay (Sync)", isSyncSelected ? ">" : " ");
  if (isSyncSelected) {
    renderer.fillRect(menuBoxX, curY, menuBoxW, itemHeight, 0x00);
    renderer.drawCenteredText(UI_10_FONT_ID, curY + 6, syncBuf, false, EpdFontFamily::BOLD);
  } else {
    renderer.drawRect(menuBoxX, curY, menuBoxW, itemHeight, 0x00);
    renderer.drawCenteredText(UI_10_FONT_ID, curY + 6, syncBuf, true);
  }
  curY += itemHeight + 8;

  // 2. Menu Item: Cập nhật sau (Interval)
  const bool isIntervalSelected = (selectedMenu == MenuSelection::INTERVAL);
  char intervalBuf[128];
  snprintf(intervalBuf, sizeof(intervalBuf), "%s 2. Cap nhat sau: < %s >", isIntervalSelected ? ">" : " ", getIntervalLabel());
  if (isIntervalSelected) {
    renderer.fillRect(menuBoxX, curY, menuBoxW, itemHeight, 0x00);
    renderer.drawCenteredText(UI_10_FONT_ID, curY + 6, intervalBuf, false, EpdFontFamily::BOLD);
  } else {
    renderer.drawRect(menuBoxX, curY, menuBoxW, itemHeight, 0x00);
    renderer.drawCenteredText(UI_10_FONT_ID, curY + 6, intervalBuf, true);
  }
  curY += itemHeight + 12;

  // Status message
  if (!statusMessage.empty()) {
    renderer.drawCenteredText(SMALL_FONT_ID, curY, statusMessage.c_str(), true, EpdFontFamily::BOLD);
  }

  // Button hints at bottom
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
