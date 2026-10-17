// samp_query.cpp — SA-MP / open.mp server query client (portable C++17)
// Implements the SA-MP query protocol: opcodes i, r, c, d, p
// Ref: https://open.mp/docs/tutorials/QueryMechanism
//
// Build:  g++ -O2 -std=c++17 -o sampquery samp_query.cpp
// Usage:  ./sampquery <ip:port> [opcode]
//         ./sampquery 127.0.0.1:7777 i
//         ./sampquery 127.0.0.1:7777 all
//
// This file is deliberately dependency-free so the exact same code can be
// compiled with the Android NDK (arm64-v8a) and dropped into libsampandroid.so.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Reader {
  const uint8_t* p;
  size_t n;
  size_t off = 0;
  bool ok = true;

  Reader(const uint8_t* data, size_t size) : p(data), n(size) {}

  bool need(size_t k) {
    if (!ok || off + k > n) { ok = false; return false; }
    return true;
  }
  uint8_t u8() {
    if (!need(1)) return 0;
    return p[off++];
  }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v = static_cast<uint16_t>(p[off]) |
                 static_cast<uint16_t>(p[off + 1]) << 8;
    off += 2;
    return v;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = static_cast<uint32_t>(p[off]) |
                 static_cast<uint32_t>(p[off + 1]) << 8 |
                 static_cast<uint32_t>(p[off + 2]) << 16 |
                 static_cast<uint32_t>(p[off + 3]) << 24;
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

std::string sanitize(std::string s) {
  for (char& c : s) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) c = '?';
  }
  return s;
}

bool parse_addr(const std::string& in, std::string& host, uint16_t& port) {
  auto pos = in.find(':');
  if (pos == std::string::npos) { host = in; port = 7777; return true; }
  host = in.substr(0, pos);
  int p = std::atoi(in.c_str() + pos + 1);
  if (p <= 0 || p > 65535) return false;
  port = static_cast<uint16_t>(p);
  return true;
}

// "SAMP" + ip octets + port low + port high + opcode
std::vector<uint8_t> build_packet(const std::string& host, uint16_t port,
                                  char opcode,
                                  const std::vector<uint8_t>& payload = {}) {
  std::vector<uint8_t> pkt;
  pkt.insert(pkt.end(), {'S', 'A', 'M', 'P'});

  in_addr a{};
  if (inet_pton(AF_INET, host.c_str(), &a) != 1) return {};
  const auto* b = reinterpret_cast<const uint8_t*>(&a.s_addr);
  for (int i = 0; i < 4; ++i) pkt.push_back(b[i]);  // octets as-is (network order)

  pkt.push_back(static_cast<uint8_t>(port & 0xFF));
  pkt.push_back(static_cast<uint8_t>((port >> 8) & 0xFF));
  pkt.push_back(static_cast<uint8_t>(opcode));
  pkt.insert(pkt.end(), payload.begin(), payload.end());
  return pkt;
}

struct Socket {
  int fd = -1;
  Socket() { fd = ::socket(AF_INET, SOCK_DGRAM, 0); }
  ~Socket() { if (fd >= 0) ::close(fd); }
  bool valid() const { return fd >= 0; }
};

bool send_recv(const std::string& host, uint16_t port,
               const std::vector<uint8_t>& pkt, std::vector<uint8_t>& out,
               int timeout_ms = 1500) {
  Socket s;
  if (!s.valid()) return false;

  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) return false;

  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (::sendto(s.fd, pkt.data(), pkt.size(), 0,
               reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0)
    return false;

  out.assign(4096, 0);
  sockaddr_in from{};
  socklen_t fromlen = sizeof(from);
  ssize_t got = ::recvfrom(s.fd, out.data(), out.size(), 0,
                           reinterpret_cast<sockaddr*>(&from), &fromlen);
  if (got <= 0) { out.clear(); return false; }
  out.resize(static_cast<size_t>(got));

  // response must start with "SAMP"
  if (out.size() < 11 || std::memcmp(out.data(), "SAMP", 4) != 0) return false;
  return true;
}

