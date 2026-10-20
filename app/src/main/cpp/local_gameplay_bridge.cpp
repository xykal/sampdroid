#include "local_gameplay_bridge.h"

#include "local_player_sync.h"

#include <algorithm>
#include <cmath>

namespace samp::gameplay {
namespace {

constexpr float kMovementEpsilon = 0.0001f;
constexpr float kVelocityEpsilon = 0.05f;
constexpr float kPi = 3.14159265358979323846f;
// Packet-200 CVector values use GTA's 50 Hz movement-speed convention, while
// local pilots integrate world units per second. Keep outbound conversion in
// this bridge paired with vehiclemotion::applyServerLinearVelocity.
constexpr float kGtaPhysicsTicksPerSecond = 50.0f;
// A deliberately close static-world probe avoids pretending this early port
// owns GTA's full per-weapon trajectory/range model. It is only an optional
// absolute HitType=None contact; unresolved distance remains the zero sentinel.
constexpr float kNoTargetStaticImpactDistance = 20.0f;

bool validSyncVital(float value) {
  return std::isfinite(value) && value >= 0.0f && value <= 100.0f;
}

uint8_t percentByte(float value) {
  return static_cast<uint8_t>(std::lround(value));
}

bool serverVelocityIsActive(const state::LocalPlayer& local) {
  if (!local.velocityKnown) return false;
  return std::fabs(local.velocityX) > kVelocityEpsilon ||
         std::fabs(local.velocityY) > kVelocityEpsilon ||
         std::fabs(local.velocityZ) > kVelocityEpsilon;
}

LocalGameplayStatus fromMovementStatus(movement::StepStatus status) {
  switch (status) {
    case movement::StepStatus::Applied:
      return LocalGameplayStatus::Applied;
    case movement::StepStatus::MissingGround:
      return LocalGameplayStatus::MissingGround;
    case movement::StepStatus::InvalidInput:
      return LocalGameplayStatus::InvalidInput;
    case movement::StepStatus::InvalidState:
      return LocalGameplayStatus::InvalidState;
  }
  return LocalGameplayStatus::InvalidState;
}

LocalGameplayStatus fromVehicleMovementStatus(vehiclemotion::StepStatus status) {
  switch (status) {
    case vehiclemotion::StepStatus::Applied:
      return LocalGameplayStatus::Applied;
    case vehiclemotion::StepStatus::Blocked:
      return LocalGameplayStatus::VehicleBlocked;
    case vehiclemotion::StepStatus::MissingGround:
      return LocalGameplayStatus::MissingGround;
    case vehiclemotion::StepStatus::InvalidInput:
      return LocalGameplayStatus::InvalidInput;
    case vehiclemotion::StepStatus::InvalidState:
      return LocalGameplayStatus::InvalidState;
  }
  return LocalGameplayStatus::InvalidState;
}

uint8_t aimCameraModeForWeapon(uint8_t weapon) {
  // Exact compatible camera-mode choices used by open.mp's NPC aim updater.
  // GTA/SA-MP IDs 0..15 are the melee weapon family (fist through cane).
  if (weapon <= 15u) return 4;
  // An unrecognized server-authorized weapon stays on ordinary aim mode rather
  // than causing Android to invent a special camera protocol.
  switch (weapon) {
    case 34:  // sniper rifle
      return 7;
    case 43:  // camera
      return 46;
    case 35:  // rocket launcher
      return 8;
    case 36:  // heat seeker
      return 51;
    default:
      return 53;
  }
}

uint8_t weaponStateForAmmo(uint32_t ammo) {
  // open.mp SDK PlayerWeaponState: NoBullets=0, LastBullet=1,
  // MoreBullets=2, Reloading=3. Reloading cannot be inferred locally.
  if (ammo == 0u) return 0;
  if (ammo == 1u) return 1;
  return 2;
}

bool isPassengerDriveByWeapon(uint8_t weapon) {
  // Preserve the stock compatible passenger-drive-by family only: the SMG
  // slot named by open.mp's own SetPlayerArmedWeapon drive-by example. Do not
  // turn an arbitrary armed item, rifle, or explosive into a mobile control.
  return weapon == 28u || weapon == 29u || weapon == 32u;  // Uzi, MP5, Tec-9
}

// Health is server-owned state. A known zero is the only death fact this
// bounded bridge needs: it stops client-originated sync without attempting a
// GTA death animation, damage report, or a guessed respawn transition.
bool isServerDead(const state::LocalPlayer& local) {
  return local.healthKnown && std::isfinite(local.health) && local.health <= 0.0f;
}

// RPC 17 is explicitly an exterior-only server policy. It does not replace a
// COL query, infer an interior, or mutate server state; it merely keeps a local
// prediction from stepping beyond an already validated recipient rectangle.
bool insideServerWorldBounds(const state::WorldEnvironment* environment,
                             bool knownExterior, const gta::col::Vec3& position) {
  // An unknown local interior is not evidence of exterior. Leave prediction to
  // normal collision/server correction until RPC 156 gives that fact instead
  // of accidentally applying an exterior-only restriction inside a building.
  if (environment == nullptr || !environment->worldBoundsKnown || !knownExterior) return true;
  if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
      !std::isfinite(environment->worldMaxX) || !std::isfinite(environment->worldMinX) ||
      !std::isfinite(environment->worldMaxY) || !std::isfinite(environment->worldMinY) ||
      environment->worldMaxX <= environment->worldMinX ||
      environment->worldMaxY <= environment->worldMinY) {
    return false;
  }
  return position.x <= environment->worldMaxX && position.x >= environment->worldMinX &&
         position.y <= environment->worldMaxY && position.y >= environment->worldMinY;
}

}  // namespace

