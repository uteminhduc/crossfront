#pragma once

#include <cstddef>
#include <string>

namespace crossfront {


// Generates cryptographically secure alphanumeric random token (e.g. 32 chars)
void generateRandomToken(char* outToken, size_t length);

// Generates persistent unique hardware device ID based on eFuse MAC
void getDeviceHardwareId(char* outId, size_t maxLen);

}  // namespace crossfront
