package com.xykal.sampdroid.ui

import android.content.Context
import java.security.SecureRandom

/**
 * Small, local-only legacy identity store. A server sees this nickname in its
 * normal ClientJoin flow; it is not an account password or an attempt to mimic
 * the proprietary PC client. The generated default avoids every fresh install
 * colliding on one bundled nickname.
 */
object PlayerProfile {
    private const val PREFS = "player_profile"
    private const val KEY_NICKNAME = "nickname"
    private const val MIN_LENGTH = 3
    private const val MAX_LENGTH = 24
    private val random = SecureRandom()

    fun nickname(context: Context): String {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val saved = prefs.getString(KEY_NICKNAME, null)?.trim().orEmpty()
        if (isValidNickname(saved)) return saved

        val generated = "Samp_${randomSuffix()}"
        prefs.edit().putString(KEY_NICKNAME, generated).apply()
        return generated
    }

    fun saveNickname(context: Context, value: String): Boolean {
        val normalized = value.trim()
        if (!isValidNickname(normalized)) return false
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_NICKNAME, normalized)
            .apply()
        return true
    }

    fun isValidNickname(value: String): Boolean {
        if (value.length !in MIN_LENGTH..MAX_LENGTH) return false
        return value.all { char -> char.code in 0x21..0x7e }
    }

    private fun randomSuffix(): String {
        val alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
        return buildString(6) {
            repeat(6) { append(alphabet[random.nextInt(alphabet.length)]) }
        }
    }
}
