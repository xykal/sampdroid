// RakNet 2.x (SA-MP) native transport/session — written from scratch.
// Handshake + obfuscation + reliability datagram encoder + retained session.
// Probe tetap dipakai diagnostik; session eksplisit dipakai lifecycle Android native.
#include "raknet_probe.h"
#include "client_rpc.h"
#include "client_sync.h"
#include "input_state.h"
#include "local_gameplay_bridge.h"
#include "local_player_sync.h"
#include "raknet_codec.h"
#include "raknet_handshake.h"
#include "raknet_session.h"
#include "raknet_session_policy.h"
#include "sync_policy.h"
#include "sync_decoder.h"
#include "rpc_decoder.h"
#include "rpc_state.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
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
  // Values match the SA-MP-compatible RakNet 2.52 identifier table.
  ID_INTERNAL_PING = 6,
  ID_CONNECTED_PONG = 9,
  ID_REQUEST_STATIC_DATA = 10,
  ID_CONNECTION_REQUEST = 11,
  ID_AUTH_KEY = 12,
  ID_DETECT_LOST_CONNECTIONS = 23,
  ID_NEW_INCOMING_CONNECTION = 30,
  ID_OPEN_CONNECTION_REQUEST = 24,
  ID_OPEN_CONNECTION_REPLY = 25,
  ID_OPEN_CONNECTION_COOKIE = 26,
  ID_NO_FREE_INCOMING_CONNECTIONS = 31,
  ID_DISCONNECTION_NOTIFICATION = 32,
  ID_CONNECTION_REQUEST_ACCEPTED = 34,
  ID_RECEIVED_STATIC_DATA = 41,
};

std::string describeOpenConnectionReply(uint8_t id, size_t bytes) {
  if (id == ID_NO_FREE_INCOMING_CONNECTIONS) {
    return "server penuh sebelum open connection";
  }
  if (handshake::isTerminalSessionControlId(id)) {
    return "server menolak sebelum open connection (RakNet id " +
           std::to_string(static_cast<unsigned>(id)) + ")";
  }
  return "balasan RakNet tak dikenal sebelum open connection (id " +
         std::to_string(static_cast<unsigned>(id)) + ", " +
         std::to_string(bytes) + " byte)";
}

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

  // The first poll receive may wait for the caller's bounded socket timeout;
  // subsequent burst receives must never stall input/render threads behind an
  // already available UDP backlog.
  int recv(std::vector<uint8_t>& out, bool nonBlocking = false) const {
    out.assign(2048, 0);
    sockaddr_in from{};
    socklen_t fromLength = sizeof(from);
    const int flags = nonBlocking ? MSG_DONTWAIT : 0;
    const int n = static_cast<int>(::recvfrom(fd, out.data(), out.size(), flags,
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

enum class ReliablePurpose : uint8_t {
  Generic,
  SpawnConfirmation,
};

struct PendingReliable {
  uint16_t messageNumber = 0;
  ReliablePurpose purpose = ReliablePurpose::Generic;
  std::vector<uint8_t> wrappedPacket;
  std::chrono::steady_clock::time_point lastSent;
  uint8_t retries = 0;
};

// The real RakPeer reliability layer tracks transport ACKs from the very first
// CONNECTION_REQUEST, not only after a UI-owned session is retained. Keep this
// small bounded equivalent during the synchronous connection phase, then move
// any still-outstanding records into LiveSession at hand-off.
bool queueTrackedReliable(uint16_t port, std::vector<PendingReliable>& pending,
                          const std::vector<uint8_t>& packet, uint16_t messageNumber,
                          ReliablePurpose purpose = ReliablePurpose::Generic) {
  constexpr size_t kMaxHandshakePendingReliable = 8u;
  if (packet.empty() || pending.size() >= kMaxHandshakePendingReliable) return false;
  PendingReliable record;
  record.messageNumber = messageNumber;
  record.purpose = purpose;
  record.wrappedPacket = wrap_packet(packet, port);
  record.lastSent = std::chrono::steady_clock::now();
  pending.push_back(std::move(record));
  return true;
}

bool sendTrackedReliable(Net& net, uint16_t port, std::vector<PendingReliable>& pending,
                         const std::vector<uint8_t>& packet, uint16_t messageNumber,
                         ReliablePurpose purpose = ReliablePurpose::Generic) {
  if (!queueTrackedReliable(port, pending, packet, messageNumber, purpose)) return false;
  if (net.send(pending.back().wrappedPacket)) return true;
  pending.pop_back();
  return false;
}

void consumeTrackedReliableAcks(std::vector<PendingReliable>& pending,
                                const std::vector<uint16_t>& messageNumbers) {
  if (pending.empty() || messageNumbers.empty()) return;
  pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const PendingReliable& record) {
    return std::find(messageNumbers.begin(), messageNumbers.end(), record.messageNumber) !=
           messageNumbers.end();
  }), pending.end());
}

bool resendTrackedReliableDue(Net& net, std::vector<PendingReliable>& pending,
                              std::string& error) {
  constexpr auto kResendAfter = std::chrono::milliseconds(750);
  constexpr uint8_t kMaxRetries = 5u;
  const auto now = std::chrono::steady_clock::now();
  for (PendingReliable& record : pending) {
    if (now - record.lastSent < kResendAfter) continue;
    if (record.retries >= kMaxRetries) {
      error = "reliable handshake tidak di-ACK server";
      return false;
    }
    if (!net.send(record.wrappedPacket)) {
      error = "gagal mengirim ulang reliable handshake";
      return false;
    }
    record.lastSent = now;
    ++record.retries;
  }
  return true;
}

