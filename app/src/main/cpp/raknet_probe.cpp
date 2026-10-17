// RakNet 2.x (SA-MP) connect probe — written from scratch.
// Handshake + obfuscation + reliability datagram encoder.
// Dipakai app untuk mendiagnosis kemampuan connect ke server tertentu.
#include "raknet_probe.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cstring>
#include <vector>

namespace samp {
namespace {

// ---- SA-MP packet obfuscation ---------------------------------------------
static const uint8_t kKey[256] = {
#include "samp_crypto_table.inc"
};

struct InvTable {
  uint8_t inv[256];
  InvTable() {
    for (int i = 0; i < 256; ++i) inv[kKey[i]] = static_cast<uint8_t>(i);
  }
};
const InvTable& inv_table() {
  static InvTable t;
  return t;
}

// ---- tabel autentikasi client SA-MP/open.mp ------------------------------
// Server mengirim challenge acak; client wajib membalas pasangan tetapnya.
// 256 entri, diekstrak dari openmultiplayer/RakNet (SAMPRakNet.cpp).
struct AuthPair {
  const char* send;
  const char* recv;
};
static const AuthPair kAuthTable[256] = {
#include "samp_auth_table.inc"
};

const char* auth_lookup(const std::string& challenge) {
  for (const auto& p : kAuthTable) {
    if (challenge == p.send) return p.recv;
  }
  return nullptr;
}

std::vector<uint8_t> wrap_packet(const std::vector<uint8_t>& plain, uint16_t port) {
  const uint8_t pm = static_cast<uint8_t>((port ^ 0xCC) & 0xFF);
  std::vector<uint8_t> out(plain.size() + 1, 0);
  uint8_t checksum = 0;
  const auto& t = inv_table().inv;
  for (size_t i = 0; i < plain.size(); ++i) {
    checksum ^= static_cast<uint8_t>(plain[i] & 0xAA);
    uint8_t v = t[plain[i]];
    const size_t idx = i + 1;
    if ((idx & 1) == 0) v ^= pm;
    out[idx] = v;
  }
  out[0] = checksum;
  return out;
}

// ---- bit writer ------------------------------------------------------------
struct BitWriter {
  std::vector<uint8_t> buf;
  size_t bitPos = 0;

  // RakNet menulis MSB-first di dalam tiap byte.
  void bit(bool v) {
    if (bitPos / 8 >= buf.size()) buf.push_back(0);
    if (v) buf[bitPos / 8] |= static_cast<uint8_t>(0x80 >> (bitPos % 8));
    ++bitPos;
  }
  // Nilai ditulis per byte mulai dari byte paling rendah (little-endian),
  // tiap byte MSB-first. Ini konvensi BitStream::WriteBits(..., true).
  void bits(uint32_t v, int n) {
    for (int c = 0; c < (n + 7) / 8; ++c) {
      const int s = (n - 8 * c) < 8 ? (n - 8 * c) : 8;
      const uint8_t b = static_cast<uint8_t>((v >> (8 * c)) & 0xFF);
      for (int i = 0; i < s; ++i) bit((b >> (s - 1 - i)) & 1u);
    }
  }
  void align() {
    while (bitPos % 8) bit(false);
  }
  void bytes(const uint8_t* d, size_t n) {
    align();
    buf.insert(buf.end(), d, d + n);
    bitPos += n * 8;
  }
  void compressed_u16(uint16_t v) {
    const uint8_t hi = static_cast<uint8_t>(v >> 8);
    const uint8_t lo = static_cast<uint8_t>(v & 0xFF);
    if (hi == 0) {
      bit(true);
      if ((lo & 0xF0) == 0) {
        bit(true);
        bits(lo & 0x0F, 4);
      } else {
        bit(false);
        bits(lo, 8);
      }
    } else {
      bit(false);
      bits(v, 16);
    }
  }
};

// ---- bit reader ------------------------------------------------------------
struct BitReader {
  const uint8_t* d;
  size_t n;
  size_t bitPos = 0;
  bool ok = true;

