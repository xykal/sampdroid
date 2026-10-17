package com.xykal.sampdroid.ui

import android.content.Context
import android.text.method.ScrollingMovementMethod
import android.view.LayoutInflater
import android.view.View
import android.widget.Button
import android.widget.TextView
import androidx.lifecycle.LifecycleCoroutineScope
import androidx.lifecycle.lifecycleScope
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.xykal.sampdroid.NativeBridge
import com.xykal.sampdroid.R
import com.xykal.sampdroid.model.ServerEntry
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

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

        fun run(block: () -> String) {
            log.text = "…"
            activity.lifecycleScope.launch {
                val out = withContext(Dispatchers.IO) {
                    runCatching(block).getOrElse { "ERROR: ${it.message}" }
                }
                log.text = out
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
                    append("varian          : ").append(o.optString("variant").ifBlank { "-" }).append('\n')
                    append("status          : ").append(o.optString("error").ifBlank { "ok" })
                }
            }
        }

        dialog.show()
    }
}
