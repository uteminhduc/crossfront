#include "crossfront/CrossFrontCrypto.h"

#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/md.h>

#include <cstdio>
#include <cstring>

namespace crossfront {

std::string computeHmacSha256(const std::string& secret, const std::string& message) {
  uint8_t hmac[32] = {0};
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);

  const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!mdInfo) {
    mbedtls_md_free(&ctx);
    return "";
  }

  if (mbedtls_md_setup(&ctx, mdInfo, 1) != 0) {  // 1 = HMAC enabled
    mbedtls_md_free(&ctx);
    return "";
  }

  mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char*>(secret.data()), secret.size());
  mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char*>(message.data()), message.size());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);

  char hex[65] = {0};
  for (size_t i = 0; i < 32; ++i) {
    snprintf(hex + (i * 2), 3, "%02x", hmac[i]);
  }
  return std::string(hex);
}

void generateRandomToken(char* outToken, size_t length) {
  static constexpr char ALPHABET[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  constexpr size_t ALPHABET_LEN = sizeof(ALPHABET) - 1;
  constexpr uint8_t LIMIT = static_cast<uint8_t>(256 - (256 % ALPHABET_LEN));

  size_t idx = 0;
  while (idx < length) {
    const uint32_t r = esp_random();
    for (size_t b = 0; b < sizeof(r) && idx < length; ++b) {
      const uint8_t byteVal = static_cast<uint8_t>(r >> (b * 8));
      if (byteVal < LIMIT) {
        outToken[idx++] = ALPHABET[byteVal % ALPHABET_LEN];
      }
    }
  }
  outToken[length] = '\0';
}

void getDeviceHardwareId(char* outId, size_t maxLen) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(outId, maxLen, "CF-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

}  // namespace crossfront
