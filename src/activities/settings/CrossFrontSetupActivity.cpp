#include "CrossFrontSetupActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_random.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "crossfront/CrossFrontCrypto.h"
#include "crossfront/CrossFrontService.h"
#include "crossfront/CrossFrontSettings.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/QrUtils.h"
#include <Utf8.h>

namespace fui = freeink::ui;

namespace {
constexpr int QR_SIZE = 184;
constexpr int QR_PAD = 16;
constexpr int QR_RIGHT_PAD = 24;

std::string formatShortSsid(const std::string& ssid, size_t maxLen = 10) {
  if (ssid.length() <= maxLen) return ssid;
  int safeLen = utf8SafeTruncateBuffer(ssid.c_str(), static_cast<int>(maxLen - 2));
  return ssid.substr(0, safeLen) + "..";
}
}

CrossFrontSetupActivity::CrossFrontSetupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("CrossFrontSetup", renderer, mappedInput) {}

std::string CrossFrontSetupActivity::getFriendlyModelName() const {
  std::string raw = BoardConfig::ACTIVE.name;
  for (char& c : raw) c = tolower(c);
  if (raw.find("x4pro") != std::string::npos || raw.find("x4_pro") != std::string::npos ||
      (raw.find("x4") != std::string::npos && raw.find("pro") != std::string::npos)) {
    return "x4p";
  }
  if (raw.find("x4") != std::string::npos) {
    return "x4";
  }
  if (raw.find("x3") != std::string::npos) {
    return "x3";
  }
  return "x4";
}

void CrossFrontSetupActivity::ensureTokenGenerated() {
  CROSSFRONT_SETTINGS.getDeviceId(deviceId, sizeof(deviceId));

  if (CROSSFRONT_SETTINGS.deviceToken[0] == '\0') {
    crossfront::generateRandomToken(CROSSFRONT_SETTINGS.deviceToken, 8);
    CROSSFRONT_SETTINGS.saveToFile();
  }
}

std::string CrossFrontSetupActivity::getPairingUrl() const {
  std::string base = CROSSFRONT_SETTINGS.getWebUrl();
  if (!base.empty() && base.back() == '/') {
    base.pop_back();
  }

  const char* cleanId = (strncmp(deviceId, "CF-", 3) == 0) ? (deviceId + 3) : deviceId;

  return base + "/connect?d=" + cleanId +
         "&t=" + CROSSFRONT_SETTINGS.deviceToken +
         "&m=" + getFriendlyModelName();
}

const char* CrossFrontSetupActivity::getIntervalLabel(uint8_t interval) const {
  switch (interval) {
    case CrossFrontSettings::ONE_MINUTE: return tr(STR_CROSSFRONT_1_MIN);
    case CrossFrontSettings::TWO_MINUTES: return tr(STR_CROSSFRONT_2_MIN);
    case CrossFrontSettings::FIVE_MINUTES: return tr(STR_CROSSFRONT_5_MIN);
    case CrossFrontSettings::FIFTEEN_MINUTES: return tr(STR_CROSSFRONT_15_MIN);
    case CrossFrontSettings::THIRTY_MINUTES: return tr(STR_CROSSFRONT_30_MIN);
    case CrossFrontSettings::ONE_HOUR: return tr(STR_CROSSFRONT_1_HOUR);
    case CrossFrontSettings::TWO_HOURS: return tr(STR_CROSSFRONT_2_HOURS);
    case CrossFrontSettings::THREE_HOURS: return tr(STR_CROSSFRONT_3_HOURS);
    case CrossFrontSettings::SIX_HOURS: return tr(STR_CROSSFRONT_6_HOURS);
    case CrossFrontSettings::TWELVE_HOURS: return tr(STR_CROSSFRONT_12_HOURS);
    case CrossFrontSettings::ONE_DAY: return tr(STR_CROSSFRONT_24_HOURS);
    case CrossFrontSettings::ON_SLEEP:
    default:
      return tr(STR_CROSSFRONT_ON_SLEEP);
  }
}

