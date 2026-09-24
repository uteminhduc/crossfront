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

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "crossfront/CrossFrontService.h"
#include "crossfront/CrossFrontSettings.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/QrUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr int QR_SIZE = 240;
constexpr int QR_TOP_PAD = 10;
constexpr int QR_BOTTOM_PAD = 14;
constexpr char TOKEN_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
constexpr size_t TOKEN_LENGTH = 8;
constexpr uint8_t TOKEN_RANDOM_LIMIT = 248;
}

CrossFrontSetupActivity::CrossFrontSetupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("CrossFrontSetup", renderer, mappedInput) {}

std::string CrossFrontSetupActivity::getFriendlyModelName() const {
  std::string raw = BoardConfig::ACTIVE.name;
  for (char& c : raw) c = tolower(c);
  if (raw.find("x4pro") != std::string::npos || raw.find("x4_pro") != std::string::npos ||
      (raw.find("x4") != std::string::npos && raw.find("pro") != std::string::npos)) {
    return "X4 Pro";
  }
  if (raw.find("x4") != std::string::npos) {
    return "X4";
  }
  if (raw.find("x3") != std::string::npos) {
    return "X3";
  }
  return BoardConfig::ACTIVE.name;
}

void CrossFrontSetupActivity::ensureTokenGenerated() {
  CROSSFRONT_SETTINGS.getDeviceId(deviceId, sizeof(deviceId));

  if (CROSSFRONT_SETTINGS.deviceToken[0] == '\0') {
    size_t tokenIndex = 0;
    while (tokenIndex < TOKEN_LENGTH) {
      const uint32_t randomValue = esp_random();
      for (uint8_t byteIndex = 0; byteIndex < sizeof(randomValue) && tokenIndex < TOKEN_LENGTH; ++byteIndex) {
        const uint8_t randomByte = static_cast<uint8_t>(randomValue >> (byteIndex * 8));
        if (randomByte < TOKEN_RANDOM_LIMIT) {
          CROSSFRONT_SETTINGS.deviceToken[tokenIndex++] =
              TOKEN_ALPHABET[randomByte % (sizeof(TOKEN_ALPHABET) - 1)];
        }
      }
    }
    CROSSFRONT_SETTINGS.deviceToken[TOKEN_LENGTH] = '\0';
    CROSSFRONT_SETTINGS.saveToFile();
  }
}

std::string CrossFrontSetupActivity::getPairingUrl() const {
  std::string base = CROSSFRONT_SETTINGS.getWebUrl();
  if (!base.empty() && base.back() == '/') {
    base.pop_back();
  }

  std::string model = getFriendlyModelName();
  for (char& c : model) {
    if (c == ' ') c = '+';
  }

  const char* cleanId = (strncmp(deviceId, "CF-", 3) == 0) ? (deviceId + 3) : deviceId;

  return base + "/connect?dev=" + cleanId + "&token=" + CROSSFRONT_SETTINGS.deviceToken +
         "&model=" + model +
         "&w=" + std::to_string(BoardConfig::ACTIVE.displayWidth) +
         "&h=" + std::to_string(BoardConfig::ACTIVE.displayHeight);
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
  // Header band + top padding + spacing + QR padding + QR + bottom padding + divider.
  return metrics.topPadding + metrics.headerHeight + 6 + QR_TOP_PAD + QR_SIZE + QR_BOTTOM_PAD + 2;
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
  currentSyncStep = CrossFrontService::SyncStep::CONNECTING_WIFI;
  lastSyncResult = CrossFrontService::SyncResult::OK;
  viewMode = ViewMode::MAIN;

  // Default selection to row 0 (Sync Now)
  nav.selected = 0;

  // Full refresh to prevent e-ink ghosting
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
  const int qrY = topY + QR_TOP_PAD;

  // 1. Right side: QR code (enlarged)
  const int qrX = pageWidth - QR_SIZE - 40;
  const Rect qrBounds(qrX, qrY, QR_SIZE, QR_SIZE);
  QrUtils::drawQrCode(renderer, qrBounds, getPairingUrl());

  // 2. Left side: Webapp address, Device ID, and Token
  const int leftX = 40;
  const int indentX = leftX + 24;

  // Line 1: Web App address
  char webBuf[128];
  snprintf(webBuf, sizeof(webBuf), "%s  (%s)", CROSSFRONT_SETTINGS.getWebUrl(), getFriendlyModelName().c_str());
  const int line1Y = qrY + 14;
  renderer.drawText(UI_12_FONT_ID, leftX, line1Y, webBuf, true, EpdFontFamily::BOLD);

  // Line 2: "Mã thiết bị" (normal text)
  const int line2Y = line1Y + 36;
  renderer.drawText(UI_10_FONT_ID, leftX, line2Y, tr(STR_CROSSFRONT_DEVICE_ID), true, EpdFontFamily::REGULAR);

  // Line 3: <mã thiết bị> (indented, bold, larger text)
  const int line3Y = line2Y + 22;
  renderer.drawText(NOTOSANS_18_FONT_ID, indentX, line3Y, deviceId, true, EpdFontFamily::BOLD);

  // Line 4: "Token" (normal text)
  const int line4Y = line3Y + 40;
  renderer.drawText(UI_10_FONT_ID, leftX, line4Y, tr(STR_CROSSFRONT_TOKEN), true, EpdFontFamily::REGULAR);

  // Line 5: <token> (indented, bold, larger text)
  const int line5Y = line4Y + 22;
  renderer.drawText(NOTOSANS_18_FONT_ID, indentX, line5Y, CROSSFRONT_SETTINGS.deviceToken, true, EpdFontFamily::BOLD);

  // Divider line
  const int dividerY = qrY + QR_SIZE + QR_BOTTOM_PAD;
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
          rowValues[0] = tr(STR_CROSSFRONT_CONNECTING_WIFI);
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
        case CrossFrontService::SyncResult::OK:
          rowValues[0] = tr(STR_CROSSFRONT_SYNC_OK);
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
      rowValues[0] = "";
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

void CrossFrontSetupActivity::onSyncProgress(CrossFrontService::SyncStep step, void* userData) {
  auto* self = static_cast<CrossFrontSetupActivity*>(userData);
  if (self) {
    self->handleSyncStep(step);
  }
}

void CrossFrontSetupActivity::handleSyncStep(CrossFrontService::SyncStep step) {
  currentSyncStep = step;
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
