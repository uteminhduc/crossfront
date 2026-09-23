#pragma once

#include <string>

#include "activities/UiListActivity.h"
#include "crossfront/CrossFrontSettings.h"

class CrossFrontSetupActivity final : public UiListActivity {
 public:
  static constexpr int MAIN_ITEM_COUNT = 3;
  static constexpr int WAIT_ITEM_COUNT = 4;
  static constexpr int MAX_ITEM_COUNT = CrossFrontSettings::UPDATE_INTERVAL_COUNT;

  explicit CrossFrontSetupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void onBackButton() override;
  void drawChrome() override;

 private:
  void ensureTokenGenerated();
  std::string getPairingUrl() const;
  void performManualSync();
  const char* getIntervalLabel(uint8_t interval) const;
  std::string getNetworkWaitLabel(uint16_t timeoutMs) const;
  std::string getFriendlyModelName() const;
  int computeQrSectionHeight() const;

  enum class ViewMode : uint8_t { MAIN, INTERVAL, NETWORK_WAIT };
  ViewMode viewMode = ViewMode::MAIN;

  char deviceId[32] = {0};
  enum class SyncStatus : uint8_t { IDLE, SYNCING, SUCCESS, FAILED };
  SyncStatus syncStatus = SyncStatus::IDLE;

  freeink::ui::ListItem rowItems[MAX_ITEM_COUNT]{};
  std::string rowValues[MAX_ITEM_COUNT]{};
};
