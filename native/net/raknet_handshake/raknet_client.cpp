// raknet_client.cpp — RakNet 2.x (SA-MP) handshake client, written from scratch.
//
// Ini BUKAN RakNet 4.x. Server SA-MP/open.mp memakai RakNet 2.52 yang punya
// handshake sangat berbeda dari versi modern. Semua format di bawah diambil
// langsung dari source open.mp/RakNet (Include/raknet/PacketEnumerations.h,
// Source/RakPeer.cpp, Source/ReliabilityLayer.cpp).
//
// Handshake:
//   C -> S  3 byte   [24][0][0]                  ID_OPEN_CONNECTION_REQUEST
//   S -> C  2 byte   [25][0]                     ID_OPEN_CONNECTION_REPLY
//   C -> S  RELIABLE [11]                        ID_CONNECTION_REQUEST
//   S -> C  RELIABLE [34 ip port index token]    ID_CONNECTION_REQUEST_ACCEPTED
//
// Datagram RakNet 2.x:
//   bit 0        : hasAcks
//   [ack ranges] : compressed count + (bit isSingle + uint16 min [+ uint16 max])*
//   per packet   : (hasAcks ? bit 0 : -)
//                  uint16 messageNumber
//                  uint4  reliability        (RAKNET_LEGACY: UNRELIABLE=6, RELIABLE=8, ORDERED=9)
//                  [uint5 channel + uint16 orderIndex]  jika ordered/sequenced
//                  bit    isSplit [+ split header]
//                  compressed uint16 dataBitLength
//                  align ke byte, lalu data
//
// Build: g++ -O2 -std=c++17 -o rakclient raknet_client.cpp

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum : uint8_t {
  ID_PING = 7,
  ID_PONG = 9,
  ID_CONNECTION_REQUEST = 11,
  ID_AUTH_KEY = 12,
  ID_OPEN_CONNECTION_REQUEST = 24,
  ID_OPEN_CONNECTION_REPLY = 25,
  ID_OPEN_CONNECTION_COOKIE = 26,
  ID_NEW_INCOMING_CONNECTION = 30,
  ID_NO_FREE_INCOMING_CONNECTIONS = 31,
  ID_DISCONNECTION_NOTIFICATION = 32,
  ID_CONNECTION_LOST = 33,
  ID_CONNECTION_REQUEST_ACCEPTED = 34,
  ID_INVALID_PASSWORD = 28,
  ID_CONNECTION_ATTEMPT_FAILED = 29,
};

// RAKNET_LEGACY offsets (lihat PacketPriority.h: "SA:MP seems to like +6 offsets")
enum : uint8_t {
  REL_UNRELIABLE = 6,
  REL_UNRELIABLE_SEQUENCED = 7,
  REL_RELIABLE = 8,
  REL_RELIABLE_ORDERED = 9,
  REL_RELIABLE_SEQUENCED = 10,
};

// ---------------------------------------------------------------- bit writer
struct BitWriter {
  std::vector<uint8_t> buf;
  size_t bitPos = 0;

  void bit(bool v) {
    if (bitPos / 8 >= buf.size()) buf.push_back(0);
    if (v) buf[bitPos / 8] |= static_cast<uint8_t>(1u << (bitPos % 8));
    ++bitPos;
  }
  // tulis n bit, LSB-first (sama seperti RakNet WriteBits(..., true))
  void bits(uint32_t v, int n) {
    for (int i = 0; i < n; ++i) bit((v >> i) & 1u);
  }
  void align() { while (bitPos % 8) bit(false); }
  void bytes(const uint8_t* d, size_t n) {
    align();
    buf.insert(buf.end(), d, d + n);
    bitPos += n * 8;
  }
  // RakNet WriteCompressed(unsigned short)
  void compressed_u16(uint16_t v) {
    uint8_t hi = static_cast<uint8_t>(v >> 8);
    uint8_t lo = static_cast<uint8_t>(v & 0xFF);
    if (hi == 0) {
      bit(true);                       // high byte nol
      if ((lo & 0xF0) == 0) {
        bit(true); bits(lo & 0x0F, 4); // hanya 4 bit
      } else {
        bit(false); bits(lo, 8);
      }
    } else {
      bit(false); bits(v, 16);         // 16 bit penuh
    }
  }
};

// ---------------------------------------------------------------- bit reader
struct BitReader {
  const uint8_t* d;
  size_t n, bitPos = 0;
  bool ok = true;

