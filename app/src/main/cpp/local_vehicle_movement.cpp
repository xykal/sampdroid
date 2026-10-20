#include "local_vehicle_movement.h"

#include <algorithm>
#include <cmath>

namespace samp::vehiclemotion {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaxStepSeconds = 0.05f;
constexpr VehicleMotionTuning kDefaultMotionTuning{};
// The user-owned handling.cfg bridge is allowed to specialize this fallback,
// but never outside these collision-pilot safety envelopes.
constexpr float kMinForwardAcceleration = 0.5f;
constexpr float kMaxForwardAcceleration = 45.0f;
constexpr float kMinReverseAcceleration = 0.25f;
constexpr float kMaxReverseAcceleration = 30.0f;
constexpr float kMinBrakeDeceleration = 1.0f;
constexpr float kMaxBrakeDeceleration = 70.0f;
constexpr float kMinCoastDeceleration = 0.1f;
constexpr float kMaxCoastDeceleration = 15.0f;
constexpr float kMinLateralCoastDeceleration = 0.5f;
constexpr float kMaxLateralCoastDeceleration = 35.0f;
constexpr float kMinForwardSpeed = 3.0f;
constexpr float kMaxForwardSpeed = 100.0f;
constexpr float kMinReverseSpeed = 1.0f;
constexpr float kMaxReverseSpeed = 35.0f;
constexpr float kMinSteeringDegreesPerSecond = 20.0f;
constexpr float kMaxSteeringDegreesPerSecond = 210.0f;
constexpr float kVehicleCollisionRadius = 1.05f;
constexpr float kVehicleCollisionHeight = 1.30f;
constexpr float kGroundRiseLimit = 1.20f;
constexpr float kGroundDropLimit = 3.0f;
// Source vector values are GTA CPhysical move-speed values per physics tick.
// GTA SA's game-space velocity convention is 50 fixed steps per second; retain
// this conversion in one place rather than treating a raw CVector as m/s.
constexpr float kGtaPhysicsTicksPerSecond = 50.0f;
constexpr float kMaxSupportedRpc91LinearSpeed = 3.0f;
constexpr float kDefaultServerGravity = 0.008f;
constexpr float kMinServerGravity = 0.0001f;
constexpr float kMaxServerGravity = 0.1f;
constexpr float kBaseGravityAcceleration = 15.0f;
// A direct source command may launch a vehicle far above the short grounded
// step range. Bound floor lookup rather than turning missing collision data
// into unrestricted free-flight.
constexpr float kMaxAirborneFloorDrop = 900.0f;
constexpr float kEpsilon = 0.0001f;
constexpr float kMaxCoordinate = 30000.0f;

bool finitePosition(const gta::col::Vec3& position) {
  return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
         std::fabs(position.x) < kMaxCoordinate && std::fabs(position.y) < kMaxCoordinate &&
         std::fabs(position.z) < kMaxCoordinate;
}

bool floorNear(const gta::world::CollisionWorld& collisionWorld, float x, float y, float centerZ,
               int32_t interior, gta::col::FloorHit& floor,
               const std::vector<gta::removal::Directive>* removals) {
  return collisionWorld.findHighestFloor(x, y, centerZ - kGroundDropLimit,
                                        centerZ + kGroundRiseLimit, interior, floor, removals);
}

bool floorBelow(const gta::world::CollisionWorld& collisionWorld, float x, float y, float centerZ,
                int32_t interior, gta::col::FloorHit& floor,
                const std::vector<gta::removal::Directive>* removals) {
  return collisionWorld.findHighestFloor(x, y, centerZ - kMaxAirborneFloorDrop,
                                        centerZ + kGroundRiseLimit, interior, floor, removals);
}

bool validServerGravity(float gravity) {
  return std::isfinite(gravity) && gravity >= kMinServerGravity && gravity <= kMaxServerGravity;
}

float moveTowardZero(float value, float amount) {
  if (value > 0.0f) return std::max(0.0f, value - amount);
  if (value < 0.0f) return std::min(0.0f, value + amount);
  return 0.0f;
}

}  // namespace

