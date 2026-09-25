#include "crossfront/CrossFrontCrypto.h"

#include <esp_mac.h>
#include <esp_random.h>
#include <cstdio>
#include <cstring>

namespace crossfront {

void generateRandomToken(char* outToken, size_t length) {
  static constexpr char LETTERS[] = "ABCDEFGHJKLMNPQRSTUVWXYZ";  // 24 letters, excludes ambiguous 'I' and 'O'
  constexpr size_t LETTERS_LEN = sizeof(LETTERS) - 1;
  static constexpr char DIGITS[] = "0123456789";
  constexpr size_t DIGITS_LEN = sizeof(DIGITS) - 1;

  if (length == 8) {
    for (size_t i = 0; i < 3; ++i) {
      outToken[i] = LETTERS[esp_random() % LETTERS_LEN];
    }
    for (size_t i = 3; i < 8; ++i) {
      outToken[i] = DIGITS[esp_random() % DIGITS_LEN];
    }
    outToken[8] = '\0';
    return;
  }

  static constexpr char ALPHABET[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  constexpr size_t ALPHABET_LEN = sizeof(ALPHABET) - 1;
  for (size_t i = 0; i < length; ++i) {
    outToken[i] = ALPHABET[esp_random() % ALPHABET_LEN];
  }
  outToken[length] = '\0';
}

void getDeviceHardwareId(char* outId, size_t maxLen) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(outId, maxLen, "CF-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

}  // namespace crossfront
