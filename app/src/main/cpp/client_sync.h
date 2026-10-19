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

constexpr uint8_t kPlayerVehicleSyncPacketId = 200;
constexpr uint8_t kPlayerAimSyncPacketId = 203;
constexpr uint8_t kPlayerBulletSyncPacketId = 206;
constexpr uint8_t kPlayerFootSyncPacketId = 207;
constexpr uint8_t kPlayerPassengerSyncPacketId = 211;
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

// Exact client-to-server reader layout for Packet 200 / PlayerVehicleSync.
// Unlike the server-to-recipient broadcast writer, this is not compressed and
// does not contain a player ID. Quaternion order is W, X, Y, Z.
struct InCarState {
  uint16_t vehicleId = 0;
  uint16_t leftRight = 0;
  uint16_t upDown = 0;
  uint16_t keys = 0;

  float quaternionW = 1.0f;
  float quaternionX = 0.0f;
  float quaternionY = 0.0f;
  float quaternionZ = 0.0f;

  float positionX = 0.0f;
  float positionY = 0.0f;
  float positionZ = 0.0f;

  float velocityX = 0.0f;
  float velocityY = 0.0f;
  float velocityZ = 0.0f;

  float vehicleHealth = 1000.0f;
  uint8_t playerHealth = 100;
  uint8_t playerArmour = 0;
  uint8_t additionalKeyWeapon = 0;
  uint8_t siren = 0;
  uint8_t landingGear = 0;
  uint16_t trailerId = 0;
  uint32_t hydraThrustAngle = 0;
};

// Exact client-to-server Packet 203 / PlayerAimSync reader layout. The server
// identifies the peer, so client packets have no PlayerID. `zoomWeaponState`
// is serialized manually: CamZoom uses bits 0..5 and WeaponState uses bits
// 6..7, avoiding implementation-defined C++ bitfield allocation.
struct AimState {
  uint8_t cameraMode = 4;
  float cameraFrontX = 0.0f;
  float cameraFrontY = 1.0f;
  float cameraFrontZ = 0.0f;
  float cameraPositionX = 0.0f;
  float cameraPositionY = 0.0f;
  float cameraPositionZ = 0.0f;
  // GTA aim pitch in radians. The native input mapper derives this from its
  // bounded relative camera pitch; it is never guessed from a FIRE/AIM press.
  float aimZ = 0.0f;
  uint8_t cameraZoom = 0;
  uint8_t weaponState = 0;
  uint8_t aspectRatio = 0;
};

// Exact client-to-server Packet 206 / PlayerBulletSync reader layout verified
// against open.mp PlayerBulletSync::read at commit 3d20f3dc. The server
// identifies the UDP peer, so client packets carry no PlayerID (unlike the
// 43-byte server-to-recipient broadcast). This native path deliberately
// supports only HitType=None: it can report an authenticated local firing
// action, but cannot nominate a player/vehicle/object target or claim damage.
struct BulletState {
  uint8_t hitType = 0;
  uint16_t hitId = 0;
  float originX = 0.0f;
  float originY = 0.0f;
  float originZ = 0.0f;
  // For HitType=None this is either an all-zero no-impact sentinel or an
  // absolute contact from the bounded static COL world-ray bridge. It never
  // identifies an entity; offsets stay zero.
  float hitPositionX = 0.0f;
  float hitPositionY = 0.0f;
  float hitPositionZ = 0.0f;
  float offsetX = 0.0f;
  float offsetY = 0.0f;
  float offsetZ = 0.0f;
  uint8_t weaponId = 0;
};

// Exact client-to-server Packet 211 reader layout. Like Packet 200 it omits
// PlayerID because the server identifies the UDP peer. The packed word is
// serialized manually below instead of relying on implementation-defined C++
// bitfields: Seat is low six bits, then DriveBy/Cuffed, followed by the raw
// Weapon/AdditionalKey byte.
struct PassengerState {
  uint16_t vehicleId = 0;
  uint8_t seatId = 0;
  bool driveBy = false;
  bool cuffed = false;
  uint8_t weaponAdditionalKey = 0;
  uint8_t health = 100;
  uint8_t armour = 0;
  uint16_t leftRight = 0;
  uint16_t upDown = 0;
  uint16_t keys = 0;
  float positionX = 0.0f;
  float positionY = 0.0f;
  float positionZ = 0.0f;
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

// Mirrors PlayerVehicleSync::read in open.mp's compatible NetCode layout. The
// output is exactly 64 bytes / 512 bits: Packet ID, raw input/transform/health
// fields, legacy trailer ID, and Hydra thrust word. It is deliberately not the
// compressed server-to-recipient Packet-200 representation.
bool isValidInCarState(const InCarState& state);
EncodedPayload buildPlayerVehicleSync(const InCarState& state);

// Mirrors PlayerAimSync::read/write as exercised by open.mp's compatible NPC
// generator: Packet ID, camera mode, two raw VEC3 values, AimZ, packed
// CamZoom/WeaponState byte, and raw aspect ratio. Output is 32 bytes / 256
// bits with no player ID.
bool isValidAimState(const AimState& state);
EncodedPayload buildPlayerAimSync(const AimState& state);

// Builds only the conservative 41-byte client Packet-206 no-target form:
// `HitType=None`, neutral ID/offset, an optional bounded static-world absolute
// hit position, and a server-authorized shootable weapon. It cannot encode
// entity/object hits or a damage assertion.
bool isValidNoTargetBulletState(const BulletState& state);
EncodedPayload buildPlayerNoTargetBulletSync(const BulletState& state);

// Mirrors the server's PlayerPassengerSync reader. Seat zero is deliberately
// rejected: it belongs to Packet 200 driver sync. Output is 25 bytes / 200
// bits and carries no local movement prediction or seat authority.
bool isValidPassengerState(const PassengerState& state);
EncodedPayload buildPlayerPassengerSync(const PassengerState& state);

}  // namespace samp::sync