void print_info(const std::vector<uint8_t>& r) {
  Reader rd(r.data() + 11, r.size() - 11);
  bool password = rd.u8() != 0;
  uint16_t players = rd.u16();
  uint16_t maxplayers = rd.u16();
  std::string hostname = sanitize(rd.str(rd.u32()));
  std::string gamemode = sanitize(rd.str(rd.u32()));
  std::string language = sanitize(rd.str(rd.u32()));
  if (!rd.ok) { std::cout << "  [parse error: info]\n"; return; }

  std::cout << "  " << std::left << std::setw(10) << "hostname " << hostname << "\n"
            << "  " << std::setw(10) << "gamemode " << gamemode << "\n"
            << "  " << std::setw(10) << "language " << language << "\n"
            << "  " << std::setw(10) << "players  " << players << " / " << maxplayers << "\n"
            << "  " << std::setw(10) << "password " << (password ? "yes" : "no") << "\n";
}

void print_rules(const std::vector<uint8_t>& r) {
  Reader rd(r.data() + 11, r.size() - 11);
  uint16_t count = rd.u16();
  std::cout << "  rules(" << count << "):\n";
  for (uint16_t i = 0; i < count && rd.ok; ++i) {
    std::string name = sanitize(rd.str(rd.u8()));
    std::string value = sanitize(rd.str(rd.u8()));
    if (!rd.ok) break;
    std::cout << "    " << std::setw(22) << name << " = " << value << "\n";
  }
}

void print_players(const std::vector<uint8_t>& r, char opcode) {
  Reader rd(r.data() + 11, r.size() - 11);
  uint16_t count = rd.u16();
  std::cout << "  players(" << count << "):\n";
  for (uint16_t i = 0; i < count && rd.ok; ++i) {
    if (opcode == 'd') {
      uint8_t id = rd.u8();
      std::string name = sanitize(rd.str(rd.u8()));
      int32_t score = static_cast<int32_t>(rd.u32());
      uint32_t ping = rd.u32();
      if (!rd.ok) break;
      std::cout << "    [" << std::setw(3) << static_cast<int>(id) << "] "
                << std::setw(28) << std::left << name << std::right
                << " score " << std::setw(6) << score
                << " ping " << ping << "\n";
    } else {
      std::string name = sanitize(rd.str(rd.u8()));
      int32_t score = static_cast<int32_t>(rd.u32());
      if (!rd.ok) break;
      std::cout << "    " << std::setw(28) << std::left << name << std::right
                << " score " << score << "\n";
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cout <<
      "SA-MP / open.mp query client\n"
      "usage: " << (argc ? argv[0] : "sampquery") << " <ip:port> [i|r|c|d|p|all]\n";
    return 1;
  }

  std::string host;
  uint16_t port = 0;
  if (!parse_addr(argv[1], host, port)) {
    std::cerr << "bad address\n";
    return 1;
  }
  std::string what = (argc > 2) ? argv[2] : "i";
  std::cout << "-> " << host << ":" << port << "\n";

  auto query = [&](char op, const std::vector<uint8_t>& payload = {}) {
    std::vector<uint8_t> pkt = build_packet(host, port, op, payload);
    std::vector<uint8_t> resp;
    if (!send_recv(host, port, pkt, resp)) {
      std::cout << "  [" << op << "] no response\n";
      return false;
    }
    std::cout << "  [" << op << "] " << resp.size() << " bytes\n";
    switch (op) {
      case 'i': print_info(resp); break;
      case 'r': print_rules(resp); break;
      case 'c':
      case 'd': print_players(resp, op); break;
      case 'p':
        std::cout << "    echo: " << sanitize(std::string(
            reinterpret_cast<const char*>(resp.data() + 11),
            resp.size() > 11 ? resp.size() - 11 : 0)) << "\n";
        break;
    }
    return true;
  };

  if (what == "all") {
    for (char op : {'i', 'r', 'c', 'd'}) query(op);
  } else if (!what.empty()) {
    query(what[0]);
  }

  // ping measurement
  if (what == "p" || what == "all") {
    std::mt19937 rng(std::random_device{}());
    std::vector<uint8_t> payload(4);
    for (auto& b : payload) b = static_cast<uint8_t>(rng() & 0xFF);
    std::vector<uint8_t> pkt = build_packet(host, port, 'p', payload);
    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> resp;
    if (send_recv(host, port, pkt, resp)) {
      auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
      bool echo_ok = resp.size() >= 15 &&
                     std::memcmp(resp.data() + 11, payload.data(), 4) == 0;
      std::cout << "  ping: " << (us / 1000.0) << " ms (echo "
                << (echo_ok ? "ok" : "MISMATCH") << ")\n";
    } else {
      std::cout << "  ping: timeout\n";
    }
  }
  return 0;
}
