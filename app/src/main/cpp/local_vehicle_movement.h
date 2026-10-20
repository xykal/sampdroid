// Conservative collision-grounded vehicle motion pilot.
//
// This owns no network or render state. It is intentionally a bounded
// kinematic fallback while the native GTA vehicle physics stack is still being
// built: it only moves a server-authorized driver over user-owned COL ground
// and refuses absent ground or blocked geometry instead of inventing a route.
#pragma once

#include "gta_collision_world.h"
#include "input_state.h"

#include <cstdint>
#include <vector>

namespace samp::vehiclemotion {

// One bounded data-informed envelope for the local collision pilot. It remains
// intentionally smaller than GTA SA's complete handling model: no suspension,
// wheel contact, gearbox, tyre, collision damage, or angular physics is
// inferred from a handling.cfg row. A null/invalid tuning keeps the stable
// generic fallback below.
struct VehicleMotionTuning {
  float forwardAcceleration = 11.0f;
  float reverseAcceleration = 6.0f;
  float serviceBrakeDeceleration = 18.0f;
  float handBrakeDeceleration = 28.0f;
  float coastDeceleration = 2.8f;
  float lateralCoastDeceleration = 9.0f;
  float maxForwardSpeed = 32.0f;
  float maxReverseSpeed = 10.0f;
  float maxSteeringDegreesPerSecond = 95.0f;
};

// Checks the compact conservative kinematic envelope before it can affect a
// source-authorized local driver. Public because the game-data bridge must be
// able to reject a malformed user-owned handling.cfg conversion before the
// session mutex is entered.
bool validMotionTuning(const VehicleMotionTuning& tuning);

struct LocalVehicleMotionState {
  bool initialized = false;
  bool grounded = false;
  uint16_t vehicleId = 0;
  gta::col::Vec3 position;
  float headingDegrees = 0.0f;
  // Signed forward world-units per second. Positive follows the GTA/scene
  // heading; negative is bounded reverse motion. This remains the ordinary
  // touch-throttle component of the fallback pilot.
  float speed = 0.0f;
  // Server SetVehicleVelocity can carry a world-space vector that is not
  // aligned with the current yaw. Keep lateral/vertical components separate
  // rather than rotating the car to fit the vector or silently dropping it.
  float lateralSpeed = 0.0f;
  float verticalVelocity = 0.0f;
  // Last accepted recipient-local linear RPC-91 revision. It belongs to motion
  // state only; LocalGameplayState keeps its own cross-reset observation token.
  uint64_t appliedServerLinearVelocityRevision = 0;
  int32_t interior = 0;
};

enum class StepStatus : uint8_t {
  Applied,
  MissingGround,
  Blocked,
  InvalidState,
  InvalidInput,
};

struct StepResult {
  StepStatus status = StepStatus::InvalidState;
  bool moved = false;
  bool blocked = false;
  bool grounded = false;
  float speed = 0.0f;
};

// Starts/reconciles vehicle prediction only from a known streamed vehicle
// transform that the server has assigned to the local driver.
bool resetFromAuthoritative(uint16_t vehicleId, const gta::col::Vec3& position,
                            float headingDegrees, int32_t interior,
                            LocalVehicleMotionState& out);

// Applies one validated, source-unit CVector from recipient-local RPC 91 to
// an already initialized driver pilot. GTA/SA-MP movement speed vectors are
// per 50 Hz physics tick, while this bounded pilot stores world units/second;
// the conversion is centralized here. It supports only the finite bounded
// linear channel—not angular orientation—so a caller cannot turn X/Y/Z angular
// data into a made-up yaw/pitch/roll transform.
bool applyServerLinearVelocity(float velocityX, float velocityY, float velocityZ,
                               uint64_t revision, LocalVehicleMotionState& state);

// Advances bounded throttle/steering over the collision world. `moveY` is
// throttle/reverse, `moveX` is steering, and JUMP is a conservative brake.
// It has no authority to enter a vehicle, mutate a WorldState vehicle, or send
// a packet; its caller must still construct/validate Packet 200 separately.
StepResult advance(const gta::world::CollisionWorld& collisionWorld,
                   const input::TouchIntent& intent, float deltaSeconds,
                   LocalVehicleMotionState& state,
                   const std::vector<gta::removal::Directive>* removals = nullptr,
                   float serverGravity = 0.008f,
                   const VehicleMotionTuning* tuning = nullptr);

const char* stepStatusName(StepStatus status);

}  // namespace samp::vehiclemotion