void invalidateLocalPrediction(LocalGameplayState& state) {
  state.motion = {};
  state.predictionActive = false;
  state.authoritativeResetPending = true;
  state.vehicleMotion = {};
  state.vehiclePredictionActive = false;
  state.vehicleAuthoritativeResetPending = true;
}

const char* localGameplayStatusName(LocalGameplayStatus status) {
  switch (status) {
    case LocalGameplayStatus::Applied:
      return "applied";
    case LocalGameplayStatus::AwaitingAuthoritativePosition:
      return "awaiting-authoritative-position";
    case LocalGameplayStatus::AwaitingHeading:
      return "awaiting-heading";
    case LocalGameplayStatus::FrozenByServer:
      return "frozen-by-server";
    case LocalGameplayStatus::Spectating:
      return "spectating";
    case LocalGameplayStatus::Dead:
      return "dead";
    case LocalGameplayStatus::CollisionUnavailable:
      return "collision-unavailable";
    case LocalGameplayStatus::GravityUnsupported:
      return "gravity-unsupported";
    case LocalGameplayStatus::ServerVelocityActive:
      return "server-velocity-active";
    case LocalGameplayStatus::MissingGround:
      return "missing-ground";
    case LocalGameplayStatus::VehicleUnavailable:
      return "vehicle-unavailable";
    case LocalGameplayStatus::PassengerSeat:
      return "passenger-seat";
    case LocalGameplayStatus::VehicleBlocked:
      return "vehicle-blocked";
    case LocalGameplayStatus::AimInactive:
      return "aim-inactive";
    case LocalGameplayStatus::WeaponUnavailable:
      return "weapon-unavailable";
    case LocalGameplayStatus::InvalidInput:
      return "invalid-input";
    case LocalGameplayStatus::InvalidState:
      return "invalid-state";
    case LocalGameplayStatus::InvalidSyncState:
      return "invalid-sync-state";
  }
  return "invalid-state";
}

