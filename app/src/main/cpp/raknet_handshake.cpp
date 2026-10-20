#include "raknet_handshake.h"

#include "raknet_codec.h"

#include <algorithm>
#include <array>
#include <chrono>
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

}  // namespace

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

std::vector<uint8_t> buildClientJoin(uint32_t challengeResponse, const std::string& nickname,
                                     const std::string& serial) {
  static constexpr char kVersion[] = "0.3.7-R2";
  if (!isValidNickname(nickname) || !isValidLegacySerial(serial)) return {};

  raknet::BitWriter writer;
  writer.writeBits(kIdRpc, 8);
  writer.writeBits(kRpcClientJoin, 8);
  writer.writeBits(kLegacyNetGameVersion, 32);
  writer.writeBits(1u, 8);  // legacy modded-client capability byte
  writer.writeBits(static_cast<uint32_t>(nickname.size()), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(nickname.data()), nickname.size());
  writer.writeBits(challengeResponse, 32);
  writer.writeBits(static_cast<uint32_t>(serial.size()), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(serial.data()), serial.size());
  constexpr size_t kVersionLength = sizeof(kVersion) - 1u;
  writer.writeBits(static_cast<uint32_t>(kVersionLength), 8);
  writer.writeBytes(reinterpret_cast<const uint8_t*>(kVersion), kVersionLength);
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
