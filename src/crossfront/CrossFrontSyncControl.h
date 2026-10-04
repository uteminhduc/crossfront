#pragma once

#include <cstddef>
#include <cstdint>

namespace crossfront {

enum class SyncErrorInputAction : uint8_t {
  NONE,
  BACK,
  RETRY,
};

struct SyncErrorInputDecision {
  bool armed;
  SyncErrorInputAction action;
};

constexpr SyncErrorInputDecision evaluateSyncErrorInput(const bool armed, const bool inputIdle,
                                                        const bool backRequested, const bool retryRequested) {
  if (!armed) return {inputIdle, SyncErrorInputAction::NONE};
  if (backRequested) return {true, SyncErrorInputAction::BACK};
  if (retryRequested) return {true, SyncErrorInputAction::RETRY};
  return {true, SyncErrorInputAction::NONE};
}

constexpr bool canAppendBoundedResponse(const size_t currentSize, const size_t incomingSize, const size_t limit) {
  return incomingSize <= limit && currentSize <= limit - incomingSize;
}

}  // namespace crossfront