LocalGameplayStep advanceLocalOnFoot(const state::LocalPlayer& local,
                                     const input::OnFootInputMapper& controls,
                                     const gta::world::CollisionWorld& collisionWorld,
                                     float deltaSeconds, LocalGameplayState& state,
                                     float serverGravity,
                                     const state::WorldEnvironment* environment,
                                     const std::vector<gta::removal::Directive>* removals) {
  LocalGameplayStep result;
  if (local.spectatingKnown && local.spectating) {
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::Spectating;
    return result;
  }
  if (isServerDead(local)) {
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::Dead;
    return result;
  }
  if (!local.positionKnown) {
    result.status = LocalGameplayStatus::AwaitingAuthoritativePosition;
    return result;
  }
  if (local.controllableKnown && !local.controllable) {
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::FrozenByServer;
    return result;
  }
  if (!controls.headingKnown()) {
    result.status = LocalGameplayStatus::AwaitingHeading;
    return result;
  }
  if (!movement::isSupportedServerGravity(serverGravity)) {
    // Do not keep an airborne velocity derived from another gravity value.
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::GravityUnsupported;
    return result;
  }
  if (collisionWorld.empty()) {
    // A data remount/clear invalidates the support assumptions behind any
    // prior prediction, so immediately fall back to server position.
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::CollisionUnavailable;
    return result;
  }
  // SetPlayerVelocity needs its own physics integration. Applying walking on
  // top of a server force would create a fabricated trajectory, so wait for an
  // explicit zero/correction instead.
  if (serverVelocityIsActive(local)) {
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::ServerVelocityActive;
    return result;
  }

  const input::WireInput wire = controls.wireInput();
  if (!wire.headingKnown || !std::isfinite(wire.headingDegrees)) {
    result.status = LocalGameplayStatus::AwaitingHeading;
    return result;
  }

  if (state.authoritativeResetPending || !state.motion.initialized) {
    const gta::col::Vec3 authoritative{local.x, local.y, local.z};
    // CollisionWorld indexes IPL placement interiors exactly. An unknown local
    // interior conservatively retains exterior zero; a known value is carried
    // unchanged into floor/sweep queries instead of mixing map instances.
    const int32_t interior = local.interiorKnown ? static_cast<int32_t>(local.interior) : 0;
    if (!movement::resetFromAuthoritative(authoritative, local.facingKnown ? local.angle : wire.headingDegrees,
                                          interior, state.motion)) {
      result.status = LocalGameplayStatus::InvalidState;
      return result;
    }
    state.authoritativeResetPending = false;
  }

  const movement::LocalMotionState previousMotion = state.motion;
  const gta::col::Vec3 previousPosition = previousMotion.position;
  const movement::StepResult motion = movement::advance(
      collisionWorld, controls.intent(), wire.headingDegrees, deltaSeconds, state.motion,
      serverGravity, removals);
  result.status = fromMovementStatus(motion.status);
  result.moved = motion.moved;
  result.jumped = motion.jumped;
  result.grounded = motion.grounded;
  if (motion.status != movement::StepStatus::Applied) return result;
  if (!insideServerWorldBounds(environment, local.interiorKnown && local.interior == 0u,
                               state.motion.position)) {
    // Do not fabricate the server's push-back force. Retain the last accepted
    // collision-grounded position, discard a boundary-crossing jump/velocity,
    // and still let the normal sync gate send the safe stationary candidate.
    // Heading may remain current so a player can turn back into the rectangle.
    const float currentHeading = state.motion.headingDegrees;
    state.motion = previousMotion;
    state.motion.headingDegrees = currentHeading;
    // A successful movement step may have established a floor for a freshly
    // seeded prediction. Preserve that proven grounded state while discarding
    // only the forbidden horizontal/vertical boundary crossing.
    if (motion.grounded) {
      state.motion.grounded = true;
      state.motion.verticalVelocity = 0.0f;
    }
    result.moved = std::fabs(currentHeading - previousMotion.headingDegrees) > kMovementEpsilon;
    result.jumped = false;
    result.grounded = state.motion.grounded;
  }

  const bridge::OnFootPreview preview = bridge::buildOnFootPreview(local, wire);
  if (!preview.ready()) {
    result.status = LocalGameplayStatus::InvalidSyncState;
    return result;
  }

  sync::OnFootState candidate = preview.state;
  candidate.positionX = state.motion.position.x;
  candidate.positionY = state.motion.position.y;
  candidate.positionZ = state.motion.position.z;

  // `advance` bounds its accepted timestep. Use that exact bound for horizontal
  // velocity so a delayed Android frame cannot manufacture a high-speed packet.
  const float elapsed = std::min(deltaSeconds, 0.05f);
  if (!std::isfinite(elapsed) || elapsed <= 0.0f) {
    result.status = LocalGameplayStatus::InvalidInput;
    return result;
  }
  candidate.velocityX = (state.motion.position.x - previousPosition.x) / elapsed;
  candidate.velocityY = (state.motion.position.y - previousPosition.y) / elapsed;
  candidate.velocityZ = state.motion.grounded ? 0.0f : state.motion.verticalVelocity;
  if (std::fabs(candidate.velocityX) < kMovementEpsilon) candidate.velocityX = 0.0f;
  if (std::fabs(candidate.velocityY) < kMovementEpsilon) candidate.velocityY = 0.0f;
  if (std::fabs(candidate.velocityZ) < kMovementEpsilon) candidate.velocityZ = 0.0f;

  if (!sync::isValidOnFootState(candidate)) {
    result.status = LocalGameplayStatus::InvalidSyncState;
    return result;
  }
  state.vehiclePredictionActive = false;
  state.predictionActive = true;
  result.onFoot = candidate;
  result.onFootReady = true;
  result.status = LocalGameplayStatus::Applied;
  return result;
}

