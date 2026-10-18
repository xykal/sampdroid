// JNI bridge: mengekspos protokol SA-MP ke Kotlin.
#include <jni.h>

#include <cstdint>
#include <sstream>
#include <string>

#include "raknet_probe.h"
#include "raknet_session.h"
#include "samp_query.h"

namespace {

std::string to_std(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return out;
}

jstring to_java(JNIEnv* env, const std::string& s) {
  return env->NewStringUTF(s.c_str());
}

std::string session_json(const samp::SessionSnapshot& s) {
  std::ostringstream o;
  o << "{\"active\":" << (s.active ? "true" : "false")
    << ",\"accepted\":" << (s.accepted ? "true" : "false")
    << ",\"joined\":" << (s.joined ? "true" : "false")
    << ",\"playerInit\":" << (s.playerInit ? "true" : "false")
    << ",\"classSelectionKnown\":" << (s.classSelectionKnown ? "true" : "false")
    << ",\"classSelectable\":" << (s.classSelectable ? "true" : "false")
    << ",\"spawnResponseKnown\":" << (s.spawnResponseKnown ? "true" : "false")
    << ",\"spawnAllowed\":" << (s.spawnAllowed ? "true" : "false")
    << ",\"spawnConfirmationSent\":" << (s.spawnConfirmationSent ? "true" : "false")
    << ",\"spawnConfirmationAcked\":" << (s.spawnConfirmationAcked ? "true" : "false")
    << ",\"localSpawnInfoKnown\":" << (s.localSpawnInfoKnown ? "true" : "false")
    << ",\"localPositionKnown\":" << (s.localPositionKnown ? "true" : "false")
    << ",\"nativeControlsKnown\":" << (s.nativeControlsKnown ? "true" : "false")
    << ",\"onFootPreviewReady\":" << (s.onFootPreviewReady ? "true" : "false")
    << ",\"checkpointActive\":" << (s.checkpointActive ? "true" : "false")
    << ",\"raceCheckpointActive\":" << (s.raceCheckpointActive ? "true" : "false")
    << ",\"playerId\":" << s.playerId
    << ",\"classModel\":" << s.classModel
    << ",\"localModel\":" << s.localModel
    << ",\"localX\":" << s.localX
    << ",\"localY\":" << s.localY
    << ",\"localZ\":" << s.localZ
    << ",\"localAngle\":" << s.localAngle
    << ",\"controlMoveX\":" << s.controlMoveX
    << ",\"controlMoveY\":" << s.controlMoveY
    << ",\"controlHeading\":" << s.controlHeading
    << ",\"controlLeftRight\":" << s.controlLeftRight
    << ",\"controlUpDown\":" << s.controlUpDown
    << ",\"controlKeys\":" << s.controlKeys
    << ",\"onFootSyncRateMs\":" << s.onFootSyncRateMs
    << ",\"effectiveOnFootSyncRateMs\":" << s.effectiveOnFootSyncRateMs
    << ",\"receivedPackets\":" << s.receivedPackets
    << ",\"receivedRpcs\":" << s.receivedRpcs
    << ",\"splitFragments\":" << s.splitFragments
    << ",\"reassembledPackets\":" << s.reassembledPackets
    << ",\"trackedPlayers\":" << s.trackedPlayers
    << ",\"chatMessages\":" << s.chatMessages
    << ",\"reliableKeepalives\":" << s.reliableKeepalives
    << ",\"connectedPongs\":" << s.connectedPongs
    << ",\"outboundOnFootPackets\":" << s.outboundOnFootPackets
    << ",\"localOnFootStateKnown\":" << (s.localOnFootStateKnown ? "true" : "false")
    << ",\"serverName\":\"" << samp::json_escape(s.serverName) << "\""
    << ",\"rpcSummary\":\"" << samp::json_escape(s.rpcSummary) << "\""
    << ",\"stateSummary\":\"" << samp::json_escape(s.stateSummary) << "\""
    << ",\"error\":\"" << samp::json_escape(s.error) << "\"}";
  return o.str();
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeVersion(JNIEnv* env, jclass) {
  return to_java(env, "0.14.0 / native-touch-preview");
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeQueryInfo(JNIEnv* env, jclass, jstring ip,
                                                      jint port, jint timeoutMs) {
  auto host = to_std(env, ip);
  auto info = samp::query_info(host, static_cast<uint16_t>(port), timeoutMs);
  std::ostringstream o;
  o << "{";
  o << "\"ok\":" << (info.ok ? "true" : "false") << ",";
  o << "\"error\":\"" << samp::json_escape(info.error) << "\",";
  o << "\"password\":" << (info.password ? "true" : "false") << ",";
  o << "\"players\":" << info.players << ",";
  o << "\"maxPlayers\":" << info.maxPlayers << ",";
  o << "\"hostname\":\"" << samp::json_escape(info.hostname) << "\",";
  o << "\"gamemode\":\"" << samp::json_escape(info.gamemode) << "\",";
  o << "\"language\":\"" << samp::json_escape(info.language) << "\"";
  o << "}";
  return to_java(env, o.str());
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeQueryRules(JNIEnv* env, jclass, jstring ip,
                                                       jint port, jint timeoutMs) {
  auto rules = samp::query_rules(to_std(env, ip), static_cast<uint16_t>(port), timeoutMs);
  std::ostringstream o;
  o << "[";
  for (size_t i = 0; i < rules.size(); ++i) {
    if (i) o << ",";
    o << "{\"name\":\"" << samp::json_escape(rules[i].name) << "\",\"value\":\""
      << samp::json_escape(rules[i].value) << "\"}";
  }
  o << "]";
  return to_java(env, o.str());
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeQueryPlayers(JNIEnv* env, jclass, jstring ip,
                                                         jint port, jboolean detailed,
                                                         jint timeoutMs) {
  auto players = samp::query_players(to_std(env, ip), static_cast<uint16_t>(port),
                                     detailed == JNI_TRUE, timeoutMs);
  std::ostringstream o;
  o << "[";
  for (size_t i = 0; i < players.size(); ++i) {
    if (i) o << ",";
    o << "{\"id\":" << players[i].id << ",\"name\":\"" << samp::json_escape(players[i].name)
      << "\",\"score\":" << players[i].score << ",\"ping\":" << players[i].ping << "}";
  }
  o << "]";
  return to_java(env, o.str());
}

JNIEXPORT jint JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeQueryPing(JNIEnv* env, jclass, jstring ip,
                                                      jint port, jint timeoutMs) {
  return samp::query_ping(to_std(env, ip), static_cast<uint16_t>(port), timeoutMs);
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeRaknetProbe(JNIEnv* env, jclass, jstring ip,
                                                        jint port, jint timeoutMs) {
  auto r = samp::raknet_probe(to_std(env, ip), static_cast<uint16_t>(port), timeoutMs);
  std::ostringstream o;
  o << "{\"openConnection\":" << (r.openConnection ? "true" : "false")
    << ",\"cookieOk\":" << (r.cookieOk ? "true" : "false")
    << ",\"authenticated\":" << (r.authenticated ? "true" : "false")
    << ",\"joined\":" << (r.joined ? "true" : "false")
    << ",\"accepted\":" << (r.accepted ? "true" : "false")
    << ",\"playerInit\":" << (r.playerInit ? "true" : "false")
    << ",\"playerId\":" << r.playerId
    << ",\"receivedPackets\":" << r.receivedPackets
    << ",\"receivedRpcs\":" << r.receivedRpcs
    << ",\"splitFragments\":" << r.splitFragments
    << ",\"reassembledPackets\":" << r.reassembledPackets
    << ",\"trackedPlayers\":" << r.trackedPlayers
    << ",\"serverName\":\"" << samp::json_escape(r.serverName) << "\""
    << ",\"rpcSummary\":\"" << samp::json_escape(r.rpcSummary) << "\""
    << ",\"stateSummary\":\"" << samp::json_escape(r.stateSummary) << "\""
    << ",\"variant\":\"" << samp::json_escape(r.variant) << "\""
    << ",\"error\":\"" << samp::json_escape(r.error) << "\""
    << ",\"log\":\"" << samp::json_escape(r.log) << "\"}";
  return to_java(env, o.str());
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionConnect(JNIEnv* env, jclass, jstring ip,
                                                           jint port, jint timeoutMs) {
  return to_java(env, session_json(samp::raknet_session_connect(
                          to_std(env, ip), static_cast<uint16_t>(port), timeoutMs)));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionPoll(JNIEnv* env, jclass, jint timeoutMs) {
  return to_java(env, session_json(samp::raknet_session_poll(timeoutMs)));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionRequestClass(JNIEnv* env, jclass,
                                                                 jint classId) {
  if (classId < 0 || classId > 65535) {
    samp::SessionSnapshot invalid;
    invalid.error = "class ID di luar rentang u16";
    return to_java(env, session_json(invalid));
  }
  return to_java(env, session_json(
                          samp::raknet_session_request_class(static_cast<uint16_t>(classId))));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionRequestSpawn(JNIEnv* env, jclass) {
  return to_java(env, session_json(samp::raknet_session_request_spawn()));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionUpdateControls(JNIEnv* env, jclass,
                                                                  jfloat moveX, jfloat moveY,
                                                                  jfloat lookDeltaDegrees,
                                                                  jint heldActions) {
  if (heldActions < 0 || heldActions > 0xFFFF) {
    samp::SessionSnapshot invalid;
    invalid.error = "mask input native di luar rentang u16";
    return to_java(env, session_json(invalid));
  }
  samp::input::TouchIntent input;
  input.moveX = moveX;
  input.moveY = moveY;
  input.lookDeltaDegrees = lookDeltaDegrees;
  input.heldActions = static_cast<uint16_t>(heldActions);
  return to_java(env, session_json(samp::raknet_session_update_controls(input)));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionDisconnect(JNIEnv* env, jclass) {
  return to_java(env, session_json(samp::raknet_session_disconnect()));
}

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeSessionStatus(JNIEnv* env, jclass) {
  return to_java(env, session_json(samp::raknet_session_status()));
}

}  // extern "C"
