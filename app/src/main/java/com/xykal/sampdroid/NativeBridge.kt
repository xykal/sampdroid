package com.xykal.sampdroid

/**
 * Jembatan ke native library (C++):
 *  - query protocol SA-MP (i / r / c / d / p)
 *  - RakNet 2.x connect probe (handshake + obfuscation layer)
 */
object NativeBridge {

    init {
        System.loadLibrary("sampdroid")
    }

    external fun nativeVersion(): String

    /** @return JSON: {ok,error,password,players,maxPlayers,hostname,gamemode,language} */
    external fun nativeQueryInfo(ip: String, port: Int, timeoutMs: Int): String

    /** @return JSON array: [{name,value}] */
    external fun nativeQueryRules(ip: String, port: Int, timeoutMs: Int): String

    /** @return JSON array: [{id,name,score,ping}] */
    external fun nativeQueryPlayers(ip: String, port: Int, detailed: Boolean, timeoutMs: Int): String

    /** @return ping ms, atau -1 kalau gagal */
    external fun nativeQueryPing(ip: String, port: Int, timeoutMs: Int): Int

    /**
     * @return JSON status handshake + stream: connection/auth/join, fragment
     * reassembly, RPC summary, and PlayerInit data when the server sends it.
     */
    external fun nativeRaknetProbe(ip: String, port: Int, timeoutMs: Int): String
}
