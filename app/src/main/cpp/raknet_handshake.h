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
// SA-MP 0.3.DL-R1 keeps the legacy ClientJoin layout but identifies its
// netgame protocol with 4062 and reports a distinct bounded version string.
constexpr uint32_t kDlR1NetGameVersion = 4062u;

// ClientJoin has two intentionally small, source-backed identity profiles.
// This is not a general version-spoofing surface: callers can select only the
// stock 0.3.7-R2 baseline or 0.3.DL-R1 after an exact server-query match.
struct ClientJoinProfile {
  uint32_t netGameVersion;
  const char* clientVersion;
  const char* displayName;
};

inline constexpr ClientJoinProfile kLegacyClientJoinProfile{
    kLegacyNetGameVersion, "0.3.7-R2", "0.3.7-R2 (4057)"};
inline constexpr ClientJoinProfile kDlR1ClientJoinProfile{
    kDlR1NetGameVersion, "0.3.DL-R1", "0.3.DL-R1 (4062)"};

// Why one of the two immutable profiles was selected. This is diagnostic
// metadata only; it never changes the wire fields beyond the fixed profile.
enum class ClientJoinProfileSource : uint8_t {
  LegacyBaseline,
  ExactDlR1Version,
  DlOnlyAllowedClients,
};

// Treat only a whitespace-padded, case-insensitive exact 0.3.DL-R1 query
// value as DL. Unknown, blank, open.mp, or mixed-version advertisements remain
// on the well-covered legacy profile rather than speculatively changing wire
// identity.
bool isExactDlR1AdvertisedVersion(const std::string& advertisedVersion);

// Current open.mp LegacyNetwork publishes `allowed_clients` as either
// `0.3.DL` or `0.3.7, 0.3.DL`. A fresh, parseable rule containing only the
// known DL token is enough to choose the existing 4062/DL-R1 wire profile;
// any legacy token, unknown token, or malformed list deliberately stays false.
bool isDlOnlyAllowedClientsRule(const std::string& advertisedAllowedClients);

// Combines the exact version identity above with that source-owned fresh rule.
// The rule never creates a new identity: it can only choose the existing fixed
// DL-R1 profile when legacy 0.3.7 is explicitly unavailable.
ClientJoinProfileSource clientJoinProfileSourceForAdvertisedRules(
    const std::string& advertisedVersion, const std::string& advertisedAllowedClients);

// Stable machine-readable diagnostic labels: "baseline", "version", or
// "allowed_clients". They are deliberately not version strings.
const char* clientJoinProfileSourceLabel(ClientJoinProfileSource source);

ClientJoinProfile clientJoinProfileForAdvertisedRules(
    const std::string& advertisedVersion, const std::string& advertisedAllowedClients);

// Compatibility wrappers for callers that have no fresh allowed_clients rule.
ClientJoinProfileSource clientJoinProfileSourceForAdvertisedVersion(
    const std::string& advertisedVersion);
ClientJoinProfile clientJoinProfileForAdvertisedVersion(
    const std::string& advertisedVersion);

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

// Legacy RakPeer emits these transport controls after processing NEW_INCOMING.
// They are post-accept metadata handled by the receive path, not a prerequisite
// between the source peer's own NEW_INCOMING/ping/static sequence and ClientJoin.
bool isPostAcceptControlId(uint8_t id);

// Produces the raw legacy RPC ClientJoin payload [ID_RPC, RPC=25, fields].
// The outer RakNet reliable-ordered frame is intentionally owned by the
// session transport, where its message/order counters live.
// Computes the ClientJoin challenge response using the profile's exact
// netgame version. Keeping this beside the profile prevents the version field
// and challenge XOR from drifting apart.
uint32_t clientJoinChallengeResponse(uint32_t acceptedToken, const ClientJoinProfile& profile);

std::vector<uint8_t> buildClientJoin(uint32_t challengeResponse, const std::string& nickname,
                                     const std::string& serial,
                                     const ClientJoinProfile& profile = kLegacyClientJoinProfile);

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