  BitReader(const uint8_t* data, size_t size) : d(data), n(size) {}
  bool bit() {
    if (bitPos / 8 >= n) {
      ok = false;
      return false;
    }
    bool v = (d[bitPos / 8] >> (7 - (bitPos % 8))) & 1u;
    ++bitPos;
    return v;
  }
  uint32_t bits(int count) {
    uint32_t v = 0;
    for (int c = 0; c < (count + 7) / 8 && ok; ++c) {
      const int s = (count - 8 * c) < 8 ? (count - 8 * c) : 8;
      uint32_t b = 0;
      for (int i = 0; i < s; ++i) b = (b << 1) | (bit() ? 1u : 0u);
      v |= b << (8 * c);
    }
    return v;
  }
  void align() {
    while (bitPos % 8) bit();
  }
  std::vector<uint8_t> bytes(size_t len) {
    align();
    if (bitPos / 8 + len > n) {
      ok = false;
      return {};
    }
    std::vector<uint8_t> out(d + bitPos / 8, d + bitPos / 8 + len);
    bitPos += len * 8;
    return out;
  }
  uint16_t compressed_u16() {
    int byteIndex = 1;
    while (byteIndex > 0) {
      if (bit()) {
        --byteIndex;
      } else {
        return static_cast<uint16_t>(bits((byteIndex + 1) * 8));
      }
    }
    if (bit()) return static_cast<uint16_t>(bits(4));
    return static_cast<uint16_t>(bits(8));
  }
};

enum : uint8_t {
  ID_CONNECTION_REQUEST = 11,
  ID_AUTH_KEY = 12,
  ID_NEW_INCOMING_CONNECTION = 30,
  ID_OPEN_CONNECTION_REQUEST = 24,
  ID_OPEN_CONNECTION_REPLY = 25,
  ID_OPEN_CONNECTION_COOKIE = 26,
  ID_NO_FREE_INCOMING_CONNECTIONS = 31,
  ID_DISCONNECTION_NOTIFICATION = 32,
  ID_CONNECTION_REQUEST_ACCEPTED = 34,
  REL_RELIABLE = 8,
};

struct Net {
  int fd = -1;
  sockaddr_in to{};

  bool open(const std::string& ip, uint16_t port, int timeoutMs) {
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) return false;
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
  }
  bool send(const std::vector<uint8_t>& p) {
    return ::sendto(fd, p.data(), p.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) >= 0;
  }
  int recv(std::vector<uint8_t>& out) {
    out.assign(2048, 0);
    int n = static_cast<int>(::recv(fd, out.data(), out.size(), 0));
    if (n > 0)
      out.resize(static_cast<size_t>(n));
    else
      out.clear();
    return n;
  }
  ~Net() {
    if (fd >= 0) ::close(fd);
  }
};

std::vector<uint8_t> build_reliable(uint16_t msgNumber, const std::vector<uint8_t>& payload) {
  BitWriter w;
  w.bit(false);                      // hasAcks
  w.bits(msgNumber, 16);             // message number
  w.bits(REL_RELIABLE, 4);           // reliability (RAKNET_LEGACY: 8)
  w.bit(false);                      // isSplit
  w.compressed_u16(static_cast<uint16_t>(payload.size() * 8));
  w.bytes(payload.data(), payload.size());
  return w.buf;
}

std::vector<uint8_t> parse_accepted(const std::vector<uint8_t>& dgram) {
  BitReader r(dgram.data(), dgram.size());
  bool hasAcks = r.bit();
  if (hasAcks) {
    uint16_t count = r.compressed_u16();
    for (uint16_t i = 0; i < count && r.ok; ++i) {
      bool single = r.bit();
      r.bits(16);
      if (!single) r.bits(16);
    }
  }
  while (r.ok) {
    if (hasAcks) r.bit();
    if (!r.ok) break;
    r.bits(16);  // message number
    uint8_t rel = static_cast<uint8_t>(r.bits(4));
    if (rel == 7 || rel == 9 || rel == 10) {
      r.bits(5);
      r.bits(16);
    }
    bool split = r.bit();
    if (split) {
      r.bits(16);
      r.compressed_u16();
      r.compressed_u16();
    }
    uint16_t bitLen = r.compressed_u16();
    if (!r.ok || bitLen == 0) break;
    auto data = r.bytes((bitLen + 7) / 8);
    if (!r.ok || data.empty()) break;
    return data;
  }
  return {};
}

std::string hex(const std::vector<uint8_t>& v, size_t max = 24) {
  std::string s;
  char buf[8];
  for (size_t i = 0; i < v.size() && i < max; ++i) {
    std::snprintf(buf, sizeof(buf), "%02X ", v[i]);
    s += buf;
  }
  if (v.size() > max) s += "...";
  return s;
}

void logln(ProbeResult& r, const std::string& line) {
  r.log += line;
  r.log += "\n";
}

}  // namespace

