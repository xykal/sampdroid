// Bounded cadence policy for client-originated movement sync.
#pragma once

#include <algorithm>
#include <cstdint>

namespace samp::syncpolicy {

// PlayerInit sends OnFootRate in milliseconds. open.mp's default is 30 ms.
constexpr uint32_t kDefaultOnFootRateMs = 30;
constexpr uint32_t kDefaultInCarRateMs = 30;
constexpr uint32_t kDefaultWeaponRateMs = 30;

// A malicious/malformed server must not turn the Android client into a packet
// flooder or suspend its local state for an unbounded period. Valid server
// rates inside this conservative range are honored exactly.
constexpr uint32_t kMinOnFootRateMs = 20;
constexpr uint32_t kMaxOnFootRateMs = 1000;

// Ordering indices are scoped to an ordering channel. Keeping them in one
// tiny state object makes accidental reuse of channel-2 RPC ordering for
// channel-1 movement observable in host regression tests.
struct OrderingCounters {
  uint16_t nextRpc = 0;
  uint16_t nextSync = 0;

  uint16_t takeRpc() { return nextRpc++; }
  uint16_t currentSync() const { return nextSync; }
  uint16_t takeSync() { return nextSync++; }
};

// Channel 1 sync must never outrun the channel-2 spawn transition. A
// transport ACK for the PlayerSpawn confirmation is the bounded local signal
// that the server received that transition.
inline bool onFootSessionReady(bool accepted, bool playerInit, bool spawnResponseKnown,
                               bool spawnAllowed, bool playerSpawnAcked) {
  return accepted && playerInit && spawnResponseKnown && spawnAllowed && playerSpawnAcked;
}

inline uint32_t boundedOnFootRateMs(uint32_t serverRateMs) {
  if (serverRateMs == 0) return kDefaultOnFootRateMs;
  return std::clamp(serverRateMs, kMinOnFootRateMs, kMaxOnFootRateMs);
}

// `lastSentMs == 0` means no successful packet has been emitted in this
// session. A backwards clock sample is treated as not due; the caller can
// retry at the next monotonic sample.
inline bool onFootSendDue(uint64_t nowMs, uint64_t lastSentMs, uint32_t serverRateMs) {
  return lastSentMs == 0 ||
         (nowMs >= lastSentMs && nowMs - lastSentMs >= boundedOnFootRateMs(serverRateMs));
}

// PlayerInit's InCarRate has the same millisecond semantics as OnFootRate, but
// keep a distinct API/counter at the call site so a high-frequency vehicle tick
// cannot accidentally consume the on-foot cadence state.
inline uint32_t boundedInCarRateMs(uint32_t serverRateMs) {
  if (serverRateMs == 0) return kDefaultInCarRateMs;
  return std::clamp(serverRateMs, kMinOnFootRateMs, kMaxOnFootRateMs);
}

inline bool inCarSendDue(uint64_t nowMs, uint64_t lastSentMs, uint32_t serverRateMs) {
  return lastSentMs == 0 ||
         (nowMs >= lastSentMs && nowMs - lastSentMs >= boundedInCarRateMs(serverRateMs));
}

// PlayerInit also supplies WeaponRate. AimSync is a secondary sync packet, not
// an input-event packet, so it receives its own bounded cadence/counter rather
// than borrowing an OnFoot timestamp and starving movement updates.
inline uint32_t boundedWeaponRateMs(uint32_t serverRateMs) {
  if (serverRateMs == 0) return kDefaultWeaponRateMs;
  return std::clamp(serverRateMs, kMinOnFootRateMs, kMaxOnFootRateMs);
}

inline bool weaponSendDue(uint64_t nowMs, uint64_t lastSentMs, uint32_t serverRateMs) {
  return lastSentMs == 0 ||
         (nowMs >= lastSentMs && nowMs - lastSentMs >= boundedWeaponRateMs(serverRateMs));
}

}  // namespace samp::syncpolicy
