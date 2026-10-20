// Bounded reader for the user-owned GTA SA `data/handling.cfg` table.
//
// This parser intentionally keeps only the conservative fields needed by the
// Android client's collision-grounded local-driver pilot. It does not execute
// scripts, load a PC handling mod, or attempt to reproduce GTA's full vehicle
// simulation/suspension/drivetrain.
#pragma once

#include "local_vehicle_movement.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace samp::gta::handling {

constexpr size_t kMaxHandlingProfiles = 1024;

// Normalized source fields from one GTA SA handling.cfg row. `id` is lowercase
// ASCII and joins the `cars` IDE handling token; it is never a filesystem path.
struct Profile {
  std::string id;
  float dragMultiplier = 0.0f;
  float tractionMultiplier = 0.0f;
  float tractionLoss = 0.0f;
  float tractionBias = 0.0f;
  uint8_t numberOfGears = 0;
  float maxVelocityKph = 0.0f;
  float engineAcceleration = 0.0f;
  float engineInertia = 0.0f;
  float brakeDeceleration = 0.0f;
  float brakeBias = 0.0f;
  float steeringLockDegrees = 0.0f;
};

// Strictly parses standard whitespace-delimited GTA SA handling rows. Comments
// and blank lines are ignored. At least the first 21 documented fields are
// validated; the later model/flags/lights fields remain opaque so stock and
// compatible extension tails do not become gameplay authority. Duplicate IDs,
// malformed numeric values, unsafe ranges, embedded controls, and cap overflow
// fail closed and leave `out` unchanged.
bool parseHandlingCfg(std::string_view text,
                      std::unordered_map<std::string, Profile>& out,
                      std::string& error,
                      size_t maxProfiles = kMaxHandlingProfiles);

// Converts only validated GTA source coefficients into the bounded kinematic
// envelope used by LocalVehicleMotion. This is deliberately data-informed, not
// a claim of full GTA handling fidelity. Returns false for an invalid profile.
bool makeMotionTuning(const Profile& profile,
                      vehiclemotion::VehicleMotionTuning& out);

}  // namespace samp::gta::handling