LocalGameplayStep advanceLocalDriverVehicle(const state::LocalPlayer& local,
                                            const state::Vehicle& vehicle,
                                            const input::OnFootInputMapper& controls,
                                            const gta::world::CollisionWorld& collisionWorld,
                                            float deltaSeconds,
                                            LocalGameplayState& state,
                                            const state::WorldEnvironment* environment,
                                            const std::vector<gta::removal::Directive>* removals,
                                            const vehiclemotion::VehicleMotionTuning* tuning,
                                            bool manualVehicleEngineAndLights) {
  LocalGameplayStep result;
  if (local.spectatingKnown && local.spectating) {
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::Spectating;
    return result;
  }
  if (isServerDead(local)) {
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::Dead;
    return result;
  }
  if (!local.vehicleKnown || !local.inVehicle || local.vehicleId == 0) {
    result.status = LocalGameplayStatus::VehicleUnavailable;
    return result;
  }
  if (local.vehicleSeat != 0) {
    result.status = LocalGameplayStatus::PassengerSeat;
    return result;
  }
  if (!vehicle.streamedIn || vehicle.id != local.vehicleId ||
      !std::isfinite(vehicle.x) || !std::isfinite(vehicle.y) || !std::isfinite(vehicle.z) ||
      !std::isfinite(vehicle.angle) || !std::isfinite(vehicle.health)) {
    result.status = LocalGameplayStatus::VehicleUnavailable;
    return result;
  }
  if (local.controllableKnown && !local.controllable) {
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::FrozenByServer;
    return result;
  }
  // CollisionWorld partitions placements by IPL interior. Do not infer a
  // vehicle's partition from coordinates: server-local and StreamIn interior
  // facts must agree before any driver prediction is allowed. If the local
  // interior has not arrived, only the conservative exterior vehicle is safe.
  const int32_t localInterior = local.interiorKnown ? static_cast<int32_t>(local.interior) : 0;
  const int32_t vehicleInterior = static_cast<int32_t>(vehicle.interior);
  if ((local.interiorKnown && localInterior != vehicleInterior) ||
      (!local.interiorKnown && vehicleInterior != 0)) {
    state.vehicleMotion = {};
    state.vehiclePredictionActive = false;
    state.vehicleAuthoritativeResetPending = true;
    result.status = LocalGameplayStatus::CollisionUnavailable;
    return result;
  }
  if (collisionWorld.empty()) {
    state.vehicleMotion = {};
    state.vehiclePredictionActive = false;
    state.vehicleAuthoritativeResetPending = true;
    result.status = LocalGameplayStatus::CollisionUnavailable;
    return result;
  }
  if (serverVelocityIsActive(local)) {
    state.vehicleMotion = {};
    state.vehiclePredictionActive = false;
    state.vehicleAuthoritativeResetPending = true;
    result.status = LocalGameplayStatus::ServerVelocityActive;
    return result;
  }

  const input::WireInput wire = controls.wireInput();
  // A server's explicit engine-off parameter is a safe vehicle-control effect
  // only when its PlayerInit manual-engine bit says scripts own GTA's automatic
  // enter/night behavior. Keep steering/braking/coasting usable, but do not let
  // held Android throttle produce local acceleration in that authoritative
  // manual mode. Unset (-1), or a non-manual server, preserves the normal pilot.
  const bool serverEngineOff = manualVehicleEngineAndLights && vehicle.parameters.known &&
      vehicle.parameters.value(state::VehicleParameter::Engine) == 0;
  input::TouchIntent vehicleIntent = controls.intent();
  if (serverEngineOff) vehicleIntent.moveY = 0.0f;
  if (state.vehicleAuthoritativeResetPending || !state.vehicleMotion.initialized ||
      state.vehicleMotion.vehicleId != vehicle.id) {
    if (!vehiclemotion::resetFromAuthoritative(vehicle.id, {vehicle.x, vehicle.y, vehicle.z},
                                                vehicle.angle, vehicleInterior, state.vehicleMotion)) {
      result.status = LocalGameplayStatus::InvalidState;
      return result;
    }
    state.vehicleAuthoritativeResetPending = false;
  }

  if (state.observedVehicleVelocityVehicleId != vehicle.id) {
    state.observedVehicleVelocityVehicleId = vehicle.id;
    state.consumedVehicleLinearVelocityRevision = 0;
  }
  // RPC 91 carries no vehicle ID and is retained only for this exact local
  // streamed driver vehicle. Apply each linear revision once after an
  // authoritative transform seed; angular velocity remains renderer/physics
  // state until the port owns a full three-axis GTA vehicle orientation path.
  if (vehicle.linearVelocityKnown &&
      vehicle.linearVelocityRevision != state.consumedVehicleLinearVelocityRevision) {
    if (!vehiclemotion::applyServerLinearVelocity(
            vehicle.linearVelocityX, vehicle.linearVelocityY, vehicle.linearVelocityZ,
            vehicle.linearVelocityRevision, state.vehicleMotion)) {
      state.vehicleMotion = {};
      state.vehiclePredictionActive = false;
      state.vehicleAuthoritativeResetPending = true;
      result.status = LocalGameplayStatus::ServerVelocityActive;
      return result;
    }
    state.consumedVehicleLinearVelocityRevision = vehicle.linearVelocityRevision;
  }

  const vehiclemotion::LocalVehicleMotionState previousMotion = state.vehicleMotion;
  const gta::col::Vec3 previousPosition = previousMotion.position;
  const float vehicleGravity = environment != nullptr && environment->gravityKnown
                                   ? environment->gravity
                                   : 0.008f;
  const vehiclemotion::StepResult motion = vehiclemotion::advance(
      collisionWorld, vehicleIntent, deltaSeconds, state.vehicleMotion, removals, vehicleGravity, tuning);
  result.status = fromVehicleMovementStatus(motion.status);
  result.moved = motion.moved;
  result.grounded = motion.grounded;
  if (motion.status != vehiclemotion::StepStatus::Applied &&
      motion.status != vehiclemotion::StepStatus::Blocked) {
    return result;
  }
  if (!insideServerWorldBounds(environment, local.interiorKnown && local.interior == 0u,
                               state.vehicleMotion.position)) {
    // Match static-COL treatment at a recipient world edge: retain the last
    // safe transform and stop the kinematic pilot instead of making up the
    // server's push-back response or driving through its rectangle.
    const float currentHeading = state.vehicleMotion.headingDegrees;
    state.vehicleMotion = previousMotion;
    state.vehicleMotion.headingDegrees = currentHeading;
    // A just-proven floor is safe to retain even when the horizontal candidate
    // crossed the server rectangle. All travel speed is still cleared.
    if (motion.grounded) {
      state.vehicleMotion.grounded = true;
    }
    state.vehicleMotion.speed = 0.0f;
    state.vehicleMotion.lateralSpeed = 0.0f;
    state.vehicleMotion.verticalVelocity = 0.0f;
    result.status = LocalGameplayStatus::VehicleBlocked;
    result.moved = std::fabs(currentHeading - previousMotion.headingDegrees) > kMovementEpsilon;
    result.grounded = state.vehicleMotion.grounded;
  }

  const float health = local.healthKnown ? local.health : 100.0f;
  const float armour = local.armourKnown ? local.armour : 0.0f;
  if (!validSyncVital(health) || !validSyncVital(armour) || vehicle.health < 0.0f ||
      vehicle.health > 65535.0f) {
    result.status = LocalGameplayStatus::InvalidSyncState;
    return result;
  }

  sync::InCarState candidate;
  candidate.vehicleId = vehicle.id;
  candidate.leftRight = static_cast<uint16_t>(wire.leftRight);
  // Match the bounded manual-engine-off pilot in outbound authoritative sync;
  // otherwise a server could receive a fabricated held throttle despite the
  // client correctly rendering/coasting an engine-off vehicle.
  candidate.upDown = static_cast<uint16_t>(serverEngineOff ? 0 : wire.upDown);
  candidate.keys = wire.keys;
  const float halfRadians = -state.vehicleMotion.headingDegrees * kPi / 360.0f;
  candidate.quaternionW = std::cos(halfRadians);
  candidate.quaternionX = 0.0f;
  candidate.quaternionY = 0.0f;
  candidate.quaternionZ = std::sin(halfRadians);
  candidate.positionX = state.vehicleMotion.position.x;
  candidate.positionY = state.vehicleMotion.position.y;
  candidate.positionZ = state.vehicleMotion.position.z;
  const float elapsed = std::min(deltaSeconds, 0.05f);
  if (!std::isfinite(elapsed) || elapsed <= 0.0f) {
    result.status = LocalGameplayStatus::InvalidInput;
    return result;
  }
  candidate.velocityX = (state.vehicleMotion.position.x - previousPosition.x) /
                        (elapsed * kGtaPhysicsTicksPerSecond);
  candidate.velocityY = (state.vehicleMotion.position.y - previousPosition.y) /
                        (elapsed * kGtaPhysicsTicksPerSecond);
  candidate.velocityZ = state.vehicleMotion.grounded ? 0.0f :
                        state.vehicleMotion.verticalVelocity / kGtaPhysicsTicksPerSecond;
  if (std::fabs(candidate.velocityX) < kMovementEpsilon) candidate.velocityX = 0.0f;
  if (std::fabs(candidate.velocityY) < kMovementEpsilon) candidate.velocityY = 0.0f;
  if (std::fabs(candidate.velocityZ) < kMovementEpsilon) candidate.velocityZ = 0.0f;
  candidate.vehicleHealth = vehicle.health;
  candidate.playerHealth = percentByte(health);
  candidate.playerArmour = percentByte(armour);
  // The low six weapon bits come only from the server's exact armed-weapon
  // RPC. Driver FIRE/AIM never emits BulletSync or a hit/reward claim; the
  // conservative no-target path is on-foot/AIM-only. Siren is likewise only
  // mirrored when this exact streamed driver vehicle has a retained server
  // RPC-24 value; landing gear, trailers, and Hydra thrust remain neutral.
  candidate.additionalKeyWeapon = local.armedWeaponKnown ? local.armedWeapon : 0;
  candidate.siren = vehicle.parameters.known &&
          vehicle.parameters.value(state::VehicleParameter::Siren) == 1 ? 1 : 0;
  candidate.landingGear = 0;
  candidate.trailerId = 0;
  candidate.hydraThrustAngle = 0;
  if (!sync::isValidInCarState(candidate)) {
    result.status = LocalGameplayStatus::InvalidSyncState;
    return result;
  }
  state.predictionActive = false;
  state.vehiclePredictionActive = true;
  result.inCar = candidate;
  result.inCarReady = true;
  return result;
}

