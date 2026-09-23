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
constexpr int QR_SIZE = 230;
constexpr int QR_TOP_SPACING = 24;
constexpr int QR_BOTTOM_SPACING = 16;
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
  std::string base = CROSSFRONT_SETTINGS.webUrl[0] != '\0' ? CROSSFRONT_SETTINGS.webUrl : "https://cf.pocketgo.org";
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
  // Header band + top padding + spacing + device info + QR padding + QR + divider.
  return metrics.topPadding + metrics.headerHeight + 6 + 22 + QR_TOP_SPACING + QR_SIZE + QR_BOTTOM_SPACING + 2;
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
  int curY = metrics.topPadding + metrics.headerHeight + 6;

  // Device info: Model | ID | Token
  const char* cleanId = (strncmp(deviceId, "CF-", 3) == 0) ? (deviceId + 3) : deviceId;
  char infoBuf[128];
  snprintf(infoBuf, sizeof(infoBuf), "%s  |  %s  |  %s", getFriendlyModelName().c_str(), cleanId,
           CROSSFRONT_SETTINGS.deviceToken);
  renderer.drawCenteredText(UI_12_FONT_ID, curY, infoBuf, true, EpdFontFamily::BOLD);
  curY += 22 + QR_TOP_SPACING;

  const int qrX = (pageWidth - QR_SIZE) / 2;
  const Rect qrBounds(qrX, curY, QR_SIZE, QR_SIZE);
  QrUtils::drawQrCode(renderer, qrBounds, getPairingUrl());
  curY += QR_SIZE + QR_BOTTOM_SPACING;

  // Divider line
  renderer.drawLine(0, curY, pageWidth - 1, curY, true);
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
      rowValues[0] = tr(STR_CROSSFRONT_SYNCING);
    } else if (syncStatus == SyncStatus::SUCCESS) {
      rowValues[0] = tr(STR_CROSSFRONT_SYNC_OK);
    } else if (syncStatus == SyncStatus::FAILED) {
      rowValues[0] = tr(STR_CROSSFRONT_SYNC_FAILED);
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
    constexpr uint16_t waitOptionsMs[WAIT_ITEM_COUNT] = {3000, 5000, 10000, 15000};
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
      requestUpdate(true);
    } else if (index == 2) {
      viewMode = ViewMode::NETWORK_WAIT;
      nav.reset();
      requestUpdate(true);
    }
    return;
  }

  if (viewMode == ViewMode::INTERVAL) {
    CROSSFRONT_SETTINGS.updateInterval = static_cast<CrossFrontSettings::UpdateInterval>(index);
  } else {
    constexpr uint16_t waitOptionsMs[WAIT_ITEM_COUNT] = {3000, 5000, 10000, 15000};
    if (index < 0 || index >= WAIT_ITEM_COUNT) return;
    CROSSFRONT_SETTINGS.sleepNetworkTimeoutMs = waitOptionsMs[index];
  }

  CROSSFRONT_SETTINGS.saveToFile();
  viewMode = ViewMode::MAIN;
  nav.reset();
  requestUpdate(true);
}

void CrossFrontSetupActivity::onBackButton() {
  if (viewMode != ViewMode::MAIN) {
    viewMode = ViewMode::MAIN;
    nav.reset();
    requestUpdate(true);
    return;
  }
  finish();
}

void CrossFrontSetupActivity::performManualSync() {
  syncStatus = SyncStatus::SYNCING;
  requestUpdateAndWait();

  const auto result = CrossFrontService::syncNow();
  if (result == CrossFrontService::SyncResult::OK) {
    syncStatus = SyncStatus::SUCCESS;
  } else {
    syncStatus = SyncStatus::FAILED;
    LOG_ERR("CF", "Manual sync failed: %d", static_cast<int>(result));
  }

  requestUpdate();
}
