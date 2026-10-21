#include "openmp_transport.h"

#include <cstddef>

namespace samp::openmp {
namespace {

uint32_t readLe32(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) |
         (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
         (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
         (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

bool isAsciiWhitespace(uint8_t value) {
  return value == static_cast<uint8_t>(' ') || value == static_cast<uint8_t>('\t') ||
         value == static_cast<uint8_t>('\r') || value == static_cast<uint8_t>('\n');
}

uint8_t asciiLower(uint8_t value) {
  return value >= static_cast<uint8_t>('A') && value <= static_cast<uint8_t>('Z')
      ? static_cast<uint8_t>(value + static_cast<uint8_t>('a' - 'A'))
      : value;
}

}  // namespace

std::optional<AccessRequest> parseAccessRequest(const std::vector<uint8_t>& datagram) {
  if (datagram.size() != kAccessRequestBytes || datagram[0] != kLegacyUserPacketEnum ||
      readLe32(datagram, 1u) != kAccessMagic) {
    return std::nullopt;
  }

  AccessRequest out;
  out.encryptionKey = readLe32(datagram, 5u);
  out.serverClientModVersion = readLe32(datagram, 9u);
  // A zero version cannot be emitted by the audited server enum and makes a
  // malformed/raw unrelated datagram less likely to become transport state.
  if (out.serverClientModVersion == 0u) return std::nullopt;
  return out;
}

bool decryptServerDatagram(const std::vector<uint8_t>& wire, uint32_t encryptionKey,
                           std::vector<uint8_t>& plain) {
  plain.clear();
  if (wire.size() < 2u) return false;

  const uint8_t keyBytes[4] = {
      static_cast<uint8_t>(encryptionKey & 0xFFu),
      static_cast<uint8_t>((encryptionKey >> 8u) & 0xFFu),
      static_cast<uint8_t>((encryptionKey >> 16u) & 0xFFu),
      static_cast<uint8_t>((encryptionKey >> 24u) & 0xFFu),
  };
  plain.resize(wire.size() - 1u);
  uint8_t checksum = 0u;
  for (size_t i = 0u; i < plain.size(); ++i) {
    const uint8_t value = static_cast<uint8_t>(wire[i + 1u] ^ keyBytes[i % 4u]);
    plain[i] = value;
    checksum ^= static_cast<uint8_t>(value & 0xAAu);
  }
  if (checksum != wire[0]) {
    plain.clear();
    return false;
  }
  return true;
}

bool isOpenMpAdvertisement(const std::string& advertisedVersion) {
  size_t begin = 0u;
  size_t end = advertisedVersion.size();
  while (begin < end && isAsciiWhitespace(static_cast<uint8_t>(advertisedVersion[begin]))) ++begin;
  while (end > begin && isAsciiWhitespace(static_cast<uint8_t>(advertisedVersion[end - 1u]))) --end;

  // Require `omp`, at least one ASCII separator, and at least one following
  // version byte. This recognizes the source-owned query rule without treating
  // a host/gamemode banner containing merely the word "omp" as a negotiation
  // instruction.
  if (end - begin < 5u || asciiLower(static_cast<uint8_t>(advertisedVersion[begin])) != 'o' ||
      asciiLower(static_cast<uint8_t>(advertisedVersion[begin + 1u])) != 'm' ||
      asciiLower(static_cast<uint8_t>(advertisedVersion[begin + 2u])) != 'p' ||
      !isAsciiWhitespace(static_cast<uint8_t>(advertisedVersion[begin + 3u]))) {
    return false;
  }
  size_t position = begin + 3u;
  while (position < end && isAsciiWhitespace(static_cast<uint8_t>(advertisedVersion[position]))) {
    ++position;
  }
  // LegacyNetwork writes four unsigned decimal components exactly:
  // major.minor.patch.build. Do not let an arbitrary "omp hello" MOTD/rule
  // switch the cookie branch just because it shares the prefix.
  unsigned components = 0u;
  bool sawDigit = false;
  for (; position < end; ++position) {
    const uint8_t value = static_cast<uint8_t>(advertisedVersion[position]);
    if (value >= static_cast<uint8_t>('0') && value <= static_cast<uint8_t>('9')) {
      sawDigit = true;
      continue;
    }
    if (value == static_cast<uint8_t>('.') && sawDigit && components < 3u) {
      ++components;
      sawDigit = false;
      continue;
    }
    return false;
  }
  return sawDigit && components == 3u;
}

}  // namespace samp::openmp
