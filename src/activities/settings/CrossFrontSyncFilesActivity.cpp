#include "CrossFrontSyncFilesActivity.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <esp_system.h>
#include <mbedtls/base64.h>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/HeaderBackTapTarget.h"
#include "components/UITheme.h"
#include "crossfront/CrossFrontFileSafety.h"
#include "crossfront/CrossFrontService.h"
#include "crossfront/CrossFrontSettings.h"
#include "crossfront/CrossFrontSyncControl.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr size_t MAX_PAGE_RESPONSE_BYTES = 32 * 1024;
constexpr uint32_t MAX_PAGE_ITEMS = 6;
constexpr uint32_t MAX_TOTAL_PAGES = 10000;
constexpr uint16_t THUMBNAIL_WIDTH = 40;
constexpr uint16_t THUMBNAIL_HEIGHT = 56;
constexpr size_t MAX_THUMBNAIL_BMP_BYTES = 4096;
constexpr const char* PAGE_RESPONSE_PATH = "/cf_ebooks_page.tmp";

uint16_t readLittleEndian16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLittleEndian32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

bool decodeThumbnailBmp(const char* encoded,
                        std::array<uint8_t, THUMBNAIL_WIDTH * THUMBNAIL_HEIGHT / 8>& output) {
  output.fill(0);
  if (!encoded || encoded[0] == '\0') return false;
  const size_t encodedLength = std::strlen(encoded);
  size_t decodedLength = 0;
  int result = mbedtls_base64_decode(nullptr, 0, &decodedLength,
                                     reinterpret_cast<const unsigned char*>(encoded), encodedLength);
  if ((result != 0 && result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) || decodedLength < 62 ||
      decodedLength > MAX_THUMBNAIL_BMP_BYTES) {
    return false;
  }
  auto bmp = makeUniqueNoThrow<uint8_t[]>(decodedLength);
  if (!bmp) {
    LOG_ERR("CF", "OOM decoding %u-byte ebook thumbnail", static_cast<unsigned>(decodedLength));
    return false;
  }
  result = mbedtls_base64_decode(bmp.get(), decodedLength, &decodedLength,
                                 reinterpret_cast<const unsigned char*>(encoded), encodedLength);
  if (result != 0 || decodedLength < 62 || bmp[0] != 'B' || bmp[1] != 'M') return false;
  const uint32_t pixelOffset = readLittleEndian32(&bmp[10]);
  const uint32_t width = readLittleEndian32(&bmp[18]);
  const uint32_t height = readLittleEndian32(&bmp[22]);
  const uint16_t planes = readLittleEndian16(&bmp[26]);
  const uint16_t bitsPerPixel = readLittleEndian16(&bmp[28]);
  const uint32_t compression = readLittleEndian32(&bmp[30]);
  if (width != THUMBNAIL_WIDTH || height != THUMBNAIL_HEIGHT || planes != 1 || bitsPerPixel != 1 ||
      compression != 0 || pixelOffset < 62 || pixelOffset > decodedLength) {
    return false;
  }
  const size_t sourceStride = ((width + 31) / 32) * 4;
  if (sourceStride * height > decodedLength - pixelOffset) return false;

  const size_t destinationStride = (THUMBNAIL_WIDTH + 7) / 8;
  for (uint16_t y = 0; y < THUMBNAIL_HEIGHT; ++y) {
    const uint8_t* sourceRow = bmp.get() + pixelOffset + (THUMBNAIL_HEIGHT - 1 - y) * sourceStride;
    uint8_t* destinationRow = output.data() + y * destinationStride;
    for (uint16_t x = 0; x < THUMBNAIL_WIDTH; ++x) {
      const bool white = ((sourceRow[x / 8] >> (7 - x % 8)) & 0x01) != 0;
      if (!white) destinationRow[x / 8] |= 1 << (7 - x % 8);
    }
  }
  return true;
}

std::vector<std::pair<std::string, std::string>> makeHeaders(const char* token) {
  std::vector<std::pair<std::string, std::string>> headers;
  headers.reserve(3);
  if (token && token[0] != '\0') headers.emplace_back("X-Device-Token", token);
  return headers;
}

// The streaming SecureHttpClient overload invokes its data callback whenever
// the server sends a response body, including the small JSON ACKs from these
// endpoints. Keep a real sink callback instead of passing an empty function.
bool discardResponseBody(const uint8_t*, size_t) { return true; }

