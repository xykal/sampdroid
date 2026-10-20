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

constexpr uint8_t kPlayerVehicleSyncPacketId = 200;
constexpr uint8_t kPlayerAimSyncPacketId = 203;
constexpr uint8_t kPlayerBulletSyncPacketId = 206;
constexpr uint8_t kPlayerFootSyncPacketId = 207;
constexpr uint8_t kPlayerUnoccupiedSyncPacketId = 209;
constexpr uint8_t kPlayerTrailerSyncPacketId = 210;
constexpr uint8_t kPlayerPassengerSyncPacketId = 211;

struct RemoteVehicleSync {
  uint16_t playerId = 0;
  uint16_t vehicleId = 0;
  bool driverSync = false;
  bool angleKnown = false;
  // Packet-200 compressed quaternion reduced to GTA X→Y→Z DFF Euler values.
  // Packet-209 has only roll/direction vectors, so it does not manufacture a
  // new full attitude and leaves this false for its own decoded frame.
  bool attitudeKnown = false;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  float health = 1000.0f;
  float velocityX = 0.0f;
  float velocityY = 0.0f;
  float velocityZ = 0.0f;
};

// Exact server-to-recipient Packet 210 trailer transform. It carries a raw
// quaternion (wire order w,x,y,z) and has no health or seat claim, so consumers
// may update only an already streamed trailer vehicle's transform. It must not
// fabricate a cab/trailer attachment, driver, passenger, or local trailer
// physics from this packet alone.
struct RemoteTrailerSync {
  uint16_t playerId = 0;
  uint16_t vehicleId = 0;
  bool attitudeKnown = false;
  float rotationX = 0.0f;
  float rotationY = 0.0f;
  float rotationZ = 0.0f;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float velocityX = 0.0f;
  float velocityY = 0.0f;
  float velocityZ = 0.0f;
  float turnVelocityX = 0.0f;
  float turnVelocityY = 0.0f;
  float turnVelocityZ = 0.0f;
};

// Packet 211 is exact server-to-client passenger state. Position comes from
// the packet; the renderer must not invent a seat offset from vehicle geometry.
struct RemotePassengerSync {
  uint16_t playerId = 0;
  uint16_t vehicleId = 0;
  uint8_t seatId = 0;
  bool driveBy = false;
  bool cuffed = false;
  uint8_t weaponAdditionalKey = 0;
  float health = 100.0f;
  float armour = 0.0f;
  int16_t leftRight = 0;
  int16_t upDown = 0;
  uint16_t keys = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

// Exact server-to-recipient Packet 203 has the remote player ID prefix that
// client-originated AimSync deliberately lacks. It remains camera presentation
// state only; it carries no BulletSync, target, raycast, hit, or damage fact.
struct RemoteAimSync {
  uint16_t playerId = 0;
  uint8_t cameraMode = 0;
  float cameraFrontX = 0.0f;
  float cameraFrontY = 0.0f;
  float cameraFrontZ = 0.0f;
  float cameraPositionX = 0.0f;
  float cameraPositionY = 0.0f;
  float cameraPositionZ = 0.0f;
  float aimZ = 0.0f;
  uint8_t cameraZoom = 0;
  uint8_t weaponState = 0;
  uint8_t aspectRatio = 0;
};

// Server-relayed Packet 206 is intentionally decode-only. Its hit fields are
// retained for exact validation but downstream presentation exposes only a
// short-lived origin-to-direction cue, never a local hit/damage authority.
struct RemoteBulletSync {
  uint16_t playerId = 0;
  uint8_t hitType = 0;
  uint16_t hitId = 0;
  float originX = 0.0f;
  float originY = 0.0f;
  float originZ = 0.0f;
  float hitPositionX = 0.0f;
  float hitPositionY = 0.0f;
  float hitPositionZ = 0.0f;
  float offsetX = 0.0f;
  float offsetY = 0.0f;
  float offsetZ = 0.0f;
  uint8_t weaponId = 0;
};

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

// Decodes the open.mp/SA-MP server-to-client Packet 203 broadcast writer:
// ID, player ID, camera mode, raw front/position VEC3 values, AimZ, packed
// CamZoom:6|WeaponState:2, and aspect ratio. The exact frame is 34 bytes.
bool decodeRemoteAimBroadcast(const std::vector<uint8_t>& payload, RemoteAimSync& out,
                              size_t payloadBitLength = 0);

// Decodes the open.mp/SA-MP server-to-client Packet 206 broadcast writer:
// ID, player ID, hit type/id, three raw VEC3 values, and weapon ID. This is
// receive-only and never creates a local shot/hit claim.
bool decodeRemoteBulletBroadcast(const std::vector<uint8_t>& payload, RemoteBulletSync& out,
                                 size_t payloadBitLength = 0);

// Decodes the open.mp/SA-MP server-to-client Packet 207 broadcast writer:
// ID, player ID, conditional directional inputs, raw position, compressed
// quaternion/percent/velocity, and optional surf/animation fields.
bool decodeRemoteOnFootBroadcast(const std::vector<uint8_t>& payload, RemoteOnFootSync& out,
                                 size_t payloadBitLength = 0);

// Decodes server-to-recipient Packet 200 driver sync and Packet 209
// unoccupied-vehicle sync. The reducer only applies this to an already
// StreamInVehicle-established entity; sync packets never create vehicles.
bool decodeRemoteVehicleBroadcast(const std::vector<uint8_t>& payload, RemoteVehicleSync& out,
                                  size_t payloadBitLength = 0);

// Decodes the fixed-size server-to-recipient Packet 210 PlayerTrailerSync:
// player/vehicle IDs, raw VEC3 position, raw VEC4 quaternion in w,x,y,z wire
// order, raw VEC3 linear velocity, then raw VEC3 turn velocity. Receive-only.
bool decodeRemoteTrailerBroadcast(const std::vector<uint8_t>& payload, RemoteTrailerSync& out,
                                  size_t payloadBitLength = 0);

// Decodes open.mp/SA-MP Packet 211 `PlayerPassengerSync` exactly as its
// public writer emits it: player/vehicle IDs, packed seat flags, raw u8
// health+armour, input words, then raw VEC3 position.
bool decodeRemotePassengerBroadcast(const std::vector<uint8_t>& payload, RemotePassengerSync& out,
                                    size_t payloadBitLength = 0);

std::string summarize(const RemoteAimSync& sync);
std::string summarize(const RemoteBulletSync& sync);
std::string summarize(const RemoteOnFootSync& sync);
std::string summarize(const RemoteVehicleSync& sync);
std::string summarize(const RemoteTrailerSync& sync);
std::string summarize(const RemotePassengerSync& sync);

}  // namespace samp::syncdecode
