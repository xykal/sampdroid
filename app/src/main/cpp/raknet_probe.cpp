// RakNet 2.x (SA-MP) native transport/session — written from scratch.
// Handshake + obfuscation + reliability datagram encoder + retained session.
// Probe tetap dipakai diagnostik; session eksplisit dipakai lifecycle Android native.
#include "raknet_probe.h"
#include "client_rpc.h"
#include "raknet_codec.h"
#include "raknet_session.h"
#include "rpc_decoder.h"
#include "rpc_state.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
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
// Provenance dan third-party notice data kompatibilitas ini ada di NOTICE.md.
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

// Shared codec lives in raknet_codec.{h,cpp}; keeping it separate lets CI run
// protocol regression tests on a normal host compiler as well as Android NDK.
using raknet::BitWriter;

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
};

struct Net {
  int fd = -1;
  sockaddr_in to{};

  Net() = default;
  Net(const Net&) = delete;
  Net& operator=(const Net&) = delete;

  bool open(const std::string& ip, uint16_t port, int timeoutMs) {
    close();
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) {
      close();
      return false;
    }
    return setTimeout(timeoutMs);
  }

  bool setTimeout(int timeoutMs) {
    if (fd < 0 || timeoutMs < 1) return false;
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
  }

  bool send(const std::vector<uint8_t>& p) const {
    if (fd < 0 || p.empty()) return false;
    const ssize_t sent = ::sendto(fd, p.data(), p.size(), 0,
                                  reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    return sent == static_cast<ssize_t>(p.size());
  }

  int recv(std::vector<uint8_t>& out) const {
    out.assign(2048, 0);
    sockaddr_in from{};
    socklen_t fromLength = sizeof(from);
    const int n = static_cast<int>(::recvfrom(fd, out.data(), out.size(), 0,
                                               reinterpret_cast<sockaddr*>(&from), &fromLength));
    if (n <= 0) {
      out.clear();
      return n;
    }
    // A socket can receive arbitrary UDP traffic. Only accept packets from the
    // endpoint chosen explicitly by the user; ignore off-path injections.
    if (from.sin_family != AF_INET || from.sin_port != to.sin_port ||
        from.sin_addr.s_addr != to.sin_addr.s_addr) {
      out.clear();
      return 0;
    }
    out.resize(static_cast<size_t>(n));
    return n;
  }

  void close() {
    if (fd >= 0) ::close(fd);
    fd = -1;
  }

  ~Net() { close(); }
};

struct PendingReliable {
  uint16_t messageNumber = 0;
  std::vector<uint8_t> wrappedPacket;
  std::chrono::steady_clock::time_point lastSent;
  uint8_t retries = 0;
};

// One explicit session is retained for the Android shell. It intentionally
// contains only network/state ownership: rendering and input live above this
// boundary, and a new connection replaces the old one atomically under mutex.
struct LiveSession {
  std::unique_ptr<Net> net;
  uint16_t port = 0;
  uint16_t nextMessageNumber = 0;
  uint16_t nextOrderingIndex = 0;
  bool accepted = false;
  bool joined = false;
  int receivedPackets = 0;
  int receivedRpcs = 0;
  int splitFragments = 0;
  int reassembledPackets = 0;
  std::string rpcSummary;
  std::string stateSummary;
  std::string error;
  std::vector<PendingReliable> pendingReliable;
  raknet::SplitReassembler reassembler;
  state::WorldState world;
};

std::unique_ptr<LiveSession> gLiveSession;
std::mutex gLiveSessionMutex;

// Payload ID_CONNECTION_REQUEST_ACCEPTED: [id][ip x4][port u16][idx u16][token u32]
bool parse_accepted(const std::vector<uint8_t>& payload, uint32_t& token) {
  if (payload.size() < 13 || payload[0] != ID_CONNECTION_REQUEST_ACCEPTED) return false;
  token = static_cast<uint32_t>(payload[9]) | (static_cast<uint32_t>(payload[10]) << 8) |
          (static_cast<uint32_t>(payload[11]) << 16) |
          (static_cast<uint32_t>(payload[12]) << 24);
  return true;
}

