package com.xykal.sampdroid.ui

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.MediaPlayer
import android.net.Uri
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import org.json.JSONObject
import java.util.Locale
import kotlin.math.sqrt

// RPC 41's source writer stores URL in HybridString<128>, i.e. at most 127
// data bytes before its terminal storage byte. HTTPS URLs are ASCII here.
private const val MAX_AUDIO_STREAM_URL_CHARS = 127
private const val MAX_AUDIO_STREAM_DISTANCE = 50_000f

/**
 * Validates the presentation hand-off again at the Android network boundary.
 *
 * A SA-MP/open.mp server owns the RPC directive, but it does not get arbitrary
 * cleartext/media access on a device: the app manifest blocks cleartext and
 * this accepts only normal HTTPS URLs with no embedded credentials or obvious
 * local literal host. DNS resolution remains platform-owned, so users should
 * still treat a multiplayer server as a trusted content source.
 */
private fun approvedHttpsAudioUrl(raw: String): String? {
    if (raw.length !in 1..MAX_AUDIO_STREAM_URL_CHARS ||
        raw.any { it.code !in 0x21..0x7e }
    ) {
        return null
    }
    val uri = runCatching { Uri.parse(raw) }.getOrNull() ?: return null
    if (!uri.isAbsolute || !uri.scheme.equals("https", ignoreCase = true) ||
        uri.host.isNullOrBlank() || uri.userInfo != null || uri.fragment != null ||
        uri.port !in -1..65535
    ) {
        return null
    }
    val host = uri.host!!.lowercase(Locale.ROOT).trimEnd('.')
    if (host == "localhost" || isBlockedPrivateLiteralHost(host)) return null
    return uri.toString()
}

/** Blocks common literal loopback/link-local/private endpoints without DNS work on the UI thread. */
private fun isBlockedPrivateLiteralHost(rawHost: String): Boolean {
    val host = rawHost.removePrefix("[").removeSuffix("]")
    if (host.contains(':')) {
        // URI host normalizes many IPv6 spellings, but reject the obvious local,
        // link-local, and ULA ranges without synchronously resolving DNS.
        return host == "::1" || host.startsWith("0:0:0:0:0:0:0:") ||
            host.startsWith("fe80:") || host.startsWith("fc") || host.startsWith("fd")
    }
    return isBlockedPrivateIpv4Literal(host)
}

private fun isBlockedPrivateIpv4Literal(host: String): Boolean {
    val parts = host.split('.')
    if (parts.size != 4) return false
    val octets = IntArray(4)
    for (index in parts.indices) {
        val value = parts[index].toIntOrNull() ?: return false
        if (value !in 0..255) return false
        octets[index] = value
    }
    return octets[0] == 0 || octets[0] == 10 || octets[0] == 127 ||
        (octets[0] == 100 && octets[1] in 64..127) ||
        (octets[0] == 169 && octets[1] == 254) ||
        (octets[0] == 172 && octets[1] in 16..31) ||
        (octets[0] == 192 && octets[1] == 168)
}

/**
 * Parses the immutable native JSON hand-off. An unsafe/malformed directive is
 * represented as active-but-not-playable so a new revision still stops a prior
 * stream, while no URL is ever passed on to [MediaPlayer].
 */
internal fun parseWorldAudioStream(json: JSONObject?): WorldAudioStream {
    if (json == null) return WorldAudioStream()
    val active = json.optBoolean("active")
    val revision = json.optLong("revision", 0L).takeIf { it > 0L } ?: 0L
    val positional = json.optBoolean("positional")
    val x = json.optDouble("x").toFloat()
    val y = json.optDouble("y").toFloat()
    val z = json.optDouble("z").toFloat()
    val distance = json.optDouble("distance").toFloat()
    val url = approvedHttpsAudioUrl(json.optString("url"))
    val validPosition = !positional ||
        (x.isFinite() && y.isFinite() && z.isFinite() &&
            x > -20_000f && x < 20_000f && y > -20_000f && y < 20_000f &&
            z > -1_000f && z < 200_000f &&
            distance.isFinite() && distance > 0f && distance <= MAX_AUDIO_STREAM_DISTANCE)
    val validNonPositionalDistance = positional ||
        (distance.isFinite() && distance >= 0f && distance <= MAX_AUDIO_STREAM_DISTANCE)
    val validDirective = active && revision > 0L && url != null && validPosition && validNonPositionalDistance
    return WorldAudioStream(
        active = active && revision > 0L,
        revision = revision,
        playable = validDirective,
        url = url.orEmpty(),
        positional = positional,
        x = x,
        y = y,
        z = z,
        distance = distance
    )
}

/**
 * Lifecycle-bound HTTPS playback for RPC 41/42. It owns no socket, world
 * mutation, collision, or sync state. A media error/completion is local device
 * feedback only and never sends a server response or claims a stream was heard.
 */
