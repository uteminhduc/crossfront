#pragma once

#include <string>

#include "CrossPointSettings.h"
#include "activities/UiListActivity.h"

class CrossFrontSetupActivity final : public UiListActivity {
 public:
  static constexpr int TOTAL_ITEMS = 13;  // 0 = Sync now, 1..12 = 12 update interval options

  explicit CrossFrontSetupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 protected:
  int listCount() const override { return TOTAL_ITEMS; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void drawChrome() override;

 private:
  void ensureTokenGenerated();
  std::string getPairingUrl() const;
  void performManualSync();
  const char* getIntervalLabel(uint8_t interval) const;
  std::string getFriendlyModelName() const;
  int computeQrSectionHeight() const;

  char deviceId[32] = {0};
  enum class SyncStatus : uint8_t { IDLE, SYNCING, SUCCESS, FAILED };
  SyncStatus syncStatus = SyncStatus::IDLE;

  freeink::ui::ListItem rowItems[TOTAL_ITEMS]{};
  std::string rowValues[TOTAL_ITEMS]{};
};