LocalGameplayStep buildLocalAimSync(const state::LocalPlayer& local,
                                    const input::OnFootInputMapper& controls,
                                    const LocalGameplayState& state) {
  LocalGameplayStep result;
  if (local.spectatingKnown && local.spectating) {
    result.status = LocalGameplayStatus::Spectating;
    return result;
  }
  if (isServerDead(local)) {
    result.status = LocalGameplayStatus::Dead;
    return result;
  }
  if (local.vehicleKnown && local.inVehicle) {
    result.status = LocalGameplayStatus::PassengerSeat;
    return result;
  }
  if (local.controllableKnown && !local.controllable) {
    result.status = LocalGameplayStatus::FrozenByServer;
    return result;
  }
  if ((controls.intent().heldActions & input::kHeldAim) == 0u) {
    result.status = LocalGameplayStatus::AimInactive;
    return result;
  }
  if (!state.predictionActive || !state.motion.initialized) {
    result.status = LocalGameplayStatus::CollisionUnavailable;
    return result;
  }
  const input::WireInput wire = controls.wireInput();
  if (!wire.headingKnown || !std::isfinite(wire.headingDegrees) ||
      !std::isfinite(wire.aimPitchDegrees)) {
    result.status = LocalGameplayStatus::AwaitingHeading;
    return result;
  }
  // The camera's weapon state is a server-facing protocol fact. Do not guess
  // it from FIRE/AIM input: wait until SetPlayerArmedWeapon and an exact
  // GivePlayerWeapon/SetPlayerAmmo-derived count establish both values.
  if (!local.armedWeaponKnown || local.armedWeapon >= local.weaponAmmoKnown.size() ||
      !local.weaponAmmoKnown[local.armedWeapon]) {
    result.status = LocalGameplayStatus::WeaponUnavailable;
    return result;
  }

  const float headingRadians = input::normalizeDegrees(wire.headingDegrees) * kPi / 180.0f;
  const float pitchRadians = wire.aimPitchDegrees * kPi / 180.0f;
  const float horizontalFront = std::cos(pitchRadians);
  sync::AimState candidate;
  candidate.cameraMode = aimCameraModeForWeapon(local.armedWeapon);
  candidate.cameraFrontX = std::sin(headingRadians) * horizontalFront;
  candidate.cameraFrontY = std::cos(headingRadians) * horizontalFront;
  candidate.cameraFrontZ = std::sin(pitchRadians);
  // The native collision-grounded transform is the only local geometry owned
  // by this port. Its eye-height offset is a fixed camera presentation value,
  // not a target/hit or third-person seat-offset claim.
  candidate.cameraPositionX = state.motion.position.x;
  candidate.cameraPositionY = state.motion.position.y;
  candidate.cameraPositionZ = state.motion.position.z + 0.70f;
  // open.mp's compatible NPC calculation stores negative elevation for an
  // upward front vector and positive elevation for downward. This is exactly
  // -asin(frontZ) for our normalized heading/pitch vector.
  candidate.aimZ = -pitchRadians;
  candidate.cameraZoom = 0;
  candidate.weaponState = weaponStateForAmmo(local.weaponAmmo[local.armedWeapon]);
  candidate.aspectRatio = 0;
  if (!sync::isValidAimState(candidate)) {
    result.status = LocalGameplayStatus::InvalidSyncState;
    return result;
  }
  result.aim = candidate;
  result.aimReady = true;
  result.status = LocalGameplayStatus::Applied;
  return result;
}

