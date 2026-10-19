// Collision-grounded local gameplay bridge.
//
// This is deliberately independent from sockets/JNI: it turns authoritative
// local-player facts plus Android-native semantic input into one source-checked
// PlayerFootSync candidate only while a user-owned collision world can support
// the movement, a bounded local AimSync and no-target BulletSync candidate,
// plus a non-predictive PassengerSync candidate for a server-authorized
// passenger seat. The retained RakNet session decides whether/when to transmit
// any candidate.
#pragma once

#include "client_sync.h"
#include "gta_collision_world.h"
#include "input_state.h"
#include "local_movement.h"
#include "local_vehicle_movement.h"
#include "rpc_state.h"

#include <cstdint>

namespace samp::gameplay {

enum class LocalGameplayStatus : uint8_t {
  Applied,
  AwaitingAuthoritativePosition,
  AwaitingHeading,
  FrozenByServer,
  // Server spectator lifecycle blocks all local movement/aim/passenger output.
  Spectating,
  // An authoritative SetPlayerHealth value of zero revokes every locally
  // generated movement candidate until the server restores valid health or a
  // normal new spawn flow completes.
  Dead,
  CollisionUnavailable,
  // Server gravity outside the bounded on-foot pilot envelope; no client-side
  // trajectory/sync is emitted until a supported authoritative value arrives.
  GravityUnsupported,
  ServerVelocityActive,
  MissingGround,
  VehicleUnavailable,
  PassengerSeat,
  VehicleBlocked,
  AimInactive,
  WeaponUnavailable,
  InvalidInput,
  InvalidState,
  InvalidSyncState,
};

struct LocalGameplayState {
  // Predicted state is always replaceable by an authoritative SetSpawnInfo or
  // SetPlayerPosition RPC. It never mutates WorldState's server-owned facts.
  movement::LocalMotionState motion;
  bool predictionActive = false;
  bool authoritativeResetPending = true;
  // Kept separate from pedestrian motion so a driver packet can never reuse
  // on-foot height/velocity assumptions after an enter/exit transition.
  vehiclemotion::LocalVehicleMotionState vehicleMotion;
  bool vehiclePredictionActive = false;
  bool vehicleAuthoritativeResetPending = true;
};

struct LocalGameplayStep {
  LocalGameplayStatus status = LocalGameplayStatus::AwaitingAuthoritativePosition;
  bool moved = false;
  bool jumped = false;
  bool grounded = false;
  bool onFootReady = false;
  bool inCarReady = false;
  bool aimReady = false;
  bool passengerReady = false;
  sync::OnFootState onFoot;
  sync::InCarState inCar;
  sync::AimState aim;
  sync::PassengerState passenger;
};

// Call after an authoritative position/spawn correction, a spawn transition,
// or a server freeze. The next valid step re-seeds only from server state.
void invalidateLocalPrediction(LocalGameplayState& state);

const char* localGameplayStatusName(LocalGameplayStatus status);

// Advances basic on-foot movement against a read-only collision world, using
// the exact server-authoritative IPL interior when known (or exterior zero
// conservatively otherwise). It refuses absent matching collision, active
// server velocity, malformed input, frozen local control, or invalid sync
// state instead of inventing coordinates. `serverGravity` is PlayerInit/RPC146
// authority in GTA units; unsupported values stop prediction rather than being
// silently replaced by a guessed default. When supplied, an exact valid RPC-17
// environment also prevents only exterior prediction from crossing the server's
// maxX/minX/maxY/minY rectangle; it neither modifies COL nor constrains
// interiors. Animation/full GTA physics remain separate work.
LocalGameplayStep advanceLocalOnFoot(const state::LocalPlayer& local,
                                     const input::OnFootInputMapper& controls,
                                     const gta::world::CollisionWorld& collisionWorld,
                                     float deltaSeconds, LocalGameplayState& state,
                                     float serverGravity = movement::kDefaultServerGravity,
                                     const state::WorldEnvironment* environment = nullptr);

// Advances only a server-authorized local driver (seat zero) using a streamed
// vehicle's immutable facts. The output remains a bounded Packet-200 candidate;
// a supplied RPC-17 environment constrains exterior prediction only. Passenger
// sync, trailers, weapons, and full GTA vehicle physics are separate work rather
// than silently fabricated here.
LocalGameplayStep advanceLocalDriverVehicle(const state::LocalPlayer& local,
                                            const state::Vehicle& vehicle,
                                            const input::OnFootInputMapper& controls,
                                            const gta::world::CollisionWorld& collisionWorld,
                                            float deltaSeconds,
                                            LocalGameplayState& state,
                                            const state::WorldEnvironment* environment = nullptr);

// Builds Packet-203 aim state only while on foot and holding the native AIM
// action. It uses the collision-grounded local transform plus bounded yaw/pitch
// from the same mapper as Packet-207; it never chooses a target or claims a
// local hit/reward. A server-controlled camera must be gated by the retained
// session before this candidate can be transmitted.
LocalGameplayStep buildLocalAimSync(const state::LocalPlayer& local,
                                    const input::OnFootInputMapper& controls,
                                    const LocalGameplayState& state);

// Builds only a Packet-206 HitType=None candidate from a ready native aim pose
// while AIM+FIRE are both held. It starts with neutral ID/hit position/offset;
// callers may then request the separate world-only COL bridge below. It never
// synthesizes entity/object ID, target, damage, ammo decrement, or reward.
bool buildLocalNoTargetBulletSync(const state::LocalPlayer& local,
                                  const input::OnFootInputMapper& controls,
                                  const LocalGameplayState& state,
                                  const sync::AimState& aim,
                                  sync::BulletState& out);

// Applies at most one close static GTA-COL contact to an already-valid
// HitType=None candidate. The exact collision-grounded interior and AimSync
// direction are reused; unresolved/budget-exhausted worlds keep the all-zero
// sentinel. No remote player/vehicle/server object is queried or named.
bool applyLocalNoTargetStaticWorldImpact(const gta::world::CollisionWorld& collisionWorld,
                                         const LocalGameplayState& state,
                                         const sync::AimState& aim,
                                         sync::BulletState& inOut);

// Builds Packet-211 passenger state only after the server has seated the local
// player in a non-driver seat and the exact vehicle remains streamed. It has no
// local movement/seat prediction: position is copied from the authoritative
// vehicle transform and must never invent a GTA seat offset. Held AIM can set
// only the legacy drive-by bit for a server-armed Uzi/MP5/Tec-9 with known ammo;
// FIRE stays an input key and no passenger BulletSync/target/damage is created.
LocalGameplayStep buildLocalPassengerSync(const state::LocalPlayer& local,
                                          const state::Vehicle& vehicle,
                                          const input::OnFootInputMapper& controls,
                                          LocalGameplayState& state);

}  // namespace samp::gameplay
