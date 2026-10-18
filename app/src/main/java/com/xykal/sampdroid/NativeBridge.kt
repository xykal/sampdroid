package com.xykal.sampdroid

/**
 * Jembatan ke native library (C++):
 *  - query protocol SA-MP (i / r / c / d / p)
 *  - RakNet 2.x probe and explicit retained native session
 *    (handshake + obfuscation + bounded protocol state)
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

    /**
     * Explicit user-initiated persistent native session. Calls must run on
     * Dispatchers.IO; this owns a UDP socket until nativeSessionDisconnect().
     */
    external fun nativeSessionConnect(ip: String, port: Int, timeoutMs: Int): String

    /** Receives one datagram, ACKs reliable traffic, and returns reduced state JSON. */
    external fun nativeSessionPoll(timeoutMs: Int): String

    /** Sends PlayerRequestClass on reliable ordered channel 2. */
    external fun nativeSessionRequestClass(classId: Int): String

    /** Starts the explicit RequestSpawn → PlayerSpawn native handshake. */
    external fun nativeSessionRequestSpawn(): String

    /**
     * Updates bounded Android-native semantic touch/controller state and returns
     * an OnFoot preview. This does not transmit movement to the server.
     */
    external fun nativeSessionUpdateControls(
        moveX: Float,
        moveY: Float,
        lookDeltaDegrees: Float,
        heldActions: Int
    ): String

    /** Releases the retained UDP socket. Safe if no session is active. */
    external fun nativeSessionDisconnect(): String

    /** Returns native session state without doing network IO. */
    external fun nativeSessionStatus(): String
}
