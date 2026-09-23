#pragma once

#include <string>

#include "activities/Activity.h"

class CrossFrontSetupActivity final : public Activity {
 public:
  enum class MenuSelection : uint8_t {
    SYNC_NOW = 0,
    INTERVAL = 1,
    COUNT = 2
  };

  explicit CrossFrontSetupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void ensureTokenGenerated();
  std::string getPairingUrl() const;
  void performManualSync();
  const char* getIntervalLabel() const;

  char deviceId[32] = {0};
  std::string statusMessage;
  bool isSyncing = false;
  MenuSelection selectedMenu = MenuSelection::SYNC_NOW;
};
