#include "gta_vehicle_handling.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

namespace samp::gta::handling {
namespace {

constexpr size_t kMaxHandlingLineBytes = 4096;
constexpr size_t kMaxHandlingIdBytes = 31;
constexpr size_t kRequiredFields = 21;
constexpr size_t kMaxFieldsPerLine = 48;

bool asciiWhitespace(char character) {
  return character == ' ' || character == '\t' || character == '\r' || character == '\n' ||
         character == '\f' || character == '\v';
}

std::string_view trim(std::string_view value) {
  while (!value.empty() && asciiWhitespace(value.front())) value.remove_prefix(1);
  while (!value.empty() && asciiWhitespace(value.back())) value.remove_suffix(1);
  return value;
}

bool nextLine(std::string_view& input, std::string_view& line) {
  if (input.empty()) return false;
  const size_t ending = input.find('\n');
  if (ending == std::string_view::npos) {
    line = input;
    input = {};
  } else {
    line = input.substr(0, ending);
    input.remove_prefix(ending + 1u);
  }
  line = trim(line);
  return true;
}

bool splitWhitespace(std::string_view line, std::vector<std::string_view>& fields) {
  fields.clear();
  while (!line.empty()) {
    while (!line.empty() && asciiWhitespace(line.front())) line.remove_prefix(1);
    if (line.empty() || line.front() == ';' || line.front() == '#') return true;
    const size_t ending = line.find_first_of(" \t\r\n\f\v;#");
    const std::string_view token = line.substr(0, ending);
    if (token.empty() || fields.size() >= kMaxFieldsPerLine) return false;
    fields.push_back(token);
    if (ending == std::string_view::npos) return true;
    if (line[ending] == ';' || line[ending] == '#') return true;
    line.remove_prefix(ending + 1u);
  }
  return true;
}

bool normalizeId(std::string_view input, std::string& out) {
  out.clear();
  input = trim(input);
  if (input.empty() || input.size() > kMaxHandlingIdBytes) return false;
  out.reserve(input.size());
  for (const char character : input) {
    if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '_')) {
      out.clear();
      return false;
    }
    out.push_back(character >= 'A' && character <= 'Z'
                      ? static_cast<char>(character - 'A' + 'a')
                      : character);
  }
  return true;
}

bool parseFiniteFloat(std::string_view token, float& out) {
  if (token.empty() || token.size() > 63u) return false;
  std::string owned(token);
  char* end = nullptr;
  const float parsed = std::strtof(owned.c_str(), &end);
  if (end == owned.c_str() || end == nullptr || *end != '\0' || !std::isfinite(parsed)) return false;
  out = parsed;
  return true;
}

bool parseUnsignedByte(std::string_view token, uint8_t& out) {
  if (token.empty() || token.size() > 3u) return false;
  unsigned value = 0;
  for (const char character : token) {
    if (character < '0' || character > '9') return false;
    value = value * 10u + static_cast<unsigned>(character - '0');
    if (value > std::numeric_limits<uint8_t>::max()) return false;
  }
  out = static_cast<uint8_t>(value);
  return true;
}

bool validProfile(const Profile& profile) {
  // Wider ingestion limits keep legal custom/offline handling tables usable;
  // makeMotionTuning applies the much tighter Android kinematic bounds.
  return !profile.id.empty() && std::isfinite(profile.dragMultiplier) &&
         std::isfinite(profile.tractionMultiplier) && std::isfinite(profile.tractionLoss) &&
         std::isfinite(profile.tractionBias) && std::isfinite(profile.maxVelocityKph) &&
         std::isfinite(profile.engineAcceleration) && std::isfinite(profile.engineInertia) &&
         std::isfinite(profile.brakeDeceleration) && std::isfinite(profile.brakeBias) &&
         std::isfinite(profile.steeringLockDegrees) && profile.numberOfGears >= 1u &&
         profile.numberOfGears <= 10u && profile.dragMultiplier >= 0.0f &&
         profile.dragMultiplier <= 100.0f && profile.tractionMultiplier >= 0.0f &&
         profile.tractionMultiplier <= 100.0f && profile.tractionLoss >= 0.0f &&
         profile.tractionLoss <= 100.0f && profile.tractionBias >= 0.0f &&
         profile.tractionBias <= 1.0f && profile.maxVelocityKph >= 1.0f &&
         profile.maxVelocityKph <= 1000.0f && profile.engineAcceleration >= 0.0f &&
         profile.engineAcceleration <= 500.0f && profile.engineInertia >= 0.0f &&
         profile.engineInertia <= 500.0f && profile.brakeDeceleration >= 0.0f &&
         profile.brakeDeceleration <= 500.0f && profile.brakeBias >= 0.0f &&
         profile.brakeBias <= 1.0f && profile.steeringLockDegrees >= 1.0f &&
         profile.steeringLockDegrees <= 180.0f;
}

}  // namespace

