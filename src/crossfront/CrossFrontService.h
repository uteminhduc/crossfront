#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <string>

// Service managing CrossFront sync, network, and sleep screen workflows.
class CrossFrontService {
 public:
  static constexpr char SLEEP_BMP_PATH[] = "/.crosspoint/cf_sleep.bmp";
  static constexpr char ETAG_FILE_PATH[] = "/.crosspoint/cf_sleep.etag";
  static constexpr unsigned long DEFAULT_SLEEP_NETWORK_TIMEOUT_MS = 15000;
  static constexpr unsigned long MANUAL_SYNC_TIMEOUT_MS = 30000;

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
    IMAGE_FETCH_FAILED,
    TIMEOUT
  };

  enum class SyncStep : uint8_t {
    CONNECTING_WIFI,
    FETCHING_CONFIG,
    FETCHING_IMAGE
  };

  using ProgressFn = void (*)(SyncStep step, void* userData);

  // Immediate sync: connects Wi-Fi, fetches config/wifi_list & image, saves settings (30s fixed budget).
  static SyncResult syncNow(ProgressFn onProgress = nullptr, void* userData = nullptr,
                            unsigned long timeoutMs = MANUAL_SYNC_TIMEOUT_MS);

  // Configures RTC timer wakeup based on CrossFront's update interval.
  static void armSleepTimer();
};