std::string CrossFrontSetupActivity::getNetworkWaitLabel(uint16_t timeoutMs) const {
  char value[32];
  snprintf(value, sizeof(value), tr(STR_CROSSFRONT_SECONDS), static_cast<unsigned>(timeoutMs / 1000));
  return value;
}

int CrossFrontSetupActivity::listCount() const {
  switch (viewMode) {
    case ViewMode::MAIN: return MAIN_ITEM_COUNT;
    case ViewMode::INTERVAL: return CrossFrontSettings::UPDATE_INTERVAL_COUNT;
    case ViewMode::NETWORK_WAIT: return WAIT_ITEM_COUNT;
  }
  return MAIN_ITEM_COUNT;
}

int CrossFrontSetupActivity::computeQrSectionHeight() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Header band + top padding + spacing + QR + bottom padding + divider.
  return metrics.topPadding + metrics.headerHeight + 6 + QR_PAD + QR_SIZE + QR_PAD + 2;
}

const char* CrossFrontSetupActivity::headerTitle() const {
  if (viewMode == ViewMode::INTERVAL) return tr(STR_CROSSFRONT_REFRESH_INTERVAL);
  if (viewMode == ViewMode::NETWORK_WAIT) return tr(STR_CROSSFRONT_SLEEP_NETWORK_WAIT);
  return tr(STR_CROSSFRONT_SETUP);
}

void CrossFrontSetupActivity::onEnter() {
  UiListActivity::onEnter();
  ensureTokenGenerated();
  syncStatus = SyncStatus::IDLE;
  syncDetail = "";
  rotateTokenStatus = RotateTokenStatus::IDLE;
  currentSyncStep = CrossFrontService::SyncStep::CONNECTING_WIFI;
  lastSyncResult = CrossFrontService::SyncResult::OK;
  viewMode = ViewMode::MAIN;

  // Default selection to row 0 (Sync Now)
  nav.selected = 0;

  // Full refresh to prevent e-ink ghosting
  renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
  requestUpdate(true);
}

void CrossFrontSetupActivity::onExit() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  UiListActivity::onExit();
}

void CrossFrontSetupActivity::drawChrome() {
  UiListActivity::drawChrome();
  if (viewMode != ViewMode::MAIN) return;

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int topY = metrics.topPadding + metrics.headerHeight + 6;
  const int qrY = topY + QR_PAD;

  // 1. Right side: QR Code
  const int qrX = pageWidth - QR_SIZE - QR_RIGHT_PAD;
  const Rect qrBounds(qrX, qrY, QR_SIZE, QR_SIZE);
  QrUtils::drawQrCode(renderer, qrBounds, getPairingUrl());

  // 2. Left side: Web App, Device ID, and Token
  const int leftX = 32;
  const int indentX = leftX + 12;
  const char* displayDeviceId = (strncmp(deviceId, "CF-", 3) == 0) ? (deviceId + 3) : deviceId;

  // 2a. Web App Address (above Device ID)
  const int webLabelY = qrY + 6;
  renderer.drawText(UI_10_FONT_ID, leftX, webLabelY, tr(STR_CROSSFRONT_WEB_URL), true, EpdFontFamily::REGULAR);
  const int webValueY = webLabelY + 22;
  renderer.drawText(UI_10_FONT_ID, indentX, webValueY, CROSSFRONT_SETTINGS.getWebUrl(), true, EpdFontFamily::BOLD);

  // 2b. Device ID (gap matches token label vs device value)
  const int deviceLabelY = webValueY + 40;
  renderer.drawText(UI_10_FONT_ID, leftX, deviceLabelY, tr(STR_CROSSFRONT_DEVICE_ID), true, EpdFontFamily::REGULAR);
  const int deviceValueY = deviceLabelY + 22;
  renderer.drawText(UI_12_FONT_ID, indentX, deviceValueY, displayDeviceId, true, EpdFontFamily::BOLD);

  // 2c. Token (identical gap)
  const int tokenLabelY = deviceValueY + 40;
  renderer.drawText(UI_10_FONT_ID, leftX, tokenLabelY, tr(STR_CROSSFRONT_TOKEN), true, EpdFontFamily::REGULAR);
  const int tokenValueY = tokenLabelY + 22;
  renderer.drawText(UI_12_FONT_ID, indentX, tokenValueY, CROSSFRONT_SETTINGS.deviceToken, true, EpdFontFamily::BOLD);

  // Divider line
  const int dividerY = qrY + QR_SIZE + QR_PAD;
  renderer.drawLine(0, dividerY, pageWidth - 1, dividerY, true);
}

void CrossFrontSetupActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int topMargin = viewMode == ViewMode::MAIN ? computeQrSectionHeight() : metrics.topPadding + metrics.headerHeight + 6;

  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(topMargin),
      0,
      static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing),
      0});

  if (viewMode == ViewMode::MAIN) {
    rowItems[0].sectionHeading = nullptr;
    rowItems[0].label = tr(STR_CROSSFRONT_SYNC_NOW);
    if (syncStatus == SyncStatus::SYNCING) {
      switch (currentSyncStep) {
        case CrossFrontService::SyncStep::CONNECTING_WIFI:
          if (!syncDetail.empty()) {
            std::string connStr = tr(STR_CONNECTING);
            while (connStr.length() >= 3 && connStr.substr(connStr.length() - 3) == "...") {
              connStr.erase(connStr.length() - 3);
            }
            rowValues[0] = connStr + ": " + formatShortSsid(syncDetail, 10);
          } else {
            rowValues[0] = tr(STR_CROSSFRONT_CONNECTING_WIFI);
          }
          break;
        case CrossFrontService::SyncStep::FETCHING_CONFIG:
          rowValues[0] = tr(STR_CROSSFRONT_FETCHING_CONFIG);
          break;
        case CrossFrontService::SyncStep::FETCHING_IMAGE:
          rowValues[0] = tr(STR_CROSSFRONT_FETCHING_IMAGE);
          break;
      }
    } else if (syncStatus == SyncStatus::FINISHED) {
      switch (lastSyncResult) {
        case CrossFrontService::SyncResult::OK: {
          const std::string& wifi = CrossFrontService::getLastSyncedWifi();
          if (!wifi.empty()) {
            rowValues[0] = std::string(tr(STR_CROSSFRONT_SYNC_OK)) + " (" + formatShortSsid(wifi, 10) + ")";
          } else {
            rowValues[0] = tr(STR_CROSSFRONT_SYNC_OK);
          }
          break;
        }
        case CrossFrontService::SyncResult::NOT_PAIRED:
          rowValues[0] = tr(STR_CROSSFRONT_ERR_NOT_PAIRED);
          break;
        case CrossFrontService::SyncResult::NO_WIFI_CONFIGURED:
          rowValues[0] = tr(STR_CROSSFRONT_ERR_NO_WIFI);
          break;
        case CrossFrontService::SyncResult::WIFI_CONNECT_FAILED:
          rowValues[0] = tr(STR_CROSSFRONT_ERR_WIFI);
          break;
        case CrossFrontService::SyncResult::CONFIG_FETCH_FAILED:
          rowValues[0] = tr(STR_CROSSFRONT_ERR_SERVER);
          break;
        case CrossFrontService::SyncResult::IMAGE_FETCH_FAILED:
          rowValues[0] = tr(STR_CROSSFRONT_ERR_IMAGE);
          break;
        case CrossFrontService::SyncResult::TIMEOUT:
          rowValues[0] = tr(STR_CROSSFRONT_ERR_TIMEOUT);
          break;
      }
    } else {
      auto& store = WifiCredentialStore::getInstance();
      store.loadFromFile();
      if (store.getCredentialCount() == 0) {
        rowValues[0] = tr(STR_CROSSFRONT_ERR_NO_WIFI);
      } else {
        rowValues[0] = tr(STR_CROSSFRONT_PRESS_TO_SYNC);
      }
    }
    rowItems[0].value = rowValues[0].empty() ? nullptr : rowValues[0].c_str();
    rowItems[0].actionValue = 0;

    rowItems[1].sectionHeading = nullptr;
    rowItems[1].label = tr(STR_CROSSFRONT_REFRESH_INTERVAL);
    rowValues[1] = getIntervalLabel(CROSSFRONT_SETTINGS.updateInterval);
    rowItems[1].value = rowValues[1].c_str();
    rowItems[1].actionValue = 1;

    rowItems[2].sectionHeading = nullptr;
    rowItems[2].label = tr(STR_CROSSFRONT_SLEEP_NETWORK_WAIT);
    rowValues[2] = getNetworkWaitLabel(CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs);
    rowItems[2].value = rowValues[2].c_str();
    rowItems[2].actionValue = 2;

    rowItems[3].sectionHeading = nullptr;
    rowItems[3].label = tr(STR_CROSSFRONT_ROTATE_TOKEN);
    if (rotateTokenStatus == RotateTokenStatus::ROTATING) {
      rowValues[3] = tr(STR_CROSSFRONT_ROTATING_TOKEN);
    } else if (rotateTokenStatus == RotateTokenStatus::FINISHED) {
      switch (lastRotateResult) {
        case CrossFrontService::RotateTokenResult::OK:
          rowValues[3] = tr(STR_CROSSFRONT_ROTATE_OK);
          break;
        case CrossFrontService::RotateTokenResult::NO_WIFI_CONFIGURED:
          rowValues[3] = tr(STR_CROSSFRONT_ERR_NO_WIFI);
          break;
        case CrossFrontService::RotateTokenResult::WIFI_CONNECT_FAILED:
          rowValues[3] = tr(STR_CROSSFRONT_ERR_WIFI);
          break;
        case CrossFrontService::RotateTokenResult::SERVER_FAILED:
        default:
          rowValues[3] = tr(STR_CROSSFRONT_ERR_SERVER);
          break;
      }
    } else {
      rowValues[3] = "";
    }
    rowItems[3].value = rowValues[3].empty() ? nullptr : rowValues[3].c_str();
    rowItems[3].actionValue = 3;
  } else if (viewMode == ViewMode::INTERVAL) {
    for (int index = 0; index < CrossFrontSettings::UPDATE_INTERVAL_COUNT; ++index) {
      rowItems[index].sectionHeading = nullptr;
      rowItems[index].label = getIntervalLabel(static_cast<uint8_t>(index));
      rowValues[index] = (CROSSFRONT_SETTINGS.updateInterval == index) ? tr(STR_SELECTED) : "";
      rowItems[index].value = rowValues[index].empty() ? nullptr : rowValues[index].c_str();
      rowItems[index].actionValue = static_cast<int16_t>(index);
    }
  } else {
    constexpr uint16_t waitOptionsMs[WAIT_ITEM_COUNT] = {15000, 20000, 25000, 30000};
    for (int index = 0; index < WAIT_ITEM_COUNT; ++index) {
      rowItems[index].sectionHeading = nullptr;
      rowValues[index] = getNetworkWaitLabel(waitOptionsMs[index]);
      rowItems[index].label = rowValues[index].c_str();
      rowItems[index].value = (CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs == waitOptionsMs[index]) ? tr(STR_SELECTED) : nullptr;
      rowItems[index].actionValue = static_cast<int16_t>(index);
    }
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  props.headerText = screen.theme().smallText;
  props.headerUnderline = true;
  props.sectionGap = 6;
  syncListViewport(screen, props);
  screen.list(props);
}

