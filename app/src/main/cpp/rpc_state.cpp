// Renderer-independent state reducer for player, class/spawn, chat, and marker RPCs.
#include "rpc_state.h"

#include "raknet_codec.h"
#include "raknet_string_compressor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace samp::state {
namespace {

constexpr uint8_t kIdRpc = 20;
constexpr uint8_t kSetPlayerName = 11;
constexpr uint8_t kPlayerBeginObjectSelect = 27;
// Current open.mp SetVehicleParams writer: u16 VehicleID followed by sixteen
// signed int8 vehicle parameter values (serialized as raw bytes).
constexpr uint8_t kSetVehicleParams = 24;
constexpr uint8_t kSetPlayerTime = 29;
constexpr uint8_t kTogglePlayerClock = 30;
constexpr uint8_t kPlayerBeginAttachedObjectEdit = 116;
constexpr uint8_t kPlayerBeginObjectEdit = 117;
constexpr uint8_t kPlayerCancelObjectEdit = 28;
constexpr uint8_t kSetVehiclePosition = 159;
constexpr uint8_t kSetVehicleZAngle = 160;
// Legacy per-recipient objective/door-lock RPC, separate from the sixteen
// signed-field SetVehicleParamsEx-compatible RPC 24.
constexpr uint8_t kSetVehicleParamsForPlayer = 161;
constexpr uint8_t kSetPlayerPosition = 12;
constexpr uint8_t kSetPlayerPositionFindZ = 13;
constexpr uint8_t kSetPlayerHealth = 14;
constexpr uint8_t kSetWorldBounds = 17;
constexpr uint8_t kGivePlayerMoney = 18;
constexpr uint8_t kResetPlayerMoney = 20;
constexpr uint8_t kTogglePlayerControllable = 15;
constexpr uint8_t kSetPlayerFacingAngle = 19;
constexpr uint8_t kResetPlayerWeapons = 21;
constexpr uint8_t kGivePlayerWeapon = 22;
constexpr uint8_t kPlayerStreamIn = 32;
constexpr uint8_t kPlayerShowTextLabel = 36;
constexpr uint8_t kPlayAudioStream = 41;
constexpr uint8_t kStopAudioStream = 42;
// RemoveBuildingForPlayer: uint32 model (-1 is wildcard) + four IEEE floats.
constexpr uint8_t kRemoveBuilding = 43;
constexpr uint8_t kCreateObject = 44;
// Core non-DL/open.mp writer: signed 32-bit world ID. It is distinct from
// GTA interior IDs and is retained only as server state/presentation.
constexpr uint8_t kSetPlayerVirtualWorld = 48;
constexpr uint8_t kSendDeathMessage = 55;
constexpr uint8_t kSetPlayerCameraPosition = 157;
constexpr uint8_t kSetPlayerCameraLookAt = 158;
constexpr uint8_t kSetPlayerCameraBehindPlayer = 162;
constexpr uint8_t kInterpolateCamera = 82;
constexpr uint8_t kShowPlayerNameTagForPlayer = 80;
constexpr uint8_t kStopFlashGangZone = 85;
constexpr uint8_t kSetObjectPosition = 45;
constexpr uint8_t kSetObjectRotation = 46;
constexpr uint8_t kDestroyObject = 47;
constexpr uint8_t kDisableCheckpoint = 37;
constexpr uint8_t kSetRaceCheckpoint = 38;
constexpr uint8_t kDisableRaceCheckpoint = 39;
// Current open.mp core.hpp calls RPC 40 PlayerClose. It has no body and means
// the server has requested that this recipient terminate its client session.
constexpr uint8_t kPlayerClose = 40;
// Legacy ConnectionRejected has exactly one byte: 1=version, 2=nickname,
// 3=mod version, 4=server cannot allocate a player slot.
constexpr uint8_t kConnectionRejected = 130;
constexpr uint8_t kPlayerHideTextLabel = 58;
constexpr uint8_t kSetPlayerMapIcon = 56;
constexpr uint8_t kSetPlayerChatBubble = 59;
constexpr uint8_t kShowDialog = 61;
constexpr uint8_t kDestroyPickup = 63;
constexpr uint8_t kLinkVehicleToInterior = 65;
constexpr uint8_t kSetPlayerArmour = 66;
constexpr uint8_t kSetPlayerArmedWeapon = 67;
constexpr uint8_t kSetSpawnInfo = 68;
constexpr uint8_t kPutPlayerInVehicle = 70;
constexpr uint8_t kRemovePlayerFromVehicle = 71;
constexpr uint8_t kSetPlayerColor = 72;
constexpr uint8_t kSendGameText = 73;
constexpr uint8_t kForcePlayerClassSelection = 74;
constexpr uint8_t kAttachObjectToPlayer = 75;
constexpr uint8_t kPlayerInitMenu = 76;
constexpr uint8_t kPlayerShowMenu = 77;
constexpr uint8_t kPlayerHideMenu = 78;
constexpr uint8_t kTextDrawSelection = 83;
constexpr uint8_t kSetObjectMaterial = 84;
constexpr uint8_t kSetPlayerSpecialAction = 88;
constexpr uint8_t kApplyPlayerAnimation = 86;
constexpr uint8_t kClearPlayerTasks = 87;
constexpr uint8_t kSetPlayerVelocity = 90;
// Recipient-local SetVehicleVelocity has no VehicleID. Type 0 is linear and
// Type 1 is angular in open.mp's VehicleVelocitySetType enum.
constexpr uint8_t kSetVehicleVelocity = 91;
constexpr uint8_t kSendClientMessage = 93;
constexpr uint8_t kSetWorldTime = 94;
constexpr uint8_t kCreatePickup = 95;
constexpr uint8_t kMoveObject = 99;
constexpr uint8_t kPlayerChatMessage = 101;
constexpr uint8_t kTextDrawSetString = 105;
constexpr uint8_t kSetCheckpoint = 107;
constexpr uint8_t kShowGangZone = 108;
constexpr uint8_t kSetPlayerAttachedObject = 113;
constexpr uint8_t kHideGangZone = 120;
constexpr uint8_t kFlashGangZone = 121;
constexpr uint8_t kStopObject = 122;
constexpr uint8_t kTogglePlayerSpectating = 124;
constexpr uint8_t kPlayerSpectatePlayer = 126;
constexpr uint8_t kPlayerSpectateVehicle = 127;
constexpr uint8_t kSetPlayerWantedLevel = 133;
constexpr uint8_t kShowTextDraw = 134;
constexpr uint8_t kHideTextDraw = 135;
constexpr uint8_t kPlayerJoin = 137;
constexpr uint8_t kPlayerQuit = 138;
constexpr uint8_t kRemovePlayerMapIcon = 144;
constexpr uint8_t kSetPlayerAmmo = 145;
constexpr uint8_t kPlayerRequestClass = 128;
constexpr uint8_t kPlayerRequestSpawn = 129;
constexpr uint8_t kPlayerInit = 139;
constexpr uint8_t kSetPlayerGravity = 146;
constexpr uint8_t kSetVehicleHealth = 147;
constexpr uint8_t kSetWeather = 152;
constexpr uint8_t kSetPlayerSkin = 153;
// Legacy SA-MP/open.mp ScrSetPlayerInterior carries one unsigned-byte interior
// ID. It is distinct from the client-to-server InteriorChangeNotification.
constexpr uint8_t kSetPlayerInterior = 156;
constexpr uint8_t kSendPlayerScoresAndPings = 155;
constexpr uint8_t kPlayerStreamOut = 163;
constexpr uint8_t kStreamInVehicle = 164;
constexpr uint8_t kStreamOutVehicle = 165;
// Server PlayerDeath is observer-oriented and carries only a remote PlayerID.
constexpr uint8_t kPlayerDeath = 166;
constexpr uint8_t kShowActor = 171;
constexpr uint8_t kHideActor = 172;
constexpr uint8_t kApplyActorAnimation = 173;
constexpr uint8_t kClearActorAnimation = 174;
constexpr uint8_t kSetActorFacingAngle = 175;
constexpr uint8_t kSetActorPosition = 176;
constexpr uint8_t kSetActorHealth = 178;
// 0.3.DL ModelRequest/ModelInfo. It is accepted only in the DL R1 selected
// wire profile and is used solely for an in-memory stock-model fallback map.
constexpr uint8_t kDlModelInfo = 179;
constexpr size_t kMaxDlObjectFallbacks = 4096u;
constexpr int32_t kMinDlCustomSkinId = 20001;
constexpr int32_t kMaxDlCustomSkinId = 30000;
constexpr int32_t kMinDlCustomObjectId = -30000;
constexpr int32_t kMaxDlCustomObjectId = -1000;
constexpr size_t kMaxPlayerNameBytes = 24;
constexpr size_t kMaxChatBubbleBytes = 128;
// ApplyPlayerAnimation/ApplyActorAnimation use DynStr8 fields. Current
// open.mp AnimationData stores lib in HybridString<16> and name in
// HybridString<24>, so the sender cannot emit more than 15/23 data bytes.
constexpr size_t kMaxAnimationLibraryBytes = 15;
constexpr size_t kMaxAnimationNameBytes = 23;
// Current open.mp RPC 41 stores its DynStr8 endpoint in HybridString<128>.
// Like the nearby HybridString<32> material fields, one terminal byte is
// storage, so retain at most 127 UTF-8/ASCII wire bytes. Native keeps it
// opaque; Android independently permits only a narrow HTTPS endpoint before
// it ever opens MediaPlayer.
constexpr size_t kMaxAudioStreamUrlBytes = 127;
constexpr float kMaxAudioStreamDistance = 50'000.0f;
constexpr size_t kMaxScoreboardEntries = 1000;
constexpr size_t kMaxNameTagVisibilityOverrides = 1000;
constexpr uint32_t kMaxChatBubbleLifetimeMs = 3'600'000u;
constexpr float kMaxChatBubbleDrawDistance = 50'000.0f;
constexpr size_t kMaxDeathMessages = 16;
constexpr size_t kHardHudDeathMessageLimit = 8;
constexpr uint64_t kDeathMessagePresentationLifetimeMs = 8'000u;
// Packet-203 has no explicit stop event. Keep its renderer-only direction
// marker short-lived rather than leaving a stale aim pose after a remote player
// releases AIM or stops syncing it.
constexpr uint64_t kRemoteAimPresentationLifetimeMs = 1'200u;
// A remote BulletSync packet is already server-relayed, but its visual cue
// must disappear quickly and cannot become a persistent target/hit marker.
constexpr uint64_t kRemoteBulletPresentationLifetimeMs = 220u;
// RPC 166 has no paired respawn acknowledgement for observers. Keep an honest
// temporary wasted cue, then let the next regular server presentation state
// stand on its own instead of inventing a remote respawn transition.
constexpr uint64_t kRemoteWastedPresentationLifetimeMs = 8'000u;
constexpr uint32_t kMaxLegacyWeaponId = 46u;
// SA-MP reserves 2048 global and 256 player textdraw wire IDs. Keep the
// protocol boundary centralized so Show/Hide/SetString cannot disagree.
constexpr uint16_t kTextDrawGlobalPoolSize = 2048u;
constexpr uint16_t kTextDrawPlayerPoolSize = 256u;
constexpr uint16_t kTextDrawWireIdLimit =
    static_cast<uint16_t>(kTextDrawGlobalPoolSize + kTextDrawPlayerPoolSize);
constexpr size_t kMaxTrackedTextDraws =
    static_cast<size_t>(kTextDrawGlobalPoolSize) + kTextDrawPlayerPoolSize;
// TEXT_LABEL_POOL_SIZE is 1024 in the public legacy SDK. Player labels are
// emitted with exactly that offset, forming a bounded 0..2047 wire namespace.
constexpr uint16_t kTextLabelPoolSize = 1024u;
constexpr uint16_t kTextLabelWireIdLimit = static_cast<uint16_t>(kTextLabelPoolSize * 2u);
constexpr size_t kMaxTrackedTextLabels = static_cast<size_t>(kTextLabelWireIdLimit);
// Legacy SA-MP has exactly one hundred SetPlayerMapIcon slots per recipient.
constexpr uint8_t kMapIconSlotLimit = 100u;
constexpr size_t kMaxTrackedMapIcons = static_cast<size_t>(kMapIconSlotLimit);
// open.mp SDK values.hpp retains the legacy per-player gang-zone namespace at
// 1024. Bound reducer state to that interoperable ID range regardless of the
// signed 16-bit wire container.
constexpr uint16_t kGangZonePoolSize = 1024u;
// Current open.mp SDK values.hpp retains legacy menu limits: 128 IDs, twelve
// rows, and `StaticString<32>` fixed wire fields including terminal NUL.
constexpr size_t kMaxTrackedMenus = 128u;
constexpr size_t kMenuFixedTextBytes = 32u;
constexpr uint32_t kMaxCameraInterpolationMs = 3'600'000u;

const char* connectionRejectionSummary(uint8_t reason) {
  switch (reason) {
    case 1u: return "ConnectionRejected: versi client tidak cocok";
    case 2u: return "ConnectionRejected: nickname ditolak server";
    case 3u: return "ConnectionRejected: versi/mod client ditolak server";
    case 4u: return "ConnectionRejected: slot pemain server tidak tersedia";
    default: return nullptr;
  }
}

std::string readString8(raknet::BitReader& reader, size_t maxLength) {
  const size_t length = static_cast<uint8_t>(reader.readBits(8));
  if (!reader.ok() || length > maxLength) return {};
  const auto bytes = reader.readBytes(length);
  if (!reader.ok() || bytes.empty()) return {};
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string readString32(raknet::BitReader& reader, size_t maxLength) {
  const uint32_t length = reader.readBits(32);
  if (!reader.ok() || length == 0 || length > maxLength) return {};
  const auto bytes = reader.readBytes(static_cast<size_t>(length));
  if (!reader.ok() || bytes.empty()) return {};
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

bool readString8AllowEmpty(raknet::BitReader& reader, size_t maxLength, std::string& out) {
  out.clear();
  const size_t length = static_cast<uint8_t>(reader.readBits(8));
  if (!reader.ok() || length > maxLength) return false;
  if (length == 0) return true;
  const auto bytes = reader.readBytes(length);
  if (!reader.ok() || bytes.size() != length) return false;
  out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool readString16AllowEmpty(raknet::BitReader& reader, size_t maxLength, std::string& out) {
  out.clear();
  const size_t length = static_cast<uint16_t>(reader.readBits(16));
  if (!reader.ok() || length > maxLength) return false;
  if (length == 0) return true;
  const auto bytes = reader.readBytes(length);
  if (!reader.ok() || bytes.size() != length) return false;
  out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool readString32AllowEmpty(raknet::BitReader& reader, size_t maxLength, std::string& out) {
  out.clear();
  const size_t length = reader.readBits(32);
  if (!reader.ok() || length > maxLength) return false;
  if (length == 0) return true;
  const auto bytes = reader.readBytes(length);
  if (!reader.ok() || bytes.size() != length) return false;
  out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

// PlayerInitMenu uses StaticString<32>::data(), not DynStr framing. Preserve
// bytes only through the first NUL; padding stays transport-only and no UI gets
// to see an arbitrary fixed-array tail.
bool readFixedMenuText(raknet::BitReader& reader, std::string& out) {
  out.clear();
  const auto bytes = reader.readBytes(kMenuFixedTextBytes);
  if (!reader.ok() || bytes.size() != kMenuFixedTextBytes) return false;
  size_t length = 0;
  while (length < bytes.size() && bytes[length] != 0u) ++length;
  out.assign(reinterpret_cast<const char*>(bytes.data()), length);
  return true;
}

// Keep dialog strings safe for Java's UTF-8 boundary while preserving valid
// UTF-8 sent by a server. Other legacy/codepage bytes become a visible '?'
// rather than being allowed to poison a JNI/JSON string.
std::string sanitizeUiText(const std::string& input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size();) {
    const uint8_t c = static_cast<uint8_t>(input[i]);
    if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7F)) {
      out.push_back(static_cast<char>(c));
      ++i;
      continue;
    }
    if (c < 0x80) {
      out.push_back(' ');
      ++i;
      continue;
    }
    size_t width = 0;
    if (c >= 0xC2 && c <= 0xDF) width = 2;
    if (c >= 0xE0 && c <= 0xEF) width = 3;
    if (c >= 0xF0 && c <= 0xF4) width = 4;
    bool valid = width != 0 && i + width <= input.size();
    for (size_t part = 1; valid && part < width; ++part) {
      valid = (static_cast<uint8_t>(input[i + part]) & 0xC0u) == 0x80u;
    }
    // Reject overlong, surrogate, and out-of-range standard UTF-8 sequences.
    if (valid && width == 3 && c == 0xE0 && static_cast<uint8_t>(input[i + 1]) < 0xA0) valid = false;
    if (valid && width == 3 && c == 0xED && static_cast<uint8_t>(input[i + 1]) >= 0xA0) valid = false;
    if (valid && width == 4 && c == 0xF0 && static_cast<uint8_t>(input[i + 1]) < 0x90) valid = false;
    if (valid && width == 4 && c == 0xF4 && static_cast<uint8_t>(input[i + 1]) >= 0x90) valid = false;
    if (!valid) {
      out.push_back('?');
      ++i;
      continue;
    }
    out.append(input, i, width);
    i += width;
  }
  return out;
}

// Object material names are lookup keys into an already-mounted user-owned
// TXD catalog, never paths. Match the portable TXD reader's narrow printable
// ASCII/no-separator surface and normalize case once at the RPC boundary.
bool normalizeObjectMaterialName(std::string& value) {
  constexpr size_t kMaxObjectMaterialNameBytes = 31u;
  if (value.empty() || value.size() > kMaxObjectMaterialNameBytes) return false;
  for (char& character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20u || byte > 0x7Eu || byte == '/' || byte == '\\' || byte == ':') {
      return false;
    }
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return true;
}

// open.mp's ObjectMaterialData writer emits type + slot before either its
// Default=1 texture record or Text=2 record (SDK enum None=0). Both variants
// are retained source-faithfully so a later renderer can replace exactly one
// flattened DFF material slot without desynchronizing following records.
bool readObjectMaterialRecord(raknet::BitReader& reader, uint8_t& slot,
                              ServerObjectMaterialOverride& out) {
  // Current SDK enum is None=0, Default=1, Text=2. The writer emits only
  // used Default/Text records; accepting None would silently misalign a body.
  constexpr uint8_t kObjectMaterialDefault = 1u;
  constexpr uint8_t kObjectMaterialText = 2u;
  // ObjectMaterialData uses HybridString<32>; text is a StringView over that
  // storage, so its writer cannot legitimately exceed 31 data bytes.
  constexpr size_t kMaxObjectMaterialTextBytes = 31u;
  out = {};
  const uint8_t type = static_cast<uint8_t>(reader.readBits(8));
  slot = static_cast<uint8_t>(reader.readBits(8));
  if (!reader.ok() || slot >= kMaxServerObjectMaterialSlots) return false;
  if (type == kObjectMaterialDefault) {
    out.sourceModel = static_cast<uint16_t>(reader.readBits(16));
    std::string dictionary;
    std::string texture;
    if (!readString8AllowEmpty(reader, 31u, dictionary) ||
        !readString8AllowEmpty(reader, 31u, texture)) {
      return false;
    }
    out.colourAbgr = reader.readBits(32);
    if (!reader.ok()) return false;
    // A source writer can represent an intentionally blank default material.
    // Preserve it as a no-override instead of attempting a fake dictionary
    // lookup; a half-empty pair is malformed and fails the complete RPC.
    if (dictionary.empty() && texture.empty()) return true;
    if (dictionary.empty() || texture.empty() || !normalizeObjectMaterialName(dictionary) ||
        !normalizeObjectMaterialName(texture)) {
      return false;
    }
    out.active = true;
    out.textureDictionary = std::move(dictionary);
    out.textureName = std::move(texture);
    return true;
  }
  if (type == kObjectMaterialText) {
    const uint8_t materialSize = static_cast<uint8_t>(reader.readBits(8));
    std::string font;
    if (!readString8AllowEmpty(reader, 31u, font)) return false;
    const uint8_t fontSize = static_cast<uint8_t>(reader.readBits(8));
    const uint8_t bold = static_cast<uint8_t>(reader.readBits(8));
    const uint32_t fontColourArgb = reader.readBits(32);
    const uint32_t backgroundColourArgb = reader.readBits(32);
    const uint8_t alignment = static_cast<uint8_t>(reader.readBits(8));
    std::string text;
    if (!reader.ok() || materialSize < 10u || materialSize > 140u ||
        materialSize % 10u != 0u || bold > 1u || alignment > 2u ||
        !raknet::stringcompressor::decodeDefaultString(reader, kMaxObjectMaterialTextBytes, text)) {
      return false;
    }
    out.textActive = true;
    out.textMaterialSize = materialSize;
    out.textFontSize = fontSize;
    out.textBold = bold != 0u;
    out.textColourArgb = fontColourArgb;
    out.textBackgroundColourArgb = backgroundColourArgb;
    out.textAlignment = alignment;
    out.textFont = std::move(font);
    out.text = std::move(text);
    return true;
  }
  return false;
}

void advanceDialogRevision(Dialog& dialog) {
  ++dialog.revision;
  if (dialog.revision == 0) dialog.revision = 1;
}

void advanceMenuRevision(Menu& menu) {
  ++menu.revision;
  if (menu.revision == 0) menu.revision = 1;
}

void advanceObjectSelectionRevision(uint64_t& revision) {
  ++revision;
  if (revision == 0) revision = 1;
}

void advanceObjectEditRevision(ObjectEditState& edit) {
  ++edit.revision;
  if (edit.revision == 0) edit.revision = 1;
}

void advanceAttachedObjectEditRevision(AttachedObjectEditState& edit) {
  ++edit.revision;
  if (edit.revision == 0) edit.revision = 1;
}

float readFloat(raknet::BitReader& reader, bool& finite) {
  const uint32_t raw = reader.readBits(32);
  float value = 0.0f;
  static_assert(sizeof(value) == sizeof(raw), "SA-MP uses 32-bit float values");
  std::memcpy(&value, &raw, sizeof(value));
  finite = finite && std::isfinite(value);
  return value;
}

int32_t readInt32(raknet::BitReader& reader) {
  const uint32_t raw = reader.readBits(32);
  int32_t value = 0;
  static_assert(sizeof(value) == sizeof(raw), "SA-MP uses 32-bit signed model fields");
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

bool validDlCustomSkinId(int32_t modelId) {
  return modelId >= kMinDlCustomSkinId && modelId <= kMaxDlCustomSkinId;
}

bool validDlCustomObjectId(int32_t modelId) {
  return modelId >= kMinDlCustomObjectId && modelId <= kMaxDlCustomObjectId;
}

bool validWorldPosition(float x, float y, float z) {
  // Match the safety envelope used by the compatible server's inbound
  // PlayerFootSync reader. It also bounds renderer/input-facing local state.
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
         x > -20000.0f && x < 20000.0f && y > -20000.0f && y < 20000.0f &&
         z > -1000.0f && z < 200000.0f;
}

// URL handling is intentionally not delegated to a permissive text/UI path.
// The legacy DynStr8 stream endpoint is an opaque ASCII transport token here;
// Kotlin subsequently permits only safe HTTPS URIs before opening media.
bool validAudioStreamUrl(const std::string& url) {
  if (url.empty() || url.size() > kMaxAudioStreamUrlBytes) return false;
  for (const unsigned char byte : url) {
    if (byte < 0x21u || byte > 0x7Eu) return false;
  }
  return true;
}

// GTA animation library/name values are identifiers, not UI text or paths.
// Constrain them at the network boundary so they can safely survive JNI/JSON
// and later resolve only through a mounted IFP catalog. Case is preserved: the
// catalog owns GTA's lookup semantics and must not silently retarget a name.
bool validAnimationToken(const std::string& token, size_t maximumBytes) {
  if (token.empty() || token.size() > maximumBytes) return false;
  for (const unsigned char byte : token) {
    const bool alphaNumeric = (byte >= 'A' && byte <= 'Z') ||
                              (byte >= 'a' && byte <= 'z') ||
                              (byte >= '0' && byte <= '9');
    if (!alphaNumeric && byte != '_' && byte != '-') return false;
  }
  return true;
}

bool readAnimationDirective(raknet::BitReader& reader, AnimationDirective& out) {
  AnimationDirective next;
  if (!readString8AllowEmpty(reader, kMaxAnimationLibraryBytes, next.library) ||
      !readString8AllowEmpty(reader, kMaxAnimationNameBytes, next.name)) {
    return false;
  }
  bool finite = true;
  next.delta = readFloat(reader, finite);
  next.loop = reader.readBit();
  next.lockX = reader.readBit();
  next.lockY = reader.readBit();
  next.freeze = reader.readBit();
  next.durationMs = reader.readBits(32);
  if (!reader.ok() || !finite || std::fabs(next.delta) > 100.0f ||
      !validAnimationToken(next.library, kMaxAnimationLibraryBytes) ||
      !validAnimationToken(next.name, kMaxAnimationNameBytes)) {
    return false;
  }
  next.active = true;
  // Retain the monotonic presentation revision across a replacement; callers
  // advance it only after the complete RPC body is accepted.
  next.revision = out.revision;
  out = std::move(next);
  return true;
}

void advanceAnimationRevision(AnimationDirective& animation) {
  ++animation.revision;
  if (animation.revision == 0) animation.revision = 1;
}

void clearAnimationDirective(AnimationDirective& animation) {
  const uint64_t nextRevision = animation.revision == std::numeric_limits<uint64_t>::max()
                                    ? 1u
                                    : animation.revision + 1u;
  animation = {};
  animation.revision = nextRevision;
}

bool validWorldBounds(float maxX, float minX, float maxY, float minY) {  // The compatible SetPlayerWorldBounds contract is maxX, minX, maxY, minY
  // and defaults to ±20000. Keep the same world envelope as inbound player
  // positions, reject inverted/degenerate rectangles, and never normalize a
  // malformed server rectangle into a different movement policy.
  return std::isfinite(maxX) && std::isfinite(minX) && std::isfinite(maxY) &&
         std::isfinite(minY) && maxX > minX && maxY > minY &&
         maxX <= 20000.0f && minX >= -20000.0f &&
         maxY <= 20000.0f && minY >= -20000.0f;
}

bool validServerGravity(float gravity) {
  // This remains broader than the local pilot's support envelope. Retaining a
  // legitimate unusual server value is safe; movement independently fails
  // closed if it cannot model that value without fabricating a trajectory.
  return std::isfinite(gravity) && gravity >= 0.0001f && gravity <= 0.1f;
}

bool validVelocity(float x, float y, float z) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
  const double lengthSquared = static_cast<double>(x) * x + static_cast<double>(y) * y +
                               static_cast<double>(z) * z;
  return std::isfinite(lengthSquared) && lengthSquared <= 100.0 * 100.0;
}

uint64_t nextDirectiveRevision(uint64_t revision) {
  return revision == std::numeric_limits<uint64_t>::max() ? 1u : revision + 1u;
}

// SA-MP documents SetVehicleVelocity/SetVehicleAngularVelocity as having no
// effect on trains. Reject the full GTA-SA rail family here rather than trying
// to reinterpret this ID-less recipient command as a generic vehicle force.
bool isTrainVehicleModel(uint32_t model) {
  switch (model) {
    case 449u:  // Tram
    case 537u:  // Freight
    case 538u:  // Brown Streak
    case 569u:  // Freight Flat trailer
    case 570u:  // Brown Streak carriage
    case 590u:  // Freight box carriage
      return true;
    default:
      return false;
  }
}

bool validVehicleParameterValue(int8_t value) {
  return value == -1 || value == 0 || value == 1;
}

bool applyVehicleParameters(const std::array<int8_t, kVehicleParameterCount>& incoming,
                            VehicleParameters& target) {
  for (int8_t value : incoming) {
    if (!validVehicleParameterValue(value)) return false;
  }
  // Current open.mp's SetVehicleParams and SetVehicleParamsForPlayer writers
  // serialize the complete VehicleParams struct. The server also replaces its
  // retained struct before writing it, so raw -1 means that field is now
  // *unset*, not a delta/no-change instruction. Keep the same full-snapshot
  // contract or an old engine/lock fact would survive a later server reset.
  const bool changed = !target.known || target.values != incoming;
  target.values = incoming;
  target.known = true;
  return changed;
}

bool applyVehicleParametersForPlayer(uint8_t objective, uint8_t doors,
                                     VehicleParameters& target) {
  if (objective > 1u || doors > 1u) return false;
  bool changed = false;
  const size_t objectiveIndex = static_cast<size_t>(VehicleParameter::Objective);
  const size_t doorsIndex = static_cast<size_t>(VehicleParameter::Doors);
  if (target.values[objectiveIndex] != static_cast<int8_t>(objective)) {
    target.values[objectiveIndex] = static_cast<int8_t>(objective);
    changed = true;
  }
  if (target.values[doorsIndex] != static_cast<int8_t>(doors)) {
    target.values[doorsIndex] = static_cast<int8_t>(doors);
    changed = true;
  }
  if (!target.known) {
    target.known = true;
    changed = true;
  }
  return changed;
}

void clearVehicleParameters(Vehicle& vehicle) {
  vehicle.parameters = {};
}

void clearVehicleVelocityDirectives(Vehicle& vehicle) {
  vehicle.linearVelocityKnown = false;
  vehicle.angularVelocityKnown = false;
  vehicle.linearVelocityX = 0.0f;
  vehicle.linearVelocityY = 0.0f;
  vehicle.linearVelocityZ = 0.0f;
  vehicle.angularVelocityX = 0.0f;
  vehicle.angularVelocityY = 0.0f;
  vehicle.angularVelocityZ = 0.0f;
  // Do not reset these to zero on an ID reuse. A retained local driver bridge
  // can then distinguish a fresh StreamIn lifecycle from an old command for
  // the same raw vehicle ID without relying on a wall-clock deadline.
  vehicle.linearVelocityRevision = nextDirectiveRevision(vehicle.linearVelocityRevision);
  vehicle.angularVelocityRevision = nextDirectiveRevision(vehicle.angularVelocityRevision);
}

bool validRemoteAim(const syncdecode::RemoteAimSync& sync) {
  if (sync.cameraMode < 3u || sync.cameraMode > 64u || sync.cameraMode == 5u ||
      sync.cameraMode == 6u || sync.cameraMode == 17u || sync.cameraMode == 52u ||
      sync.cameraMode == 54u || sync.cameraMode == 60u || sync.cameraMode == 61u ||
      (sync.cameraMode >= 9u && sync.cameraMode <= 13u) ||
      (sync.cameraMode >= 19u && sync.cameraMode <= 21u) ||
      (sync.cameraMode >= 23u && sync.cameraMode <= 28u) ||
      (sync.cameraMode >= 30u && sync.cameraMode <= 45u) ||
      (sync.cameraMode >= 48u && sync.cameraMode <= 50u) ||
      !validWorldPosition(sync.cameraPositionX, sync.cameraPositionY, sync.cameraPositionZ) ||
      !std::isfinite(sync.cameraFrontX) || !std::isfinite(sync.cameraFrontY) ||
      !std::isfinite(sync.cameraFrontZ) || !std::isfinite(sync.aimZ) ||
      sync.aimZ < -3.14159265358979323846f || sync.aimZ > 3.14159265358979323846f) {
    return false;
  }
  const double frontSquared = static_cast<double>(sync.cameraFrontX) * sync.cameraFrontX +
                              static_cast<double>(sync.cameraFrontY) * sync.cameraFrontY +
                              static_cast<double>(sync.cameraFrontZ) * sync.cameraFrontZ;
  return std::isfinite(frontSquared) && frontSquared > 0.0 && frontSquared < 1.5;
}

bool validRemoteBullet(const syncdecode::RemoteBulletSync& sync) {
  const auto finiteVectorWithinSquared = [](float x, float y, float z, double maximumSquared) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    const double squared = static_cast<double>(x) * x + static_cast<double>(y) * y +
                           static_cast<double>(z) * z;
    return std::isfinite(squared) && squared <= maximumSquared;
  };
  const bool shootableWeapon = (sync.weaponId >= 22u && sync.weaponId <= 34u) ||
                               sync.weaponId == 38u;
  if (sync.hitType > 4u || !shootableWeapon ||
      !finiteVectorWithinSquared(sync.originX, sync.originY, sync.originZ, 20000.0 * 20000.0) ||
      !validWorldPosition(sync.hitPositionX, sync.hitPositionY, sync.hitPositionZ)) {
    return false;
  }
  const double maxOffsetSquared = sync.hitType == 0u ? 20000.0 * 20000.0 : 1000.0 * 1000.0;
  return finiteVectorWithinSquared(sync.offsetX, sync.offsetY, sync.offsetZ, maxOffsetSquared);
}

bool validVital(float value) {
  // The native game can receive scripted health/armour changes. Keep a broad,
  // finite cap for a safe HUD/state bridge rather than assuming only 0..100.
  return std::isfinite(value) && value >= 0.0f && value <= 1000.0f;
}

bool validObjectRotation(float x, float y, float z) {
  // SA-MP accepts angles beyond a single turn, but arbitrary huge values only
  // destabilize later matrix math. Normalize after this broad finite bound.
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
         std::fabs(x) <= 360000.0f && std::fabs(y) <= 360000.0f &&
         std::fabs(z) <= 360000.0f;
}

bool validObjectAttachmentOffset(float x, float y, float z) {
  // Attachment offsets are local distances, not world coordinates. Keep them
  // finite and broadly bounded before applying a parent transform so a forged
  // CreateObject cannot overflow/trash a renderer snapshot.
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
         std::fabs(x) <= 10000.0f && std::fabs(y) <= 10000.0f &&
         std::fabs(z) <= 10000.0f;
}

bool validPlayerAttachedObjectScale(float x, float y, float z) {
  // Scripted accessories can legitimately mirror an axis, but zero/near-zero
  // or huge scales destabilize DFF transforms and add no usable presentation.
  const auto validAxis = [](float value) {
    return std::isfinite(value) && std::fabs(value) >= 0.001f && std::fabs(value) <= 100.0f;
  };
  return validAxis(x) && validAxis(y) && validAxis(z);
}

bool validObjectDrawDistance(float value) {
  // Zero requests GTA's model default. A world-scale upper cap bounds renderer
  // visibility without silently trusting infinities or a forged giant value.
  return std::isfinite(value) && value >= 0.0f && value <= 50000.0f;
}

bool validGangZoneBounds(float minX, float minY, float maxX, float maxY) {
  // Gang zones have no Z coordinate. Retain the same finite horizontal safety
  // envelope as world positions and require a non-empty oriented rectangle so
  // Canvas never receives a pathological/inverted draw region.
  return std::isfinite(minX) && std::isfinite(minY) && std::isfinite(maxX) &&
         std::isfinite(maxY) && minX > -20000.0f && minX < 20000.0f &&
         minY > -20000.0f && minY < 20000.0f && maxX > -20000.0f &&
         maxX < 20000.0f && maxY > -20000.0f && maxY < 20000.0f &&
         minX < maxX && minY < maxY;
}

float normalizedAngle(float angle) {
  float result = std::fmod(angle, 360.0f);
  if (result < 0.0f) result += 360.0f;
  return result;
}

uint64_t monotonicMilliseconds() {
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

bool activeChatBubble(const ChatBubble& bubble, uint64_t nowMs) {
  return bubble.active && !bubble.text.empty() && bubble.drawDistance > 0.0f &&
         bubble.expiresAtMs > nowMs;
}

void clearChatBubble(ChatBubble& bubble) {
  bubble = {};
}

int64_t saturatingMoneyAdd(int64_t amount, int32_t delta) {
  if (delta > 0 && amount > std::numeric_limits<int64_t>::max() - delta) {
    return std::numeric_limits<int64_t>::max();
  }
  if (delta < 0 && amount < std::numeric_limits<int64_t>::min() - delta) {
    return std::numeric_limits<int64_t>::min();
  }
  return amount + delta;
}

void advanceAudioStreamRevision(AudioStream& stream) {
  // Zero is the no-directive snapshot default. Keep it reserved even after a
  // theoretical 64-bit wrap so Android can always distinguish an update.
  if (stream.revision == std::numeric_limits<uint64_t>::max()) {
    stream.revision = 1u;
  } else {
    ++stream.revision;
    if (stream.revision == 0u) stream.revision = 1u;
  }
}

float interpolateAngle(float start, float target, float progress) {
  float delta = std::fmod(target - start + 540.0f, 360.0f) - 180.0f;
  return normalizedAngle(start + delta * progress);
}

void sampleCameraEndpoint(const CameraEndpoint& endpoint, uint64_t nowMs,
                          bool& known, bool& interpolating,
                          float& x, float& y, float& z) {
  known = endpoint.known;
  interpolating = false;
  x = endpoint.x;
  y = endpoint.y;
  z = endpoint.z;
  if (!endpoint.known || !endpoint.interpolating || endpoint.durationMs == 0u ||
      nowMs < endpoint.startedAtMs) {
    return;
  }
  const uint64_t elapsed = nowMs - endpoint.startedAtMs;
  const float progress = std::clamp(static_cast<float>(elapsed) /
                                        static_cast<float>(endpoint.durationMs),
                                    0.0f, 1.0f);
  x = endpoint.fromX + (endpoint.toX - endpoint.fromX) * progress;
  y = endpoint.fromY + (endpoint.toY - endpoint.fromY) * progress;
  z = endpoint.fromZ + (endpoint.toZ - endpoint.fromZ) * progress;
  interpolating = progress < 1.0f;
}

ServerObjectView sampledObjectView(const ServerObject& object, uint64_t nowMs) {
  ServerObjectView view;
  view.id = object.id;
  view.model = object.model;
  view.sourceModel = object.sourceModel;
  view.x = object.x;
  view.y = object.y;
  view.z = object.z;
  view.rotationX = object.rotationX;
  view.rotationY = object.rotationY;
  view.rotationZ = object.rotationZ;
  view.drawDistance = object.drawDistance;
  view.materials = object.materials;
  if (!object.moving || object.moveDurationSeconds <= 0.0f || nowMs < object.moveStartedMs) {
    return view;
  }
  const float elapsedSeconds = static_cast<float>(nowMs - object.moveStartedMs) / 1000.0f;
  const float progress = std::clamp(elapsedSeconds / object.moveDurationSeconds, 0.0f, 1.0f);
  view.x += (object.moveTargetX - object.x) * progress;
  view.y += (object.moveTargetY - object.y) * progress;
  view.z += (object.moveTargetZ - object.z) * progress;
  if (object.moveHasTargetRotation) {
    view.rotationX = interpolateAngle(object.rotationX, object.moveTargetRotationX, progress);
    view.rotationY = interpolateAngle(object.rotationY, object.moveTargetRotationY, progress);
    view.rotationZ = interpolateAngle(object.rotationZ, object.moveTargetRotationZ, progress);
  }
  view.moving = progress < 1.0f;
  return view;
}

// Legacy CreateObject attachment fields are source-writer verified. The state
// reducer retains their parent IDs/offsets; frame export resolves only the
// yaw-oriented parent transform available from this bounded client. A vehicle
// has only a replicated heading here, so the renderer deliberately does not
// claim full GTA matrix, vehicle pitch/roll, or suspension fidelity.
bool applyObjectAttachment(const ServerObject& object, const ServerObjectView& parent,
                           ServerObjectView& child) {
  constexpr float kDegreesToRadians = 0.01745329251994329577f;
  const float radians = parent.rotationZ * kDegreesToRadians;
  const float cosine = std::cos(radians);
  const float sine = std::sin(radians);
  const float offsetX = object.attachmentOffsetX * cosine + object.attachmentOffsetY * sine;
  const float offsetY = -object.attachmentOffsetX * sine + object.attachmentOffsetY * cosine;
  child.x = parent.x + offsetX;
  child.y = parent.y + offsetY;
  child.z = parent.z + object.attachmentOffsetZ;
  if (object.attachmentSyncRotation) {
    child.rotationX = normalizedAngle(parent.rotationX + object.attachmentRotationX);
    child.rotationY = normalizedAngle(parent.rotationY + object.attachmentRotationY);
    child.rotationZ = normalizedAngle(parent.rotationZ + object.attachmentRotationZ);
  }
  child.moving = child.moving || parent.moving;
  return validWorldPosition(child.x, child.y, child.z) &&
         validObjectRotation(child.rotationX, child.rotationY, child.rotationZ);
}

// The protocol names a GTA ped bone but no portable ped skeleton is available
// in this asset-free renderer. These fixed vertical anchors make common head/
// spine/limb accessories visible while staying explicitly presentation-only;
// they are never used for collision, targeting, or outbound sync.
float approximateAttachedBoneHeight(uint8_t bone) {
  switch (bone) {
    case 1: return 0.95f;   // spine
    case 2: return 1.62f;   // head
    case 3: case 4: return 1.30f;
    case 5: case 6: return 1.12f;
    case 7: case 8: return 0.62f;
    case 9: case 10: return 0.08f;
    case 11: case 12: return 0.34f;
    case 13: case 14: return 0.98f;
    case 15: case 16: return 1.42f;
    case 17: return 1.48f; // neck
    case 18: return 1.57f; // jaw
    default: return 0.0f;
  }
}

bool resolvePlayerAttachmentParent(const std::map<uint16_t, Player>& players,
                                   const LocalPlayer& localPlayer, bool localPlayerIdKnown,
                                   uint16_t localPlayerId,
                                   const LocalObjectAttachmentAnchor* localAttachmentAnchor,
                                   uint16_t playerId, ServerObjectView& parent) {
  if (localPlayerIdKnown && playerId == localPlayerId) {
    const bool predictedAnchorValid = localAttachmentAnchor != nullptr &&
        localAttachmentAnchor->valid &&
        validWorldPosition(localAttachmentAnchor->x, localAttachmentAnchor->y,
                           localAttachmentAnchor->z) &&
        validObjectRotation(0.0f, 0.0f, localAttachmentAnchor->angle);
    if (predictedAnchorValid) {
      parent.x = localAttachmentAnchor->x;
      parent.y = localAttachmentAnchor->y;
      parent.z = localAttachmentAnchor->z;
      parent.rotationZ = localAttachmentAnchor->angle;
      return true;
    }
    if (!localPlayer.positionKnown || !localPlayer.facingKnown) return false;
    parent.x = localPlayer.x;
    parent.y = localPlayer.y;
    parent.z = localPlayer.z;
    parent.rotationZ = localPlayer.angle;
    return true;
  }
  const auto player = players.find(playerId);
  if (player == players.end() || !player->second.streamedIn || !player->second.positionKnown) {
    return false;
  }
  parent.x = player->second.x;
  parent.y = player->second.y;
  parent.z = player->second.z;
  parent.rotationZ = player->second.angle;
  return validWorldPosition(parent.x, parent.y, parent.z) &&
         validObjectRotation(0.0f, 0.0f, parent.rotationZ);
}

constexpr size_t kMaxObjectAttachmentDepth = 8;

bool resolveObjectAttachmentView(const std::map<uint16_t, ServerObject>& objects,
                                 const std::map<uint16_t, Vehicle>& vehicles,
                                 const std::map<uint16_t, Player>& players,
                                 const LocalPlayer& localPlayer, bool localPlayerIdKnown,
                                 uint16_t localPlayerId,
                                 const LocalObjectAttachmentAnchor* localAttachmentAnchor,
                                 uint16_t objectId, uint64_t nowMs,
                                 std::array<uint16_t, kMaxObjectAttachmentDepth>& ancestry,
                                 size_t depth, ServerObjectView& out) {
  if (depth >= ancestry.size()) return false;
  for (size_t index = 0; index < depth; ++index) {
    if (ancestry[index] == objectId) return false;
  }
  const auto found = objects.find(objectId);
  if (found == objects.end()) return false;
  ancestry[depth] = objectId;

  const ServerObject& object = found->second;
  ServerObjectView child = sampledObjectView(object, nowMs);
  if (!object.attached || object.attachmentKind == ObjectAttachmentKind::None) {
    out = std::move(child);
    return true;
  }

  ServerObjectView parent;
  switch (object.attachmentKind) {
    case ObjectAttachmentKind::Vehicle: {
      const auto vehicle = vehicles.find(object.attachmentParentId);
      if (vehicle == vehicles.end() || !vehicle->second.streamedIn) return false;
      parent.x = vehicle->second.x;
      parent.y = vehicle->second.y;
      parent.z = vehicle->second.z;
      parent.rotationZ = vehicle->second.angle;
      break;
    }
    case ObjectAttachmentKind::Object:
      if (!resolveObjectAttachmentView(objects, vehicles, players, localPlayer, localPlayerIdKnown,
                                       localPlayerId, localAttachmentAnchor, object.attachmentParentId,
                                       nowMs, ancestry, depth + 1u, parent)) {
        return false;
      }
      break;
    case ObjectAttachmentKind::Player: {
      if (localPlayerIdKnown && object.attachmentParentId == localPlayerId) {
        const bool predictedAnchorValid = localAttachmentAnchor != nullptr &&
            localAttachmentAnchor->valid &&
            validWorldPosition(localAttachmentAnchor->x, localAttachmentAnchor->y,
                               localAttachmentAnchor->z) &&
            validObjectRotation(0.0f, 0.0f, localAttachmentAnchor->angle);
        if (predictedAnchorValid) {
          parent.x = localAttachmentAnchor->x;
          parent.y = localAttachmentAnchor->y;
          parent.z = localAttachmentAnchor->z;
          parent.rotationZ = localAttachmentAnchor->angle;
        } else {
          if (!localPlayer.positionKnown || !localPlayer.facingKnown) return false;
          parent.x = localPlayer.x;
          parent.y = localPlayer.y;
          parent.z = localPlayer.z;
          parent.rotationZ = localPlayer.angle;
        }
      } else {
        const auto player = players.find(object.attachmentParentId);
        if (player == players.end() || !player->second.streamedIn || !player->second.positionKnown) {
          return false;
        }
        parent.x = player->second.x;
        parent.y = player->second.y;
        parent.z = player->second.z;
        parent.rotationZ = player->second.angle;
      }
      break;
    }
    case ObjectAttachmentKind::None:
      return false;
  }

  if (!applyObjectAttachment(object, parent, child)) return false;
  out = std::move(child);
  return true;
}

void setEvent(RpcEvent& event, RpcEventKind kind, uint16_t playerId, const char* action) {
  event.kind = kind;
  event.playerId = playerId;
  event.summary = action;
  event.summary += " #";
  event.summary += std::to_string(playerId);
}

}  // namespace

Player* WorldState::getOrCreate(uint16_t id) {
  const auto existing = players_.find(id);
  if (existing != players_.end()) return &existing->second;

  constexpr size_t kMaxTrackedPlayers = 1000;
  if (players_.size() >= kMaxTrackedPlayers) return nullptr;
  auto [it, inserted] = players_.try_emplace(id);
  if (inserted) {
    it->second.id = id;
    const auto override = nameTagVisibilityOverrides_.find(id);
    if (override != nameTagVisibilityOverrides_.end()) {
      it->second.nameTagVisible = override->second;
    }
    const auto scoreOverride = scorePingOverrides_.find(id);
    if (scoreOverride != scorePingOverrides_.end()) {
      it->second.scorePing = scoreOverride->second;
    }
  }
  return &it->second;
}

Vehicle* WorldState::getOrCreateVehicle(uint16_t id) {
  const auto existing = vehicles_.find(id);
  if (existing != vehicles_.end()) return &existing->second;

  // SA-MP's normal vehicle pool is bounded below this. Keep an independent
  // hard ceiling in case a malformed stream repeatedly uses new IDs.
  constexpr size_t kMaxTrackedVehicles = 2000;
  if (vehicles_.size() >= kMaxTrackedVehicles) return nullptr;
  auto [it, inserted] = vehicles_.try_emplace(id);
  if (inserted) it->second.id = id;
  return &it->second;
}

ServerActor* WorldState::getOrCreateServerActor(uint16_t id) {
  const auto existing = serverActors_.find(id);
  if (existing != serverActors_.end()) return &existing->second;

  // Actors are server-managed NPCs, not player-pool IDs. Keep their lifecycle
  // independently bounded so forged ShowActor traffic cannot amplify state.
  constexpr size_t kMaxTrackedServerActors = 1000;
  if (serverActors_.size() >= kMaxTrackedServerActors) return nullptr;
  auto [it, inserted] = serverActors_.try_emplace(id);
  if (inserted) it->second.id = id;
  return &it->second;
}

void WorldState::clearRemoteVehicleOccupancy(uint16_t playerId, bool hideActor) {
  // Packet-200 driver IDs and Packet-211 passenger IDs can be zero. Always
  // use the explicit bool state rather than treating a numeric ID as absent.
  const auto player = players_.find(playerId);
  if (player != players_.end()) {
    Player& actor = player->second;
    actor.remoteVehicleKnown = false;
    actor.remoteVehicleDriver = false;
    actor.remoteVehicleId = 0;
    actor.remoteVehicleSeat = 0;
    if (hideActor) {
      actor.positionKnown = false;
      actor.remoteOnFootKnown = false;
    }
  }
  // A player can drive at most one vehicle. Scan the bounded vehicle table to
  // heal a stale partial state as well as the normal direct association.
  for (auto& [vehicleId, vehicle] : vehicles_) {
    (void)vehicleId;
    if (vehicle.driverKnown && vehicle.driverId == playerId) {
      vehicle.driverKnown = false;
      vehicle.driverId = 0;
    }
  }
}

ServerObject* WorldState::getOrCreateObject(uint16_t id) {
  const auto existing = objects_.find(id);
  if (existing != objects_.end()) return &existing->second;

  // Servers usually keep this below 1000, but permit a bounded high ceiling
  // for open.mp extensions without allowing an RPC flood to consume memory.
  constexpr size_t kMaxTrackedObjects = 4096;
  if (objects_.size() >= kMaxTrackedObjects) return nullptr;
  auto [it, inserted] = objects_.try_emplace(id);
  if (inserted) it->second.id = id;
  return &it->second;
}

Pickup* WorldState::getOrCreatePickup(int32_t id) {
  const auto existing = pickups_.find(id);
  if (existing != pickups_.end()) return &existing->second;

  // Keep pickup state independent of objects: servers can legitimately send a
  // dense stream of map-owned collectible entities.
  constexpr size_t kMaxTrackedPickups = 4096;
  if (pickups_.size() >= kMaxTrackedPickups) return nullptr;
  auto [it, inserted] = pickups_.try_emplace(id);
  if (inserted) it->second.id = id;
  return &it->second;
}

const Player* WorldState::findPlayer(uint16_t id) const {
  const auto it = players_.find(id);
  return it == players_.end() ? nullptr : &it->second;
}

size_t WorldState::streamedPlayerCount() const {
  size_t count = 0;
  for (const auto& [id, player] : players_) {
    (void)id;
    if (player.streamedIn) ++count;
  }
  return count;
}

size_t WorldState::remoteOnFootCount() const {
  size_t count = 0;
  for (const auto& [id, player] : players_) {
    (void)id;
    if (player.streamedIn && player.remoteOnFootKnown) ++count;
  }
  return count;
}

size_t WorldState::streamedVehicleCount() const {
  size_t count = 0;
  for (const auto& [id, vehicle] : vehicles_) {
    (void)id;
    if (vehicle.streamedIn) ++count;
  }
  return count;
}

GameText WorldState::gameText() const {
  GameText snapshot = gameText_;
  if (!snapshot.active || snapshot.durationMs == 0 || snapshot.startedAtMs == 0) {
    snapshot.active = false;
    return snapshot;
  }
  const uint64_t nowMs = monotonicMilliseconds();
  if (nowMs < snapshot.startedAtMs || nowMs - snapshot.startedAtMs >= snapshot.durationMs) {
    snapshot.active = false;
  }
  return snapshot;
}

CameraView WorldState::camera() const {
  CameraView view;
  const uint64_t nowMs = monotonicMilliseconds();
  sampleCameraEndpoint(camera_.position, nowMs, view.positionKnown,
                       view.interpolatingPosition, view.positionX, view.positionY,
                       view.positionZ);
  sampleCameraEndpoint(camera_.lookAt, nowMs, view.lookAtKnown,
                       view.interpolatingLookAt, view.lookAtX, view.lookAtY,
                       view.lookAtZ);
  return view;
}

std::vector<ServerObjectView> WorldState::renderableObjects(
    size_t maxObjects, const LocalObjectAttachmentAnchor* localAttachmentAnchor) const {
  constexpr size_t kHardRenderObjectLimit = 128;
  const size_t limit = std::min(maxObjects, kHardRenderObjectLimit);
  std::vector<ServerObjectView> out;
  out.reserve(std::min(limit, objects_.size()));
  const uint64_t nowMs = monotonicMilliseconds();
  // std::map yields a deterministic ID order for copied frame snapshots. A
  // child is hidden only while a parent is absent/streamed out, malformed, or
  // cyclic; its retained lifecycle record can become renderable later when the
  // matching parent arrives again.
  for (const auto& [id, object] : objects_) {
    (void)object;
    if (out.size() >= limit) break;
    std::array<uint16_t, kMaxObjectAttachmentDepth> ancestry{};
    ServerObjectView view;
    if (!resolveObjectAttachmentView(objects_, vehicles_, players_, localPlayer_, initialized_,
                                     initialized_ ? playerInit_.playerId : 0u,
                                     localAttachmentAnchor, id, nowMs, ancestry, 0u, view)) {
      continue;
    }
    out.push_back(std::move(view));
  }
  return out;
}

bool WorldState::selectedObjectView(uint16_t objectId, ServerObjectView& out) const {
  out = {};
  if (!objectSelectionActive_ || objectId == 0u || objectId == 0xFFFFu ||
      objects_.find(objectId) == objects_.end()) {
    return false;
  }
  // The chosen ID must still resolve through the same bounded attachment-state
  // logic as the renderer. A disappeared vehicle/player/object parent therefore
  // makes selection unavailable rather than leaking the child's stale Create
  // coordinates into a server callback.
  std::array<uint16_t, kMaxObjectAttachmentDepth> ancestry{};
  const uint64_t nowMs = monotonicMilliseconds();
  return resolveObjectAttachmentView(objects_, vehicles_, players_, localPlayer_, initialized_,
                                     initialized_ ? playerInit_.playerId : 0u, nullptr, objectId,
                                     nowMs, ancestry, 0u, out);
}

bool WorldState::editedObjectView(uint16_t objectId, bool playerObject,
                                  ServerObjectView& out) const {
  out = {};
  if (!objectEdit_.active || objectEdit_.objectId != objectId ||
      objectEdit_.playerObject != playerObject || objectId == 0u || objectId == 0xFFFFu ||
      objects_.find(objectId) == objects_.end()) {
    return false;
  }
  // Apply the same bounded resolution as renderer/selection. Edit replies may
  // carry a user-adjusted transform, but only an extant server object whose
  // current parent chain resolves can remain an active Android editor target.
  std::array<uint16_t, kMaxObjectAttachmentDepth> ancestry{};
  const uint64_t nowMs = monotonicMilliseconds();
  return resolveObjectAttachmentView(objects_, vehicles_, players_, localPlayer_, initialized_,
                                     initialized_ ? playerInit_.playerId : 0u, nullptr, objectId,
                                     nowMs, ancestry, 0u, out);
}

bool WorldState::editedAttachedObject(uint8_t slot, PlayerAttachedObject& out) const {
  out = {};
  if (!attachedObjectEdit_.active || attachedObjectEdit_.slot != slot ||
      slot >= kMaxPlayerAttachedObjectSlots || !initialized_) {
    return false;
  }
  const auto playerSlots = playerAttachedObjects_.find(playerInit_.playerId);
  if (playerSlots == playerAttachedObjects_.end()) return false;
  const PlayerAttachedObject& attachment = playerSlots->second[slot];
  if (!attachment.active) return false;
  out = attachment;
  return true;
}

std::vector<PlayerAttachedObjectView> WorldState::renderablePlayerAttachedObjects(
    size_t maxAttachments, const LocalObjectAttachmentAnchor* localAttachmentAnchor) const {
  constexpr size_t kHardRenderAttachmentLimit = 64;
  const size_t limit = std::min(maxAttachments, kHardRenderAttachmentLimit);
  std::vector<PlayerAttachedObjectView> out;
  out.reserve(std::min(limit, playerAttachedObjects_.size() * kMaxPlayerAttachedObjectSlots));
  for (const auto& [playerId, slots] : playerAttachedObjects_) {
    if (out.size() >= limit) break;
    ServerObjectView parent;
    if (!resolvePlayerAttachmentParent(players_, localPlayer_, initialized_,
                                       initialized_ ? playerInit_.playerId : 0u,
                                       localAttachmentAnchor, playerId, parent)) {
      continue;
    }
    constexpr float kDegreesToRadians = 0.01745329251994329577f;
    const float radians = parent.rotationZ * kDegreesToRadians;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    for (size_t slot = 0; slot < slots.size() && out.size() < limit; ++slot) {
      const PlayerAttachedObject& attachment = slots[slot];
      if (!attachment.active) continue;
      const float offsetX = attachment.offsetX * cosine + attachment.offsetY * sine;
      const float offsetY = -attachment.offsetX * sine + attachment.offsetY * cosine;
      PlayerAttachedObjectView view;
      view.playerId = playerId;
      view.slot = static_cast<uint8_t>(slot);
      view.model = attachment.model;
      view.sourceModel = attachment.sourceModel;
      view.bone = attachment.bone;
      view.x = parent.x + offsetX;
      view.y = parent.y + offsetY;
      view.z = parent.z + approximateAttachedBoneHeight(attachment.bone) + attachment.offsetZ;
      view.rotationX = normalizedAngle(attachment.rotationX);
      view.rotationY = normalizedAngle(attachment.rotationY);
      view.rotationZ = normalizedAngle(parent.rotationZ + attachment.rotationZ);
      view.scaleX = attachment.scaleX;
      view.scaleY = attachment.scaleY;
      view.scaleZ = attachment.scaleZ;
      view.colour1Abgr = attachment.colour1Abgr;
      view.colour2Abgr = attachment.colour2Abgr;
      if (!validWorldPosition(view.x, view.y, view.z) ||
          !validObjectRotation(view.rotationX, view.rotationY, view.rotationZ) ||
          !validPlayerAttachedObjectScale(view.scaleX, view.scaleY, view.scaleZ)) {
        continue;
      }
      out.push_back(std::move(view));
    }
  }
  return out;
}

std::vector<PickupView> WorldState::renderablePickups(size_t maxPickups) const {
  constexpr size_t kHardRenderPickupLimit = 128;
  const size_t limit = std::min(maxPickups, kHardRenderPickupLimit);
  std::vector<PickupView> out;
  out.reserve(std::min(limit, pickups_.size()));
  // Pickups live in a std::map for a stable ascending-ID snapshot.
  for (const auto& [id, pickup] : pickups_) {
    (void)id;
    if (out.size() >= limit) break;
    out.push_back({pickup.id, pickup.model, pickup.spawnType, pickup.x, pickup.y, pickup.z});
  }
  return out;
}

const Pickup* WorldState::nearestPickup(float x, float y, float z, float radius) const {
  if (!validWorldPosition(x, y, z) || !std::isfinite(radius) || radius <= 0.0f ||
      radius > 12.0f) {
    return nullptr;
  }
  const float maximumDistanceSquared = radius * radius;
  const Pickup* nearest = nullptr;
  float nearestDistanceSquared = maximumDistanceSquared;
  for (const auto& [id, pickup] : pickups_) {
    (void)id;
    const float dx = pickup.x - x;
    const float dy = pickup.y - y;
    const float dz = pickup.z - z;
    const float distanceSquared = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(distanceSquared) || distanceSquared > nearestDistanceSquared) continue;
    // A deterministic ID tiebreaker makes interaction selection reproducible.
    if (nearest == nullptr || distanceSquared < nearestDistanceSquared || pickup.id < nearest->id) {
      nearest = &pickup;
      nearestDistanceSquared = distanceSquared;
    }
  }
  return nearest;
}

std::vector<TextDrawView> WorldState::renderableTextDraws(size_t maxTextDraws) const {
  constexpr size_t kHardRenderTextDrawLimit = 32;
  const size_t limit = std::min(maxTextDraws, kHardRenderTextDrawLimit);
  std::vector<TextDrawView> out;
  out.reserve(std::min(limit, textDraws_.size()));
  // Wire IDs are map keys, yielding deterministic global-before-player order.
  for (const auto& [wireId, draw] : textDraws_) {
    (void)wireId;
    if (out.size() >= limit) break;
    TextDrawView view;
    view.wireId = draw.wireId;
    view.playerTextDraw = draw.playerTextDraw;
    view.useBox = draw.useBox;
    view.alignment = draw.alignment;
    view.letterWidth = draw.letterWidth;
    view.letterHeight = draw.letterHeight;
    view.letterColourAbgr = draw.letterColourAbgr;
    view.textWidth = draw.textWidth;
    view.textHeight = draw.textHeight;
    view.boxColourAbgr = draw.boxColourAbgr;
    view.shadow = draw.shadow;
    view.outline = draw.outline;
    view.backgroundColourAbgr = draw.backgroundColourAbgr;
    view.style = draw.style;
    view.selectable = draw.selectable;
    view.x = draw.x;
    view.y = draw.y;
    view.model = draw.model;
    view.rotationX = draw.rotationX;
    view.rotationY = draw.rotationY;
    view.rotationZ = draw.rotationZ;
    view.zoom = draw.zoom;
    view.colour1 = draw.colour1;
    view.colour2 = draw.colour2;
    view.text = draw.text;
    out.push_back(std::move(view));
  }
  return out;
}

std::vector<TextLabelView> WorldState::renderableTextLabels(size_t maxTextLabels) const {
  constexpr size_t kHardRenderTextLabelLimit = 64;
  const size_t limit = std::min(maxTextLabels, kHardRenderTextLabelLimit);
  std::vector<TextLabelView> out;
  out.reserve(std::min(limit, textLabels_.size()));
  // Wire IDs are map keys, preserving global labels before player labels.
  for (const auto& [wireId, label] : textLabels_) {
    (void)wireId;
    if (out.size() >= limit) break;
    TextLabelView view;
    view.wireId = label.wireId;
    view.playerTextLabel = label.playerTextLabel;
    view.rgba = label.rgba;
    view.x = label.x;
    view.y = label.y;
    view.z = label.z;
    view.drawDistance = label.drawDistance;
    view.los = label.los;
    view.playerAttachId = label.playerAttachId;
    view.vehicleAttachId = label.vehicleAttachId;
    view.text = label.text;
    out.push_back(std::move(view));
  }
  return out;
}

std::vector<MapIconView> WorldState::renderableMapIcons(size_t maxMapIcons) const {
  constexpr size_t kHardRenderMapIconLimit = 64;
  const size_t limit = std::min(maxMapIcons, kHardRenderMapIconLimit);
  std::vector<MapIconView> out;
  out.reserve(std::min(limit, mapIcons_.size()));
  for (const auto& [id, icon] : mapIcons_) {
    (void)id;
    if (out.size() >= limit) break;
    MapIconView view;
    view.id = icon.id;
    view.x = icon.x;
    view.y = icon.y;
    view.z = icon.z;
    view.type = icon.type;
    view.rgba = icon.rgba;
    view.style = icon.style;
    out.push_back(view);
  }
  return out;
}

std::vector<GangZoneView> WorldState::renderableGangZones(size_t maxGangZones) const {
  constexpr size_t kHardRenderGangZoneLimit = 64;
  const size_t limit = std::min(maxGangZones, kHardRenderGangZoneLimit);
  std::vector<GangZoneView> out;
  out.reserve(std::min(limit, gangZones_.size()));
  // map ordering provides stable ascending legacy IDs across snapshots.
  for (const auto& [id, zone] : gangZones_) {
    (void)id;
    if (out.size() >= limit) break;
    out.push_back({zone.id, zone.minX, zone.minY, zone.maxX, zone.maxY,
                   zone.colourAbgr, zone.flashing, zone.flashColourAbgr});
  }
  return out;
}

bool WorldState::isSelectableTextDraw(uint16_t wireId) const {
  const auto found = textDraws_.find(wireId);
  return textDrawSelectionActive_ && found != textDraws_.end() && found->second.selectable;
}

std::vector<ChatLine> WorldState::recentChat(size_t maxLines) const {
  constexpr size_t kHardHudChatLimit = 8;
  const size_t limit = std::min(maxLines, kHardHudChatLimit);
  if (limit == 0 || chatLines_.empty()) return {};
  const size_t first = chatLines_.size() > limit ? chatLines_.size() - limit : 0;
  return std::vector<ChatLine>(chatLines_.begin() + static_cast<std::ptrdiff_t>(first),
                               chatLines_.end());
}

std::vector<DeathMessage> WorldState::recentDeathMessages(size_t maxMessages) const {
  const size_t limit = std::min(maxMessages, kHardHudDeathMessageLimit);
  if (limit == 0 || deathMessages_.empty()) return {};
  const uint64_t nowMs = monotonicMilliseconds();
  std::vector<DeathMessage> live;
  live.reserve(std::min(limit, deathMessages_.size()));
  for (const DeathMessage& message : deathMessages_) {
    if (message.expiresAtMs <= nowMs) continue;
    live.push_back(message);
  }
  if (live.size() > limit) {
    live.erase(live.begin(), live.end() - static_cast<std::ptrdiff_t>(limit));
  }
  return live;
}

std::vector<RemotePlayerView> WorldState::renderablePlayers(size_t maxPlayers) const {
  constexpr size_t kHardRenderEntityLimit = 64;
  const size_t limit = std::min(maxPlayers, kHardRenderEntityLimit);
  std::vector<RemotePlayerView> out;
  out.reserve(std::min(limit, players_.size()));
  const uint64_t nowMs = monotonicMilliseconds();
  // players_ is a std::map, so the renderer receives stable ascending player
  // IDs rather than arbitrary hash-map iteration order.
  for (const auto& [id, player] : players_) {
    if (!player.streamedIn || !player.positionKnown || out.size() >= limit) continue;
    RemotePlayerView view;
    view.id = id;
    view.rgba = player.rgba;
    view.model = player.model;
    view.customModel = player.customModel;
    view.onFootKnown = player.remoteOnFootKnown;
    const bool remoteAimFresh = player.remoteAimKnown && nowMs >= player.remoteAimUpdatedMs &&
        nowMs - player.remoteAimUpdatedMs <= kRemoteAimPresentationLifetimeMs;
    view.aimKnown = remoteAimFresh;
    if (remoteAimFresh) {
      view.aimCameraMode = player.aimCameraMode;
      view.aimWeaponState = player.aimWeaponState;
      view.aimCameraFrontX = player.aimCameraFrontX;
      view.aimCameraFrontY = player.aimCameraFrontY;
      view.aimCameraFrontZ = player.aimCameraFrontZ;
      view.aimCameraPositionX = player.aimCameraPositionX;
      view.aimCameraPositionY = player.aimCameraPositionY;
      view.aimCameraPositionZ = player.aimCameraPositionZ;
    }
    const bool remoteBulletFresh = player.remoteBulletKnown && nowMs >= player.remoteBulletUpdatedMs &&
        nowMs - player.remoteBulletUpdatedMs <= kRemoteBulletPresentationLifetimeMs;
    view.bulletKnown = remoteBulletFresh;
    if (remoteBulletFresh) {
      view.bulletWeaponId = player.bulletWeaponId;
      view.bulletOriginX = player.bulletOriginX;
      view.bulletOriginY = player.bulletOriginY;
      view.bulletOriginZ = player.bulletOriginZ;
      view.bulletDirectionX = player.bulletDirectionX;
      view.bulletDirectionY = player.bulletDirectionY;
      view.bulletDirectionZ = player.bulletDirectionZ;
    }
    view.wasted = player.remoteWastedKnown && nowMs >= player.remoteWastedUpdatedMs &&
                  nowMs - player.remoteWastedUpdatedMs <= kRemoteWastedPresentationLifetimeMs;
    view.forcedAnimation = player.forcedAnimation;
    view.inVehicle = player.remoteVehicleKnown;
    view.vehicleDriver = player.remoteVehicleDriver;
    view.vehicleId = player.remoteVehicleId;
    view.vehicleSeat = player.remoteVehicleSeat;
    view.nameTagVisible = player.nameTagVisible;
    if (activeChatBubble(player.chatBubble, nowMs)) {
      view.chatBubbleActive = true;
      view.chatBubbleRgba = player.chatBubble.rgba;
      view.chatBubbleDrawDistance = player.chatBubble.drawDistance;
      view.chatBubbleText = player.chatBubble.text;
    }
    view.x = player.x;
    view.y = player.y;
    view.z = player.z;
    view.angle = player.angle;
    view.health = player.health;
    view.armour = player.armour;
    view.name = player.name;
    out.push_back(std::move(view));
  }
  return out;
}

std::vector<ServerActorView> WorldState::renderableServerActors(size_t maxActors) const {
  constexpr size_t kHardRenderServerActorLimit = 64;
  const size_t limit = std::min(maxActors, kHardRenderServerActorLimit);
  std::vector<ServerActorView> out;
  out.reserve(std::min(limit, serverActors_.size()));
  // std::map gives the renderer a deterministic actor-ID order regardless of
  // the arrival order of ShowActor/SetActor mutators.
  for (const auto& [id, actor] : serverActors_) {
    if (out.size() >= limit) break;
    ServerActorView view;
    view.id = id;
    view.model = actor.model;
    view.customModel = actor.customModel;
    view.x = actor.x;
    view.y = actor.y;
    view.z = actor.z;
    view.angle = actor.angle;
    view.health = actor.health;
    view.invulnerable = actor.invulnerable;
    view.forcedAnimation = actor.forcedAnimation;
    out.push_back(std::move(view));
  }
  return out;
}

bool WorldState::hasActiveLocalChatBubble() const {
  return activeChatBubble(localPlayer_.chatBubble, monotonicMilliseconds());
}

std::vector<ScoreboardPlayerView> WorldState::scoreboardPlayers(size_t maxPlayers) const {
  constexpr size_t kHardScoreboardLimit = 64;
  const size_t limit = std::min(maxPlayers, kHardScoreboardLimit);
  std::vector<ScoreboardPlayerView> out;
  out.reserve(std::min(limit, players_.size()));
  // Stable player-ID order avoids a noisy table when a server sends the same
  // complete score/ping roster repeatedly.
  for (const auto& [id, player] : players_) {
    if (!player.scorePing.known || out.size() >= limit) continue;
    ScoreboardPlayerView view;
    view.id = id;
    view.rgba = player.rgba;
    view.npc = player.npc;
    view.score = player.scorePing.score;
    view.ping = player.scorePing.ping;
    view.name = player.name;
    out.push_back(std::move(view));
  }
  return out;
}

std::vector<RemoteVehicleView> WorldState::renderableVehicles(size_t maxVehicles) const {
  constexpr size_t kHardRenderVehicleLimit = 64;
  const size_t limit = std::min(maxVehicles, kHardRenderVehicleLimit);
  std::vector<RemoteVehicleView> out;
  out.reserve(std::min(limit, vehicles_.size()));
  // std::map keeps exported vehicle IDs stable/ascending across frames.
  for (const auto& [id, vehicle] : vehicles_) {
    if (!vehicle.streamedIn || out.size() >= limit) continue;
    RemoteVehicleView view;
    view.id = id;
    view.dynamicSyncKnown = vehicle.dynamicSyncKnown;
    view.driverKnown = vehicle.driverKnown;
    view.driverId = vehicle.driverId;
    view.model = vehicle.model;
    view.colour1 = vehicle.colour1;
    view.colour2 = vehicle.colour2;
    view.paintjob = vehicle.paintjob;
    view.x = vehicle.x;
    view.y = vehicle.y;
    view.z = vehicle.z;
    view.angle = vehicle.angle;
    view.attitudeKnown = vehicle.attitudeKnown;
    view.rotationX = vehicle.rotationX;
    view.rotationY = vehicle.rotationY;
    view.rotationZ = vehicle.rotationZ;
    view.parameters = vehicle.parameters;
    view.health = vehicle.health;
    out.push_back(view);
  }
  return out;
}

const Vehicle* WorldState::findVehicle(uint16_t id) const {
  const auto found = vehicles_.find(id);
  return found != vehicles_.end() && found->second.streamedIn ? &found->second : nullptr;
}

const Vehicle* WorldState::localVehiclePresentationAnchor() const {
  if (!localPlayer_.vehicleKnown || !localPlayer_.inVehicle || localPlayer_.vehicleId == 0) {
    return nullptr;
  }
  return findVehicle(localPlayer_.vehicleId);
}

const Vehicle* WorldState::nearestVehicle(float x, float y, float z, float radius,
                                          uint8_t localInterior, bool interiorKnown) const {
  if (!validWorldPosition(x, y, z) || !std::isfinite(radius) || radius <= 0.0f ||
      radius > 16.0f) {
    return nullptr;
  }
  const float maximumDistanceSquared = radius * radius;
  const Vehicle* nearest = nullptr;
  float nearestDistanceSquared = maximumDistanceSquared;
  for (const auto& [id, vehicle] : vehicles_) {
    (void)id;
    if (!vehicle.streamedIn || !validWorldPosition(vehicle.x, vehicle.y, vehicle.z) ||
        (interiorKnown && vehicle.interior != localInterior)) {
      continue;
    }
    const float dx = vehicle.x - x;
    const float dy = vehicle.y - y;
    const float dz = vehicle.z - z;
    const float distanceSquared = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(distanceSquared) || distanceSquared > nearestDistanceSquared) continue;
    if (nearest == nullptr || distanceSquared < nearestDistanceSquared || vehicle.id < nearest->id) {
      nearest = &vehicle;
      nearestDistanceSquared = distanceSquared;
    }
  }
  return nearest;
}

