#pragma once

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
  if (storage.exists(backupPath.c_str()) &&
      (storage.exists(destinationPath.c_str()) || !storage.rename(backupPath.c_str(), destinationPath.c_str()))) {
    return false;
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
