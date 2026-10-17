// Small, renderer-independent state reducer for decoded server RPCs.
#pragma once

#include "rpc_decoder.h"

#include <cstddef>
#include <cstdint>
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

// This has no rendering or socket responsibility. It is the stable boundary
// between untrusted network bytes and a future HUD/world renderer.
class WorldState {
public:
  bool applyRpc(const std::vector<uint8_t>& rpcPayload, RpcEvent& event);

  bool initialized() const { return initialized_; }
  const rpc::PlayerInitData& playerInit() const { return playerInit_; }
  size_t playerCount() const { return players_.size(); }
  const Player* findPlayer(uint16_t id) const;

private:
  // SA-MP/open.mp player IDs are small in normal use; bound state regardless
  // so an untrusted peer cannot grow the model indefinitely with fake joins.
  Player* getOrCreate(uint16_t id);

  bool initialized_ = false;
  rpc::PlayerInitData playerInit_;
  std::map<uint16_t, Player> players_;
};

}  // namespace samp::state