std::string formatSize(const size_t bytes) {
  char out[32];
  if (bytes < 1024) {
    snprintf(out, sizeof(out), "%u B", static_cast<unsigned>(bytes));
  } else if (bytes < 1024 * 1024) {
    snprintf(out, sizeof(out), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(out, sizeof(out), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  }
  return out;
}

bool isKnownStatus(const std::string_view status) {
  return status == "pending" || status == "crawling" || status == "packaging" || status == "ready" ||
         status == "failed";
}

std::string makeDownloadId(const char* deviceId) {
  char id[80];
  snprintf(id, sizeof(id), "%s-%08lx-%08lx", deviceId ? deviceId : "device",
           static_cast<unsigned long>(millis()), static_cast<unsigned long>(esp_random()));
  return id;
}
}  // namespace

CrossFrontSyncFilesActivity::CrossFrontSyncFilesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("CrossFrontSyncFiles", renderer, mappedInput) {}

void CrossFrontSyncFilesActivity::onEnter() {
  UiListActivity::onEnter();
  state = State::CONNECTING_WIFI;
  retryAction = RetryAction::NONE;
  errorMessage.clear();
  clearPendingDownloadStatus();
  items.clear();
  rowLabels.clear();
  rowSubtitles.clear();
  rowValues.clear();
  rowItems.clear();
  rowsDirty = true;
  page = 1;
  pendingPage = 1;
  totalPages = 0;
  activeDownloadIndex = -1;
  fileBytesDownloaded = 0;
  fileBytesTotal = 0;
  cancelRequested = false;
  errorInputArmed = false;

  CROSSFRONT_SETTINGS.loadFromFile();
  CROSSFRONT_SETTINGS.getDeviceId(deviceId, sizeof(deviceId));
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  requestUpdate(true);
}

void CrossFrontSyncFilesActivity::onExit() {
  CrossFrontService::disconnectWifi();
  if (Storage.exists(PAGE_RESPONSE_PATH)) Storage.remove(PAGE_RESPONSE_PATH);
  Activity::onExit();
}

bool CrossFrontSyncFilesActivity::skipLoopDelay() {
  return state == State::CONNECTING_WIFI || state == State::FETCHING_LIST || state == State::DOWNLOADING ||
         state == State::SYNCING_DOWNLOAD_STATUS;
}

int CrossFrontSyncFilesActivity::listCount() const {
  if (state != State::BROWSING && state != State::DOWNLOADING) return 0;
  return static_cast<int>(items.size()) + (page > 1 ? 1 : 0) + (page < totalPages ? 1 : 0);
}

const char* CrossFrontSyncFilesActivity::headerTitle() const { return tr(STR_CROSSFRONT_SYNC_FILES); }

bool CrossFrontSyncFilesActivity::isPreviousRow(const int row) const { return page > 1 && row == 0; }

bool CrossFrontSyncFilesActivity::isNextRow(const int row) const {
  return page < totalPages && row == listCount() - 1;
}

int CrossFrontSyncFilesActivity::itemIndexFromRow(const int row) const {
  const int index = row - (page > 1 ? 1 : 0);
  return index >= 0 && index < static_cast<int>(items.size()) ? index : -1;
}

bool CrossFrontSyncFilesActivity::isDownloadable(const EbookItem& item) const {
  return item.status == "ready" && crossfront::isSafeMediaId(item.mediaId) &&
         crossfront::isSafeFileName(item.fileName) && item.revision > 0 && item.size > 0;
}

bool CrossFrontSyncFilesActivity::isFilePresent(const EbookItem& item) const {
  if (!isDownloadable(item)) return false;
  const std::string path = std::string(CROSSFRONT_SETTINGS.getEbookDir()) + "/" + item.fileName;
  if (!Storage.exists(path.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("CF", path, file)) return false;
  const size_t actualSize = file.size();
  file.close();
  return crossfront::isExpectedDownloadedSize(actualSize, item.size);
}

std::string CrossFrontSyncFilesActivity::itemStatusText(const EbookItem& item) const {
  if (item.status == "ready") {
    return isDownloadable(item) ? std::string(tr(STR_CROSSFRONT_EBOOK_READY)) + " - " + formatSize(item.size)
                                : tr(STR_CROSSFRONT_EBOOK_UNAVAILABLE);
  }
  if (item.status == "crawling") {
    const std::string progress = std::to_string(item.crawled) + "/" +
                                 (item.total > 0 ? std::to_string(item.total) : "?");
    return std::string(tr(STR_CROSSFRONT_EBOOK_CRAWLING)) + " " + progress;
  }
  if (item.status == "packaging") return tr(STR_CROSSFRONT_EBOOK_CREATING_EPUB);
  if (item.status == "pending") return tr(STR_CROSSFRONT_EBOOK_PENDING);
  if (item.status == "failed") return tr(STR_CROSSFRONT_EBOOK_FAILED);
  return item.status;
}

std::string CrossFrontSyncFilesActivity::itemSubtitleText(const EbookItem& item) const {
  const char* source = item.sourceType == "generated" ? tr(STR_CROSSFRONT_EBOOK_SOURCE_GENERATED)
                                                        : tr(STR_CROSSFRONT_EBOOK_SOURCE_UPLOAD);
  return itemStatusText(item) + " - " + source;
}

void CrossFrontSyncFilesActivity::rebuildRows() {
  const int count = listCount();
  rowLabels.assign(count, std::string());
  rowSubtitles.assign(count, std::string());
  rowValues.assign(count, std::string());
  rowItems.clear();
  rowItems.reserve(count);

  for (int row = 0; row < count; ++row) {
    fui::ListItem listItem;
    if (isPreviousRow(row)) {
      rowLabels[row] = tr(STR_CROSSFRONT_EBOOK_PREVIOUS_PAGE);
      rowValues[row] = std::to_string(page - 1) + "/" + std::to_string(totalPages);
      listItem.label = rowLabels[row].c_str();
      listItem.value = rowValues[row].c_str();
    } else if (isNextRow(row)) {
      rowLabels[row] = tr(STR_CROSSFRONT_EBOOK_NEXT_PAGE);
      rowValues[row] = std::to_string(page + 1) + "/" + std::to_string(totalPages);
      listItem.label = rowLabels[row].c_str();
      listItem.value = rowValues[row].c_str();
    } else {
      const int itemIndex = itemIndexFromRow(row);
      const auto& ebook = items[itemIndex];
      listItem.label = ebook.title.empty() ? ebook.fileName.c_str() : ebook.title.c_str();
      rowSubtitles[row] = itemSubtitleText(ebook);
      listItem.subtitle = rowSubtitles[row].c_str();
      if (isDownloadable(ebook)) {
        rowValues[row] = ebook.downloaded ? tr(STR_CROSSFRONT_EBOOK_DOWNLOADED) : tr(STR_DOWNLOAD);
        listItem.value = rowValues[row].c_str();
      }
      if (ebook.hasThumbnail) {
        listItem.icon = fui::BitmapRef{ebook.thumbnailBits.data(), THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT,
                                      fui::BitmapFormat::BW1, false};
      }
      if (!isDownloadable(ebook)) listItem.state = fui::StateDisabled;
    }
    listItem.actionValue = static_cast<int16_t>(row);
    rowItems.push_back(listItem);
  }
  rowsDirty = false;
}

void CrossFrontSyncFilesActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (items.empty()) {
    screen.centeredText(tr(STR_CROSSFRONT_EBOOK_EMPTY), screen.theme().bodyText);
    return;
  }
  if (rowsDirty) rebuildRows();

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.rowHeight = 64;
  props.rowPaddingY = 5;
  props.iconSize = 48;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
  if (state == State::DOWNLOADING && activeDownloadIndex >= 0 &&
      activeDownloadIndex < static_cast<int>(items.size())) {
    const int percent = crossfront::downloadProgressPercent(fileBytesDownloaded, fileBytesTotal);
    const std::string message = items[activeDownloadIndex].fileName + "\n" + std::to_string(percent) + "%";
    fui::PopupProps popup;
    popup.message = message.c_str();
    popup.showProgress = true;
    popup.progress.value = percent;
    popup.progress.max = 100;
    popup.progress.border = fui::Paint::solid(fui::Color::Black);
    popup.progress.borderWidth = 1;
    popup.progressHeight = 10;
    screen.popup(popup);
  }
}

void CrossFrontSyncFilesActivity::activateIndex(const int row) {
  if (state != State::BROWSING) return;
  app.clearTapFlash();
  if (isPreviousRow(row)) {
    state = State::FETCHING_LIST;
    rowsDirty = true;
    pendingPage = page - 1;
    requestUpdate(true);
    return;
  }
  if (isNextRow(row)) {
    state = State::FETCHING_LIST;
    rowsDirty = true;
    pendingPage = page + 1;
    requestUpdate(true);
    return;
  }
  const int itemIndex = itemIndexFromRow(row);
  if (itemIndex >= 0 && isDownloadable(items[itemIndex])) promptDownload(itemIndex);
}

void CrossFrontSyncFilesActivity::promptDownload(const int itemIndex) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
  const std::string body = items[itemIndex].fileName.empty() ? items[itemIndex].title : items[itemIndex].fileName;
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DOWNLOAD), body);
  if (!confirmation) {
    LOG_ERR("CF", "OOM: ebook download confirmation");
    return;
  }
  startActivityForResult(std::move(confirmation),
                         [this, itemIndex](const ActivityResult& result) { onDownloadConfirmation(itemIndex, result); });
}

