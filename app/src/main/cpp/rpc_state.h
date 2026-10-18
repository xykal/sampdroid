// Small, renderer-independent state reducer for decoded player/chat server RPCs.
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
  SpawnResponse,
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
  bool spawnResponseKnown() const { return spawnResponseKnown_; }
  bool spawnAllowed() const { return spawnAllowed_; }

private:
  // SA-MP/open.mp player IDs are small in normal use; bound state regardless
  // so an untrusted peer cannot grow the model indefinitely with fake joins.
  Player* getOrCreate(uint16_t id);

  bool initialized_ = false;
  rpc::PlayerInitData playerInit_;
  ClassSelection classSelection_;
  bool spawnResponseKnown_ = false;
  bool spawnAllowed_ = false;
  std::map<uint16_t, Player> players_;
  std::deque<ChatLine> chatLines_;
};

}  // namespace samp::state
