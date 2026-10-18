// Small deterministic policy helpers for the retained RakNet session.
#pragma once

#include <cstddef>
#include <cstdint>

namespace samp::sessionpolicy {

// RakNet 2.52 compatibility behavior: after five seconds without an outbound
// reliable message, send a reliable ID_DETECT_LOST_CONNECTIONS frame only when
// no other reliable message is awaiting an ACK.
constexpr uint64_t kReliableKeepaliveIntervalMs = 5000;

inline bool shouldSendReliableKeepalive(uint64_t reliableIdleMs,
                                        size_t pendingReliableCount) {
  return pendingReliableCount == 0 && reliableIdleMs >= kReliableKeepaliveIntervalMs;
}

}  // namespace samp::sessionpolicy
