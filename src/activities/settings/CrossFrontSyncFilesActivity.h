#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class CrossFrontSyncFilesActivity final : public UiListActivity {
 public:
  explicit CrossFrontSyncFilesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override;

 private:
  enum class State : uint8_t {
    CONNECTING_WIFI,
    FETCHING_LIST,
    BROWSING,
    DOWNLOADING,
    SYNCING_DOWNLOAD_STATUS,
    ERROR_STATE,
  };

  enum class RetryAction : uint8_t {
    NONE,
    CONNECT_WIFI,
    FETCH_LIST,
    SYNC_DOWNLOAD_STATUS,
  };

  struct EbookItem {
    std::string title;
    std::string status;
    std::string mediaId;
    std::string fileName;
    std::string sourceType;
    std::vector<uint8_t> thumbnailBits;
    size_t size = 0;
    uint32_t revision = 0;
    uint32_t crawled = 0;
    uint32_t total = 0;
    bool downloaded = false;
  };

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;
  void drawFooter() override;

  bool fetchPage(uint32_t requestedPage);
  void rebuildRows();
  int itemIndexFromRow(int row) const;
  bool isPreviousRow(int row) const;
  bool isNextRow(int row) const;
  bool isDownloadable(const EbookItem& item) const;
  bool isFilePresent(const EbookItem& item) const;
  std::string itemStatusText(const EbookItem& item) const;
  std::string itemSubtitleText(const EbookItem& item) const;
  void promptDownload(int itemIndex);
  void onDownloadConfirmation(int itemIndex, const ActivityResult& result);
  bool downloadSingleFile(const EbookItem& item, const std::string& downloadId);
  bool syncLocalPresence(const std::vector<EbookItem>& pageItems);
  bool markFileDone(const std::string& mediaId, uint32_t revision, const std::string& downloadId);
  bool ensureTargetFolderExists(const std::string& folder) const;
  void clearPendingDownloadStatus();

  State state = State::CONNECTING_WIFI;
  RetryAction retryAction = RetryAction::NONE;
  std::string errorMessage;
  std::string pendingMediaId;
  std::string pendingDownloadId;
  uint32_t pendingMediaRevision = 0;
  std::vector<EbookItem> items;
  std::vector<std::string> rowLabels;
  std::vector<std::string> rowSubtitles;
  std::vector<std::string> rowValues;
  std::vector<freeink::ui::ListItem> rowItems;
  bool rowsDirty = true;

  uint32_t page = 1;
  uint32_t pendingPage = 1;
  uint32_t perPage = 6;
  uint32_t totalPages = 0;
  int activeDownloadIndex = -1;
  size_t fileBytesDownloaded = 0;
  size_t fileBytesTotal = 0;
  bool cancelRequested = false;
  char deviceId[32] = {0};
  std::unique_ptr<char[]> pageResponseBuffer;
};
