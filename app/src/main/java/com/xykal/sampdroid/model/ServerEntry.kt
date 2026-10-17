package com.xykal.sampdroid.model

data class ServerEntry(
    val ip: String,
    val port: Int,
    val hostname: String,
    val players: Int,
    val maxPlayers: Int,
    val gamemode: String,
    val language: String,
    val passworded: Boolean,
    val isOpenMp: Boolean,
    val version: String
) {
    val address: String get() = "$ip:$port"
}