bool buildLocalNoTargetBulletSync(const state::LocalPlayer& local,
                                  const input::OnFootInputMapper& controls,
                                  const LocalGameplayState& state,
                                  const sync::AimState& aim,
                                  sync::BulletState& out) {
  out = {};
  // A held FIRE alone is only an on-foot key bit. A no-target BulletSync is
  // permitted only from the exact collision-grounded, server-authorized aim
  // pose already constructed above; it never turns screen coordinates into a
  // target/hit assertion.
  const uint16_t held = controls.intent().heldActions;
  if ((held & (input::kHeldAim | input::kHeldFire)) !=
      (input::kHeldAim | input::kHeldFire)) {
    return false;
  }
  if ((local.spectatingKnown && local.spectating) || isServerDead(local) ||
      (local.vehicleKnown && local.inVehicle) ||
      (local.controllableKnown && !local.controllable) ||
      !state.predictionActive || !state.motion.initialized ||
      !sync::isValidAimState(aim) || !local.armedWeaponKnown ||
      local.armedWeapon >= local.weaponAmmoKnown.size() ||
      !local.weaponAmmoKnown[local.armedWeapon] || local.weaponAmmo[local.armedWeapon] == 0u) {
    return false;
  }
  out.hitType = 0u;  // BULLET_HIT_TYPE_NONE only.
  out.hitId = 0u;
  out.originX = aim.cameraPositionX;
  out.originY = aim.cameraPositionY;
  out.originZ = aim.cameraPositionZ;
  // Start with the compatible no-hit sentinel. The separate helper below may
  // replace it only with a bounded static-COL point; it never invents an
  // entity ID, offset, target, or outcome.
  out.hitPositionX = 0.0f;
  out.hitPositionY = 0.0f;
  out.hitPositionZ = 0.0f;
  out.offsetX = 0.0f;
  out.offsetY = 0.0f;
  out.offsetZ = 0.0f;
  out.weaponId = local.armedWeapon;
  return sync::isValidNoTargetBulletState(out);
}

