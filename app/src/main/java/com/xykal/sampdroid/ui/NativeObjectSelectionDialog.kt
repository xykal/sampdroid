package com.xykal.sampdroid.ui

import android.graphics.Color
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
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
import kotlin.math.sqrt

/**
 * Native presentation for PlayerBeginObjectSelect (RPC 27).
 *
 * A PC client normally picks object geometry directly in its GTA view. Until
 * the Android renderer owns full mesh hit-testing, this deliberate fallback
 * lists current bounded renderer objects. Android gives native only an object
 * ID; native rechecks server selection mode, model, lifecycle and sampled
 * transform before it emits OnPlayerSelectObject, so this view cannot forge a
 * target model or a callback coordinate.
 */
internal object NativeObjectSelectionDialog {
    private const val MAX_VISIBLE_OBJECTS = 24
    private const val POLL_DELAY_MS = 220L

    fun show(activity: AppCompatActivity) {
        val density = activity.resources.displayMetrics.density
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((12f * density).toInt(), (8f * density).toInt(), (12f * density).toInt(), 0)
            setBackgroundColor(Color.rgb(10, 20, 28))
        }
        val description = TextView(activity).apply {
            text = "PILIH OBJEK SERVER · menunggu mode RPC 27…"
            textSize = 11f
            setTextColor(Color.rgb(183, 225, 241))
        }
        val rows = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, (6f * density).toInt(), 0, (12f * density).toInt())
        }
        val scroll = ScrollView(activity).apply {
            addView(rows, ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ))
        }
        root.addView(description)
        root.addView(scroll, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            0,
            1f
        ))

        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Pilih objek native")
            .setView(root)
            .setNegativeButton("Tutup", null)
            .create()
        var pollJob: Job? = null
        var sending = false
        var lastLayoutKey = ""

        fun submitChoice(objectId: Int, expectedRevision: Long) {
            if (sending || expectedRevision <= 0L) return
            sending = true
            activity.lifecycleScope.launch {
                val response = withContext(Dispatchers.IO) {
                    runCatching { NativeBridge.nativeSessionSelectObject(objectId, expectedRevision) }
                }
                sending = false
                val error = response.exceptionOrNull()?.message
                    ?: response.getOrNull()?.let { raw ->
                        runCatching { JSONObject(raw).optString("error") }.getOrDefault("")
                    }.orEmpty()
                if (error.isNotBlank()) {
                    Toast.makeText(activity, "Pilihan objek belum terkirim: $error", Toast.LENGTH_SHORT).show()
                }
            }
        }

        fun render(frame: WorldFrame) {
            val selectable = frame.active && frame.objectSelectionActive
            // Current source object pools reserve ID zero; filtering it here
            // avoids presenting an action that the native RPC-27 gate must
            // reject. Model bounds mirror the sender's final validation.
            val candidates = frame.objects.filter { objectView ->
                objectView.id in 1..65534 && objectView.model in 1L..65535L
            }
            val ordered = if (frame.localKnown) {
                candidates.sortedWith(compareBy<WorldObject>({ objectDistanceSquared(frame, it) }, { it.id }))
            } else {
                candidates.sortedBy { it.id }
            }.take(MAX_VISIBLE_OBJECTS)
            val key = buildString {
                append(selectable).append('|').append(frame.active).append('|')
                // Revision belongs in the structural key: identical candidates
                // under a new server selection lifecycle must receive fresh
                // click closures carrying the new native token.
                append(frame.objectSelectionRevision).append('|')
                ordered.forEach { append(it.id).append(':').append(it.model).append(':').append(it.moving).append(';') }
            }
            description.text = when {
                !frame.active -> "Sesi native belum aktif."
                !frame.objectSelectionActive ->
                    "Mode pilih objek belum diberikan server. Tombol ini hanya aktif setelah PlayerBeginObjectSelect (RPC 27)."
                ordered.isEmpty() ->
                    "Mode pilih objek server aktif, tetapi belum ada objek renderable di snapshot bounded."
                else ->
                    "Mode pilih objek server aktif · pilih salah satu objek (maks. $MAX_VISIBLE_OBJECTS). " +
                        "Posisi/model tidak berasal dari Android; native revalidasi ulang sebelum kirim."
            }
            if (key == lastLayoutKey) {
                rows.alpha = if (selectable) 1f else 0.55f
                return
            }
            lastLayoutKey = key
            rows.removeAllViews()
            rows.alpha = if (selectable) 1f else 0.55f
            if (ordered.isEmpty()) {
                rows.addView(TextView(activity).apply {
                    text = "Menunggu object Create/stream state yang dapat dirender…"
                    textSize = 12f
                    setTextColor(Color.rgb(220, 238, 246))
                })
                return
            }
            ordered.forEach { objectView ->
                rows.addView(Button(activity).apply {
                    text = objectLabel(frame, objectView)
                    textSize = 11f
                    isAllCaps = false
                    isEnabled = selectable
                    // Capture the exact native selection lifecycle represented
                    // by this row; sender-side revision validation is final.
                    setOnClickListener {
                        submitChoice(objectView.id, frame.objectSelectionRevision)
                    }
                }, LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply {
                    bottomMargin = (3f * density).toInt()
                })
            }
            rows.alpha = if (selectable) 1f else 0.55f
        }

        dialog.setOnShowListener {
            dialog.window?.setLayout(
                ViewGroup.LayoutParams.MATCH_PARENT,
                (activity.resources.displayMetrics.heightPixels * 0.70f).toInt()
            )
            pollJob = activity.lifecycleScope.launch {
                while (isActive && dialog.isShowing) {
                    val frame = runCatching {
                        val raw = withContext(Dispatchers.IO) { NativeBridge.nativeSessionWorldSnapshot() }
                        WorldFrame.from(JSONObject(raw))
                    }.getOrElse { WorldFrame(error = it.message ?: "snapshot pilih objek gagal") }
                    render(frame)
                    delay(POLL_DELAY_MS)
                }
            }
        }
        dialog.setOnDismissListener { pollJob?.cancel() }
        dialog.show()
    }

    private fun objectDistanceSquared(frame: WorldFrame, objectView: WorldObject): Float {
        val dx = objectView.x - frame.localX
        val dy = objectView.y - frame.localY
        val dz = objectView.z - frame.localZ
        return dx * dx + dy * dy + dz * dz
    }

    private fun objectLabel(frame: WorldFrame, objectView: WorldObject): String {
        val motion = if (objectView.moving) " · bergerak" else ""
        val distance = if (frame.localKnown) {
            val meters = sqrt(objectDistanceSquared(frame, objectView).toDouble()).toInt()
            " · ±${meters}m"
        } else {
            ""
        }
        return "Objek #${objectView.id} · model ${objectView.model}$motion$distance"
    }
}