// RPC PlayerConnect (ClientJoin): id 20 / rpc 25
std::vector<uint8_t> build_client_join(uint32_t challengeResponse, const std::string& nick) {
  BitWriter w;
  w.writeBits(20, 8);   // ID_RPC
  w.writeBits(25, 8);   // RPC_ClientJoin / PlayerConnect
  w.writeBits(4057, 32);  // VersionNumber (0.3.7)
  w.writeBits(1, 8);      // Modded
  w.writeBits(static_cast<uint32_t>(nick.size()), 8);
  w.writeBytes(reinterpret_cast<const uint8_t*>(nick.data()), nick.size());
  w.writeBits(challengeResponse, 32);
  static const char kKey[] = "1234567890123456789012345678901234567890";  // 40 hex
  w.writeBits(40, 8);
  w.writeBytes(reinterpret_cast<const uint8_t*>(kKey), 40);
  static const char kVer[] = "0.3.7-R5";
  w.writeBits(9, 8);
  w.writeBytes(reinterpret_cast<const uint8_t*>(kVer), 9);
  return w.buffer;
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

void appendBounded(std::string& target, const std::string& value, size_t maxBytes) {
  if (value.empty() || target.size() + value.size() + 2 >= maxBytes) return;
  if (!target.empty()) target += ", ";
  target += value;
}

bool sendSessionReliable(LiveSession& session, const std::vector<uint8_t>& packet,
                         uint16_t messageNumber) {
  constexpr size_t kMaxPendingReliable = 32;
  if (session.pendingReliable.size() >= kMaxPendingReliable) {
    session.error = "antrian reliable penuh";
    return false;
  }

  PendingReliable pending;
  pending.messageNumber = messageNumber;
  pending.wrappedPacket = wrap_packet(packet, session.port);
  pending.lastSent = std::chrono::steady_clock::now();
  if (!session.net->send(pending.wrappedPacket)) {
    session.error = "gagal mengirim packet reliable";
    return false;
  }
  session.pendingReliable.push_back(std::move(pending));
  return true;
}

void consumeRemoteAcks(LiveSession& session, const std::vector<uint16_t>& messageNumbers) {
  if (messageNumbers.empty() || session.pendingReliable.empty()) return;
  session.pendingReliable.erase(
      std::remove_if(session.pendingReliable.begin(), session.pendingReliable.end(),
                     [&messageNumbers](const PendingReliable& pending) {
                       return std::find(messageNumbers.begin(), messageNumbers.end(),
                                        pending.messageNumber) != messageNumbers.end();
                     }),
      session.pendingReliable.end());
}

void resendDueReliable(LiveSession& session) {
  if (!session.net || session.net->fd < 0) return;
  constexpr auto kResendAfter = std::chrono::milliseconds(750);
  constexpr uint8_t kMaxRetries = 5;
  const auto now = std::chrono::steady_clock::now();
  for (PendingReliable& pending : session.pendingReliable) {
    if (now - pending.lastSent < kResendAfter) continue;
    if (pending.retries >= kMaxRetries) {
      session.error = "reliable packet tidak di-ACK server";
      session.net->close();
      return;
    }
    if (!session.net->send(pending.wrappedPacket)) {
      session.error = "gagal mengirim ulang packet reliable";
      session.net->close();
      return;
    }
    pending.lastSent = now;
    ++pending.retries;
  }
}

void processSessionDatagram(LiveSession& session, const std::vector<uint8_t>& bytes) {
  const raknet::Datagram datagram = raknet::decodeDatagram(bytes);
  consumeRemoteAcks(session, datagram.receivedAckMessageNumbers);
  if (!datagram.ackMessageNumbers.empty()) {
    session.net->send(wrap_packet(raknet::buildAck(datagram.ackMessageNumbers), session.port));
  }

  for (const auto& raw : datagram.packets) {
    if (raw.split) ++session.splitFragments;
    for (const auto& packet : session.reassembler.push(raw)) {
      if (packet.payload.empty()) continue;
      ++session.receivedPackets;
      if (packet.reassembled) ++session.reassembledPackets;

      const uint8_t id = packet.payload[0];
      if (id == ID_DISCONNECTION_NOTIFICATION) {
        session.error = "server memutus sesi";
        session.net->close();
        return;
      }
      if (id != 20 || packet.payload.size() < 2) continue;

      ++session.receivedRpcs;
      appendBounded(session.rpcSummary, rpc::summarize(packet.payload), 320);
      state::RpcEvent event;
      if (session.world.applyRpc(packet.payload, event)) {
        appendBounded(session.stateSummary, event.summary, 240);
      }
    }
  }
  session.joined = session.receivedPackets > 0;
}

SessionSnapshot snapshotFrom(const LiveSession* session) {
  SessionSnapshot snapshot;
  if (!session) {
    snapshot.error = "tidak ada sesi native aktif";
    return snapshot;
  }

  snapshot.active = session->net && session->net->fd >= 0;
  snapshot.accepted = session->accepted;
  snapshot.joined = session->joined;
  snapshot.playerInit = session->world.initialized();
  snapshot.classSelectionKnown = session->world.classSelection().valid;
  snapshot.classSelectable = session->world.classSelection().selectable;
  snapshot.spawnResponseKnown = session->world.spawnResponseKnown();
  snapshot.spawnAllowed = session->world.spawnAllowed();
  snapshot.playerId = snapshot.playerInit ? session->world.playerInit().playerId : 0;
  snapshot.classModel = snapshot.classSelectionKnown ? session->world.classSelection().model : 0;
  snapshot.receivedPackets = session->receivedPackets;
  snapshot.receivedRpcs = session->receivedRpcs;
  snapshot.splitFragments = session->splitFragments;
  snapshot.reassembledPackets = session->reassembledPackets;
  snapshot.trackedPlayers = static_cast<int>(session->world.playerCount());
  snapshot.chatMessages = static_cast<int>(session->world.chatCount());
  snapshot.serverName = snapshot.playerInit ? session->world.playerInit().serverName : "";
  snapshot.rpcSummary = session->rpcSummary;
  snapshot.stateSummary = session->stateSummary;
  snapshot.error = session->error;
  return snapshot;
}

}  // namespace

