package com.xykal.sampdroid.ui

import android.graphics.Color
import android.graphics.Typeface
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.xykal.sampdroid.NativeBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * TAB-style roster for exact server RPC-155 score/ping data. It requests a
 * server cache then renders immutable WorldFrame copies; no Android row is
 * selectable and no score/ping value is synthesized locally.
 */
internal object NativeScoreboardDialog {

    fun show(activity: AppCompatActivity) {
        val density = activity.resources.displayMetrics.density
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((12f * density).toInt(), (8f * density).toInt(), (12f * density).toInt(), 0)
            setBackgroundColor(Color.rgb(10, 20, 28))
        }
        val description = TextView(activity).apply {
            text = "SCOREBOARD SERVER · RPC 155\nMeminta cache score/ping server. Bukan roster query web dan tidak mengubah gameplay."
            setTextColor(Color.rgb(183, 225, 241))
            textSize = 11f
        }
        val refresh = Button(activity).apply {
            text = "REFRESH SERVER"
            textSize = 11f
        }
        val rows = TextView(activity).apply {
            typeface = Typeface.MONOSPACE
            setTextColor(Color.rgb(228, 244, 249))
            textSize = 12f
            setPadding(0, (6f * density).toInt(), 0, (12f * density).toInt())
            text = "Meminta scoreboard server…"
        }
        val scroll = ScrollView(activity).apply {
            addView(rows, ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ))
        }
        root.addView(description)
        root.addView(refresh)
        root.addView(scroll, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1f
        ))

        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Scoreboard native")
            .setView(root)
            .setNegativeButton("Tutup", null)
            .create()
        var pollJob: Job? = null
        fun requestServerRoster() {
            activity.lifecycleScope.launch(Dispatchers.IO) {
                runCatching { NativeBridge.nativeSessionRequestScores() }
            }
        }
        refresh.setOnClickListener { requestServerRoster() }
        dialog.setOnShowListener {
            dialog.window?.setLayout(
                ViewGroup.LayoutParams.MATCH_PARENT,
                (activity.resources.displayMetrics.heightPixels * 0.68f).toInt()
            )
            requestServerRoster()
            pollJob = activity.lifecycleScope.launch {
                while (isActive && dialog.isShowing) {
                    val frame = runCatching {
                        val raw = withContext(Dispatchers.IO) {
                            NativeBridge.nativeSessionWorldSnapshot()
                        }
                        WorldFrame.from(JSONObject(raw))
                    }.getOrElse { WorldFrame(error = it.message ?: "snapshot scoreboard gagal") }
                    rows.text = format(frame)
                    delay(250)
                }
            }
        }
        dialog.setOnDismissListener { pollJob?.cancel() }
        dialog.show()
    }

    private fun format(frame: WorldFrame): String = buildString {
        append(" ID  NAME                     SCORE   PING\n")
        append("────────────────────────────────────────\n")
        if (!frame.active) {
            append("Sesi native belum aktif.")
            return@buildString
        }
        if (frame.localScoreKnown) {
            append(row(frame.localId, "YOU", frame.localScore, frame.localPing, false))
        }
        frame.scoreboard.forEach { entry ->
            // A server may include local ID in its generic roster. Keep a
            // single explicit YOU row so a stale duplicate cannot confuse UI.
            if (frame.localScoreKnown && entry.id == frame.localId) return@forEach
            append(row(entry.id, entry.name.ifBlank { "#${entry.id}" }, entry.score, entry.ping, entry.npc))
        }
        if (!frame.localScoreKnown && frame.scoreboard.isEmpty()) {
            append("Menunggu respons RPC ScoresAndPings dari server…")
        } else {
            append("\n${frame.scoreboard.size + if (frame.localScoreKnown) 1 else 0} baris server ditampilkan (cap 64).")
        }
        if (frame.error.isNotBlank()) append("\n\nSesi: ${frame.error}")
    }

    private fun row(id: Int, rawName: String, score: Int, ping: Long, npc: Boolean): String {
        val name = (if (npc) "[NPC] $rawName" else rawName).take(24)
        return "%3d  %-24s %6d %6d\n".format(id, name, score, ping.coerceAtMost(999_999L))
    }
}
