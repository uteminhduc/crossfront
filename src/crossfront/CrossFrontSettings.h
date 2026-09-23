#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstddef>
#include <cstdint>

// CrossFront-only persisted configuration. Keeping this in the CrossFront
// module prevents its service/API details from becoming part of the reader's
// global settings contract.
class CrossFrontSettings : public PersistableStore<CrossFrontSettings> {
 private:
  CrossFrontSettings() = default;

  friend class PersistableStore<CrossFrontSettings>;

 public:
  enum UpdateInterval : uint8_t {
    ON_SLEEP = 0,
    ONE_MINUTE = 1,
    TWO_MINUTES = 2,
    FIVE_MINUTES = 3,
    FIFTEEN_MINUTES = 4,
    THIRTY_MINUTES = 5,
    ONE_HOUR = 6,
    TWO_HOURS = 7,
    THREE_HOURS = 8,
    SIX_HOURS = 9,
    TWELVE_HOURS = 10,
    ONE_DAY = 11,
    UPDATE_INTERVAL_COUNT
  };

  char serverUrl[128] = "https://cf-api.pocketgo.org";
  char webUrl[128] = "https://cf.pocketgo.org";
  char deviceToken[32] = "";
  uint8_t updateInterval = ON_SLEEP;
  uint16_t sleepNetworkTimeoutMs = 10000;

  static const char* getFilePath() { return "/.crosspoint/crossfront.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void getDeviceId(char* outId, size_t maxLen) const;
  uint32_t getUpdateIntervalSeconds() const;
};

#define CROSSFRONT_SETTINGS CrossFrontSettings::getInstance()