static ProbeResult raknet_connect_impl(const std::string& ip, uint16_t port, int timeoutMs,
                                       bool retainSession) {
  ProbeResult res;
  auto ownedNet = std::make_unique<Net>();
  Net& net = *ownedNet;
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
  net.send(wrap_packet(raknet::buildReliable(1, {ID_CONNECTION_REQUEST}), port));

  // ---- 4. AUTH_KEY -> ACCEPTED --------------------------------------------
  uint16_t msgNo = 2;
  uint32_t token = 0;
  bool gotChallenge = false;
  // One reassembler is kept for the entire session: a server can legally
  // split an RPC over more than one UDP datagram.
  raknet::SplitReassembler reassembler;
  for (int i = 0; i < 20 && !res.accepted; ++i) {
    if (net.recv(resp) <= 0) continue;
    raknet::Datagram dg = raknet::decodeDatagram(resp);
    if (!dg.ackMessageNumbers.empty()) {
      net.send(wrap_packet(raknet::buildAck(dg.ackMessageNumbers), port));
    }
    for (const auto& raw : dg.packets) {
      for (const auto& pkt : reassembler.push(raw)) {
        if (pkt.payload.empty()) continue;
        const uint8_t id = pkt.payload[0];
        if (id == ID_AUTH_KEY && pkt.payload.size() >= 3) {
          const uint8_t len = pkt.payload[1];
          std::string challenge(reinterpret_cast<const char*>(pkt.payload.data()) + 2,
                                len > 0 ? len - 1 : 0);
          logln(res, "    AUTH_KEY challenge: " + challenge);
          const char* answer = auth_lookup(challenge);
          if (!answer) {
            logln(res, "    challenge tidak ada di tabel ini");
            continue;
          }
          res.authenticated = true;
          std::vector<uint8_t> pl = {ID_AUTH_KEY,
                                     static_cast<uint8_t>(std::strlen(answer))};
          pl.insert(pl.end(), answer, answer + std::strlen(answer));
          net.send(wrap_packet(raknet::buildReliable(msgNo++, pl), port));
          logln(res, std::string("[4] AUTH_KEY response: ") + answer);
          gotChallenge = true;
        } else if (id == ID_CONNECTION_REQUEST_ACCEPTED) {
          if (parse_accepted(pkt.payload, token)) {
            res.accepted = true;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "    token server 0x%08X", token);
            logln(res, "    === CONNECTION REQUEST ACCEPTED ===");
            logln(res, buf);
          }
        } else if (id == ID_NO_FREE_INCOMING_CONNECTIONS) {
          res.error = "server penuh";
          return res;
        } else if (id == ID_DISCONNECTION_NOTIFICATION) {
          res.error = "server memutus koneksi";
          return res;
        }
      }
    }
  }

  if (!res.accepted) {
    res.error = gotChallenge ? "AUTH_KEY dijawab tapi ACCEPTED tidak datang"
                             : "tidak menerima AUTH_KEY challenge";
    return res;
  }

  // ---- 5. NEW_INCOMING + ClientJoin ---------------------------------------
  net.send(wrap_packet(raknet::buildReliable(msgNo++, {ID_NEW_INCOMING_CONNECTION}), port));

  const uint32_t challengeResponse = token ^ 4057u;
  const std::string nick = "SampDroid";
  auto join = build_client_join(challengeResponse, nick);
  net.send(wrap_packet(raknet::buildReliable(msgNo++, join), port));
  char jb[96];
  std::snprintf(jb, sizeof(jb), "[5] ClientJoin terkirim (nick %s, cr %u)",
                nick.c_str(), challengeResponse);
  logln(res, jb);

  // ---- 6. dengarkan stream server ----------------------------------------
  int rxPackets = 0;
  int rpcCount = 0;
  int splitFragments = 0;
  int reassembledPackets = 0;
  std::string rpcSummary;
  std::string stateSummary;
  state::WorldState world;
  for (int i = 0; i < 20; ++i) {
    if (net.recv(resp) <= 0) continue;
    raknet::Datagram dg = raknet::decodeDatagram(resp);
    if (!dg.ackMessageNumbers.empty()) {
      net.send(wrap_packet(raknet::buildAck(dg.ackMessageNumbers), port));
    }
    for (const auto& raw : dg.packets) {
      if (raw.split) ++splitFragments;
      for (const auto& pkt : reassembler.push(raw)) {
        if (pkt.payload.empty()) continue;
        ++rxPackets;
        if (pkt.reassembled) ++reassembledPackets;
        if (pkt.payload[0] != 20 || pkt.payload.size() < 2) continue;

        ++rpcCount;
        const std::string label = rpc::summarize(pkt.payload);
        if (rpcSummary.size() + label.size() + 2 < 320) {
          if (!rpcSummary.empty()) rpcSummary += ", ";
          rpcSummary += label;
        }

        state::RpcEvent stateEvent;
        if (world.applyRpc(pkt.payload, stateEvent) && !stateEvent.summary.empty() &&
            stateSummary.size() + stateEvent.summary.size() + 2 < 240) {
          if (!stateSummary.empty()) stateSummary += ", ";
          stateSummary += stateEvent.summary;
        }
      }
    }
  }

  res.receivedPackets = rxPackets;
  res.receivedRpcs = rpcCount;
  res.splitFragments = splitFragments;
  res.reassembledPackets = reassembledPackets;
  res.trackedPlayers = static_cast<int>(world.playerCount());
  res.rpcSummary = rpcSummary;
  res.stateSummary = stateSummary;
  res.playerInit = world.initialized();
  if (res.playerInit) {
    res.playerId = world.playerInit().playerId;
    res.serverName = world.playerInit().serverName;
  }

  res.joined = rxPackets > 0;
  char sb[512];
  std::snprintf(sb, sizeof(sb),
                "[6] %d paket diterima, %d RPC, split=%d/reassembled=%d (%s)",
                rxPackets, rpcCount, splitFragments, reassembledPackets,
                rpcSummary.empty() ? "belum ada RPC" : rpcSummary.c_str());
  logln(res, sb);
  if (res.playerInit) {
    std::snprintf(sb, sizeof(sb), "    PlayerInit: playerId=%u server=%s",
                  static_cast<unsigned>(res.playerId), res.serverName.c_str());
    logln(res, sb);
  }
  std::snprintf(sb, sizeof(sb), "    State reducer: %d tracked player(s)%s%s",
                res.trackedPlayers, stateSummary.empty() ? "" : " — ",
                stateSummary.empty() ? "" : stateSummary.c_str());
  logln(res, sb);
  if (rxPackets > 0) {
    logln(res, "");
    logln(res, "STATUS: ClientJoin diterima server, stream data berjalan");
  } else {
    logln(res, "");
    logln(res, "STATUS: tersambung sebagai peer, ClientJoin belum dibalas");
  }

  if (retainSession) {
    // raknet_session_connect holds gLiveSessionMutex across this implementation.
    // Move the opened UDP socket and all decoder state, rather than reopening
    // a second connection or losing ordering/message counters after ClientJoin.
    auto live = std::make_unique<LiveSession>();
    live->net = std::move(ownedNet);
    live->port = port;
    live->nextMessageNumber = msgNo;
    live->nextOrderingIndex = 0;
    live->accepted = res.accepted;
    live->joined = res.joined;
    live->receivedPackets = rxPackets;
    live->receivedRpcs = rpcCount;
    live->splitFragments = splitFragments;
    live->reassembledPackets = reassembledPackets;
    live->rpcSummary = std::move(rpcSummary);
    live->stateSummary = std::move(stateSummary);
    live->reassembler = std::move(reassembler);
    live->world = std::move(world);
    gLiveSession = std::move(live);
  }
  return res;
}

