#pragma once

#include <string>

namespace crossfront {

inline std::string sleepImageRequestUrl(const std::string& server, const char* deviceId) {
  return server + "/api/cf/device/" + deviceId + "/sleep.bmp";
}

template <typename StorageType>
void discardSleepImageCache(StorageType& storage, const char* bmpPath, const char* etagPath, const char* backupPath) {
  storage.remove(bmpPath);
  storage.remove(etagPath);
  storage.remove(backupPath);
}

}
