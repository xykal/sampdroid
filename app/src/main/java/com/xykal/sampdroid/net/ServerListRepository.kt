package com.xykal.sampdroid.net

import com.xykal.sampdroid.model.ServerEntry
import org.json.JSONArray
import java.net.HttpURLConnection
import java.net.URL

/**
 * Sumber daftar server: open.mp public server index.
 * Masterlist SA-MP resmi sudah mati bersama sa-mp.com.
 */
object ServerListRepository {

    private const val API = "https://api.open.mp/servers"

    fun fetch(): List<ServerEntry> {
        val conn = (URL(API).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15000
            readTimeout = 20000
            setRequestProperty("User-Agent", "sampdroid/0.1")
        }
        try {
            if (conn.responseCode !in 200..299) {
                throw IllegalStateException("HTTP ${conn.responseCode}")
            }
            val body = conn.inputStream.bufferedReader().use { it.readText() }
            return parse(JSONArray(body))
        } finally {
            conn.disconnect()
        }
    }

    private fun parse(arr: JSONArray): List<ServerEntry> {
        val out = ArrayList<ServerEntry>(arr.length())
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val addr = o.optString("ip")
            val parts = addr.split(":")
            if (parts.size != 2) continue
            val port = parts[1].toIntOrNull() ?: continue
            out.add(
                ServerEntry(
                    ip = parts[0],
                    port = port,
                    hostname = o.optString("hn", "(tanpa nama)"),
                    players = o.optInt("pc", 0),
                    maxPlayers = o.optInt("pm", 0),
                    gamemode = o.optString("gm", ""),
                    language = o.optString("la", ""),
                    passworded = o.optBoolean("pa", false),
                    isOpenMp = o.optBoolean("omp", false),
                    version = o.optString("vn", "")
                )
            )
        }
        return out.sortedByDescending { it.players }
    }
}
