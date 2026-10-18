// Source-validated outbound local-player sync primitives.
//
// This is an independent implementation. It intentionally models the
// client-to-server PlayerFootSync reader layout, not the different
// server-to-recipient broadcast writer layout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace samp::sync {

constexpr uint8_t kPlayerFootSyncPacketId = 207;
constexpr uint8_t kSyncOrderingChannel = 1;

// Exact fixed-layout data consumed by the SA-MP-compatible server's
// PlayerFootSync::read handler after the packet ID. The quaternion is emitted
// in W, X, Y, Z order, matching the legacy GTA quaternion storage used by the
// server-side reader. Values are local-player state only; no remote state is
// accepted or generated here.
struct OnFootState {
  uint16_t leftRight = 0;
  uint16_t upDown = 0;
  uint16_t keys = 0;

  float positionX = 0.0f;
  float positionY = 0.0f;
  float positionZ = 0.0f;

  float quaternionW = 1.0f;
  float quaternionX = 0.0f;
  float quaternionY = 0.0f;
  float quaternionZ = 0.0f;

  uint8_t health = 100;
  uint8_t armour = 0;
  // Legacy Weapon (low six bits) plus AdditionalKey (high two bits), kept as
  // one raw byte so a future input bridge cannot depend on C++ bitfield layout.
  uint8_t weaponAdditionalKey = 0;
  uint8_t specialAction = 0;

  float velocityX = 0.0f;
  float velocityY = 0.0f;
  float velocityZ = 0.0f;

  float surfingOffsetX = 0.0f;
  float surfingOffsetY = 0.0f;
  float surfingOffsetZ = 0.0f;
  uint16_t surfingId = 0;

  uint16_t animationId = 0;
  uint16_t animationFlags = 0;
};

struct EncodedPayload {
  std::vector<uint8_t> bytes;
  uint16_t bitLength = 0;

  bool valid() const { return !bytes.empty() && bitLength > 0; }
};

// Mirrors the finite/world-bound checks made by the server reader before it
// applies a received PlayerFootSync. It also rejects unsafe local velocity.
bool isValidOnFootState(const OnFootState& state);

// Builds one client-to-server packet-207 payload. Invalid state produces an
// empty payload. The output is exactly 69 bytes / 552 bits for the current
// fixed legacy on-foot layout.
EncodedPayload buildPlayerFootSync(const OnFootState& state);

}  // namespace samp::sync
