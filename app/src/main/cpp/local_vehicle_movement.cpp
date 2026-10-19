#include "local_vehicle_movement.h"

#include <algorithm>
#include <cmath>

namespace samp::vehiclemotion {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaxStepSeconds = 0.05f;
constexpr float kForwardAcceleration = 11.0f;
constexpr float kReverseAcceleration = 6.0f;
constexpr float kServiceBrakeDeceleration = 18.0f;
constexpr float kHandBrakeDeceleration = 28.0f;
constexpr float kCoastDeceleration = 2.8f;
constexpr float kMaxForwardSpeed = 32.0f;
constexpr float kMaxReverseSpeed = 10.0f;
constexpr float kMaxSteeringDegreesPerSecond = 95.0f;
constexpr float kVehicleCollisionRadius = 1.05f;
constexpr float kVehicleCollisionHeight = 1.30f;
constexpr float kGroundRiseLimit = 1.20f;
constexpr float kGroundDropLimit = 3.0f;
constexpr float kEpsilon = 0.0001f;
constexpr float kMaxCoordinate = 30000.0f;

bool finitePosition(const gta::col::Vec3& position) {
  return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
         std::fabs(position.x) < kMaxCoordinate && std::fabs(position.y) < kMaxCoordinate &&
         std::fabs(position.z) < kMaxCoordinate;
}

bool floorNear(const gta::world::CollisionWorld& collisionWorld, float x, float y, float centerZ,
               int32_t interior, gta::col::FloorHit& floor) {
  return collisionWorld.findHighestFloor(x, y, centerZ - kGroundDropLimit,
                                        centerZ + kGroundRiseLimit, interior, floor);
}

float moveTowardZero(float value, float amount) {
  if (value > 0.0f) return std::max(0.0f, value - amount);
  if (value < 0.0f) return std::min(0.0f, value + amount);
  return 0.0f;
}

}  // namespace

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

StepResult advance(const gta::world::CollisionWorld& collisionWorld,
                   const input::TouchIntent& intent, float deltaSeconds,
                   LocalVehicleMotionState& state) {
  StepResult result;
  if (!state.initialized || state.vehicleId == 0 || !finitePosition(state.position) ||
      !std::isfinite(state.headingDegrees) || !std::isfinite(state.speed)) {
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
  LocalVehicleMotionState next = state;
  gta::col::FloorHit currentFloor;
  if (!floorNear(collisionWorld, next.position.x, next.position.y, next.position.z,
                 next.interior, currentFloor)) {
    result.status = StepStatus::MissingGround;
    return result;
  }
  next.position.z = currentFloor.z;
  next.grounded = true;

  const float throttle = sanitized.moveY;
  const bool hardBrake = (sanitized.heldActions & input::kHeldJump) != 0 ||
                         (sanitized.heldActions & input::kHeldSprint) != 0;
  if (hardBrake) {
    next.speed = moveTowardZero(next.speed, kHandBrakeDeceleration * stepSeconds);
  } else if (throttle > 0.0f) {
    // Pressing forward while slowly reversing brakes first rather than instantly
    // flipping a vehicle trajectory through zero.
    if (next.speed < 0.0f) {
      next.speed = std::min(0.0f, next.speed + kServiceBrakeDeceleration * throttle * stepSeconds);
    } else {
      next.speed = std::min(kMaxForwardSpeed,
                            next.speed + kForwardAcceleration * throttle * stepSeconds);
    }
  } else if (throttle < 0.0f) {
    if (next.speed > 0.0f) {
      next.speed = std::max(0.0f, next.speed + kServiceBrakeDeceleration * throttle * stepSeconds);
    } else {
      next.speed = std::max(-kMaxReverseSpeed,
                            next.speed + kReverseAcceleration * throttle * stepSeconds);
    }
  } else {
    next.speed = moveTowardZero(next.speed, kCoastDeceleration * stepSeconds);
  }

  // Steering is speed-sensitive but remains controllable at low speed. Camera
  // look remains camera-only; a user must move the joystick horizontally to
  // steer the authoritative driver vehicle.
  const float steeringStrength = std::clamp(std::fabs(next.speed) / 6.0f, 0.18f, 1.0f);
  const float travelSign = next.speed < 0.0f ? -1.0f : 1.0f;
  next.headingDegrees = input::normalizeDegrees(
      next.headingDegrees + sanitized.moveX * travelSign * kMaxSteeringDegreesPerSecond *
                                steeringStrength * stepSeconds);

  const float headingRadians = next.headingDegrees * kPi / 180.0f;
  const float targetX = next.position.x + std::sin(headingRadians) * next.speed * stepSeconds;
  const float targetY = next.position.y + std::cos(headingRadians) * next.speed * stepSeconds;
  const gta::world::HorizontalMoveResult horizontal = collisionWorld.resolveHorizontalMove(
      next.position.x, next.position.y, targetX, targetY, next.position.z,
      kVehicleCollisionRadius, kVehicleCollisionHeight, next.interior);
  if (!horizontal.valid) {
    result.status = StepStatus::InvalidState;
    return result;
  }

  gta::col::FloorHit targetFloor;
  if (!floorNear(collisionWorld, horizontal.position.x, horizontal.position.y, next.position.z,
                 next.interior, targetFloor)) {
    result.status = StepStatus::MissingGround;
    return result;
  }
  if (targetFloor.z - currentFloor.z > kGroundRiseLimit ||
      currentFloor.z - targetFloor.z > kGroundDropLimit) {
    result.status = StepStatus::MissingGround;
    return result;
  }

  next.position.x = horizontal.position.x;
  next.position.y = horizontal.position.y;
  next.position.z = targetFloor.z;
  if (horizontal.blocked) {
    // A static world collision is an immediate local stop. The next server
    // Packet 200 correction can always reset this prediction.
    next.speed = 0.0f;
    result.blocked = true;
  }
  if (!finitePosition(next.position) || !std::isfinite(next.speed)) {
    result.status = StepStatus::InvalidState;
    return result;
  }

  result.moved = std::fabs(next.position.x - state.position.x) > kEpsilon ||
                 std::fabs(next.position.y - state.position.y) > kEpsilon ||
                 std::fabs(next.position.z - state.position.z) > kEpsilon ||
                 std::fabs(next.headingDegrees - state.headingDegrees) > kEpsilon;
  result.grounded = true;
  result.speed = next.speed;
  state = next;
  result.status = result.blocked ? StepStatus::Blocked : StepStatus::Applied;
  return result;
}

}  // namespace samp::vehiclemotion