void CrossFrontSyncFilesActivity::onDownloadConfirmation(const int itemIndex, const ActivityResult& result) {
  if (result.isCancelled || itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) {
    requestUpdate();
    return;
  }
  activeDownloadIndex = itemIndex;
  retryAction = RetryAction::NONE;
  errorMessage.clear();
  clearPendingDownloadStatus();
  pendingMediaId = items[itemIndex].mediaId;
  pendingMediaRevision = items[itemIndex].revision;
  pendingDownloadId = makeDownloadId(deviceId);
  fileBytesDownloaded = 0;
  fileBytesTotal = items[itemIndex].size;
  cancelRequested = false;
  state = State::DOWNLOADING;
  requestUpdateAndWait();

  const bool downloaded = downloadSingleFile(items[itemIndex], pendingDownloadId);
  if (downloaded) {
    items[itemIndex].downloaded = true;
    rowsDirty = true;
  }
  if (cancelRequested) {
    clearPendingDownloadStatus();
    activeDownloadIndex = -1;
    cancelRequested = false;
    state = State::BROWSING;
  } else if (!downloaded) {
    clearPendingDownloadStatus();
    enterErrorState(tr(STR_CROSSFRONT_EBOOK_DOWNLOAD_FAILED), RetryAction::FETCH_LIST);
  } else {
    retryAction = RetryAction::SYNC_DOWNLOAD_STATUS;
    state = State::SYNCING_DOWNLOAD_STATUS;
  }
  requestUpdate(true);
}

