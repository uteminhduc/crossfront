#include "CrossFrontSyncFilesActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SecureHttpClient.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
std::vector<std::pair<std::string, std::string>> makeHeaders(const char* deviceId, const char* token) {
  std::vector<std::pair<std::string, std::string>> headers;
  headers.reserve(2);
  headers.emplace_back("X-Device-Id", deviceId);
  if (token && token[0] != '\0') {
    headers.emplace_back("X-Device-Token", token);
  }
  return headers;
}

void formatSize(const size_t bytes, char* out, const size_t outSize) {
  if (bytes < 1024) {
    snprintf(out, outSize, "%u B", static_cast<unsigned>(bytes));
  } else if (bytes < 1024 * 1024) {
    snprintf(out, outSize, "%.1f KB", static_cast<float>(bytes) / 1024.0f);
  } else {
    snprintf(out, outSize, "%.1f MB", static_cast<float>(bytes) / (1024.0f * 1024.0f));
  }
}
}  // namespace

CrossFrontSyncFilesActivity::CrossFrontSyncFilesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("CrossFrontSyncFiles", renderer, mappedInput) {}

void CrossFrontSyncFilesActivity::onEnter() {
  Activity::onEnter();
  state = State::CONNECTING_WIFI;
  errorMessage = "";
  pendingFiles.clear();
  currentFileIndex = 0;
  downloadedSuccessCount = 0;
  fileBytesDownloaded = 0;
  fileBytesTotal = 0;
  cancelRequested = false;

  CROSSFRONT_SETTINGS.loadFromFile();
  CROSSFRONT_SETTINGS.getDeviceId(deviceId, sizeof(deviceId));

  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }

  requestUpdate(true);
}

void CrossFrontSyncFilesActivity::onExit() {
  CrossFrontService::disconnectWifi();
  Activity::onExit();
}

bool CrossFrontSyncFilesActivity::skipLoopDelay() {
  return state == State::CONNECTING_WIFI || state == State::FETCHING_LIST || state == State::DOWNLOADING;
}

void CrossFrontSyncFilesActivity::loop() {
  int touchX = 0;
  int touchY = 0;
  const bool screenTapped = mappedInput.wasScreenTapped(touchX, touchY);
  const bool backPressed = mappedInput.wasReleased(MappedInputManager::Button::Back);
  const bool confirmPressed = mappedInput.wasReleased(MappedInputManager::Button::Confirm);

  if (state == State::COMPLETE || state == State::NO_FILES || state == State::ERROR_STATE) {
    if (backPressed || confirmPressed || screenTapped) {
      finish();
      return;
    }
  } else if (state == State::CONNECTING_WIFI) {
    if (backPressed || screenTapped) {
      finish();
      return;
    }
  } else if (state == State::DOWNLOADING) {
    if (backPressed || screenTapped) {
      cancelRequested = true;
      return;
    }
  }

  switch (state) {
    case State::CONNECTING_WIFI: {
      const bool wifiOk = CrossFrontService::connectWifiQuick(15000);
      if (!wifiOk) {
        state = State::ERROR_STATE;
        errorMessage = tr(STR_CROSSFRONT_ERR_WIFI);
        requestUpdate(true);
        return;
      }
      state = State::FETCHING_LIST;
      requestUpdate(true);
      return;
    }

    case State::FETCHING_LIST: {
      const bool listOk = fetchFileList();
      if (!listOk) {
        state = State::ERROR_STATE;
        if (errorMessage.empty()) {
          errorMessage = tr(STR_CROSSFRONT_ERR_SERVER);
        }
        requestUpdate(true);
        return;
      }
      if (pendingFiles.empty()) {
        state = State::NO_FILES;
        requestUpdate(true);
        return;
      }
      state = State::DOWNLOADING;
      currentFileIndex = 0;
      requestUpdate(true);
      return;
    }

    case State::DOWNLOADING: {
      if (cancelRequested) {
        finish();
        return;
      }

      if (currentFileIndex < pendingFiles.size()) {
        const auto& file = pendingFiles[currentFileIndex];
        const bool ok = downloadSingleFile(file);
        if (cancelRequested) {
          finish();
          return;
        }

        if (ok) {
          markFileDone(file.id);
          downloadedSuccessCount++;
        }

        currentFileIndex++;
        if (currentFileIndex < pendingFiles.size()) {
          requestUpdate(true);
        } else {
          state = State::COMPLETE;
          requestUpdate(true);
        }
      }
      return;
    }

    case State::COMPLETE:
    case State::NO_FILES:
    case State::ERROR_STATE:
      break;
  }
}

