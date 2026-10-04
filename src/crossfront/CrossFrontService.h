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

  enum class SyncResult : uint8_t {
    OK,
    NO_WIFI_CONFIGURED,
    WIFI_CONNECT_FAILED,
    NOT_PAIRED,
    CONFIG_FETCH_FAILED,
    IMAGE_FETCH_FAILED,
    TIMEOUT
  };

  enum class SyncStep : uint8_t {
    CONNECTING_WIFI,
    FETCHING_CONFIG,
    FETCHING_IMAGE
  };

  using ProgressFn = void (*)(SyncStep step, const char* detail, void* userData);
  using CancelFn = bool (*)(void* userData);

  static bool connectWifiQuick(unsigned long timeoutMs = 2500, ProgressFn onProgress = nullptr,
                               void* userData = nullptr, CancelFn shouldCancel = nullptr);
  static void disconnectWifi();

  static bool fetchSleepImage(unsigned long maxBudgetMs = DEFAULT_SLEEP_NETWORK_TIMEOUT_MS,
                              bool* outNetworkOk = nullptr);

  // Handles RTC timer wakeup event. Returns true if handled.
  static bool handleTimerWakeup(HalDisplay& display, GfxRenderer& renderer);

  // Draws sleep screen image. Returns true on success.
  static bool renderSleepScreen(GfxRenderer& renderer);

  // Immediate sync: connects Wi-Fi, exchanges config/Wi-Fi/OPDS, saves settings (30s fixed budget).
  static SyncResult syncNow(ProgressFn onProgress = nullptr, void* userData = nullptr,
                            unsigned long timeoutMs = MANUAL_SYNC_TIMEOUT_MS);
  static const std::string& getLastSyncedWifi();

  enum class RotateTokenResult : uint8_t {
    OK,
    NO_WIFI_CONFIGURED,
    WIFI_CONNECT_FAILED,
    SERVER_FAILED,
    TIMEOUT
  };

  // Rotates device token and syncs the new token to the backend server.
  static RotateTokenResult rotateToken(const char* newToken, unsigned long timeoutMs = MANUAL_SYNC_TIMEOUT_MS);

  // Configures RTC timer wakeup based on CrossFront's update interval.
  static void armSleepTimer();
};
