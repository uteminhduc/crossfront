#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <string>

/**
 * CrossFrontService
 * Module độc lập quản lý toàn bộ logic đồng bộ, kết nối mạng và hiển thị Sleep Screen của CrossFront.
 * Tách biệt hoàn toàn khỏi core của CrossPoint nhằm giảm thiểu conflict khi sync/merge upstream.
 */
class CrossFrontService {
 public:
  static constexpr char SLEEP_BMP_PATH[] = "/.crosspoint/cf_sleep.bmp";
  static constexpr char ETAG_FILE_PATH[] = "/.crosspoint/cf_sleep.etag";
  static constexpr unsigned long MAX_BUDGET_MS = 4800;  // Giới hạn cứng 5s

  /**
   * Đọc ETag đã lưu từ bộ nhớ
   */
  static std::string getSavedEtag();

  /**
   * Lưu ETag mới
   */
  static void saveEtag(const std::string& etag);

  /**
   * Kết nối Wi-Fi nhanh với timeout ngắn
   */
  static bool connectWifiQuick(unsigned long timeoutMs = 2500);

  /**
   * Ngắt Wi-Fi an toàn
   */
  static void disconnectWifi();

  /**
   * Thực hiện conditional fetch (ETag / HTTP 304) và đồng bộ Wi-Fi/cấu hình
   * Trả về true nếu có ảnh mới được tải về, false nếu ảnh không đổi hoặc lỗi/timeout
   */
  static bool fetchSleepImageConditional(unsigned long maxBudgetMs = MAX_BUDGET_MS);

  /**
   * Xử lý chu trình khi thiết bị thức dậy bởi Timer Wakeup (gọi từ main.cpp)
   * Trả về true nếu sự kiện đã được xử lý và sẵn sàng chuyển tiếp vào Deep Sleep
   */
  static bool handleTimerWakeup(HalDisplay& display, GfxRenderer& renderer);

  /**
   * Xử lý hiển thị màn hình Sleep Screen khi máy vào chế độ Sleep (gọi từ SleepActivity.cpp)
   * Trả về true nếu vẽ thành công ảnh CrossFront, false nếu cần fallback về màn hình mặc định
   */
  static bool renderSleepScreen(const GfxRenderer& renderer);

  /**
   * Cài đặt RTC timer wakeup theo chu kỳ đã cấu hình
   */
  static void armSleepTimer();
};