bool CrossFrontSyncFilesActivity::fetchFileList() {
  const char* serverUrl = CROSSFRONT_SETTINGS.getServerUrl();
  std::string server = std::string(serverUrl);
  if (!server.empty() && server.back() == '/') {
    server.pop_back();
  }

  const char* token = (CROSSFRONT_SETTINGS.deviceToken[0] != '\0') ? CROSSFRONT_SETTINGS.deviceToken : deviceId;
  const char* cleanId = (strncmp(deviceId, "CF-", 3) == 0) ? (deviceId + 3) : deviceId;

  std::string filesUrl = server + "/api/cf/device/" + cleanId + "/files";
  std::string jsonBody;

  freeink::SecureHttpClient http;
  http.setTimeout(15000);
  http.setInsecure();
  if (!http.begin(filesUrl)) {
    errorMessage = tr(STR_CROSSFRONT_ERR_SERVER);
    return false;
  }

  http.setUserAgent("CrossPoint-ESP32");
  auto extraHeaders = makeHeaders(deviceId, token);
  for (const auto& h : extraHeaders) {
    http.addHeader(h.first.c_str(), h.second.c_str());
  }

  const int httpCode = http.sendRequest("GET", nullptr, 0, [&jsonBody](const uint8_t* data, size_t len) {
    jsonBody.append(reinterpret_cast<const char*>(data), len);
    return true;
  });
  http.end();

  if (httpCode != 200) {
    LOG_ERR("CF", "fetchFileList failed: HTTP %d", httpCode);
    if (httpCode == 401 || httpCode == 404) {
      errorMessage = tr(STR_CROSSFRONT_ERR_NOT_PAIRED);
    } else {
      errorMessage = tr(STR_CROSSFRONT_ERR_SERVER);
    }
    return false;
  }

  JsonDocument doc;
  const auto err = deserializeJson(doc, jsonBody);
  if (err != DeserializationError::Ok) {
    LOG_ERR("CF", "fetchFileList: JSON parse error");
    errorMessage = tr(STR_CROSSFRONT_ERR_SERVER);
    return false;
  }

  pendingFiles.clear();
  if (doc.is<JsonArray>()) {
    const auto arr = doc.as<JsonArray>();
    pendingFiles.reserve(arr.size());
    for (JsonObject obj : arr) {
      const bool downloaded = obj["downloaded"] | false;
      if (!downloaded) {
        FileItem item;
        item.id = obj["id"] | "";
        item.name = obj["fileName"] | obj["name"] | "";
        item.type = obj["type"] | "book";
        item.folder = obj["folder"] | "";
        item.size = obj["size"] | 0;
        item.downloaded = false;
        if (!item.id.empty() && !item.name.empty()) {
          pendingFiles.push_back(std::move(item));
        }
      }
    }
  }

  LOG_INF("CF", "Found %u pending files to download", static_cast<unsigned>(pendingFiles.size()));
  return true;
}

namespace {

std::string extractFontFamilyName(const std::string& filename) {
  const size_t dotPos = filename.rfind('.');
  std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;

  const size_t underPos = base.rfind('_');
  if (underPos != std::string::npos && underPos > 0) {
    bool allDigits = true;
    for (size_t i = underPos + 1; i < base.length(); ++i) {
      if (!isdigit(static_cast<unsigned char>(base[i]))) {
        allDigits = false;
        break;
      }
    }
    if (allDigits && underPos + 1 < base.length()) {
      return base.substr(0, underPos);
    }
  }
  return base;
}

}  // namespace

std::string CrossFrontSyncFilesActivity::resolveAndSanitizeTargetFolder(const FileItem& file) const {
  if (file.type == "font") {
    if (!file.folder.empty() && (file.folder.rfind("/.fonts", 0) == 0 || file.folder.rfind("/fonts", 0) == 0)) {
      return file.folder;
    }
    const char* root = Storage.exists("/.fonts") ? "/.fonts" : (Storage.exists("/fonts") ? "/fonts" : "/.fonts");
    std::string family = extractFontFamilyName(file.name);
    if (!family.empty()) {
      return std::string(root) + "/" + family;
    }
    return std::string(root);
  }
  if (file.type == "sleep") {
    if (Storage.exists("/sleep")) {
      return "/sleep";
    }
    if (Storage.exists("/.sleep")) {
      return "/.sleep";
    }
    return "/sleep";
  }

  std::string folder = file.folder;
  if (folder.empty()) {
    folder = CROSSFRONT_SETTINGS.getEbookDir();
    // Auto-detect existing "/Books" on user's SD card if not explicitly overridden
    if (folder == CrossFrontSettings::DEFAULT_EBOOK_DIR && Storage.exists("/Books")) {
      folder = "/Books";
    }
  }

  while (!folder.empty() && (folder.front() == ' ' || folder.front() == '\t')) {
    folder.erase(folder.begin());
  }
  while (!folder.empty() && (folder.back() == ' ' || folder.back() == '\t')) {
    folder.pop_back();
  }

  for (auto& c : folder) {
    if (c == '\\') c = '/';
  }

  // Never allow empty or root "/"
  if (folder.empty() || folder == "/") {
    folder = CrossFrontSettings::DEFAULT_EBOOK_DIR;
  }

  if (folder.front() != '/') {
    folder = "/" + folder;
  }

  while (folder.length() > 1 && folder.back() == '/') {
    folder.pop_back();
  }

  if (folder == "/") {
    folder = CrossFrontSettings::DEFAULT_EBOOK_DIR;
  }

  return folder;
}