  BitReader(const uint8_t* data, size_t size) : d(data), n(size) {}
  bool bit() {
    if (bitPos / 8 >= n) { ok = false; return false; }
    bool v = (d[bitPos / 8] >> (bitPos % 8)) & 1u;
    ++bitPos;
    return v;
  }
  uint32_t bits(int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; ++i)
      if (bit()) v |= (1u << i);
    return v;
  }
  void align() { while (bitPos % 8) bit(); }
  std::vector<uint8_t> bytes(size_t len) {
    align();
    if (bitPos / 8 + len > n) { ok = false; return {}; }
    std::vector<uint8_t> out(d + bitPos / 8, d + bitPos / 8 + len);
    bitPos += len * 8;
    return out;
  }
  uint16_t compressed_u16() {
    int byteIndex = 1;  // size 2 byte
    while (byteIndex > 0) {
      if (bit()) { --byteIndex; }
      else { return static_cast<uint16_t>(bits((byteIndex + 1) * 8)); }
    }
    if (bit()) return static_cast<uint16_t>(bits(4));
    return static_cast<uint16_t>(bits(8));
  }
};

// ------------------------------------------------- SA-MP packet obfuscation
// Semua paket SA-MP dibungkus: [checksum][byte yang sudah di-substitusi].
// Decrypt (server, SAMPRakNet.cpp):
//   cur = src[i]; if (!(i & 1)) cur ^= (port ^ 0xCC); cur = key[cur];
//   checksum ^= cur & 0xAA; out[i-1] = cur;
//   valid bila src[0] == checksum.
// Ini yang bikin paket RakNet polos di-drop oleh semua server SA-MP.
namespace samp_crypto {

static const uint8_t kKey[256] = {
#include "samp_crypto_table.inc"
};

struct Tables {
  uint8_t inv[256];
  Tables() {
    for (int i = 0; i < 256; ++i) inv[kKey[i]] = static_cast<uint8_t>(i);
  }
};
static const Tables g_tables;

inline uint8_t port_mask(uint16_t port) {
  return static_cast<uint8_t>((port ^ 0xCC) & 0xFF);
}

std::vector<uint8_t> wrap(const std::vector<uint8_t>& plain, uint16_t port) {
  std::vector<uint8_t> out(plain.size() + 1, 0);
  uint8_t checksum = 0;
  const uint8_t pm = port_mask(port);
  for (size_t i = 0; i < plain.size(); ++i) {
    const uint8_t p = plain[i];
    checksum ^= static_cast<uint8_t>(p & 0xAA);
    uint8_t t = g_tables.inv[p];
    const size_t idx = i + 1;               // 1-based
    if ((idx & 1) == 0) t ^= pm;
    out[idx] = t;
  }
  out[0] = checksum;
  return out;
}

// inverse, untuk memeriksa balasan (server membalas plaintext untuk client legacy)
std::vector<uint8_t> unwrap(const std::vector<uint8_t>& src, uint16_t port) {
  if (src.size() < 2) return {};
  uint8_t checksum = 0;
  const uint8_t pm = port_mask(port);
  std::vector<uint8_t> out(src.size() - 1);
  for (size_t i = 1; i < src.size(); ++i) {
    uint8_t cur = src[i];
    if ((i & 1) == 0) cur ^= pm;
    cur = kKey[cur];
    checksum ^= static_cast<uint8_t>(cur & 0xAA);
    out[i - 1] = cur;
  }
  if (checksum != src[0]) return {};
  return out;
}

}  // namespace samp_crypto

void hexdump(const char* label, const std::vector<uint8_t>& v, size_t max = 40) {
  std::printf("    %s (%zu B): ", label, v.size());
  for (size_t i = 0; i < v.size() && i < max; ++i) std::printf("%02X ", v[i]);
  if (v.size() > max) std::printf("...");
  std::printf("\n");
}

struct Net {
  int fd = -1;
  sockaddr_in to{};