bool WorldState::applyRemoteOnFoot(const syncdecode::RemoteOnFootSync& sync, RpcEvent& event) {
  event = {};
  const double quaternionLengthSquared =
      static_cast<double>(sync.quaternionW) * sync.quaternionW +
      static_cast<double>(sync.quaternionX) * sync.quaternionX +
      static_cast<double>(sync.quaternionY) * sync.quaternionY +
      static_cast<double>(sync.quaternionZ) * sync.quaternionZ;
  if (!validWorldPosition(sync.positionX, sync.positionY, sync.positionZ) ||
      !validVelocity(sync.velocityX, sync.velocityY, sync.velocityZ) ||
      !std::isfinite(quaternionLengthSquared) || quaternionLengthSquared < 0.98 ||
      quaternionLengthSquared > 1.02 ||
      sync.health < 0.0f || sync.health > 100.0f || sync.armour < 0.0f || sync.armour > 100.0f) {
    return false;
  }
  // The server broadcasts only other players. Never let a packet claiming our
  // own ID overwrite authoritative local-player reducer state.
  if (initialized_ && sync.playerId == playerInit_.playerId) return false;
  auto it = players_.find(sync.playerId);
  // StreamIn/Join establishes entity identity first. Ignoring orphan sync
  // frames prevents a UDP peer from filling the bounded player model with IDs.
  if (it == players_.end() || !it->second.streamedIn) return false;

  Player& player = it->second;
  // A fresh Packet-207 is authoritative on-foot state. It replaces either a
  // Packet-200 driver or Packet-211 passenger attachment without inventing a
  // departure position; this packet itself supplies the usable transform.
  clearRemoteVehicleOccupancy(sync.playerId, false);
  player.positionKnown = true;
  player.remoteOnFootKnown = true;
  player.x = sync.positionX;
  player.y = sync.positionY;
  player.z = sync.positionZ;
  player.quaternionW = sync.quaternionW;
  player.quaternionX = sync.quaternionX;
  player.quaternionY = sync.quaternionY;
  player.quaternionZ = sync.quaternionZ;
  // Keep the renderer-facing yaw fresh as well as the raw quaternion. This is
  // also the only available parent orientation for AttachObjectToPlayer; pitch
  // and roll never become fabricated full-ped attachment transforms.
  const float yawRadians = std::atan2(
      2.0f * (sync.quaternionW * sync.quaternionZ + sync.quaternionX * sync.quaternionY),
      1.0f - 2.0f * (sync.quaternionY * sync.quaternionY + sync.quaternionZ * sync.quaternionZ));
  player.angle = normalizedAngle(-yawRadians * 180.0f / 3.14159265358979323846f);
  player.health = sync.health;
  player.armour = sync.armour;
  player.weaponAdditionalKey = sync.weaponAdditionalKey;
  player.specialAction = sync.specialAction;
  player.velocityX = sync.velocityX;
  player.velocityY = sync.velocityY;
  player.velocityZ = sync.velocityZ;
  player.animationKnown = sync.animationKnown;
  player.animationId = sync.animationId;
  player.animationFlags = sync.animationFlags;
  setEvent(event, RpcEventKind::RemotePlayerSync, sync.playerId, "PlayerFootSync");
  return true;
}

