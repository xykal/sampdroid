package com.xykal.sampdroid.ui

import android.content.Context

/**
 * Tiny local-only primary-server shortcut. It stores an endpoint the player
 * explicitly selected; it never probes, connects, or uploads anything by
 * itself. UIF is only the initial convenience candidate, not a claim that the
 * endpoint is currently reachable or compatible.
 */
internal data class FavoriteServer(
    val host: String,
    val port: Int,
    val label: String
)

internal object ServerFavorites {
    private const val PREFS = "server_favorites"
    private const val KEY_HOST = "primary_host"
    private const val KEY_PORT = "primary_port"
    private const val KEY_LABEL = "primary_label"

    private val defaultPrimary = FavoriteServer(
        host = "play.uifserver.net",
        port = 7776,
        label = "UIF"
    )

    fun primary(context: Context): FavoriteServer {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val host = prefs.getString(KEY_HOST, null)?.trim().orEmpty()
        val port = prefs.getInt(KEY_PORT, -1)
        val label = prefs.getString(KEY_LABEL, null)?.trim().orEmpty()
        return if (isValidHost(host) && port in 1..65_535 && isValidLabel(label)) {
            FavoriteServer(host, port, label)
        } else {
            defaultPrimary
        }
    }

    fun savePrimary(context: Context, host: String, port: Int, label: String): Boolean {
        val normalizedHost = host.trim().lowercase()
        val normalizedLabel = label.trim().ifBlank { normalizedHost }
        if (!isValidHost(normalizedHost) || port !in 1..65_535 || !isValidLabel(normalizedLabel)) {
            return false
        }
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString(KEY_HOST, normalizedHost)
            .putInt(KEY_PORT, port)
            .putString(KEY_LABEL, normalizedLabel)
            .apply()
        return true
    }

    private fun isValidHost(value: String): Boolean {
        if (value.isBlank() || value.length > 253 || value.startsWith('.') || value.endsWith('.')) return false
        // The legacy native transport is IPv4-only, and the resolver performs
        // that final capability check. Here we only persist a conservative
        // hostname/IPv4 spelling without URLs, paths, credentials, or IPv6.
        return value.all { char ->
            char in 'a'..'z' || char in 'A'..'Z' || char in '0'..'9' || char == '.' || char == '-'
        }
    }

    private fun isValidLabel(value: String): Boolean =
        value.length in 1..48 && value.all { char -> char.code in 0x20..0x7e }
}