bool validMotionTuning(const VehicleMotionTuning& tuning) {
  return std::isfinite(tuning.forwardAcceleration) &&
         std::isfinite(tuning.reverseAcceleration) &&
         std::isfinite(tuning.serviceBrakeDeceleration) &&
         std::isfinite(tuning.handBrakeDeceleration) &&
         std::isfinite(tuning.coastDeceleration) &&
         std::isfinite(tuning.lateralCoastDeceleration) &&
         std::isfinite(tuning.maxForwardSpeed) &&
         std::isfinite(tuning.maxReverseSpeed) &&
         std::isfinite(tuning.maxSteeringDegreesPerSecond) &&
         tuning.forwardAcceleration >= kMinForwardAcceleration &&
         tuning.forwardAcceleration <= kMaxForwardAcceleration &&
         tuning.reverseAcceleration >= kMinReverseAcceleration &&
         tuning.reverseAcceleration <= kMaxReverseAcceleration &&
         tuning.serviceBrakeDeceleration >= kMinBrakeDeceleration &&
         tuning.serviceBrakeDeceleration <= kMaxBrakeDeceleration &&
         tuning.handBrakeDeceleration >= tuning.serviceBrakeDeceleration &&
         tuning.handBrakeDeceleration <= kMaxBrakeDeceleration &&
         tuning.coastDeceleration >= kMinCoastDeceleration &&
         tuning.coastDeceleration <= kMaxCoastDeceleration &&
         tuning.lateralCoastDeceleration >= kMinLateralCoastDeceleration &&
         tuning.lateralCoastDeceleration <= kMaxLateralCoastDeceleration &&
         tuning.maxForwardSpeed >= kMinForwardSpeed &&
         tuning.maxForwardSpeed <= kMaxForwardSpeed &&
         tuning.maxReverseSpeed >= kMinReverseSpeed &&
         tuning.maxReverseSpeed <= kMaxReverseSpeed &&
         tuning.maxSteeringDegreesPerSecond >= kMinSteeringDegreesPerSecond &&
         tuning.maxSteeringDegreesPerSecond <= kMaxSteeringDegreesPerSecond;
}

const char* stepStatusName(StepStatus status) {
  switch (status) {
    case StepStatus::Applied:
      return "applied";
    case StepStatus::MissingGround:
      return "missing-ground";
    case StepStatus::Blocked:
      return "blocked";
    case StepStatus::InvalidState:
      return "invalid-state";
    case StepStatus::InvalidInput:
      return "invalid-input";
  }
  return "invalid-state";
}

bool resetFromAuthoritative(uint16_t vehicleId, const gta::col::Vec3& position,
                            float headingDegrees, int32_t interior,
                            LocalVehicleMotionState& out) {
  if (vehicleId == 0 || !finitePosition(position) || !std::isfinite(headingDegrees)) return false;
  out = {};
  out.initialized = true;
  out.vehicleId = vehicleId;
  out.position = position;
  out.headingDegrees = input::normalizeDegrees(headingDegrees);
  out.interior = interior;
  return true;
}

bool applyServerLinearVelocity(float velocityX, float velocityY, float velocityZ,
                               uint64_t revision, LocalVehicleMotionState& state) {
  if (!state.initialized || state.vehicleId == 0 || revision == 0 ||
      !finitePosition(state.position) || !std::isfinite(state.headingDegrees) ||
      !std::isfinite(velocityX) || !std::isfinite(velocityY) || !std::isfinite(velocityZ)) {
    return false;
  }
  const double magnitudeSquared = static_cast<double>(velocityX) * velocityX +
                                  static_cast<double>(velocityY) * velocityY +
                                  static_cast<double>(velocityZ) * velocityZ;
  if (!std::isfinite(magnitudeSquared) ||
      magnitudeSquared > static_cast<double>(kMaxSupportedRpc91LinearSpeed) *
                             kMaxSupportedRpc91LinearSpeed) {
    // Retaining a broader valid reducer vector is safe, but this early
    // collision pilot deliberately refuses to turn an extreme command into an
    // unchecked long-range local trajectory.
    return false;
  }
  const float worldX = velocityX * kGtaPhysicsTicksPerSecond;
  const float worldY = velocityY * kGtaPhysicsTicksPerSecond;
  const float worldZ = velocityZ * kGtaPhysicsTicksPerSecond;
  if (!std::isfinite(worldX) || !std::isfinite(worldY) || !std::isfinite(worldZ)) return false;
  const float headingRadians = state.headingDegrees * kPi / 180.0f;
  const float forwardX = std::sin(headingRadians);
  const float forwardY = std::cos(headingRadians);
  const float rightX = std::cos(headingRadians);
  const float rightY = -std::sin(headingRadians);
  state.speed = worldX * forwardX + worldY * forwardY;
  state.lateralSpeed = worldX * rightX + worldY * rightY;
  state.verticalVelocity = worldZ;
  // A nonzero Z command is a source-directed launch (e.g. the documented
  // 0.2 car jump). Do not snap it back to a floor before the next step.
  state.grounded = std::fabs(worldZ) <= kEpsilon;
  state.appliedServerLinearVelocityRevision = revision;
  return std::isfinite(state.speed) && std::isfinite(state.lateralSpeed);
}