ProbeResult raknet_probe(const std::string& ip, uint16_t port, int timeoutMs) {
  return raknet_connect_impl(ip, port, timeoutMs, false);
}

SessionSnapshot raknet_session_connect(const std::string& ip, uint16_t port, int timeoutMs) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  gLiveSession.reset();

  const ProbeResult result = raknet_connect_impl(ip, port, timeoutMs, true);
  if (!gLiveSession) {
    SessionSnapshot snapshot;
    snapshot.accepted = result.accepted;
    snapshot.joined = result.joined;
    snapshot.playerInit = result.playerInit;
    snapshot.playerId = result.playerId;
    snapshot.serverName = result.serverName;
    snapshot.receivedPackets = result.receivedPackets;
    snapshot.receivedRpcs = result.receivedRpcs;
    snapshot.splitFragments = result.splitFragments;
    snapshot.reassembledPackets = result.reassembledPackets;
    snapshot.trackedPlayers = result.trackedPlayers;
    snapshot.rpcSummary = result.rpcSummary;
    snapshot.stateSummary = result.stateSummary;
    snapshot.error = result.error.empty() ? "gagal membuat sesi native" : result.error;
    return snapshot;
  }
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_poll(int timeoutMs) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession) return snapshotFrom(nullptr);
  if (!gLiveSession->net || gLiveSession->net->fd < 0) return snapshotFrom(gLiveSession.get());
  if (!gLiveSession->net->setTimeout(timeoutMs)) {
    gLiveSession->error = "gagal mengatur timeout sesi";
    return snapshotFrom(gLiveSession.get());
  }

  std::vector<uint8_t> packet;
  if (gLiveSession->net->recv(packet) > 0) {
    processSessionDatagram(*gLiveSession, packet);
  }
  resendDueReliable(*gLiveSession);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_request_class(uint16_t classId) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }

  const auto rpc = clientrpc::buildRequestClass(classId);
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->nextOrderingIndex++;
  const auto packet = raknet::buildReliableOrdered(messageNumber, 2, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim PlayerRequestClass";
  } else {
    appendBounded(gLiveSession->stateSummary,
                  "PlayerRequestClass #" + std::to_string(classId), 240);
  }
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_request_spawn() {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->world.classSelection().valid ||
      !gLiveSession->world.classSelection().selectable) {
    gLiveSession->error = "class belum dipilih atau ditolak server";
    return snapshotFrom(gLiveSession.get());
  }

  const auto rpc = clientrpc::buildRequestSpawn();
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->nextOrderingIndex++;
  const auto packet = raknet::buildReliableOrdered(messageNumber, 2, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim PlayerRequestSpawn";
  } else {
    appendBounded(gLiveSession->stateSummary, "PlayerRequestSpawn", 240);
  }
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_disconnect() {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  SessionSnapshot snapshot;
  if (gLiveSession && gLiveSession->net && gLiveSession->net->fd >= 0) {
    const auto goodbye = raknet::buildReliable(gLiveSession->nextMessageNumber++,
                                               {ID_DISCONNECTION_NOTIFICATION});
    gLiveSession->net->send(wrap_packet(goodbye, gLiveSession->port));
  }
  gLiveSession.reset();
  snapshot.stateSummary = "sesi native ditutup";
  return snapshot;
}

SessionSnapshot raknet_session_status() {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  return snapshotFrom(gLiveSession.get());
}

}  // namespace samp
