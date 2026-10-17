// raknet_handshake.cpp — RakNet 2.52 offline handshake, written from scratch.
//
// Tujuan: membuktikan bahwa client buatan sendiri bisa melewati handshake
// RakNet milik server SA-MP/open.mp yang asli — tanpa memakai library RakNet
// sama sekali. Ini fondasi transport untuk client Android nanti.
//
// Alur (semua OFFLINE message, jadi tidak butuh reliability layer):
//   C -> S  OpenConnectionRequest1   (0x05)  magic + protocolVersion + padding
//   S -> C  OpenConnectionReply1     (0x06)  serverGuid + useSecurity + MTU
//   C -> S  OpenConnectionRequest2   (0x07)  serverAddr + MTU + clientGuid
//   S -> C  OpenConnectionReply2     (0x08)  serverGuid + clientAddr + MTU
//   C -> S  ConnectionRequest        (0x09)  clientGuid + time + doSecurity
//   S -> C  ConnectionRequestAccepted(0x10)  <- target milestone
//
// Build:  g++ -O2 -std=c++17 -o handshake raknet_handshake.cpp
// Usage:  ./handshake <ip:port> [protocol_version]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr uint8_t kMagic[16] = {0x00, 0xFF, 0xFF, 0x00, 0xFE, 0xFE, 0xFE, 0xFE,
                                0xFD, 0xFD, 0xFD, 0xFD, 0x12, 0x34, 0x56, 0x78};

enum : uint8_t {
  ID_OPEN_CONNECTION_REQUEST_1 = 0x05,
  ID_OPEN_CONNECTION_REPLY_1 = 0x06,
  ID_OPEN_CONNECTION_REQUEST_2 = 0x07,
  ID_OPEN_CONNECTION_REPLY_2 = 0x08,
  ID_CONNECTION_REQUEST = 0x09,
  ID_CONNECTION_REQUEST_ACCEPTED = 0x10,
  ID_NEW_INCOMING_CONNECTION = 0x13,
  ID_INCOMPATIBLE_PROTOCOL_VERSION = 0x1A,
  ID_NO_FREE_INCOMING_CONNECTIONS = 0x14,
};

const char* name_of(uint8_t id) {
  switch (id) {
    case ID_OPEN_CONNECTION_REQUEST_1: return "OpenConnectionRequest1";
    case ID_OPEN_CONNECTION_REPLY_1: return "OpenConnectionReply1";
    case ID_OPEN_CONNECTION_REQUEST_2: return "OpenConnectionRequest2";
    case ID_OPEN_CONNECTION_REPLY_2: return "OpenConnectionReply2";
    case ID_CONNECTION_REQUEST: return "ConnectionRequest";
    case ID_CONNECTION_REQUEST_ACCEPTED: return "ConnectionRequestAccepted";
    case ID_NEW_INCOMING_CONNECTION: return "NewIncomingConnection";
    case ID_INCOMPATIBLE_PROTOCOL_VERSION: return "INCOMPATIBLE_PROTOCOL_VERSION";
    case ID_NO_FREE_INCOMING_CONNECTIONS: return "NO_FREE_INCOMING_CONNECTIONS";
    default: return "?";
  }
}

void hexdump(const char* label, const uint8_t* d, size_t n, size_t max = 48) {
  std::printf("    %s (%zu bytes): ", label, n);
  for (size_t i = 0; i < n && i < max; ++i) std::printf("%02X ", d[i]);
  if (n > max) std::printf("...");
  std::printf("\n");
}

// big-endian helpers
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint64_t be64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}
void put_be16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x & 0xFF));
}
void put_be64(std::vector<uint8_t>& v, uint64_t x) {
  for (int i = 7; i >= 0; --i) v.push_back(static_cast<uint8_t>((x >> (i * 8)) & 0xFF));
}
void put_magic(std::vector<uint8_t>& v) { v.insert(v.end(), kMagic, kMagic + 16); }

struct Net {
  int fd = -1;
  sockaddr_in to{};