  bool open(const std::string& host, uint16_t port, int timeout_ms = 2000) {
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) return false;
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
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

// Bangun satu datagram berisi satu internal packet.
std::vector<uint8_t> build_datagram(uint16_t msgNumber, uint8_t reliability,
                                    const std::vector<uint8_t>& payload) {
  BitWriter w;
  w.bit(false);                 // hasAcks = false
  w.bits(msgNumber, 16);        // message number
  w.bits(reliability, 4);       // reliability (4 bit, RAKNET_LEGACY)
  if (reliability == REL_RELIABLE_ORDERED || reliability == REL_RELIABLE_SEQUENCED ||
      reliability == REL_UNRELIABLE_SEQUENCED) {
    w.bits(0, 5);               // ordering channel
    w.bits(0, 16);              // ordering index
  }
  w.bit(false);                 // isSplit
  w.compressed_u16(static_cast<uint16_t>(payload.size() * 8));
  w.bytes(payload.data(), payload.size());
  return w.buf;
}

struct InternalPacket {
  uint16_t msgNumber = 0;
  uint8_t reliability = 0;
  std::vector<uint8_t> data;
};

std::vector<InternalPacket> parse_datagram(const std::vector<uint8_t>& d) {
  std::vector<InternalPacket> out;
  BitReader r(d.data(), d.size());
  bool hasAcks = r.bit();
  if (hasAcks) {
    uint16_t count = r.compressed_u16();
    for (uint16_t i = 0; i < count && r.ok; ++i) {
      bool single = r.bit();
      r.bits(16);                       // min index
      if (!single) r.bits(16);          // max index
    }
    if (!r.ok) return out;
  }
  while (r.ok) {
    if (hasAcks) r.bit();               // false bit penanda sebelum tiap packet
    if (!r.ok) break;
    InternalPacket p;
    p.msgNumber = static_cast<uint16_t>(r.bits(16));
    p.reliability = static_cast<uint8_t>(r.bits(4));
    if (p.reliability == REL_RELIABLE_ORDERED || p.reliability == REL_RELIABLE_SEQUENCED ||
        p.reliability == REL_UNRELIABLE_SEQUENCED) {
      r.bits(5);                        // channel
      r.bits(16);                       // ordering index
    }
    bool split = r.bit();
    if (split) {
      r.bits(16);                       // split id
      r.compressed_u16();               // split index
      r.compressed_u16();               // split count
    }
    uint16_t bitLen = r.compressed_u16();
    if (!r.ok || bitLen == 0) break;
    p.data = r.bytes((bitLen + 7) / 8);
    if (!r.ok || p.data.empty()) break;
    out.push_back(std::move(p));
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s <ip:port>\n", argv[0]);
    return 1;
  }
  std::string addr = argv[1];
  auto colon = addr.find(':');
  std::string host = addr.substr(0, colon);
  uint16_t port = static_cast<uint16_t>(std::atoi(addr.c_str() + colon + 1));

  Net net;
  if (!net.open(host, port, 2000)) { std::printf("socket error\n"); return 1; }

  std::printf("target: %s:%u\n\n", host.c_str(), port);
  std::vector<uint8_t> resp;

  // ---- step 1: OpenConnectionRequest + cookie challenge --------------------
  // Server SA-MP membalas [26][cookie_lo][cookie_hi] untuk IP kita.
  // Client harus mengirim ulang request dengan cookie ^ 0x6969 (SAMP)
  // atau cookie ^ 0x6D70 (open.mp). Ini anti-bot bawaan SA-MP.
  constexpr uint16_t SAMP_PETARDED = 0x6969;
  constexpr uint16_t OMP_PETARDED = 0x6D70;

  std::printf("[1a] -> ID_OPEN_CONNECTION_REQUEST  [18 00 00] (probe, tanpa cookie)\n");
  net.send(samp_crypto::wrap({ID_OPEN_CONNECTION_REQUEST, 0x00, 0x00}, port));
  if (net.recv(resp) <= 0) {
    std::printf("     !! timeout\n");
    return 2;
  }
  hexdump("<-", resp);

  uint16_t cookie = 0;
  bool isOmp = false;
  bool gotReply = false;

  for (int attempt = 0; attempt < 5 && !gotReply; ++attempt) {
    if (!(resp.size() >= 3 && resp[0] == ID_OPEN_CONNECTION_COOKIE)) break;
    cookie = static_cast<uint16_t>(resp[1] | (resp[2] << 8));
    std::printf("     cookie dari server: 0x%04X\n", cookie);

    // coba legacy dulu, lalu open.mp
    for (uint16_t magic : {static_cast<uint16_t>(SAMP_PETARDED),
                           static_cast<uint16_t>(OMP_PETARDED)}) {
      uint16_t x = static_cast<uint16_t>(cookie ^ magic);
      std::vector<uint8_t> p = {ID_OPEN_CONNECTION_REQUEST,
                                static_cast<uint8_t>(x & 0xFF),
                                static_cast<uint8_t>(x >> 8)};
      net.send(samp_crypto::wrap(p, port));
      int n = net.recv(resp);
      if (n > 0 && resp[0] == ID_OPEN_CONNECTION_REPLY) {
        hexdump("<-", resp);
        isOmp = (magic == OMP_PETARDED);
        gotReply = true;
        break;
      }
    }
  }

  if (!gotReply) {
    if (!(resp.size() >= 2 && resp[0] == ID_OPEN_CONNECTION_REPLY)) {
      hexdump("<-", resp);
      std::printf("     !! cookie ditolak\n");
      return 2;
    }
    hexdump("<-", resp);
  }

  std::printf("     diminta=%s  server menerima open connection request\n",
              isOmp ? "open.mp" : "SA-MP legacy");

  // ---- step 2: ConnectionRequest (reliable) --------------------------------
  std::vector<uint8_t> crPayload = {ID_CONNECTION_REQUEST};  // tanpa password
  auto dgram = build_datagram(1, REL_RELIABLE, crPayload);
  std::printf("\n[2] -> ID_CONNECTION_REQUEST (reliable, %zu B datagram)\n", dgram.size());
  hexdump("   ", dgram);
  net.send(samp_crypto::wrap(dgram, port));

  // ---- step 3: tunggu ACCEPTED ---------------------------------------------
  std::printf("\n[3] menunggu balasan...\n");
  for (int i = 0; i < 10; ++i) {
    if (net.recv(resp) <= 0) {
      std::printf("    (timeout)\n");
      break;
    }
    hexdump("<-", resp);
    // server membalas plaintext untuk client legacy; kalau bukan, coba unwrap
    if (!resp.empty() && resp[0] > 0x40 && resp[0] != 0x53) {
      auto unwrapped = samp_crypto::unwrap(resp, port);
      if (!unwrapped.empty()) {
        std::printf("    (ter-obfuscate, berhasil di-unwrap)\n");
        hexdump("   ", unwrapped);
        resp = unwrapped;
      }
    }
    auto packets = parse_datagram(resp);
    if (packets.empty()) {
      std::printf("    (datagram tanpa internal packet / ack saja)\n");
      continue;
    }
    for (const auto& p : packets) {
      std::printf("    packet rel=%u msgnum=%u len=%zu id=0x%02X\n",
                  p.reliability, p.msgNumber, p.data.size(),
                  p.data.empty() ? 0 : p.data[0]);
      if (p.data.empty()) continue;
      if (p.data[0] == ID_CONNECTION_REQUEST_ACCEPTED) {
        std::printf("\n    ===== CONNECTION REQUEST ACCEPTED =====\n");
        if (p.data.size() >= 13) {
          const uint8_t* q = p.data.data();
          uint32_t ip; uint16_t pport, index, token;
          std::memcpy(&ip, q + 1, 4);
          std::memcpy(&pport, q + 5, 2);
          std::memcpy(&index, q + 7, 2);
          std::memcpy(&token, q + 9, 4);
          in_addr a{}; a.s_addr = ip;
          char ipstr[32]{};
          ::inet_ntop(AF_INET, &a, ipstr, sizeof(ipstr));
          std::printf("    external address : %s:%u\n", ipstr, pport);
          std::printf("    player index     : %u\n", index);
          std::printf("    token            : 0x%08X\n", token);
        }
        std::printf("\n    RakNet 2.x handshake selesai — client ini resmi\n");
        std::printf("    terdaftar sebagai peer di server SA-MP.\n");
        return 0;
      }
      if (p.data[0] == ID_NO_FREE_INCOMING_CONNECTIONS) {
        std::printf("    !! server penuh\n");
        return 3;
      }
      if (p.data[0] == ID_INVALID_PASSWORD) {
        std::printf("    !! password server salah/kosong\n");
        return 3;
      }
      if (p.data[0] == ID_DISCONNECTION_NOTIFICATION) {
        std::printf("    !! server memutus koneksi\n");
        return 4;
      }
      if (p.data[0] == ID_AUTH_KEY) {
        std::printf("    server minta auth key (open.mp security) — langkah berikutnya\n");
      }
    }
  }
  std::printf("\n    !! tidak menerima ACCEPTED\n");
  return 5;
}