// One explicit session is retained for the Android shell. It intentionally
// contains only network/state ownership: rendering and input live above this
// boundary, and a new connection replaces the old one atomically under mutex.
struct LiveSession {
  std::unique_ptr<Net> net;
  uint16_t port = 0;
  uint16_t nextMessageNumber = 0;
  // SA-MP/open.mp uses SyncPacket channel 1 for USeq movement and SyncRPC
  // channel 2 for reliable application RPC. RakNet owns separate
  // ordered/sequenced counters; keep the two local counter families distinct.
  syncpolicy::OrderingCounters orderingCounters;
  bool accepted = false;
  bool joined = false;
  int receivedPackets = 0;
  int receivedRpcs = 0;
  int splitFragments = 0;
  int reassembledPackets = 0;
  int reliableKeepalives = 0;
  int connectedPongs = 0;
  int outboundOnFootPackets = 0;
  int outboundInCarPackets = 0;
  int outboundAimPackets = 0;
  int outboundNoTargetBulletPackets = 0;
  // Successful local socket sends whose no-target form contained only a
  // bounded static-COL HitPos. This is presentation feedback, never a server
  // receipt, target, or damage result.
  uint64_t lastNoTargetStaticWorldImpactMs = 0;
  int outboundPassengerPackets = 0;
  int outboundChatMessages = 0;
  int outboundDialogResponses = 0;
  int outboundPickupNotifications = 0;
  int outboundVehicleInteractions = 0;
  int outboundTextDrawSelections = 0;
  int outboundObjectSelections = 0;
  int outboundObjectEdits = 0;
  int outboundAttachedObjectEdits = 0;
  int outboundScoreRequests = 0;
  uint64_t lastInteractionUseMs = 0;
  uint64_t lastObjectSelectionMs = 0;
  uint64_t lastObjectSelectionRevision = 0;
  // One click is terminal for a server BeginObjectSelect revision. The packet
  // itself stays in reliable retry state; a second Android tap must wait for a
  // fresh server lifecycle rather than enqueue competing callbacks.
  uint64_t objectSelectionTerminalRevision = 0;
  uint64_t lastObjectEditUpdateMs = 0;
  // Rate limiting is revision-bound too: a new BeginObjectEdit must not inherit
  // a short update cooldown from a now-stale editor lifecycle.
  uint64_t objectEditUpdateRevision = 0;
  // A Final/Cancel response is terminal for one server edit revision. Wait for
  // a new begin/cancel lifecycle rather than queueing conflicting replies.
  uint64_t objectEditTerminalRevision = 0;
  // RPC 116 takes one terminal save/cancel response; pin it to a server edit
  // revision so rapid Android taps cannot enqueue conflicting responses.
  uint64_t attachedObjectEditTerminalRevision = 0;
  uint64_t lastScoreRequestMs = 0;
  int remoteOnFootPackets = 0;
  int remoteAimPackets = 0;
  int remoteBulletPackets = 0;
  int remoteVehiclePackets = 0;
  // Transport diagnostics stay monotonic and never influence gameplay state.
  // They help an Android user distinguish an alive quiet server from a stalled
  // reliable queue without pretending an RTT is server-authoritative ping.
  std::chrono::steady_clock::time_point lastInboundAt = std::chrono::steady_clock::now();
  uint32_t latestReliableRttMs = 0;
  uint32_t smoothedReliableRttMs = 0;
  int reliableResends = 0;
  bool spawnConfirmationSent = false;
  bool spawnConfirmationAcked = false;
  // Transport ACK only proves the request/confirmation reached RakNet. Keep a
  // separate response-pending gate so repeated Android respawn taps cannot
  // enqueue duplicate PlayerRequestSpawn RPCs before the server replies.
  bool spawnRequestPending = false;
  std::chrono::steady_clock::time_point spawnRequestSent = std::chrono::steady_clock::now();
  bool classRequestPending = false;
  uint16_t requestedClassId = 0;
  std::chrono::steady_clock::time_point classRequestSent = std::chrono::steady_clock::now();
  bool nativeControlsKnown = false;
  bool onFootPreviewReady = false;
  bool inCarPreviewReady = false;
  bool aimPreviewReady = false;
  bool passengerPreviewReady = false;
  bool localOnFootStateKnown = false;
  bool localInCarStateKnown = false;
  bool localAimStateKnown = false;
  bool localPassengerStateKnown = false;
  uint64_t lastOnFootSentMs = 0;
  uint64_t lastInCarSentMs = 0;
  uint64_t lastAimSentMs = 0;
  uint64_t lastNoTargetBulletSentMs = 0;
  uint64_t lastPassengerSentMs = 0;
  input::OnFootInputMapper controls;
  // Owns only collision-validated local prediction; WorldState remains the
  // source of truth for every server-authored correction.
  gameplay::LocalGameplayState localGameplay;
  sync::OnFootState onFootPreview;
  sync::InCarState inCarPreview;
  sync::AimState aimPreview;
  sync::PassengerState passengerPreview;
  sync::OnFootState localOnFootState;
  sync::InCarState localInCarState;
  sync::AimState localAimState;
  sync::PassengerState localPassengerState;
  std::chrono::steady_clock::time_point lastReliableSent = std::chrono::steady_clock::now();
  std::string rpcSummary;
  std::string syncSummary;
  std::string stateSummary;
  std::string error;
  std::vector<PendingReliable> pendingReliable;
  // First-stage RakNet receipt history drops retransmitted reliable message
  // numbers before a duplicate can re-enter split assembly or control paths.
  raknet::ReliableMessageGate inboundReliable;
  raknet::SplitReassembler reassembler;
  // Packet ACK proves transport receipt, not that packets arrived in order.
  // Keep reliable RPCs behind the bounded ordered gate before WorldState sees
  // them; USeq sync keeps its separate stale-frame gate below.
  raknet::ReliableOrderedGate inboundOrdered;
  raknet::SequencedPacketGate inboundSequenced;
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

// ClientJoin and the exact seven-byte NEW_INCOMING record are built by the
// isolated handshake module so host regressions can prove their wire layout.

uint32_t currentMonotonicMilliseconds32() {
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

bool reply_to_internal_ping(Net& net, uint16_t port, uint16_t& nextMessageNumber,
                            const std::vector<uint8_t>& payload) {
  if (payload.size() != 5u || payload[0] != ID_INTERNAL_PING) return false;
  BitWriter pong;
  pong.writeBits(ID_CONNECTED_PONG, 8);
  pong.writeBits(static_cast<uint32_t>(payload[1]) |
                     (static_cast<uint32_t>(payload[2]) << 8u) |
                     (static_cast<uint32_t>(payload[3]) << 16u) |
                     (static_cast<uint32_t>(payload[4]) << 24u),
                 32);
  pong.writeBits(currentMonotonicMilliseconds32(), 32);
  const auto frame = raknet::buildUnreliable(nextMessageNumber++, pong.buffer);
  return !frame.empty() && net.send(wrap_packet(frame, port));
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

// RPC 113 sends attachment colours as ABGR, while RPC 116's current reader
// calls Colour::FromARGB. Preserve alpha/green and swap only red/blue before
// re-emitting the server-owned slot colours.
uint32_t abgrToArgb(uint32_t colourAbgr) {
  return (colourAbgr & 0xFF00FF00u) |
         ((colourAbgr & 0x000000FFu) << 16u) |
         ((colourAbgr & 0x00FF0000u) >> 16u);
}

// Only an explicit server health value of zero is treated as dead. It gates
// locally originated movement/vehicle/aim packets without fabricating a death
// report, GTA animation, or respawn; the normal RequestSpawn handshake stays
// server-authoritative.
bool serverMarkedDead(const state::LocalPlayer& local) {
  return local.healthKnown && std::isfinite(local.health) && local.health <= 0.0f;
}

bool sendSessionReliable(LiveSession& session, const std::vector<uint8_t>& packet,
                         uint16_t messageNumber,
                         ReliablePurpose purpose = ReliablePurpose::Generic) {
  constexpr size_t kMaxPendingReliable = 32;
  if (session.pendingReliable.size() >= kMaxPendingReliable) {
    session.error = "antrian reliable penuh";
    return false;
  }

  PendingReliable pending;
  pending.messageNumber = messageNumber;
  pending.purpose = purpose;
  pending.wrappedPacket = wrap_packet(packet, session.port);
  pending.lastSent = std::chrono::steady_clock::now();
  if (!session.net->send(pending.wrappedPacket)) {
    session.error = "gagal mengirim packet reliable";
    return false;
  }
  session.lastReliableSent = pending.lastSent;
  session.pendingReliable.push_back(std::move(pending));
  return true;
}

void consumeRemoteAcks(LiveSession& session, const std::vector<uint16_t>& messageNumbers) {
  if (messageNumbers.empty() || session.pendingReliable.empty()) return;
  const auto now = std::chrono::steady_clock::now();
  for (auto it = session.pendingReliable.begin(); it != session.pendingReliable.end();) {
    const bool acknowledged = std::find(messageNumbers.begin(), messageNumbers.end(),
                                        it->messageNumber) != messageNumbers.end();
    if (!acknowledged) {
      ++it;
      continue;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - it->lastSent).count();
    const uint32_t rttMs = static_cast<uint32_t>(std::clamp<int64_t>(
        elapsed, 0, static_cast<int64_t>(std::numeric_limits<uint32_t>::max())));
    session.latestReliableRttMs = rttMs;
    session.smoothedReliableRttMs = session.smoothedReliableRttMs == 0u
        ? rttMs
        : static_cast<uint32_t>((static_cast<uint64_t>(session.smoothedReliableRttMs) * 7u +
                                 rttMs) / 8u);
    if (it->purpose == ReliablePurpose::SpawnConfirmation) {
      // The server has transport-ACKed SyncRPC PlayerSpawn, so SyncPacket USeq
      // movement can no longer race in ahead of that spawned-state transition.
      session.spawnConfirmationAcked = true;
      appendBounded(session.stateSummary, "PlayerSpawn ACK", 240);
    }
    it = session.pendingReliable.erase(it);
  }
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
    session.lastReliableSent = now;
    ++pending.retries;
    ++session.reliableResends;
  }
}

uint64_t monotonicMilliseconds64() {
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

uint32_t monotonicMilliseconds32() {
  return static_cast<uint32_t>(monotonicMilliseconds64());
}

void replyToInternalPing(LiveSession& session, const std::vector<uint8_t>& payload) {
  // RakNet 2.52 replies to ID_INTERNAL_PING (6) with an unreliable
  // ID_CONNECTED_PONG (9), echoing the peer timestamp and adding ours.
  if (!session.net || session.net->fd < 0 || payload.size() != 5) return;
  raknet::BitReader reader(payload.data() + 1, payload.size() - 1);
  const uint32_t remoteTimestamp = reader.readBits(32);
  if (!reader.ok()) return;

  BitWriter pong;
  pong.writeBits(ID_CONNECTED_PONG, 8);
  pong.writeBits(remoteTimestamp, 32);
  pong.writeBits(monotonicMilliseconds32(), 32);
  const uint16_t messageNumber = session.nextMessageNumber;
  const auto packet = raknet::buildUnreliable(messageNumber, pong.buffer);
  if (!session.net->send(wrap_packet(packet, session.port))) {
    session.error = "gagal membalas RakNet internal ping";
    return;
  }
  ++session.nextMessageNumber;
  ++session.connectedPongs;
}

bool sendEmptyStaticData(LiveSession& session) {
  if (!session.net || session.net->fd < 0) return false;
  const uint16_t messageNumber = session.nextMessageNumber++;
  return sendSessionReliable(session,
                             raknet::buildReliable(messageNumber,
                                                   handshake::buildEmptyStaticData()),
                             messageNumber);
}

void sendDueReliableKeepalive(LiveSession& session) {
  if (!session.net || session.net->fd < 0) return;
  const auto now = std::chrono::steady_clock::now();
  const uint64_t idleMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - session.lastReliableSent).count());
  if (!sessionpolicy::shouldSendReliableKeepalive(idleMs, session.pendingReliable.size())) return;

  const uint16_t messageNumber = session.nextMessageNumber++;
  if (sendSessionReliable(session, raknet::buildReliable(messageNumber,
                                                           {ID_DETECT_LOST_CONNECTIONS}),
                          messageNumber)) {
    ++session.reliableKeepalives;
  }
}

bool sendSpawnConfirmation(LiveSession& session) {
  if (session.spawnConfirmationSent) return true;
  if (!session.net || session.net->fd < 0 || !session.world.spawnAllowed()) return false;

  const uint16_t messageNumber = session.nextMessageNumber++;
  const uint16_t orderingIndex = session.orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(
      messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, clientrpc::buildPlayerSpawn());
  if (!sendSessionReliable(session, packet, messageNumber, ReliablePurpose::SpawnConfirmation)) {
    session.error = "gagal mengirim PlayerSpawn";
    return false;
  }

  session.spawnConfirmationSent = true;
  session.spawnConfirmationAcked = false;
  appendBounded(session.stateSummary, "PlayerSpawn dikirim", 240);
  return true;
}

// A stock client asks for its first advertised class as soon as InitGame lands.
// This is not a spawn or a local-world decision: it is the server-owned class
// selection handshake. Waiting for a person to notice a UI button can make a
// busy legacy server close the new peer before it reaches the class screen.
// The response remains authoritative and the Android UI can then browse a
// different class or explicitly request spawn.
bool requestInitialClass(LiveSession& session) {
  if (!session.net || session.net->fd < 0 || !session.accepted ||
      !session.world.initialized() || session.classRequestPending) {
    return false;
  }
  const uint32_t classCount = session.world.playerInit().spawnClassCount;
  if (classCount == 0u) {
    appendBounded(session.stateSummary, "PlayerInit tanpa class", 240);
    return false;
  }

  const auto rpc = clientrpc::buildRequestClass(0u);
  const uint16_t messageNumber = session.nextMessageNumber++;
  const uint16_t orderingIndex = session.orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(
      messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(session, packet, messageNumber)) {
    session.error = "gagal mengirim PlayerRequestClass awal";
    return false;
  }

  session.world.beginClassRequest();
  session.spawnRequestPending = false;
  session.spawnConfirmationSent = false;
  session.spawnConfirmationAcked = false;
  session.classRequestPending = true;
  session.requestedClassId = 0u;
  session.classRequestSent = std::chrono::steady_clock::now();
  appendBounded(session.stateSummary, "PlayerInit → PlayerRequestClass #0", 240);
  return true;
}

void invalidateLocalGameplay(LiveSession& session) {
  gameplay::invalidateLocalPrediction(session.localGameplay);
  session.onFootPreviewReady = false;
  session.inCarPreviewReady = false;
  session.aimPreviewReady = false;
  session.passengerPreviewReady = false;
}

void rebuildLocalOnFootPreview(LiveSession& session) {
  const state::LocalPlayer& local = session.world.localPlayer();
  // Spectator/freeze scripts are authoritative. Keep visual camera code free
  // to run, but never retain a prospective movement packet while the server
  // has explicitly revoked local gameplay control.
  if (local.spectatingKnown && local.spectating) {
    invalidateLocalGameplay(session);
    return;
  }
  if (serverMarkedDead(local)) {
    invalidateLocalGameplay(session);
    return;
  }
  if (local.controllableKnown && !local.controllable) {
    invalidateLocalGameplay(session);
    return;
  }
  if (local.vehicleKnown && local.inVehicle) {
    // On-foot state must never remain live after the server seats the player,
    // but do not discard the separate driver prediction on every touch sample.
    session.onFootPreviewReady = false;
    return;
  }
  if (!session.controls.headingKnown() && local.facingKnown) {
    session.controls.synchronizeHeading(local.angle);
  }
  const bridge::OnFootPreview preview =
      bridge::buildOnFootPreview(local, session.controls.wireInput());
  session.onFootPreviewReady = preview.ready();
  if (!preview.ready()) return;

  session.onFootPreview = preview.state;
  // Keep diagnostic preview position aligned with a collision-validated local
  // prediction between authoritative server corrections. The WorldState itself
  // remains untouched, so an RPC can replace prediction atomically.
  if (session.localGameplay.predictionActive && session.localGameplay.motion.initialized) {
    session.onFootPreview.positionX = session.localGameplay.motion.position.x;
    session.onFootPreview.positionY = session.localGameplay.motion.position.y;
    session.onFootPreview.positionZ = session.localGameplay.motion.position.z;
  }
}

enum class OnFootSendOutcome : uint8_t {
  Sent,
  RateLimited,
  Rejected,
};

OnFootSendOutcome sendOnFootLocked(LiveSession& session, const sync::OnFootState& state) {
  if (session.world.localPlayer().spectatingKnown && session.world.localPlayer().spectating) {
    session.error = "OnFootSync dihentikan saat server mengaktifkan spectator";
    return OnFootSendOutcome::Rejected;
  }
  if (serverMarkedDead(session.world.localPlayer())) {
    session.error = "OnFootSync dihentikan karena health server nol";
    return OnFootSendOutcome::Rejected;
  }
  // A server freeze applies even after the regular spawn/transport gate has
  // opened. It is a distinct authoritative stop, not a local UI preference.
  if (session.world.localPlayer().controllableKnown &&
      !session.world.localPlayer().controllable) {
    session.error = "OnFootSync dihentikan oleh TogglePlayerControllable server";
    return OnFootSendOutcome::Rejected;
  }
  if (session.world.localPlayer().vehicleKnown && session.world.localPlayer().inVehicle) {
    session.error = "OnFootSync dihentikan saat server menempatkan player di kendaraan";
    return OnFootSendOutcome::Rejected;
  }
  // Movement is never emitted merely because a socket exists. The server must
  // have initialized this player and explicitly accepted a spawn request.
  if (!syncpolicy::onFootSessionReady(
          session.accepted, session.world.initialized(), session.world.spawnResponseKnown(),
          session.world.spawnAllowed(), session.spawnConfirmationAcked)) {
    session.error = "OnFootSync menunggu PlayerInit, PlayerSpawn, dan ACK server";
    return OnFootSendOutcome::Rejected;
  }

  const sync::EncodedPayload payload = sync::buildPlayerFootSync(state);
  if (!payload.valid()) {
    session.error = "OnFootSync lokal tidak valid atau di luar batas server";
    return OnFootSendOutcome::Rejected;
  }

  const uint64_t nowMs = monotonicMilliseconds64();
  const uint32_t serverRateMs = session.world.playerInit().onFootSyncRateMs;
  if (!syncpolicy::onFootSendDue(nowMs, session.lastOnFootSentMs, serverRateMs)) {
    // This is expected in an input/render loop; do not turn normal rate
    // limiting into a sticky session error.
    return OnFootSendOutcome::RateLimited;
  }

  const uint16_t messageNumber = session.nextMessageNumber;
  const uint16_t orderingIndex = session.orderingCounters.currentSync();
  const auto packet = raknet::buildUnreliableSequenced(
      messageNumber, sync::kSyncOrderingChannel, orderingIndex, payload.bytes, payload.bitLength);
  if (packet.empty() || !session.net->send(wrap_packet(packet, session.port))) {
    session.error = "gagal mengirim OnFootSync";
    return OnFootSendOutcome::Rejected;
  }

  ++session.nextMessageNumber;
  (void)session.orderingCounters.takeSync();
  session.error.clear();
  session.lastOnFootSentMs = nowMs;
  session.localOnFootState = state;
  session.localOnFootStateKnown = true;
  ++session.outboundOnFootPackets;
  appendBounded(session.stateSummary,
                "OnFootSync #" + std::to_string(session.outboundOnFootPackets), 240);
  return OnFootSendOutcome::Sent;
}

enum class InCarSendOutcome : uint8_t {
  Sent,
  RateLimited,
  Rejected,
};

InCarSendOutcome sendInCarLocked(LiveSession& session, const sync::InCarState& inCar) {
  const state::LocalPlayer& local = session.world.localPlayer();
  if (local.spectatingKnown && local.spectating) {
    session.error = "PlayerVehicleSync dihentikan saat server mengaktifkan spectator";
    return InCarSendOutcome::Rejected;
  }
  if (serverMarkedDead(local)) {
    session.error = "PlayerVehicleSync dihentikan karena health server nol";
    return InCarSendOutcome::Rejected;
  }
  if (local.controllableKnown && !local.controllable) {
    session.error = "PlayerVehicleSync dihentikan oleh TogglePlayerControllable server";
    return InCarSendOutcome::Rejected;
  }
  if (!local.vehicleKnown || !local.inVehicle || local.vehicleSeat != 0 || local.vehicleId == 0 ||
      inCar.vehicleId != local.vehicleId || session.world.findVehicle(inCar.vehicleId) == nullptr) {
    session.error = "PlayerVehicleSync hanya untuk driver kendaraan server yang masih streamed";
    return InCarSendOutcome::Rejected;
  }
  if (!syncpolicy::onFootSessionReady(
          session.accepted, session.world.initialized(), session.world.spawnResponseKnown(),
          session.world.spawnAllowed(), session.spawnConfirmationAcked)) {
    session.error = "PlayerVehicleSync menunggu PlayerInit, PlayerSpawn, dan ACK server";
    return InCarSendOutcome::Rejected;
  }
  const sync::EncodedPayload payload = sync::buildPlayerVehicleSync(inCar);
  if (!payload.valid()) {
    session.error = "PlayerVehicleSync lokal tidak valid atau di luar batas server";
    return InCarSendOutcome::Rejected;
  }
  const uint64_t nowMs = monotonicMilliseconds64();
  const uint32_t serverRateMs = session.world.playerInit().inCarSyncRateMs;
  if (!syncpolicy::inCarSendDue(nowMs, session.lastInCarSentMs, serverRateMs)) {
    return InCarSendOutcome::RateLimited;
  }
  const uint16_t messageNumber = session.nextMessageNumber;
  const uint16_t orderingIndex = session.orderingCounters.currentSync();
  const auto packet = raknet::buildUnreliableSequenced(
      messageNumber, sync::kSyncOrderingChannel, orderingIndex, payload.bytes, payload.bitLength);
  if (packet.empty() || !session.net->send(wrap_packet(packet, session.port))) {
    session.error = "gagal mengirim PlayerVehicleSync";
    return InCarSendOutcome::Rejected;
  }
  ++session.nextMessageNumber;
  (void)session.orderingCounters.takeSync();
  session.error.clear();
  session.lastInCarSentMs = nowMs;
  session.localInCarState = inCar;
  session.localInCarStateKnown = true;
  ++session.outboundInCarPackets;
  appendBounded(session.stateSummary,
                "PlayerVehicleSync #" + std::to_string(session.outboundInCarPackets), 240);
  return InCarSendOutcome::Sent;
}

enum class AimSendOutcome : uint8_t {
  Sent,
  RateLimited,
  Rejected,
};

AimSendOutcome sendAimLocked(LiveSession& session, const sync::AimState& aim) {
  const state::LocalPlayer& local = session.world.localPlayer();
  if (local.spectatingKnown && local.spectating) {
    session.error = "PlayerAimSync dihentikan saat server mengaktifkan spectator";
    return AimSendOutcome::Rejected;
  }
  if (serverMarkedDead(local)) {
    session.error = "PlayerAimSync dihentikan karena health server nol";
    return AimSendOutcome::Rejected;
  }
  if (local.controllableKnown && !local.controllable) {
    session.error = "PlayerAimSync dihentikan oleh TogglePlayerControllable server";
    return AimSendOutcome::Rejected;
  }
  if (local.vehicleKnown && local.inVehicle) {
    session.error = "PlayerAimSync hanya dikirim saat on-foot";
    return AimSendOutcome::Rejected;
  }
  if (!syncpolicy::onFootSessionReady(
          session.accepted, session.world.initialized(), session.world.spawnResponseKnown(),
          session.world.spawnAllowed(), session.spawnConfirmationAcked)) {
    session.error = "PlayerAimSync menunggu PlayerInit, PlayerSpawn, dan ACK server";
    return AimSendOutcome::Rejected;
  }
  // Camera RPC endpoints are server authority. This native port has not
  // implemented a server-directed camera-to-aim bridge yet, so fail closed
  // rather than reporting collision/player heading as a fabricated camera.
  const state::CameraView serverCamera = session.world.camera();
  if (serverCamera.positionKnown || serverCamera.lookAtKnown) {
    session.error = "PlayerAimSync ditahan saat kamera dikendalikan server";
    return AimSendOutcome::Rejected;
  }
  const sync::EncodedPayload payload = sync::buildPlayerAimSync(aim);
  if (!payload.valid()) {
    session.error = "PlayerAimSync lokal tidak valid atau di luar batas server";
    return AimSendOutcome::Rejected;
  }
  const uint64_t nowMs = monotonicMilliseconds64();
  const uint32_t serverRateMs = session.world.playerInit().weaponSyncRateMs;
  if (!syncpolicy::weaponSendDue(nowMs, session.lastAimSentMs, serverRateMs)) {
    return AimSendOutcome::RateLimited;
  }
  const uint16_t messageNumber = session.nextMessageNumber;
  const uint16_t orderingIndex = session.orderingCounters.currentSync();
  const auto packet = raknet::buildUnreliableSequenced(
      messageNumber, sync::kSyncOrderingChannel, orderingIndex, payload.bytes, payload.bitLength);
  if (packet.empty() || !session.net->send(wrap_packet(packet, session.port))) {
    session.error = "gagal mengirim PlayerAimSync";
    return AimSendOutcome::Rejected;
  }
  ++session.nextMessageNumber;
  (void)session.orderingCounters.takeSync();
  session.error.clear();
  session.lastAimSentMs = nowMs;
  session.localAimState = aim;
  session.localAimStateKnown = true;
  ++session.outboundAimPackets;
  appendBounded(session.stateSummary,
                "PlayerAimSync #" + std::to_string(session.outboundAimPackets), 240);
  return AimSendOutcome::Sent;
}

enum class NoTargetBulletSendOutcome : uint8_t {
  Sent,
  RateLimited,
  Rejected,
};

uint64_t noTargetBulletIntervalMs(uint8_t weaponId) {
  // This is a local upper-bound safety cadence rather than a GTA damage/fire
  // simulation. It prevents a held Android control from flooding Packet-206
  // while staying usable for the legacy automatic weapon families.
  switch (weaponId) {
    case 25:  // shotgun
      return 750u;
    case 26:  // sawed-off shotgun
      return 550u;
    case 27:  // combat shotgun
      return 300u;
    case 33:  // country rifle
      return 800u;
    case 34:  // sniper rifle
      return 900u;
    case 38:  // minigun
      return 50u;
    case 28:  // Uzi
    case 29:  // MP5
    case 30:  // AK-47
    case 31:  // M4
    case 32:  // Tec-9
      return 90u;
    default:  // pistol, silenced pistol, Desert Eagle
      return 240u;
  }
}

NoTargetBulletSendOutcome sendNoTargetBulletLocked(LiveSession& session,
                                                   const sync::BulletState& bullet) {
  const state::LocalPlayer& local = session.world.localPlayer();
  if (local.spectatingKnown && local.spectating) {
    session.error = "PlayerBulletSync dihentikan saat server mengaktifkan spectator";
    return NoTargetBulletSendOutcome::Rejected;
  }
  if (serverMarkedDead(local)) {
    session.error = "PlayerBulletSync dihentikan karena health server nol";
    return NoTargetBulletSendOutcome::Rejected;
  }
  if (local.controllableKnown && !local.controllable) {
    session.error = "PlayerBulletSync dihentikan oleh TogglePlayerControllable server";
    return NoTargetBulletSendOutcome::Rejected;
  }
  if (local.vehicleKnown && local.inVehicle) {
    session.error = "PlayerBulletSync no-target hanya dikirim saat on-foot";
    return NoTargetBulletSendOutcome::Rejected;
  }
  if (!syncpolicy::onFootSessionReady(
          session.accepted, session.world.initialized(), session.world.spawnResponseKnown(),
          session.world.spawnAllowed(), session.spawnConfirmationAcked)) {
    session.error = "PlayerBulletSync menunggu PlayerInit, PlayerSpawn, dan ACK server";
    return NoTargetBulletSendOutcome::Rejected;
  }
  // open.mp's PlayerBulletSync handler returns before decoding when its
  // PlayerInit-advertised lag-compensation mode is Disabled (zero). Do not
  // flood a server with a no-target form it will deliberately ignore.
  if (session.world.playerInit().lagCompensation == 0u) {
    session.error = "PlayerBulletSync ditahan: lag compensation server nonaktif";
    return NoTargetBulletSendOutcome::Rejected;
  }
  const state::CameraView serverCamera = session.world.camera();
  if (serverCamera.positionKnown || serverCamera.lookAtKnown) {
    session.error = "PlayerBulletSync ditahan saat kamera dikendalikan server";
    return NoTargetBulletSendOutcome::Rejected;
  }
  if (!local.armedWeaponKnown || bullet.weaponId != local.armedWeapon ||
      local.armedWeapon >= local.weaponAmmoKnown.size() ||
      !local.weaponAmmoKnown[local.armedWeapon] || local.weaponAmmo[local.armedWeapon] == 0u) {
    session.error = "PlayerBulletSync menunggu weapon/ammo authoritative server";
    return NoTargetBulletSendOutcome::Rejected;
  }
  const sync::EncodedPayload payload = sync::buildPlayerNoTargetBulletSync(bullet);
  if (!payload.valid()) {
    session.error = "PlayerBulletSync no-target lokal tidak valid";
    return NoTargetBulletSendOutcome::Rejected;
  }
  const uint64_t nowMs = monotonicMilliseconds64();
  // Honor a slower server WeaponRate as well as the local per-weapon flood
  // ceiling; neither side can turn a held button into an unbounded packet loop.
  const uint64_t intervalMs = std::max<uint64_t>(
      noTargetBulletIntervalMs(bullet.weaponId),
      syncpolicy::boundedWeaponRateMs(session.world.playerInit().weaponSyncRateMs));
  if (session.lastNoTargetBulletSentMs != 0u &&
      (nowMs < session.lastNoTargetBulletSentMs ||
       nowMs - session.lastNoTargetBulletSentMs < intervalMs)) {
    return NoTargetBulletSendOutcome::RateLimited;
  }
  const uint16_t messageNumber = session.nextMessageNumber;
  const uint16_t orderingIndex = session.orderingCounters.currentSync();
  const auto packet = raknet::buildUnreliableSequenced(
      messageNumber, sync::kSyncOrderingChannel, orderingIndex, payload.bytes, payload.bitLength);
  if (packet.empty() || !session.net->send(wrap_packet(packet, session.port))) {
    session.error = "gagal mengirim PlayerBulletSync no-target";
    return NoTargetBulletSendOutcome::Rejected;
  }
  ++session.nextMessageNumber;
  (void)session.orderingCounters.takeSync();
  session.lastNoTargetBulletSentMs = nowMs;
  const bool hasStaticWorldImpact = bullet.hitPositionX != 0.0f ||
                                    bullet.hitPositionY != 0.0f ||
                                    bullet.hitPositionZ != 0.0f;
  session.lastNoTargetStaticWorldImpactMs = hasStaticWorldImpact ? nowMs : 0u;
  ++session.outboundNoTargetBulletPackets;
  session.error.clear();
  appendBounded(session.stateSummary,
                "PlayerBulletSync none #" +
                    std::to_string(session.outboundNoTargetBulletPackets),
                240);
  return NoTargetBulletSendOutcome::Sent;
}

enum class PassengerSendOutcome : uint8_t {
  Sent,
  RateLimited,
  Rejected,
};

PassengerSendOutcome sendPassengerLocked(LiveSession& session,
                                         const sync::PassengerState& passenger) {
  const state::LocalPlayer& local = session.world.localPlayer();
  if (local.spectatingKnown && local.spectating) {
    session.error = "PlayerPassengerSync dihentikan saat server mengaktifkan spectator";
    return PassengerSendOutcome::Rejected;
  }
  if (serverMarkedDead(local)) {
    session.error = "PlayerPassengerSync dihentikan karena health server nol";
    return PassengerSendOutcome::Rejected;
  }
  if (local.controllableKnown && !local.controllable) {
    session.error = "PlayerPassengerSync dihentikan oleh TogglePlayerControllable server";
    return PassengerSendOutcome::Rejected;
  }
  if (!local.vehicleKnown || !local.inVehicle || local.vehicleSeat == 0 ||
      local.vehicleId == 0 || passenger.vehicleId != local.vehicleId ||
      passenger.seatId != local.vehicleSeat ||
      session.world.findVehicle(passenger.vehicleId) == nullptr) {
    session.error = "PlayerPassengerSync hanya untuk kursi passenger server pada kendaraan streamed";
    return PassengerSendOutcome::Rejected;
  }
  if (!syncpolicy::onFootSessionReady(
          session.accepted, session.world.initialized(), session.world.spawnResponseKnown(),
          session.world.spawnAllowed(), session.spawnConfirmationAcked)) {
    session.error = "PlayerPassengerSync menunggu PlayerInit, PlayerSpawn, dan ACK server";
    return PassengerSendOutcome::Rejected;
  }
  const sync::EncodedPayload payload = sync::buildPlayerPassengerSync(passenger);
  if (!payload.valid()) {
    session.error = "PlayerPassengerSync lokal tidak valid atau di luar batas server";
    return PassengerSendOutcome::Rejected;
  }
  const uint64_t nowMs = monotonicMilliseconds64();
  const uint32_t serverRateMs = session.world.playerInit().inCarSyncRateMs;
  if (!syncpolicy::inCarSendDue(nowMs, session.lastPassengerSentMs, serverRateMs)) {
    return PassengerSendOutcome::RateLimited;
  }
  const uint16_t messageNumber = session.nextMessageNumber;
  const uint16_t orderingIndex = session.orderingCounters.currentSync();
  const auto packet = raknet::buildUnreliableSequenced(
      messageNumber, sync::kSyncOrderingChannel, orderingIndex, payload.bytes, payload.bitLength);
  if (packet.empty() || !session.net->send(wrap_packet(packet, session.port))) {
    session.error = "gagal mengirim PlayerPassengerSync";
    return PassengerSendOutcome::Rejected;
  }
  ++session.nextMessageNumber;
  (void)session.orderingCounters.takeSync();
  session.error.clear();
  session.lastPassengerSentMs = nowMs;
  session.localPassengerState = passenger;
  session.localPassengerStateKnown = true;
  ++session.outboundPassengerPackets;
  appendBounded(session.stateSummary,
                "PlayerPassengerSync #" + std::to_string(session.outboundPassengerPackets), 240);
  return PassengerSendOutcome::Sent;
}

bool isReliableTransport(uint8_t reliability) {
  return reliability == raknet::kReliabilityReliable ||
         reliability == raknet::kReliabilityReliableOrdered ||
         reliability == raknet::kReliabilityReliableSequenced;
}

std::vector<raknet::IncomingPacket> applyInboundReliableOrder(
    raknet::ReliableOrderedGate& ordered, raknet::IncomingPacket packet) {
  if (packet.reliability != raknet::kReliabilityReliableOrdered) {
    return {std::move(packet)};
  }
  return ordered.push(std::move(packet));
}

void processSessionDatagram(LiveSession& session, const std::vector<uint8_t>& bytes) {
  // Net has already peer-filtered this UDP datagram. Count any peer traffic as
  // transport liveness even if its application payload is malformed; protocol
  // reducers still reject malformed state separately.
  session.lastInboundAt = std::chrono::steady_clock::now();
  const raknet::Datagram datagram = raknet::decodeDatagram(bytes);
  consumeRemoteAcks(session, datagram.receivedAckMessageNumbers);
  if (!datagram.ackMessageNumbers.empty()) {
    session.net->send(wrap_packet(raknet::buildAck(datagram.ackMessageNumbers), session.port));
  }

  for (const auto& raw : datagram.packets) {
    // ACKs above intentionally include retransmissions. RakNet then suppresses
    // the duplicate reliable message number before split/order/application
    // handling; otherwise a repeated non-ordered control packet could replay.
    if (isReliableTransport(raw.reliability) &&
        !session.inboundReliable.accept(raw.messageNumber)) {
      continue;
    }
    if (raw.split) ++session.splitFragments;
    auto reassembled = session.reassembler.push(raw);
    if (session.reassembler.overflowed()) {
      // ACKs are generated before reassembly so every reliable fragment gets
      // transport acknowledgement. Once the bounded assembly budget is hit,
      // retaining the session could otherwise lose an ordered parent forever.
      session.error = "reassembly split inbound melebihi batas aman";
      session.net->close();
      return;
    }
    for (auto packet : reassembled) {
      auto delivered = applyInboundReliableOrder(session.inboundOrdered, std::move(packet));
      if (session.inboundOrdered.overflowed()) {
        // The datagram was transport-ACKed before application ordering. Do
        // not pretend a dropped ordered RPC can recover after this bound;
        // close cleanly and require a fresh session instead.
        session.error = "antrean reliable-ordered inbound melebihi batas aman";
        session.net->close();
        return;
      }
      for (const auto& packet : delivered) {
        if (packet.payload.empty()) continue;
        ++session.receivedPackets;
        if (packet.reassembled) ++session.reassembledPackets;
        if ((packet.reliability == raknet::kReliabilityUnreliableSequenced ||
             packet.reliability == raknet::kReliabilityReliableSequenced) &&
            !session.inboundSequenced.accept(packet.orderingChannel, packet.orderingIndex)) {
          // A late sync datagram must not roll a remote entity back in time.
          continue;
        }

        const uint8_t id = packet.payload[0];
        if (handshake::isTerminalSessionControlId(id)) {
          session.controls.reset();
          invalidateLocalGameplay(session);
          // No inactive snapshot may retain prior server world/audio/dialog
          // facts after a transport-level refusal/disconnect.
          session.world = {};
          session.joined = false;
          session.error = "server memutus atau menolak sesi native (RakNet id " +
                          std::to_string(static_cast<unsigned>(id)) + ")";
          session.net->close();
          return;
        }
        if (id == ID_INTERNAL_PING) {
          replyToInternalPing(session, packet.payload);
          continue;
        }
        if (id == ID_REQUEST_STATIC_DATA) {
          // RakPeer answers the connected peer's explicit static-data request
          // with its (empty) local-static-data payload on reliable transport.
          // SampDroid intentionally exposes no arbitrary app static data.
          if (!sendEmptyStaticData(session)) {
            session.error = "gagal mengirim static-data RakNet";
          }
          continue;
        }
        if (id == ID_RECEIVED_STATIC_DATA) {
          // The legacy peer may send its own static record after acceptance.
          // It is transport metadata, not GTA/world data and is deliberately not
          // surfaced to Java or allowed to alter gameplay state.
          continue;
        }
        if (id == syncdecode::kPlayerAimSyncPacketId) {
          syncdecode::RemoteAimSync remote;
          if (syncdecode::decodeRemoteAimBroadcast(packet.payload, remote,
                                                   packet.payloadBitLength)) {
            ++session.remoteAimPackets;
            appendBounded(session.syncSummary, syncdecode::summarize(remote), 320);
            state::RpcEvent event;
            if (session.world.applyRemoteAim(remote, event)) {
              appendBounded(session.stateSummary, event.summary, 240);
            }
          }
          continue;
        }
        if (id == syncdecode::kPlayerBulletSyncPacketId) {
          syncdecode::RemoteBulletSync remote;
          if (syncdecode::decodeRemoteBulletBroadcast(packet.payload, remote,
                                                      packet.payloadBitLength)) {
            ++session.remoteBulletPackets;
            appendBounded(session.syncSummary, syncdecode::summarize(remote), 320);
            state::RpcEvent event;
            if (session.world.applyRemoteBullet(remote, event)) {
              appendBounded(session.stateSummary, event.summary, 240);
            }
          }
          continue;
        }
        if (id == syncdecode::kPlayerFootSyncPacketId) {
          syncdecode::RemoteOnFootSync remote;
          if (syncdecode::decodeRemoteOnFootBroadcast(packet.payload, remote,
                                                      packet.payloadBitLength)) {
            ++session.remoteOnFootPackets;
            appendBounded(session.syncSummary, syncdecode::summarize(remote), 320);
            state::RpcEvent event;
            if (session.world.applyRemoteOnFoot(remote, event)) {
              appendBounded(session.stateSummary, event.summary, 240);
            }
          }
          continue;
        }
        if (id == syncdecode::kPlayerPassengerSyncPacketId) {
          syncdecode::RemotePassengerSync remote;
          if (syncdecode::decodeRemotePassengerBroadcast(packet.payload, remote,
                                                          packet.payloadBitLength)) {
            ++session.remoteVehiclePackets;
            appendBounded(session.syncSummary, syncdecode::summarize(remote), 320);
            state::RpcEvent event;
            if (session.world.applyRemotePassenger(remote, event)) {
              appendBounded(session.stateSummary, event.summary, 240);
            }
          }
          continue;
        }
        if (id == syncdecode::kPlayerTrailerSyncPacketId) {
          syncdecode::RemoteTrailerSync remote;
          if (syncdecode::decodeRemoteTrailerBroadcast(packet.payload, remote,
                                                        packet.payloadBitLength)) {
            ++session.remoteVehiclePackets;
            appendBounded(session.syncSummary, syncdecode::summarize(remote), 320);
            state::RpcEvent event;
            if (session.world.applyRemoteTrailer(remote, event)) {
              appendBounded(session.stateSummary, event.summary, 240);
            }
          }
          continue;
        }
        if (id == syncdecode::kPlayerVehicleSyncPacketId ||
            id == syncdecode::kPlayerUnoccupiedSyncPacketId) {
          syncdecode::RemoteVehicleSync remote;
          if (syncdecode::decodeRemoteVehicleBroadcast(packet.payload, remote,
                                                        packet.payloadBitLength)) {
            ++session.remoteVehiclePackets;
            appendBounded(session.syncSummary, syncdecode::summarize(remote), 320);
            state::RpcEvent event;
            if (session.world.applyRemoteVehicle(remote, event)) {
              appendBounded(session.stateSummary, event.summary, 240);
              const state::LocalPlayer& local = session.world.localPlayer();
              if (local.vehicleKnown && local.inVehicle && event.playerId == local.vehicleId) {
                // A received transform for the local assigned vehicle is an
                // authoritative correction over the kinematic prediction.
                invalidateLocalGameplay(session);
              }
            }
          }
          continue;
        }
        // Transport control frames are handled above or require no app-level
        // reducer action. Game RPC state starts at ID_RPC (20).
        if (id != 20 || packet.payload.size() < 2) continue;

        ++session.receivedRpcs;
        appendBounded(session.rpcSummary, rpc::summarize(packet.payload), 320);
        state::RpcEvent event;
        if (session.world.applyRpc(packet.payload, event)) {
          appendBounded(session.stateSummary, event.summary, 240);
          if (event.kind == state::RpcEventKind::PlayerInitialized) {
            // Legacy servers commonly expect RequestClass immediately after
            // InitGame. Queue the first server-advertised class before the UI
            // presentation round-trip, while leaving class changes/spawn under
            // the normal explicit Android controls.
            requestInitialClass(session);
          }
          if (event.kind == state::RpcEventKind::SessionCloseRequested) {
            // PlayerClose/ConnectionRejected are exact server lifecycle
            // instructions. Stop all held semantic input and local prediction
            // before closing, rather than leaving a stale playable-looking
            // renderer/sync gate after a restart or explicit rejection.
            session.controls.reset();
            invalidateLocalGameplay(session);
            session.classRequestPending = false;
            session.spawnRequestPending = false;
            session.spawnConfirmationSent = false;
            session.spawnConfirmationAcked = false;
            session.joined = false;
            // The terminal reason lives in SessionSnapshot.error; clear the
            // server-derived reducer state so an inactive world snapshot cannot
            // retain stale audio, dialogs, menus, entities, or gameplay facts.
            session.world = {};
            session.error = "server mengakhiri sesi native (" + event.summary + ")";
            if (session.net) session.net->close();
            return;
          }
          if (event.kind == state::RpcEventKind::ClassSelection) {
            // Both a class reply and ForcePlayerClassSelection supersede every
            // prior spawn handshake. A RakNet ACK for old PlayerSpawn is not a
            // server grant to keep syncing after the server returns to classes.
            session.classRequestPending = false;
            session.spawnRequestPending = false;
            session.spawnConfirmationSent = false;
            session.spawnConfirmationAcked = false;
            session.controls.reset();
            invalidateLocalGameplay(session);
          }
          if (event.localFacingUpdated) {
            session.controls.synchronizeHeading(session.world.localPlayer().angle);
          }
          if (event.kind == state::RpcEventKind::CameraChanged ||
              event.kind == state::RpcEventKind::WeaponStateChanged) {
            // Do not leave a prior Packet-203 candidate shown as ready across a
            // camera or authoritative weapon/ammo transition. The next gameplay
            // tick rebuilds it only from the newly reduced server facts.
            session.aimPreviewReady = false;
          }
          if (event.localPositionUpdated || event.localVelocityUpdated ||
              event.localInteriorUpdated || event.worldGravityUpdated || event.worldBoundsUpdated ||
              event.localSpectateUpdated ||
              event.kind == state::RpcEventKind::BuildingRemovalChanged) {
            // SetSpawnInfo/SetPlayerPos is authoritative over a locally
            // predicted collision step. A server velocity requires a dedicated
            // physics bridge; an interior transition changes the exact IPL/COL
            // query partition; gravity/RPC-17 can change the movement envelope;
            // RPC-43 may remove the exact floor/wall used by the current step;
            // spectator state revokes local movement authority. Each therefore
            // drops prediction, while health or armour-only events deliberately
            // do not trigger this reset.
            invalidateLocalGameplay(session);
          }
          const state::LocalPlayer& localAfterRpc = session.world.localPlayer();
          if (event.localSpectateUpdated && localAfterRpc.spectatingKnown &&
              localAfterRpc.spectating) {
            // Clear semantic holds immediately at the RPC boundary rather than
            // waiting for the next renderer-owned gameplay tick. The separate
            // send gates remain mandatory in case a caller races this reducer.
            session.controls.reset();
            invalidateLocalGameplay(session);
          }
          if ((event.kind == state::RpcEventKind::VehicleStreamedIn ||
               event.kind == state::RpcEventKind::VehicleStreamedOut ||
               (event.kind == state::RpcEventKind::VehicleChanged &&
                !event.vehicleParametersUpdated)) &&
              localAfterRpc.vehicleKnown && localAfterRpc.inVehicle &&
              event.playerId == localAfterRpc.vehicleId) {
            // Transform/velocity mutators, including implicit-local RPC-91
            // SetVehicleVelocity, and stream lifecycle are authoritative over
            // any local driver prediction for this same raw vehicle ID. RPC-24
            // parameter facts carry no transform; preserving the current pilot
            // lets its next bounded step apply manual-engine throttle gating
            // without a needless position snap.
            invalidateLocalGameplay(session);
          }
          if (event.kind == state::RpcEventKind::SpawnInfoChanged ||
              event.kind == state::RpcEventKind::LocalPlayerChanged) {
            if (serverMarkedDead(localAfterRpc)) {
              // Stop held semantic input at the same serialized reducer boundary
              // as SetPlayerHealth(0), rather than waiting for a renderer tick.
              session.controls.reset();
              invalidateLocalGameplay(session);
            } else if (localAfterRpc.controllableKnown && !localAfterRpc.controllable) {
              session.controls.reset();
              invalidateLocalGameplay(session);
            } else if (event.localVehicleUpdated) {
              // Enter/exit is server authority. Clear potentially held walking
              // input, then let the next Android touch sample seed driver input.
              session.controls.reset();
              invalidateLocalGameplay(session);
              if (localAfterRpc.inVehicle && localAfterRpc.vehicleSeat == 0) {
                const state::Vehicle* vehicle = session.world.findVehicle(localAfterRpc.vehicleId);
                if (vehicle != nullptr) session.controls.synchronizeHeading(vehicle->angle);
              }
            }
            rebuildLocalOnFootPreview(session);
          }
          if (event.kind == state::RpcEventKind::SpawnResponse) {
            // This is the authoritative response to the current request. Clear
            // the UI/retry gate before potentially queuing the separate
            // PlayerSpawn confirmation RPC below.
            session.spawnRequestPending = false;
            if (session.world.spawnAllowed()) {
              // PlayerRequestSpawn is only permission. The legacy protocol
              // requires the client to follow it with empty RPC PlayerSpawn (52).
              sendSpawnConfirmation(session);
            } else {
              session.spawnConfirmationSent = false;
              session.spawnConfirmationAcked = false;
              invalidateLocalGameplay(session);
            }
          }
        }
      }
    }
  }
  // Broadcast RPCs alone do not prove ClientJoin completed.  Keep the session
  // explicitly in its initializing state until the server supplied InitGame.
  session.joined = session.world.initialized();
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
  snapshot.classSelectionForced = session->world.classSelectionForced();
  snapshot.classSelectable = session->world.classSelection().selectable;
  snapshot.classRequestPending = session->classRequestPending;
  snapshot.requestedClassId = session->requestedClassId;
  snapshot.spawnResponseKnown = session->world.spawnResponseKnown();
  snapshot.spawnAllowed = session->world.spawnAllowed();
  snapshot.spawnRequestPending = session->spawnRequestPending;
  snapshot.spawnConfirmationSent = session->spawnConfirmationSent;
  snapshot.spawnConfirmationAcked = session->spawnConfirmationAcked;
  snapshot.localSpawnInfoKnown = session->world.localPlayer().spawnInfoKnown;
  snapshot.localPositionKnown = session->world.localPlayer().positionKnown;
  snapshot.localControllableKnown = session->world.localPlayer().controllableKnown;
  snapshot.localControllable = session->world.localPlayer().controllable;
  snapshot.localInteriorKnown = session->world.localPlayer().interiorKnown;
  snapshot.localVirtualWorldKnown = session->world.localPlayer().virtualWorldKnown;
  snapshot.localVehicleKnown = session->world.localPlayer().vehicleKnown;
  snapshot.localInVehicle = session->world.localPlayer().inVehicle;
  snapshot.localInterior = session->world.localPlayer().interior;
  snapshot.localVirtualWorld = session->world.localPlayer().virtualWorld;
  snapshot.localVehicleId = session->world.localPlayer().vehicleId;
  snapshot.localVehicleSeat = session->world.localPlayer().vehicleSeat;
  snapshot.nativeControlsKnown = session->nativeControlsKnown;
  snapshot.onFootPreviewReady = session->onFootPreviewReady;
  snapshot.inCarPreviewReady = session->inCarPreviewReady;
  snapshot.aimPreviewReady = session->aimPreviewReady;
  snapshot.passengerPreviewReady = session->passengerPreviewReady;
  const bool onFootPrediction = session->localGameplay.predictionActive &&
                                session->localGameplay.motion.initialized;
  const bool vehiclePrediction = session->localGameplay.vehiclePredictionActive &&
                                 session->localGameplay.vehicleMotion.initialized;
  snapshot.localPredictionActive = onFootPrediction || vehiclePrediction;
  snapshot.localGrounded = vehiclePrediction ? session->localGameplay.vehicleMotion.grounded
                                              : (onFootPrediction && session->localGameplay.motion.grounded);
  snapshot.checkpointActive = session->world.checkpoint().active;
  snapshot.raceCheckpointActive = session->world.raceCheckpoint().active;
  snapshot.playerId = snapshot.playerInit ? session->world.playerInit().playerId : 0;
  snapshot.spawnClassCount = snapshot.playerInit ? session->world.playerInit().spawnClassCount : 0;
  snapshot.classModel = snapshot.classSelectionKnown ? session->world.classSelection().model : 0;
  const state::LocalPlayer& local = session->world.localPlayer();
  snapshot.localModel = local.spawnInfoKnown ? local.model : 0;
  snapshot.localX = local.positionKnown ? local.x : 0.0f;
  snapshot.localY = local.positionKnown ? local.y : 0.0f;
  snapshot.localZ = local.positionKnown ? local.z : 0.0f;
  snapshot.localAngle = local.facingKnown ? local.angle : 0.0f;
  if (vehiclePrediction) {
    snapshot.localX = session->localGameplay.vehicleMotion.position.x;
    snapshot.localY = session->localGameplay.vehicleMotion.position.y;
    snapshot.localZ = session->localGameplay.vehicleMotion.position.z;
    snapshot.localAngle = session->localGameplay.vehicleMotion.headingDegrees;
  } else if (onFootPrediction) {
    snapshot.localX = session->localGameplay.motion.position.x;
    snapshot.localY = session->localGameplay.motion.position.y;
    snapshot.localZ = session->localGameplay.motion.position.z;
    snapshot.localAngle = session->localGameplay.motion.headingDegrees;
  }
  const input::WireInput controls = session->controls.wireInput();
  snapshot.controlMoveX = session->controls.intent().moveX;
  snapshot.controlMoveY = session->controls.intent().moveY;
  snapshot.controlHeading = controls.headingKnown ? controls.headingDegrees : 0.0f;
  snapshot.controlAimPitch = controls.aimPitchDegrees;
  snapshot.controlLeftRight = controls.leftRight;
  snapshot.controlUpDown = controls.upDown;
  snapshot.controlKeys = controls.keys;
  snapshot.onFootSyncRateMs = snapshot.playerInit ? session->world.playerInit().onFootSyncRateMs : 0;
  snapshot.effectiveOnFootSyncRateMs =
      snapshot.playerInit ? syncpolicy::boundedOnFootRateMs(snapshot.onFootSyncRateMs) : 0;
  snapshot.inCarSyncRateMs = snapshot.playerInit ? session->world.playerInit().inCarSyncRateMs : 0;
  snapshot.effectiveInCarSyncRateMs =
      snapshot.playerInit ? syncpolicy::boundedInCarRateMs(snapshot.inCarSyncRateMs) : 0;
  snapshot.weaponSyncRateMs = snapshot.playerInit ? session->world.playerInit().weaponSyncRateMs : 0;
  snapshot.effectiveWeaponSyncRateMs =
      snapshot.playerInit ? syncpolicy::boundedWeaponRateMs(snapshot.weaponSyncRateMs) : 0;
  snapshot.receivedPackets = session->receivedPackets;
  snapshot.receivedRpcs = session->receivedRpcs;
  const auto inboundIdle = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - session->lastInboundAt).count();
  snapshot.inboundIdleMs = static_cast<uint32_t>(std::clamp<int64_t>(
      inboundIdle, 0, static_cast<int64_t>(std::numeric_limits<uint32_t>::max())));
  snapshot.latestReliableRttMs = session->latestReliableRttMs;
  snapshot.smoothedReliableRttMs = session->smoothedReliableRttMs;
  snapshot.reliableResends = session->reliableResends;
  snapshot.splitFragments = session->splitFragments;
  snapshot.reassembledPackets = session->reassembledPackets;
  snapshot.trackedPlayers = static_cast<int>(session->world.playerCount());
  snapshot.streamedPlayers = static_cast<int>(session->world.streamedPlayerCount());
  snapshot.remoteOnFootPlayers = static_cast<int>(session->world.remoteOnFootCount());
  snapshot.trackedVehicles = static_cast<int>(session->world.vehicleCount());
  snapshot.streamedVehicles = static_cast<int>(session->world.streamedVehicleCount());
  snapshot.trackedServerActors = static_cast<int>(session->world.serverActorCount());
  snapshot.remoteOnFootPackets = session->remoteOnFootPackets;
  snapshot.remoteAimPackets = session->remoteAimPackets;
  snapshot.remoteBulletPackets = session->remoteBulletPackets;
  snapshot.remoteVehiclePackets = session->remoteVehiclePackets;
  snapshot.chatMessages = static_cast<int>(session->world.chatCount());
  snapshot.trackedPickups = static_cast<int>(session->world.pickupCount());
  snapshot.trackedTextLabels = static_cast<int>(session->world.textLabelCount());
  snapshot.trackedMapIcons = static_cast<int>(session->world.mapIconCount());
  snapshot.trackedGangZones = static_cast<int>(session->world.gangZoneCount());
  snapshot.outboundPickupNotifications = session->outboundPickupNotifications;
  snapshot.outboundVehicleInteractions = session->outboundVehicleInteractions;
  snapshot.outboundTextDrawSelections = session->outboundTextDrawSelections;
  snapshot.outboundObjectSelections = session->outboundObjectSelections;
  snapshot.outboundObjectEdits = session->outboundObjectEdits;
  snapshot.outboundAttachedObjectEdits = session->outboundAttachedObjectEdits;
  snapshot.outboundScoreRequests = session->outboundScoreRequests;
  snapshot.reliableKeepalives = session->reliableKeepalives;
  snapshot.connectedPongs = session->connectedPongs;
  snapshot.outboundOnFootPackets = session->outboundOnFootPackets;
  snapshot.outboundInCarPackets = session->outboundInCarPackets;
  snapshot.outboundAimPackets = session->outboundAimPackets;
  snapshot.outboundNoTargetBulletPackets = session->outboundNoTargetBulletPackets;
  snapshot.outboundPassengerPackets = session->outboundPassengerPackets;
  snapshot.outboundChatMessages = session->outboundChatMessages;
  snapshot.outboundDialogResponses = session->outboundDialogResponses;
  snapshot.localOnFootStateKnown = session->localOnFootStateKnown;
  snapshot.localAimStateKnown = session->localAimStateKnown;
  snapshot.localPassengerStateKnown = session->localPassengerStateKnown;
  snapshot.serverName = snapshot.playerInit ? session->world.playerInit().serverName : "";
  snapshot.rpcSummary = session->rpcSummary;
  snapshot.syncSummary = session->syncSummary;
  snapshot.stateSummary = session->stateSummary;
  snapshot.error = session->error;
  return snapshot;
}

