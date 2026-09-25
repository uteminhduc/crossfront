#pragma once

#include <cstddef>
#include <string>

namespace crossfront {

// Computes HMAC-SHA256 of message using secret key, returns lowercase hex string (64 chars)
std::string computeHmacSha256(const std::string& secret, const std::string& message);

// Generates cryptographically secure alphanumeric random token (e.g. 32 chars)
void generateRandomToken(char* outToken, size_t length);

// Generates persistent unique hardware device ID based on eFuse MAC
void getDeviceHardwareId(char* outId, size_t maxLen);

}  // namespace crossfront
