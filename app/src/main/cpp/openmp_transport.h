// Minimal, source-audited open.mp transport negotiation helpers.
//
// These helpers are deliberately separate from ClientJoin identity. They do not
// claim an official client marker or make a player game-admitted; they only
// recognize the optional open.mp cookie-side access record and decode the
// server-to-client XOR envelope when that exact record supplied a session key.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace samp::openmp {

// In the legacy RakNet ID table used by SA-MP/open.mp, ID_USER_PACKET_ENUM is
// 94. The open.mp server sends this one raw pre-RakNet access record only after
// an OMP-cookie connection request; it is not a normal application RPC.
constexpr uint8_t kLegacyUserPacketEnum = 94u;
// Exact source integer. SAMPRakNet writes it through a native uint32_t pointer;
// on deployed little-endian servers the raw bytes are 70 6D 6F 00.
constexpr uint32_t kAccessMagic = 0x006F6D70u;
constexpr size_t kAccessRequestBytes = 13u;

struct AccessRequest {
  // Server-generated per-peer XOR key. It applies only to subsequent
  // server-to-client datagrams when that server enables its optional open.mp
  // transport envelope; client-to-server packets keep their legacy SA-MP
  // wrapper and are not transformed here.
  uint32_t encryptionKey = 0u;
  // Diagnostic-only version written by the server's access record. SampDroid
  // does not echo it as an official-client claim.
  uint32_t serverClientModVersion = 0u;
};

// Parses exactly the raw 13-byte access record emitted by current open.mp
// SAMPRakNet after an OMP cookie. Any other UDP datagram is rejected unchanged.
std::optional<AccessRequest> parseAccessRequest(const std::vector<uint8_t>& datagram);

// Decodes one server-to-client optional open.mp XOR envelope. Returns false for
// a short frame or a checksum mismatch and leaves `plain` empty. The checksum
// is the legacy XOR of every decoded byte masked with 0xAA.
bool decryptServerDatagram(const std::vector<uint8_t>& wire, uint32_t encryptionKey,
                           std::vector<uint8_t>& plain);

// Current open.mp legacy-network source publishes its direct query `version`
// rule as `omp <major>.<minor>.<patch>.<build>`. Match only that whole bounded,
// whitespace-tolerant numeric form so arbitrary query banners cannot opt a
// session into the OMP-cookie path.
bool isOpenMpAdvertisement(const std::string& advertisedVersion);

}  // namespace samp::openmp