WorldAnimationSnapshot worldAnimationSnapshotFrom(const state::AnimationDirective& directive) {
  WorldAnimationSnapshot snapshot;
  snapshot.active = directive.active;
  snapshot.revision = directive.revision;
  snapshot.library = directive.library;
  snapshot.name = directive.name;
  snapshot.delta = directive.delta;
  snapshot.loop = directive.loop;
  snapshot.lockX = directive.lockX;
  snapshot.lockY = directive.lockY;
  snapshot.freeze = directive.freeze;
  snapshot.durationMs = directive.durationMs;
  return snapshot;
}

WorldSnapshot worldSnapshotFrom(const LiveSession* session) {
  WorldSnapshot snapshot;
  if (!session) {
    snapshot.error = "tidak ada sesi native aktif";
    return snapshot;
  }

  snapshot.active = session->net && session->net->fd >= 0;
  snapshot.playerInit = session->world.initialized();
  snapshot.classSelectionKnown = session->world.classSelection().valid;
  snapshot.classRequestPending = session->classRequestPending;
  snapshot.spawnResponseKnown = session->world.spawnResponseKnown();
  snapshot.spawnAllowed = session->world.spawnAllowed();
  snapshot.spawnRequestPending = session->spawnRequestPending;
  snapshot.spawnConfirmationSent = session->spawnConfirmationSent;
  snapshot.spawnConfirmationAcked = session->spawnConfirmationAcked;
  snapshot.streamedPlayers = static_cast<int>(session->world.streamedPlayerCount());
  snapshot.remoteOnFootPlayers = static_cast<int>(session->world.remoteOnFootCount());
  snapshot.streamedVehicles = static_cast<int>(session->world.streamedVehicleCount());
  snapshot.trackedServerActors = static_cast<int>(session->world.serverActorCount());
  snapshot.serverObjects = static_cast<int>(session->world.serverObjectCount());
  snapshot.trackedPickups = static_cast<int>(session->world.pickupCount());
  snapshot.trackedTextDraws = static_cast<int>(session->world.textDrawCount());
  snapshot.trackedTextLabels = static_cast<int>(session->world.textLabelCount());
  snapshot.trackedMapIcons = static_cast<int>(session->world.mapIconCount());
  snapshot.trackedGangZones = static_cast<int>(session->world.gangZoneCount());
  const state::CameraView camera = session->world.camera();
  snapshot.camera.positionKnown = camera.positionKnown;
  snapshot.camera.lookAtKnown = camera.lookAtKnown;
  snapshot.camera.interpolatingPosition = camera.interpolatingPosition;
  snapshot.camera.interpolatingLookAt = camera.interpolatingLookAt;
  snapshot.camera.positionX = camera.positionX;
  snapshot.camera.positionY = camera.positionY;
  snapshot.camera.positionZ = camera.positionZ;
  snapshot.camera.lookAtX = camera.lookAtX;
  snapshot.camera.lookAtY = camera.lookAtY;
  snapshot.camera.lookAtZ = camera.lookAtZ;
  snapshot.textDrawSelectionActive = session->world.textDrawSelectionActive();
  snapshot.objectSelectionActive = session->world.objectSelectionActive();
  snapshot.objectSelectionRevision = session->world.objectSelectionRevision();
  const state::ObjectEditState& objectEdit = session->world.objectEdit();
  snapshot.objectEditActive = objectEdit.active;
  snapshot.objectEditPlayerObject = objectEdit.playerObject;
  snapshot.objectEditId = objectEdit.objectId;
  snapshot.objectEditRevision = objectEdit.revision;
  state::ServerObjectView editedObject;
  if (objectEdit.active && session->world.editedObjectView(
          objectEdit.objectId, objectEdit.playerObject, editedObject)) {
    snapshot.objectEditTransformAvailable = true;
    snapshot.objectEditX = editedObject.x;
    snapshot.objectEditY = editedObject.y;
    snapshot.objectEditZ = editedObject.z;
    snapshot.objectEditRotationX = editedObject.rotationX;
    snapshot.objectEditRotationY = editedObject.rotationY;
    snapshot.objectEditRotationZ = editedObject.rotationZ;
  }
  const state::AttachedObjectEditState& attachedEdit = session->world.attachedObjectEdit();
  snapshot.attachedObjectEdit.active = attachedEdit.active;
  snapshot.attachedObjectEdit.slot = attachedEdit.slot;
  snapshot.attachedObjectEdit.revision = attachedEdit.revision;
  state::PlayerAttachedObject attachment;
  if (attachedEdit.active && session->world.editedAttachedObject(attachedEdit.slot, attachment)) {
    snapshot.attachedObjectEdit.model = attachment.model;
    snapshot.attachedObjectEdit.bone = attachment.bone;
    snapshot.attachedObjectEdit.offsetX = attachment.offsetX;
    snapshot.attachedObjectEdit.offsetY = attachment.offsetY;
    snapshot.attachedObjectEdit.offsetZ = attachment.offsetZ;
    snapshot.attachedObjectEdit.rotationX = attachment.rotationX;
    snapshot.attachedObjectEdit.rotationY = attachment.rotationY;
    snapshot.attachedObjectEdit.rotationZ = attachment.rotationZ;
    snapshot.attachedObjectEdit.scaleX = attachment.scaleX;
    snapshot.attachedObjectEdit.scaleY = attachment.scaleY;
    snapshot.attachedObjectEdit.scaleZ = attachment.scaleZ;
  }
  snapshot.textDrawSelectionColourRgba = session->world.textDrawSelectionColourRgba();
  const state::GameText gameText = session->world.gameText();
  snapshot.gameText.active = gameText.active;
  snapshot.gameText.revision = gameText.revision;
  snapshot.gameText.style = gameText.style;
  snapshot.gameText.durationMs = gameText.durationMs;
  snapshot.gameText.text = gameText.text;
  const state::Dialog& dialog = session->world.dialog();
  snapshot.dialog.active = dialog.active;
  snapshot.dialog.revision = dialog.revision;
  snapshot.dialog.id = dialog.id;
  snapshot.dialog.style = dialog.style;
  snapshot.dialog.title = dialog.title;
  snapshot.dialog.button1 = dialog.button1;
  snapshot.dialog.button2 = dialog.button2;
  snapshot.dialog.body = dialog.body;
  const state::Menu& menu = session->world.menu();
  snapshot.menu.active = menu.active;
  snapshot.menu.revision = menu.revision;
  snapshot.menu.id = menu.id;
  snapshot.menu.twoColumns = menu.twoColumns;
  snapshot.menu.enabled = menu.enabled;
  snapshot.menu.rowEnabled = menu.rowEnabled;
  snapshot.menu.title = menu.title;
  snapshot.menu.column1Header = menu.column1Header;
  snapshot.menu.column2Header = menu.column2Header;
  snapshot.menu.column1Items = menu.column1Items;
  snapshot.menu.column2Items = menu.column2Items;
  snapshot.menu.column1Count = menu.column1Count;
  snapshot.menu.column2Count = menu.column2Count;
  snapshot.serverName = session->world.initialized() ? session->world.playerInit().serverName : "";
  snapshot.error = session->error;

  const state::LocalPlayer& local = session->world.localPlayer();
  snapshot.localSpawnInfoKnown = local.spawnInfoKnown;
  snapshot.localPositionKnown = local.positionKnown;
  snapshot.localFacingKnown = local.facingKnown;
  snapshot.localHealthKnown = local.healthKnown;
  snapshot.localArmourKnown = local.armourKnown;
  snapshot.localMoneyKnown = local.moneyKnown;
  snapshot.localWantedLevelKnown = local.wantedLevelKnown;
  snapshot.localScoreKnown = local.scorePing.known;
  snapshot.localControllableKnown = local.controllableKnown;
  snapshot.localControllable = local.controllable;
  snapshot.localInteriorKnown = local.interiorKnown;
  snapshot.localVirtualWorldKnown = local.virtualWorldKnown;
  snapshot.localVehicleKnown = local.vehicleKnown;
  snapshot.localInVehicle = local.inVehicle;
  snapshot.localPassengerDriveByActive = local.inVehicle && local.vehicleSeat != 0u &&
                                          session->passengerPreviewReady &&
                                          session->passengerPreview.driveBy;
  snapshot.localInterior = local.interior;
  snapshot.localVirtualWorld = local.virtualWorld;
  snapshot.localVehicleId = local.vehicleId;
  snapshot.localVehicleSeat = local.vehicleSeat;
  snapshot.spectate.known = local.spectatingKnown;
  snapshot.spectate.active = local.spectating;
  snapshot.spectate.targetKnown = local.spectateTargetKnown;
  snapshot.spectate.targetVehicle = local.spectateTargetVehicle;
  snapshot.spectate.targetId = local.spectateTargetId;
  snapshot.spectate.mode = local.spectateMode;
  const input::WireInput localControls = session->controls.wireInput();
  const uint16_t heldActions = session->controls.intent().heldActions;
  snapshot.localAimHeld = (heldActions & input::kHeldAim) != 0u;
  snapshot.localFireHeld = (heldActions & input::kHeldFire) != 0u;
  snapshot.localAimSyncReady = session->aimPreviewReady;
  snapshot.localBulletSyncServerEnabled =
      session->world.initialized() && session->world.playerInit().lagCompensation != 0u;
  const uint64_t snapshotNowMs = monotonicMilliseconds64();
  snapshot.localStaticWorldContactActive =
      session->lastNoTargetStaticWorldImpactMs != 0u &&
      snapshotNowMs >= session->lastNoTargetStaticWorldImpactMs &&
      snapshotNowMs - session->lastNoTargetStaticWorldImpactMs <= 180u;
  snapshot.outboundNoTargetBulletPackets = static_cast<uint32_t>(
      std::max(0, session->outboundNoTargetBulletPackets));
  snapshot.localAimPitchDegrees = localControls.aimPitchDegrees;
  const bool onFootPrediction = session->localGameplay.predictionActive &&
                                session->localGameplay.motion.initialized;
  const bool vehiclePrediction = session->localGameplay.vehiclePredictionActive &&
                                 session->localGameplay.vehicleMotion.initialized;
  snapshot.localPredictionActive = onFootPrediction || vehiclePrediction;
  snapshot.localGrounded = vehiclePrediction ? session->localGameplay.vehicleMotion.grounded
                                              : (onFootPrediction && session->localGameplay.motion.grounded);
  const state::WorldEnvironment& environment = session->world.environment();
  snapshot.worldTimeKnown = environment.timeKnown;
  snapshot.playerTimeKnown = environment.playerTimeKnown;
  snapshot.playerClockKnown = environment.playerClockKnown;
  snapshot.playerClockEnabled = environment.playerClockEnabled;
  const state::AudioStream& audioStream = session->world.audioStream();
  snapshot.audioStreamActive = audioStream.active;
  snapshot.audioStreamPositional = audioStream.positional;
  snapshot.audioStreamRevision = audioStream.revision;
  snapshot.audioStreamUrl = audioStream.url;
  snapshot.audioStreamX = audioStream.x;
  snapshot.audioStreamY = audioStream.y;
  snapshot.audioStreamZ = audioStream.z;
  snapshot.audioStreamDistance = audioStream.distance;
  snapshot.weatherKnown = environment.weatherKnown;
  snapshot.gravityKnown = environment.gravityKnown;
  snapshot.worldBoundsKnown = environment.worldBoundsKnown;
  snapshot.worldTime = environment.worldTime;
  snapshot.playerHour = environment.playerHour;
  snapshot.playerMinute = environment.playerMinute;
  snapshot.weather = environment.weather;
  snapshot.gravity = environment.gravity;
  snapshot.worldMaxX = environment.worldMaxX;
  snapshot.worldMinX = environment.worldMinX;
  snapshot.worldMaxY = environment.worldMaxY;
  snapshot.worldMinY = environment.worldMinY;
  if (session->world.initialized()) {
    const rpc::PlayerInitData& init = session->world.playerInit();
    snapshot.showNameTags = init.showNameTags;
    snapshot.disableNameTagLos = init.disableNameTagLos;
    snapshot.nameTagDrawDistance = init.nameTagDrawDistance;
  }
  snapshot.localPlayerId = session->world.initialized() ? session->world.playerInit().playerId : 0;
  snapshot.localModel = local.spawnInfoKnown ? local.model : 0;
  if (local.positionKnown) {
    snapshot.localX = local.x;
    snapshot.localY = local.y;
    snapshot.localZ = local.z;
  }
  if (local.facingKnown) snapshot.localAngle = local.angle;
  if (vehiclePrediction) {
    snapshot.localX = session->localGameplay.vehicleMotion.position.x;
    snapshot.localY = session->localGameplay.vehicleMotion.position.y;
    snapshot.localZ = session->localGameplay.vehicleMotion.position.z;
    snapshot.localAngle = session->localGameplay.vehicleMotion.headingDegrees;
  } else if (onFootPrediction) {
    snapshot.localX = session->localGameplay.motion.position.x;
    snapshot.localY = session->localGameplay.motion.position.y;
    snapshot.localZ = session->localGameplay.motion.position.z;
    snapshot.localAngle = session->localGameplay.motion.headingDegrees;
  } else if (local.vehicleKnown && local.inVehicle && local.vehicleId != 0) {
    // Passenger (and a newly seated driver before its bounded pilot initializes)
    // must follow only the streamed authoritative vehicle transform, never a
    // guessed GTA seat offset. This is a camera/render anchor, not local state.
    const state::Vehicle* vehicle = session->world.localVehiclePresentationAnchor();
    if (vehicle != nullptr) {
      snapshot.localVehicleAnchorKnown = true;
      snapshot.localPositionKnown = true;
      snapshot.localX = vehicle->x;
      snapshot.localY = vehicle->y;
      snapshot.localZ = vehicle->z;
      snapshot.localAngle = vehicle->angle;
    }
  }
  if (local.healthKnown) snapshot.localHealth = local.health;
  if (local.armourKnown) snapshot.localArmour = local.armour;
  if (local.moneyKnown) snapshot.localMoney = local.money;
  if (local.wantedLevelKnown) snapshot.localWantedLevel = local.wantedLevel;
  if (local.scorePing.known) {
    snapshot.localScore = local.scorePing.score;
    snapshot.localPing = local.scorePing.ping;
  }
  snapshot.localArmedWeaponKnown = local.armedWeaponKnown;
  if (local.armedWeaponKnown) {
    snapshot.localArmedWeapon = local.armedWeapon;
    snapshot.localArmedAmmoKnown = local.weaponAmmoKnown[local.armedWeapon];
    if (snapshot.localArmedAmmoKnown) {
      snapshot.localArmedAmmo = local.weaponAmmo[local.armedWeapon];
    }
  }
  snapshot.localForcedAnimation = worldAnimationSnapshotFrom(local.forcedAnimation);
  if (session->world.hasActiveLocalChatBubble()) {
    snapshot.localChatBubbleActive = true;
    snapshot.localChatBubbleRgba = local.chatBubble.rgba;
    snapshot.localChatBubbleDrawDistance = local.chatBubble.drawDistance;
    snapshot.localChatBubbleText = local.chatBubble.text;
  }

  const state::Checkpoint& checkpoint = session->world.checkpoint();
  snapshot.checkpointActive = checkpoint.active;
  if (checkpoint.active) {
    snapshot.checkpointX = checkpoint.x;
    snapshot.checkpointY = checkpoint.y;
    snapshot.checkpointZ = checkpoint.z;
    snapshot.checkpointSize = checkpoint.size;
  }
  const state::RaceCheckpoint& race = session->world.raceCheckpoint();
  snapshot.raceCheckpointActive = race.active;
  if (race.active) {
    snapshot.raceCheckpointX = race.x;
    snapshot.raceCheckpointY = race.y;
    snapshot.raceCheckpointZ = race.z;
    snapshot.raceCheckpointSize = race.size;
  }

  for (const state::RemotePlayerView& player : session->world.renderablePlayers()) {
    WorldActorSnapshot actor;
    actor.id = player.id;
    actor.rgba = player.rgba;
    actor.model = player.model;
    actor.onFootKnown = player.onFootKnown;
    actor.aimKnown = player.aimKnown;
    actor.aimCameraMode = player.aimCameraMode;
    actor.aimWeaponState = player.aimWeaponState;
    actor.aimCameraFrontX = player.aimCameraFrontX;
    actor.aimCameraFrontY = player.aimCameraFrontY;
    actor.aimCameraFrontZ = player.aimCameraFrontZ;
    actor.aimCameraPositionX = player.aimCameraPositionX;
    actor.aimCameraPositionY = player.aimCameraPositionY;
    actor.aimCameraPositionZ = player.aimCameraPositionZ;
    actor.bulletKnown = player.bulletKnown;
    actor.bulletWeaponId = player.bulletWeaponId;
    actor.bulletOriginX = player.bulletOriginX;
    actor.bulletOriginY = player.bulletOriginY;
    actor.bulletOriginZ = player.bulletOriginZ;
    actor.bulletDirectionX = player.bulletDirectionX;
    actor.bulletDirectionY = player.bulletDirectionY;
    actor.bulletDirectionZ = player.bulletDirectionZ;
    actor.wasted = player.wasted;
    actor.forcedAnimation = worldAnimationSnapshotFrom(player.forcedAnimation);
    actor.inVehicle = player.inVehicle;
    actor.vehicleDriver = player.vehicleDriver;
    actor.vehicleId = player.vehicleId;
    actor.vehicleSeat = player.vehicleSeat;
    actor.nameTagVisible = player.nameTagVisible;
    actor.chatBubbleActive = player.chatBubbleActive;
    actor.chatBubbleRgba = player.chatBubbleRgba;
    actor.chatBubbleDrawDistance = player.chatBubbleDrawDistance;
    actor.chatBubbleText = player.chatBubbleText;
    actor.x = player.x;
    actor.y = player.y;
    actor.z = player.z;
    actor.angle = player.angle;
    actor.health = player.health;
    actor.armour = player.armour;
    actor.name = player.name;
    snapshot.actors.push_back(std::move(actor));
  }
  for (const state::ServerActorView& actor : session->world.renderableServerActors()) {
    WorldServerActorSnapshot view;
    view.id = actor.id;
    view.model = actor.model;
    view.x = actor.x;
    view.y = actor.y;
    view.z = actor.z;
    view.angle = actor.angle;
    view.health = actor.health;
    view.invulnerable = actor.invulnerable;
    view.forcedAnimation = worldAnimationSnapshotFrom(actor.forcedAnimation);
    snapshot.serverActors.push_back(std::move(view));
  }
  for (const state::ScoreboardPlayerView& player : session->world.scoreboardPlayers()) {
    WorldScoreSnapshot entry;
    entry.id = player.id;
    entry.rgba = player.rgba;
    entry.npc = player.npc;
    entry.score = player.score;
    entry.ping = player.ping;
    entry.name = player.name;
    snapshot.scoreboard.push_back(std::move(entry));
  }
  for (const state::RemoteVehicleView& vehicle : session->world.renderableVehicles()) {
    WorldVehicleSnapshot view;
    view.id = vehicle.id;
    view.dynamicSyncKnown = vehicle.dynamicSyncKnown;
    view.driverKnown = vehicle.driverKnown;
    view.driverId = vehicle.driverId;
    view.model = vehicle.model;
    view.colour1 = vehicle.colour1;
    view.colour2 = vehicle.colour2;
    view.paintjob = vehicle.paintjob;
    view.x = vehicle.x;
    view.y = vehicle.y;
    view.z = vehicle.z;
    view.angle = vehicle.angle;
    view.attitudeKnown = vehicle.attitudeKnown;
    view.rotationX = vehicle.rotationX;
    view.rotationY = vehicle.rotationY;
    view.rotationZ = vehicle.rotationZ;
    view.parametersKnown = vehicle.parameters.known;
    view.parameters = vehicle.parameters.values;
    view.health = vehicle.health;
    snapshot.vehicles.push_back(std::move(view));
  }
  // Even before COL/prediction is ready, retain the current local seated
  // vehicle inside the bounded presentation budget. This guarantees server
  // engine/lights/siren facts are visible after an RPC-24 correction instead
  // of disappearing merely because lower raw IDs filled the generic list.
  const bool localVehiclePresentationRequired =
      local.vehicleKnown && local.inVehicle && local.vehicleId != 0u;
  if (localVehiclePresentationRequired) {
    const auto existing = std::find_if(snapshot.vehicles.begin(), snapshot.vehicles.end(),
                                       [&local](const WorldVehicleSnapshot& view) {
                                         return view.id == local.vehicleId;
                                       });
    if (existing != snapshot.vehicles.end()) {
      if (vehiclePrediction) {
        existing->x = session->localGameplay.vehicleMotion.position.x;
        existing->y = session->localGameplay.vehicleMotion.position.y;
        existing->z = session->localGameplay.vehicleMotion.position.z;
        existing->angle = session->localGameplay.vehicleMotion.headingDegrees;
        // The current bounded local driver pilot has yaw only. Do not retain a
        // stale remote Packet-200 roll/pitch while its predicted transform wins.
        existing->attitudeKnown = false;
        existing->rotationX = 0.0f;
        existing->rotationY = 0.0f;
        existing->rotationZ = existing->angle;
      }
    } else if (const state::Vehicle* vehicle = session->world.findVehicle(local.vehicleId)) {
      WorldVehicleSnapshot localView;
      localView.id = vehicle->id;
      localView.dynamicSyncKnown = vehicle->dynamicSyncKnown;
      localView.driverKnown = vehicle->driverKnown;
      localView.driverId = vehicle->driverId;
      localView.model = vehicle->model;
      localView.colour1 = vehicle->colour1;
      localView.colour2 = vehicle->colour2;
      localView.paintjob = vehicle->paintjob;
      localView.x = vehicle->x;
      localView.y = vehicle->y;
      localView.z = vehicle->z;
      localView.angle = vehicle->angle;
      localView.attitudeKnown = vehicle->attitudeKnown;
      localView.rotationX = vehicle->rotationX;
      localView.rotationY = vehicle->rotationY;
      localView.rotationZ = vehicle->rotationZ;
      if (vehiclePrediction) {
        localView.x = session->localGameplay.vehicleMotion.position.x;
        localView.y = session->localGameplay.vehicleMotion.position.y;
        localView.z = session->localGameplay.vehicleMotion.position.z;
        localView.angle = session->localGameplay.vehicleMotion.headingDegrees;
        localView.attitudeKnown = false;
        localView.rotationX = 0.0f;
        localView.rotationY = 0.0f;
        localView.rotationZ = localView.angle;
      }
      localView.parametersKnown = vehicle->parameters.known;
      localView.parameters = vehicle->parameters.values;
      localView.health = vehicle->health;
      if (snapshot.vehicles.size() < 64u) {
        snapshot.vehicles.push_back(std::move(localView));
      } else if (!snapshot.vehicles.empty()) {
        // Keep the player-owned occupied vehicle inside the existing bounded
        // renderer budget even when lower IDs filled the generic export first.
        snapshot.vehicles.back() = std::move(localView);
      }
    }
  }
  // WorldState remains authoritative. This copied prediction anchor exists
  // only for renderer-side object/player-attached-object resolution, so a held local
  // item follows the same collision-grounded transform presented for the local
  // actor/driver without feeding that prediction back into RPC state.
  state::LocalObjectAttachmentAnchor localAttachmentAnchor;
  if (vehiclePrediction) {
    localAttachmentAnchor.valid = true;
    localAttachmentAnchor.x = session->localGameplay.vehicleMotion.position.x;
    localAttachmentAnchor.y = session->localGameplay.vehicleMotion.position.y;
    localAttachmentAnchor.z = session->localGameplay.vehicleMotion.position.z;
    localAttachmentAnchor.angle = session->localGameplay.vehicleMotion.headingDegrees;
  } else if (onFootPrediction) {
    localAttachmentAnchor.valid = true;
    localAttachmentAnchor.x = session->localGameplay.motion.position.x;
    localAttachmentAnchor.y = session->localGameplay.motion.position.y;
    localAttachmentAnchor.z = session->localGameplay.motion.position.z;
    localAttachmentAnchor.angle = session->localGameplay.motion.headingDegrees;
  }
  for (const state::ServerObjectView& object : session->world.renderableObjects(
           128u, localAttachmentAnchor.valid ? &localAttachmentAnchor : nullptr)) {
    WorldObjectSnapshot view;
    view.id = object.id;
    view.model = object.model;
    view.moving = object.moving;
    view.x = object.x;
    view.y = object.y;
    view.z = object.z;
    view.rotationX = object.rotationX;
    view.rotationY = object.rotationY;
    view.rotationZ = object.rotationZ;
    view.drawDistance = object.drawDistance;
    for (size_t materialIndex = 0; materialIndex < object.materials.size(); ++materialIndex) {
      const state::ServerObjectMaterialOverride& material = object.materials[materialIndex];
      if (!material.active && !material.textActive) continue;
      WorldObjectMaterialSnapshot& materialView = view.materials[materialIndex];
      materialView.active = material.active;
      materialView.sourceModel = material.sourceModel;
      materialView.colourAbgr = material.colourAbgr;
      materialView.textureDictionary = material.textureDictionary;
      materialView.textureName = material.textureName;
      materialView.textActive = material.textActive;
      materialView.textMaterialSize = material.textMaterialSize;
      materialView.textFontSize = material.textFontSize;
      materialView.textBold = material.textBold;
      materialView.textColourArgb = material.textColourArgb;
      materialView.textBackgroundColourArgb = material.textBackgroundColourArgb;
      materialView.textAlignment = material.textAlignment;
      materialView.textFont = material.textFont;
      materialView.text = material.text;
    }
    snapshot.objects.push_back(std::move(view));
  }
  for (const state::PlayerAttachedObjectView& attachment :
       session->world.renderablePlayerAttachedObjects(
           64u, localAttachmentAnchor.valid ? &localAttachmentAnchor : nullptr)) {
    WorldPlayerAttachedObjectSnapshot view;
    view.playerId = attachment.playerId;
    view.slot = attachment.slot;
    view.model = attachment.model;
    view.bone = attachment.bone;
    view.x = attachment.x;
    view.y = attachment.y;
    view.z = attachment.z;
    view.rotationX = attachment.rotationX;
    view.rotationY = attachment.rotationY;
    view.rotationZ = attachment.rotationZ;
    view.scaleX = attachment.scaleX;
    view.scaleY = attachment.scaleY;
    view.scaleZ = attachment.scaleZ;
    view.colour1Abgr = attachment.colour1Abgr;
    view.colour2Abgr = attachment.colour2Abgr;
    snapshot.playerAttachedObjects.push_back(std::move(view));
  }
  for (const state::PickupView& pickup : session->world.renderablePickups()) {
    WorldPickupSnapshot view;
    view.id = pickup.id;
    view.model = pickup.model;
    view.spawnType = pickup.spawnType;
    view.x = pickup.x;
    view.y = pickup.y;
    view.z = pickup.z;
    snapshot.pickups.push_back(std::move(view));
  }
  for (const state::GangZoneView& zone : session->world.renderableGangZones()) {
    WorldGangZoneSnapshot view;
    view.id = zone.id;
    view.minX = zone.minX;
    view.minY = zone.minY;
    view.maxX = zone.maxX;
    view.maxY = zone.maxY;
    view.colourAbgr = zone.colourAbgr;
    view.flashing = zone.flashing;
    view.flashColourAbgr = zone.flashColourAbgr;
    snapshot.gangZones.push_back(std::move(view));
  }
  for (const state::TextLabelView& label : session->world.renderableTextLabels()) {
    WorldTextLabelSnapshot view;
    view.wireId = label.wireId;
    view.playerTextLabel = label.playerTextLabel;
    view.rgba = label.rgba;
    view.x = label.x;
    view.y = label.y;
    view.z = label.z;
    view.drawDistance = label.drawDistance;
    view.los = label.los;
    view.playerAttachId = label.playerAttachId;
    view.vehicleAttachId = label.vehicleAttachId;
    view.text = label.text;
    snapshot.textLabels.push_back(std::move(view));
  }
  for (const state::MapIconView& icon : session->world.renderableMapIcons()) {
    WorldMapIconSnapshot view;
    view.id = icon.id;
    view.x = icon.x;
    view.y = icon.y;
    view.z = icon.z;
    view.type = icon.type;
    view.rgba = icon.rgba;
    view.style = icon.style;
    snapshot.mapIcons.push_back(view);
  }
  for (const state::TextDrawView& draw : session->world.renderableTextDraws()) {
    WorldTextDrawSnapshot view;
    view.wireId = draw.wireId;
    view.playerTextDraw = draw.playerTextDraw;
    view.useBox = draw.useBox;
    view.alignment = draw.alignment;
    view.letterWidth = draw.letterWidth;
    view.letterHeight = draw.letterHeight;
    view.letterColourAbgr = draw.letterColourAbgr;
    view.textWidth = draw.textWidth;
    view.textHeight = draw.textHeight;
    view.boxColourAbgr = draw.boxColourAbgr;
    view.shadow = draw.shadow;
    view.outline = draw.outline;
    view.backgroundColourAbgr = draw.backgroundColourAbgr;
    view.style = draw.style;
    view.selectable = draw.selectable;
    view.x = draw.x;
    view.y = draw.y;
    view.model = draw.model;
    view.rotationX = draw.rotationX;
    view.rotationY = draw.rotationY;
    view.rotationZ = draw.rotationZ;
    view.zoom = draw.zoom;
    view.colour1 = draw.colour1;
    view.colour2 = draw.colour2;
    view.text = draw.text;
    snapshot.textDraws.push_back(std::move(view));
  }
  for (const state::ChatLine& line : session->world.recentChat()) {
    WorldChatSnapshot chat;
    chat.rgba = line.rgba;
    chat.text = line.text;
    snapshot.chat.push_back(std::move(chat));
  }
  // Resolve each short-lived global feed row against facts that already belong
  // to this WorldState copy. Unknown/non-streamed IDs remain explicit labels;
  // a UI must not invent player identity from nearby entities.
  const auto deathName = [&session](uint16_t id) {
    if (session->world.initialized() && id == session->world.playerInit().playerId) {
      return std::string("YOU");
    }
    if (const state::Player* player = session->world.findPlayer(id)) {
      if (!player->name.empty()) return player->name;
    }
    return std::string("#") + std::to_string(id);
  };
  for (const state::DeathMessage& message : session->world.recentDeathMessages()) {
    WorldDeathMessageSnapshot death;
    death.hasKiller = message.hasKiller;
    death.killerId = message.killerId;
    death.victimId = message.victimId;
    death.reason = message.reason;
    if (message.hasKiller) death.killerName = deathName(message.killerId);
    death.victimName = deathName(message.victimId);
    snapshot.deathMessages.push_back(std::move(death));
  }
  return snapshot;
}

}  // namespace

