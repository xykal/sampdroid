#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace samp {

struct ServerInfo {
  bool ok = false;
  std::string error;
  bool password = false;
  int players = 0;
  int maxPlayers = 0;
  std::string hostname;
  std::string gamemode;
  std::string language;
  int pingMs = -1;
};

struct Rule {
  std::string name;
  std::string value;
};

struct PlayerEntry {
  int id = 0;
  std::string name;
  int score = 0;
  int ping = -1;
};

ServerInfo query_info(const std::string& ip, uint16_t port, int timeoutMs = 1500);
std::vector<Rule> query_rules(const std::string& ip, uint16_t port, int timeoutMs = 1500);
std::vector<PlayerEntry> query_players(const std::string& ip, uint16_t port,
                                       bool detailed, int timeoutMs = 1500);
int query_ping(const std::string& ip, uint16_t port, int timeoutMs = 1500);

std::string json_escape(const std::string& s);

}  // namespace samp