bool CrossFrontSyncFilesActivity::ensureTargetFolderExists(const std::string& folder) const {
  if (folder.empty() || folder == "/") {
    return false;
  }

  if (Storage.exists(folder.c_str())) {
    return true;
  }

  LOG_INF("CF", "Target directory does not exist, creating: %s", folder.c_str());

  bool ok = Storage.ensureDirectoryExists(folder.c_str());
  if (!ok) {
    ok = Storage.mkdir(folder.c_str(), true);
  }

  if (!Storage.exists(folder.c_str())) {
    std::string partial = "";
    for (size_t i = 0; i < folder.length(); ++i) {
      if (folder[i] == '/' && !partial.empty()) {
        if (!Storage.exists(partial.c_str())) {
          Storage.mkdir(partial.c_str(), false);
        }
      }
      partial += folder[i];
    }
    if (!partial.empty() && !Storage.exists(partial.c_str())) {
      Storage.mkdir(partial.c_str(), false);
    }
  }

  const bool verified = Storage.exists(folder.c_str());
  if (!verified) {
    LOG_ERR("CF", "Failed to create target directory on SD: %s", folder.c_str());
  }
  return verified;
}

bool CrossFrontSyncFilesActivity::downloadSingleFile(const FileItem& file) {
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("CF", "Low heap before file download (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }

  const std::string folder = resolveAndSanitizeTargetFolder(file);
  if (!ensureTargetFolderExists(folder)) {
    LOG_ERR("CF", "Target folder missing and cannot be created: %s", folder.c_str());
    return false;
  }

  std::string destPath = folder + "/" + file.name;

  // Guard: Must not save directly to root
  if (destPath.length() <= 1 || destPath.find('/', 1) == std::string::npos) {
    LOG_ERR("CF", "Illegal root destination rejected: %s", destPath.c_str());
    destPath = std::string(CrossFrontSettings::DEFAULT_EBOOK_DIR) + "/" + file.name;
    ensureTargetFolderExists(CrossFrontSettings::DEFAULT_EBOOK_DIR);
  }

  const std::string tmpPath = destPath + ".tmp";
  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  const char* serverUrl = CROSSFRONT_SETTINGS.getServerUrl();
  std::string server = std::string(serverUrl);
  if (!server.empty() && server.back() == '/') {
    server.pop_back();
  }

  const char* token = (CROSSFRONT_SETTINGS.deviceToken[0] != '\0') ? CROSSFRONT_SETTINGS.deviceToken : deviceId;
  const char* cleanId = (strncmp(deviceId, "CF-", 3) == 0) ? (deviceId + 3) : deviceId;
  std::string downloadUrl = server + "/api/cf/device/" + cleanId + "/files/" + file.id + "/download";

  auto extraHeaders = makeHeaders(deviceId, token);

  fileBytesDownloaded = 0;
  fileBytesTotal = file.size;
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = millis();

  LOG_INF("CF", "Downloading %s -> %s (temp: %s)", downloadUrl.c_str(), destPath.c_str(), tmpPath.c_str());

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, tmpPath,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        fileBytesDownloaded = downloaded;
        if (total > 0) fileBytesTotal = total;

        mappedInput.update(true);
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          cancelRequested = true;
        }

        const int percent = fileBytesTotal > 0 ? static_cast<int>(fileBytesDownloaded * 100 / fileBytesTotal) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 || percent >= lastRenderedPercent + 5 ||
            now - lastProgressUpdateMs >= 2000) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      &cancelRequested, "", "", false, 120000, "", nullptr, extraHeaders);

  if (cancelRequested || result != HttpDownloader::OK) {
    LOG_ERR("CF", "Download failed or cancelled (err %d, cancel %d). Cleaning up temp file: %s",
            static_cast<int>(result), cancelRequested ? 1 : 0, tmpPath.c_str());
    if (Storage.exists(tmpPath.c_str())) {
      Storage.remove(tmpPath.c_str());
    }
    return false;
  }

  HalFile checkTmp;
  if (!Storage.openFileForRead("CF", tmpPath, checkTmp)) {
    LOG_ERR("CF", "Cannot open downloaded temp file: %s", tmpPath.c_str());
    if (Storage.exists(tmpPath.c_str())) {
      Storage.remove(tmpPath.c_str());
    }
    return false;
  }
  const size_t actualBytes = checkTmp.size();
  checkTmp.close();

  if (actualBytes == 0) {
    LOG_ERR("CF", "Downloaded temp file is 0 bytes: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
    LOG_ERR("CF", "Failed to commit temp file %s to %s", tmpPath.c_str(), destPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  LOG_INF("CF", "Successfully saved %s (%u bytes)", destPath.c_str(), static_cast<unsigned>(actualBytes));
  return true;
}

void CrossFrontSyncFilesActivity::markFileDone(const std::string& fileId) {
  const char* serverUrl = CROSSFRONT_SETTINGS.getServerUrl();
  std::string server = std::string(serverUrl);
  if (!server.empty() && server.back() == '/') {
    server.pop_back();
  }

  const char* token = (CROSSFRONT_SETTINGS.deviceToken[0] != '\0') ? CROSSFRONT_SETTINGS.deviceToken : deviceId;
  const char* cleanId = (strncmp(deviceId, "CF-", 3) == 0) ? (deviceId + 3) : deviceId;
  std::string doneUrl = server + "/api/cf/device/" + cleanId + "/files/" + fileId + "/done";

  freeink::SecureHttpClient http;
  http.setTimeout(10000);
  http.setInsecure();
  if (http.begin(doneUrl)) {
    http.setUserAgent("CrossPoint-ESP32");
    auto extraHeaders = makeHeaders(deviceId, token);
    for (const auto& h : extraHeaders) {
      http.addHeader(h.first.c_str(), h.second.c_str());
    }
    http.sendRequest("POST", nullptr, 0, nullptr);
    http.end();
  }
}

void CrossFrontSyncFilesActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CROSSFRONT_SYNC_FILES));

  const auto fontHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - fontHeight) / 2;

  switch (state) {
    case State::CONNECTING_WIFI: {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_CROSSFRONT_CONNECTING_WIFI));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    case State::FETCHING_LIST: {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_CROSSFRONT_SYNCING_FILES));
      break;
    }

    case State::DOWNLOADING: {
      if (currentFileIndex < pendingFiles.size()) {
        const auto& file = pendingFiles[currentFileIndex];
        char titleBuf[128];
        snprintf(titleBuf, sizeof(titleBuf), "[%u/%u] %s",
                 static_cast<unsigned>(currentFileIndex + 1),
                 static_cast<unsigned>(pendingFiles.size()),
                 file.name.c_str());

        const int topY = pageHeight / 3;
        renderer.drawCenteredText(UI_10_FONT_ID, topY, titleBuf, true, EpdFontFamily::BOLD);

        const int percent = fileBytesTotal > 0 ? static_cast<int>(fileBytesDownloaded * 100 / fileBytesTotal) : 0;
        int barY = topY + fontHeight + metrics.verticalSpacing * 2;
        GUI.drawProgressBar(renderer,
                            Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2,
                                 metrics.progressBarHeight},
                            percent, 100);

        char sizeBuf[64];
        char downStr[32];
        char totStr[32];
        formatSize(fileBytesDownloaded, downStr, sizeof(downStr));
        formatSize(fileBytesTotal, totStr, sizeof(totStr));
        snprintf(sizeBuf, sizeof(sizeBuf), "%s / %s (%d%%)", downStr, totStr, percent);

        barY += metrics.progressBarHeight + metrics.verticalSpacing + fontHeight;
        renderer.drawCenteredText(UI_10_FONT_ID, barY, sizeBuf);

        const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      }
      break;
    }

    case State::NO_FILES: {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_CROSSFRONT_NO_NEW_FILES), true, EpdFontFamily::BOLD);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    case State::COMPLETE: {
      char doneBuf[64];
      snprintf(doneBuf, sizeof(doneBuf), tr(STR_CROSSFRONT_FILES_SYNC_OK), static_cast<unsigned>(downloadedSuccessCount));
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, doneBuf, true, EpdFontFamily::BOLD);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }

    case State::ERROR_STATE: {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY - fontHeight, tr(STR_CROSSFRONT_ERR_SERVER), true, EpdFontFamily::BOLD);
      if (!errorMessage.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, centerY + fontHeight, errorMessage.c_str());
      }
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }

  renderer.displayBuffer();
}