static ProbeResult raknet_connect_impl(const std::string& ip, uint16_t port, int timeoutMs,
                                       bool retainSession, const std::string& requestedNickname) {
  ProbeResult res;
  // Android invokes this from an IO coroutine, but a dead UDP endpoint must not
  // hold the connect action for every historical retry/read loop. Keep each
  // receive caller-configurable within a sane range and cap the complete
  // open/auth/post-accept bootstrap independently. A retained session takes
  // over any later PlayerInit waiting through its normal short poll cadence.
  const int receiveTimeoutMs = std::clamp(timeoutMs, 25, 1500);
  const int connectionBudgetMs = std::clamp(receiveTimeoutMs * 8, 1000, 12000);
  const auto connectionDeadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(connectionBudgetMs);
  bool connectionDeadlineReached = false;
  auto ownedNet = std::make_unique<Net>();
  Net& net = *ownedNet;
  if (!net.open(ip, port, receiveTimeoutMs)) {
    res.error = "socket error / invalid ip";
    return res;
  }
  const auto receiveDuringConnect = [&](std::vector<uint8_t>& out) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= connectionDeadline) {
      out.clear();
      connectionDeadlineReached = true;
      return -1;
    }
    const int remainingMs = static_cast<int>(std::max<int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(connectionDeadline - now).count()));
    if (!net.setTimeout(std::min(receiveTimeoutMs, remainingMs))) {
      out.clear();
      return -1;
    }
    return net.recv(out);
  };
  const auto connectionDeadlineError = [&]() {
    return "batas waktu handshake native habis (" + std::to_string(connectionBudgetMs) + " ms)";
  };
  if (!handshake::isValidNickname(requestedNickname)) {
    res.error = "nickname harus 3-24 karakter ASCII tanpa spasi";
    return res;
  }

  logln(res, "target " + ip + ":" + std::to_string(port));

  std::vector<uint8_t> resp;

  // ---- 1. probe tanpa cookie ------------------------------------------------
  logln(res, "[1] OPEN_CONNECTION_REQUEST (probe)");
  net.send(wrap_packet({ID_OPEN_CONNECTION_REQUEST, 0x00, 0x00}, port));
  if (receiveDuringConnect(resp) <= 0) {
    res.error = connectionDeadlineReached ? connectionDeadlineError()
                                          : "timeout: server tidak membalas sama sekali";
    logln(res, "    " + res.error);
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
        if (receiveDuringConnect(resp) <= 0) continue;
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
        } else if (handshake::isTerminalSessionControlId(resp[0])) {
          res.error = describeOpenConnectionReply(resp[0], resp.size());
          logln(res, "    " + res.error);
          return res;
        } else {
          logln(res, "    " + describeOpenConnectionReply(resp[0], resp.size()));
        }
      }
    }
    if (!got) {
      res.error = connectionDeadlineReached ? connectionDeadlineError() : "cookie challenge gagal";
      logln(res, "    " + res.error);
      return res;
    }
  } else {
    res.error = describeOpenConnectionReply(resp[0], resp.size());
    logln(res, "    " + res.error);
    return res;
  }

  // ---- 3. connection request ----------------------------------------------
  // RakNet starts its reliable message-number space at zero.  Beginning at
  // one can appear to transport packets, but leaves strict legacy peers with
  // a permanent hole before NEW_INCOMING / ClientJoin.
  logln(res, "[3] CONNECTION_REQUEST (reliable, msg=0)");
  std::vector<PendingReliable> handshakePending;
  if (!sendTrackedReliable(net, port, handshakePending,
                           raknet::buildReliable(0, {ID_CONNECTION_REQUEST}), 0)) {
    res.error = "gagal mengirim CONNECTION_REQUEST";
    return res;
  }

  // ---- 4. AUTH_KEY -> ACCEPTED --------------------------------------------
  uint16_t msgNo = 1;
  uint32_t token = 0;
  bool gotChallenge = false;
  std::string answeredAuthChallenge;
  // These transport gates remain alive through the synchronous handshake and
  // the retained session hand-off. A server can split or reorder an RPC across
  // datagrams before the UI gets its first snapshot.
  raknet::ReliableMessageGate inboundReliable;
  raknet::SplitReassembler reassembler;
  raknet::ReliableOrderedGate inboundOrdered;
  for (int i = 0; i < 20 && !res.accepted; ++i) {
    if (receiveDuringConnect(resp) <= 0) {
      if (!resendTrackedReliableDue(net, handshakePending, res.error)) return res;
      continue;
    }
    raknet::Datagram dg = raknet::decodeDatagram(resp);
    consumeTrackedReliableAcks(handshakePending, dg.receivedAckMessageNumbers);
    if (!dg.ackMessageNumbers.empty()) {
      net.send(wrap_packet(raknet::buildAck(dg.ackMessageNumbers), port));
    }
    for (const auto& raw : dg.packets) {
      if (isReliableTransport(raw.reliability) && !inboundReliable.accept(raw.messageNumber)) {
        continue;
      }
      auto reassembled = reassembler.push(raw);
      if (reassembler.overflowed()) {
        res.error = "reassembly split inbound melebihi batas aman";
        return res;
      }
      for (auto packet : reassembled) {
        auto delivered = applyInboundReliableOrder(inboundOrdered, std::move(packet));
        if (inboundOrdered.overflowed()) {
          res.error = "antrean reliable-ordered inbound melebihi batas aman";
          return res;
        }
        for (const auto& pkt : delivered) {
          if (pkt.payload.empty()) continue;
          const uint8_t id = pkt.payload[0];
          if (id == ID_AUTH_KEY) {
            const auto challenge = handshake::parseAuthChallenge(pkt.payload);
            if (!challenge) {
              logln(res, "    AUTH_KEY malformed diabaikan");
              continue;
            }
            logln(res, "    AUTH_KEY challenge: " + *challenge);
            // A dropped reliable response makes RakPeer retransmit this exact
            // challenge. Our tracked packet will retry with the same message
            // number, so do not create a competing fresh ordered/reliable slot.
            if (!answeredAuthChallenge.empty() && *challenge == answeredAuthChallenge) {
              gotChallenge = true;
              continue;
            }
            const char* answer = auth_lookup(*challenge);
            if (!answer) {
              logln(res, "    challenge tidak ada di tabel ini");
              continue;
            }
            res.authenticated = true;
            std::vector<uint8_t> pl = {ID_AUTH_KEY,
                                       static_cast<uint8_t>(std::strlen(answer))};
            pl.insert(pl.end(), answer, answer + std::strlen(answer));
            const uint16_t authMessageNumber = msgNo++;
            if (!sendTrackedReliable(net, port, handshakePending,
                                     raknet::buildReliable(authMessageNumber, pl), authMessageNumber)) {
              res.error = "gagal mengirim AUTH_KEY response";
              return res;
            }
            answeredAuthChallenge = *challenge;
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
          } else if (handshake::isTerminalSessionControlId(id)) {
            res.error = id == ID_NO_FREE_INCOMING_CONNECTIONS
                ? "server penuh"
                : "server menolak atau memutus handshake native (RakNet id " +
                    std::to_string(static_cast<unsigned>(id)) + ")";
            return res;
          }
        }
      }
    }
    if (!res.accepted && !resendTrackedReliableDue(net, handshakePending, res.error)) return res;
  }

  if (!res.accepted) {
    res.error = connectionDeadlineReached ? connectionDeadlineError()
              : gotChallenge ? "AUTH_KEY dijawab tapi ACCEPTED tidak datang"
                             : "tidak menerima AUTH_KEY challenge";
    return res;
  }

  // ---- 5. RakPeer post-accept controls + ClientJoin ------------------------
  // RakPeer.cpp marks the initiating peer CONNECTED when ACCEPTED is decoded,
  // then immediately emits NEW_INCOMING, an internal ping, and an empty
  // static-data record before the game thread dispatches ClientJoin. Preserve
  // that source ordering and message-number progression: ClientJoin follows
  // those three outbound records directly. An inbound ping/static-data record
  // is processed during the initial/retained receive path below; it is not a
  // prerequisite invented between the source peer's static-data and ClientJoin.
  const auto* targetBytes = reinterpret_cast<const uint8_t*>(&net.to.sin_addr.s_addr);
  const auto incoming = handshake::buildNewIncoming(
      {targetBytes[0], targetBytes[1], targetBytes[2], targetBytes[3]}, port);
  const uint16_t incomingMessageNumber = msgNo++;
  const uint16_t pingMessageNumber = msgNo++;
  const std::vector<uint8_t> internalPingPayload =
      handshake::buildInternalPing(monotonicMilliseconds32());
  const uint16_t staticDataMessageNumber = msgNo++;
  const std::vector<uint8_t> staticDataPayload = handshake::buildEmptyStaticData();

  const uint32_t challengeResponse = token ^ handshake::kLegacyNetGameVersion;
  const std::string serial = handshake::generateLegacySerial();
  const auto join = handshake::buildClientJoin(challengeResponse, requestedNickname, serial);
  if (join.empty()) {
    res.error = "ClientJoin lokal tidak valid";
    return res;
  }
  // Legacy/open.mp client RPC uses the ordered SyncRPC stream on channel 2.
  // ClientJoin consumes index zero, so retained session RPCs continue at one.
  const uint16_t joinMessageNumber = msgNo++;

  const auto incomingFrame = raknet::buildReliable(incomingMessageNumber, incoming);
  const auto pingFrame = raknet::buildUnreliable(pingMessageNumber, internalPingPayload);
  const auto staticDataFrame = raknet::buildReliable(staticDataMessageNumber, staticDataPayload);
  const auto joinFrame = raknet::buildReliableOrdered(
      joinMessageNumber, clientrpc::kReliableOrderedChannel, 0, join);
  const auto bootstrap = raknet::buildCoalescedDatagram({
      {incomingMessageNumber, raknet::kReliabilityReliable, 0u, 0u, incoming},
      {pingMessageNumber, raknet::kReliabilityUnreliable, 0u, 0u, internalPingPayload},
      {staticDataMessageNumber, raknet::kReliabilityReliable, 0u, 0u, staticDataPayload},
      {joinMessageNumber, raknet::kReliabilityReliableOrdered,
       clientrpc::kReliableOrderedChannel, 0u, join},
  });
  if (incomingFrame.empty() || pingFrame.empty() || staticDataFrame.empty() ||
      joinFrame.empty() || bootstrap.empty()) {
    res.error = "bootstrap RakNet lokal tidak valid";
    return res;
  }

  // RakPeer's normal update tick coalesces these source-ordered records into
  // one no-ACK datagram. Keep individual reliable copies for retransmission:
  // loss recovery may legally send a later frame alone without changing its
  // message/order identity.
  const size_t pendingBeforeBootstrap = handshakePending.size();
  if (!queueTrackedReliable(port, handshakePending, incomingFrame, incomingMessageNumber) ||
      !queueTrackedReliable(port, handshakePending, staticDataFrame, staticDataMessageNumber) ||
      !queueTrackedReliable(port, handshakePending, joinFrame, joinMessageNumber)) {
    handshakePending.resize(pendingBeforeBootstrap);
    res.error = "antrian reliable bootstrap penuh";
    return res;
  }
  if (!net.send(wrap_packet(bootstrap, port))) {
    handshakePending.resize(pendingBeforeBootstrap);
    res.error = "gagal mengirim bootstrap RakNet + ClientJoin";
    return res;
  }

  char jb[160];
  std::snprintf(jb, sizeof(jb), "[5] post-accept RakPeer + ClientJoin (nick %s, cr %u)",
                requestedNickname.c_str(), challengeResponse);
  logln(res, jb);

  // ---- 6. dengarkan stream server ----------------------------------------
  int rxPackets = 0;
  int rpcCount = 0;
  int splitFragments = 0;
  int reassembledPackets = 0;
  std::string rpcSummary;
  std::string stateSummary;
  state::WorldState world;
  // A retained session must return control to Android promptly even if a
  // compatible server is quiet after ClientJoin. Further PlayerInit/world
  // packets are consumed by lifecycle polling rather than monopolising this
  // one connect coroutine for twenty full socket timeouts. Eight bounded reads
  // cover the source-equivalent 2/3/4/5 bootstrap's separate transport ACKs
  // before its first game-layer frame without turning connect into a long wait.
  constexpr int kMaxInitialSessionReadPolls = 8;
  for (int i = 0; i < kMaxInitialSessionReadPolls && !connectionDeadlineReached; ++i) {
    if (receiveDuringConnect(resp) <= 0) {
      if (connectionDeadlineReached || !resendTrackedReliableDue(net, handshakePending, res.error)) break;
      continue;
    }
    raknet::Datagram dg = raknet::decodeDatagram(resp);
    consumeTrackedReliableAcks(handshakePending, dg.receivedAckMessageNumbers);
    if (!dg.ackMessageNumbers.empty()) {
      net.send(wrap_packet(raknet::buildAck(dg.ackMessageNumbers), port));
    }
    for (const auto& raw : dg.packets) {
      if (isReliableTransport(raw.reliability) && !inboundReliable.accept(raw.messageNumber)) {
        continue;
      }
      if (raw.split) ++splitFragments;
      auto reassembled = reassembler.push(raw);
      if (reassembler.overflowed()) {
        res.error = "reassembly split inbound melebihi batas aman";
        return res;
      }
      for (auto packet : reassembled) {
        auto delivered = applyInboundReliableOrder(inboundOrdered, std::move(packet));
        if (inboundOrdered.overflowed()) {
          res.error = "antrean reliable-ordered inbound melebihi batas aman";
          return res;
        }
        for (const auto& pkt : delivered) {
          if (pkt.payload.empty()) continue;
          ++rxPackets;
          if (pkt.reassembled) ++reassembledPackets;
          if (pkt.payload[0] == ID_INTERNAL_PING) {
            if (!reply_to_internal_ping(net, port, msgNo, pkt.payload)) {
              res.error = "gagal membalas RakNet internal ping";
              break;
            }
            continue;
          }
          if (pkt.payload[0] == ID_REQUEST_STATIC_DATA) {
            const uint16_t staticMessageNumber = msgNo++;
            if (!sendTrackedReliable(net, port, handshakePending,
                                     raknet::buildReliable(staticMessageNumber,
                                                           handshake::buildEmptyStaticData()),
                                     staticMessageNumber)) {
              res.error = "gagal membalas request static-data RakNet";
              break;
            }
            continue;
          }
          if (pkt.payload[0] == ID_RECEIVED_STATIC_DATA) continue;
          // A connection can receive unrelated broadcast traffic before the
          // legacy game layer accepts ClientJoin.  Never call that a successful
          // gameplay join; surface the transport refusal instead.
          if (handshake::isTerminalSessionControlId(pkt.payload[0])) {
            res.error = "server menolak atau memutus sesi native (RakNet id " +
                        std::to_string(static_cast<unsigned>(pkt.payload[0])) + ")";
            break;
          }
          if (pkt.payload[0] != 20 || pkt.payload.size() < 2) continue;

          ++rpcCount;
          const std::string label = rpc::summarize(pkt.payload);
          if (rpcSummary.size() + label.size() + 2 < 320) {
            if (!rpcSummary.empty()) rpcSummary += ", ";
            rpcSummary += label;
          }

          state::RpcEvent stateEvent;
          if (world.applyRpc(pkt.payload, stateEvent)) {
            if (!stateEvent.summary.empty() &&
                stateSummary.size() + stateEvent.summary.size() + 2 < 240) {
              if (!stateSummary.empty()) stateSummary += ", ";
              stateSummary += stateEvent.summary;
            }
            // A server can reject/restart a recipient before it ever emits
            // PlayerInit. Do not downgrade that explicit terminal RPC into the
            // generic "menunggu PlayerInit" state or retain its socket.
            if (stateEvent.kind == state::RpcEventKind::SessionCloseRequested) {
              res.error = "server mengakhiri atau menolak sesi native (" +
                          stateEvent.summary + ")";
              break;
            }
          }
        }
        if (!res.error.empty()) break;
      }
      if (!res.error.empty()) break;
    }
    if (!res.error.empty() || !resendTrackedReliableDue(net, handshakePending, res.error)) break;
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

  // Server broadcasts can begin as soon as RakNet accepts a UDP peer.  A
  // playable legacy session exists only after InitGame/PlayerInit is decoded.
  res.joined = res.playerInit;
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
  logln(res, "");
  if (!res.error.empty()) {
    logln(res, "STATUS: " + res.error);
  } else if (res.playerInit) {
    logln(res, "STATUS: InitGame diterima; sesi gameplay siap memilih class");
  } else {
    logln(res, "STATUS: RakNet tersambung, menunggu PlayerInit setelah ClientJoin");
  }

  if (retainSession && res.error.empty()) {
    // raknet_session_connect holds gLiveSessionMutex across this implementation.
    // Move the opened UDP socket and all decoder state, rather than reopening
    // a second connection or losing ordering/message counters after ClientJoin.
    auto live = std::make_unique<LiveSession>();
    live->net = std::move(ownedNet);
    live->port = port;
    live->nextMessageNumber = msgNo;
    live->orderingCounters = {};
    // ClientJoin is the first RELIABLE_ORDERED packet on SyncRPC channel 2.
    // Continue at index one so RequestClass cannot be discarded as its stale
    // duplicate by legacy/open.mp servers.
    live->orderingCounters.nextRpc = 1u;
    // Continue source-equivalent ACK/retry ownership across the synchronous
    // handshake → retained-session hand-off. This includes any NEW_INCOMING,
    // empty static-data, or ClientJoin record that the server has not ACKed.
    live->pendingReliable = std::move(handshakePending);
    // ClientJoin/static-data was the most recent reliable handshake send;
    // start the steady-state keepalive interval from this hand-off.
    live->lastReliableSent = std::chrono::steady_clock::now();
    live->accepted = res.accepted;
    live->joined = res.joined;
    live->receivedPackets = rxPackets;
    live->receivedRpcs = rpcCount;
    live->splitFragments = splitFragments;
    live->reassembledPackets = reassembledPackets;
    live->rpcSummary = std::move(rpcSummary);
    live->stateSummary = std::move(stateSummary);
    // Preserve all inbound transport gates across the synchronous handshake →
    // live hand-off. Resetting receipt/order state could replay a reliable
    // control frame or make a later ordered index wait forever after early
    // PlayerInit/world frames were already reduced above.
    live->inboundReliable = std::move(inboundReliable);
    live->reassembler = std::move(reassembler);
    live->inboundOrdered = std::move(inboundOrdered);
    live->world = std::move(world);
    // The synchronous handoff can already contain PlayerInit. Preserve the
    // same stock-client class bootstrap that processSessionDatagram applies
    // when InitGame arrives later during the retained poll loop.
    if (live->world.initialized()) requestInitialClass(*live);
    gLiveSession = std::move(live);
  }
  return res;
}

