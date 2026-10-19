// SA-MP / open.mp query protocol implementation (opcodes i, r, c, d, p).
// Portable: dipakai di Android (JNI) dan bisa dites di desktop.
#include "samp_query.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

namespace samp {
namespace {

struct Reader {
  const uint8_t* p;
  size_t n;
  size_t off = 0;
  bool ok = true;

  Reader(const uint8_t* d, size_t size) : p(d), n(size) {}
  bool need(size_t k) {
    if (!ok || off + k > n) {
      ok = false;
      return false;
    }
    return true;
  }
  uint8_t u8() {
    if (!need(1)) return 0;
    return p[off++];
  }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v = static_cast<uint16_t>(p[off]) | static_cast<uint16_t>(p[off + 1]) << 8;
    off += 2;
    return v;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = static_cast<uint32_t>(p[off]) | static_cast<uint32_t>(p[off + 1]) << 8 |
                 static_cast<uint32_t>(p[off + 2]) << 16 | static_cast<uint32_t>(p[off + 3]) << 24;
    off += 4;
    return v;
  }
  std::string str(size_t len) {
    if (!need(len)) return {};
    std::string s(reinterpret_cast<const char*>(p + off), len);
    off += len;
    return s;
  }
};

std::string clean(std::string s) {
  for (char& c : s) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) c = ' ';
  }
  return s;
}

// JNI NewStringUTF and JSON both require well-formed text. Query/RPC strings
// originate on the network, so preserve valid UTF-8 but replace an isolated or
// malformed high byte before it can cross either boundary.
size_t validUtf8Width(const std::string& value, size_t index) {
  const uint8_t first = static_cast<uint8_t>(value[index]);
  size_t width = 0;
  if (first >= 0xC2 && first <= 0xDF) width = 2;
  if (first >= 0xE0 && first <= 0xEF) width = 3;
  if (first >= 0xF0 && first <= 0xF4) width = 4;
  if (width == 0 || index + width > value.size()) return 0;
  for (size_t part = 1; part < width; ++part) {
    if ((static_cast<uint8_t>(value[index + part]) & 0xC0u) != 0x80u) return 0;
  }
  const uint8_t second = static_cast<uint8_t>(value[index + 1]);
  if ((width == 3 && ((first == 0xE0 && second < 0xA0) ||
                      (first == 0xED && second >= 0xA0))) ||
      (width == 4 && ((first == 0xF0 && second < 0x90) ||
                      (first == 0xF4 && second >= 0x90)))) {
    return 0;
  }
  return width;
}

bool send_query(const std::string& ip, uint16_t port, char opcode,
                const std::vector<uint8_t>& extra, int timeoutMs,
                std::vector<uint8_t>& resp) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;

  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) {
    ::close(fd);
    return false;
  }

  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  std::vector<uint8_t> pkt = {'S', 'A', 'M', 'P'};
  const auto* b = reinterpret_cast<const uint8_t*>(&to.sin_addr.s_addr);
  for (int i = 0; i < 4; ++i) pkt.push_back(b[i]);
  pkt.push_back(static_cast<uint8_t>(port & 0xFF));
  pkt.push_back(static_cast<uint8_t>((port >> 8) & 0xFF));
  pkt.push_back(static_cast<uint8_t>(opcode));
  pkt.insert(pkt.end(), extra.begin(), extra.end());

  bool ok = ::sendto(fd, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) >= 0;
  if (ok) {
    resp.assign(4096, 0);
    sockaddr_in from{};
    socklen_t fromlen = sizeof(from);
    ssize_t got = ::recvfrom(fd, resp.data(), resp.size(), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromlen);
    ok = got > 11 && std::memcmp(resp.data(), "SAMP", 4) == 0;
    if (ok) resp.resize(static_cast<size_t>(got));
  }
  ::close(fd);
  return ok;
}

}  // namespace

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = static_cast<uint8_t>(s[i]);
    switch (c) {
      case '"': out += "\\\""; ++i; break;
      case '\\': out += "\\\\"; ++i; break;
      case '\n': out += "\\n"; ++i; break;
      case '\r': out += "\\r"; ++i; break;
      case '\t': out += "\\t"; ++i; break;
      default:
        // JSON strings may not contain raw control bytes. Server-provided
        // names/rules and decoded RPC strings are untrusted network input.
        if (c < 0x20) {
          char escaped[7];
          std::snprintf(escaped, sizeof(escaped), "\\u%04X", static_cast<unsigned>(c));
          out += escaped;
          ++i;
        } else if (c < 0x80) {
          out += static_cast<char>(c);
          ++i;
        } else {
          const size_t width = validUtf8Width(s, i);
          if (width == 0) {
            out += '?';
            ++i;
          } else {
            out.append(s, i, width);
            i += width;
          }
        }
    }
  }
  return out;
}

ServerInfo query_info(const std::string& ip, uint16_t port, int timeoutMs) {
  ServerInfo info;
  std::vector<uint8_t> resp;
  if (!send_query(ip, port, 'i', {}, timeoutMs, resp)) {
    info.error = "no response";
    return info;
  }
  Reader rd(resp.data() + 11, resp.size() - 11);
  info.password = rd.u8() != 0;
  info.players = rd.u16();
  info.maxPlayers = rd.u16();
  info.hostname = clean(rd.str(rd.u32()));
  info.gamemode = clean(rd.str(rd.u32()));
  info.language = clean(rd.str(rd.u32()));
  info.ok = rd.ok;
  if (!rd.ok) info.error = "parse error";
  return info;
}

std::vector<Rule> query_rules(const std::string& ip, uint16_t port, int timeoutMs) {
  std::vector<Rule> out;
  std::vector<uint8_t> resp;
  if (!send_query(ip, port, 'r', {}, timeoutMs, resp)) return out;
  Reader rd(resp.data() + 11, resp.size() - 11);
  uint16_t count = rd.u16();
  for (uint16_t i = 0; i < count && rd.ok; ++i) {
    Rule r;
    r.name = clean(rd.str(rd.u8()));
    r.value = clean(rd.str(rd.u8()));
    if (!rd.ok) break;
    out.push_back(std::move(r));
  }
  return out;
}

std::vector<PlayerEntry> query_players(const std::string& ip, uint16_t port, bool detailed,
                                       int timeoutMs) {
  std::vector<PlayerEntry> out;
  std::vector<uint8_t> resp;
  if (!send_query(ip, port, detailed ? 'd' : 'c', {}, timeoutMs, resp)) return out;
  Reader rd(resp.data() + 11, resp.size() - 11);
  uint16_t count = rd.u16();
  for (uint16_t i = 0; i < count && rd.ok; ++i) {
    PlayerEntry p;
    if (detailed) p.id = rd.u8();
    p.name = clean(rd.str(rd.u8()));
    p.score = static_cast<int>(rd.u32());
    if (detailed) p.ping = static_cast<int>(rd.u32());
    if (!rd.ok) break;
    out.push_back(std::move(p));
  }
  return out;
}

int query_ping(const std::string& ip, uint16_t port, int timeoutMs) {
  std::mt19937 rng(std::random_device{}());
  std::vector<uint8_t> payload(4);
  for (auto& b : payload) b = static_cast<uint8_t>(rng() & 0xFF);

  auto t0 = std::chrono::steady_clock::now();
  std::vector<uint8_t> resp;
  if (!send_query(ip, port, 'p', payload, timeoutMs, resp)) return -1;
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
  if (resp.size() < 15 || std::memcmp(resp.data() + 11, payload.data(), 4) != 0) return -1;
  return static_cast<int>(us / 1000);
}

}  // namespace samp