void CrossFrontSyncFilesActivity::clearPendingDownloadStatus() {
  pendingMediaId.clear();
  pendingDownloadId.clear();
  pendingMediaRevision = 0;
}

void CrossFrontSyncFilesActivity::enterErrorState(const char* message, const RetryAction action) {
  errorMessage = message ? message : "";
  retryAction = action;
  if (action != RetryAction::SYNC_DOWNLOAD_STATUS) activeDownloadIndex = -1;
  cancelRequested = false;
  errorInputArmed = false;
  state = State::ERROR_STATE;
}

void CrossFrontSyncFilesActivity::leaveActiveOperation() {
  retryAction = RetryAction::NONE;
  errorMessage.clear();
  clearPendingDownloadStatus();
  activeDownloadIndex = -1;
  cancelRequested = false;
  errorInputArmed = false;
  if (items.empty()) {
    finish();
    return;
  }
  state = State::BROWSING;
  rowsDirty = true;
  requestUpdate(true);
}

bool CrossFrontSyncFilesActivity::handleBackBeforeActiveOperation() {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Back)) return false;
  cancelRequested = true;
  leaveActiveOperation();
  return true;
}

bool CrossFrontSyncFilesActivity::pollBackCancellation() {
  if (cancelRequested) return true;
  mappedInput.update(true);
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelRequested = true;
  return cancelRequested;
}

void CrossFrontSyncFilesActivity::retryFailedAction() {
  errorMessage.clear();
  cancelRequested = false;
  errorInputArmed = false;
  switch (retryAction) {
    case RetryAction::CONNECT_WIFI:
      state = State::CONNECTING_WIFI;
      break;
    case RetryAction::SYNC_DOWNLOAD_STATUS:
      state = !pendingMediaId.empty() && !pendingDownloadId.empty() ? State::SYNCING_DOWNLOAD_STATUS
                                                                    : State::FETCHING_LIST;
      break;
    case RetryAction::FETCH_LIST:
    case RetryAction::NONE:
      state = State::FETCHING_LIST;
      break;
  }
  requestUpdate(true);
}

bool CrossFrontSyncFilesActivity::handleCustomInput() {
  if (state == State::BROWSING) return false;

  if (state == State::CONNECTING_WIFI) {
    if (handleBackBeforeActiveOperation()) return true;
    cancelRequested = false;
    const bool connected = CrossFrontService::connectWifiQuick(
        15000, nullptr, this,
        [](void* userData) { return static_cast<CrossFrontSyncFilesActivity*>(userData)->pollBackCancellation(); });
    if (cancelRequested) {
      leaveActiveOperation();
      return true;
    } else if (!connected) {
      enterErrorState(tr(STR_CROSSFRONT_ERR_WIFI), RetryAction::CONNECT_WIFI);
    } else {
      retryAction = RetryAction::NONE;
      errorMessage.clear();
      state = State::FETCHING_LIST;
    }
    requestUpdate(true);
    return true;
  }

  if (state == State::FETCHING_LIST) {
    if (handleBackBeforeActiveOperation()) return true;
    cancelRequested = false;
    const bool fetched = fetchPage(pendingPage);
    if (cancelRequested) {
      leaveActiveOperation();
      return true;
    } else if (!fetched) {
      const std::string message = errorMessage.empty() ? tr(STR_CROSSFRONT_ERR_SERVER) : errorMessage;
      enterErrorState(message.c_str(), RetryAction::FETCH_LIST);
    } else {
      retryAction = RetryAction::NONE;
      errorMessage.clear();
      state = State::BROWSING;
      nav.reset();
      rowsDirty = true;
    }
    requestUpdate(true);
    return true;
  }

  if (state == State::SYNCING_DOWNLOAD_STATUS) {
    if (handleBackBeforeActiveOperation()) return true;
    cancelRequested = false;
    const bool acknowledged = !pendingMediaId.empty() && !pendingDownloadId.empty() &&
                              markFileDone(pendingMediaId, pendingMediaRevision, pendingDownloadId);
    if (cancelRequested) {
      leaveActiveOperation();
      return true;
    } else if (acknowledged) {
      retryAction = RetryAction::NONE;
      errorMessage.clear();
      if (activeDownloadIndex >= 0 && activeDownloadIndex < static_cast<int>(items.size())) {
        items[activeDownloadIndex].downloaded = true;
        rowsDirty = true;
      }
      clearPendingDownloadStatus();
      activeDownloadIndex = -1;
      state = State::BROWSING;
    } else {
      if (activeDownloadIndex >= 0 && activeDownloadIndex < static_cast<int>(items.size())) {
        items[activeDownloadIndex].downloaded = isFilePresent(items[activeDownloadIndex]);
        rowsDirty = true;
      }
      enterErrorState(tr(STR_CROSSFRONT_EBOOK_STATUS_SYNC_FAILED), RetryAction::SYNC_DOWNLOAD_STATUS);
    }
    requestUpdate(true);
    return true;
  }

  if (state == State::DOWNLOADING) return true;

  if (state == State::ERROR_STATE) {
    int tapX = 0;
    int tapY = 0;
    const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
    const bool headerBackTapped = tapped && HeaderBackTapTarget::contains(tapX, tapY);
    const bool backRequested = headerBackTapped || mappedInput.wasReleased(MappedInputManager::Button::Back);
    const bool retryRequested = mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
                                (tapped && !headerBackTapped);
    const bool inputIdle = !backRequested && !retryRequested && !mappedInput.wasAnyPressed() &&
                           !mappedInput.wasAnyReleased() && !mappedInput.wasScreenTouchReleased() &&
                           !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                           !mappedInput.isPressed(MappedInputManager::Button::Confirm);
    const auto decision =
        crossfront::evaluateSyncErrorInput(errorInputArmed, inputIdle, backRequested, retryRequested);
    errorInputArmed = decision.armed;
    if (decision.action == crossfront::SyncErrorInputAction::BACK) {
      leaveActiveOperation();
    } else if (decision.action == crossfront::SyncErrorInputAction::RETRY) {
      retryFailedAction();
    }
    return true;
  }
  return true;
}

