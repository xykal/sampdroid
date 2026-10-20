package com.xykal.sampdroid.ui

import android.text.method.ScrollingMovementMethod
import android.view.LayoutInflater
import android.view.View
import android.widget.Button
import android.widget.TextView
import androidx.lifecycle.lifecycleScope
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.xykal.sampdroid.NativeBridge
import com.xykal.sampdroid.R
import com.xykal.sampdroid.model.ServerEntry
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.util.Locale

// Keep native socket ownership in this explicit session dialog, but use a
// short wait so movement/Aim sync is not serialized behind a 250 ms receive.
// The native side drains only a capped ready-datagram burst after this wait.
private const val SESSION_POLL_TIMEOUT_MS = 40
private const val SESSION_POLL_YIELD_MS = 4L
// A retained explicit session may keep its transport alive while the app is
// briefly hidden, but it must not burn the foreground gameplay cadence or
// present server-driven UI in background.
private const val BACKGROUND_SESSION_POLL_TIMEOUT_MS = 200
private const val BACKGROUND_SESSION_POLL_YIELD_MS = 100L
// Poll transport promptly, but avoid rebuilding a full JSON world/UI more often
// than the renderer's own immutable-frame cadence.
private const val SESSION_PRESENTATION_INTERVAL_NANOS = 100_000_000L

object ServerDetailDialog {

