#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "crossfront/CrossFrontService.h"
#include "crossfront/CrossFrontSettings.h"
#include "network/HttpDownloader.h"

class CrossFrontSyncFilesActivity final : public Activity {
 public:
  struct FileItem {
    std::string id;
    std::string name;
    std::string type;
    std::string folder;
    size_t size = 0;
    bool downloaded = false;
  };

  explicit CrossFrontSyncFilesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override;

 private:
  enum class State : uint8_t {
    CONNECTING_WIFI,
    FETCHING_LIST,
    DOWNLOADING,
    COMPLETE,
    NO_FILES,
    ERROR_STATE,
  };

  bool fetchFileList();
  bool downloadSingleFile(const FileItem& file);
  void markFileDone(const std::string& fileId);
  std::string resolveAndSanitizeTargetFolder(const FileItem& file) const;
  bool ensureTargetFolderExists(const std::string& folder) const;

  State state = State::CONNECTING_WIFI;
  std::string errorMessage;
  std::vector<FileItem> pendingFiles;
  size_t currentFileIndex = 0;
  size_t downloadedSuccessCount = 0;

  size_t fileBytesDownloaded = 0;
  size_t fileBytesTotal = 0;
  bool cancelRequested = false;

  unsigned long lastRenderMs = 0;
  char deviceId[32] = {0};
};
