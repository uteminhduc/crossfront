#pragma once
#include <string>

#include "activities/Activity.h"

class Bitmap;
class HalFile;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout, bool stayAwake)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout), stayAwake(stayAwake) {}
  void onEnter() override;
  void loop() override;
  bool preventAutoSleep() override { return stayAwake; }

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool preserveBackground = false) const;
  bool renderSleepOverlayFile(HalFile& file, const char* pathForLog) const;
  bool renderTransparentOverlayPng(const std::string& path) const;
  bool renderSleepOverlayPath(const std::string& path) const;
  void renderLastScreenSleepScreen() const;
  void renderTransparentCustomSleepScreen() const;
  void renderBlankSleepScreen() const;
  void renderCrossFrontSleepScreen();
  void renderCrossFrontFallbackScreen() const;

  bool fromTimeout = false;
  bool stayAwake = false;
  unsigned long lastCrossFrontRefreshMs = 0;
};