bool applyLocalNoTargetStaticWorldImpact(const gta::world::CollisionWorld& collisionWorld,
                                         const LocalGameplayState& state,
                                         const sync::AimState& aim,
                                         sync::BulletState& inOut,
                                         const std::vector<gta::removal::Directive>* removals) {
  // This helper intentionally has no world-entity lookup. It can only replace
  // the zero no-impact sentinel with a close face from read-only user-owned
  // COL data after the same collision-grounded state that permitted AimSync.
  if (collisionWorld.empty() || !state.predictionActive || !state.motion.initialized ||
      !sync::isValidAimState(aim) || !sync::isValidNoTargetBulletState(inOut) ||
      inOut.originX != aim.cameraPositionX || inOut.originY != aim.cameraPositionY ||
      inOut.originZ != aim.cameraPositionZ || inOut.hitPositionX != 0.0f ||
      inOut.hitPositionY != 0.0f || inOut.hitPositionZ != 0.0f) {
    return false;
  }
  const gta::col::Vec3 origin{aim.cameraPositionX, aim.cameraPositionY, aim.cameraPositionZ};
  const gta::col::Vec3 direction{aim.cameraFrontX, aim.cameraFrontY, aim.cameraFrontZ};
  gta::world::RaycastHit hit;
  if (!collisionWorld.raycastStaticWorld(origin, direction, kNoTargetStaticImpactDistance,
                                         state.motion.interior, hit, removals) ||
      !hit.valid || !hit.hit) {
    return false;
  }
  sync::BulletState candidate = inOut;
  candidate.hitPositionX = hit.position.x;
  candidate.hitPositionY = hit.position.y;
  candidate.hitPositionZ = hit.position.z;
  if (!sync::isValidNoTargetBulletState(candidate)) return false;
  inOut = candidate;
  return true;
}