  bool open(const std::string& host, uint16_t port) {
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) return false;
    timeval tv{};
    tv.tv_sec = 2;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
  }
  bool send(const std::vector<uint8_t>& p) {
    return ::sendto(fd, p.data(), p.size(), 0,
                    reinterpret_cast<sockaddr*>(&to), sizeof(to)) >= 0;
  }
  int recv(std::vector<uint8_t>& out) {
    out.assign(2048, 0);
    int n = static_cast<int>(::recv(fd, out.data(), out.size(), 0));
    if (n > 0) out.resize(static_cast<size_t>(n)); else out.clear();
    return n;
  }
  ~Net() { if (fd >= 0) ::close(fd); }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s <ip:port> [protocol_version=5]\n", argv[0]);
    return 1;
  }

  std::string addr = argv[1];
  auto colon = addr.find(':');
  std::string host = addr.substr(0, colon);
  uint16_t port = static_cast<uint16_t>(std::atoi(addr.c_str() + colon + 1));
  int version = (argc > 2) ? std::atoi(argv[2]) : 5;

  Net net;
  if (!net.open(host, port)) { std::printf("socket error\n"); return 1; }

  // guid client: 8 byte acak
  uint64_t guid;
  std::random_device rd;
  guid = (static_cast<uint64_t>(rd()) << 32) ^ rd();
  std::printf("client guid  : %016llX\n", static_cast<unsigned long long>(guid));
  std::printf("target       : %s:%u\n", host.c_str(), port);
  std::printf("proto version: %d\n\n", version);

  std::vector<uint8_t> pkt, resp;

  // ---- 1. OpenConnectionRequest1 -------------------------------------------
  pkt = {ID_OPEN_CONNECTION_REQUEST_1};
  put_magic(pkt);
  pkt.push_back(static_cast<uint8_t>(version));
  // MTU probe: total datagram 1472 byte (= MTU 1500 - 28)
  pkt.resize(1472, 0x00);
  std::printf("[1] -> OpenConnectionRequest1 (MTU probe 1472)\n");
  net.send(pkt);

  int n = net.recv(resp);
  if (n <= 0) { std::printf("    !! no reply (timeout)\n"); return 2; }
  hexdump("<-", resp.data(), resp.size());

  if (resp[0] == ID_INCOMPATIBLE_PROTOCOL_VERSION) {
    std::printf("    !! server menolak versi %d, versi yang didukung: %d\n",
                version, resp.size() > 17 ? resp[17] : -1);
    return 3;
  }
  if (resp[0] != ID_OPEN_CONNECTION_REPLY_1) {
    std::printf("    !! unexpected: %s (0x%02X)\n", name_of(resp[0]), resp[0]);
    return 3;
  }

  uint64_t serverGuid = be64(resp.data() + 17);
  uint8_t useSecurity = resp[25];
  uint16_t mtu = be16(resp.data() + 26);
  std::printf("    serverGuid=%016llX useSecurity=%u mtu=%u\n",
              static_cast<unsigned long long>(serverGuid), useSecurity, mtu);

  if (useSecurity) {
    std::printf("\n    !! server minta security (RSA proof) — belum diimplementasi.\n");
    std::printf("       Ini akan jadi langkah berikutnya kalau dibutuhkan.\n");
  }

  // ---- 2. OpenConnectionRequest2 -------------------------------------------
  pkt.clear();
  pkt.push_back(ID_OPEN_CONNECTION_REQUEST_2);
  put_magic(pkt);
  // server address: 4 byte IP + 2 byte port (big endian)
  {
    in_addr a{};
    ::inet_pton(AF_INET, host.c_str(), &a);
    const auto* b = reinterpret_cast<const uint8_t*>(&a.s_addr);
    for (int i = 0; i < 4; ++i) pkt.push_back(b[i]);
  }
  put_be16(pkt, port);
  put_be16(pkt, mtu ? mtu : 1492);
  put_be64(pkt, guid);
  std::printf("\n[2] -> OpenConnectionRequest2\n");
  net.send(pkt);

  n = net.recv(resp);
  if (n <= 0) { std::printf("    !! no reply (timeout)\n"); return 4; }
  hexdump("<-", resp.data(), resp.size());
  if (resp[0] != ID_OPEN_CONNECTION_REPLY_2) {
    std::printf("    !! unexpected: %s (0x%02X)\n", name_of(resp[0]), resp[0]);
    return 4;
  }
  std::printf("    koneksi UDP tercatat di server\n");

  // ---- 3. ConnectionRequest ------------------------------------------------
  pkt.clear();
  pkt.push_back(ID_CONNECTION_REQUEST);
  put_magic(pkt);
  put_be64(pkt, guid);
  {
    // timestamp: waktu sekarang dalam ms sejak epoch, big endian
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    put_be64(pkt, static_cast<uint64_t>(ms));
  }
  pkt.push_back(0);  // doSecurity = false
  std::printf("\n[3] -> ConnectionRequest (doSecurity=0)\n");
  net.send(pkt);

  // server bisa kirim beberapa paket; cari accepted
  for (int attempt = 0; attempt < 6; ++attempt) {
    n = net.recv(resp);
    if (n <= 0) break;
    hexdump("<-", resp.data(), resp.size());
    uint8_t id = resp[0];
    if (id == ID_CONNECTION_REQUEST_ACCEPTED) {
      std::printf("\n    === CONNECTION ACCEPTED ===\n");
      std::printf("    RakNet handshake selesai. Server menerima client ini.\n");
      std::printf("    system index: %u\n", resp.size() > 23 ? be16(resp.data() + 22) : 0);
      return 0;
    }
    if (id == ID_NO_FREE_INCOMING_CONNECTIONS) {
      std::printf("    !! server penuh\n");
      return 5;
    }
    std::printf("    (paket lain: %s)\n", name_of(id));
  }

  std::printf("\n    !! tidak menerima ConnectionRequestAccepted\n");
  std::printf("       Kemungkinan: server menolak diam-diam, butuh security, atau\n");
  std::printf("       format ConnectionRequest beda (RakNet 2.52 vs versi lain).\n");
  return 6;
}
