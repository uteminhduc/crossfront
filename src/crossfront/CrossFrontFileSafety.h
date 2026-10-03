#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace crossfront {

inline bool isSafeAssignmentId(const std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  for (const unsigned char character : id) {
    if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' || character == '_') {
      continue;
    }
    return false;
  }
  return true;
}

inline bool isSafeFileName(const std::string_view name) {
  if (name.empty() || name.size() > 249 || name == "." || name == ".." || name.back() == ' ' ||
      name.back() == '.') return false;
  for (const unsigned char character : name) {
    if (character < 32 || character == 127 || character == '/' || character == '\\' || character == ':') {
      return false;
    }
  }
  return true;
}

inline bool isSupportedFileType(const std::string_view type) {
  return type == "book" || type == "font";
}

inline bool isSafeFileAssignment(const std::string_view id, const std::string_view name,
                                 const std::string_view type, const uint32_t revision) {
  return isSafeAssignmentId(id) && isSafeFileName(name) && isSupportedFileType(type) && revision > 0;
}

inline int downloadProgressPercent(const std::size_t downloaded, const std::size_t total) {
  if (total == 0) return 0;
  if (downloaded >= total) return 100;
  const int percent = static_cast<int>((static_cast<double>(downloaded) / static_cast<double>(total)) * 100.0);
  return percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

inline bool isExpectedDownloadedSize(const std::size_t actual, const std::size_t expected) {
  return expected > 0 && actual == expected;
}

inline bool isSafeTargetFolder(const std::string_view folder) {
  if (folder.size() < 2 || folder.size() > 255 || folder.front() != '/') return false;
  constexpr std::string_view reservedFolder = ".crosspoint";
  const auto firstSegmentEnd = folder.find('/', 1);
  const auto firstSegment = folder.substr(1, firstSegmentEnd == std::string_view::npos ? firstSegmentEnd
                                                                                      : firstSegmentEnd - 1);
  if (firstSegment.size() == reservedFolder.size()) {
    bool isReserved = true;
    for (std::size_t index = 0; index < firstSegment.size(); ++index) {
      const char character = firstSegment[index];
      const char lowercase = (character >= 'A' && character <= 'Z')
                                 ? static_cast<char>(character - 'A' + 'a')
                                 : character;
      if (lowercase != reservedFolder[index]) {
        isReserved = false;
        break;
      }
    }
    if (isReserved) return false;
  }
  std::size_t segmentStart = 1;
  for (std::size_t index = 1; index <= folder.size(); ++index) {
    if (index == folder.size() || folder[index] == '/') {
      const auto segment = folder.substr(segmentStart, index - segmentStart);
      if (segment.empty() || segment == "." || segment == "..") return false;
      segmentStart = index + 1;
      continue;
    }
    const auto character = static_cast<unsigned char>(folder[index]);
    if (character < 32 || character == 127 || character == '\\' || character == ':') return false;
  }
  return true;
}

template <typename StorageType>
bool replaceDownloadedFile(StorageType& storage, const std::string& temporaryPath, const std::string& destinationPath) {
  const std::string backupPath = destinationPath + ".cfbak";
  if (storage.exists(backupPath.c_str())) {
    if (storage.exists(destinationPath.c_str())) {
      if (!storage.remove(backupPath.c_str())) return false;
    } else if (!storage.rename(backupPath.c_str(), destinationPath.c_str())) {
      return false;
    }
  }

  const bool hasPreviousFile = storage.exists(destinationPath.c_str());
  if (hasPreviousFile && !storage.rename(destinationPath.c_str(), backupPath.c_str())) return false;
  if (!storage.rename(temporaryPath.c_str(), destinationPath.c_str())) {
    if (hasPreviousFile) storage.rename(backupPath.c_str(), destinationPath.c_str());
    return false;
  }
  if (hasPreviousFile) storage.remove(backupPath.c_str());
  return true;
}

}