bool WorldState::applyRemoteAim(const syncdecode::RemoteAimSync& sync, RpcEvent& event) {
  event = {};
  // Revalidate here as well as at the packet decoder: this reducer is a public
  // state boundary used by host tests and must not trust a caller to decode.
  if (!validRemoteAim(sync) || (initialized_ && sync.playerId == playerInit_.playerId)) {
    return false;
  }
  const auto it = players_.find(sync.playerId);
  // Packet 203 is only an update for an existing streamed actor. It must not
  // allocate an entity or overwrite local-player/vehicle authority by itself.
  if (it == players_.end() || !it->second.streamedIn) return false;

  Player& player = it->second;
  player.remoteAimKnown = true;
  player.remoteAimUpdatedMs = monotonicMilliseconds();
  player.aimCameraMode = sync.cameraMode;
  player.aimWeaponState = sync.weaponState;
  player.aimCameraFrontX = sync.cameraFrontX;
  player.aimCameraFrontY = sync.cameraFrontY;
  player.aimCameraFrontZ = sync.cameraFrontZ;
  player.aimCameraPositionX = sync.cameraPositionX;
  player.aimCameraPositionY = sync.cameraPositionY;
  player.aimCameraPositionZ = sync.cameraPositionZ;
  setEvent(event, RpcEventKind::RemoteAimSync, sync.playerId, "PlayerAimSync");
  return true;
}