ProbeResult raknet_probe(const std::string& ip, uint16_t port, int timeoutMs) {
  return raknet_connect_impl(ip, port, timeoutMs, false, "SampDroid");
}

SessionSnapshot raknet_session_connect(const std::string& ip, uint16_t port, int timeoutMs,
                                       const std::string& nickname) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  gLiveSession.reset();

  const ProbeResult result = raknet_connect_impl(ip, port, timeoutMs, true, nickname);
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
  const int firstReceive = gLiveSession->net->recv(packet);
  if (firstReceive > 0) {
    processSessionDatagram(*gLiveSession, packet);
  }
  // A 30–40 ms Android poll must not reduce a busy server to one queued
  // datagram per poll. Drain a finite burst after the one permitted blocking
  // receive so remote sync, ACKs, and RPC lifecycle changes keep up without an
  // unbounded UI-thread/UDP loop. Off-path datagrams are still peer-filtered in
  // Net::recv; a zero result merely consumes one bounded burst slot.
  constexpr size_t kMaxImmediateDatagramsPerPoll = 16u;
  if (firstReceive >= 0) {
    for (size_t index = 1u;
         index < kMaxImmediateDatagramsPerPoll && gLiveSession->net &&
         gLiveSession->net->fd >= 0;
         ++index) {
      const int received = gLiveSession->net->recv(packet, true);
      if (received < 0) break;
      if (received > 0) processSessionDatagram(*gLiveSession, packet);
    }
  }
  resendDueReliable(*gLiveSession);
  sendDueReliableKeepalive(*gLiveSession);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_request_class(uint16_t classId) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->accepted || !gLiveSession->world.initialized()) {
    gLiveSession->error = "class menunggu ClientJoin dan PlayerInit";
    return snapshotFrom(gLiveSession.get());
  }
  const uint32_t classCount = gLiveSession->world.playerInit().spawnClassCount;
  if (classCount == 0 || static_cast<uint32_t>(classId) >= classCount) {
    gLiveSession->error = "ID class di luar SetSpawnInfoCount server";
    return snapshotFrom(gLiveSession.get());
  }
  if (gLiveSession->spawnConfirmationSent && !gLiveSession->spawnConfirmationAcked) {
    gLiveSession->error = "PlayerSpawn sebelumnya masih menunggu ACK server";
    return snapshotFrom(gLiveSession.get());
  }
  constexpr auto kClassResponseTimeout = std::chrono::seconds(8);
  const auto now = std::chrono::steady_clock::now();
  if (gLiveSession->classRequestPending && now - gLiveSession->classRequestSent < kClassResponseTimeout) {
    gLiveSession->error = "menunggu balasan class server";
    return snapshotFrom(gLiveSession.get());
  }
  // A server may lose a response despite reliable request transport. Let an
  // explicit retry proceed after a bounded wait; reliable ordering still makes
  // the server observe requests in sequence.
  gLiveSession->classRequestPending = false;

  const auto rpc = clientrpc::buildRequestClass(classId);
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim PlayerRequestClass";
  } else {
    gLiveSession->world.beginClassRequest();
    gLiveSession->spawnConfirmationSent = false;
    gLiveSession->spawnConfirmationAcked = false;
    gLiveSession->spawnRequestPending = false;
    gLiveSession->classRequestPending = true;
    gLiveSession->requestedClassId = classId;
    gLiveSession->classRequestSent = std::chrono::steady_clock::now();
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
  if (gLiveSession->spawnConfirmationSent && !gLiveSession->spawnConfirmationAcked) {
    gLiveSession->error = "PlayerSpawn sebelumnya masih menunggu ACK server";
    return snapshotFrom(gLiveSession.get());
  }
  constexpr auto kSpawnResponseTimeout = std::chrono::seconds(8);
  const auto now = std::chrono::steady_clock::now();
  if (gLiveSession->spawnRequestPending &&
      now - gLiveSession->spawnRequestSent < kSpawnResponseTimeout) {
    gLiveSession->error = "menunggu balasan spawn server";
    return snapshotFrom(gLiveSession.get());
  }
  // A response may be lost after RakNet transport ACKs the reliable request.
  // Permit an explicit retry only after a bounded wait; ordered RPC semantics
  // still preserve the server-observed sequence.
  gLiveSession->spawnRequestPending = false;

  const auto rpc = clientrpc::buildRequestSpawn();
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim PlayerRequestSpawn";
  } else {
    gLiveSession->world.beginSpawnRequest();
    gLiveSession->spawnConfirmationSent = false;
    gLiveSession->spawnConfirmationAcked = false;
    gLiveSession->spawnRequestPending = true;
    gLiveSession->spawnRequestSent = std::chrono::steady_clock::now();
    invalidateLocalGameplay(*gLiveSession);
    appendBounded(gLiveSession->stateSummary, "PlayerRequestSpawn", 240);
  }
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_update_controls(const input::TouchIntent& input) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }

  const state::LocalPlayer& local = gLiveSession->world.localPlayer();
  if (serverMarkedDead(local)) {
    gLiveSession->controls.reset();
    invalidateLocalGameplay(*gLiveSession);
    gLiveSession->nativeControlsKnown = true;
    gLiveSession->error.clear();
    return snapshotFrom(gLiveSession.get());
  }
  if (local.controllableKnown && !local.controllable) {
    gLiveSession->controls.reset();
    invalidateLocalGameplay(*gLiveSession);
    gLiveSession->nativeControlsKnown = true;
    gLiveSession->error.clear();
    return snapshotFrom(gLiveSession.get());
  }
  if (!gLiveSession->controls.headingKnown()) {
    if (local.vehicleKnown && local.inVehicle) {
      if (const state::Vehicle* vehicle = gLiveSession->world.findVehicle(local.vehicleId)) {
        gLiveSession->controls.synchronizeHeading(vehicle->angle);
      }
    } else if (local.facingKnown) {
      gLiveSession->controls.synchronizeHeading(local.angle);
    }
  }
  if (!gLiveSession->controls.update(input)) {
    gLiveSession->error = "input touch native tidak valid";
    return snapshotFrom(gLiveSession.get());
  }

  gLiveSession->nativeControlsKnown = true;
  rebuildLocalOnFootPreview(*gLiveSession);
  gLiveSession->error.clear();
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_send_chat(const std::string& message) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->accepted || !gLiveSession->world.initialized()) {
    gLiveSession->error = "chat menunggu sesi accepted dan PlayerInit";
    return snapshotFrom(gLiveSession.get());
  }

  const bool command = !message.empty() && message.front() == '/';
  const std::vector<uint8_t> rpc = command ? clientrpc::buildCommandMessage(message)
                                             : clientrpc::buildChatMessage(message);
  if (rpc.empty()) {
    gLiveSession->error = command ? "command tidak valid atau melebihi batas" :
                                    "chat tidak valid atau melebihi batas";
    return snapshotFrom(gLiveSession.get());
  }

  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = command ? "gagal mengirim command" : "gagal mengirim chat";
    return snapshotFrom(gLiveSession.get());
  }

  gLiveSession->error.clear();
  ++gLiveSession->outboundChatMessages;
  appendBounded(gLiveSession->stateSummary,
                std::string(command ? "Command" : "Chat") + " #" +
                    std::to_string(gLiveSession->outboundChatMessages),
                240);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_respond_dialog(int16_t dialogId, uint64_t dialogRevision,
                                              bool response, int16_t listItem,
                                              const std::string& text) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->accepted || !gLiveSession->world.initialized()) {
    gLiveSession->error = "dialog menunggu sesi accepted dan PlayerInit";
    return snapshotFrom(gLiveSession.get());
  }
  if (!gLiveSession->world.dialog().active || gLiveSession->world.dialog().id != dialogId ||
      gLiveSession->world.dialog().revision != dialogRevision) {
    gLiveSession->error = "dialog server sudah tidak aktif, berubah, atau ID tidak cocok";
    return snapshotFrom(gLiveSession.get());
  }
  if (listItem < -1) {
    gLiveSession->error = "pilihan dialog tidak valid";
    return snapshotFrom(gLiveSession.get());
  }

  const std::vector<uint8_t> rpc = clientrpc::buildDialogResponse(dialogId, response, listItem, text);
  if (rpc.empty()) {
    gLiveSession->error = "respon dialog tidak valid atau melebihi batas";
    return snapshotFrom(gLiveSession.get());
  }
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim respon dialog";
    return snapshotFrom(gLiveSession.get());
  }
  if (!gLiveSession->world.acknowledgeDialogResponse(dialogId, dialogRevision)) {
    // This should be unreachable under the session mutex, but do not report a
    // successful UI completion if the state ever changes unexpectedly.
    gLiveSession->error = "dialog berubah sebelum respon dapat diterapkan";
    return snapshotFrom(gLiveSession.get());
  }
  gLiveSession->error.clear();
  ++gLiveSession->outboundDialogResponses;
  appendBounded(gLiveSession->stateSummary,
                "DialogResponse #" + std::to_string(gLiveSession->outboundDialogResponses), 240);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_select_menu(uint8_t menuId, uint64_t menuRevision, uint8_t row) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->accepted || !gLiveSession->world.initialized()) {
    gLiveSession->error = "menu menunggu sesi accepted dan PlayerInit";
    return snapshotFrom(gLiveSession.get());
  }
  const state::Menu& menu = gLiveSession->world.menu();
  const uint8_t rowCount = std::max(menu.column1Count, menu.column2Count);
  if (!menu.active || menu.id != menuId || menu.revision != menuRevision || !menu.enabled ||
      row >= rowCount || !menu.rowEnabled[row]) {
    gLiveSession->error = "menu server sudah berubah, nonaktif, atau pilihan tidak valid";
    return snapshotFrom(gLiveSession.get());
  }
  const std::vector<uint8_t> rpc = clientrpc::buildMenuSelect(row);
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim pilihan menu";
    return snapshotFrom(gLiveSession.get());
  }
  if (!gLiveSession->world.acknowledgeMenuSelection(menuId, menuRevision, row)) {
    gLiveSession->error = "menu berubah sebelum pilihan dapat diterapkan";
    return snapshotFrom(gLiveSession.get());
  }
  gLiveSession->error.clear();
  appendBounded(gLiveSession->stateSummary,
                "MenuSelect #" + std::to_string(menuId) + ":" + std::to_string(row), 240);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_exit_menu(uint8_t menuId, uint64_t menuRevision) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->accepted || !gLiveSession->world.initialized()) {
    gLiveSession->error = "menu menunggu sesi accepted dan PlayerInit";
    return snapshotFrom(gLiveSession.get());
  }
  const state::Menu& menu = gLiveSession->world.menu();
  if (!menu.active || menu.id != menuId || menu.revision != menuRevision) {
    gLiveSession->error = "menu server sudah tidak aktif, berubah, atau ID tidak cocok";
    return snapshotFrom(gLiveSession.get());
  }
  const std::vector<uint8_t> rpc = clientrpc::buildMenuExit();
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim keluar menu";
    return snapshotFrom(gLiveSession.get());
  }
  if (!gLiveSession->world.acknowledgeMenuExit(menuId, menuRevision)) {
    gLiveSession->error = "menu berubah sebelum keluar dapat diterapkan";
    return snapshotFrom(gLiveSession.get());
  }
  gLiveSession->error.clear();
  appendBounded(gLiveSession->stateSummary, "MenuExit #" + std::to_string(menuId), 240);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_use_nearby(bool passengerIntent) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->accepted || !gLiveSession->world.initialized() ||
      !gLiveSession->spawnConfirmationAcked) {
    gLiveSession->error = "interaksi menunggu sesi spawn native yang sudah di-ACK";
    return snapshotFrom(gLiveSession.get());
  }
  const state::LocalPlayer& local = gLiveSession->world.localPlayer();
  if (serverMarkedDead(local)) {
    gLiveSession->error = "interaksi dihentikan karena health server nol";
    return snapshotFrom(gLiveSession.get());
  }
  if (local.controllableKnown && !local.controllable) {
    gLiveSession->error = "interaksi dihentikan TogglePlayerControllable server";
    return snapshotFrom(gLiveSession.get());
  }

  enum class InteractionKind : uint8_t { Pickup, EnterVehicle, ExitVehicle };
  InteractionKind kind = InteractionKind::Pickup;
  int32_t pickupId = -1;
  uint16_t vehicleId = 0;
  if (local.vehicleKnown && local.inVehicle) {
    kind = InteractionKind::ExitVehicle;
    vehicleId = local.vehicleId;
  } else {
    if (!local.positionKnown) {
      gLiveSession->error = "interaksi butuh posisi local server";
      return snapshotFrom(gLiveSession.get());
    }
    float x = local.x;
    float y = local.y;
    float z = local.z;
    if (gLiveSession->localGameplay.predictionActive &&
        gLiveSession->localGameplay.motion.initialized) {
      x = gLiveSession->localGameplay.motion.position.x;
      y = gLiveSession->localGameplay.motion.position.y;
      z = gLiveSession->localGameplay.motion.position.z;
    }
    // Pickup RPCs lack world/interior facts, so never infer that one is valid
    // in a nonzero interior. Vehicle stream records do have interior, and their
    // selection is explicitly filtered against the local authoritative value.
    if (!local.interiorKnown || local.interior == 0) {
      constexpr float kPickupUseRadius = 2.15f;
      if (const state::Pickup* pickup =
              gLiveSession->world.nearestPickup(x, y, z, kPickupUseRadius)) {
        kind = InteractionKind::Pickup;
        pickupId = pickup->id;
      }
    }
    if (pickupId < 0) {
      constexpr float kVehicleUseRadius = 3.25f;
      const state::Vehicle* vehicle = gLiveSession->world.nearestVehicle(
          x, y, z, kVehicleUseRadius, local.interior, local.interiorKnown);
      if (!vehicle) {
        gLiveSession->error = "tidak ada pickup atau kendaraan server dalam jarak interaksi";
        return snapshotFrom(gLiveSession.get());
      }
      // SetVehicleParamsForPlayer/RPC-24 door state is recipient authority.
      // Mirror the PC client's locked-door boundary by refusing to manufacture
      // an EnterVehicle RPC; an unset field stays permissive and server RPC-70
      // remains the only authority that can place a player in a seat.
      if (state::isRecipientVehicleEntryLocked(*vehicle)) {
        gLiveSession->error = "kendaraan terkunci oleh parameter server";
        return snapshotFrom(gLiveSession.get());
      }
      kind = InteractionKind::EnterVehicle;
      vehicleId = vehicle->id;
    }
  }

  const uint64_t nowMs = monotonicMilliseconds64();
  constexpr uint64_t kInteractionUseMinimumIntervalMs = 450;
  if (gLiveSession->lastInteractionUseMs != 0 &&
      nowMs - gLiveSession->lastInteractionUseMs < kInteractionUseMinimumIntervalMs) {
    gLiveSession->error = "interaksi terlalu cepat";
    return snapshotFrom(gLiveSession.get());
  }

  std::vector<uint8_t> rpc;
  switch (kind) {
    case InteractionKind::Pickup:
      rpc = clientrpc::buildPickedUpPickup(pickupId);
      break;
    case InteractionKind::EnterVehicle:
      // A normal USE asks for driver; long-press USE asks for passenger. Both
      // are intent only: the server can deny/reassign and RPC 70 is still the
      // only authority that updates local seat state.
      rpc = clientrpc::buildEnterVehicle(vehicleId, passengerIntent);
      break;
    case InteractionKind::ExitVehicle:
      rpc = clientrpc::buildExitVehicle(vehicleId);
      break;
  }
  if (rpc.empty()) {
    gLiveSession->error = "target interaksi server tidak valid";
    return snapshotFrom(gLiveSession.get());
  }
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim interaksi server";
    return snapshotFrom(gLiveSession.get());
  }
  gLiveSession->lastInteractionUseMs = nowMs;
  gLiveSession->error.clear();
  if (kind == InteractionKind::Pickup) {
    ++gLiveSession->outboundPickupNotifications;
    appendBounded(gLiveSession->stateSummary,
                  "PickupUse #" + std::to_string(gLiveSession->outboundPickupNotifications) +
                      " id=" + std::to_string(pickupId),
                  240);
  } else {
    ++gLiveSession->outboundVehicleInteractions;
    const std::string action = kind == InteractionKind::EnterVehicle
                                   ? (passengerIntent ? "EnterVehiclePassenger" : "EnterVehicleDriver")
                                   : "ExitVehicle";
    appendBounded(gLiveSession->stateSummary,
                  action + " #" + std::to_string(gLiveSession->outboundVehicleInteractions) +
                      " id=" + std::to_string(vehicleId),
                  240);
  }
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_request_scores() {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->accepted || !gLiveSession->world.initialized()) {
    gLiveSession->error = "scoreboard menunggu sesi accepted dan PlayerInit";
    return snapshotFrom(gLiveSession.get());
  }
  const uint64_t nowMs = monotonicMilliseconds64();
  constexpr uint64_t kScoreRequestMinimumIntervalMs = 1'000;
  if (gLiveSession->lastScoreRequestMs != 0 &&
      nowMs - gLiveSession->lastScoreRequestMs < kScoreRequestMinimumIntervalMs) {
    gLiveSession->error = "permintaan scoreboard terlalu cepat";
    return snapshotFrom(gLiveSession.get());
  }
  const std::vector<uint8_t> rpc = clientrpc::buildRequestScoresAndPings();
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal meminta scoreboard";
    return snapshotFrom(gLiveSession.get());
  }
  gLiveSession->lastScoreRequestMs = nowMs;
  gLiveSession->error.clear();
  ++gLiveSession->outboundScoreRequests;
  appendBounded(gLiveSession->stateSummary,
                "RequestScores #" + std::to_string(gLiveSession->outboundScoreRequests), 240);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_select_textdraw(uint16_t wireId) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->accepted || !gLiveSession->world.initialized()) {
    gLiveSession->error = "textdraw menunggu sesi accepted dan PlayerInit";
    return snapshotFrom(gLiveSession.get());
  }
  if (!gLiveSession->world.isSelectableTextDraw(wireId)) {
    gLiveSession->error = "textdraw tidak selectable atau mode pilih server sudah berakhir";
    return snapshotFrom(gLiveSession.get());
  }
  const std::vector<uint8_t> rpc = clientrpc::buildSelectTextDraw(wireId);
  if (rpc.empty()) {
    gLiveSession->error = "ID pilihan textdraw tidak valid";
    return snapshotFrom(gLiveSession.get());
  }
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim pilihan textdraw";
    return snapshotFrom(gLiveSession.get());
  }
  ++gLiveSession->outboundTextDrawSelections;
  gLiveSession->error.clear();
  appendBounded(gLiveSession->stateSummary,
                "TextDrawSelect #" + std::to_string(gLiveSession->outboundTextDrawSelections) +
                    " id=" + std::to_string(wireId),
                240);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_select_object(uint16_t objectId, uint64_t expectedRevision) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->accepted || !gLiveSession->world.initialized()) {
    gLiveSession->error = "pilih objek menunggu sesi accepted dan PlayerInit";
    return snapshotFrom(gLiveSession.get());
  }
  const uint64_t selectionRevision = gLiveSession->world.objectSelectionRevision();
  if (!gLiveSession->world.objectSelectionActive() || expectedRevision == 0u ||
      expectedRevision != selectionRevision) {
    gLiveSession->error = "revisi pilihan objek sudah stale atau mode server berakhir";
    return snapshotFrom(gLiveSession.get());
  }
  if (gLiveSession->objectSelectionTerminalRevision == selectionRevision) {
    gLiveSession->error = "pilihan objek untuk mode server ini sudah dikirim";
    return snapshotFrom(gLiveSession.get());
  }
  const uint64_t nowMs = monotonicMilliseconds64();
  constexpr uint64_t kObjectSelectionMinimumIntervalMs = 120u;
  if (gLiveSession->lastObjectSelectionRevision == selectionRevision &&
      gLiveSession->lastObjectSelectionMs != 0u &&
      nowMs - gLiveSession->lastObjectSelectionMs < kObjectSelectionMinimumIntervalMs) {
    gLiveSession->error = "pilihan objek terlalu cepat";
    return snapshotFrom(gLiveSession.get());
  }

  state::ServerObjectView selected;
  if (!gLiveSession->world.selectedObjectView(objectId, selected)) {
    gLiveSession->error = "objek tidak lagi tersedia atau mode pilih server berakhir";
    return snapshotFrom(gLiveSession.get());
  }
  // Android sends only the object ID. Model and origin are re-sampled under
  // this session mutex from the current server-owned object lifecycle so a
  // stale UI frame cannot nominate another model or an arbitrary callback hit.
  const std::vector<uint8_t> rpc = clientrpc::buildSelectObject(
      selected.id, selected.model, selected.x, selected.y, selected.z);
  if (rpc.empty()) {
    gLiveSession->error = "payload pilihan objek tidak valid";
    return snapshotFrom(gLiveSession.get());
  }
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim pilihan objek";
    return snapshotFrom(gLiveSession.get());
  }
  gLiveSession->lastObjectSelectionMs = nowMs;
  gLiveSession->lastObjectSelectionRevision = selectionRevision;
  gLiveSession->objectSelectionTerminalRevision = selectionRevision;
  ++gLiveSession->outboundObjectSelections;
  gLiveSession->error.clear();
  appendBounded(gLiveSession->stateSummary,
                "ObjectSelect #" + std::to_string(gLiveSession->outboundObjectSelections) +
                    " id=" + std::to_string(selected.id) +
                    " model=" + std::to_string(selected.model),
                240);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_edit_object(uint32_t response, uint64_t expectedRevision,
                                           float x, float y, float z,
                                           float rotationX, float rotationY, float rotationZ) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->accepted || !gLiveSession->world.initialized()) {
    gLiveSession->error = "edit objek menunggu sesi accepted dan PlayerInit";
    return snapshotFrom(gLiveSession.get());
  }
  constexpr uint32_t kObjectEditCancel = 0u;
  constexpr uint32_t kObjectEditFinal = 1u;
  constexpr uint32_t kObjectEditUpdate = 2u;
  if (response > kObjectEditUpdate) {
    gLiveSession->error = "respons edit objek tidak valid";
    return snapshotFrom(gLiveSession.get());
  }
  const state::ObjectEditState& edit = gLiveSession->world.objectEdit();
  if (!edit.active || edit.objectId == 0u || edit.objectId == 0xFFFFu ||
      edit.revision == 0u) {
    gLiveSession->error = "mode edit objek server sudah berakhir";
    return snapshotFrom(gLiveSession.get());
  }
  if (expectedRevision == 0u || expectedRevision != edit.revision) {
    gLiveSession->error = "revisi edit objek sudah stale";
    return snapshotFrom(gLiveSession.get());
  }
  if (gLiveSession->objectEditTerminalRevision == edit.revision) {
    gLiveSession->error = "respons akhir edit objek sudah dikirim";
    return snapshotFrom(gLiveSession.get());
  }
  const uint64_t nowMs = monotonicMilliseconds64();
  constexpr uint64_t kObjectEditUpdateMinimumIntervalMs = 90u;
  if (response == kObjectEditUpdate &&
      gLiveSession->objectEditUpdateRevision == edit.revision &&
      gLiveSession->lastObjectEditUpdateMs != 0u &&
      nowMs - gLiveSession->lastObjectEditUpdateMs < kObjectEditUpdateMinimumIntervalMs) {
    gLiveSession->error = "update edit objek terlalu cepat";
    return snapshotFrom(gLiveSession.get());
  }
  state::ServerObjectView edited;
  if (!gLiveSession->world.editedObjectView(edit.objectId, edit.playerObject, edited) ||
      edited.id != edit.objectId) {
    gLiveSession->error = "objek edit tidak lagi tersedia";
    return snapshotFrom(gLiveSession.get());
  }
  // Android submits only a bounded transform. The server-selected object ID
  // and global/player classification are sampled from WorldState under this
  // mutex, so stale UI cannot retarget another lifecycle record.
  const std::vector<uint8_t> rpc = clientrpc::buildObjectEditResponse(
      edit.playerObject, edit.objectId, response, x, y, z,
      rotationX, rotationY, rotationZ);
  if (rpc.empty()) {
    gLiveSession->error = "transform edit objek tidak valid";
    return snapshotFrom(gLiveSession.get());
  }
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim respons edit objek";
    return snapshotFrom(gLiveSession.get());
  }
  if (response == kObjectEditUpdate) {
    gLiveSession->lastObjectEditUpdateMs = nowMs;
    gLiveSession->objectEditUpdateRevision = edit.revision;
  } else if (response == kObjectEditCancel || response == kObjectEditFinal) {
    gLiveSession->objectEditTerminalRevision = edit.revision;
  }
  ++gLiveSession->outboundObjectEdits;
  gLiveSession->error.clear();
  const char* responseLabel = response == kObjectEditUpdate ? "update" :
                              (response == kObjectEditFinal ? "final" : "cancel");
  appendBounded(gLiveSession->stateSummary,
                "ObjectEdit " + std::string(responseLabel) + " #" +
                    std::to_string(gLiveSession->outboundObjectEdits) +
                    " id=" + std::to_string(edit.objectId),
                240);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_edit_attached_object(
    uint32_t response, uint64_t expectedRevision,
    float offsetX, float offsetY, float offsetZ,
    float rotationX, float rotationY, float rotationZ,
    float scaleX, float scaleY, float scaleZ) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!gLiveSession->accepted || !gLiveSession->world.initialized()) {
    gLiveSession->error = "edit attachment menunggu sesi accepted dan PlayerInit";
    return snapshotFrom(gLiveSession.get());
  }
  if (response > 1u) {
    gLiveSession->error = "respons edit attachment tidak valid";
    return snapshotFrom(gLiveSession.get());
  }
  const state::AttachedObjectEditState& edit = gLiveSession->world.attachedObjectEdit();
  if (!edit.active || edit.slot >= state::kMaxPlayerAttachedObjectSlots || edit.revision == 0u) {
    gLiveSession->error = "mode edit attachment server sudah berakhir";
    return snapshotFrom(gLiveSession.get());
  }
  if (expectedRevision == 0u || expectedRevision != edit.revision) {
    gLiveSession->error = "revisi edit attachment sudah stale";
    return snapshotFrom(gLiveSession.get());
  }
  if (gLiveSession->attachedObjectEditTerminalRevision == edit.revision) {
    gLiveSession->error = "respons edit attachment sudah dikirim";
    return snapshotFrom(gLiveSession.get());
  }
  state::PlayerAttachedObject attachment;
  if (!gLiveSession->world.editedAttachedObject(edit.slot, attachment)) {
    gLiveSession->error = "slot attachment edit tidak lagi tersedia";
    return snapshotFrom(gLiveSession.get());
  }
  // Kotlin cannot nominate model/bone/colour or a slot. Re-sample all of
  // those native facts under the session mutex; only the adjustment vectors
  // cross the Java boundary and the builder bounds them again.
  const std::vector<uint8_t> rpc = clientrpc::buildAttachedObjectEditResponse(
      response, edit.slot, attachment.model, attachment.bone,
      offsetX, offsetY, offsetZ, rotationX, rotationY, rotationZ,
      scaleX, scaleY, scaleZ, abgrToArgb(attachment.colour1Abgr),
      abgrToArgb(attachment.colour2Abgr));
  if (rpc.empty()) {
    gLiveSession->error = "transform attachment edit tidak valid";
    return snapshotFrom(gLiveSession.get());
  }
  const uint16_t messageNumber = gLiveSession->nextMessageNumber++;
  const uint16_t orderingIndex = gLiveSession->orderingCounters.takeRpc();
  const auto packet = raknet::buildReliableOrdered(messageNumber, clientrpc::kReliableOrderedChannel, orderingIndex, rpc);
  if (!sendSessionReliable(*gLiveSession, packet, messageNumber)) {
    gLiveSession->error = "gagal mengirim respons edit attachment";
    return snapshotFrom(gLiveSession.get());
  }
  gLiveSession->attachedObjectEditTerminalRevision = edit.revision;
  ++gLiveSession->outboundAttachedObjectEdits;
  gLiveSession->error.clear();
  appendBounded(gLiveSession->stateSummary,
                std::string("AttachedObjectEdit ") + (response == 0u ? "cancel" : "save") +
                    " #" + std::to_string(gLiveSession->outboundAttachedObjectEdits) +
                    " slot=" + std::to_string(edit.slot),
                240);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_send_onfoot(const sync::OnFootState& state) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  (void)sendOnFootLocked(*gLiveSession, state);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_send_incar(const sync::InCarState& state) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  (void)sendInCarLocked(*gLiveSession, state);
  return snapshotFrom(gLiveSession.get());
}

