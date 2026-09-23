#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <string>

// Service managing CrossFront sync, network, and sleep screen workflows.
class CrossFrontService {
 public:
  static constexpr char SLEEP_BMP_PATH[] = "/.crosspoint/cf_sleep.bmp";
  static constexpr char ETAG_FILE_PATH[] = "/.crosspoint/cf_sleep.etag";
  static constexpr unsigned long DEFAULT_SLEEP_NETWORK_TIMEOUT_MS = 10000;

  static std::string getSavedEtag();
  static void saveEtag(const std::string& etag);

  static bool connectWifiQuick(unsigned long timeoutMs = 2500);
  static void disconnectWifi();

  // Conditional fetch using ETag (HTTP 304). Returns true if new image downloaded.
  static bool fetchSleepImageConditional(unsigned long maxBudgetMs = DEFAULT_SLEEP_NETWORK_TIMEOUT_MS);

  // Handles RTC timer wakeup event. Returns true if handled.
  static bool handleTimerWakeup(HalDisplay& display, GfxRenderer& renderer);

  // Draws sleep screen image. Returns true on success.
  static bool renderSleepScreen(const GfxRenderer& renderer);

  enum class SyncResult : uint8_t {
    OK,
    NO_WIFI_CONFIGURED,
    WIFI_CONNECT_FAILED,
    CONFIG_FETCH_FAILED,
    IMAGE_FETCH_FAILED
  };

  // Immediate sync: connects Wi-Fi, fetches config/wifi_list & image, saves settings.
  static SyncResult syncNow();

  // Configures RTC timer wakeup based on CrossFront's update interval.
  static void armSleepTimer();
};