void CrossFrontSetupActivity::activateIndex(const int index) {
  if (viewMode == ViewMode::MAIN) {
    if (index == 0) {
      performManualSync();
    } else if (index == 1) {
      viewMode = ViewMode::INTERVAL;
      nav.reset();
      nav.selected = CROSSFRONT_SETTINGS.updateInterval;
      requestUpdate(true);
    } else if (index == 2) {
      viewMode = ViewMode::NETWORK_WAIT;
      nav.reset();
      constexpr uint16_t waitOptionsMs[WAIT_ITEM_COUNT] = {15000, 20000, 25000, 30000};
      for (int i = 0; i < WAIT_ITEM_COUNT; ++i) {
        if (waitOptionsMs[i] == CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs) {
          nav.selected = i;
          break;
        }
      }
      requestUpdate(true);
    } else if (index == 3) {
      promptRotateToken();
    }
    return;
  }

  const int returnRow = (viewMode == ViewMode::INTERVAL) ? 1 : 2;
  if (viewMode == ViewMode::INTERVAL) {
    CROSSFRONT_SETTINGS.updateInterval = static_cast<CrossFrontSettings::UpdateInterval>(index);
  } else {
    constexpr uint16_t waitOptionsMs[WAIT_ITEM_COUNT] = {15000, 20000, 25000, 30000};
    if (index < 0 || index >= WAIT_ITEM_COUNT) return;
    CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs = waitOptionsMs[index];
  }

  CROSSFRONT_SETTINGS.saveToFile();
  viewMode = ViewMode::MAIN;
  nav.reset();
  nav.selected = returnRow;
  requestUpdate(true);
}