ProbeResult raknet_probe(const std::string& ip, uint16_t port, int timeoutMs) {
  ProbeResult res;
  Net net;
  if (!net.open(ip, port, timeoutMs)) {
    res.error = "socket error / invalid ip";
    return res;
  }

  logln(res, "target " + ip + ":" + std::to_string(port));

  std::vector<uint8_t> resp;

  // ---- 1. probe tanpa cookie ------------------------------------------------
  logln(res, "[1] OPEN_CONNECTION_REQUEST (probe)");
  net.send(wrap_packet({ID_OPEN_CONNECTION_REQUEST, 0x00, 0x00}, port));
  if (net.recv(resp) <= 0) {
    res.error = "timeout: server tidak membalas sama sekali";
    logln(res, "    timeout");
    return res;
  }
  logln(res, "    <- " + hex(resp));

  if (resp[0] == ID_OPEN_CONNECTION_REPLY) {
    res.openConnection = true;
    res.variant = "cache";
    logln(res, "    langsung OPEN_CONNECTION_REPLY (cookie sudah pernah dikirim)");
  } else if (resp[0] == ID_OPEN_CONNECTION_COOKIE && resp.size() >= 3) {
    uint16_t cookie = static_cast<uint16_t>(resp[1] | (resp[2] << 8));
    char cbuf[32];
    std::snprintf(cbuf, sizeof(cbuf), "    cookie challenge 0x%04X", cookie);
    logln(res, cbuf);

    // ---- 2. echo cookie (SA-MP dulu, lalu open.mp) ------------------------
    bool got = false;
    for (int attempt = 0; attempt < 6 && !got; ++attempt) {
      bool newCookie = false;
      const uint16_t variants[2] = {static_cast<uint16_t>(cookie ^ 0x6969),
                                    static_cast<uint16_t>(cookie ^ 0x6D70)};
      const char* names[2] = {"samp", "omp"};
      for (int i = 0; i < 2 && !got && !newCookie; ++i) {
        const uint16_t x = variants[i];
        std::vector<uint8_t> p = {ID_OPEN_CONNECTION_REQUEST,
                                  static_cast<uint8_t>(x & 0xFF),
                                  static_cast<uint8_t>(x >> 8)};
        logln(res, std::string("[2] echo cookie (") + names[i] + ")");
        net.send(wrap_packet(p, port));
        if (net.recv(resp) <= 0) continue;
        logln(res, "    <- " + hex(resp));
        if (resp[0] == ID_OPEN_CONNECTION_REPLY) {
          got = true;
          res.openConnection = true;
          res.cookieOk = true;
          res.variant = names[i];
          logln(res, std::string("    OPEN_CONNECTION_REPLY - server menerima (") + names[i] + ")");
        } else if (resp[0] == ID_OPEN_CONNECTION_COOKIE && resp.size() >= 3) {
          // cookie di-reseed server; ulangi dengan nilai terbaru
          cookie = static_cast<uint16_t>(resp[1] | (resp[2] << 8));
          newCookie = true;
          std::snprintf(cbuf, sizeof(cbuf), "    cookie baru 0x%04X", cookie);
          logln(res, cbuf);
        }
      }
    }
    if (!got) {
      res.error = "cookie challenge gagal";
      logln(res, "    cookie ditolak server");
      return res;
    }
  } else {
    res.error = "unexpected reply";
    logln(res, "    unexpected packet id " + std::to_string(resp[0]));
    return res;
  }

  // ---- 3. connection request ----------------------------------------------
  logln(res, "[3] CONNECTION_REQUEST (reliable, msg=1)");
  net.send(wrap_packet(build_reliable(1, {ID_CONNECTION_REQUEST}), port));

  // ---- 4. tantangan AUTH_KEY ----------------------------------------------
  uint16_t msgNo = 2;
  bool gotChallenge = false;
  for (int i = 0; i < 12 && !gotChallenge; ++i) {
    if (net.recv(resp) <= 0) continue;
    logln(res, "    <- " + hex(resp));
    auto pkt = parse_accepted(resp);
    if (pkt.empty()) {          // bisa jadi ack-only (E3 xx 00)
      if (resp.size() >= 3 && resp[0] == 0xE3) {
        logln(res, "       (ACK reliable)");
        continue;
      }
      continue;
    }
    if (pkt[0] == ID_AUTH_KEY && pkt.size() >= 2) {
      const uint8_t len = pkt[1];
      std::string challenge(reinterpret_cast<const char*>(pkt.data()) + 2,
                            len > 0 ? len - 1 : 0);
      logln(res, "    challenge: " + challenge);
      const char* answer = auth_lookup(challenge);
      if (!answer) {
        res.error = "challenge tidak dikenal (bukan tabel open.mp)";
        logln(res, "    challenge tidak ada di tabel (kemungkinan server SA-MP 0.3.7 legacy)");
        return res;
      }
      res.authenticated = true;
      std::vector<uint8_t> payload = {ID_AUTH_KEY,
                                      static_cast<uint8_t>(std::strlen(answer))};
      payload.insert(payload.end(), answer, answer + std::strlen(answer));
      logln(res, std::string("[4] AUTH_KEY response: ") + answer);
      net.send(wrap_packet(build_reliable(msgNo++, payload), port));
      gotChallenge = true;
    }
  }

  if (!gotChallenge) {
    res.error = "tidak menerima AUTH_KEY challenge";
    logln(res, "    tidak ada challenge auth");
    return res;
  }

  // ---- 5. tunggu ACCEPTED --------------------------------------------------
  logln(res, "[5] menunggu CONNECTION_REQUEST_ACCEPTED...");
  for (int i = 0; i < 20; ++i) {
    if (net.recv(resp) <= 0) continue;
    auto pkt = parse_accepted(resp);
    if (pkt.empty()) continue;
    logln(res, "    <- id 0x" + hex({pkt[0]}));
    if (pkt[0] == ID_CONNECTION_REQUEST_ACCEPTED) {
      res.accepted = true;
      logln(res, "    === CONNECTION REQUEST ACCEPTED ===");
      // selesaikan handshake RakNet
      std::vector<uint8_t> nic = {ID_NEW_INCOMING_CONNECTION};
      net.send(wrap_packet(build_reliable(msgNo++, nic), port));
      logln(res, "[6] NEW_INCOMING_CONNECTION terkirim — peer terdaftar");
      logln(res, "");
      logln(res, "STATUS: terhubung sebagai peer RakNet (belum join game)");
      return res;
    }
    if (pkt[0] == ID_NO_FREE_INCOMING_CONNECTIONS) {
      res.error = "server penuh";
      return res;
    }
    if (pkt[0] == ID_DISCONNECTION_NOTIFICATION) {
      res.error = "server memutus koneksi";
      return res;
    }
    if (pkt[0] == ID_AUTH_KEY) {
      // challenge ulang, diabaikan
      continue;
    }
  }

  res.error = "handshake berhenti di CONNECTION_REQUEST (belum accepted)";
  logln(res, "    belum menerima CONNECTION_REQUEST_ACCEPTED");
  return res;
}

}  // namespace samp
