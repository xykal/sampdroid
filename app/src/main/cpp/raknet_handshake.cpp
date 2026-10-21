#include "raknet_handshake.h"

#include "raknet_codec.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <random>

namespace samp::handshake {
namespace {

constexpr uint8_t kIdRpc = 20u;
constexpr uint8_t kIdAuthKey = 12u;
constexpr uint8_t kRpcClientJoin = 25u;
constexpr uint8_t kIdNewIncomingConnection = 30u;
constexpr uint8_t kIdInternalPing = 6u;
constexpr uint8_t kIdReceivedStaticData = 41u;
constexpr uint8_t kIdRsaPublicKeyMismatch = 28u;
constexpr uint8_t kIdConnectionAttemptFailed = 29u;
constexpr uint8_t kIdNoFreeIncomingConnections = 31u;
constexpr uint8_t kIdDisconnectionNotification = 32u;
constexpr uint8_t kIdConnectionLost = 33u;
constexpr uint8_t kIdConnectionBanned = 36u;
constexpr uint8_t kIdInvalidPassword = 37u;
constexpr uint32_t kLegacySerialDivisor = 1001u;

char hexDigit(unsigned value) {
  return "0123456789ABCDEF"[value & 0xFu];
}

int hexValue(unsigned char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

bool isAsciiWhitespace(unsigned char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

unsigned char asciiLower(unsigned char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

}  // namespace

bool isExactDlR1AdvertisedVersion(const std::string& advertisedVersion) {
  constexpr char kExpected[] = "0.3.DL-R1";
  size_t begin = 0u;
  size_t end = advertisedVersion.size();
  while (begin < end && isAsciiWhitespace(static_cast<unsigned char>(advertisedVersion[begin]))) {
    ++begin;
  }
  while (end > begin && isAsciiWhitespace(static_cast<unsigned char>(advertisedVersion[end - 1u]))) {
    --end;
  }
  constexpr size_t kExpectedLength = sizeof(kExpected) - 1u;
  if (end - begin != kExpectedLength) return false;
  for (size_t i = 0u; i < kExpectedLength; ++i) {
    if (asciiLower(static_cast<unsigned char>(advertisedVersion[begin + i])) !=
        asciiLower(static_cast<unsigned char>(kExpected[i]))) {
      return false;
    }
  }
  return true;
}

bool isDlOnlyAllowedClientsRule(const std::string& advertisedAllowedClients) {
  bool hasDl = false;
  bool hasLegacy037 = false;
  size_t tokenBegin = 0u;
  while (tokenBegin <= advertisedAllowedClients.size()) {
    const size_t comma = advertisedAllowedClients.find(',', tokenBegin);
    size_t begin = tokenBegin;
    size_t end = comma == std::string::npos ? advertisedAllowedClients.size() : comma;
    while (begin < end &&
           isAsciiWhitespace(static_cast<unsigned char>(advertisedAllowedClients[begin]))) {
      ++begin;
    }
    while (end > begin &&
           isAsciiWhitespace(static_cast<unsigned char>(advertisedAllowedClients[end - 1u]))) {
      --end;
    }
    if (begin == end) return false;

    const auto equalsToken = [&](const char* expected) {
      size_t expectedLength = 0u;
      while (expected[expectedLength] != '\0') ++expectedLength;
      if (end - begin != expectedLength) return false;
      for (size_t index = 0u; index < expectedLength; ++index) {
        if (asciiLower(static_cast<unsigned char>(advertisedAllowedClients[begin + index])) !=
            asciiLower(static_cast<unsigned char>(expected[index]))) {
          return false;
        }
      }
      return true;
    };
    if (equalsToken("0.3.DL") || equalsToken("0.3.DL-R1")) {
      hasDl = true;
    } else if (equalsToken("0.3.7")) {
      hasLegacy037 = true;
    } else {
      // Do not treat arbitrary third-party rule text as a profile selector.
      return false;
    }
    if (comma == std::string::npos) break;
    tokenBegin = comma + 1u;
  }
  return hasDl && !hasLegacy037;
}

ClientJoinProfileSource clientJoinProfileSourceForAdvertisedRules(
    const std::string& advertisedVersion, const std::string& advertisedAllowedClients) {
  if (isExactDlR1AdvertisedVersion(advertisedVersion)) {
    return ClientJoinProfileSource::ExactDlR1Version;
  }
  if (isDlOnlyAllowedClientsRule(advertisedAllowedClients)) {
    return ClientJoinProfileSource::DlOnlyAllowedClients;
  }
  return ClientJoinProfileSource::LegacyBaseline;
}

const char* clientJoinProfileSourceLabel(ClientJoinProfileSource source) {
  switch (source) {
    case ClientJoinProfileSource::ExactDlR1Version:
      return "version";
    case ClientJoinProfileSource::DlOnlyAllowedClients:
      return "allowed_clients";
    case ClientJoinProfileSource::LegacyBaseline:
      return "baseline";
  }
  return "baseline";
}

ClientJoinProfile clientJoinProfileForAdvertisedRules(
    const std::string& advertisedVersion, const std::string& advertisedAllowedClients) {
  const ClientJoinProfileSource source =
      clientJoinProfileSourceForAdvertisedRules(advertisedVersion, advertisedAllowedClients);
  return source == ClientJoinProfileSource::LegacyBaseline ? kLegacyClientJoinProfile
                                                            : kDlR1ClientJoinProfile;
}

ClientJoinProfileSource clientJoinProfileSourceForAdvertisedVersion(
    const std::string& advertisedVersion) {
  return clientJoinProfileSourceForAdvertisedRules(advertisedVersion, "");
}

ClientJoinProfile clientJoinProfileForAdvertisedVersion(
    const std::string& advertisedVersion) {
  return clientJoinProfileForAdvertisedRules(advertisedVersion, "");
}

bool isValidNickname(const std::string& nickname) {
  if (nickname.size() < kMinNicknameBytes || nickname.size() > kMaxNicknameBytes) return false;
  for (const unsigned char value : nickname) {
    // Legacy servers put this directly into a one-byte-length text field. Keep
    // it printable ASCII and eliminate protocol/control ambiguity at source.
    if (value < 0x21u || value > 0x7Eu) return false;
  }
  return true;
}

bool isValidLegacySerial(const std::string& serial) {
  if (serial.empty() || serial.size() >= 50u) return false;
  uint32_t remainder = 0u;
  for (const unsigned char value : serial) {
    const int digit = hexValue(value);
    if (digit < 0) return false;
    remainder = (remainder * 16u + static_cast<uint32_t>(digit)) % kLegacySerialDivisor;
  }
  return remainder == 0u;
}

std::string generateLegacySerial() {
  // Generate a nonzero 37-digit base-16 seed, then multiply it by 1001 via
  // elementary big-integer arithmetic. This is safe on both Android ABIs and
  // yields a valid GPCI-style serial without reusing a bundled identity.
  constexpr size_t kSeedDigits = 37u;
  std::array<unsigned, kSeedDigits> digits{};
  std::random_device device;
  std::mt19937_64 random((static_cast<uint64_t>(device()) << 32u) ^ device() ^
                         static_cast<uint64_t>(std::chrono::steady_clock::now()
                             .time_since_epoch().count()));
  for (unsigned& digit : digits) digit = static_cast<unsigned>(random() & 0xFu);
  if (digits.front() == 0u) digits.front() = 1u;

  std::string reversed;
  reversed.reserve(kSeedDigits + 4u);
  unsigned carry = 0u;
  for (size_t offset = kSeedDigits; offset-- > 0u;) {
    const unsigned value = digits[offset] * kLegacySerialDivisor + carry;
    reversed.push_back(hexDigit(value));
    carry = value >> 4u;
  }
  while (carry != 0u) {
    reversed.push_back(hexDigit(carry));
    carry >>= 4u;
  }
  return std::string(reversed.rbegin(), reversed.rend());
}

std::optional<std::string> parseAuthChallenge(const std::vector<uint8_t>& payload) {
  if (payload.size() < 3u || payload[0] != kIdAuthKey) return std::nullopt;
  const size_t wireLength = payload[1];
  // The server writer includes the terminal NUL. Requiring an exact record
  // eliminates both OOB reads and malformed trailing-byte ambiguity before
  // source-auth-table lookup.
  if (wireLength < 2u || wireLength > kMaxAuthChallengeBytes + 1u ||
      payload.size() != wireLength + 2u || payload.back() != 0u ||
      std::find(payload.begin() + 2, payload.end() - 1, 0u) != payload.end() - 1) {
    return std::nullopt;
  }
  return std::string(reinterpret_cast<const char*>(payload.data() + 2u), wireLength - 1u);
}

bool isTerminalSessionControlId(uint8_t id) {
  return id == kIdRsaPublicKeyMismatch || id == kIdConnectionAttemptFailed ||
         id == kIdNoFreeIncomingConnections || id == kIdDisconnectionNotification ||
         id == kIdConnectionLost || id == kIdConnectionBanned ||
         id == kIdInvalidPassword;
}

bool isPostAcceptControlId(uint8_t id) {
  return id == kIdInternalPing || id == kIdReceivedStaticData;
}

uint32_t clientJoinChallengeResponse(uint32_t acceptedToken, const ClientJoinProfile& profile) {
  return acceptedToken ^ profile.netGameVersion;
}

std::vector<uint8_t> buildClientJoin(uint32_t challengeResponse, const std::string& nickname,
                                     const std::string& serial, const ClientJoinProfile& profile) {
  if (!isValidNickname(nickname) || !isValidLegacySerial(serial) || profile.clientVersion == nullptr) {
    return {};
  }
  const size_t versionLength = std::strlen(profile.clientVersion);
  // GetPlayerVersion accepts at most 24 bytes, and ClientJoin carries its
  // length in one byte. Both stock profile literals are well inside this bound.
  if (versionLength == 0u || versionLength > 24u) return {};

  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kRpcClientJoin, 8);
  writer.writeBits(profile.netGameVersion, 32);
  writer.writeBits(1u, 8);  // legacy modded-client capability byte
  writer.writeBits(static_cast<uint32_t>(nickname.size()), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(nickname.data()), nickname.size());
  writer.writeBits(challengeResponse, 32);
  writer.writeBits(static_cast<uint32_t>(serial.size()), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(serial.data()), serial.size());
  writer.writeBits(static_cast<uint32_t>(versionLength), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(profile.clientVersion), versionLength);
  return writer.buffer;
}

std::vector<uint8_t> buildNewIncoming(const std::array<uint8_t, 4>& address, uint16_t port) {
  return {
      kIdNewIncomingConnection,
      address[0], address[1], address[2], address[3],
      static_cast<uint8_t>(port & 0xFFu),
      static_cast<uint8_t>((port >> 8u) & 0xFFu),
  };
}

std::vector<uint8_t> buildInternalPing(uint32_t timestamp) {
  return {
      kIdInternalPing,
      static_cast<uint8_t>(timestamp & 0xFFu),
      static_cast<uint8_t>((timestamp >> 8u) & 0xFFu),
      static_cast<uint8_t>((timestamp >> 16u) & 0xFFu),
      static_cast<uint8_t>((timestamp >> 24u) & 0xFFu),
  };
}

std::vector<uint8_t> buildEmptyStaticData() {
  return {kIdReceivedStaticData};
}

}  // namespace samp::handshake
