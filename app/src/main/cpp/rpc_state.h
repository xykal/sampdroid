// Small, renderer-independent state reducer for decoded player/chat/marker server RPCs.
#pragma once

#include "rpc_decoder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace samp::state {

enum class RpcEventKind : uint8_t {
  Ignored,
  PlayerInitialized,
  PlayerJoined,
  PlayerQuit,
  PlayerRenamed,
  PlayerColorChanged,
  ChatMessage,
  ClassSelection,
  SpawnInfoChanged,
  SpawnResponse,
  LocalPlayerChanged,
  CheckpointChanged,
  RaceCheckpointChanged,
  Malformed,
};

struct RpcEvent {
  RpcEventKind kind = RpcEventKind::Ignored;
  uint16_t playerId = 0;
  std::string summary;
};

struct Player {
  uint16_t id = 0;
  uint32_t rgba = 0;
  bool npc = false;
  std::string name;
};

struct ChatLine {
  uint32_t rgba = 0;
  std::string text;
};

struct Checkpoint {
  bool active = false;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float size = 0.0f;
};

struct RaceCheckpoint {
  bool active = false;
  uint8_t type = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float nextX = 0.0f;
  float nextY = 0.0f;
  float nextZ = 0.0f;
  float size = 0.0f;
};

struct ClassSelection {
  bool valid = false;
  bool selectable = false;
  uint8_t team = 0;
  uint32_t model = 0;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  std::array<uint32_t, 3> weapons{};
  std::array<uint32_t, 3> ammo{};
};

// Bounded local-player facts supplied by authoritative server RPCs. This is
// intentionally renderer-neutral: the Android world/input bridge consumes it
// later, while the network reducer owns validation and lifetime.
struct LocalPlayer {
  bool spawnInfoKnown = false;
  bool positionKnown = false;
  bool positionFindZ = false;
  bool facingKnown = false;
  bool healthKnown = false;
  bool armourKnown = false;
  bool velocityKnown = false;
  bool specialActionKnown = false;

  uint8_t team = 0;
  uint32_t model = 0;
  uint8_t spawnUnknown = 0;
  std::array<uint32_t, 3> spawnWeapons{};
  std::array<uint32_t, 3> spawnAmmo{};

  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float angle = 0.0f;
  float health = 100.0f;
  float armour = 0.0f;
  uint8_t specialAction = 0;
  float velocityX = 0.0f;
  float velocityY = 0.0f;
  float velocityZ = 0.0f;
};

// This has no rendering or socket responsibility. It is the stable boundary
// between untrusted network bytes and a future HUD/world renderer.
class WorldState {
public:
  bool applyRpc(const std::vector<uint8_t>& rpcPayload, RpcEvent& event);

  bool initialized() const { return initialized_; }
  const rpc::PlayerInitData& playerInit() const { return playerInit_; }
  size_t playerCount() const { return players_.size(); }
  const Player* findPlayer(uint16_t id) const;
  size_t chatCount() const { return chatLines_.size(); }
  const ChatLine* latestChat() const {
    return chatLines_.empty() ? nullptr : &chatLines_.back();
  }
  const ClassSelection& classSelection() const { return classSelection_; }
  const LocalPlayer& localPlayer() const { return localPlayer_; }
  const Checkpoint& checkpoint() const { return checkpoint_; }
  const RaceCheckpoint& raceCheckpoint() const { return raceCheckpoint_; }
  bool spawnResponseKnown() const { return spawnResponseKnown_; }
  bool spawnAllowed() const { return spawnAllowed_; }

  // Called only after an outbound PlayerRequestSpawn is queued successfully;
  // stale permission from a prior attempt must not unlock local movement.
  void beginSpawnRequest();

private:
  // SA-MP/open.mp player IDs are small in normal use; bound state regardless
  // so an untrusted peer cannot grow the model indefinitely with fake joins.
  Player* getOrCreate(uint16_t id);

  bool initialized_ = false;
  rpc::PlayerInitData playerInit_;
  ClassSelection classSelection_;
  LocalPlayer localPlayer_;
  Checkpoint checkpoint_;
  RaceCheckpoint raceCheckpoint_;
  bool spawnResponseKnown_ = false;
  bool spawnAllowed_ = false;
  std::map<uint16_t, Player> players_;
  std::deque<ChatLine> chatLines_;
};

}  // namespace samp::state
