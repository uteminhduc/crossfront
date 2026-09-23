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

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "crossfront/CrossFrontService.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/QrUtils.h"

namespace fui = freeink::ui;

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
  SETTINGS.getCfDeviceId(deviceId, sizeof(deviceId));

  if (SETTINGS.cfDeviceToken[0] == '\0') {
    uint32_t r1 = esp_random();
    snprintf(SETTINGS.cfDeviceToken, sizeof(SETTINGS.cfDeviceToken), "%08X", (unsigned int)r1);
    SETTINGS.saveToFile();
  }
}

std::string CrossFrontSetupActivity::getPairingUrl() const {
  std::string base = SETTINGS.cfWebUrl[0] != '\0' ? SETTINGS.cfWebUrl : "https://cf.pocketgo.org";
  if (!base.empty() && base.back() == '/') {
    base.pop_back();
  }

  std::string model = getFriendlyModelName();
  for (char& c : model) {
    if (c == ' ') c = '+';
  }

  const char* cleanId = (strncmp(deviceId, "CF-", 3) == 0) ? (deviceId + 3) : deviceId;

  return base + "/connect?dev=" + cleanId + "&token=" + SETTINGS.cfDeviceToken +
         "&model=" + model +
         "&w=" + std::to_string(BoardConfig::ACTIVE.displayWidth) +
         "&h=" + std::to_string(BoardConfig::ACTIVE.displayHeight);
}

const char* CrossFrontSetupActivity::getIntervalLabel(uint8_t interval) const {
  switch (interval) {
    case CrossPointSettings::CF_1_MIN: return tr(STR_CROSSFRONT_1_MIN);
    case CrossPointSettings::CF_2_MIN: return tr(STR_CROSSFRONT_2_MIN);
    case CrossPointSettings::CF_5_MIN: return tr(STR_CROSSFRONT_5_MIN);
    case CrossPointSettings::CF_15_MIN: return tr(STR_CROSSFRONT_15_MIN);
    case CrossPointSettings::CF_30_MIN: return tr(STR_CROSSFRONT_30_MIN);
    case CrossPointSettings::CF_1_HOUR: return tr(STR_CROSSFRONT_1_HOUR);
    case CrossPointSettings::CF_2_HOURS: return tr(STR_CROSSFRONT_2_HOURS);
    case CrossPointSettings::CF_3_HOURS: return tr(STR_CROSSFRONT_3_HOURS);
    case CrossPointSettings::CF_6_HOURS: return tr(STR_CROSSFRONT_6_HOURS);
    case CrossPointSettings::CF_12_HOURS: return tr(STR_CROSSFRONT_12_HOURS);
    case CrossPointSettings::CF_1_DAY: return tr(STR_CROSSFRONT_24_HOURS);
    case CrossPointSettings::CF_ON_SLEEP:
    default:
      return tr(STR_CROSSFRONT_ON_SLEEP);
  }
}

int CrossFrontSetupActivity::computeQrSectionHeight() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Header band + top padding + Spacing(6) + Device Info(22) + Hint(18) + QR(200) + Spacing(10) + Divider line(2)
  return metrics.topPadding + metrics.headerHeight + 6 + 22 + 18 + 200 + 10 + 2;
}

const char* CrossFrontSetupActivity::headerTitle() const {
  return tr(STR_CROSSFRONT_SETUP);
}

void CrossFrontSetupActivity::onEnter() {
  UiListActivity::onEnter();
  ensureTokenGenerated();
  syncStatus = SyncStatus::IDLE;

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

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  int curY = metrics.topPadding + metrics.headerHeight + 6;

  // Device info: Model | ID | Token
  const char* cleanId = (strncmp(deviceId, "CF-", 3) == 0) ? (deviceId + 3) : deviceId;
  char infoBuf[128];
  snprintf(infoBuf, sizeof(infoBuf), "%s  |  %s  |  %s", getFriendlyModelName().c_str(), cleanId, SETTINGS.cfDeviceToken);
  renderer.drawCenteredText(UI_12_FONT_ID, curY, infoBuf, true, EpdFontFamily::BOLD);
  curY += 22 + 18; // Preserve exact QR vertical position without hint text

  // QR Code (size 200)
  const int qrSize = 200;
  const int qrX = (pageWidth - qrSize) / 2;
  const Rect qrBounds(qrX, curY, qrSize, qrSize);
  QrUtils::drawQrCode(renderer, qrBounds, getPairingUrl());
  curY += qrSize + 10;

  // Divider line
  renderer.drawLine(0, curY, pageWidth - 1, curY, true);
}

void CrossFrontSetupActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int topMargin = computeQrSectionHeight();

  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(topMargin),
      0,
      static_cast<int16_t>(metrics.buttonHintsHeight + metrics.verticalSpacing),
      0});

  // Row 0: Sync Now
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

  // Rows 1..12: Update interval options with section heading
  for (int i = 1; i < TOTAL_ITEMS; ++i) {
    const int optIdx = i - 1;
    rowItems[i].sectionHeading = (i == 1) ? tr(STR_CROSSFRONT_UPDATE_AFTER_SLEEP) : nullptr;
    rowItems[i].label = getIntervalLabel(static_cast<uint8_t>(optIdx));
    rowValues[i] = (SETTINGS.cfUpdateInterval == optIdx) ? tr(STR_SELECTED) : "";
    rowItems[i].value = rowValues[i].empty() ? nullptr : rowValues[i].c_str();
    rowItems[i].actionValue = static_cast<int16_t>(i);
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = TOTAL_ITEMS;
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
  if (index == 0) {
    performManualSync();
  } else if (index >= 1 && index < TOTAL_ITEMS) {
    SETTINGS.cfUpdateInterval = static_cast<CrossPointSettings::CROSSFRONT_INTERVAL>(index - 1);
    SETTINGS.saveToFile();
    requestUpdate();
  }
}

void CrossFrontSetupActivity::performManualSync() {
  syncStatus = SyncStatus::SYNCING;
  requestUpdateAndWait();

  const auto result = CrossFrontService::syncNow();
  if (result == CrossFrontService::SyncResult::OK) {
    syncStatus = SyncStatus::SUCCESS;
  } else {
    syncStatus = SyncStatus::FAILED;
  }

  requestUpdate();
}
