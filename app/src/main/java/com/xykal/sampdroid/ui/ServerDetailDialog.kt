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
// Poll transport promptly, but avoid rebuilding a full JSON world/UI more often
// than the renderer's own immutable-frame cadence.
private const val SESSION_PRESENTATION_INTERVAL_NANOS = 100_000_000L

object ServerDetailDialog {

    fun show(activity: AppCompatActivity, server: ServerEntry) {
        val view = LayoutInflater.from(activity).inflate(R.layout.dialog_server, null)
        val header: TextView = view.findViewById(R.id.tvHeader)
        val body: TextView = view.findViewById(R.id.tvBody)
        val log: TextView = view.findViewById(R.id.tvLog)
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
        // Cursor is a UI hint only. Native rechecks PlayerInit's raw class
        // count and serializes one reliable class request at a time.
        var classCursor = 0

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
            append("=== SESI NATIVE (EKSPERIMEN) ===\n")
            append("Soket ini native Android, bukan Winlator/samp.exe.\n")
            append("Spawn/local + sync remote native; scene GPU punya collision on-foot, driver Packet-200, AimSync Packet-203, serta Packet-206 HitType=None saat AIM+FIRE. Target/hit/damage/ammo lokal tetap tidak diklaim. Gameplay GTA penuh belum.\n")
            append("Saat aktif, stream dipoll otomatis sampai sesi diputus/dialog ditutup.\n\n")
            append("aktif           : ").append(o.optBoolean("active")).append('\n')
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

        fun stopSessionPolling() {
            sessionPollJob?.cancel()
            sessionPollJob = null
        }

        fun startSessionPolling() {
            if (sessionPollJob?.isActive == true) return
            sessionPollJob = activity.lifecycleScope.launch {
                var lastPresentationNanos = 0L
                while (isActive && dialog.isShowing) {
                    try {
                        // This is the hot path: it advances UDP/ACK/retry work
                        // frequently, while snapshot JSON/UI stays bounded below.
                        val raw = withContext(Dispatchers.IO) {
                            NativeBridge.nativeSessionPoll(SESSION_POLL_TIMEOUT_MS)
                        }
                        val snapshot = JSONObject(raw)
                        val nowNanos = System.nanoTime()
                        if (lastPresentationNanos == 0L ||
                            nowNanos - lastPresentationNanos >= SESSION_PRESENTATION_INTERVAL_NANOS ||
                            !snapshot.optBoolean("active")
                        ) {
                            val worldRaw = withContext(Dispatchers.IO) {
                                NativeBridge.nativeSessionWorldSnapshot()
                            }
                            log.text = formatSession(snapshot)
                            runCatching {
                                val frame = WorldFrame.from(JSONObject(worldRaw))
                                NativeServerDialogController.submit(activity, frame)
                                NativeServerMenuController.submit(activity, frame)
                            }
                            lastPresentationNanos = nowNanos
                        }
                        if (!snapshot.optBoolean("active")) break
                    } catch (t: Throwable) {
                        log.text = "Sesi native berhenti: ${t.message}"
                        break
                    }
                    // The native poll blocks only briefly, then drains a
                    // bounded ready-datagram burst. Yield enough for UI work
                    // without starving active remote sync on busy servers.
                    delay(SESSION_POLL_YIELD_MS)
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
            log.text = "Membuka sesi native…"
            activity.lifecycleScope.launch {
                try {
                    val raw = withContext(Dispatchers.IO) {
                        val prior = JSONObject(NativeBridge.nativeSessionStatus())
                        if (prior.optBoolean("active")) {
                            NativeBridge.nativeSessionPoll(SESSION_POLL_TIMEOUT_MS)
                        } else {
                            NativeBridge.nativeSessionConnect(server.ip, server.port, 1000)
                        }
                    }
                    val snapshot = JSONObject(raw)
                    log.text = formatSession(snapshot)
                    if (snapshot.optBoolean("active")) startSessionPolling()
                } catch (t: Throwable) {
                    log.text = "Gagal membuka sesi native: ${t.message}"
                }
            }
        }

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
                        btnClassZero.text = "Class ${classCursor + 1}/$count"
                        log.text = formatSession(snapshot)
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
                    } else {
                        startSessionPolling()
                        NativeWorldDialog.show(activity)
                    }
                } catch (t: Throwable) {
                    log.text = "Gagal membuka Dunia native: ${t.message}"
                }
            }
        }

        btnSessionDisconnect.setOnClickListener {
            stopSessionPolling()
            NativeServerDialogController.clear()
            NativeServerMenuController.clear()
            run {
                formatSession(JSONObject(NativeBridge.nativeSessionDisconnect()))
            }
        }

        btnProbe.setOnClickListener {
            run {
                val o = JSONObject(NativeBridge.nativeRaknetProbe(server.ip, server.port, 1500))
                buildString {
                    append("=== RAKNET PROBE ===\n")
                    append(o.optString("log")).append('\n')
                    append("open connection : ").append(o.optBoolean("openConnection")).append('\n')
                    append("cookie ok       : ").append(o.optBoolean("cookieOk")).append('\n')
                    append("auth challenge  : ").append(o.optBoolean("authenticated")).append('\n')
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
            NativeServerDialogController.clear()
            NativeServerMenuController.clear()
            activity.lifecycleScope.launch(Dispatchers.IO) {
                runCatching { NativeBridge.nativeSessionDisconnect() }
            }
        }
        dialog.show()
    }
}