bool WorldState::applyRemoteBullet(const syncdecode::RemoteBulletSync& sync, RpcEvent& event) {
  event = {};
  // This remote Packet-206 reducer is receive-only. Revalidate before retaining
  // only a short-lived direction cue; no hit ID/type, target, vital, ammo, or damage state is
  // applied to this Android client.
  if (!validRemoteBullet(sync) || (initialized_ && sync.playerId == playerInit_.playerId)) {
    return false;
  }
  const auto it = players_.find(sync.playerId);
  if (it == players_.end() || !it->second.streamedIn) return false;

  Player& player = it->second;
  // HitType=None may legitimately carry the all-zero no-impact sentinel. It
  // reports a server-relayed firing action but provides no drawable direction;
  // do not turn the world origin into a fabricated endpoint/aim line.
  const bool noImpactSentinel = sync.hitType == 0u && sync.hitPositionX == 0.0f &&
      sync.hitPositionY == 0.0f && sync.hitPositionZ == 0.0f;
  player.remoteBulletKnown = !noImpactSentinel;
  player.remoteBulletUpdatedMs = monotonicMilliseconds();
  if (!noImpactSentinel) {
    player.bulletWeaponId = sync.weaponId;
    player.bulletOriginX = sync.originX;
    player.bulletOriginY = sync.originY;
    player.bulletOriginZ = sync.originZ;
    // Preserve only an endpoint delta for a short camera-space cue. A zero
    // delta remains harmless and is never replaced with a guessed target.
    player.bulletDirectionX = sync.hitPositionX - sync.originX;
    player.bulletDirectionY = sync.hitPositionY - sync.originY;
    player.bulletDirectionZ = sync.hitPositionZ - sync.originZ;
  }
  setEvent(event, RpcEventKind::RemoteBulletSync, sync.playerId,
           noImpactSentinel ? "PlayerBulletSync no-impact" : "PlayerBulletSync");
  return true;
}

bool WorldState::applyRemoteVehicle(const syncdecode::RemoteVehicleSync& sync, RpcEvent& event) {
  event = {};
  if (sync.vehicleId == 0 || !validWorldPosition(sync.x, sync.y, sync.z) ||
      !validVelocity(sync.velocityX, sync.velocityY, sync.velocityZ) ||
      !std::isfinite(sync.health) || sync.health < 0.0f || sync.health > 65535.0f ||
      (sync.angleKnown && !std::isfinite(sync.angle)) ||
      (sync.attitudeKnown &&
       (!std::isfinite(sync.rotationX) || !std::isfinite(sync.rotationY) ||
        !std::isfinite(sync.rotationZ) || std::fabs(sync.rotationX) > 180.001f ||
        std::fabs(sync.rotationY) > 90.001f || std::fabs(sync.rotationZ) > 180.001f))) {
    return false;
  }
  // Vehicle sync is only a transform update. It never creates an entity: a
  // valid StreamInVehicle lifecycle record must exist first.
  const auto it = vehicles_.find(sync.vehicleId);
  if (it == vehicles_.end() || !it->second.streamedIn) return false;
  Vehicle& vehicle = it->second;

  // Packet-209 means the driver slot is empty; Packet-200 replaces only that
  // slot. Packet-211 passengers remain attached to the same vehicle, because
  // neither packet proves that they exited. This avoids one driver update
  // making every passenger ped reappear through the vehicle.
  const auto detachCurrentDriver = [&]() {
    if (!vehicle.driverKnown) return;
    const uint16_t previousDriver = vehicle.driverId;
    vehicle.driverKnown = false;
    vehicle.driverId = 0;
    const auto player = players_.find(previousDriver);
    if (player != players_.end() && player->second.remoteVehicleKnown &&
        player->second.remoteVehicleDriver &&
        player->second.remoteVehicleId == sync.vehicleId) {
      Player& actor = player->second;
      actor.remoteVehicleKnown = false;
      actor.remoteVehicleDriver = false;
      actor.remoteVehicleId = 0;
      actor.remoteVehicleSeat = 0;
      // Do not revive an old Packet-207 transform after the server replaced
      // this driver's seat state. A newer packet must supply a foot position.
      actor.positionKnown = false;
      actor.remoteOnFootKnown = false;
    }
  };

  if (sync.driverSync) {
    // A player cannot remain a passenger/driver in an earlier vehicle. Clear
    // their exact prior occupancy before assigning this Packet-200 seat.
    clearRemoteVehicleOccupancy(sync.playerId, true);
    // `clearRemoteVehicleOccupancy` may have cleared this vehicle when the
    // same driver was already attached; only a distinct stale driver remains.
    detachCurrentDriver();
    vehicle.driverKnown = true;
    vehicle.driverId = sync.playerId;
  } else {
    detachCurrentDriver();
  }

  vehicle.dynamicSyncKnown = true;
  vehicle.x = sync.x;
  vehicle.y = sync.y;
  vehicle.z = sync.z;
  if (sync.angleKnown) vehicle.angle = normalizedAngle(sync.angle);
  // Packet-200 has an exact compressed quaternion; consume its source-derived
  // full DFF attitude only for a driver sync. Packet-209 deliberately retains
  // the last attitude because its raw direction vectors are not an Euler tuple.
  if (sync.driverSync) {
    vehicle.attitudeKnown = sync.attitudeKnown;
    if (sync.attitudeKnown) {
      vehicle.rotationX = sync.rotationX;
      vehicle.rotationY = sync.rotationY;
      vehicle.rotationZ = sync.rotationZ;
    } else {
      vehicle.rotationX = 0.0f;
      vehicle.rotationY = 0.0f;
      vehicle.rotationZ = vehicle.angle;
    }
  }
  vehicle.health = sync.health;
  if (sync.driverSync) {
    const auto player = players_.find(sync.playerId);
    if (player != players_.end() && player->second.streamedIn) {
      Player& driver = player->second;
      driver.remoteVehicleKnown = true;
      driver.remoteVehicleDriver = true;
      driver.remoteVehicleId = sync.vehicleId;
      driver.remoteVehicleSeat = 0;
      driver.remoteOnFootKnown = false;
      driver.positionKnown = true;
      driver.x = vehicle.x;
      driver.y = vehicle.y;
      driver.z = vehicle.z;
      driver.angle = vehicle.angle;
    }
  }
  setEvent(event, RpcEventKind::RemoteVehicleSync, sync.vehicleId,
           sync.driverSync ? "PlayerVehicleSync" : "PlayerUnoccupiedSync");
  return true;
}

bool WorldState::applyRemoteTrailer(const syncdecode::RemoteTrailerSync& sync, RpcEvent& event) {
  event = {};
  if (sync.vehicleId == 0 || !sync.attitudeKnown ||
      !validWorldPosition(sync.x, sync.y, sync.z) ||
      !validVelocity(sync.velocityX, sync.velocityY, sync.velocityZ) ||
      !std::isfinite(sync.turnVelocityX) || !std::isfinite(sync.turnVelocityY) ||
      !std::isfinite(sync.turnVelocityZ) || sync.turnVelocityX < -1.0f ||
      sync.turnVelocityX > 1.0f || sync.turnVelocityY < -1.0f ||
      sync.turnVelocityY > 1.0f || sync.turnVelocityZ < -1.0f ||
      sync.turnVelocityZ > 1.0f || !std::isfinite(sync.rotationX) ||
      !std::isfinite(sync.rotationY) || !std::isfinite(sync.rotationZ) ||
      std::fabs(sync.rotationX) > 180.001f || std::fabs(sync.rotationY) > 90.001f ||
      std::fabs(sync.rotationZ) > 180.001f) {
    return false;
  }
  // Packet 210 is a transform for a trailer that already exists in the
  // recipient stream. It has no cab relationship, health, seat, or local
  // authority, so preserve all of those retained facts rather than inferring
  // occupancy or a physical attachment from the packet alone.
  const auto it = vehicles_.find(sync.vehicleId);
  if (it == vehicles_.end() || !it->second.streamedIn) return false;
  Vehicle& trailer = it->second;
  trailer.dynamicSyncKnown = true;
  trailer.x = sync.x;
  trailer.y = sync.y;
  trailer.z = sync.z;
  trailer.attitudeKnown = true;
  trailer.rotationX = sync.rotationX;
  trailer.rotationY = sync.rotationY;
  trailer.rotationZ = sync.rotationZ;
  trailer.angle = normalizedAngle(sync.rotationZ);
  setEvent(event, RpcEventKind::RemoteTrailerSync, sync.vehicleId, "PlayerTrailerSync");
  return true;
}

bool WorldState::applyRemotePassenger(const syncdecode::RemotePassengerSync& sync, RpcEvent& event) {
  event = {};
  if (sync.vehicleId == 0 || sync.health < 0.0f || sync.health > 100.0f ||
      sync.armour < 0.0f || sync.armour > 100.0f ||
      !validWorldPosition(sync.x, sync.y, sync.z)) {
    return false;
  }
  if (initialized_ && sync.playerId == playerInit_.playerId) return false;
  const auto vehicle = vehicles_.find(sync.vehicleId);
  const auto player = players_.find(sync.playerId);
  // Both lifecycles must exist. A UDP packet cannot allocate a player or
  // vehicle solely by claiming a passenger relation.
  if (vehicle == vehicles_.end() || !vehicle->second.streamedIn ||
      player == players_.end() || !player->second.streamedIn) {
    return false;
  }

  // Packet 211 carries an explicit world position, so retain it as the anchor
  // rather than guessing a seat offset from a vehicle model. It supersedes any
  // older foot/driver/passenger relation for this one actor only.
  clearRemoteVehicleOccupancy(sync.playerId, true);
  Player& passenger = player->second;
  passenger.remoteVehicleKnown = true;
  passenger.remoteVehicleDriver = false;
  passenger.remoteVehicleId = sync.vehicleId;
  passenger.remoteVehicleSeat = sync.seatId;
  passenger.remoteOnFootKnown = false;
  passenger.positionKnown = true;
  passenger.x = sync.x;
  passenger.y = sync.y;
  passenger.z = sync.z;
  passenger.health = sync.health;
  passenger.armour = sync.armour;
  passenger.weaponAdditionalKey = sync.weaponAdditionalKey;
  passenger.animationKnown = false;
  setEvent(event, RpcEventKind::RemotePassengerSync, sync.playerId, "PlayerPassengerSync");
  return true;
}

void WorldState::beginClassRequest() {
  // Do not let the Spawn button reuse a selectable reply from an earlier class
  // while a new PlayerRequestClass is in flight. Position/spawn facts remain
  // server-owned and are deliberately not cleared by this UI/protocol request.
  classSelection_ = {};
  classSelectionForced_ = false;
  beginSpawnRequest();
}

void WorldState::beginSpawnRequest() {
  spawnResponseKnown_ = false;
  spawnAllowed_ = false;
}

bool WorldState::acknowledgeDialogResponse(int16_t id, uint64_t revision) {
  if (!dialog_.active || dialog_.id != id || dialog_.revision != revision) return false;
  dialog_.active = false;
  dialog_.id = -1;
  dialog_.style = 0;
  dialog_.title.clear();
  dialog_.button1.clear();
  dialog_.button2.clear();
  dialog_.body.clear();
  advanceDialogRevision(dialog_);
  return true;
}

bool WorldState::acknowledgeMenuSelection(uint8_t id, uint64_t revision, uint8_t row) {
  const uint8_t rowCount = std::max(activeMenu_.column1Count, activeMenu_.column2Count);
  if (!activeMenu_.active || activeMenu_.id != id || activeMenu_.revision != revision ||
      !activeMenu_.enabled || row >= rowCount || !activeMenu_.rowEnabled[row]) {
    return false;
  }
  activeMenu_.active = false;
  advanceMenuRevision(activeMenu_);
  return true;
}

bool WorldState::acknowledgeMenuExit(uint8_t id, uint64_t revision) {
  if (!activeMenu_.active || activeMenu_.id != id || activeMenu_.revision != revision) return false;
  activeMenu_.active = false;
  advanceMenuRevision(activeMenu_);
  return true;
}