    fun show(activity: AppCompatActivity, server: ServerEntry) {
        val view = LayoutInflater.from(activity).inflate(R.layout.dialog_server, null)
        val header: TextView = view.findViewById(R.id.tvHeader)
        val body: TextView = view.findViewById(R.id.tvBody)
        val log: TextView = view.findViewById(R.id.tvLog)
        val connectionHint: TextView = view.findViewById(R.id.tvConnectionHint)
        val btnQuickConnect: Button = view.findViewById(R.id.btnQuickConnect)
        val btnOpenWorld: Button = view.findViewById(R.id.btnOpenWorld)
        val btnInfo: Button = view.findViewById(R.id.btnQueryInfo)
        val btnRules: Button = view.findViewById(R.id.btnRules)
        val btnPlayers: Button = view.findViewById(R.id.btnPlayers)
        val btnPing: Button = view.findViewById(R.id.btnPing)
        val btnProbe: Button = view.findViewById(R.id.btnProbe)
        val btnSession: Button = view.findViewById(R.id.btnSession)
        val btnClassZero: Button = view.findViewById(R.id.btnClassZero)
        val btnSpawn: Button = view.findViewById(R.id.btnSpawn)
        val btnControls: Button = view.findViewById(R.id.btnControls)
        val btnChat: Button = view.findViewById(R.id.btnChat)
        val btnWorld: Button = view.findViewById(R.id.btnWorld)
        val btnSessionDisconnect: Button = view.findViewById(R.id.btnSessionDisconnect)

        log.movementMethod = ScrollingMovementMethod()

        header.text = server.hostname
        body.text = buildString {
            append("Alamat   : ").append(server.address).append('\n')
            append("Players  : ").append(server.players).append(" / ").append(server.maxPlayers).append('\n')
            append("Gamemode : ").append(server.gamemode.ifBlank { "-" }).append('\n')
            append("Bahasa   : ").append(server.language.ifBlank { "-" }).append('\n')
            append("Versi    : ").append(server.version.ifBlank { "-" })
        }

        val dialog = MaterialAlertDialogBuilder(activity)
            .setView(view)
            .setNegativeButton("Tutup", null)
            .create()
        var sessionPollJob: Job? = null
        var sessionConnectJob: Job? = null
        var sessionDisconnectJob: Job? = null
        // Cursor is a UI hint only. Native rechecks PlayerInit's raw class
        // count and serializes one reliable class request at a time.
        var classCursor = 0
        // The player flow starts locked rather than making a first tap fail as
        // a developer-style error. Live snapshots below unlock each step only
        // after the server has supplied the required authority.
        btnOpenWorld.isEnabled = false
        btnWorld.isEnabled = false
        btnClassZero.isEnabled = false
        btnSpawn.isEnabled = false

        fun run(block: () -> String) {
            log.text = "…"
            activity.lifecycleScope.launch {
                val out = withContext(Dispatchers.IO) {
                    runCatching(block).getOrElse { "ERROR: ${it.message}" }
                }
                log.text = out
            }
        }

        fun formatSession(o: JSONObject): String = buildString {
            append("=== STATUS SESI NATIVE ===\n")
            append("Android native tanpa Winlator/samp.exe. Stream dipoll otomatis selama sesi ini terbuka.\n")
            append("Saat PlayerInit membawa class, native langsung meminta class awal agar handshake legacy tidak kedaluwarsa; " +
                "kamu tetap dapat memilih class lain lalu Spawn sesuai respons server. Dunia menyediakan HUD/kontrol native setelah gate spawn/ACK. " +
                "Hit/damage/ammo lokal dan gameplay GTA penuh belum diklaim.\n\n")
            append("aktif           : ").append(o.optBoolean("active")).append('\n')
            append("profil wire     : ").append(o.optString("clientProtocol").ifBlank { "belum dipilih" }).append('\n')
            if (o.optString("clientProtocol").startsWith("0.3.DL-R1")) {
                val objectFallbacks = o.optInt("dlObjectFallbacks").coerceAtLeast(0)
                append("DL R1          : ClientJoin + layout CustomSkin inti aktif. ")
                append("Catalog object pasif: ").append(objectFallbacks)
                    .append(" fallback custom→base stock; model/artwork custom tidak diunduh.\n")
            }
            append("accepted        : ").append(o.optBoolean("accepted")).append('\n')
            append("ClientJoin stream: ").append(o.optBoolean("joined")).append('\n')
            append("PlayerInit       : ").append(o.optBoolean("playerInit")).append('\n')
            if (o.optBoolean("playerInit")) {
                append("player ID       : ").append(o.optInt("playerId")).append('\n')
                append("nama server     : ").append(o.optString("serverName")).append('\n')
                append("class server    : ").append(o.optLong("spawnClassCount"))
                    .append(" slot\n")
                append("sync on-foot    : ").append(o.optInt("onFootSyncRateMs"))
                    .append(" ms (efektif ").append(o.optInt("effectiveOnFootSyncRateMs"))
                    .append(" ms)\n")
                append("sync in-car     : ").append(o.optInt("inCarSyncRateMs"))
                    .append(" ms (efektif ").append(o.optInt("effectiveInCarSyncRateMs"))
                    .append(" ms)\n")
                append("sync senjata    : ").append(o.optInt("weaponSyncRateMs"))
                    .append(" ms (efektif ").append(o.optInt("effectiveWeaponSyncRateMs"))
                    .append(" ms)\n")
            }
            append("class state      : ")
                .append(if (o.optBoolean("classRequestPending")) {
                    "menunggu balasan class #${o.optInt("requestedClassId")}"
                } else if (o.optBoolean("classSelectionKnown")) {
                    "model ${o.optInt("classModel")} / " +
                        (if (o.optBoolean("classSelectable")) "selectable" else "ditolak")
                } else if (o.optBoolean("classSelectionForced")) {
                    "server memaksa pilih ulang — tekan tombol Class"
                } else {
                    "belum ada balasan"
                }).append('\n')
            append("spawn protocol  : ")
                .append(when {
                    !o.optBoolean("spawnResponseKnown") -> "belum ada balasan"
                    !o.optBoolean("spawnAllowed") -> "ditolak server"
                    o.optBoolean("spawnConfirmationAcked") -> "PlayerSpawn di-ACK"
                    o.optBoolean("spawnConfirmationSent") -> "PlayerSpawn menunggu ACK"
                    else -> "diizinkan, menyiapkan PlayerSpawn"
                }).append('\n')
            append("local spawn     : ")
                .append(if (o.optBoolean("localSpawnInfoKnown")) {
                    String.format(
                        Locale.US,
                        "skin ${o.optInt("localModel")} @ %.2f, %.2f, %.2f / %.1f°",
                        o.optDouble("localX"), o.optDouble("localY"),
                        o.optDouble("localZ"), o.optDouble("localAngle")
                    )
                } else {
                    "belum ada SetSpawnInfo"
                }).append('\n')
            append("kontrol server  : ").append(
                if (o.optBoolean("localControllableKnown")) {
                    if (o.optBoolean("localControllable", true)) "aktif" else "FREEZE"
                } else {
                    "belum diatur"
                }
            ).append('\n')
            append("dunia server    : ").append(
                buildString {
                    if (o.optBoolean("localInteriorKnown")) append("INT ${o.optInt("localInterior")}")
                    else append("INT ?")
                    append(" · ")
                    if (o.optBoolean("localVirtualWorldKnown")) append("VW ${o.optInt("localVirtualWorld")}")
                    else append("VW ?")
                    append(" (VW bukan filter collision lokal)")
                }
            ).append('\n')
            append("touch input     : ")
                .append(if (o.optBoolean("nativeControlsKnown")) {
                    String.format(
                        Locale.US,
                        "move %.2f/%.2f, heading %.1f°, pitch %.1f°, keys=0x%04X",
                        o.optDouble("controlMoveX"), o.optDouble("controlMoveY"),
                        o.optDouble("controlHeading"), o.optDouble("controlAimPitch"),
                        o.optInt("controlKeys")
                    )
                } else {
                    "belum ada"
                }).append('\n')
            append("on-foot preview : ")
                .append(if (o.optBoolean("onFootPreviewReady")) {
                    if (o.optBoolean("localPredictionActive")) {
                        if (o.optBoolean("localGrounded")) "collision grounded" else "collision airborne"
                    } else {
                        "siap lokal (menunggu tick collision)"
                    }
                } else {
                    "menunggu state lokal"
                }).append('\n')
            append("aim lokal       : ").append(
                if (o.optBoolean("aimPreviewReady")) {
                    "Packet-203 siap (AIM on-foot; kamera/weapon server-safe)"
                } else {
                    "idle / menunggu AIM, ammo server, collision, atau kamera server"
                }
            ).append('\n')
            append("kendaraan lokal : ").append(
                when {
                    !o.optBoolean("localInVehicle") -> "tidak duduk"
                    o.optInt("localVehicleSeat") != 0 && o.optBoolean("passengerPreviewReady") ->
                        "passenger #${o.optInt("localVehicleId")} · Packet-211 server-anchor"
                    o.optInt("localVehicleSeat") != 0 ->
                        "passenger #${o.optInt("localVehicleId")} menunggu vehicle/sync server"
                    o.optBoolean("inCarPreviewReady") && o.optBoolean("localPredictionActive") ->
                        "driver #${o.optInt("localVehicleId")} · kinematic collision Packet-200"
                    else -> "driver #${o.optInt("localVehicleId")} menunggu vehicle/COL server"
                }
            ).append('\n')
            append("checkpoint      : ")
                .append(if (o.optBoolean("checkpointActive")) "aktif" else "tidak aktif")
                .append(" / race ")
                .append(if (o.optBoolean("raceCheckpointActive")) "aktif" else "tidak aktif")
                .append('\n')
            append("stream          : ").append(o.optInt("receivedPackets"))
                .append(" packet / ").append(o.optInt("receivedRpcs")).append(" RPC\n")
            append("transport       : ").append(o.optInt("reliableKeepalives"))
                .append(" keepalive reliable / ").append(o.optInt("connectedPongs"))
                .append(" pong\n")
            append("link lokal      : idle ").append(o.optLong("inboundIdleMs"))
                .append("ms · ACK terakhir ").append(o.optLong("latestReliableRttMs"))
                .append("ms · rata2 ").append(o.optLong("smoothedReliableRttMs"))
                .append("ms · resend ").append(o.optInt("reliableResends"))
                .append(" (diagnostik transport, bukan ping server)\n")
            append("on-foot keluar : ").append(o.optInt("outboundOnFootPackets"))
                .append(" packet / state lokal ")
                .append(if (o.optBoolean("localOnFootStateKnown")) "siap" else "belum ada")
                .append('\n')
            append("in-car keluar  : ").append(o.optInt("outboundInCarPackets"))
                .append(" Packet-200 kinematic\n")
            append("aim keluar     : ").append(o.optInt("outboundAimPackets"))
                .append(" Packet-203 / state ")
                .append(if (o.optBoolean("localAimStateKnown")) "siap" else "belum ada")
                .append('\n')
            append("tembak keluar  : ").append(o.optInt("outboundNoTargetBulletPackets"))
                .append(" Packet-206 HitType=None (AIM+FIRE; tanpa target/damage lokal)\n")
            append("passenger keluar: ").append(o.optInt("outboundPassengerPackets"))
                .append(" Packet-211 / state ")
                .append(if (o.optBoolean("localPassengerStateKnown")) "siap" else "belum ada")
                .append('\n')
            append("fragment        : ").append(o.optInt("splitFragments"))
                .append(" split / ").append(o.optInt("reassembledPackets")).append(" tersusun\n")
            append("remote on-foot  : ").append(o.optInt("remoteOnFootPlayers"))
                .append(" player / ").append(o.optInt("remoteOnFootPackets"))
                .append(" packet broadcast\n")
            append("remote aim      : ").append(o.optInt("remoteAimPackets"))
                .append(" Packet-203 visual (tanpa hit)\n")
            append("remote bullet   : ").append(o.optInt("remoteBulletPackets"))
                .append(" Packet-206 relay visual (tanpa damage lokal)\n")
            append("state pemain     : ").append(o.optInt("trackedPlayers"))
                .append(" terlacak / ").append(o.optInt("streamedPlayers"))
                .append(" stream-in\n")
            append("state kendaraan  : ").append(o.optInt("trackedVehicles"))
                .append(" terlacak / ").append(o.optInt("streamedVehicles"))
                .append(" stream-in / ").append(o.optInt("remoteVehiclePackets"))
                .append(" sync visual\n")
            append("NPC server       : ").append(o.optInt("trackedServerActors"))
                .append(" retained / max 64 copy render (visual-only)\n")
            append("chat state       : ").append(o.optInt("chatMessages"))
                .append(" pesan tersimpan / ").append(o.optInt("outboundChatMessages"))
                .append(" terkirim native\n")
            append("dialog native    : ").append(o.optInt("outboundDialogResponses"))
                .append(" respon terkirim (ShowDialog tampil otomatis)\n")
            append("RPC terlihat     : ").append(o.optString("rpcSummary").ifBlank { "-" }).append('\n')
            append("sync terlihat    : ").append(o.optString("syncSummary").ifBlank { "-" }).append('\n')
            append("event            : ").append(o.optString("stateSummary").ifBlank { "-" }).append('\n')
            append("status           : ").append(o.optString("error").ifBlank { "ok" })
        }

        fun updateConnectionHint(snapshot: JSONObject) {
            val active = snapshot.optBoolean("active")
            val playerInit = snapshot.optBoolean("playerInit")
            val classCount = snapshot.optLong("spawnClassCount")
            val classPending = snapshot.optBoolean("classRequestPending")
            val spawnPending = snapshot.optBoolean("spawnRequestPending") ||
                (snapshot.optBoolean("spawnConfirmationSent") &&
                    !snapshot.optBoolean("spawnConfirmationAcked"))
            val transportError = snapshot.optString("error").trim()
            val dlR1Profile = snapshot.optString("clientProtocol").startsWith("0.3.DL-R1")
            btnQuickConnect.text = if (active) "Sesi native aktif" else "Hubungkan native"
            // Opening a renderer while the transport is merely accepted made an
            // empty scene look like playable world state. Wait for the native
            // PlayerInit boundary; class/spawn setup can then remain visible
            // inside the world while its own input gate stays locked to ACK.
            val worldAvailable = active && playerInit
            btnOpenWorld.isEnabled = worldAvailable
            btnWorld.isEnabled = worldAvailable
            btnOpenWorld.text = if (active && !playerInit) "Menunggu PlayerInit…" else "Buka Dunia"
            btnClassZero.isEnabled = active && playerInit && classCount > 0L && !classPending && !spawnPending
            btnClassZero.text = when {
                classPending -> "Memuat class…"
                snapshot.optBoolean("classSelectionKnown") && classCount > 0L ->
                    "Ganti class ${snapshot.optInt("requestedClassId") + 1}/$classCount"
                playerInit && classCount > 0L -> "Pilih class"
                else -> "Class awal otomatis"
            }
            btnSpawn.isEnabled = active && playerInit && snapshot.optBoolean("classSelectionKnown") &&
                snapshot.optBoolean("classSelectable") && !spawnPending
            connectionHint.text = when {
                !active && transportError.isNotEmpty() ->
                    "Handshake native berhenti: $transportError. Periksa server/koneksi lalu coba lagi manual."
                !active -> "Sesi belum aktif. Hubungkan native untuk memulai handshake."
                !playerInit && dlR1Profile ->
                    "Profil DL R1 aktif dari versi query; layout CustomSkin inti siap, tetapi unduh/artwork custom masih belum dijamin."
                !playerInit -> "Transport aktif; menunggu PlayerInit dari server sebelum class/spawn."
                snapshot.optBoolean("spawnConfirmationAcked") ->
                    "Spawn dikonfirmasi transport. Buka Dunia untuk kontrol, chat, dan HUD native."
                snapshot.optBoolean("spawnConfirmationSent") ->
                    "PlayerSpawn sudah dikirim; menunggu ACK transport server."
                snapshot.optBoolean("spawnResponseKnown") && !snapshot.optBoolean("spawnAllowed") ->
                    "Server menolak spawn saat ini. Pilih class lain atau ikuti instruksi server."
                snapshot.optBoolean("classRequestPending") ->
                    "Meminta class awal/pilihan ke server; tunggu responsnya sebelum Spawn."
                snapshot.optBoolean("classSelectionKnown") ->
                    "Class diterima. Tekan Spawn untuk meminta izin spawn dari server."
                snapshot.optLong("spawnClassCount") > 0L ->
                    "Pilih class (tombol Class), lalu tekan Spawn setelah server memberi respons."
                else -> "Sesi siap, tetapi server belum mengumumkan class yang dapat dipilih."
            }
        }

        fun stopSessionPolling() {
            sessionPollJob?.cancel()
            sessionPollJob = null
        }

        fun startSessionPolling() {
            if (sessionPollJob?.isActive == true) return
            sessionPollJob = activity.lifecycleScope.launch {
                var lastPresentationNanos = 0L
                var wasForeground = activity.lifecycle.currentState.isAtLeast(
                    androidx.lifecycle.Lifecycle.State.RESUMED
                )
                while (isActive && dialog.isShowing) {
                    try {
                        val foreground = activity.lifecycle.currentState.isAtLeast(
                            androidx.lifecycle.Lifecycle.State.RESUMED
                        )
                        if (wasForeground && !foreground) {
                            // Presentation is lifecycle-bound even though the
                            // explicit socket receives a brief low-power grace.
                            NativeServerDialogController.clear()
                            NativeServerMenuController.clear()
                        }
                        wasForeground = foreground
                        // Keep only bounded transport ACK/retry maintenance alive
                        // while hidden. Foreground remains low-latency for sync;
                        // background is deliberately slower and never opens
                        // server-directed dialog/menu presentation.
                        val timeoutMs = if (foreground) {
                            SESSION_POLL_TIMEOUT_MS
                        } else {
                            BACKGROUND_SESSION_POLL_TIMEOUT_MS
                        }
                        val raw = withContext(Dispatchers.IO) {
                            NativeBridge.nativeSessionPoll(timeoutMs)
                        }
                        if (!dialog.isShowing) break
                        val snapshot = JSONObject(raw)
                        val nowNanos = System.nanoTime()
                        val presentationDue = lastPresentationNanos == 0L ||
                            nowNanos - lastPresentationNanos >= SESSION_PRESENTATION_INTERVAL_NANOS
                        if ((foreground && presentationDue) || !snapshot.optBoolean("active")) {
                            log.text = formatSession(snapshot)
                            updateConnectionHint(snapshot)
                            if (foreground && snapshot.optBoolean("active")) {
                                val worldRaw = withContext(Dispatchers.IO) {
                                    NativeBridge.nativeSessionWorldSnapshot()
                                }
                                if (dialog.isShowing && activity.lifecycle.currentState.isAtLeast(
                                        androidx.lifecycle.Lifecycle.State.RESUMED
                                    )) {
                                    runCatching {
                                        val frame = WorldFrame.from(JSONObject(worldRaw))
                                        NativeServerDialogController.submit(activity, frame)
                                        NativeServerMenuController.submit(activity, frame)
                                    }
                                }
                            }
                            if (foreground) lastPresentationNanos = nowNanos
                        }
                        if (!snapshot.optBoolean("active")) {
                            // A terminal native event (including PlayerClose or
                            // ConnectionRejected) may arrive while this detail
                            // surface, rather than a world surface, is visible.
                            // Do not leave a retained server dialog/menu tappable
                            // after its owning UDP session is gone.
                            NativeServerDialogController.clear()
                            NativeServerMenuController.clear()
                            break
                        }
                    } catch (t: Throwable) {
                        if (dialog.isShowing) log.text = "Sesi native berhenti: ${t.message}"
                        break
                    }
                    // A foreground poll drains promptly; hidden transport gets
                    // a bounded, low-power cadence until the Activity resumes.
                    delay(if (activity.lifecycle.currentState.isAtLeast(
                            androidx.lifecycle.Lifecycle.State.RESUMED
                        )) SESSION_POLL_YIELD_MS else BACKGROUND_SESSION_POLL_YIELD_MS)
                }
            }
        }

        btnInfo.setOnClickListener {
            run {
                val json = JSONObject(NativeBridge.nativeQueryInfo(server.ip, server.port, 1500))
                if (!json.optBoolean("ok")) {
                    "GAGAL: ${json.optString("error")}"
                } else {
                    buildString {
                        append("hostname  : ").append(json.optString("hostname")).append('\n')
                        append("gamemode  : ").append(json.optString("gamemode")).append('\n')
                        append("language  : ").append(json.optString("language")).append('\n')
                        append("players   : ").append(json.optInt("players")).append(" / ")
                            .append(json.optInt("maxPlayers")).append('\n')
                        append("password  : ").append(if (json.optBoolean("password")) "ya" else "tidak")
                    }
                }
            }
        }

        btnRules.setOnClickListener {
            run {
                val arr = JSONArray(NativeBridge.nativeQueryRules(server.ip, server.port, 1500))
                if (arr.length() == 0) {
                    "Tidak ada rules (server menonaktifkan query rules)"
                } else {
                    buildString {
                        append("rules (").append(arr.length()).append("):\n")
                        for (i in 0 until arr.length()) {
                            val o = arr.getJSONObject(i)
                            append("  ").append(o.optString("name")).append(" = ")
                                .append(o.optString("value")).append('\n')
                        }
                    }
                }
            }
        }

        btnPlayers.setOnClickListener {
            run {
                val arr = JSONArray(NativeBridge.nativeQueryPlayers(server.ip, server.port, true, 1500))
                if (arr.length() == 0) {
                    "Tidak ada data player (query 'd' sering dimatikan server)"
                } else {
                    buildString {
                        append("players (").append(arr.length()).append("):\n")
                        for (i in 0 until arr.length()) {
                            val o = arr.getJSONObject(i)
                            append("  [").append(o.optInt("id")).append("] ")
                                .append(o.optString("name"))
                                .append("  score=").append(o.optInt("score"))
                                .append(" ping=").append(o.optInt("ping")).append('\n')
                        }
                    }
                }
            }
        }

        btnPing.setOnClickListener {
            run {
                val ms = NativeBridge.nativeQueryPing(server.ip, server.port, 1500)
                if (ms < 0) "ping gagal (timeout)" else "ping: $ms ms"
            }
        }

        btnSession.setOnClickListener {
            if (sessionPollJob?.isActive == true) {
                log.text = "Sesi native sudah aktif dan sedang polling otomatis."
                return@setOnClickListener
            }
            if (sessionConnectJob?.isActive == true) {
                log.text = "Handshake native masih berjalan…"
                return@setOnClickListener
            }
            if (sessionDisconnectJob?.isActive == true) {
                log.text = "Menutup sesi lama dulu…"
                return@setOnClickListener
            }
            log.text = "Mengecek profil server lalu membuka sesi native…"
            connectionHint.text = "Query ulang versi server untuk memilih profil ClientJoin aman, lalu membuka socket native…"
            sessionConnectJob = activity.lifecycleScope.launch {
                try {
                    val raw = withContext(Dispatchers.IO) {
                        val prior = JSONObject(NativeBridge.nativeSessionStatus())
                        if (prior.optBoolean("active")) {
                            NativeBridge.nativeSessionPoll(SESSION_POLL_TIMEOUT_MS)
                        } else {
                            // A directory/index value can be stale or describe a
                            // compatibility range. Re-query immediately before
                            // ClientJoin so the native DL profile is selected
                            // only from this endpoint's fresh version response.
                            val advertisedVersion = runCatching {
                                val info = JSONObject(NativeBridge.nativeQueryInfo(
                                    server.ip,
                                    server.port,
                                    1_500
                                ))
                                if (info.optBoolean("ok")) info.optString("version") else ""
                            }.getOrElse { "" }
                            NativeBridge.nativeSessionConnect(
                                server.ip,
                                server.port,
                                1000,
                                PlayerProfile.nickname(activity),
                                advertisedVersion
                            )
                        }
                    }
                    val snapshot = JSONObject(raw)
                    if (!dialog.isShowing) {
                        withContext(Dispatchers.IO) { NativeBridge.nativeSessionDisconnect() }
                        return@launch
                    }
                    log.text = formatSession(snapshot)
                    updateConnectionHint(snapshot)
                    if (snapshot.optBoolean("active")) startSessionPolling()
                } catch (t: Throwable) {
                    if (dialog.isShowing) {
                        log.text = "Gagal membuka sesi native: ${t.message}"
                        connectionHint.text = "Handshake gagal. Cek koneksi/server lalu coba lagi; jangan ulangi percobaan malformed."
                    }
                } finally {
                    sessionConnectJob = null
                }
            }
        }

        btnQuickConnect.setOnClickListener {
            if (sessionPollJob?.isActive == true) {
                log.text = "Sesi native sudah aktif. Ikuti status class/spawn di atas atau buka Dunia."
            } else {
                btnSession.performClick()
            }
        }
        btnOpenWorld.setOnClickListener { btnWorld.performClick() }

        fun requestClass(step: Int) {
            activity.lifecycleScope.launch {
                try {
                    val before = withContext(Dispatchers.IO) {
                        JSONObject(NativeBridge.nativeSessionStatus())
                    }
                    val count = before.optLong("spawnClassCount").coerceIn(0L, 65_536L).toInt()
                    if (!before.optBoolean("active") || !before.optBoolean("playerInit")) {
                        log.text = "Tunggu ClientJoin dan PlayerInit sebelum memilih class."
                    } else if (count <= 0) {
                        log.text = "Server tidak mengumumkan slot class yang dapat dipilih."
                    } else {
                        val base = ((classCursor % count) + count) % count
                        val requested = if (step < 0) (base - 1 + count) % count else base
                        val raw = withContext(Dispatchers.IO) {
                            NativeBridge.nativeSessionRequestClass(requested)
                        }
                        val snapshot = JSONObject(raw)
                        // Advance only when native successfully retained this
                        // one request as pending; error replies keep the cursor.
                        if (snapshot.optBoolean("classRequestPending")) {
                            classCursor = (requested + 1) % count
                        }
                        btnClassZero.text = "Class ${requested + 1}/$count"
                        log.text = formatSession(snapshot)
                        updateConnectionHint(snapshot)
                        if (snapshot.optBoolean("active")) startSessionPolling()
                    }
                } catch (t: Throwable) {
                    log.text = "Gagal meminta class: ${t.message}"
                }
            }
        }
        btnClassZero.setOnClickListener { requestClass(1) }
        btnClassZero.setOnLongClickListener {
            requestClass(-1)
            true
        }

        btnSpawn.setOnClickListener {
            activity.lifecycleScope.launch {
                try {
                    val raw = withContext(Dispatchers.IO) {
                        NativeBridge.nativeSessionRequestSpawn()
                    }
                    val snapshot = JSONObject(raw)
                    log.text = formatSession(snapshot)
                    updateConnectionHint(snapshot)
                    if (snapshot.optBoolean("active")) startSessionPolling()
                } catch (t: Throwable) {
                    log.text = "Gagal meminta spawn: ${t.message}"
                }
            }
        }

        btnControls.setOnClickListener {
            activity.lifecycleScope.launch {
                try {
                    val snapshot = withContext(Dispatchers.IO) {
                        JSONObject(NativeBridge.nativeSessionStatus())
                    }
                    if (!snapshot.optBoolean("active")) {
                        log.text = "Buka Sesi native dulu sebelum memakai kontrol touch."
                    } else {
                        NativeControlsDialog.show(activity)
                    }
                } catch (t: Throwable) {
                    log.text = "Gagal membuka kontrol touch: ${t.message}"
                }
            }
        }

        btnChat.setOnClickListener {
            activity.lifecycleScope.launch {
                try {
                    val snapshot = withContext(Dispatchers.IO) {
                        JSONObject(NativeBridge.nativeSessionStatus())
                    }
                    if (!snapshot.optBoolean("active")) {
                        log.text = "Buka Sesi native dulu sebelum mengirim chat atau command."
                    } else {
                        NativeChatDialog.show(activity)
                    }
                } catch (t: Throwable) {
                    log.text = "Gagal membuka chat native: ${t.message}"
                }
            }
        }

        btnWorld.setOnClickListener {
            activity.lifecycleScope.launch {
                try {
                    val snapshot = withContext(Dispatchers.IO) {
                        JSONObject(NativeBridge.nativeSessionStatus())
                    }
                    if (!snapshot.optBoolean("active")) {
                        log.text = "Buka Sesi native dulu sebelum membuka Dunia native."
                    } else if (!snapshot.optBoolean("playerInit")) {
                        log.text = "Transport aktif, tetapi server belum mengirim PlayerInit. Tunggu dulu sebelum membuka Dunia."
                    } else {
                        startSessionPolling()
                        // A player-facing world entry prefers the actual GLES
                        // scene; the scene routes older GLES devices to the
                        // native radar fallback itself.
                        NativeWorldSceneDialog.show(activity)
                    }
                } catch (t: Throwable) {
                    log.text = "Gagal membuka Dunia native: ${t.message}"
                }
            }
        }

        btnSessionDisconnect.setOnClickListener {
            if (sessionDisconnectJob?.isActive == true) {
                log.text = "Menutup sesi native…"
                return@setOnClickListener
            }
            stopSessionPolling()
            val connecting = sessionConnectJob
            connecting?.cancel()
            NativeServerDialogController.clear()
            NativeServerMenuController.clear()
            sessionDisconnectJob = activity.lifecycleScope.launch {
                try {
                    val raw = withContext(Dispatchers.IO) {
                        // Native connect is a bounded blocking UDP operation and
                        // cannot be interrupted safely from Java. Wait for it to
                        // unwind before disconnecting so it cannot recreate a
                        // hidden socket after the player chose Disconnect.
                        connecting?.join()
                        NativeBridge.nativeSessionDisconnect()
                    }
                    if (dialog.isShowing) {
                        val snapshot = JSONObject(raw)
                        updateConnectionHint(snapshot)
                        log.text = formatSession(snapshot)
                    }
                } finally {
                    sessionDisconnectJob = null
                }
            }
        }

        btnProbe.setOnClickListener {
            run {
                // Keep this diagnostic on the same fresh, bounded profile
                // selection as the player-facing retained-session action.
                val advertisedVersion = runCatching {
                    val info = JSONObject(NativeBridge.nativeQueryInfo(server.ip, server.port, 1_500))
                    if (info.optBoolean("ok")) info.optString("version") else ""
                }.getOrElse { "" }
                val o = JSONObject(NativeBridge.nativeRaknetProbe(
                    server.ip,
                    server.port,
                    1500,
                    advertisedVersion
                ))
                buildString {
                    append("=== RAKNET PROBE ===\n")
                    append(o.optString("log")).append('\n')
                    append("open connection : ").append(o.optBoolean("openConnection")).append('\n')
                    append("cookie ok       : ").append(o.optBoolean("cookieOk")).append('\n')
                    append("auth challenge  : ").append(o.optBoolean("authenticated")).append('\n')
                    append("profil wire     : ").append(o.optString("clientProtocol").ifBlank { "-" }).append('\n')
                    append("client join     : ").append(o.optBoolean("joined")).append('\n')
                    append("accepted        : ").append(o.optBoolean("accepted")).append('\n')
                    append("PlayerInit RPC  : ").append(o.optBoolean("playerInit")).append('\n')
                    if (o.optBoolean("playerInit")) {
                        append("player ID       : ").append(o.optInt("playerId")).append('\n')
                        append("nama server     : ").append(o.optString("serverName")).append('\n')
                    }
                    append("stream          : ").append(o.optInt("receivedPackets"))
                        .append(" packet / ").append(o.optInt("receivedRpcs")).append(" RPC\n")
                    append("fragment        : ").append(o.optInt("splitFragments"))
                        .append(" split / ").append(o.optInt("reassembledPackets")).append(" tersusun\n")
                    append("RPC terlihat     : ").append(o.optString("rpcSummary").ifBlank { "-" }).append('\n')
                    append("state pemain     : ").append(o.optInt("trackedPlayers"))
                        .append(" terlacak — ").append(o.optString("stateSummary").ifBlank { "-" }).append('\n')
                    append("varian          : ").append(o.optString("variant").ifBlank { "-" }).append('\n')
                    append("status          : ").append(o.optString("error").ifBlank { "ok" })
                }
            }
        }

        // A retained UDP socket belongs to this explicit detail/session view.
        // Closing the dialog releases it instead of leaving an invisible player
        // connection behind while a renderer/sync loop is still under development.
        dialog.setOnDismissListener {
            stopSessionPolling()
            val connecting = sessionConnectJob
            connecting?.cancel()
            NativeServerDialogController.clear()
            NativeServerMenuController.clear()
            if (sessionDisconnectJob?.isActive != true) {
                sessionDisconnectJob = activity.lifecycleScope.launch(Dispatchers.IO) {
                    // Serialize teardown behind a possibly in-flight native
                    // connect; otherwise a late handshake return could retain
                    // a socket after this player-facing dialog disappears.
                    runCatching {
                        connecting?.join()
                        NativeBridge.nativeSessionDisconnect()
                    }
                }
            }
        }
        dialog.show()
    }
}
