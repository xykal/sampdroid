// Read-only decoder for server-to-recipient player sync packets.
//
// Do not use this layout for outbound local packets: the server broadcast adds
// a player ID and uses conditional/compressed fields that differ from the
// client-to-server PlayerFootSync reader layout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace samp::syncdecode {

constexpr uint8_t kPlayerFootSyncPacketId = 207;

struct RemoteOnFootSync {
  uint16_t playerId = 0;
  int16_t leftRight = 0;
  int16_t upDown = 0;
  uint16_t keys = 0;

  float positionX = 0.0f;
  float positionY = 0.0f;
  float positionZ = 0.0f;
  float quaternionW = 1.0f;
  float quaternionX = 0.0f;
  float quaternionY = 0.0f;
  float quaternionZ = 0.0f;

  // Server broadcast format stores a one-byte, quantized percent pair.
  float health = 100.0f;
  float armour = 0.0f;
  uint8_t weaponAdditionalKey = 0;
  uint8_t specialAction = 0;
  float velocityX = 0.0f;
  float velocityY = 0.0f;
  float velocityZ = 0.0f;

  bool surfing = false;
  uint16_t surfingId = 0;
  float surfingOffsetX = 0.0f;
  float surfingOffsetY = 0.0f;
  float surfingOffsetZ = 0.0f;
  bool animationKnown = false;
  uint16_t animationId = 0;
  uint16_t animationFlags = 0;
};

// Decodes the open.mp/SA-MP server-to-client Packet 207 broadcast writer:
// ID, player ID, conditional directional inputs, raw position, compressed
// quaternion/percent/velocity, and optional surf/animation fields.
bool decodeRemoteOnFootBroadcast(const std::vector<uint8_t>& payload, RemoteOnFootSync& out,
                                 size_t payloadBitLength = 0);

std::string summarize(const RemoteOnFootSync& sync);

}  // namespace samp::syncdecode
