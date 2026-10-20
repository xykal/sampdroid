// Bounded legacy SA-MP/open.mp handshake builders shared by the native
// transport and host regression tests. They contain no socket ownership.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace samp::handshake {

constexpr uint32_t kLegacyNetGameVersion = 4057u;
constexpr size_t kMinNicknameBytes = 3u;
constexpr size_t kMaxNicknameBytes = 24u;
// Legacy AUTH_KEY challenge strings are one-byte length-prefixed, include a
// terminal NUL on the server-to-client path, and are at most 16 ASCII bytes.
constexpr size_t kMaxAuthChallengeBytes = 16u;

bool isValidNickname(const std::string& nickname);
bool isValidLegacySerial(const std::string& serial);
std::string generateLegacySerial();

// Validates a server AUTH_KEY payload `[12][length including NUL][challenge\0]`
// before it reaches the lookup table. Malformed/oversized records are ignored.
std::optional<std::string> parseAuthChallenge(const std::vector<uint8_t>& payload);

// Immediate peer-terminal controls can arrive before, during, or after
// AUTH_KEY/ClientJoin. Keeping their legacy IDs in one pure helper prevents
// every transport phase from turning an explicit refusal into a timeout.
bool isTerminalSessionControlId(uint8_t id);

// Produces the raw legacy RPC ClientJoin payload [ID_RPC, RPC=25, fields].
// The outer RakNet reliable-ordered frame is intentionally owned by the
// session transport, where its message/order counters live.
std::vector<uint8_t> buildClientJoin(uint32_t challengeResponse, const std::string& nickname,
                                     const std::string& serial);

// SA-MP's RakNet 2.52 connection-complete payload, exactly
// [ID_NEW_INCOMING_CONNECTION, IPv4 bytes, port little-endian].
std::vector<uint8_t> buildNewIncoming(const std::array<uint8_t, 4>& address, uint16_t port);

// RakPeer sends these two source-defined post-accept control payloads after
// NEW_INCOMING: an ID_INTERNAL_PING carrying a little-endian RakNetTime and
// an empty ID_RECEIVED_STATIC_DATA record. Their outer reliability frames and
// message counters remain owned by the session transport.
std::vector<uint8_t> buildInternalPing(uint32_t timestamp);
std::vector<uint8_t> buildEmptyStaticData();

}  // namespace samp::handshake