bool WorldState::applyRpc(const std::vector<uint8_t>& rpcPayload, RpcEvent& event) {
  event = {};
  if (rpcPayload.size() < 2 || rpcPayload[0] != kIdRpc) return false;

  const uint8_t rpcId = rpcPayload[1];
  if (rpcId == kPlayerInit) {
    rpc::PlayerInitData init;
    if (!rpc::decodePlayerInit(rpcPayload, init)) {
      event.kind = RpcEventKind::Malformed;
      event.summary = "malformed PlayerInit";
      return false;
    }
    playerInit_ = std::move(init);
    const auto localScore = scorePingOverrides_.find(playerInit_.playerId);
    if (localScore != scorePingOverrides_.end()) {
      localPlayer_.scorePing = localScore->second;
      scorePingOverrides_.erase(localScore);
    }
    environment_.worldTime = playerInit_.worldTime;
    environment_.weather = playerInit_.weather;
    // PlayerInit starts a fresh recipient visual baseline. Do not leak a
    // recipient-specific clock override/hidden flag from a prior lifecycle.
    environment_.playerTimeKnown = false;
    environment_.playerHour = playerInit_.worldTime;
    environment_.playerMinute = 0;
    environment_.playerClockKnown = false;
    environment_.playerClockEnabled = true;
    // PlayerInit belongs to a fresh session baseline. Any prior stream from a
    // reconnect must stop rather than survive against a new server state.
    audioStream_ = {};
    environment_.timeKnown = true;
    environment_.weatherKnown = true;
    environment_.gravityKnown = validServerGravity(playerInit_.gravity);
    if (environment_.gravityKnown) environment_.gravity = playerInit_.gravity;
    initialized_ = true;
    setEvent(event, RpcEventKind::PlayerInitialized, playerInit_.playerId, "PlayerInit");
    event.worldGravityUpdated = environment_.gravityKnown;
    return true;
  }

  raknet::BitReader reader(rpcPayload.data() + 2, rpcPayload.size() - 2);
  switch (rpcId) {
    // Current open.mp PlayerBeginObjectSelect is bodyless. Its paired client
    // RPC has the same numeric ID but the opposite direction and carries the
    // model/position selection payload; no inbound Android payload is parsed
    // here. Selection remains server-owned until its cancel/edit lifecycle.
    case kPlayerBeginObjectSelect:
      if (rpcPayload.size() - 2u != 0u) break;
      objectSelectionActive_ = true;
      advanceObjectSelectionRevision(objectSelectionRevision_);
      if (objectEdit_.active) {
        objectEdit_.active = false;
        objectEdit_.playerObject = false;
        objectEdit_.objectId = 0xFFFFu;
        advanceObjectEditRevision(objectEdit_);
      }
      if (attachedObjectEdit_.active) {
        attachedObjectEdit_.active = false;
        advanceAttachedObjectEditRevision(attachedObjectEdit_);
      }
      setEvent(event, RpcEventKind::ObjectSelectionChanged, 0, "PlayerBeginObjectSelect");
      return true;

    // Current open.mp writer uses one u32 attachment slot. It starts a
    // distinct edit mode from RPC 117 ObjectEdit, so Java cannot reinterpret a
    // player attachment index as a global object ID.
    case kPlayerBeginAttachedObjectEdit: {
      if (rpcPayload.size() - 2u != 4u) break;
      const uint32_t rawSlot = reader.readBits(32);
      if (!reader.ok() || rawSlot >= kMaxPlayerAttachedObjectSlots) break;
      AttachedObjectEditState next;
      next.active = true;
      next.slot = static_cast<uint8_t>(rawSlot);
      next.revision = attachedObjectEdit_.revision;
      advanceAttachedObjectEditRevision(next);
      attachedObjectEdit_ = next;
      if (objectSelectionActive_) {
        objectSelectionActive_ = false;
        advanceObjectSelectionRevision(objectSelectionRevision_);
      }
      if (objectEdit_.active) {
        objectEdit_.active = false;
        objectEdit_.playerObject = false;
        objectEdit_.objectId = 0xFFFFu;
        advanceObjectEditRevision(objectEdit_);
      }
      setEvent(event, RpcEventKind::AttachedObjectEditChanged, 0,
               "PlayerBeginAttachedObjectEdit");
      return true;
    }

    // Current open.mp writer: BIT PlayerObject then u16 ObjectID. The physical
    // body has seven final transport-padding bits; only 17 bits carry state.
    // Do not require the object to already be renderable here: a later sender
    // does that under the session mutex, avoiding a stale Android edit target.
    case kPlayerBeginObjectEdit: {
      if (rpcPayload.size() - 2u != 3u) break;
      const bool playerObject = reader.readBit();
      const uint16_t objectId = static_cast<uint16_t>(reader.readBits(16));
      const size_t bodyBits = (rpcPayload.size() - 2u) * 8u;
      if (!reader.ok() || objectId == 0u || objectId == 0xFFFFu ||
          reader.bitPosition() > bodyBits || bodyBits - reader.bitPosition() > 7u) {
        break;
      }
      ObjectEditState next;
      next.active = true;
      next.playerObject = playerObject;
      next.objectId = objectId;
      next.revision = objectEdit_.revision;
      advanceObjectEditRevision(next);
      objectEdit_ = next;
      if (objectSelectionActive_) {
        objectSelectionActive_ = false;
        advanceObjectSelectionRevision(objectSelectionRevision_);
      }
      if (attachedObjectEdit_.active) {
        attachedObjectEdit_.active = false;
        advanceAttachedObjectEditRevision(attachedObjectEdit_);
      }
      setEvent(event, RpcEventKind::ObjectEditChanged, objectId,
               playerObject ? "PlayerBeginObjectEdit player" : "PlayerBeginObjectEdit global");
      return true;
    }

    // `PlayerCancelObjectEdit` is bodyless in the current writer. It ends both
    // current object selection and current object editing for this recipient.
    case kPlayerCancelObjectEdit: {
      if (rpcPayload.size() - 2u != 0u) break;
      const bool hadObjectEdit = objectEdit_.active;
      const bool hadAttachedEdit = attachedObjectEdit_.active;
      objectSelectionActive_ = false;
      advanceObjectSelectionRevision(objectSelectionRevision_);
      objectEdit_.active = false;
      objectEdit_.playerObject = false;
      objectEdit_.objectId = 0xFFFFu;
      advanceObjectEditRevision(objectEdit_);
      attachedObjectEdit_.active = false;
      advanceAttachedObjectEditRevision(attachedObjectEdit_);
      const RpcEventKind kind = hadAttachedEdit ? RpcEventKind::AttachedObjectEditChanged :
                                (hadObjectEdit ? RpcEventKind::ObjectEditChanged :
                                                 RpcEventKind::ObjectSelectionChanged);
      setEvent(event, kind, 0, "PlayerCancelObjectEdit");
      return true;
    }

    // Money comes only from server deltas/reset. Android never invokes this
    // path for spending/rewards, and it does not imply an inventory system.
    case kGivePlayerMoney: {
      if (rpcPayload.size() - 2u != 4u) break;
      const uint32_t rawDelta = reader.readBits(32);
      if (!reader.ok()) break;
      int32_t delta = 0;
      static_assert(sizeof(delta) == sizeof(rawDelta), "legacy money delta is int32");
      std::memcpy(&delta, &rawDelta, sizeof(delta));
      localPlayer_.money = saturatingMoneyAdd(localPlayer_.money, delta);
      localPlayer_.moneyKnown = true;
      setEvent(event, RpcEventKind::MoneyChanged,
               initialized_ ? playerInit_.playerId : 0, "GivePlayerMoney");
      return true;
    }

    case kResetPlayerMoney:
      if (rpcPayload.size() - 2u != 0u) break;
      localPlayer_.money = 0;
      localPlayer_.moneyKnown = true;
      setEvent(event, RpcEventKind::MoneyChanged,
               initialized_ ? playerInit_.playerId : 0, "ResetPlayerMoney");
      return true;

    // Weapon RPCs establish authoritative equipment facts only. They do not
    // create hit events, ammo rewards, BulletSync, or a locally selectable UI.
    // Keeping raw weapon-ID ammo avoids inventing GTA slot replacement rules.
    case kResetPlayerWeapons: {
      if (rpcPayload.size() - 2u != 0u) break;
      localPlayer_.weaponAmmo.fill(0u);
      localPlayer_.weaponAmmoKnown.fill(false);
      localPlayer_.armedWeaponKnown = true;
      localPlayer_.armedWeapon = 0u;
      setEvent(event, RpcEventKind::WeaponStateChanged, 0, "ResetPlayerWeapons");
      return true;
    }

    case kGivePlayerWeapon: {
      if (rpcPayload.size() - 2u != 8u) break;
      const uint32_t weaponId = reader.readBits(32);
      const uint32_t ammo = reader.readBits(32);
      if (!reader.ok() || weaponId > kMaxLegacyWeaponId) break;
      localPlayer_.weaponAmmo[weaponId] = ammo;
      localPlayer_.weaponAmmoKnown[weaponId] = true;
      setEvent(event, RpcEventKind::WeaponStateChanged, static_cast<uint16_t>(weaponId),
               "GivePlayerWeapon");
      return true;
    }

    // Source-verified open.mp SetPlayerAmmo uses compact Weapon:u8 | Ammo:u16
    // (not the GivePlayerWeapon u32/u32 layout). It refreshes the authoritative
    // local ammo fact but deliberately never changes armed selection or causes
    // any client-side ammo consumption.
    case kSetPlayerAmmo: {
      if (rpcPayload.size() - 2u != 3u) break;
      const uint8_t weaponId = static_cast<uint8_t>(reader.readBits(8));
      const uint16_t ammo = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok() || weaponId > kMaxLegacyWeaponId) break;
      localPlayer_.weaponAmmo[weaponId] = ammo;
      localPlayer_.weaponAmmoKnown[weaponId] = true;
      setEvent(event, RpcEventKind::WeaponStateChanged, weaponId, "SetPlayerAmmo");
      return true;
    }

    case kSetPlayerArmedWeapon: {
      if (rpcPayload.size() - 2u != 4u) break;
      const uint32_t weaponId = reader.readBits(32);
      if (!reader.ok() || weaponId > kMaxLegacyWeaponId) break;
      localPlayer_.armedWeaponKnown = true;
      localPlayer_.armedWeapon = static_cast<uint8_t>(weaponId);
      setEvent(event, RpcEventKind::WeaponStateChanged, static_cast<uint16_t>(weaponId),
               "SetPlayerArmedWeapon");
      return true;
    }

    // PlayerStreamIn carries a DL-only CustomSkin:u32 directly after the
    // stock/base Skin:u32. Its presence is fixed by the profile selected at
    // ClientJoin, never guessed from a payload length that an untrusted peer
    // could manipulate.
    case kPlayerStreamIn: {
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      const uint8_t team = static_cast<uint8_t>(reader.readBits(8));
      const uint32_t model = reader.readBits(32);
      const uint32_t customModel = usesDlR1WireLayout() ? reader.readBits(32) : 0u;
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      const float angle = readFloat(reader, finite);
      const uint32_t rgba = reader.readBits(32);
      const uint8_t fightingStyle = static_cast<uint8_t>(reader.readBits(8));
      // open.mp SDK NUM_SKILL_LEVELS is 11; retain no skill data until the
      // renderer/game bridge needs it, but consume the exact wire fields.
      for (int i = 0; i < 11; ++i) reader.readBits(16);
      if (!reader.ok() || !finite || !validWorldPosition(x, y, z)) break;
      if (initialized_ && id == playerInit_.playerId) return false;
      // A stream-in transform is on-foot presentation state, so it supersedes
      // any stale Packet-200 driver or Packet-211 passenger attachment.
      clearRemoteVehicleOccupancy(id, false);
      Player* player = getOrCreate(id);
      if (!player) break;
      player->streamedIn = true;
      player->positionKnown = true;
      player->remoteOnFootKnown = false;
      // A fresh stream lifecycle must not reuse an old packet-203 camera pose.
      player->remoteAimKnown = false;
      player->remoteAimUpdatedMs = 0;
      player->remoteBulletKnown = false;
      player->remoteBulletUpdatedMs = 0;
      player->remoteWastedKnown = false;
      player->remoteWastedUpdatedMs = 0;
      // Forced animation has no independent stream persistence contract; a
      // fresh remote lifecycle must wait for a fresh server directive.
      clearAnimationDirective(player->forcedAnimation);
      player->remoteVehicleKnown = false;
      player->remoteVehicleDriver = false;
      player->remoteVehicleId = 0;
      player->remoteVehicleSeat = 0;
      player->team = team;
      player->model = model;
      player->customModel = customModel;
      player->x = x;
      player->y = y;
      player->z = z;
      player->angle = normalizedAngle(angle);
      player->rgba = rgba;
      player->fightingStyle = fightingStyle;
      setEvent(event, RpcEventKind::PlayerStreamedIn, id, "PlayerStreamIn");
      return true;
    }

    case kPlayerStreamOut: {
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok()) break;
      if (initialized_ && id == playerInit_.playerId) return false;
      const auto it = players_.find(id);
      if (it == players_.end()) return false;
      clearRemoteVehicleOccupancy(id, true);
      it->second.streamedIn = false;
      it->second.remoteOnFootKnown = false;
      it->second.remoteAimKnown = false;
      it->second.remoteAimUpdatedMs = 0;
      it->second.remoteBulletKnown = false;
      it->second.remoteBulletUpdatedMs = 0;
      it->second.remoteWastedKnown = false;
      it->second.remoteWastedUpdatedMs = 0;
      clearAnimationDirective(it->second.forcedAnimation);
      it->second.animationKnown = false;
      setEvent(event, RpcEventKind::PlayerStreamedOut, id, "PlayerStreamOut");
      return true;
    }

    // Current open.mp PlayerDeath (166) is a u16 remote player ID sent to
    // observers. It deliberately does *not* establish a local death/respawn
    // lifecycle; the normal server broadcast excludes the deceased peer.
    case kPlayerDeath: {
      if (rpcPayload.size() - 2u != 2u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok()) break;
      if (initialized_ && id == playerInit_.playerId) {
        event.summary = "PlayerDeath local ignored";
        return true;
      }
      const auto player = players_.find(id);
      if (player == players_.end() || !player->second.streamedIn || !player->second.positionKnown) {
        return false;
      }
      player->second.remoteWastedKnown = true;
      player->second.remoteWastedUpdatedMs = monotonicMilliseconds();
      setEvent(event, RpcEventKind::RemotePlayerSync, id, "PlayerDeath remote");
      return true;
    }

    // SetVehicleParams is recipient-local vehicle presentation/control state:
    // u16 ID followed by the exact sixteen signed-byte fields emitted by the
    // compatible writer. The payload is one full VehicleParams snapshot: `-1`
    // explicitly clears a field to unset rather than preserving an old fact or
    // being coerced to off.
    case kSetVehicleParams: {
      if (rpcPayload.size() - 2u != 18u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      std::array<int8_t, kVehicleParameterCount> incoming{};
      bool valid = reader.ok() && id != 0u;
      for (size_t index = 0; index < incoming.size(); ++index) {
        const uint8_t raw = static_cast<uint8_t>(reader.readBits(8));
        // Avoid relying on implementation-defined unsigned→signed conversion
        // for malformed high bytes: only legacy -1, 0, and 1 are legal.
        const int8_t value = raw == 0xFFu ? static_cast<int8_t>(-1)
                           : raw <= 1u   ? static_cast<int8_t>(raw)
                                          : static_cast<int8_t>(-2);
        incoming[index] = value;
        valid = valid && validVehicleParameterValue(value);
      }
      if (!reader.ok() || !valid) break;
      const auto vehicle = vehicles_.find(id);
      if (vehicle == vehicles_.end() || !vehicle->second.streamedIn) return false;
      if (applyVehicleParameters(incoming, vehicle->second.parameters)) {
        setEvent(event, RpcEventKind::VehicleChanged, id, "SetVehicleParams");
        event.vehicleParametersUpdated = true;
      }
      return true;
    }

    // StreamInVehicle establishes the authoritative initial transform plus
    // presentation facts. Packet 200/209 may later update the same bounded
    // proxy; consume the full legacy layout to keep the wire reducer aligned.
    case kStreamInVehicle: {
      constexpr int kVehicleComponentSlotsInRpc = 14;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      const uint32_t model = reader.readBits(32);
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      const float angle = readFloat(reader, finite);
      const uint8_t colour1 = static_cast<uint8_t>(reader.readBits(8));
      const uint8_t colour2 = static_cast<uint8_t>(reader.readBits(8));
      const float health = readFloat(reader, finite);
      const uint8_t interior = static_cast<uint8_t>(reader.readBits(8));
      reader.readBits(32);  // DoorDamage
      reader.readBits(32);  // PanelDamage
      reader.readBits(8);   // LightDamage
      reader.readBits(8);   // TyreDamage
      reader.readBits(8);   // Siren
      for (int slot = 0; slot < kVehicleComponentSlotsInRpc; ++slot) reader.readBits(8);
      const uint8_t paintjob = static_cast<uint8_t>(reader.readBits(8));
      reader.readBits(32);  // BodyColour1
      reader.readBits(32);  // BodyColour2
      if (!reader.ok() || !finite || id == 0 || model == 0 || model > 65535 ||
          !validWorldPosition(x, y, z) || health < 0.0f || health > 65535.0f) {
        break;
      }
      Vehicle* vehicle = getOrCreateVehicle(id);
      if (!vehicle) break;
      vehicle->streamedIn = true;
      vehicle->dynamicSyncKnown = false;
      vehicle->driverKnown = false;
      vehicle->driverId = 0;
      // Recipient-local RPC-24 and implicit RPC-91 state must not survive a
      // raw vehicle-ID reuse/stream lifecycle.
      clearVehicleParameters(*vehicle);
      clearVehicleVelocityDirectives(*vehicle);
      // Fresh stream-in has no occupancy record. Clear any stale driver or
      // passenger anchor retained from a prior lifecycle for this raw ID.
      for (auto& [playerId, player] : players_) {
        (void)playerId;
        if (player.remoteVehicleKnown && player.remoteVehicleId == id) {
          player.remoteVehicleKnown = false;
          player.remoteVehicleDriver = false;
          player.remoteVehicleId = 0;
          player.remoteVehicleSeat = 0;
          player.positionKnown = false;
          player.remoteOnFootKnown = false;
        }
      }
      vehicle->model = model;
      vehicle->colour1 = colour1;
      vehicle->colour2 = colour2;
      vehicle->interior = interior;
      vehicle->paintjob = paintjob;
      vehicle->x = x;
      vehicle->y = y;
      vehicle->z = z;
      vehicle->angle = normalizedAngle(angle);
      vehicle->attitudeKnown = false;
      vehicle->rotationX = 0.0f;
      vehicle->rotationY = 0.0f;
      vehicle->rotationZ = vehicle->angle;
      vehicle->health = health;
      setEvent(event, RpcEventKind::VehicleStreamedIn, id, "StreamInVehicle");
      return true;
    }

    case kStreamOutVehicle: {
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok() || id == 0) break;
      const auto it = vehicles_.find(id);
      if (it == vehicles_.end()) return false;
      it->second.streamedIn = false;
      it->second.dynamicSyncKnown = false;
      it->second.driverKnown = false;
      it->second.driverId = 0;
      clearVehicleParameters(it->second);
      clearVehicleVelocityDirectives(it->second);
      for (auto& [playerId, player] : players_) {
        (void)playerId;
        if (player.remoteVehicleKnown && player.remoteVehicleId == id) {
          player.remoteVehicleKnown = false;
          player.remoteVehicleDriver = false;
          player.remoteVehicleId = 0;
          player.remoteVehicleSeat = 0;
          player.positionKnown = false;
          player.remoteOnFootKnown = false;
        }
      }
      setEvent(event, RpcEventKind::VehicleStreamedOut, id, "StreamOutVehicle");
      return true;
    }

    // Exact vehicle-mutator RPCs from the open.mp-compatible writer. They can
    // only update an existing streamed proxy and never create a vehicle from a
    // forged single-field RPC.
    case kLinkVehicleToInterior: {
      if (rpcPayload.size() - 2u != 3u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      const uint8_t interior = static_cast<uint8_t>(reader.readBits(8));
      const auto vehicle = vehicles_.find(id);
      if (!reader.ok() || vehicle == vehicles_.end() || !vehicle->second.streamedIn) return false;
      vehicle->second.interior = interior;
      setEvent(event, RpcEventKind::VehicleChanged, id, "LinkVehicleToInterior");
      return true;
    }

    // SetVehicleVelocity is special: the source writer carries only Type and
    // a VEC3. It therefore applies solely to this recipient's currently seated
    // driver vehicle, never to a guessed ID, a passenger, an unoccupied proxy,
    // a missing StreamIn record, or a train. Both channels remain separate:
    // a later angular command must not erase a retained linear vector and vice
    // versa. This reducer intentionally retains the exact vector but does not
    // fabricate a full GTA vehicle/rotation physics response here.
    case kSetVehicleVelocity: {
      constexpr uint8_t kLinearVelocity = 0u;
      constexpr uint8_t kAngularVelocity = 1u;
      if (rpcPayload.size() - 2u != 13u) break;
      const uint8_t type = static_cast<uint8_t>(reader.readBits(8));
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      if (!reader.ok() || !finite || (type != kLinearVelocity && type != kAngularVelocity) ||
          !validVelocity(x, y, z)) {
        break;
      }
      const LocalPlayer& local = localPlayer_;
      if (!local.vehicleKnown || !local.inVehicle || local.vehicleSeat != 0u ||
          local.vehicleId == 0u) {
        return false;
      }
      const auto target = vehicles_.find(local.vehicleId);
      if (target == vehicles_.end() || !target->second.streamedIn ||
          isTrainVehicleModel(target->second.model)) {
        return false;
      }
      Vehicle& vehicle = target->second;
      if (type == kLinearVelocity) {
        vehicle.linearVelocityKnown = true;
        vehicle.linearVelocityX = x;
        vehicle.linearVelocityY = y;
        vehicle.linearVelocityZ = z;
        vehicle.linearVelocityRevision = nextDirectiveRevision(vehicle.linearVelocityRevision);
      } else {
        vehicle.angularVelocityKnown = true;
        vehicle.angularVelocityX = x;
        vehicle.angularVelocityY = y;
        vehicle.angularVelocityZ = z;
        vehicle.angularVelocityRevision = nextDirectiveRevision(vehicle.angularVelocityRevision);
      }
      setEvent(event, RpcEventKind::VehicleChanged, vehicle.id,
               type == kLinearVelocity ? "SetVehicleVelocity linear" :
                                         "SetVehicleVelocity angular");
      event.localVehicleVelocityUpdated = true;
      event.localVehicleVelocityAngular = type == kAngularVelocity;
      return true;
    }

    case kSetVehicleHealth: {
      if (rpcPayload.size() - 2u != 6u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      bool finite = true;
      const float health = readFloat(reader, finite);
      const auto vehicle = vehicles_.find(id);
      // Wire-invalid health is malformed. A syntactically valid mutator for an
      // absent/out-of-stream proxy is merely ignored so it cannot fabricate a
      // vehicle state or turn a harmless ordering race into protocol poison.
      if (!reader.ok() || !finite || health < 0.0f || health > 65535.0f) break;
      if (vehicle == vehicles_.end() || !vehicle->second.streamedIn) return false;
      vehicle->second.health = health;
      setEvent(event, RpcEventKind::VehicleChanged, id, "SetVehicleHealth");
      return true;
    }

    case kSetVehiclePosition: {
      if (rpcPayload.size() - 2u != 14u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      const auto vehicle = vehicles_.find(id);
      if (!reader.ok() || !finite || !validWorldPosition(x, y, z) ||
          vehicle == vehicles_.end() || !vehicle->second.streamedIn) {
        return false;
      }
      vehicle->second.x = x;
      vehicle->second.y = y;
      vehicle->second.z = z;
      vehicle->second.dynamicSyncKnown = false;
      setEvent(event, RpcEventKind::VehicleChanged, id, "SetVehiclePosition");
      return true;
    }

    case kSetVehicleZAngle: {
      if (rpcPayload.size() - 2u != 6u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      bool finite = true;
      const float angle = readFloat(reader, finite);
      const auto vehicle = vehicles_.find(id);
      if (!reader.ok() || !finite || std::fabs(angle) > 360000.0f ||
          vehicle == vehicles_.end() || !vehicle->second.streamedIn) {
        return false;
      }
      vehicle->second.angle = normalizedAngle(angle);
      // RPC-160 supplies Z only, so a previous Packet-200 roll/pitch must not
      // survive as if this mutator had confirmed it.
      vehicle->second.attitudeKnown = false;
      vehicle->second.rotationX = 0.0f;
      vehicle->second.rotationY = 0.0f;
      vehicle->second.rotationZ = vehicle->second.angle;
      vehicle->second.dynamicSyncKnown = false;
      setEvent(event, RpcEventKind::VehicleChanged, id, "SetVehicleZAngle");
      return true;
    }

    // Legacy SetVehicleParamsForPlayer writes the recipient's objective and
    // lock state directly: u16 VehicleID, u8 objective, u8 doorsLocked. It is
    // intentionally merged into the same retained presentation fields as RPC
    // 24, while leaving unrelated engine/lights/siren facts untouched.
    case kSetVehicleParamsForPlayer: {
      if (rpcPayload.size() - 2u != 4u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      const uint8_t objective = static_cast<uint8_t>(reader.readBits(8));
      const uint8_t doors = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || id == 0u || objective > 1u || doors > 1u) break;
      const auto vehicle = vehicles_.find(id);
      if (vehicle == vehicles_.end() || !vehicle->second.streamedIn) return false;
      if (applyVehicleParametersForPlayer(objective, doors, vehicle->second.parameters)) {
        setEvent(event, RpcEventKind::VehicleChanged, id, "SetVehicleParamsForPlayer");
        event.vehicleParametersUpdated = true;
      }
      return true;
    }

    // open.mp's DL ModelRequest (179) is a fixed 39-byte catalog entry. It
    // publishes a base model for a signed custom object ID before normal
    // streaming. Retain only that bounded mapping: checksums/sizes are consumed
    // to verify layout but never become a URL, filesystem path, download, or
    // completion signal. A model catalog does not grant asset-download consent.
    case kDlModelInfo: {
      constexpr size_t kDlModelInfoBodyBytes = 39u;
      constexpr uint8_t kDlModelTypeSkin = 1u;
      constexpr uint8_t kDlModelTypeObject = 2u;
      if (!usesDlR1WireLayout()) {
        event.kind = RpcEventKind::Ignored;
        event.summary = "DL ModelInfo ignored by legacy profile";
        return false;
      }
      if (rpcPayload.size() - 2u != kDlModelInfoBodyBytes) break;
      const uint32_t poolId = reader.readBits(32);
      const int32_t count = readInt32(reader);
      const uint8_t type = static_cast<uint8_t>(reader.readBits(8));
      (void)readInt32(reader);  // virtualWorld: server routing state, not local IPL authority.
      const int32_t baseId = readInt32(reader);
      const int32_t newId = readInt32(reader);
      reader.readBits(32);  // DFF checksum: not retained before a future downloader exists.
      reader.readBits(32);  // TXD checksum: same.
      reader.readBits(32);  // DFF size: same.
      reader.readBits(32);  // TXD size: same.
      reader.readBits(8);   // timeOn: no custom time-based artwork renderer yet.
      reader.readBits(8);   // timeOff.
      if (!reader.ok() || count <= 0 || count > static_cast<int32_t>(kMaxDlObjectFallbacks) ||
          poolId >= static_cast<uint32_t>(count) ||
          (type != kDlModelTypeSkin && type != kDlModelTypeObject) ||
          baseId <= 0 || baseId > 65535 ||
          (type == kDlModelTypeSkin && !validDlCustomSkinId(newId)) ||
          (type == kDlModelTypeObject && !validDlCustomObjectId(newId))) {
        break;
      }
      if (type == kDlModelTypeObject) {
        const auto existing = dlObjectBaseModels_.find(newId);
        if (existing == dlObjectBaseModels_.end() &&
            dlObjectBaseModels_.size() >= kMaxDlObjectFallbacks) {
          break;
        }
        const uint32_t baseModel = static_cast<uint32_t>(baseId);
        dlObjectBaseModels_[newId] = baseModel;
        // ModelInfo normally precedes CreateObject on its reliable ordered
        // stream, but repair already-retained unresolved custom proxies too.
        for (auto& [objectId, object] : objects_) {
          (void)objectId;
          if (object.sourceModel == newId) object.model = baseModel;
        }
        for (auto& [playerId, slots] : playerAttachedObjects_) {
          (void)playerId;
          for (PlayerAttachedObject& attachment : slots) {
            if (attachment.active && attachment.sourceModel == newId) {
              attachment.model = baseModel;
            }
          }
        }
      }
      event.kind = RpcEventKind::DlModelCatalogChanged;
      event.summary = type == kDlModelTypeObject
          ? "DL ModelInfo object base fallback"
          : "DL ModelInfo skin reference";
      return true;
    }

    // DL R1 extends ShowActorForPlayer with CustomSkin:u32 immediately after
    // SkinID. Retain the base model for lawful fallback presentation and the
    // source custom ID separately; no custom DFF/TXD is requested here.
    case kShowActor: {
      const size_t expectedBodyBytes = usesDlR1WireLayout() ? 31u : 27u;
      if (rpcPayload.size() - 2u != expectedBodyBytes) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      const uint32_t model = reader.readBits(32);
      const uint32_t customModel = usesDlR1WireLayout() ? reader.readBits(32) : 0u;
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      const float angle = readFloat(reader, finite);
      const float health = readFloat(reader, finite);
      const uint8_t invulnerable = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || !finite || model > 65535u || !validWorldPosition(x, y, z) ||
          std::fabs(angle) > 360000.0f || health < 0.0f || health > 65535.0f ||
          invulnerable > 1u) {
        break;
      }
      ServerActor* actor = getOrCreateServerActor(id);
      if (!actor) break;
      actor->model = model;
      actor->customModel = customModel;
      actor->x = x;
      actor->y = y;
      actor->z = z;
      actor->angle = normalizedAngle(angle);
      actor->health = health;
      actor->invulnerable = invulnerable != 0u;
      setEvent(event, RpcEventKind::ServerActorCreated, id, "ShowActor");
      return true;
    }

    case kHideActor: {
      if (rpcPayload.size() - 2u != 2u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok()) break;
      // A late duplicate hide is a valid idempotent lifecycle event.
      serverActors_.erase(id);
      setEvent(event, RpcEventKind::ServerActorDestroyed, id, "HideActor");
      return true;
    }

    // Source-verified ApplyActorAnimation layout: ActorID:u16, DynStr8
    // library/name, float delta, four packed bools, then u32 duration. Bit
    // packing leaves at most four zero transport-pad bits in the final byte.
    // The retained directive is deliberately presentation-only until the
    // user-owned GTA IFP/skeleton path can execute it faithfully.
    case kApplyActorAnimation: {
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok()) break;
      const auto actor = serverActors_.find(id);
      if (actor == serverActors_.end()) return false;
      AnimationDirective next = actor->second.forcedAnimation;
      if (!readAnimationDirective(reader, next)) break;
      const size_t bodyBits = (rpcPayload.size() - 2u) * 8u;
      if (reader.bitPosition() > bodyBits || bodyBits - reader.bitPosition() > 4u ||
          (reader.bitPosition() < bodyBits && reader.readBits(bodyBits - reader.bitPosition()) != 0u)) {
        break;
      }
      advanceAnimationRevision(next);
      actor->second.forcedAnimation = std::move(next);
      setEvent(event, RpcEventKind::ServerActorChanged, id, "ApplyActorAnimation");
      return true;
    }

    case kClearActorAnimation: {
      if (rpcPayload.size() - 2u != 2u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok()) break;
      const auto actor = serverActors_.find(id);
      if (actor == serverActors_.end()) return false;
      clearAnimationDirective(actor->second.forcedAnimation);
      setEvent(event, RpcEventKind::ServerActorChanged, id, "ClearActorAnimation");
      return true;
    }

    case kSetActorFacingAngle: {
      if (rpcPayload.size() - 2u != 6u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      bool finite = true;
      const float angle = readFloat(reader, finite);
      if (!reader.ok() || !finite || std::fabs(angle) > 360000.0f) break;
      const auto actor = serverActors_.find(id);
      if (actor == serverActors_.end()) return false;
      actor->second.angle = normalizedAngle(angle);
      setEvent(event, RpcEventKind::ServerActorChanged, id, "SetActorFacingAngle");
      return true;
    }

    case kSetActorPosition: {
      if (rpcPayload.size() - 2u != 14u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      if (!reader.ok() || !finite || !validWorldPosition(x, y, z)) break;
      const auto actor = serverActors_.find(id);
      if (actor == serverActors_.end()) return false;
      actor->second.x = x;
      actor->second.y = y;
      actor->second.z = z;
      setEvent(event, RpcEventKind::ServerActorChanged, id, "SetActorPos");
      return true;
    }

    case kSetActorHealth: {
      if (rpcPayload.size() - 2u != 6u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      bool finite = true;
      const float health = readFloat(reader, finite);
      if (!reader.ok() || !finite || health < 0.0f || health > 65535.0f) break;
      const auto actor = serverActors_.find(id);
      if (actor == serverActors_.end()) return false;
      actor->second.health = health;
      setEvent(event, RpcEventKind::ServerActorChanged, id, "SetActorHealth");
      return true;
    }

    // Current open.mp RPC 41 layout is DynStr8 URL, VEC3 position, float
    // distance, then u8 usepos. Retain a bounded directive only; C++ never
    // opens a network resource and Android filters to HTTPS at its media edge.
    case kPlayAudioStream: {
      std::string url = readString8(reader, kMaxAudioStreamUrlBytes);
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      const float distance = readFloat(reader, finite);
      const uint8_t usePosition = static_cast<uint8_t>(reader.readBits(8));
      const bool positional = usePosition != 0u;
      const bool positionalValid = !positional ||
          (validWorldPosition(x, y, z) && distance > 0.0f && distance <= kMaxAudioStreamDistance);
      const bool nonPositionalValid = positional ||
          (distance >= 0.0f && distance <= kMaxAudioStreamDistance);
      if (!reader.ok() || reader.bitPosition() != (rpcPayload.size() - 2u) * 8u || !finite ||
          usePosition > 1u || !validAudioStreamUrl(url) || !positionalValid || !nonPositionalValid) {
        break;
      }
      AudioStream next;
      next.active = true;
      next.revision = audioStream_.revision;
      advanceAudioStreamRevision(next);
      next.positional = positional;
      next.url = std::move(url);
      next.x = x;
      next.y = y;
      next.z = z;
      next.distance = distance;
      audioStream_ = std::move(next);
      event.kind = RpcEventKind::WorldEnvironmentChanged;
      event.summary = "PlayAudioStream";
      return true;
    }

    case kStopAudioStream: {
      if (rpcPayload.size() - 2u != 0u) break;
      AudioStream next;
      next.revision = audioStream_.revision;
      advanceAudioStreamRevision(next);
      audioStream_ = std::move(next);
      event.kind = RpcEventKind::WorldEnvironmentChanged;
      event.summary = "StopAudioStream";
      return true;
    }

    // RemoveBuildingForPlayer (RPC 43) applies only to static GTA map placement
    // data. Dynamic server objects remain a separate CreateObject lifecycle.
    // The legacy writer is exactly uint32 model + x/y/z/radius floats; -1 is
    // transported as UINT32_MAX and removes all stock models in the radius.
    case kRemoveBuilding: {
      if (rpcPayload.size() != 22u) break;
      gta::removal::Directive removal;
      removal.modelId = reader.readBits(32);
      bool finite = true;
      removal.x = readFloat(reader, finite);
      removal.y = readFloat(reader, finite);
      removal.z = readFloat(reader, finite);
      removal.radius = readFloat(reader, finite);
      if (!reader.ok() || !finite || !gta::removal::valid(removal)) break;
      const auto existing = std::find_if(
          buildingRemovals_.begin(), buildingRemovals_.end(),
          [&](const gta::removal::Directive& value) { return gta::removal::same(value, removal); });
      if (existing == buildingRemovals_.end()) {
        if (buildingRemovals_.size() >= gta::removal::kMaxDirectives) {
          // The record itself was valid but cannot be retained safely. Do not
          // reinterpret it, grow the session, or terminate a live connection.
          setEvent(event, RpcEventKind::Ignored,
                   initialized_ ? playerInit_.playerId : 0, "RemoveBuilding capped");
          return true;
        }
        buildingRemovals_.push_back(removal);
      }
      setEvent(event, RpcEventKind::BuildingRemovalChanged,
               initialized_ ? playerInit_.playerId : 0, "RemoveBuilding");
      return true;
    }

    // PlayerShowTextLabel writes a 1024-global + 1024-player wire ID,
    // numeric RGBA, static/attachment coordinates, and a default
    // StringCompressor body. Decode/compress-bound it before Android sees UI
    // text; attached labels retain only stable IDs, never raw entity pointers.
    case kPlayerShowTextLabel: {
      constexpr size_t kShowTextLabelFixedBytes = 27u;
      constexpr size_t kShowTextLabelMaxBytes = kShowTextLabelFixedBytes + 128u;
      const size_t bodyBytes = rpcPayload.size() - 2u;
      if (bodyBytes <= kShowTextLabelFixedBytes || bodyBytes > kShowTextLabelMaxBytes) break;
      TextLabel next;
      next.wireId = static_cast<uint16_t>(reader.readBits(16));
      next.playerTextLabel = next.wireId >= kTextLabelPoolSize;
      next.rgba = reader.readBits(32);
      bool finite = true;
      next.x = readFloat(reader, finite);
      next.y = readFloat(reader, finite);
      next.z = readFloat(reader, finite);
      next.drawDistance = readFloat(reader, finite);
      const uint8_t los = static_cast<uint8_t>(reader.readBits(8));
      next.playerAttachId = static_cast<uint16_t>(reader.readBits(16));
      next.vehicleAttachId = static_cast<uint16_t>(reader.readBits(16));
      std::string text;
      const bool textRead = raknet::stringcompressor::decodeDefaultString(reader, 64u, text);
      const size_t totalBits = bodyBytes * 8u;
      const size_t consumedBits = reader.bitPosition();
      if (!reader.ok() || !finite || !textRead || consumedBits > totalBits ||
          totalBits - consumedBits > 7u || next.wireId >= kTextLabelWireIdLimit || los > 1u ||
          !validWorldPosition(next.x, next.y, next.z) || !std::isfinite(next.drawDistance) ||
          next.drawDistance < 0.0f || next.drawDistance > 50000.0f) {
        break;
      }
      // NetworkBitStream::WriteCompressedStr includes a terminal NUL in some
      // compatible writers. It is transport framing, never visible UI text.
      const size_t terminal = text.find('\0');
      if (terminal != std::string::npos) text.resize(terminal);
      next.los = los != 0u;
      next.text = sanitizeUiText(text);
      const auto existing = textLabels_.find(next.wireId);
      if (existing == textLabels_.end() && textLabels_.size() >= kMaxTrackedTextLabels) break;
      const uint16_t wireId = next.wireId;
      textLabels_[wireId] = std::move(next);
      setEvent(event, RpcEventKind::TextLabelChanged, wireId, "PlayerShowTextLabel");
      return true;
    }

    case kPlayerHideTextLabel: {
      if (rpcPayload.size() - 2u != 2u) break;
      const uint16_t wireId = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok() || wireId >= kTextLabelWireIdLimit) break;
      // Per-recipient hide is idempotent. It never invents a label if a server
      // clears an already-hidden script label during lifecycle cleanup.
      textLabels_.erase(wireId);
      setEvent(event, RpcEventKind::TextLabelChanged, wireId, "PlayerHideTextLabel");
      return true;
    }

    // SetPlayerMapIcon owns one of the documented 100 per-player slots. The
    // raw GTA sprite Type/Style is retained for a portable marker renderer;
    // no fabricated sprite assets or gameplay waypoint behavior is inferred.
    case kSetPlayerMapIcon: {
      if (rpcPayload.size() - 2u != 19u) break;
      MapIcon next;
      next.id = static_cast<uint8_t>(reader.readBits(8));
      bool finite = true;
      next.x = readFloat(reader, finite);
      next.y = readFloat(reader, finite);
      next.z = readFloat(reader, finite);
      next.type = static_cast<uint8_t>(reader.readBits(8));
      next.rgba = reader.readBits(32);
      next.style = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || !finite || next.id >= kMapIconSlotLimit ||
          !validWorldPosition(next.x, next.y, next.z)) {
        break;
      }
      const auto existing = mapIcons_.find(next.id);
      if (existing == mapIcons_.end() && mapIcons_.size() >= kMaxTrackedMapIcons) break;
      const uint8_t id = next.id;
      mapIcons_[id] = next;
      setEvent(event, RpcEventKind::MapIconChanged, id, "SetPlayerMapIcon");
      return true;
    }

    case kRemovePlayerMapIcon: {
      if (rpcPayload.size() - 2u != 1u) break;
      const uint8_t id = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || id >= kMapIconSlotLimit) break;
      mapIcons_.erase(id);
      setEvent(event, RpcEventKind::MapIconChanged, id, "RemovePlayerMapIcon");
      return true;
    }

    // ShowGangZone / HideGangZone / FlashGangZone / StopFlashGangZone use a
    // bounded legacy ID space and ABGR colours. They are a per-recipient radar
    // surface, not world geometry or a local capture/gameplay simulation.
    case kShowGangZone: {
      constexpr size_t kShowGangZoneBodyBytes = 22u;
      if (rpcPayload.size() - 2u != kShowGangZoneBodyBytes) break;
      const int16_t rawId = static_cast<int16_t>(reader.readBits(16));
      bool finite = true;
      const float minX = readFloat(reader, finite);
      const float minY = readFloat(reader, finite);
      const float maxX = readFloat(reader, finite);
      const float maxY = readFloat(reader, finite);
      const uint32_t colourAbgr = reader.readBits(32);
      if (!reader.ok() || !finite || rawId < 0 || rawId >= static_cast<int16_t>(kGangZonePoolSize) ||
          !validGangZoneBounds(minX, minY, maxX, maxY)) {
        break;
      }
      const uint16_t id = static_cast<uint16_t>(rawId);
      GangZone zone;
      zone.id = id;
      zone.minX = minX;
      zone.minY = minY;
      zone.maxX = maxX;
      zone.maxY = maxY;
      zone.colourAbgr = colourAbgr;
      // Re-showing a zone replaces its visible state just as the server's
      // ShowGangZone writer does; a prior flash must not survive stale state.
      gangZones_[id] = zone;
      setEvent(event, RpcEventKind::GangZoneChanged, id, "ShowGangZone");
      return true;
    }

    case kHideGangZone: {
      if (rpcPayload.size() - 2u != 2u) break;
      const int16_t rawId = static_cast<int16_t>(reader.readBits(16));
      if (!reader.ok() || rawId < 0 || rawId >= static_cast<int16_t>(kGangZonePoolSize)) break;
      const uint16_t id = static_cast<uint16_t>(rawId);
      // The RPC is idempotent from the renderer's perspective. A server may
      // hide an already-hidden zone during script cleanup without malformed
      // bytes, so do not turn that harmless lifecycle race into a failure.
      gangZones_.erase(id);
      setEvent(event, RpcEventKind::GangZoneChanged, id, "HideGangZone");
      return true;
    }

    case kFlashGangZone: {
      if (rpcPayload.size() - 2u != 6u) break;
      const int16_t rawId = static_cast<int16_t>(reader.readBits(16));
      const uint32_t colourAbgr = reader.readBits(32);
      if (!reader.ok() || rawId < 0 || rawId >= static_cast<int16_t>(kGangZonePoolSize)) break;
      const uint16_t id = static_cast<uint16_t>(rawId);
      const auto zone = gangZones_.find(id);
      if (zone == gangZones_.end()) return false;
      zone->second.flashing = true;
      zone->second.flashColourAbgr = colourAbgr;
      setEvent(event, RpcEventKind::GangZoneChanged, id, "FlashGangZone");
      return true;
    }

    case kStopFlashGangZone: {
      if (rpcPayload.size() - 2u != 2u) break;
      const int16_t rawId = static_cast<int16_t>(reader.readBits(16));
      if (!reader.ok() || rawId < 0 || rawId >= static_cast<int16_t>(kGangZonePoolSize)) break;
      const uint16_t id = static_cast<uint16_t>(rawId);
      const auto zone = gangZones_.find(id);
      if (zone == gangZones_.end()) return false;
      zone->second.flashing = false;
      zone->second.flashColourAbgr = 0;
      setEvent(event, RpcEventKind::GangZoneChanged, id, "StopFlashGangZone");
      return true;
    }

    // SetPlayerCameraPosition/LookAt and InterpolateCamera are visual server
    // authority. They must not move the local collision/sync position; only
    // the renderer consumes this sampled camera boundary.
    case kSetPlayerCameraPosition: {
      if (rpcPayload.size() - 2u != 12u) break;
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      if (!reader.ok() || !finite || !validWorldPosition(x, y, z)) break;
      CameraEndpoint next;
      next.known = true;
      next.x = x;
      next.y = y;
      next.z = z;
      camera_.position = next;
      setEvent(event, RpcEventKind::CameraChanged, 0, "SetPlayerCameraPosition");
      return true;
    }

    case kSetPlayerCameraLookAt: {
      if (rpcPayload.size() - 2u != 13u) break;
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      reader.readBits(8);  // CutType is visual interpolation policy; consume it.
      if (!reader.ok() || !finite || !validWorldPosition(x, y, z)) break;
      CameraEndpoint next;
      next.known = true;
      next.x = x;
      next.y = y;
      next.z = z;
      camera_.lookAt = next;
      setEvent(event, RpcEventKind::CameraChanged, 0, "SetPlayerCameraLookAt");
      return true;
    }

    case kSetPlayerCameraBehindPlayer:
      if (rpcPayload.size() != 2u) break;
      camera_ = {};
      setEvent(event, RpcEventKind::CameraChanged, 0, "SetCameraBehindPlayer");
      return true;

    case kInterpolateCamera: {
      // One leading BIT leaves seven unused bits in the physical final byte.
      // The public writer therefore occupies 233 meaningful bits / 30 bytes.
      if (rpcPayload.size() - 2u != 30u) break;
      const bool position = reader.readBit();
      bool finite = true;
      const float fromX = readFloat(reader, finite);
      const float fromY = readFloat(reader, finite);
      const float fromZ = readFloat(reader, finite);
      const float toX = readFloat(reader, finite);
      const float toY = readFloat(reader, finite);
      const float toZ = readFloat(reader, finite);
      const uint32_t durationMs = reader.readBits(32);
      reader.readBits(8);  // Cut type is consumed but not emulated by GLES.
      if (!reader.ok() || !finite || durationMs > kMaxCameraInterpolationMs ||
          !validWorldPosition(fromX, fromY, fromZ) || !validWorldPosition(toX, toY, toZ)) {
        break;
      }
      CameraEndpoint next;
      next.known = true;
      next.fromX = fromX;
      next.fromY = fromY;
      next.fromZ = fromZ;
      next.toX = toX;
      next.toY = toY;
      next.toZ = toZ;
      next.durationMs = durationMs;
      next.interpolating = durationMs != 0u;
      next.startedAtMs = next.interpolating ? monotonicMilliseconds() : 0u;
      // A zero-time interpolation is an authoritative cut to the target.
      next.x = next.interpolating ? fromX : toX;
      next.y = next.interpolating ? fromY : toY;
      next.z = next.interpolating ? fromZ : toZ;
      if (position) {
        camera_.position = next;
      } else {
        camera_.lookAt = next;
      }
      setEvent(event, RpcEventKind::CameraChanged, 0,
               position ? "InterpolateCameraPosition" : "InterpolateCameraLookAt");
      return true;
    }

    // GameText is a timed GTA HUD primitive. Retain its wire-authoritative
    // text/style/time as a bounded portable banner, not as a claim of exact
    // GTA frontend typography or animation fidelity.
    // Current open.mp `PlayerInitMenu` writer serializes fixed StaticString<32>
    // fields plus u32 booleans, not legacy DynStr values. Decode exactly that
    // bounded surface before Android sees a copy; no menu geometry is treated
    // as player movement/UI hit authority.
    case kPlayerInitMenu: {
      Menu next;
      next.id = static_cast<uint8_t>(reader.readBits(8));
      const uint32_t twoColumns = reader.readBits(32);
      std::string title;
      if (!reader.ok() || next.id >= kMaxTrackedMenus || twoColumns > 1u ||
          !readFixedMenuText(reader, title)) break;
      bool finite = true;
      const float positionX = readFloat(reader, finite);
      const float positionY = readFloat(reader, finite);
      const float column1Width = readFloat(reader, finite);
      float column2Width = 0.0f;
      if (twoColumns != 0u) column2Width = readFloat(reader, finite);
      const uint32_t enabled = reader.readBits(32);
      if (!reader.ok() || !finite || enabled > 1u || !std::isfinite(positionX) ||
          !std::isfinite(positionY) || column1Width < 0.0f || column1Width > 10000.0f ||
          column2Width < 0.0f || column2Width > 10000.0f) {
        break;
      }
      next.twoColumns = twoColumns != 0u;
      next.enabled = enabled != 0u;
      for (bool &rowEnabled : next.rowEnabled) {
        const uint32_t raw = reader.readBits(32);
        if (!reader.ok() || raw > 1u) return false;
        rowEnabled = raw != 0u;
      }
      std::string column1Header;
      if (!readFixedMenuText(reader, column1Header)) break;
      next.column1Count = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || next.column1Count > kMaxMenuItems) break;
      for (uint8_t row = 0; row < next.column1Count; ++row) {
        std::string item;
        if (!readFixedMenuText(reader, item)) return false;
        next.column1Items[row] = sanitizeUiText(item);
      }
      if (next.twoColumns) {
        std::string column2Header;
        if (!readFixedMenuText(reader, column2Header)) break;
        next.column2Count = static_cast<uint8_t>(reader.readBits(8));
        if (!reader.ok() || next.column2Count > kMaxMenuItems) break;
        for (uint8_t row = 0; row < next.column2Count; ++row) {
          std::string item;
          if (!readFixedMenuText(reader, item)) return false;
          next.column2Items[row] = sanitizeUiText(item);
        }
        next.column2Header = sanitizeUiText(column2Header);
      }
      const size_t bodyBits = (rpcPayload.size() - 2u) * 8u;
      if (!reader.ok() || reader.bitPosition() != bodyBits) break;
      next.title = sanitizeUiText(title);
      next.column1Header = sanitizeUiText(column1Header);
      // A menu definition may be reinitialized while hidden. Retain it but do
      // not silently mutate a currently presented revision; a later RPC 77 is
      // the explicit server authorization to show the new definition.
      const auto existing = menus_.find(next.id);
      if (existing == menus_.end() && menus_.size() >= kMaxTrackedMenus) break;
      const uint8_t menuId = next.id;
      menus_[menuId] = std::move(next);
      // RpcEvent's numeric slot is generic; use it as a bounded menu ID here.
      setEvent(event, RpcEventKind::MenuChanged, menuId, "PlayerInitMenu");
      return true;
    }

    case kPlayerShowMenu: {
      if (rpcPayload.size() - 2u != 1u) break;
      const uint8_t id = static_cast<uint8_t>(reader.readBits(8));
      const auto found = menus_.find(id);
      if (!reader.ok() || id >= kMaxTrackedMenus || found == menus_.end()) return false;
      Menu next = found->second;
      next.active = true;
      next.revision = activeMenu_.revision;
      advanceMenuRevision(next);
      activeMenu_ = std::move(next);
      setEvent(event, RpcEventKind::MenuChanged, id, "PlayerShowMenu");
      return true;
    }

    case kPlayerHideMenu: {
      if (rpcPayload.size() - 2u != 1u) break;
      const uint8_t id = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || id >= kMaxTrackedMenus) break;
      if (activeMenu_.active && activeMenu_.id == id) {
        activeMenu_.active = false;
        advanceMenuRevision(activeMenu_);
      }
      setEvent(event, RpcEventKind::MenuChanged, id, "PlayerHideMenu");
      return true;
    }

    case kSendGameText: {
      constexpr size_t kGameTextMaxBytes = 64u;
      constexpr size_t kGameTextMinBodyBytes = 12u;
      constexpr size_t kGameTextMaxBodyBytes = kGameTextMinBodyBytes + kGameTextMaxBytes;
      constexpr uint32_t kMaxGameTextStyle = 255u;
      constexpr uint32_t kMaxGameTextDurationMs = 3'600'000u;
      const size_t bodyBytes = rpcPayload.size() - 2u;
      if (bodyBytes < kGameTextMinBodyBytes || bodyBytes > kGameTextMaxBodyBytes) break;
      GameText next;
      next.style = reader.readBits(32);
      next.durationMs = reader.readBits(32);
      std::string text;
      const bool textRead = readString32AllowEmpty(reader, kGameTextMaxBytes, text);
      if (!reader.ok() || !textRead || reader.bitPosition() != bodyBytes * 8u ||
          next.style > kMaxGameTextStyle || next.durationMs > kMaxGameTextDurationMs) {
        break;
      }
      next.text = sanitizeUiText(text);
      next.active = next.durationMs != 0u && !next.text.empty();
      next.startedAtMs = next.active ? monotonicMilliseconds() : 0u;
      next.revision = gameText_.revision + 1u;
      if (next.revision == 0u) next.revision = 1u;
      gameText_ = std::move(next);
      event.kind = RpcEventKind::GameTextChanged;
      event.summary = gameText_.active ? "SendGameText" : "HideGameText";
      return true;
    }

    // Textdraws are a separate screen-space protocol surface. Preserve the
    // exact bounded sprite/model preview fields too: Android may resolve only
    // user-owned TXD/DFF assets through its separately capped renderer path;
    // the reducer itself never guesses a frontend asset or mutates gameplay.
    case kShowTextDraw: {
      constexpr size_t kShowTextDrawFixedBytes = 65;
      constexpr size_t kShowTextDrawMinBytes = kShowTextDrawFixedBytes + 2u;
      constexpr size_t kShowTextDrawMaxBytes = kShowTextDrawMinBytes + 256u;
      const size_t bodyBytes = rpcPayload.size() - 2u;
      if (bodyBytes < kShowTextDrawMinBytes || bodyBytes > kShowTextDrawMaxBytes) break;
      TextDraw next;
      next.wireId = static_cast<uint16_t>(reader.readBits(16));
      const uint8_t flags = static_cast<uint8_t>(reader.readBits(8));
      next.playerTextDraw = next.wireId >= kTextDrawGlobalPoolSize;
      next.useBox = (flags & 0x01u) != 0;
      next.alignment = static_cast<uint8_t>((flags >> 1u) & 0x07u);
      next.proportional = (flags & 0x10u) != 0;
      bool finite = true;
      next.letterWidth = readFloat(reader, finite);
      next.letterHeight = readFloat(reader, finite);
      next.letterColourAbgr = reader.readBits(32);
      next.textWidth = readFloat(reader, finite);
      next.textHeight = readFloat(reader, finite);
      next.boxColourAbgr = reader.readBits(32);
      next.shadow = static_cast<uint8_t>(reader.readBits(8));
      next.outline = static_cast<uint8_t>(reader.readBits(8));
      next.backgroundColourAbgr = reader.readBits(32);
      next.style = static_cast<uint8_t>(reader.readBits(8));
      const uint8_t selectable = static_cast<uint8_t>(reader.readBits(8));
      next.selectable = selectable != 0;
      next.x = readFloat(reader, finite);
      next.y = readFloat(reader, finite);
      next.model = static_cast<uint16_t>(reader.readBits(16));
      next.rotationX = readFloat(reader, finite);
      next.rotationY = readFloat(reader, finite);
      next.rotationZ = readFloat(reader, finite);
      next.zoom = readFloat(reader, finite);
      next.colour1 = static_cast<int16_t>(reader.readBits(16));
      next.colour2 = static_cast<int16_t>(reader.readBits(16));
      std::string text;
      const bool textRead = readString16AllowEmpty(reader, 256u, text);
      if (!reader.ok() || !finite || !textRead || reader.bitPosition() != bodyBytes * 8u ||
          next.wireId >= kTextDrawWireIdLimit || next.alignment > 3u || next.style > 5u ||
          selectable > 1u ||
          next.letterWidth < 0.0f || next.letterWidth > 10.0f ||
          next.letterHeight < 0.0f || next.letterHeight > 10.0f ||
          next.textWidth < -128.0f || next.textWidth > 1024.0f ||
          next.textHeight < -128.0f || next.textHeight > 1024.0f ||
          next.x < -128.0f || next.x > 768.0f || next.y < -128.0f || next.y > 608.0f ||
          std::fabs(next.rotationX) > 360000.0f || std::fabs(next.rotationY) > 360000.0f ||
          std::fabs(next.rotationZ) > 360000.0f || next.zoom < 0.0f || next.zoom > 100.0f) {
        break;
      }
      const uint16_t wireId = next.wireId;
      const auto existing = textDraws_.find(wireId);
      if (existing == textDraws_.end() && textDraws_.size() >= kMaxTrackedTextDraws) break;
      next.text = sanitizeUiText(text);
      textDraws_[wireId] = std::move(next);
      event.kind = RpcEventKind::TextDrawChanged;
      event.summary = "ShowTextDraw #" + std::to_string(wireId);
      return true;
    }

    case kHideTextDraw: {
      if (rpcPayload.size() - 2u != 2u) break;
      const uint16_t wireId = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok() || wireId >= kTextDrawWireIdLimit) break;
      textDraws_.erase(wireId);
      event.kind = RpcEventKind::TextDrawChanged;
      event.summary = "HideTextDraw #" + std::to_string(wireId);
      return true;
    }

    case kTextDrawSetString: {
      constexpr size_t kTextDrawSetStringMinBytes = 4;
      constexpr size_t kTextDrawSetStringMaxBytes = kTextDrawSetStringMinBytes + 256u;
      const size_t bodyBytes = rpcPayload.size() - 2u;
      if (bodyBytes < kTextDrawSetStringMinBytes || bodyBytes > kTextDrawSetStringMaxBytes) break;
      const uint16_t wireId = static_cast<uint16_t>(reader.readBits(16));
      std::string text;
      const bool textRead = readString16AllowEmpty(reader, 256u, text);
      const auto found = textDraws_.find(wireId);
      if (!reader.ok() || !textRead || reader.bitPosition() != bodyBytes * 8u ||
          found == textDraws_.end()) {
        return false;
      }
      found->second.text = sanitizeUiText(text);
      event.kind = RpcEventKind::TextDrawChanged;
      event.summary = "TextDrawSetString #" + std::to_string(wireId);
      return true;
    }

    // Current open.mp writer (RPC 113): u16 PlayerID, u32 slot, one Create
    // bit, then model/bone/offset/rotation/scale/two ABGR colours when set.
    // Attachments are a separate per-player ten-slot namespace—not a global or
    // player-object lifecycle—and cannot create an unknown player identity.
    case kSetPlayerAttachedObject: {
      if (rpcPayload.size() - 2u < 7u) break;
      const uint16_t playerId = static_cast<uint16_t>(reader.readBits(16));
      const uint32_t rawSlot = reader.readBits(32);
      const bool create = reader.readBit();
      PlayerAttachedObject next;
      uint32_t rawBone = 0;
      bool finite = true;
      if (create) {
        const uint32_t rawModel = reader.readBits(32);
        std::memcpy(&next.sourceModel, &rawModel, sizeof(next.sourceModel));
        next.model = rawModel;
        if (usesDlR1WireLayout() && next.sourceModel < 0) {
          const auto fallback = dlObjectBaseModels_.find(next.sourceModel);
          next.model = fallback != dlObjectBaseModels_.end() ? fallback->second : 0u;
        }
        rawBone = reader.readBits(32);
        next.bone = static_cast<uint8_t>(rawBone);
        next.offsetX = readFloat(reader, finite);
        next.offsetY = readFloat(reader, finite);
        next.offsetZ = readFloat(reader, finite);
        next.rotationX = readFloat(reader, finite);
        next.rotationY = readFloat(reader, finite);
        next.rotationZ = readFloat(reader, finite);
        next.scaleX = readFloat(reader, finite);
        next.scaleY = readFloat(reader, finite);
        next.scaleZ = readFloat(reader, finite);
        next.colour1Abgr = reader.readBits(32);
        next.colour2Abgr = reader.readBits(32);
        next.active = true;
      }
      const size_t bodyBits = (rpcPayload.size() - 2u) * 8u;
      if (!reader.ok() || rawSlot >= kMaxPlayerAttachedObjectSlots ||
          reader.bitPosition() > bodyBits || bodyBits - reader.bitPosition() > 7u ||
          (create && (!finite ||
                      ((!usesDlR1WireLayout() || !validDlCustomObjectId(next.sourceModel)) &&
                       (next.model == 0u || next.model > 65535u)) ||
                      (usesDlR1WireLayout() && next.sourceModel < 0 &&
                       !validDlCustomObjectId(next.sourceModel)) ||
                      (next.model != 0u && next.model > 65535u) || rawBone > 18u ||
                      !validObjectAttachmentOffset(next.offsetX, next.offsetY, next.offsetZ) ||
                      !validObjectRotation(next.rotationX, next.rotationY, next.rotationZ) ||
                      !validPlayerAttachedObjectScale(next.scaleX, next.scaleY, next.scaleZ)))) {
        break;
      }
      const bool localTarget = initialized_ && playerId == playerInit_.playerId;
      if (!localTarget && players_.find(playerId) == players_.end()) return false;
      const size_t slot = static_cast<size_t>(rawSlot);
      if (create) {
        playerAttachedObjects_[playerId][slot] = std::move(next);
      } else {
        const auto found = playerAttachedObjects_.find(playerId);
        if (found != playerAttachedObjects_.end()) {
          found->second[slot] = {};
          const bool anyActive = std::any_of(found->second.begin(), found->second.end(),
                                             [](const PlayerAttachedObject& value) {
                                               return value.active;
                                             });
          if (!anyActive) playerAttachedObjects_.erase(found);
        }
      }
      // A server replacement changes the immutable slot facts (and can arrive
      // while an Android frame is open), so advance its revision before any
      // older Kotlin transform can reply. A removed slot closes the edit gate;
      // a replacement remains editable only from its new baseline.
      if (localTarget && attachedObjectEdit_.active &&
          attachedObjectEdit_.slot == static_cast<uint8_t>(slot)) {
        if (!create) attachedObjectEdit_.active = false;
        advanceAttachedObjectEditRevision(attachedObjectEdit_);
      }
      setEvent(event, RpcEventKind::PlayerAttachmentChanged, playerId,
               create ? "SetPlayerAttachedObject" : "RemovePlayerAttachedObject");
      return true;
    }

    case kTextDrawSelection: {
      // `writeBIT(enable)` followed by a 32-bit colour occupies 33 bits / five
      // bytes in the RPC body. Keep this exact cap so trailing junk cannot be
      // smuggled through an otherwise tiny selection state change.
      if (rpcPayload.size() - 2u != 5u) break;
      const bool enable = reader.readBit();
      const uint32_t colour = reader.readBits(32);
      if (!reader.ok()) break;
      textDrawSelectionActive_ = enable;
      textDrawSelectionColourRgba_ = colour;
      event.kind = RpcEventKind::TextDrawSelectionChanged;
      event.summary = enable ? "TextDrawSelection on" : "TextDrawSelection off";
      return true;
    }

    // ShowDialog carries three ordinary bounded strings and one default
    // RakNet-StringCompressor body. Decode it here, before it reaches Android,
    // so untrusted compressed bytes can never be rendered as UI text directly.
    case kShowDialog: {
      constexpr size_t kMaxDialogTitleBytes = 64;
      constexpr size_t kMaxDialogButtonBytes = 48;
      constexpr size_t kMaxDialogBodyBytes = 1024;
      const int16_t id = static_cast<int16_t>(reader.readBits(16));
      if (!reader.ok()) break;
      // Legacy servers use ShowPlayerDialog(..., -1, ...) to close a visible
      // dialog. Tolerate its compact form without demanding unused fields.
      if (id == -1) {
        dialog_.active = false;
        dialog_.id = -1;
        dialog_.style = 0;
        dialog_.title.clear();
        dialog_.button1.clear();
        dialog_.button2.clear();
        dialog_.body.clear();
        advanceDialogRevision(dialog_);
        event.kind = RpcEventKind::DialogChanged;
        event.summary = "ShowDialog close";
        return true;
      }
      if (id < 0) break;
      Dialog next;
      next.active = true;
      next.id = id;
      next.style = static_cast<uint8_t>(reader.readBits(8));
      std::string title;
      std::string button1;
      std::string button2;
      std::string body;
      if (!reader.ok() ||
          !readString8AllowEmpty(reader, kMaxDialogTitleBytes, title) ||
          !readString8AllowEmpty(reader, kMaxDialogButtonBytes, button1) ||
          !readString8AllowEmpty(reader, kMaxDialogButtonBytes, button2) ||
          !raknet::stringcompressor::decodeDefaultString(reader, kMaxDialogBodyBytes, body)) {
        break;
      }
      next.title = sanitizeUiText(title);
      next.button1 = sanitizeUiText(button1);
      next.button2 = sanitizeUiText(button2);
      next.body = sanitizeUiText(body);
      next.revision = dialog_.revision;
      advanceDialogRevision(next);
      dialog_ = std::move(next);
      event.kind = RpcEventKind::DialogChanged;
      event.summary = "ShowDialog #" + std::to_string(id);
      return true;
    }

    // SetSpawnInfo is the authoritative local respawn template. DL R1 adds
    // CustomModelID:u32 after ModelID:u32. It is retained as an identity only;
    // the stock/base model remains the visual fallback until custom artwork is
    // independently available from a lawful user-approved source.
    case kSetSpawnInfo: {
      LocalPlayer next = localPlayer_;
      next.team = static_cast<uint8_t>(reader.readBits(8));
      next.model = reader.readBits(32);
      next.customModel = usesDlR1WireLayout() ? reader.readBits(32) : 0u;
      next.spawnUnknown = static_cast<uint8_t>(reader.readBits(8));
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      const float angle = readFloat(reader, finite);
      for (uint32_t& weapon : next.spawnWeapons) weapon = reader.readBits(32);
      for (uint32_t& ammo : next.spawnAmmo) ammo = reader.readBits(32);
      const bool validSpawnWeapons = std::all_of(
          next.spawnWeapons.begin(), next.spawnWeapons.end(),
          [](uint32_t weapon) { return weapon <= kMaxLegacyWeaponId; });
      if (!reader.ok() || !finite || !validWorldPosition(x, y, z) || !validSpawnWeapons) break;

      // SetSpawnInfo is a server-owned respawn loadout. Retain its exact raw
      // weapon-ID/ammo pairs so a subsequent SetPlayerArmedWeapon can make
      // AimSync ready without Android inventing an ammo value. It never picks
      // the active weapon; RPC 67 remains the sole armed-selection authority.
      for (size_t index = 0; index < next.spawnWeapons.size(); ++index) {
        const uint8_t weapon = static_cast<uint8_t>(next.spawnWeapons[index]);
        next.weaponAmmo[weapon] = next.spawnAmmo[index];
        next.weaponAmmoKnown[weapon] = true;
      }

      next.spawnInfoKnown = true;
      next.positionKnown = true;
      next.positionFindZ = false;
      next.facingKnown = true;
      next.x = x;
      next.y = y;
      next.z = z;
      next.angle = normalizedAngle(angle);
      localPlayer_ = next;
      setEvent(event, RpcEventKind::SpawnInfoChanged,
               initialized_ ? playerInit_.playerId : 0, "SetSpawnInfo");
      event.localFacingUpdated = true;
      event.localPositionUpdated = true;
      return true;
    }

    // Exact open.mp-compatible local vehicle authority. The server may force
    // this independently of an EnterVehicle request. It authorizes only the
    // later session recheck; Packet-200 remains limited to a streamed seat-0
    // vehicle through the bounded native driver bridge.
    case kPutPlayerInVehicle: {
      if (rpcPayload.size() - 2u != 3u) break;
      const uint16_t vehicleId = static_cast<uint16_t>(reader.readBits(16));
      const uint8_t seatId = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || seatId > 8u) break;
      localPlayer_.vehicleKnown = true;
      localPlayer_.inVehicle = true;
      localPlayer_.vehicleId = vehicleId;
      localPlayer_.vehicleSeat = seatId;
      event.localVehicleUpdated = true;
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0, "PutPlayerInVehicle");
      return true;
    }

    case kRemovePlayerFromVehicle:
      if (rpcPayload.size() != 2u) break;
      localPlayer_.vehicleKnown = true;
      localPlayer_.inVehicle = false;
      localPlayer_.vehicleSeat = 0;
      event.localVehicleUpdated = true;
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0, "RemovePlayerFromVehicle");
      return true;

    case kSetPlayerPosition:
    case kSetPlayerPositionFindZ: {
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      if (!reader.ok() || !finite || !validWorldPosition(x, y, z)) break;
      localPlayer_.x = x;
      localPlayer_.y = y;
      localPlayer_.z = z;
      localPlayer_.positionKnown = true;
      localPlayer_.positionFindZ = rpcId == kSetPlayerPositionFindZ;
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0,
               rpcId == kSetPlayerPosition ? "SetPlayerPosition" : "SetPlayerPositionFindZ");
      event.localPositionUpdated = true;
      return true;
    }

    case kTogglePlayerControllable: {
      const uint8_t enable = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || enable > 1) break;
      localPlayer_.controllableKnown = true;
      localPlayer_.controllable = enable != 0;
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0,
               localPlayer_.controllable ? "TogglePlayerControllable on" : "TogglePlayerControllable off");
      return true;
    }

    // `SetPlayerWorldBounds(maxX, minX, maxY, minY)` is a per-recipient
    // exterior movement policy. Keep exactly its Vector4 order; it never
    // changes GTA collision/placements or applies in interiors.
    case kSetWorldBounds: {
      if (rpcPayload.size() - 2u != 16u) break;
      bool finite = true;
      const float maxX = readFloat(reader, finite);
      const float minX = readFloat(reader, finite);
      const float maxY = readFloat(reader, finite);
      const float minY = readFloat(reader, finite);
      if (!reader.ok() || !finite || !validWorldBounds(maxX, minX, maxY, minY)) break;
      environment_.worldBoundsKnown = true;
      environment_.worldMaxX = maxX;
      environment_.worldMinX = minX;
      environment_.worldMaxY = maxY;
      environment_.worldMinY = minY;
      event.kind = RpcEventKind::WorldEnvironmentChanged;
      event.summary = "SetWorldBounds";
      event.worldBoundsUpdated = true;
      return true;
    }

    case kSetPlayerFacingAngle: {
      bool finite = true;
      const float angle = readFloat(reader, finite);
      if (!reader.ok() || !finite) break;
      localPlayer_.angle = normalizedAngle(angle);
      localPlayer_.facingKnown = true;
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0, "SetPlayerFacingAngle");
      event.localFacingUpdated = true;
      return true;
    }

    case kSetPlayerHealth:
    case kSetPlayerArmour: {
      bool finite = true;
      const float value = readFloat(reader, finite);
      if (!reader.ok() || !finite || !validVital(value)) break;
      if (rpcId == kSetPlayerHealth) {
        localPlayer_.health = value;
        localPlayer_.healthKnown = true;
      } else {
        localPlayer_.armour = value;
        localPlayer_.armourKnown = true;
      }
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0,
               rpcId == kSetPlayerHealth ? "SetPlayerHealth" : "SetPlayerArmour");
      return true;
    }

    // ApplyPlayerAnimation is an explicitly addressed player directive, unlike
    // Packet-207's compact current-animation flags. Keep it for the local
    // player or a currently streamed remote player, but never fabricate a
    // player record/physics lock from an orphan RPC. Actual GTA IFP playback
    // remains a renderer concern; this reducer does not alter movement.
    case kApplyPlayerAnimation: {
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok()) break;
      AnimationDirective* target = nullptr;
      const bool localTarget = initialized_ && id == playerInit_.playerId;
      if (localTarget) {
        target = &localPlayer_.forcedAnimation;
      } else {
        const auto player = players_.find(id);
        if (player == players_.end() || !player->second.streamedIn) return false;
        target = &player->second.forcedAnimation;
      }
      AnimationDirective next = *target;
      if (!readAnimationDirective(reader, next)) break;
      const size_t bodyBits = (rpcPayload.size() - 2u) * 8u;
      if (reader.bitPosition() > bodyBits || bodyBits - reader.bitPosition() > 4u ||
          (reader.bitPosition() < bodyBits && reader.readBits(bodyBits - reader.bitPosition()) != 0u)) {
        break;
      }
      advanceAnimationRevision(next);
      *target = std::move(next);
      setEvent(event, localTarget ? RpcEventKind::LocalPlayerChanged : RpcEventKind::RemotePlayerSync,
               id, "ApplyPlayerAnimation");
      return true;
    }

    case kClearPlayerTasks: {
      if (rpcPayload.size() - 2u != 2u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok()) break;
      const bool localTarget = initialized_ && id == playerInit_.playerId;
      AnimationDirective* target = nullptr;
      if (localTarget) {
        target = &localPlayer_.forcedAnimation;
      } else {
        const auto player = players_.find(id);
        if (player == players_.end() || !player->second.streamedIn) return false;
        target = &player->second.forcedAnimation;
      }
      clearAnimationDirective(*target);
      setEvent(event, localTarget ? RpcEventKind::LocalPlayerChanged : RpcEventKind::RemotePlayerSync,
               id, "ClearPlayerTasks");
      return true;
    }

    case kSetPlayerSpecialAction:
      localPlayer_.specialAction = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok()) break;
      localPlayer_.specialActionKnown = true;
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0, "SetPlayerSpecialAction");
      return true;

    // Per-recipient time uses an exact hour/minute pair. It is visual
    // environment/HUD state only: neither collision, game speed, nor native
    // movement cadence derives from a client clock.
    case kSetPlayerTime: {
      if (rpcPayload.size() - 2u != 2u) break;
      const uint8_t hour = static_cast<uint8_t>(reader.readBits(8));
      const uint8_t minute = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || hour > 23u || minute > 59u) break;
      environment_.playerTimeKnown = true;
      environment_.playerHour = hour;
      environment_.playerMinute = minute;
      event.kind = RpcEventKind::WorldEnvironmentChanged;
      event.summary = "SetPlayerTime";
      return true;
    }

    // TogglePlayerClock controls only the local visual clock affordance. Time
    // facts remain retained so a later enabled toggle can show the last exact
    // server-provided value without asking Android to synthesize a clock.
    case kTogglePlayerClock: {
      if (rpcPayload.size() - 2u != 1u) break;
      const uint8_t enabled = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || enabled > 1u) break;
      environment_.playerClockKnown = true;
      environment_.playerClockEnabled = enabled != 0u;
      event.kind = RpcEventKind::WorldEnvironmentChanged;
      event.summary = environment_.playerClockEnabled ? "TogglePlayerClock on" :
                                                        "TogglePlayerClock off";
      return true;
    }

    case kSetWorldTime: {
      const uint8_t worldTime = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || worldTime > 23) break;
      environment_.worldTime = worldTime;
      environment_.timeKnown = true;
      event.kind = RpcEventKind::WorldEnvironmentChanged;
      event.summary = "SetWorldTime";
      return true;
    }

    case kSetWeather:
      environment_.weather = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok()) break;
      environment_.weatherKnown = true;
      event.kind = RpcEventKind::WorldEnvironmentChanged;
      event.summary = "SetWeather";
      return true;

    // Wanted level is server-owned HUD presentation only. Do not derive police
    // behaviour, pursuits, rewards, or any local gameplay transition from it.
    case kSetPlayerWantedLevel: {
      if (rpcPayload.size() - 2u != 1u) break;
      const uint8_t level = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || level > 6u) break;
      localPlayer_.wantedLevel = level;
      localPlayer_.wantedLevelKnown = true;
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0, "SetPlayerWantedLevel");
      return true;
    }

    // Core non-DL SetPlayerGravity is one float. Store only an envelope that
    // cannot destabilize renderer/HUD state; the predictor applies a stricter
    // supported range and fails closed for exotic server physics.
    case kSetPlayerGravity: {
      if (rpcPayload.size() - 2u != 4u) break;
      bool finite = true;
      const float gravity = readFloat(reader, finite);
      if (!reader.ok() || !finite || !validServerGravity(gravity)) break;
      environment_.gravity = gravity;
      environment_.gravityKnown = true;
      event.kind = RpcEventKind::WorldEnvironmentChanged;
      event.summary = "SetPlayerGravity";
      event.worldGravityUpdated = true;
      return true;
    }

    case kSetPlayerInterior: {
      const uint8_t interior = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok()) break;
      localPlayer_.interior = interior;
      localPlayer_.interiorKnown = true;
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0, "SetPlayerInterior");
      event.localInteriorUpdated = true;
      return true;
    }

    case kSetPlayerVelocity: {
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      if (!reader.ok() || !finite || !validVelocity(x, y, z)) break;
      localPlayer_.velocityX = x;
      localPlayer_.velocityY = y;
      localPlayer_.velocityZ = z;
      localPlayer_.velocityKnown = true;
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0, "SetPlayerVelocity");
      event.localVelocityUpdated = true;
      return true;
    }

    // RPC 155 is a full score/ping roster with no count: source writer emits
    // exactly repeated u16 ID, i32 score, u32 ping tuples to payload end.
    case kSendPlayerScoresAndPings: {
      const size_t bodyBytes = rpcPayload.size() - 2u;
      constexpr size_t kScoreEntryBytes = 10u;
      if (bodyBytes % kScoreEntryBytes != 0u || bodyBytes / kScoreEntryBytes > kMaxScoreboardEntries) {
        break;
      }
      struct ScoreUpdate {
        uint16_t id;
        int32_t score;
        uint32_t ping;
      };
      std::vector<ScoreUpdate> updates;
      updates.reserve(bodyBytes / kScoreEntryBytes);
      std::map<uint16_t, bool> seen;
      const size_t entryCount = bodyBytes / kScoreEntryBytes;
      for (size_t entry = 0; entry < entryCount; ++entry) {
        const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
        const uint32_t rawScore = reader.readBits(32);
        const uint32_t ping = reader.readBits(32);
        int32_t score = 0;
        std::memcpy(&score, &rawScore, sizeof(score));
        if (!reader.ok() || seen.find(id) != seen.end()) {
          updates.clear();
          break;
        }
        seen.emplace(id, true);
        updates.push_back({id, score, ping});
      }
      if (!reader.ok() || updates.size() != bodyBytes / kScoreEntryBytes) break;

      // The compatible writer sends a complete cached roster, so replace—not
      // merge—prior facts. This prevents departed/missing IDs from lingering.
      for (auto& [id, player] : players_) {
        (void)id;
        player.scorePing = {};
      }
      localPlayer_.scorePing = {};
      scorePingOverrides_.clear();
      for (const ScoreUpdate& update : updates) {
        ScorePing value;
        value.known = true;
        value.score = update.score;
        value.ping = update.ping;
        if (initialized_ && update.id == playerInit_.playerId) {
          localPlayer_.scorePing = value;
          continue;
        }
        scorePingOverrides_.emplace(update.id, value);
        const auto player = players_.find(update.id);
        if (player != players_.end()) player->second.scorePing = value;
      }
      event.kind = RpcEventKind::ScoreboardChanged;
      event.summary = "SendPlayerScoresAndPings";
      return true;
    }

    // SetPlayerChatBubble(59): recipient-specific temporary text above a
    // player. The wire expiry is a duration, sampled against local monotonic
    // time only when exporting render copies; it has no gameplay authority.
    case kSetPlayerChatBubble: {
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      const uint32_t rgba = reader.readBits(32);
      bool finite = true;
      const float drawDistance = readFloat(reader, finite);
      const uint32_t expireTime = reader.readBits(32);
      std::string text;
      if (!readString8AllowEmpty(reader, kMaxChatBubbleBytes, text) || !reader.ok() || !finite ||
          drawDistance < 0.0f || drawDistance > kMaxChatBubbleDrawDistance) {
        break;
      }
      ChatBubble* bubble = nullptr;
      if (initialized_ && id == playerInit_.playerId) {
        bubble = &localPlayer_.chatBubble;
      } else {
        Player* player = getOrCreate(id);
        if (!player) break;
        bubble = &player->chatBubble;
      }
      if (text.empty() || expireTime == 0u || drawDistance == 0.0f) {
        clearChatBubble(*bubble);
      } else {
        const uint64_t nowMs = monotonicMilliseconds();
        bubble->active = true;
        bubble->rgba = rgba;
        bubble->drawDistance = drawDistance;
        bubble->expiresAtMs = nowMs + std::min(expireTime, kMaxChatBubbleLifetimeMs);
        bubble->text = sanitizeUiText(text);
        if (bubble->text.empty()) clearChatBubble(*bubble);
      }
      setEvent(event, RpcEventKind::ChatBubbleChanged, id, "SetPlayerChatBubble");
      return true;
    }

    case kPlayerJoin: {
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      const uint32_t rgba = reader.readBits(32);
      const bool npc = static_cast<bool>(reader.readBits(8));
      const std::string name = readString8(reader, kMaxPlayerNameBytes);
      if (!reader.ok() || name.empty()) break;
      Player* player = getOrCreate(id);
      if (!player) break;
      player->rgba = rgba;
      player->npc = npc;
      player->name = name;
      setEvent(event, RpcEventKind::PlayerJoined, id, "PlayerJoin");
      return true;
    }

    case kPlayerQuit: {
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      reader.readBits(8);  // reason; state only needs removal
      if (!reader.ok()) break;
      // Ensure a departed driver cannot leave a live vehicle displaying a
      // ghost occupant. `driverKnown` keeps player ID zero unambiguous.
      for (auto& [vehicleId, vehicle] : vehicles_) {
        (void)vehicleId;
        if (vehicle.driverKnown && vehicle.driverId == id) {
          vehicle.driverKnown = false;
          vehicle.driverId = 0;
        }
      }
      players_.erase(id);
      playerAttachedObjects_.erase(id);
      if (initialized_ && id == playerInit_.playerId && attachedObjectEdit_.active) {
        attachedObjectEdit_.active = false;
        advanceAttachedObjectEditRevision(attachedObjectEdit_);
      }
      nameTagVisibilityOverrides_.erase(id);
      scorePingOverrides_.erase(id);
      setEvent(event, RpcEventKind::PlayerQuit, id, "PlayerQuit");
      return true;
    }

    case kSetPlayerName: {
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      const std::string name = readString8(reader, kMaxPlayerNameBytes);
      reader.readBits(8);  // Success
      if (!reader.ok() || name.empty()) break;
      Player* player = getOrCreate(id);
      if (!player) break;
      player->name = name;
      setEvent(event, RpcEventKind::PlayerRenamed, id, "SetPlayerName");
      return true;
    }

    case kSetPlayerColor: {
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      const uint32_t rgba = reader.readBits(32);
      if (!reader.ok()) break;
      Player* player = getOrCreate(id);
      if (!player) break;
      player->rgba = rgba;
      setEvent(event, RpcEventKind::PlayerColorChanged, id, "SetPlayerColor");
      return true;
    }

    // Legacy SetPlayerSkin is PlayerID:u32 | Skin:u32. DL R1 changes that
    // first field to PlayerID:u16 and appends CustomSkin:u32. The selected
    // ClientJoin profile is the only allowed discriminator; treating either
    // form as a loose prefix would misalign every following state field.
    case kSetPlayerSkin: {
      const size_t expectedBodyBytes = usesDlR1WireLayout() ? 10u : 8u;
      if (rpcPayload.size() - 2u != expectedBodyBytes) break;
      const uint32_t rawId = usesDlR1WireLayout() ? reader.readBits(16) : reader.readBits(32);
      const uint32_t model = reader.readBits(32);
      const uint32_t customModel = usesDlR1WireLayout() ? reader.readBits(32) : 0u;
      if (!reader.ok() || rawId > std::numeric_limits<uint16_t>::max() || model > 65535u) {
        break;
      }
      const uint16_t id = static_cast<uint16_t>(rawId);
      if (initialized_ && id == playerInit_.playerId) {
        // Keep the spawn-known gate unchanged: a skin update before a real
        // spawn template may update retained state but cannot invent a local
        // actor at the default origin for the renderer.
        localPlayer_.model = model;
        localPlayer_.customModel = customModel;
        setEvent(event, RpcEventKind::LocalPlayerChanged, id, "SetPlayerSkin");
        return true;
      }
      Player* player = getOrCreate(id);
      if (!player) break;
      player->model = model;
      player->customModel = customModel;
      setEvent(event, RpcEventKind::PlayerSkinChanged, id, "SetPlayerSkin");
      return true;
    }

    // Per-recipient nametag visibility. Keep it as presentation-only state:
    // global PlayerInit policy is sampled in the renderer and LOS remains
    // unevaluated until the collision renderer has an exact visibility ray.
    case kShowPlayerNameTagForPlayer: {
      if (rpcPayload.size() - 2u != 3u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      const uint8_t show = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || show > 1u) break;
      if (initialized_ && id == playerInit_.playerId) return false;
      const auto existing = nameTagVisibilityOverrides_.find(id);
      if (existing == nameTagVisibilityOverrides_.end() &&
          nameTagVisibilityOverrides_.size() >= kMaxNameTagVisibilityOverrides) {
        break;
      }
      const bool visible = show != 0u;
      nameTagVisibilityOverrides_[id] = visible;
      const auto player = players_.find(id);
      if (player != players_.end()) player->second.nameTagVisible = visible;
      setEvent(event, RpcEventKind::NameTagChanged, id, "ShowPlayerNameTagForPlayer");
      return true;
    }

    // Global pickup lifecycle. RPC CreatePickup is a fixed six 32-bit-field
    // payload; virtual-world/interior selection is server-side and deliberately
    // not guessed by this client boundary.
    case kCreatePickup: {
      if (rpcPayload.size() - 2u != 24u) break;
      const uint32_t rawId = reader.readBits(32);
      const uint32_t rawModel = reader.readBits(32);
      const uint32_t rawSpawnType = reader.readBits(32);
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      if (!reader.ok() || !finite || rawId > 0x7fffffffu || rawModel == 0 ||
          rawModel > 65535u || rawSpawnType > 65535u || !validWorldPosition(x, y, z)) {
        break;
      }
      const int32_t id = static_cast<int32_t>(rawId);
      Pickup* pickup = getOrCreatePickup(id);
      if (!pickup) break;
      pickup->id = id;
      pickup->model = static_cast<int32_t>(rawModel);
      pickup->spawnType = static_cast<int32_t>(rawSpawnType);
      pickup->x = x;
      pickup->y = y;
      pickup->z = z;
      event.kind = RpcEventKind::PickupCreated;
      event.summary = "CreatePickup #" + std::to_string(id);
      return true;
    }

    case kDestroyPickup: {
      if (rpcPayload.size() - 2u != 4u) break;
      const uint32_t rawId = reader.readBits(32);
      if (!reader.ok() || rawId > 0x7fffffffu) break;
      const int32_t id = static_cast<int32_t>(rawId);
      // A late/missing create does not make a structurally valid destroy
      // malformed; it is idempotent from the renderer's perspective.
      pickups_.erase(id);
      event.kind = RpcEventKind::PickupDestroyed;
      event.summary = "DestroyPickup #" + std::to_string(id);
      return true;
    }

    // Writer-verified AttachObjectToPlayer updates an existing global object;
    // it never creates one from an orphan RPC. The renderer resolves it only
    // while the exact local/streamed player parent has a valid transform.
    case kAttachObjectToPlayer: {
      constexpr size_t kAttachObjectToPlayerBytes = 28;
      if (rpcPayload.size() - 2u != kAttachObjectToPlayerBytes) break;
      const uint16_t objectId = static_cast<uint16_t>(reader.readBits(16));
      const uint16_t playerId = static_cast<uint16_t>(reader.readBits(16));
      bool finite = true;
      const float offsetX = readFloat(reader, finite);
      const float offsetY = readFloat(reader, finite);
      const float offsetZ = readFloat(reader, finite);
      const float rotationX = readFloat(reader, finite);
      const float rotationY = readFloat(reader, finite);
      const float rotationZ = readFloat(reader, finite);
      if (!reader.ok() || !finite || !validObjectAttachmentOffset(offsetX, offsetY, offsetZ) ||
          !validObjectRotation(rotationX, rotationY, rotationZ)) {
        break;
      }
      const auto object = objects_.find(objectId);
      if (object == objects_.end()) return false;
      ServerObject& value = object->second;
      value.attached = true;
      value.attachmentKind = ObjectAttachmentKind::Player;
      value.attachmentParentId = playerId;
      value.attachmentOffsetX = offsetX;
      value.attachmentOffsetY = offsetY;
      value.attachmentOffsetZ = offsetZ;
      value.attachmentRotationX = normalizedAngle(rotationX);
      value.attachmentRotationY = normalizedAngle(rotationY);
      value.attachmentRotationZ = normalizedAngle(rotationZ);
      // RPC 75 has no SyncRotation byte; its parent-relative rotation is always
      // applied by the legacy writer/recipient path.
      value.attachmentSyncRotation = true;
      setEvent(event, RpcEventKind::ObjectChanged, objectId, "AttachObjectToPlayer");
      return true;
    }

    // Static global object lifecycle. The oldest short layout has only the
    // transform. Current SA-MP/open.mp layouts add NoCameraCol, vehicle/object
    // parent IDs, optional six attachment floats + SyncRotation, then a
    // variable material section. Default texture records are source-writer
    // parsed into a bounded renderer copy; text material records are consumed
    // but deliberately remain unsupported until a native text-atlas exists.
    case kCreateObject: {
      constexpr size_t kLegacyCreateObjectBytes = 34;
      constexpr size_t kModernStaticHeaderBytes = 40;
      constexpr size_t kModernAttachedHeaderBytes = 65;
      constexpr size_t kMaxCreateObjectBytes = 32u * 1024u;
      const size_t bodyBytes = rpcPayload.size() - 2u;
      if (bodyBytes < kLegacyCreateObjectBytes || bodyBytes > kMaxCreateObjectBytes) break;
      ServerObject next;
      next.id = static_cast<uint16_t>(reader.readBits(16));
      const uint32_t rawModel = reader.readBits(32);
      std::memcpy(&next.sourceModel, &rawModel, sizeof(next.sourceModel));
      next.model = rawModel;
      if (usesDlR1WireLayout() && next.sourceModel < 0) {
        const auto fallback = dlObjectBaseModels_.find(next.sourceModel);
        next.model = fallback != dlObjectBaseModels_.end() ? fallback->second : 0u;
      }
      bool finite = true;
      next.x = readFloat(reader, finite);
      next.y = readFloat(reader, finite);
      next.z = readFloat(reader, finite);
      next.rotationX = readFloat(reader, finite);
      next.rotationY = readFloat(reader, finite);
      next.rotationZ = readFloat(reader, finite);
      next.drawDistance = readFloat(reader, finite);
      if (bodyBytes != kLegacyCreateObjectBytes) {
        if (bodyBytes < kModernStaticHeaderBytes) break;
        reader.readBits(8);  // NoCameraCol, presentation-only for this bridge.
        // The source-compatible writer emits vehicle parent first, then object
        // parent. Exactly one can be present for this global-object surface.
        const uint16_t attachedVehicle = static_cast<uint16_t>(reader.readBits(16));
        const uint16_t attachedObject = static_cast<uint16_t>(reader.readBits(16));
        const bool hasVehicleParent = attachedVehicle != 0xFFFFu;
        const bool hasObjectParent = attachedObject != 0xFFFFu;
        if (hasVehicleParent && hasObjectParent) break;
        next.attached = hasVehicleParent || hasObjectParent;
        if (next.attached) {
          if (bodyBytes < kModernAttachedHeaderBytes) break;
          next.attachmentOffsetX = readFloat(reader, finite);
          next.attachmentOffsetY = readFloat(reader, finite);
          next.attachmentOffsetZ = readFloat(reader, finite);
          next.attachmentRotationX = readFloat(reader, finite);
          next.attachmentRotationY = readFloat(reader, finite);
          next.attachmentRotationZ = readFloat(reader, finite);
          const uint8_t syncRotation = static_cast<uint8_t>(reader.readBits(8));
          if (!reader.ok() || !finite || syncRotation > 1u ||
              !validObjectAttachmentOffset(next.attachmentOffsetX, next.attachmentOffsetY,
                                           next.attachmentOffsetZ) ||
              !validObjectRotation(next.attachmentRotationX, next.attachmentRotationY,
                                   next.attachmentRotationZ)) {
            break;
          }
          next.attachmentKind = hasVehicleParent ? ObjectAttachmentKind::Vehicle
                                                  : ObjectAttachmentKind::Object;
          next.attachmentParentId = hasVehicleParent ? attachedVehicle : attachedObject;
          next.attachmentRotationX = normalizedAngle(next.attachmentRotationX);
          next.attachmentRotationY = normalizedAngle(next.attachmentRotationY);
          next.attachmentRotationZ = normalizedAngle(next.attachmentRotationZ);
          next.attachmentSyncRotation = syncRotation != 0u;
        }
        const uint8_t materialCount = static_cast<uint8_t>(reader.readBits(8));
        if (!reader.ok() || materialCount > kMaxServerObjectMaterialSlots) break;
        std::array<bool, kMaxServerObjectMaterialSlots> materialSlotSeen{};
        bool materialsOk = true;
        for (uint8_t index = 0; index < materialCount; ++index) {
          uint8_t slot = 0;
          ServerObjectMaterialOverride material;
          if (!readObjectMaterialRecord(reader, slot, material) || materialSlotSeen[slot]) {
            materialsOk = false;
            break;
          }
          materialSlotSeen[slot] = true;
          next.materials[slot] = std::move(material);
        }
        if (!materialsOk) break;
      }
      // `reader` begins after the two-byte RPC envelope. Only final-byte
      // transport padding (at most seven bits) may remain after the exact
      // source-writer body; envelope bits are not payload slack.
      const size_t createObjectTotalBits = bodyBytes * 8u;
      if (!reader.ok() || reader.bitPosition() > createObjectTotalBits ||
          createObjectTotalBits - reader.bitPosition() > 7u) {
        break;
      }
      const bool unresolvedDlCustomObject =
          usesDlR1WireLayout() && validDlCustomObjectId(next.sourceModel) && next.model == 0u;
      if (!reader.ok() || !finite ||
          (!unresolvedDlCustomObject && (next.model == 0u || next.model > 65535u)) ||
          (usesDlR1WireLayout() && next.sourceModel < 0 &&
           !validDlCustomObjectId(next.sourceModel)) ||
          !validWorldPosition(next.x, next.y, next.z) ||
          !validObjectRotation(next.rotationX, next.rotationY, next.rotationZ) ||
          !validObjectDrawDistance(next.drawDistance)) {
        break;
      }
      next.rotationX = normalizedAngle(next.rotationX);
      next.rotationY = normalizedAngle(next.rotationY);
      next.rotationZ = normalizedAngle(next.rotationZ);
      ServerObject* object = getOrCreateObject(next.id);
      if (!object) break;
      *object = next;
      setEvent(event, RpcEventKind::ObjectCreated, next.id, "CreateObject");
      return true;
    }

    // RPC 84 is open.mp's SetPlayerObjectMaterial writer and uses the same
    // record after its object ID. Global/player objects share the client ID
    // pool for one recipient, so this renderer-unified retained object safely
    // replaces one existing slot; an orphan packet never allocates state.
    case kSetObjectMaterial: {
      if (rpcPayload.size() - 2u < 4u) break;
      const uint16_t objectId = static_cast<uint16_t>(reader.readBits(16));
      uint8_t slot = 0;
      ServerObjectMaterialOverride material;
      if (!readObjectMaterialRecord(reader, slot, material)) break;
      const size_t setMaterialTotalBits = (rpcPayload.size() - 2u) * 8u;
      if (!reader.ok() || reader.bitPosition() > setMaterialTotalBits ||
          setMaterialTotalBits - reader.bitPosition() > 7u) {
        break;
      }
      const auto object = objects_.find(objectId);
      if (object == objects_.end()) return false;
      object->second.materials[slot] = std::move(material);
      setEvent(event, RpcEventKind::ObjectChanged, objectId, "SetObjectMaterial");
      return true;
    }

    case kSetObjectPosition: {
      if (rpcPayload.size() - 2u != 14u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      bool finite = true;
      const float x = readFloat(reader, finite);
      const float y = readFloat(reader, finite);
      const float z = readFloat(reader, finite);
      if (!reader.ok() || !finite || !validWorldPosition(x, y, z)) break;
      const auto object = objects_.find(id);
      if (object == objects_.end()) return false;
      object->second.x = x;
      object->second.y = y;
      object->second.z = z;
      object->second.moving = false;
      setEvent(event, RpcEventKind::ObjectChanged, id, "SetObjectPosition");
      return true;
    }

    case kSetObjectRotation: {
      if (rpcPayload.size() - 2u != 14u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      bool finite = true;
      const float rotationX = readFloat(reader, finite);
      const float rotationY = readFloat(reader, finite);
      const float rotationZ = readFloat(reader, finite);
      if (!reader.ok() || !finite || !validObjectRotation(rotationX, rotationY, rotationZ)) break;
      const auto object = objects_.find(id);
      if (object == objects_.end()) return false;
      object->second.rotationX = normalizedAngle(rotationX);
      object->second.rotationY = normalizedAngle(rotationY);
      object->second.rotationZ = normalizedAngle(rotationZ);
      object->second.moving = false;
      setEvent(event, RpcEventKind::ObjectChanged, id, "SetObjectRotation");
      return true;
    }

    // Exact open.mp-compatible MoveObject layout: ID, current position, target
    // position, speed, and the always-present target rotation vector (42 body
    // bytes). An all-`-1000` vector means no final rotation. Interpolate only
    // frame snapshots with steady-clock time; never synthesize it to server.
    case kMoveObject: {
      constexpr size_t kMoveObjectBytes = 42;
      if (rpcPayload.size() - 2u != kMoveObjectBytes) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      bool finite = true;
      const float startX = readFloat(reader, finite);
      const float startY = readFloat(reader, finite);
      const float startZ = readFloat(reader, finite);
      const float targetX = readFloat(reader, finite);
      const float targetY = readFloat(reader, finite);
      const float targetZ = readFloat(reader, finite);
      const float speed = readFloat(reader, finite);
      const float targetRotationX = readFloat(reader, finite);
      const float targetRotationY = readFloat(reader, finite);
      const float targetRotationZ = readFloat(reader, finite);
      const bool noTargetRotation = targetRotationX <= -999.0f && targetRotationY <= -999.0f &&
                                    targetRotationZ <= -999.0f;
      const bool allTargetRotation = targetRotationX > -999.0f && targetRotationY > -999.0f &&
                                     targetRotationZ > -999.0f;
      if (!reader.ok() || !finite || !validWorldPosition(startX, startY, startZ) ||
          !validWorldPosition(targetX, targetY, targetZ) || !std::isfinite(speed) ||
          speed <= 0.0f || speed > 10000.0f || (!noTargetRotation && !allTargetRotation) ||
          (allTargetRotation && !validObjectRotation(
              targetRotationX, targetRotationY, targetRotationZ))) {
        break;
      }
      const auto object = objects_.find(id);
      if (object == objects_.end()) return false;
      const double dx = static_cast<double>(targetX) - startX;
      const double dy = static_cast<double>(targetY) - startY;
      const double dz = static_cast<double>(targetZ) - startZ;
      const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
      const double duration = distance / speed;
      if (!std::isfinite(distance) || !std::isfinite(duration) || duration > 24.0 * 60.0 * 60.0) {
        break;
      }
      ServerObject& value = object->second;
      const uint64_t moveStartedMs = monotonicMilliseconds();
      const ServerObjectView current = sampledObjectView(value, moveStartedMs);
      // The RPC carries its own position start, while rotation starts from the
      // last client-visible orientation because MoveObject only sends a target.
      value.rotationX = current.rotationX;
      value.rotationY = current.rotationY;
      value.rotationZ = current.rotationZ;
      value.x = startX;
      value.y = startY;
      value.z = startZ;
      value.moveTargetX = targetX;
      value.moveTargetY = targetY;
      value.moveTargetZ = targetZ;
      value.moveHasTargetRotation = !noTargetRotation;
      if (value.moveHasTargetRotation) {
        value.moveTargetRotationX = normalizedAngle(targetRotationX);
        value.moveTargetRotationY = normalizedAngle(targetRotationY);
        value.moveTargetRotationZ = normalizedAngle(targetRotationZ);
      }
      if (duration <= 0.0001) {
        value.x = targetX;
        value.y = targetY;
        value.z = targetZ;
        if (value.moveHasTargetRotation) {
          value.rotationX = value.moveTargetRotationX;
          value.rotationY = value.moveTargetRotationY;
          value.rotationZ = value.moveTargetRotationZ;
        }
        value.moving = false;
      } else {
        value.moveStartedMs = moveStartedMs;
        value.moveDurationSeconds = static_cast<float>(duration);
        value.moving = true;
      }
      setEvent(event, RpcEventKind::ObjectChanged, id, "MoveObject");
      return true;
    }

    case kStopObject: {
      if (rpcPayload.size() - 2u != 2u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok()) break;
      const auto object = objects_.find(id);
      if (object == objects_.end()) return false;
      const ServerObjectView sampled = sampledObjectView(object->second, monotonicMilliseconds());
      object->second.x = sampled.x;
      object->second.y = sampled.y;
      object->second.z = sampled.z;
      object->second.rotationX = sampled.rotationX;
      object->second.rotationY = sampled.rotationY;
      object->second.rotationZ = sampled.rotationZ;
      object->second.moving = false;
      setEvent(event, RpcEventKind::ObjectChanged, id, "StopObject");
      return true;
    }

    case kDestroyObject: {
      if (rpcPayload.size() - 2u != 2u) break;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      if (!reader.ok()) break;
      if (objects_.erase(id) == 0u) return false;
      // Do not leave an editor targeting a recycled/deleted ID while waiting
      // for its paired server cancel. A future begin RPC establishes a fresh
      // revision and the session refuses every outbound response meanwhile.
      if (objectEdit_.active && objectEdit_.objectId == id) {
        objectEdit_.active = false;
        objectEdit_.playerObject = false;
        objectEdit_.objectId = 0xFFFFu;
        advanceObjectEditRevision(objectEdit_);
      }
      setEvent(event, RpcEventKind::ObjectDestroyed, id, "DestroyObject");
      return true;
    }

    // SetPlayerVirtualWorld (RPC 48) uses one signed int32 in the core
    // non-DL writer. It is not a GTA interior and must not guess collision,
    // object visibility, or entity routing locally: the server already streams
    // recipient-visible entities. Retain the exact scalar for HUD/session view.
    case kSetPlayerVirtualWorld: {
      if (rpcPayload.size() - 2u != 4u) break;
      const uint32_t rawWorld = reader.readBits(32);
      if (!reader.ok()) break;
      int32_t virtualWorld = 0;
      static_assert(sizeof(virtualWorld) == sizeof(rawWorld), "virtual world is int32");
      std::memcpy(&virtualWorld, &rawWorld, sizeof(virtualWorld));
      localPlayer_.virtualWorld = virtualWorld;
      localPlayer_.virtualWorldKnown = true;
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0, "SetPlayerVirtualWorld");
      return true;
    }

    // Global kill feed. Source writer sends KillerID (or INVALID_PLAYER_ID),
    // PlayerID, then one raw reason byte. It is not the PlayerDeath (166)
    // observer state and must never mutate local health/spawn/prediction.
    case kSendDeathMessage: {
      if (rpcPayload.size() - 2u != 5u) break;
      const uint16_t killerId = static_cast<uint16_t>(reader.readBits(16));
      const uint16_t victimId = static_cast<uint16_t>(reader.readBits(16));
      const uint8_t reason = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok() || victimId == 0xFFFFu) break;
      if (deathMessages_.size() >= kMaxDeathMessages) deathMessages_.pop_front();
      DeathMessage message;
      message.hasKiller = killerId != 0xFFFFu;
      message.killerId = killerId;
      message.victimId = victimId;
      message.reason = reason;
      message.expiresAtMs = monotonicMilliseconds() + kDeathMessagePresentationLifetimeMs;
      deathMessages_.push_back(message);
      setEvent(event, RpcEventKind::DeathMessageChanged, victimId, "SendDeathMessage");
      return true;
    }

    // Server chat/HUD line. Bound both line bytes and retained history before
    // allocation so a malformed peer cannot turn chat into unbounded memory.
    case kSendClientMessage: {
      constexpr size_t kMaxChatTextBytes = 144;
      constexpr size_t kMaxChatLines = 100;
      const uint32_t rgba = reader.readBits(32);
      const std::string text = readString32(reader, kMaxChatTextBytes);
      if (!reader.ok() || text.empty()) break;
      if (chatLines_.size() >= kMaxChatLines) chatLines_.pop_front();
      chatLines_.push_back({rgba, text});
      event.kind = RpcEventKind::ChatMessage;
      // Do not put untrusted text into diagnostic summaries; a future native
      // HUD consumes the retained payload, while logs remain bounded/safe.
      event.summary = "SendClientMessage";
      return true;
    }

    // Broadcast counterpart of an accepted PlayerRequestChatMessage. It uses
    // an u16 sender followed by the legacy u8-length string. This is rendered
    // through the same bounded HUD history but is never copied into logs.
    case kPlayerChatMessage: {
      constexpr size_t kMaxChatTextBytes = 128;
      constexpr size_t kMaxChatLines = 100;
      const uint16_t id = static_cast<uint16_t>(reader.readBits(16));
      const std::string message = readString8(reader, kMaxChatTextBytes);
      if (!reader.ok() || message.empty()) break;
      uint32_t rgba = 0xFFFFFFFFu;
      std::string sender;
      if (initialized_ && id == playerInit_.playerId) {
        sender = "YOU";
      } else if (const Player* player = findPlayer(id)) {
        rgba = player->rgba;
        sender = player->name.empty() ? "#" + std::to_string(id) : player->name;
      } else {
        sender = "#" + std::to_string(id);
      }
      if (chatLines_.size() >= kMaxChatLines) chatLines_.pop_front();
      chatLines_.push_back({rgba, sender + ": " + message});
      event.kind = RpcEventKind::ChatMessage;
      event.playerId = id;
      event.summary = "PlayerChatMessage";
      return true;
    }

    // Server checkpoint state is renderer-independent: retain a finite,
    // bounded-by-type world marker for a future Android HUD/world bridge.
    case kSetCheckpoint: {
      Checkpoint checkpoint;
      bool finite = true;
      checkpoint.x = readFloat(reader, finite);
      checkpoint.y = readFloat(reader, finite);
      checkpoint.z = readFloat(reader, finite);
      checkpoint.size = readFloat(reader, finite);
      if (!reader.ok() || !finite || checkpoint.size < 0.0f) break;
      checkpoint.active = true;
      checkpoint_ = checkpoint;
      event.kind = RpcEventKind::CheckpointChanged;
      event.summary = "SetCheckpoint";
      return true;
    }

    case kDisableCheckpoint:
      checkpoint_.active = false;
      event.kind = RpcEventKind::CheckpointChanged;
      event.summary = "DisableCheckpoint";
      return true;

    case kSetRaceCheckpoint: {
      RaceCheckpoint checkpoint;
      checkpoint.type = static_cast<uint8_t>(reader.readBits(8));
      bool finite = true;
      checkpoint.x = readFloat(reader, finite);
      checkpoint.y = readFloat(reader, finite);
      checkpoint.z = readFloat(reader, finite);
      checkpoint.nextX = readFloat(reader, finite);
      checkpoint.nextY = readFloat(reader, finite);
      checkpoint.nextZ = readFloat(reader, finite);
      checkpoint.size = readFloat(reader, finite);
      if (!reader.ok() || !finite || checkpoint.size < 0.0f) break;
      checkpoint.active = true;
      raceCheckpoint_ = checkpoint;
      event.kind = RpcEventKind::RaceCheckpointChanged;
      event.summary = "SetRaceCheckpoint";
      return true;
    }

    case kDisableRaceCheckpoint:
      raceCheckpoint_.active = false;
      event.kind = RpcEventKind::RaceCheckpointChanged;
      event.summary = "DisableRaceCheckpoint";
      return true;

    case kPlayerClose:
      // `PlayerClose` is an exact empty client-lifecycle control, not a world
      // reset, class response, or a reason to keep local sync alive. The
      // session transport receives this event and closes its peer socket.
      if (rpcPayload.size() != 2u) break;
      setEvent(event, RpcEventKind::SessionCloseRequested,
               initialized_ ? playerInit_.playerId : 0, "PlayerClose");
      return true;

    case kConnectionRejected: {
      // Legacy client source accepts only four one-byte rejection causes. Do
      // not preserve/print arbitrary peer text here; surface the verified
      // reason and stop the session instead of making it look like a timeout.
      if (rpcPayload.size() != 3u) break;
      const uint8_t reason = static_cast<uint8_t>(reader.readBits(8));
      const char* summary = reader.ok() ? connectionRejectionSummary(reason) : nullptr;
      if (summary == nullptr) break;
      setEvent(event, RpcEventKind::SessionCloseRequested,
               initialized_ ? playerInit_.playerId : 0, summary);
      return true;
    }

    // Legacy TogglePlayerSpectating is a uint32 boolean. It gates outgoing
    // local gameplay immediately; an optional following target comes through
    // RPC 126/127 and remains a visual stream anchor, never a local identity.
    case kTogglePlayerSpectating: {
      if (rpcPayload.size() - 2u != 4u) break;
      const uint32_t enabled = reader.readBits(32);
      if (!reader.ok() || enabled > 1u) break;
      localPlayer_.spectatingKnown = true;
      localPlayer_.spectating = enabled != 0u;
      if (!localPlayer_.spectating) {
        localPlayer_.spectateTargetKnown = false;
        localPlayer_.spectateTargetVehicle = false;
        localPlayer_.spectateTargetId = 0;
        localPlayer_.spectateMode = 0;
      }
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0, "TogglePlayerSpectating");
      event.localSpectateUpdated = true;
      return true;
    }

    // Legacy-compatible packet-list evidence gives `u16 target | u8
    // PlayerSpectateMode` for both target classes. Preserve the raw mode only;
    // normal/fixed/side
    // camera semantics must not create a guessed exact GTA offset here.
    case kPlayerSpectatePlayer:
    case kPlayerSpectateVehicle: {
      if (rpcPayload.size() - 2u != 3u) break;
      const uint16_t targetId = static_cast<uint16_t>(reader.readBits(16));
      const uint8_t mode = static_cast<uint8_t>(reader.readBits(8));
      if (!reader.ok()) break;
      localPlayer_.spectateTargetKnown = true;
      localPlayer_.spectateTargetVehicle = rpcId == kPlayerSpectateVehicle;
      localPlayer_.spectateTargetId = targetId;
      localPlayer_.spectateMode = mode;
      setEvent(event, RpcEventKind::LocalPlayerChanged,
               initialized_ ? playerInit_.playerId : 0,
               localPlayer_.spectateTargetVehicle ? "PlayerSpectateVehicle" : "PlayerSpectatePlayer");
      event.localSpectateUpdated = true;
      return true;
    }

    // Current open.mp ForcePlayerClassSelection writer has no body. It revokes
    // any stale class/spawn permission but does not invent a class, position,
    // health, or a local respawn; the next server class reply remains sole
    // selection authority.
    case kForcePlayerClassSelection:
      if (rpcPayload.size() != 2u) break;
      classSelection_ = {};
      classSelectionForced_ = true;
      beginSpawnRequest();
      setEvent(event, RpcEventKind::ClassSelection,
               initialized_ ? playerInit_.playerId : 0, "ForcePlayerClassSelection");
      return true;

    // Server reply to PlayerRequestClass. DL R1 inserts CustomModelID:u32
    // directly after ModelID; the remainder is byte-for-byte compatible.
    case kPlayerRequestClass: {
      ClassSelection selection;
      selection.selectable = reader.readBits(8) != 0;
      selection.team = static_cast<uint8_t>(reader.readBits(8));
      selection.model = reader.readBits(32);
      selection.customModel = usesDlR1WireLayout() ? reader.readBits(32) : 0u;
      reader.readBits(8);  // Unknown1
      bool finite = true;
      selection.x = readFloat(reader, finite);
      selection.y = readFloat(reader, finite);
      selection.z = readFloat(reader, finite);
      selection.angle = readFloat(reader, finite);
      for (uint32_t& weapon : selection.weapons) weapon = reader.readBits(32);
      for (uint32_t& ammo : selection.ammo) ammo = reader.readBits(32);
      if (!reader.ok() || !finite) break;
      selection.valid = true;
      classSelection_ = selection;
      classSelectionForced_ = false;
      event.kind = RpcEventKind::ClassSelection;
      event.summary = "ClassSelection model=" + std::to_string(selection.model) +
                      (selection.selectable ? " selectable" : " blocked");
      return true;
    }

    // SA-MP legacy sends one-byte Allow; open.mp's compatible writer sends a
    // u32. Both nonzero values permit the required follow-up PlayerSpawn RPC.
    case kPlayerRequestSpawn: {
      uint32_t allow = 0;
      const size_t bodyBytes = rpcPayload.size() - 2;
      if (bodyBytes == 1) {
        allow = reader.readBits(8);
      } else if (bodyBytes == 4) {
        allow = reader.readBits(32);
      } else {
        break;
      }
      if (!reader.ok()) break;
      spawnResponseKnown_ = true;
      spawnAllowed_ = allow != 0;
      event.kind = RpcEventKind::SpawnResponse;
      event.summary = spawnAllowed_ ? "SpawnResponse allowed" : "SpawnResponse denied";
      return true;
    }

    default:
      return false;
  }

  event.kind = RpcEventKind::Malformed;
  event.summary = "malformed ";
  event.summary += rpc::name(rpcId);
  return false;
}

}  // namespace samp::state