StepResult advance(const gta::world::CollisionWorld& collisionWorld,
                   const input::TouchIntent& intent, float deltaSeconds,
                   LocalVehicleMotionState& state,
                   const std::vector<gta::removal::Directive>* removals,
                   float serverGravity,
                   const VehicleMotionTuning* tuning) {
  StepResult result;
  // An absent or rejected source profile has exactly the historic generic
  // behavior. Never let a malformed/hostile game-data row become a movement
  // multiplier just because the file was user-selected through SAF.
  const VehicleMotionTuning& motionTuning =
      tuning != nullptr && validMotionTuning(*tuning) ? *tuning : kDefaultMotionTuning;
  if (!state.initialized || state.vehicleId == 0 || !finitePosition(state.position) ||
      !std::isfinite(state.headingDegrees) || !std::isfinite(state.speed) ||
      !std::isfinite(state.lateralSpeed) || !std::isfinite(state.verticalVelocity) ||
      !validServerGravity(serverGravity)) {
    result.status = StepStatus::InvalidState;
    return result;
  }
  input::TouchIntent sanitized;
  if (!input::sanitizeTouchIntent(intent, sanitized) || !std::isfinite(deltaSeconds) ||
      deltaSeconds <= 0.0f) {
    result.status = StepStatus::InvalidInput;
    return result;
  }

  const float stepSeconds = std::min(deltaSeconds, kMaxStepSeconds);
  const float gravityAcceleration = kBaseGravityAcceleration *
                                    (serverGravity / kDefaultServerGravity);
  if (!std::isfinite(gravityAcceleration) || gravityAcceleration <= 0.0f) {
    result.status = StepStatus::InvalidState;
    return result;
  }
  LocalVehicleMotionState next = state;
  gta::col::FloorHit currentFloor;
  const bool hasCurrentFloor = floorNear(collisionWorld, next.position.x, next.position.y,
                                         next.position.z, next.interior, currentFloor, removals);
  // Ordinary driving is floor-bound. A source Z vector alone may opt into the
  // bounded airborne branch below; do not claim arbitrary vehicle flight.
  if (next.grounded || std::fabs(next.verticalVelocity) <= kEpsilon) {
    if (!hasCurrentFloor) {
      result.status = StepStatus::MissingGround;
      return result;
    }
    next.position.z = currentFloor.z;
    next.verticalVelocity = 0.0f;
    next.grounded = true;
  }

  const float throttle = sanitized.moveY;
  const bool hardBrake = (sanitized.heldActions & input::kHeldJump) != 0 ||
                         (sanitized.heldActions & input::kHeldSprint) != 0;
  if (hardBrake) {
    next.speed = moveTowardZero(next.speed, motionTuning.handBrakeDeceleration * stepSeconds);
  } else if (throttle > 0.0f) {
    // Pressing forward while slowly reversing brakes first rather than instantly
    // flipping a vehicle trajectory through zero.
    if (next.speed < 0.0f) {
      next.speed = std::min(0.0f, next.speed + motionTuning.serviceBrakeDeceleration *
                                              throttle * stepSeconds);
    } else {
      next.speed = std::min(motionTuning.maxForwardSpeed,
                            next.speed + motionTuning.forwardAcceleration *
                                             throttle * stepSeconds);
    }
  } else if (throttle < 0.0f) {
    if (next.speed > 0.0f) {
      next.speed = std::max(0.0f, next.speed + motionTuning.serviceBrakeDeceleration *
                                              throttle * stepSeconds);
    } else {
      next.speed = std::max(-motionTuning.maxReverseSpeed,
                            next.speed + motionTuning.reverseAcceleration *
                                             throttle * stepSeconds);
    }
  } else {
    next.speed = moveTowardZero(next.speed, motionTuning.coastDeceleration * stepSeconds);
  }
  // Ground traction damps a server-supplied lateral component gradually. It
  // does not rewrite the first accepted direction or rotate the vehicle into
  // it, which would fabricate a GTA steering response.
  next.lateralSpeed = moveTowardZero(next.lateralSpeed,
                                     motionTuning.lateralCoastDeceleration * stepSeconds);

  // Steering is speed-sensitive but remains controllable at low speed. Camera
  // look remains camera-only; a user must move the joystick horizontally to
  // steer the authoritative driver vehicle.
  const float steeringStrength = std::clamp(std::fabs(next.speed) / 6.0f, 0.18f, 1.0f);
  const float travelSign = next.speed < 0.0f ? -1.0f : 1.0f;
  next.headingDegrees = input::normalizeDegrees(
      next.headingDegrees + sanitized.moveX * travelSign *
                                motionTuning.maxSteeringDegreesPerSecond *
                                steeringStrength * stepSeconds);

  const float headingRadians = next.headingDegrees * kPi / 180.0f;
  const float forwardX = std::sin(headingRadians);
  const float forwardY = std::cos(headingRadians);
  const float rightX = std::cos(headingRadians);
  const float rightY = -std::sin(headingRadians);
  const float horizontalVelocityX = forwardX * next.speed + rightX * next.lateralSpeed;
  const float horizontalVelocityY = forwardY * next.speed + rightY * next.lateralSpeed;
  const float targetX = next.position.x + horizontalVelocityX * stepSeconds;
  const float targetY = next.position.y + horizontalVelocityY * stepSeconds;
  const gta::world::HorizontalMoveResult horizontal = collisionWorld.resolveHorizontalMove(
      next.position.x, next.position.y, targetX, targetY, next.position.z,
      kVehicleCollisionRadius, kVehicleCollisionHeight, next.interior, removals);
  if (!horizontal.valid) {
    result.status = StepStatus::InvalidState;
    return result;
  }

  next.position.x = horizontal.position.x;
  next.position.y = horizontal.position.y;
  if (horizontal.blocked) {
    // A static world collision is an immediate local horizontal stop. A source
    // launch may still land through the bounded vertical branch below.
    next.speed = 0.0f;
    next.lateralSpeed = 0.0f;
    result.blocked = true;
  }

  if (next.grounded) {
    gta::col::FloorHit targetFloor;
    if (!floorNear(collisionWorld, next.position.x, next.position.y, next.position.z,
                   next.interior, targetFloor, removals)) {
      result.status = StepStatus::MissingGround;
      return result;
    }
    if (!hasCurrentFloor || targetFloor.z - currentFloor.z > kGroundRiseLimit ||
        currentFloor.z - targetFloor.z > kGroundDropLimit) {
      result.status = StepStatus::MissingGround;
      return result;
    }
    next.position.z = targetFloor.z;
    next.verticalVelocity = 0.0f;
  } else {
    const float previousZ = next.position.z;
    next.verticalVelocity -= gravityAcceleration * stepSeconds;
    const float targetZ = previousZ + next.verticalVelocity * stepSeconds;
    gta::col::FloorHit landingFloor;
    const bool hasLandingFloor = floorBelow(
        collisionWorld, next.position.x, next.position.y, std::max(previousZ, targetZ),
        next.interior, landingFloor, removals);
    if (next.verticalVelocity <= 0.0f && hasLandingFloor &&
        landingFloor.z <= previousZ + kGroundRiseLimit && landingFloor.z >= targetZ - kEpsilon) {
      next.position.z = landingFloor.z;
      next.verticalVelocity = 0.0f;
      next.grounded = true;
    } else if (hasLandingFloor) {
      next.position.z = targetZ;
    } else {
      // Incomplete user-owned collision data cannot become a free-flight
      // route. Keep the prior valid transform and wait for authoritative state.
      result.status = StepStatus::MissingGround;
      return result;
    }
  }

  if (!finitePosition(next.position) || !std::isfinite(next.speed) ||
      !std::isfinite(next.lateralSpeed) || !std::isfinite(next.verticalVelocity)) {
    result.status = StepStatus::InvalidState;
    return result;
  }

  result.moved = std::fabs(next.position.x - state.position.x) > kEpsilon ||
                 std::fabs(next.position.y - state.position.y) > kEpsilon ||
                 std::fabs(next.position.z - state.position.z) > kEpsilon ||
                 std::fabs(next.headingDegrees - state.headingDegrees) > kEpsilon;
  result.grounded = next.grounded;
  result.speed = next.speed;
  state = next;
  result.status = result.blocked ? StepStatus::Blocked : StepStatus::Applied;
  return result;
}

}  // namespace samp::vehiclemotion
