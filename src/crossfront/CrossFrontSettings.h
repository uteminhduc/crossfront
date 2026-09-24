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
  CrossFrontSettings();

  friend class PersistableStore<CrossFrontSettings>;

 public:
  static constexpr char DEFAULT_SERVER_URL[] = "https://cf-api.pocketgo.org";
  static constexpr char DEFAULT_WEB_URL[] = "https://cf.pocketgo.org";

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

  char serverUrl[128] = "";
  char webUrl[128] = "";
  char deviceToken[32] = "";
  uint8_t updateInterval = ON_SLEEP;
  uint16_t sleepNetworkTimeoutMs = 15000;

  static const char* getFilePath() { return "/.crosspoint/crossfront.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const char* getServerUrl() const {
    return serverUrl[0] != '\0' ? serverUrl : DEFAULT_SERVER_URL;
  }

  const char* getWebUrl() const {
    return webUrl[0] != '\0' ? webUrl : DEFAULT_WEB_URL;
  }

  void getDeviceId(char* outId, size_t maxLen) const;
  uint32_t getUpdateIntervalSeconds() const;
};

#define CROSSFRONT_SETTINGS CrossFrontSettings::getInstance()