SessionSnapshot raknet_session_advance_gameplay(
    const gta::world::CollisionWorld& collisionWorld, float deltaSeconds,
    const vehiclemotion::VehicleMotionTuning* vehicleTuning,
    uint32_t vehicleTuningModel) {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->net || gLiveSession->net->fd < 0) {
    return snapshotFrom(nullptr);
  }
  if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f || deltaSeconds > 0.25f) {
    gLiveSession->error = "delta gameplay native tidak valid";
    return snapshotFrom(gLiveSession.get());
  }
  if (!syncpolicy::onFootSessionReady(
          gLiveSession->accepted, gLiveSession->world.initialized(),
          gLiveSession->world.spawnResponseKnown(), gLiveSession->world.spawnAllowed(),
          gLiveSession->spawnConfirmationAcked)) {
    // Do not seed a speculative position before the protocol has accepted the
    // spawn transition. The next eligible tick will start from server facts.
    gLiveSession->error = "gameplay menunggu PlayerInit, spawn, dan ACK server";
    return snapshotFrom(gLiveSession.get());
  }
  const state::LocalPlayer& authoritativeLocal = gLiveSession->world.localPlayer();
  const state::WorldEnvironment& environment = gLiveSession->world.environment();
  const bool manualVehicleEngineAndLights = gLiveSession->world.initialized() &&
      gLiveSession->world.playerInit().manualVehicleEngineAndLights;
  if (authoritativeLocal.spectatingKnown && authoritativeLocal.spectating) {
    // RPC 124 is authority to suspend every locally-originated movement/aim/
    // passenger candidate. The renderer may follow copied 126/127 targets,
    // but no target state ever becomes a local player state or outgoing sync.
    gLiveSession->controls.reset();
    invalidateLocalGameplay(*gLiveSession);
    gLiveSession->onFootPreviewReady = false;
    gLiveSession->inCarPreviewReady = false;
    gLiveSession->aimPreviewReady = false;
    gLiveSession->passengerPreviewReady = false;
    gLiveSession->error = "spectator server aktif; gameplay sync lokal dihentikan";
    return snapshotFrom(gLiveSession.get());
  }
  if (serverMarkedDead(authoritativeLocal)) {
    // Do not keep a held key, prediction, or an old Packet-207/200/203/211
    // preview alive after authoritative health reaches zero. The user may
    // explicitly request a new server-approved spawn; this tick never does it
    // automatically or fabricates a client death report.
    gLiveSession->controls.reset();
    invalidateLocalGameplay(*gLiveSession);
    gLiveSession->error = "health server nol; gameplay menunggu respawn server";
    return snapshotFrom(gLiveSession.get());
  }
  if (authoritativeLocal.vehicleKnown && authoritativeLocal.inVehicle) {
    gLiveSession->onFootPreviewReady = false;
    gLiveSession->aimPreviewReady = false;
    const state::Vehicle* vehicle = gLiveSession->world.findVehicle(authoritativeLocal.vehicleId);
    if (vehicle == nullptr) {
      gLiveSession->inCarPreviewReady = false;
      gLiveSession->error = "gameplay kendaraan menunggu StreamInVehicle server";
      return snapshotFrom(gLiveSession.get());
    }
    if (authoritativeLocal.vehicleSeat != 0) {
      // Passenger Packet-211 is input/vitals state only. It intentionally does
      // not require exterior collision or predict an offset from the vehicle.
      const gameplay::LocalGameplayStep passengerStep = gameplay::buildLocalPassengerSync(
          authoritativeLocal, *vehicle, gLiveSession->controls, gLiveSession->localGameplay);
      if (!passengerStep.passengerReady) {
        gLiveSession->passengerPreviewReady = false;
        gLiveSession->inCarPreviewReady = false;
        switch (passengerStep.status) {
          case gameplay::LocalGameplayStatus::PassengerSeat:
            gLiveSession->error = "PlayerPassengerSync menunggu seat passenger server";
            break;
          case gameplay::LocalGameplayStatus::VehicleUnavailable:
            gLiveSession->error = "PlayerPassengerSync menunggu kendaraan server yang masih streamed";
            break;
          case gameplay::LocalGameplayStatus::FrozenByServer:
            gLiveSession->error = "Passenger dihentikan TogglePlayerControllable server";
            break;
          case gameplay::LocalGameplayStatus::Dead:
            gLiveSession->error = "Passenger dihentikan karena health server nol";
            break;
          case gameplay::LocalGameplayStatus::InvalidSyncState:
            gLiveSession->error = "sync passenger lokal tidak valid";
            break;
          case gameplay::LocalGameplayStatus::Applied:
          case gameplay::LocalGameplayStatus::AwaitingAuthoritativePosition:
          case gameplay::LocalGameplayStatus::AwaitingHeading:
          case gameplay::LocalGameplayStatus::Spectating:
          case gameplay::LocalGameplayStatus::CollisionUnavailable:
          case gameplay::LocalGameplayStatus::GravityUnsupported:
          case gameplay::LocalGameplayStatus::ServerVelocityActive:
          case gameplay::LocalGameplayStatus::MissingGround:
          case gameplay::LocalGameplayStatus::VehicleBlocked:
          case gameplay::LocalGameplayStatus::AimInactive:
          case gameplay::LocalGameplayStatus::WeaponUnavailable:
          case gameplay::LocalGameplayStatus::InvalidInput:
          case gameplay::LocalGameplayStatus::InvalidState:
            gLiveSession->error = "gameplay passenger tidak menghasilkan sync lokal";
            break;
        }
        return snapshotFrom(gLiveSession.get());
      }
      gLiveSession->passengerPreview = passengerStep.passenger;
      gLiveSession->passengerPreviewReady = true;
      gLiveSession->inCarPreviewReady = false;
      // A passenger never runs vehicle physics or local seat prediction. The
      // server remains authority for transform/occupancy while this USeq frame
      // only keeps valid input/vitals flowing at InCarRate.
      gLiveSession->error.clear();
      (void)sendPassengerLocked(*gLiveSession, passengerStep.passenger);
      return snapshotFrom(gLiveSession.get());
    }
    gLiveSession->passengerPreviewReady = false;
    // JNI obtains a tuning lookup before entering this session mutex. Bind it
    // again to the authoritative current model so a concurrent stream/seat
    // change can only fall back to generic motion, never apply another car's
    // user-data coefficients for even one frame.
    const vehiclemotion::VehicleMotionTuning* applicableVehicleTuning =
        vehicleTuning != nullptr && vehicle->model != 0u && vehicle->model == vehicleTuningModel
            ? vehicleTuning
            : nullptr;
    const gameplay::LocalGameplayStep step = gameplay::advanceLocalDriverVehicle(
        authoritativeLocal, *vehicle, gLiveSession->controls, collisionWorld, deltaSeconds,
        gLiveSession->localGameplay, &environment, &gLiveSession->world.buildingRemovals(),
        applicableVehicleTuning, manualVehicleEngineAndLights);
    if (!step.inCarReady) {
      gLiveSession->inCarPreviewReady = false;
      switch (step.status) {
        case gameplay::LocalGameplayStatus::PassengerSeat:
          gLiveSession->error = "kontrol kendaraan hanya untuk seat driver server";
          break;
        case gameplay::LocalGameplayStatus::VehicleUnavailable:
          gLiveSession->error = "kendaraan server belum tersedia untuk gameplay native";
          break;
        case gameplay::LocalGameplayStatus::FrozenByServer:
          gLiveSession->error = "kendaraan dihentikan TogglePlayerControllable server";
          break;
        case gameplay::LocalGameplayStatus::Spectating:
          gLiveSession->error = "kendaraan dihentikan saat spectator server aktif";
          break;
        case gameplay::LocalGameplayStatus::Dead:
          gLiveSession->error = "kendaraan dihentikan karena health server nol";
          break;
        case gameplay::LocalGameplayStatus::CollisionUnavailable:
          gLiveSession->error = "collision GTA kendaraan belum tersedia untuk area/interior ini";
          break;
        case gameplay::LocalGameplayStatus::GravityUnsupported:
          gLiveSession->error = "gravity server di luar envelope pilot on-foot; kendaraan tetap menunggu model physics";
          break;
        case gameplay::LocalGameplayStatus::ServerVelocityActive:
          gLiveSession->error = "kendaraan menunggu physics SetPlayerVelocity server";
          break;
        case gameplay::LocalGameplayStatus::MissingGround:
          gLiveSession->error = "collision ground kendaraan belum tersedia untuk posisi ini";
          break;
        case gameplay::LocalGameplayStatus::InvalidInput:
          gLiveSession->error = "input kendaraan native tidak valid";
          break;
        case gameplay::LocalGameplayStatus::InvalidState:
          gLiveSession->error = "state kendaraan lokal tidak valid";
          break;
        case gameplay::LocalGameplayStatus::InvalidSyncState:
          gLiveSession->error = "sync kendaraan lokal tidak valid";
          break;
        case gameplay::LocalGameplayStatus::VehicleBlocked:
        case gameplay::LocalGameplayStatus::AimInactive:
        case gameplay::LocalGameplayStatus::WeaponUnavailable:
        case gameplay::LocalGameplayStatus::AwaitingAuthoritativePosition:
        case gameplay::LocalGameplayStatus::AwaitingHeading:
        case gameplay::LocalGameplayStatus::Applied:
          gLiveSession->error = "gameplay kendaraan tidak menghasilkan sync lokal";
          break;
      }
      return snapshotFrom(gLiveSession.get());
    }
    gLiveSession->inCarPreview = step.inCar;
    gLiveSession->inCarPreviewReady = true;
    // A collision-blocked vehicle intentionally sends its safe stopped state;
    // the server can correct the transform without a fabricated bypass.
    gLiveSession->error.clear();
    (void)sendInCarLocked(*gLiveSession, step.inCar);
    return snapshotFrom(gLiveSession.get());
  }
  gLiveSession->passengerPreviewReady = false;
  // LocalGameplayBridge passes the authoritative interior into CollisionWorld.
  // A missing matching floor remains fail-closed as MissingGround; do not
  // prematurely substitute exterior collision just because the ID is nonzero.
  // PlayerInit/RPC-146 gravity is likewise required for vertical prediction;
  // unknown gravity becomes an unsupported zero sentinel and stops safely.
  const float serverGravity = environment.gravityKnown ? environment.gravity : 0.0f;
  const gameplay::LocalGameplayStep step = gameplay::advanceLocalOnFoot(
      authoritativeLocal, gLiveSession->controls, collisionWorld, deltaSeconds,
      gLiveSession->localGameplay, serverGravity, &environment,
      &gLiveSession->world.buildingRemovals());
  if (!step.onFootReady) {
    gLiveSession->aimPreviewReady = false;
    switch (step.status) {
      case gameplay::LocalGameplayStatus::AwaitingAuthoritativePosition:
        gLiveSession->error = "gameplay menunggu posisi authoritative server";
        break;
      case gameplay::LocalGameplayStatus::AwaitingHeading:
        gLiveSession->error = "gameplay menunggu arah local/server";
        break;
      case gameplay::LocalGameplayStatus::FrozenByServer:
        gLiveSession->error = "gameplay dihentikan TogglePlayerControllable server";
        break;
      case gameplay::LocalGameplayStatus::Spectating:
        gLiveSession->error = "gameplay dihentikan saat spectator server aktif";
        break;
      case gameplay::LocalGameplayStatus::Dead:
        gLiveSession->error = "gameplay dihentikan karena health server nol";
        break;
      case gameplay::LocalGameplayStatus::CollisionUnavailable:
        gLiveSession->error = "collision GTA belum tersedia; sinkronkan COL/IDE/IPL dari Data GTA";
        break;
      case gameplay::LocalGameplayStatus::GravityUnsupported:
        gLiveSession->error = "gravity server tidak didukung pilot collision; OnFoot dihentikan aman";
        break;
      case gameplay::LocalGameplayStatus::ServerVelocityActive:
        gLiveSession->error = "gameplay menunggu physics server velocity";
        break;
      case gameplay::LocalGameplayStatus::MissingGround:
        gLiveSession->error = "collision ground belum tersedia untuk posisi ini";
        break;
      case gameplay::LocalGameplayStatus::InvalidInput:
        gLiveSession->error = "input gameplay native tidak valid";
        break;
      case gameplay::LocalGameplayStatus::InvalidState:
        gLiveSession->error = "state gameplay lokal tidak valid";
        break;
      case gameplay::LocalGameplayStatus::InvalidSyncState:
        gLiveSession->error = "sync gameplay lokal tidak valid";
        break;
      // These statuses belong to the driver branch above. They are retained
      // here to make enum evolution fail closed rather than silently omit an
      // on-foot diagnostic.
      case gameplay::LocalGameplayStatus::VehicleUnavailable:
      case gameplay::LocalGameplayStatus::PassengerSeat:
      case gameplay::LocalGameplayStatus::VehicleBlocked:
      case gameplay::LocalGameplayStatus::AimInactive:
      case gameplay::LocalGameplayStatus::WeaponUnavailable:
        gLiveSession->error = "mode kendaraan tidak tersedia untuk gameplay on-foot";
        break;
      case gameplay::LocalGameplayStatus::Applied:
        gLiveSession->error = "gameplay tidak menghasilkan sync lokal";
        break;
    }
    return snapshotFrom(gLiveSession.get());
  }

  gLiveSession->onFootPreview = step.onFoot;
  gLiveSession->onFootPreviewReady = true;
  gLiveSession->inCarPreviewReady = false;
  // Successful simulation is not an error even when the transport's regular
  // on-foot cadence decides this particular frame is too soon to send.
  gLiveSession->error.clear();
  (void)sendOnFootLocked(*gLiveSession, step.onFoot);

  // Packet 203 remains strictly opt-in to held AIM. A simultaneous FIRE may
  // now produce only a conservative Packet-206 HitType=None form from this
  // same collision-grounded camera pose: no Android entity target, damage,
  // reward, or ammo mutation is invented. A separate close static-COL probe
  // may provide an exact world point while retaining HitType=None.
  const gameplay::LocalGameplayStep aimStep = gameplay::buildLocalAimSync(
      authoritativeLocal, gLiveSession->controls, gLiveSession->localGameplay);
  gLiveSession->aimPreviewReady = false;
  if (aimStep.aimReady) {
    gLiveSession->aimPreview = aimStep.aim;
    // A rate-limited candidate remains ready for the next cadence slot; a
    // sender rejection (notably server camera authority) is not shown as a
    // misleading playable/ready AimSync preview.
    const AimSendOutcome aimOutcome = sendAimLocked(*gLiveSession, aimStep.aim);
    gLiveSession->aimPreviewReady = aimOutcome != AimSendOutcome::Rejected;
    if (aimOutcome != AimSendOutcome::Rejected) {
      sync::BulletState noTargetBullet;
      if (gameplay::buildLocalNoTargetBulletSync(
              authoritativeLocal, gLiveSession->controls, gLiveSession->localGameplay,
              aimStep.aim, noTargetBullet)) {
        // A bounded user-owned COL probe may supply only a close static-world
        // absolute point for HitType=None. It never picks an entity/target;
        // failure or incomplete collision keeps the conservative zero sentinel.
        (void)gameplay::applyLocalNoTargetStaticWorldImpact(
            collisionWorld, gLiveSession->localGameplay, aimStep.aim, noTargetBullet,
            &gLiveSession->world.buildingRemovals());
        (void)sendNoTargetBulletLocked(*gLiveSession, noTargetBullet);
      }
    }
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

uint32_t raknet_session_local_driver_vehicle_model() {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->accepted || !gLiveSession->net ||
      gLiveSession->net->fd < 0) {
    return 0u;
  }
  const state::LocalPlayer& local = gLiveSession->world.localPlayer();
  if (!local.vehicleKnown || !local.inVehicle || local.vehicleSeat != 0u) return 0u;
  const state::Vehicle* vehicle = gLiveSession->world.findVehicle(local.vehicleId);
  return vehicle != nullptr ? vehicle->model : 0u;
}

std::vector<gta::removal::Directive> raknet_session_building_removals() {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  if (!gLiveSession || !gLiveSession->accepted || !gLiveSession->net ||
      gLiveSession->net->fd < 0) {
    return {};
  }
  return gLiveSession->world.buildingRemovals();
}

WorldSnapshot raknet_session_world_snapshot() {
  std::lock_guard<std::mutex> lock(gLiveSessionMutex);
  return worldSnapshotFrom(gLiveSession.get());
}

}  // namespace samp
