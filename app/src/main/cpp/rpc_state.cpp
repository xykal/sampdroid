// Renderer-independent state reducer for player, class/spawn, chat, and marker RPCs.
#include "rpc_state.h"

#include "raknet_codec.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace samp::state {
namespace {

constexpr uint8_t kIdRpc = 20;
constexpr uint8_t kSetPlayerName = 11;
constexpr uint8_t kDisableCheckpoint = 37;
constexpr uint8_t kSetRaceCheckpoint = 38;
constexpr uint8_t kDisableRaceCheckpoint = 39;
constexpr uint8_t kSetPlayerColor = 72;
constexpr uint8_t kSendClientMessage = 93;
constexpr uint8_t kSetCheckpoint = 107;
constexpr uint8_t kPlayerJoin = 137;
constexpr uint8_t kPlayerQuit = 138;
constexpr uint8_t kPlayerRequestClass = 128;
constexpr uint8_t kPlayerRequestSpawn = 129;
constexpr uint8_t kPlayerInit = 139;
constexpr size_t kMaxPlayerNameBytes = 24;

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

float readFloat(raknet::BitReader& reader, bool& finite) {
  const uint32_t raw = reader.readBits(32);
  float value = 0.0f;
  static_assert(sizeof(value) == sizeof(raw), "SA-MP uses 32-bit float values");
  std::memcpy(&value, &raw, sizeof(value));
  finite = finite && std::isfinite(value);
  return value;
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
  if (inserted) it->second.id = id;
  return &it->second;
}

const Player* WorldState::findPlayer(uint16_t id) const {
  const auto it = players_.find(id);
  return it == players_.end() ? nullptr : &it->second;
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
    initialized_ = true;
    setEvent(event, RpcEventKind::PlayerInitialized, playerInit_.playerId, "PlayerInit");
    return true;
  }

  raknet::BitReader reader(rpcPayload.data() + 2, rpcPayload.size() - 2);
  switch (rpcId) {
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
      players_.erase(id);
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

    // Server reply to PlayerRequestClass. This client advertises non-DL, so
    // CustomModelID is intentionally absent from the wire layout here.
    case kPlayerRequestClass: {
      ClassSelection selection;
      selection.selectable = reader.readBits(8) != 0;
      selection.team = static_cast<uint8_t>(reader.readBits(8));
      selection.model = reader.readBits(32);
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
      event.kind = RpcEventKind::ClassSelection;
      event.summary = "ClassSelection model=" + std::to_string(selection.model) +
                      (selection.selectable ? " selectable" : " blocked");
      return true;
    }

    // Server reply to PlayerRequestSpawn: uint32 Allow.
    case kPlayerRequestSpawn: {
      const uint32_t allow = reader.readBits(32);
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