bool CrossFrontSyncFilesActivity::fetchPage(const uint32_t requestedPage) {
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("CF", "Low heap before ebook list fetch (%u free, %u max block)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }

  if (Storage.exists(PAGE_RESPONSE_PATH) && !Storage.remove(PAGE_RESPONSE_PATH)) {
    LOG_ERR("CF", "Failed to remove stale ebook list response");
    return false;
  }
  HalFile responseFile;
  if (!Storage.openFileForWrite("CF", PAGE_RESPONSE_PATH, responseFile)) {
    LOG_ERR("CF", "Failed to open ebook list response file");
    return false;
  }

  std::string server = CROSSFRONT_SETTINGS.getServerUrl();
  if (!server.empty() && server.back() == '/') server.pop_back();
  const std::string url = server + "/api/cf/device/" + deviceId + "/ebooks?page=" +
                          std::to_string(requestedPage) + "&perPage=" + std::to_string(perPage);
  if (pollBackCancellation()) {
    responseFile.close();
    Storage.remove(PAGE_RESPONSE_PATH);
    return false;
  }
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("CF", "Low heap after opening ebook list response (%u free, %u max block)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    responseFile.close();
    Storage.remove(PAGE_RESPONSE_PATH);
    return false;
  }
  size_t responseLength = 0;
  bool responseTooLarge = false;
  bool responseWriteFailed = false;
  bool responseComplete = false;
  bool requestAborted = false;
  int httpCode = -1;

  {
    freeink::SecureHttpClient http;
    http.setTimeout(15000);
    http.setInsecure();
    if (http.begin(url)) {
      http.setUserAgent("CrossPoint-ESP32");
      for (const auto& header : makeHeaders(CROSSFRONT_SETTINGS.deviceToken)) {
        http.addHeader(header.first.c_str(), header.second.c_str());
      }
      httpCode = http.sendRequest(
          "GET", nullptr, 0,
          [&responseFile, &responseLength, &responseTooLarge,
           &responseWriteFailed](const uint8_t* data, const size_t len) {
            if (!crossfront::canAppendBoundedResponse(responseLength, len, MAX_PAGE_RESPONSE_BYTES)) {
              responseTooLarge = true;
              return false;
            }
            if (responseFile.write(data, len) != len) {
              responseWriteFailed = true;
              return false;
            }
            responseLength += len;
            return true;
          },
          [this]() { return pollBackCancellation(); });
      responseComplete = http.responseComplete();
      requestAborted = http.aborted();
    }
    http.end();
  }

  const bool responseClosed = responseFile.close();
  if (!responseClosed) LOG_ERR("CF", "Failed to close ebook list response file");

  const auto discardResponse = []() {
    if (Storage.exists(PAGE_RESPONSE_PATH) && !Storage.remove(PAGE_RESPONSE_PATH)) {
      LOG_ERR("CF", "Failed to remove ebook list response file");
    }
  };

  if (cancelRequested || requestAborted) {
    discardResponse();
    return false;
  }

  if (httpCode != 200) {
    discardResponse();
    errorMessage = (httpCode == 401 || httpCode == 404) ? tr(STR_CROSSFRONT_ERR_NOT_PAIRED)
                                                        : tr(STR_CROSSFRONT_ERR_SERVER);
    return false;
  }
  if (!responseClosed || responseTooLarge || responseWriteFailed || !responseComplete || responseLength == 0) {
    if (responseTooLarge) {
      LOG_ERR("CF", "Ebook list response exceeded %u bytes", static_cast<unsigned>(MAX_PAGE_RESPONSE_BYTES));
    }
    if (responseWriteFailed) LOG_ERR("CF", "Failed to write ebook list response");
    discardResponse();
    return false;
  }

  HalFile responseInput;
  if (!Storage.openFileForRead("CF", PAGE_RESPONSE_PATH, responseInput)) {
    LOG_ERR("CF", "Failed to reopen ebook list response");
    discardResponse();
    return false;
  }
  if (responseInput.size() != responseLength) {
    LOG_ERR("CF", "Ebook list response size changed on SD");
    responseInput.close();
    discardResponse();
    return false;
  }

  std::string ebookDir;
  uint32_t parsedPage = 0;
  uint32_t parsedPerPage = 0;
  uint32_t parsedTotalPages = 0;
  std::vector<EbookItem> parsedItems;
  {
    JsonDocument doc;
    DeserializationError jsonError;
    JsonDocument filter;
    filter["page"] = true;
    filter["perPage"] = true;
    filter["totalItems"] = true;
    filter["totalPages"] = true;
    filter["ebookDir"] = true;
    filter["items"][0]["id"] = true;
    filter["items"][0]["title"] = true;
    filter["items"][0]["status"] = true;
    filter["items"][0]["mediaId"] = true;
    filter["items"][0]["fileName"] = true;
    filter["items"][0]["sourceType"] = true;
    filter["items"][0]["size"] = true;
    filter["items"][0]["revision"] = true;
    filter["items"][0]["crawled"] = true;
    filter["items"][0]["total"] = true;
    filter["items"][0]["downloaded"] = true;
    filter["items"][0]["thumbnailBmp"] = true;
    jsonError = deserializeJson(doc, responseInput, DeserializationOption::Filter(filter));
    responseInput.close();
    discardResponse();
    if (jsonError != DeserializationError::Ok || !doc.is<JsonObject>()) {
      LOG_ERR("CF", "Invalid ebook list JSON (%s)", jsonError.c_str());
      return false;
    }
    const JsonObjectConst root = doc.as<JsonObjectConst>();
    const JsonVariantConst pageValue = root["page"];
    const JsonVariantConst perPageValue = root["perPage"];
    const JsonVariantConst totalItemsValue = root["totalItems"];
    const JsonVariantConst totalPagesValue = root["totalPages"];
    const JsonVariantConst ebookDirValue = root["ebookDir"];
    const JsonVariantConst itemsValue = root["items"];
    if (!pageValue.is<uint32_t>() || !perPageValue.is<uint32_t>() || !totalItemsValue.is<uint32_t>() ||
        !totalPagesValue.is<uint32_t>() || !ebookDirValue.is<const char*>() || !itemsValue.is<JsonArrayConst>()) {
      return false;
    }

    const char* ebookDirText = ebookDirValue.as<const char*>();
    const std::string_view ebookDirView(ebookDirText);
    parsedPage = pageValue.as<uint32_t>();
    parsedPerPage = perPageValue.as<uint32_t>();
    parsedTotalPages = totalPagesValue.as<uint32_t>();
    const JsonArrayConst array = itemsValue.as<JsonArrayConst>();
    if (!crossfront::isSafeEbookFolder(ebookDirView) || parsedPage == 0 || parsedPerPage == 0 ||
        parsedPerPage > MAX_PAGE_ITEMS || parsedTotalPages > MAX_TOTAL_PAGES || array.size() > parsedPerPage) {
      return false;
    }
    ebookDir.assign(ebookDirView.data(), ebookDirView.size());

    parsedItems.reserve(array.size());
    for (const JsonVariantConst value : array) {
      if (!value.is<JsonObjectConst>()) return false;
      const JsonObjectConst object = value.as<JsonObjectConst>();
      if (!object["id"].is<const char*>() || !object["title"].is<const char*>() ||
          !object["status"].is<const char*>() || !object["crawled"].is<uint32_t>() ||
          !object["total"].is<uint32_t>() || !object["downloaded"].is<bool>()) {
        return false;
      }

      const char* idText = object["id"].as<const char*>();
      const char* titleText = object["title"].as<const char*>();
      const char* statusText = object["status"].as<const char*>();
      const char* mediaIdText = object["mediaId"] | "";
      const char* fileNameText = object["fileName"] | "";
      const char* sourceTypeText = object["sourceType"] | "upload";
      const std::string_view id(idText);
      const std::string_view title(titleText);
      const std::string_view status(statusText);
      const std::string_view mediaId(mediaIdText);
      const std::string_view fileName(fileNameText);
      const std::string_view sourceType(sourceTypeText);
      if (!crossfront::isSafeMediaId(id) || title.empty() || title.size() > 1024 || !isKnownStatus(status) ||
          (!mediaId.empty() && !crossfront::isSafeMediaId(mediaId)) ||
          (!fileName.empty() && !crossfront::isSafeFileName(fileName)) ||
          (sourceType != "upload" && sourceType != "generated")) {
        return false;
      }

      EbookItem item;
      item.title.assign(title.data(), title.size());
      item.status.assign(status.data(), status.size());
      item.mediaId.assign(mediaId.data(), mediaId.size());
      item.fileName.assign(fileName.data(), fileName.size());
      item.sourceType.assign(sourceType.data(), sourceType.size());
      item.size = object["size"] | 0U;
      item.revision = object["revision"] | 0U;
      item.crawled = object["crawled"].as<uint32_t>();
      item.total = object["total"].as<uint32_t>();
      item.downloaded = object["downloaded"].as<bool>();
      item.hasThumbnail = decodeThumbnailBmp(object["thumbnailBmp"] | "", item.thumbnailBits);
      if (item.status == "ready" && !isDownloadable(item)) {
        item.mediaId.clear();
        item.fileName.clear();
        item.size = 0;
        item.revision = 0;
        item.downloaded = false;
      }
      parsedItems.push_back(std::move(item));
    }
  }

  if (pollBackCancellation()) return false;
  if (ebookDir != CROSSFRONT_SETTINGS.getEbookDir()) {
    CROSSFRONT_SETTINGS.setEbookDir(ebookDir.c_str());
    if (ebookDir != CROSSFRONT_SETTINGS.getEbookDir() || !CROSSFRONT_SETTINGS.saveToFile()) return false;
  }
  if (!ensureTargetFolderExists(ebookDir)) return false;
  for (auto& item : parsedItems) {
    if (item.status == "ready" && isDownloadable(item)) item.downloaded = isFilePresent(item);
  }

  if (!syncLocalPresence(parsedItems)) {
    if (cancelRequested) return false;
    LOG_ERR("CF", "Failed to reconcile local ebook presence");
  }

  items = std::move(parsedItems);
  page = parsedPage;
  perPage = parsedPerPage;
  totalPages = parsedTotalPages;
  return true;
}

bool CrossFrontSyncFilesActivity::ensureTargetFolderExists(const std::string& folder) const {
  if (!crossfront::isSafeTargetFolder(folder)) return false;
  if (Storage.exists(folder.c_str())) return true;
  return Storage.ensureDirectoryExists(folder.c_str()) && Storage.exists(folder.c_str());
}

bool CrossFrontSyncFilesActivity::downloadSingleFile(const EbookItem& item, const std::string& downloadId) {
  if (!isDownloadable(item) || !crossfront::isSafeDownloadId(downloadId)) return false;
  const std::string folder = CROSSFRONT_SETTINGS.getEbookDir();
  if (!ensureTargetFolderExists(folder)) return false;

  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("CF", "Low heap for ebook download (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }

  const std::string destination = folder + "/" + item.fileName;
  const std::string temporary = destination + ".tmp";
  if (Storage.exists(temporary.c_str())) Storage.remove(temporary.c_str());

  std::string server = CROSSFRONT_SETTINGS.getServerUrl();
  if (!server.empty() && server.back() == '/') server.pop_back();
  const std::string url = server + "/api/cf/device/" + deviceId + "/ebooks/" + item.mediaId + "/download";
  auto headers = makeHeaders(CROSSFRONT_SETTINGS.deviceToken);
  headers.emplace_back("X-Media-Revision", std::to_string(item.revision));
  headers.emplace_back("X-Download-Id", downloadId);

  fileBytesDownloaded = 0;
  fileBytesTotal = item.size;
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = millis();
  const size_t expectedSize = item.size;
  bool responseTooLarge = false;
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("CF", "Low heap after preparing ebook download (%u free, %u max block)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  const auto result = HttpDownloader::downloadToFile(
      url, temporary,
      [this, &lastRenderedPercent, &lastProgressUpdateMs, expectedSize,
       &responseTooLarge](const size_t downloaded, const size_t) {
        if (downloaded > expectedSize) {
          responseTooLarge = true;
          cancelRequested = true;
          return;
        }
        fileBytesDownloaded = downloaded;
        const int percent = crossfront::downloadProgressPercent(fileBytesDownloaded, fileBytesTotal);
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 || percent >= lastRenderedPercent + 5 ||
            now - lastProgressUpdateMs >= 2000) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      &cancelRequested, "", "", false, 120000, "", nullptr, headers, nullptr,
      [this]() { return pollBackCancellation(); });

  if (responseTooLarge) cancelRequested = false;
  if (cancelRequested || result != HttpDownloader::OK) {
    if (Storage.exists(temporary.c_str())) Storage.remove(temporary.c_str());
    return false;
  }
  HalFile downloadedFile;
  if (!Storage.openFileForRead("CF", temporary, downloadedFile)) {
    Storage.remove(temporary.c_str());
    return false;
  }
  const size_t actualSize = downloadedFile.size();
  downloadedFile.close();
  if (!crossfront::isExpectedDownloadedSize(actualSize, item.size)) {
    Storage.remove(temporary.c_str());
    return false;
  }
  if (!crossfront::replaceDownloadedFile(Storage, temporary, destination)) {
    Storage.remove(temporary.c_str());
    return false;
  }
  clearBookCache(destination);
  library::markLibraryIndexDirty();
  return true;
}

bool CrossFrontSyncFilesActivity::syncLocalPresence(const std::vector<EbookItem>& pageItems) {
  JsonDocument doc;
  JsonArray presence = doc["items"].to<JsonArray>();
  for (const auto& item : pageItems) {
    if (!isDownloadable(item)) continue;
    JsonObject entry = presence.add<JsonObject>();
    entry["mediaId"] = item.mediaId;
    entry["revision"] = item.revision;
    entry["present"] = item.downloaded;
  }
  if (presence.size() == 0) return true;

  std::string payload;
  serializeJson(doc, payload);
  std::string server = CROSSFRONT_SETTINGS.getServerUrl();
  if (!server.empty() && server.back() == '/') server.pop_back();
  const std::string url = server + "/api/cf/device/" + deviceId + "/ebooks/presence";
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("CF", "Low heap for ebook presence sync (%u free, %u max block)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  freeink::SecureHttpClient http;
  http.setTimeout(10000);
  http.setInsecure();
  if (!http.begin(url)) return false;
  http.setUserAgent("CrossPoint-ESP32");
  http.addHeader("Content-Type", "application/json");
  for (const auto& header : makeHeaders(CROSSFRONT_SETTINGS.deviceToken)) {
    http.addHeader(header.first.c_str(), header.second.c_str());
  }
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("CF", "Low heap after preparing ebook presence sync (%u free, %u max block)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  const int httpCode = http.sendRequest(
      "POST", reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
      discardResponseBody, [this]() { return pollBackCancellation(); });
  const bool requestAborted = http.aborted();
  http.end();
  return !cancelRequested && !requestAborted && httpCode == 200;
}

bool CrossFrontSyncFilesActivity::markFileDone(const std::string& mediaId, const uint32_t revision,
                                                const std::string& downloadId) {
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("CF", "Low heap for ebook status sync (%u free, %u max block)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  std::string server = CROSSFRONT_SETTINGS.getServerUrl();
  if (!server.empty() && server.back() == '/') server.pop_back();
  const std::string url = server + "/api/cf/device/" + deviceId + "/ebooks/" + mediaId + "/done";
  freeink::SecureHttpClient http;
  http.setTimeout(10000);
  http.setInsecure();
  if (!http.begin(url)) return false;
  http.setUserAgent("CrossPoint-ESP32");
  for (const auto& header : makeHeaders(CROSSFRONT_SETTINGS.deviceToken)) {
    http.addHeader(header.first.c_str(), header.second.c_str());
  }
  const std::string revisionValue = std::to_string(revision);
  http.addHeader("X-Media-Revision", revisionValue.c_str());
  http.addHeader("X-Download-Id", downloadId.c_str());
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("CF", "Low heap after preparing ebook status sync (%u free, %u max block)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  const int httpCode = http.sendRequest("POST", nullptr, 0, discardResponseBody,
                                       [this]() { return pollBackCancellation(); });
  const bool requestAborted = http.aborted();
  http.end();
  return !cancelRequested && !requestAborted && httpCode == 200;
}

void CrossFrontSyncFilesActivity::drawFooter() {
  if (state == State::DOWNLOADING) {
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const int selected = nav.selected;
  const int itemIndex = itemIndexFromRow(selected);
  const char* action = "";
  if (isPreviousRow(selected) || isNextRow(selected)) {
    action = tr(STR_OPEN);
  } else if (itemIndex >= 0 && isDownloadable(items[itemIndex])) {
    action = tr(STR_DOWNLOAD);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), action, listCount() > 0 ? tr(STR_DIR_UP) : "",
                                             listCount() > 0 ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CrossFrontSyncFilesActivity::render(RenderLock&& lock) {
  if (state == State::BROWSING || state == State::DOWNLOADING) {
    UiListActivity::render(std::move(lock));
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle());

  if (state == State::CONNECTING_WIFI) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_CROSSFRONT_CONNECTING_WIFI));
  } else if (state == State::FETCHING_LIST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_CROSSFRONT_SYNCING_FILES));
  } else if (state == State::SYNCING_DOWNLOAD_STATUS) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_CROSSFRONT_EBOOK_SYNCING_STATUS));
  } else if (state == State::ERROR_STATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_ERROR_MSG), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage.empty()) renderer.drawCenteredText(UI_10_FONT_ID, centerY + lineHeight, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  if (state == State::CONNECTING_WIFI || state == State::FETCHING_LIST ||
      state == State::SYNCING_DOWNLOAD_STATUS) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
