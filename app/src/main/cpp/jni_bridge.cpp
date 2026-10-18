// JNI bridge: mengekspos protokol SA-MP ke Kotlin.
#include <jni.h>

#include <sstream>
#include <string>

#include "raknet_probe.h"
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

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_xykal_sampdroid_NativeBridge_nativeVersion(JNIEnv* env, jclass) {
  return to_java(env, "sampdroid-native 0.6.0 / native-port-track");
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

}  // extern "C"