bool parseHandlingCfg(std::string_view text,
                      std::unordered_map<std::string, Profile>& out,
                      std::string& error,
                      size_t maxProfiles) {
  error.clear();
  if (text.empty() || maxProfiles == 0u || maxProfiles > kMaxHandlingProfiles) {
    error = "batas atau isi handling.cfg tidak valid";
    return false;
  }
  std::unordered_map<std::string, Profile> candidate;
  candidate.reserve(std::min(maxProfiles, static_cast<size_t>(256u)));
  std::vector<std::string_view> fields;
  std::string_view remaining = text;
  std::string_view line;
  size_t lineNumber = 0u;
  while (nextLine(remaining, line)) {
    ++lineNumber;
    if (line.empty()) continue;
    if (line.size() > kMaxHandlingLineBytes) {
      error = "baris handling.cfg melebihi batas aman";
      return false;
    }
    if (line.front() == ';' || line.front() == '#') continue;
    if (!splitWhitespace(line, fields) || fields.empty()) {
      error = "record handling.cfg tidak valid pada baris " + std::to_string(lineNumber);
      return false;
    }
    if (fields.size() < kRequiredFields) {
      error = "record handling.cfg tidak lengkap pada baris " + std::to_string(lineNumber);
      return false;
    }
    Profile profile;
    if (!normalizeId(fields[0], profile.id) ||
        !parseFiniteFloat(fields[3], profile.dragMultiplier) ||
        !parseFiniteFloat(fields[8], profile.tractionMultiplier) ||
        !parseFiniteFloat(fields[9], profile.tractionLoss) ||
        !parseFiniteFloat(fields[10], profile.tractionBias) ||
        !parseUnsignedByte(fields[11], profile.numberOfGears) ||
        !parseFiniteFloat(fields[12], profile.maxVelocityKph) ||
        !parseFiniteFloat(fields[13], profile.engineAcceleration) ||
        !parseFiniteFloat(fields[14], profile.engineInertia) ||
        !parseFiniteFloat(fields[17], profile.brakeDeceleration) ||
        !parseFiniteFloat(fields[18], profile.brakeBias) ||
        !parseFiniteFloat(fields[20], profile.steeringLockDegrees) || !validProfile(profile)) {
      error = "nilai handling.cfg tidak valid pada baris " + std::to_string(lineNumber);
      return false;
    }
    if (candidate.size() >= maxProfiles || !candidate.emplace(profile.id, std::move(profile)).second) {
      error = candidate.size() >= maxProfiles ? "jumlah profile handling.cfg melebihi batas aman"
                                               : "ID handling.cfg duplikat";
      return false;
    }
  }
  if (candidate.empty()) {
    error = "handling.cfg tidak memiliki profile yang didukung";
    return false;
  }
  out = std::move(candidate);
  return true;
}

bool makeMotionTuning(const Profile& profile, vehiclemotion::VehicleMotionTuning& out) {
  if (!validProfile(profile)) return false;

  // GTA SA's documented `fMaxVelocity` field is a km/h top-speed limit; convert
  // it only to this client's world-units/second envelope. The real game also
  // applies flags, drag, mass, inertia, gears, and wheel physics that this
  // pilot does not reproduce. Acceleration and brake fields therefore remain
  // conservative source-scale response coefficients behind the tuning clamp,
  // preserving model-to-model differences without claiming full drivetrain
  // fidelity.
  vehiclemotion::VehicleMotionTuning candidate;
  candidate.maxForwardSpeed = std::clamp(profile.maxVelocityKph / 3.6f, 3.0f, 100.0f);
  candidate.maxReverseSpeed = std::clamp(candidate.maxForwardSpeed * 0.31f, 1.0f, 35.0f);
  candidate.forwardAcceleration = std::clamp(profile.engineAcceleration, 0.5f, 45.0f);
  candidate.reverseAcceleration = std::clamp(profile.engineAcceleration * 0.55f, 0.25f, 30.0f);
  candidate.serviceBrakeDeceleration = std::clamp(profile.brakeDeceleration, 1.0f, 45.0f);
  candidate.handBrakeDeceleration = std::clamp(profile.brakeDeceleration * 1.55f, 1.0f, 70.0f);
  candidate.coastDeceleration = std::clamp(profile.dragMultiplier * 1.4f, 0.1f, 15.0f);
  // Lower traction loss produces a stronger gradual lateral settle, while the
  // fallback still rejects a full tyre/suspension simulation claim.
  candidate.lateralCoastDeceleration =
      std::clamp(4.0f + profile.tractionMultiplier * 4.0f +
                     (1.0f - std::clamp(profile.tractionLoss, 0.0f, 1.0f)) * 2.0f,
                 0.5f, 35.0f);
  candidate.maxSteeringDegreesPerSecond =
      std::clamp(profile.steeringLockDegrees * 3.0f, 20.0f, 210.0f);
  if (!vehiclemotion::validMotionTuning(candidate)) return false;
  out = candidate;
  return true;
}

}  // namespace samp::gta::handling