internal class NativeAudioStreamController(
    private val activity: AppCompatActivity
) : DefaultLifecycleObserver {
    private val audioManager = activity.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
    private var player: MediaPlayer? = null
    private var appliedRevision = Long.MIN_VALUE
    private var failedRevision = Long.MIN_VALUE
    private var foreground = true
    private var desired = WorldAudioStream()
    private var lastFrame = WorldFrame()
    private var pausedForFocus = false
    private var duckFactor = 1f

    private val focusChangeListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_LOSS -> {
                // A permanent loss belongs to another foreground audio owner;
                // release rather than keeping a paused stream alive invisibly.
                pausedForFocus = false
                releasePlayerOnly()
                abandonAudioFocus()
                failedRevision = desired.revision
            }
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> {
                pausedForFocus = runCatching { player?.isPlaying == true }.getOrDefault(false)
                runCatching { player?.pause() }
            }
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                duckFactor = 0.2f
                applyVolume()
            }
            AudioManager.AUDIOFOCUS_GAIN -> {
                duckFactor = 1f
                applyVolume()
                if (pausedForFocus && foreground && desired.playable) {
                    runCatching { player?.start() }
                }
                pausedForFocus = false
            }
        }
    }

    init {
        activity.lifecycle.addObserver(this)
    }

    /** Called on the main/UI collector that owns the immutable world frame. */
    fun submit(frame: WorldFrame) {
        lastFrame = frame
        val stream = frame.audioStream
        val shouldPlay = foreground && stream.active && stream.playable && stream.revision > 0L
        if (!shouldPlay) {
            // An inactive/blocked new revision must silence the previous stream.
            if (player != null || appliedRevision != stream.revision) stopPlayback(keepRevision = true)
            appliedRevision = stream.revision
            if (failedRevision != stream.revision) failedRevision = Long.MIN_VALUE
            desired = stream
            return
        }
        desired = stream
        if (stream.revision != appliedRevision) {
            stopPlayback(keepRevision = true)
            appliedRevision = stream.revision
            failedRevision = Long.MIN_VALUE
        }
        if (player == null && failedRevision != stream.revision) startPlayback(stream)
        applyVolume()
    }

    override fun onStart(owner: LifecycleOwner) {
        foreground = true
        // The next retained snapshot starts a fresh player for the current
        // directive. No stream survives an app background transition.
        appliedRevision = Long.MIN_VALUE
        failedRevision = Long.MIN_VALUE
    }

    override fun onStop(owner: LifecycleOwner) {
        foreground = false
        stopPlayback(keepRevision = false)
    }

    /** Must be called when the world dialog closes so it cannot outlive its UI. */
    fun release() {
        foreground = false
        stopPlayback(keepRevision = false)
        activity.lifecycle.removeObserver(this)
    }

    private fun startPlayback(stream: WorldAudioStream) {
        if (!requestAudioFocus()) {
            failedRevision = stream.revision
            return
        }
        val created = MediaPlayer()
        try {
            created.setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            created.setOnPreparedListener { prepared ->
                if (player === prepared && foreground && desired.revision == stream.revision && desired.playable) {
                    applyVolume()
                    runCatching { prepared.start() }
                }
            }
            created.setOnCompletionListener { completed ->
                if (player === completed) {
                    releasePlayerOnly()
                    abandonAudioFocus()
                    failedRevision = stream.revision
                }
            }
            created.setOnErrorListener { failed, _, _ ->
                if (player === failed) {
                    releasePlayerOnly()
                    abandonAudioFocus()
                    failedRevision = stream.revision
                }
                true
            }
            created.setDataSource(stream.url)
            // Publish the player before prepareAsync: a fast cached prepare
            // callback must still identify the current directive correctly.
            player = created
            created.prepareAsync()
        } catch (_: Exception) {
            if (player === created) player = null
            runCatching { created.reset() }
            runCatching { created.release() }
            abandonAudioFocus()
            failedRevision = stream.revision
        }
    }

    /** A mobile attenuation proxy only; it does not change server audio state. */
    private fun applyVolume() {
        val current = player ?: return
        val base = if (!desired.positional) {
            1f
        } else if (!lastFrame.localKnown || !lastFrame.localX.isFinite() ||
            !lastFrame.localY.isFinite() || !lastFrame.localZ.isFinite() || desired.distance <= 0f
        ) {
            // Do not invent proximity before native has an authoritative local anchor.
            0f
        } else {
            val dx = lastFrame.localX - desired.x
            val dy = lastFrame.localY - desired.y
            val dz = lastFrame.localZ - desired.z
            val meters = sqrt(dx * dx + dy * dy + dz * dz)
            (1f - meters / desired.distance).coerceIn(0f, 1f)
        }
        val volume = (base * duckFactor).coerceIn(0f, 1f)
        runCatching { current.setVolume(volume, volume) }
    }

    private fun requestAudioFocus(): Boolean {
        return audioManager?.requestAudioFocus(
            focusChangeListener,
            AudioManager.STREAM_MUSIC,
            AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK
        ) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
    }

    private fun abandonAudioFocus() {
        audioManager?.abandonAudioFocus(focusChangeListener)
    }

    private fun stopPlayback(keepRevision: Boolean) {
        releasePlayerOnly()
        abandonAudioFocus()
        pausedForFocus = false
        duckFactor = 1f
        if (!keepRevision) {
            appliedRevision = Long.MIN_VALUE
            failedRevision = Long.MIN_VALUE
        }
    }

    private fun releasePlayerOnly() {
        val current = player ?: return
        player = null
        runCatching { current.setOnPreparedListener(null) }
        runCatching { current.setOnCompletionListener(null) }
        runCatching { current.setOnErrorListener(null) }
        runCatching { current.reset() }
        runCatching { current.release() }
    }
}