LocalGameplayStep buildLocalPassengerSync(const state::LocalPlayer& local,
                                          const state::Vehicle& vehicle,
                                          const input::OnFootInputMapper& controls,
                                          LocalGameplayState& state) {
  LocalGameplayStep result;
  if (local.spectatingKnown && local.spectating) {
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::Spectating;
    return result;
  }
  if (isServerDead(local)) {
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::Dead;
    return result;
  }
  if (!local.vehicleKnown || !local.inVehicle || local.vehicleId == 0 ||
      local.vehicleSeat == 0) {
    result.status = LocalGameplayStatus::PassengerSeat;
    return result;
  }
  if (!vehicle.streamedIn || vehicle.id != local.vehicleId ||
      !std::isfinite(vehicle.x) || !std::isfinite(vehicle.y) || !std::isfinite(vehicle.z)) {
    result.status = LocalGameplayStatus::VehicleUnavailable;
    return result;
  }
  if (local.controllableKnown && !local.controllable) {
    invalidateLocalPrediction(state);
    result.status = LocalGameplayStatus::FrozenByServer;
    return result;
  }
  const float health = local.healthKnown ? local.health : 100.0f;
  const float armour = local.armourKnown ? local.armour : 0.0f;
  if (!validSyncVital(health) || !validSyncVital(armour)) {
    result.status = LocalGameplayStatus::InvalidSyncState;
    return result;
  }

  const input::WireInput wire = controls.wireInput();
  sync::PassengerState candidate;
  candidate.vehicleId = vehicle.id;
  candidate.seatId = local.vehicleSeat;
  candidate.leftRight = static_cast<uint16_t>(wire.leftRight);
  candidate.upDown = static_cast<uint16_t>(wire.upDown);
  candidate.keys = wire.keys;
  candidate.health = percentByte(health);
  candidate.armour = percentByte(armour);
  // Only a server SetPlayerSpecialAction can assert cuffed state. Absence of
  // that authoritative action leaves the legacy cuff bit clear. Passenger
  // drive-by is a distinct, tightly bounded state: held AIM plus a server-armed
  // SMG with known nonzero ammo, never a touch-selected weapon or BulletSync
  // target/damage claim. FIRE itself remains the ordinary semantic key bit.
  constexpr uint8_t kSpecialActionCuffed = 24;
  candidate.cuffed = local.specialActionKnown && local.specialAction == kSpecialActionCuffed;
  const bool driveByAimHeld = (controls.intent().heldActions & input::kHeldAim) != 0u;
  const bool driveByWeaponReady =
      local.armedWeaponKnown && isPassengerDriveByWeapon(local.armedWeapon) &&
      local.armedWeapon < local.weaponAmmoKnown.size() && local.weaponAmmoKnown[local.armedWeapon] &&
      local.weaponAmmo[local.armedWeapon] != 0u;
  candidate.driveBy = !candidate.cuffed && driveByAimHeld && driveByWeaponReady;
  candidate.weaponAdditionalKey = local.armedWeaponKnown ? local.armedWeapon : 0;
  // open.mp's inbound handler ultimately anchors a passenger to vehicle
  // position. Copy this streamed server transform exactly instead of an
  // invented seat offset or last on-foot coordinate.
  candidate.positionX = vehicle.x;
  candidate.positionY = vehicle.y;
  candidate.positionZ = vehicle.z;
  if (!sync::isValidPassengerState(candidate)) {
    result.status = LocalGameplayStatus::InvalidSyncState;
    return result;
  }

  // Passenger packets carry input/vitals only; no passenger movement or
  // vehicle prediction is simulated on Android.
  state.predictionActive = false;
  state.vehiclePredictionActive = false;
  result.passenger = candidate;
  result.passengerReady = true;
  result.status = LocalGameplayStatus::Applied;
  return result;
}

}  // namespace samp::gameplay