void CrossFrontSetupActivity::onBackButton() {
  if (viewMode != ViewMode::MAIN) {
    const int returnRow = (viewMode == ViewMode::INTERVAL) ? 1 : 2;
    viewMode = ViewMode::MAIN;
    nav.reset();
    nav.selected = returnRow;
    requestUpdate(true);
    return;
  }
  finish();
}

void CrossFrontSetupActivity::onSyncProgress(CrossFrontService::SyncStep step, const char* detail, void* userData) {
  auto* self = static_cast<CrossFrontSetupActivity*>(userData);
  if (self) {
    self->handleSyncStep(step, detail);
  }
}

void CrossFrontSetupActivity::handleSyncStep(CrossFrontService::SyncStep step, const char* detail) {
  currentSyncStep = step;
  syncDetail = (detail != nullptr) ? detail : "";
  syncStatus = SyncStatus::SYNCING;
  requestUpdateAndWait();
}

void CrossFrontSetupActivity::performManualSync() {
  const auto result = CrossFrontService::syncNow(&CrossFrontSetupActivity::onSyncProgress, this);
  lastSyncResult = result;
  syncStatus = SyncStatus::FINISHED;
  if (result != CrossFrontService::SyncResult::OK) {
    LOG_ERR("CF", "Manual sync failed: %d", static_cast<int>(result));
  }

  requestUpdate();
}

void CrossFrontSetupActivity::promptRotateToken() {
  const std::string heading = tr(STR_CROSSFRONT_ROTATE_TOKEN);
  const std::string body = tr(STR_CROSSFRONT_ROTATE_TOKEN_CONFIRM);

  startActivityForResult(
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, heading, body),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          performRotateToken();
        }
      });
}

void CrossFrontSetupActivity::performRotateToken() {
  char newToken[16] = {0};
  crossfront::generateRandomToken(newToken, 8);

  rotateTokenStatus = RotateTokenStatus::ROTATING;
  requestUpdateAndWait();

  const auto result = CrossFrontService::rotateToken(newToken);
  lastRotateResult = result;
  rotateTokenStatus = RotateTokenStatus::FINISHED;

  if (result != CrossFrontService::RotateTokenResult::OK) {
    LOG_ERR("CF", "Token rotation failed: %d", static_cast<int>(result));
  }

  requestUpdate(true);
}
