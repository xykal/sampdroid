package com.xykal.sampdroid.ui

import android.graphics.Color
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
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
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject
import kotlin.math.abs

/**
 * Bounded numeric fallback for the server's ObjectEdit mode (RPC 117).
 *
 * A desktop client uses a GTA mouse gizmo; this Android-native surface offers
 * fine/coarse axis steps instead. It never chooses the object ID or its global/
 * player-object kind; each tap carries only a copied revision and transform.
 * Native rechecks revision equality, visible object lifecycle, transform envelope,
 * cadence, and reliable channel immediately
 * before every response packet.
 */
internal object NativeObjectEditDialog {
    private const val RESPONSE_CANCEL = 0
    private const val RESPONSE_FINAL = 1
    private const val RESPONSE_UPDATE = 2

    private data class Transform(
        val x: Float,
        val y: Float,
        val z: Float,
        val rotationX: Float,
        val rotationY: Float,
        val rotationZ: Float
    )

    fun show(activity: AppCompatActivity) {
        val density = activity.resources.displayMetrics.density
        var activeRevision = 0L
        var baseline: Transform? = null
        var working: Transform? = null
        var fineStep = true
        var terminalQueued = false
        var modeActive = false
        var pollJob: Job? = null
        // Serializes touch taps so a delayed older UPDATE cannot overtake a
        // newer transform in the reliable RPC stream.
        val outboundMutex = Mutex()
        val editorButtons = mutableListOf<Button>()

        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((14f * density).toInt(), (8f * density).toInt(), (14f * density).toInt(), 0)
        }
        val help = TextView(activity).apply {
            text = "EDIT OBJEK NATIVE\nServer membuka RPC 117. Android memakai langkah angka sebagai fallback " +
                "touch; ID/kind/revisi objek dan pengiriman reliable tetap divalidasi C++."
            textSize = 12f
            setTextColor(Color.rgb(215, 237, 247))
        }
        val target = TextView(activity).apply {
            text = "Menunggu mode edit objek dari server…"
            textSize = 12f
            setTextColor(Color.rgb(185, 246, 202))
            setPadding(0, (8f * density).toInt(), 0, 0)
        }
        val values = TextView(activity).apply {
            text = "Transform belum tersedia."
            textSize = 12f
            setTextColor(Color.rgb(225, 232, 238))
            setPadding(0, (4f * density).toInt(), 0, 0)
        }
        val axes = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, (6f * density).toInt(), 0, 0)
        }
        val status = TextView(activity).apply {
            text = "Tahan server sebagai authority: UPDATE hanya request edit, FINAL/CANCEL menunggu lifecycle server."
            textSize = 11f
            setTextColor(Color.rgb(255, 202, 96))
            setPadding(0, (7f * density).toInt(), 0, 0)
        }
        val actionRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, (7f * density).toInt(), 0, 0)
        }

        fun labelFor(transform: Transform?): String = if (transform == null) {
            "Transform belum tersedia."
        } else {
            "POS  X ${format(transform.x)}   Y ${format(transform.y)}   Z ${format(transform.z)}\n" +
                "ROT  X ${format(transform.rotationX)}°   Y ${format(transform.rotationY)}°   " +
                "Z ${format(transform.rotationZ)}°"
        }
        fun refreshValues() {
            values.text = labelFor(working)
        }
        fun setEditorEnabled(enabled: Boolean) {
            editorButtons.forEach { it.isEnabled = enabled }
        }

        lateinit var dialog: androidx.appcompat.app.AlertDialog
        fun send(response: Int, closeOnSuccess: Boolean = false) {
            val current = working
            // Capture the copied server revision together with this transform.
            // Native rejects it if BeginObjectEdit changes before the coroutine
            // reaches the session mutex, so an old tap cannot retarget a new UI.
            val revision = activeRevision
            if (current == null || revision <= 0L) {
                status.text = "Transform objek belum resolve; menunggu lifecycle/render state server."
                return
            }
            activity.lifecycleScope.launch {
                outboundMutex.withLock {
                    val responseResult = runCatching {
                        withContext(Dispatchers.IO) {
                            NativeBridge.nativeSessionEditObject(
                                response,
                                revision,
                                current.x,
                                current.y,
                                current.z,
                                current.rotationX,
                                current.rotationY,
                                current.rotationZ
                            )
                        }
                    }
                    val error = responseResult.exceptionOrNull()?.message
                        ?: responseResult.getOrNull()?.let { raw ->
                            runCatching { JSONObject(raw).optString("error") }
                                .getOrDefault("respons edit native tidak dapat dibaca")
                        }.orEmpty()
                    if (error.isNotBlank()) {
                        status.text = "EDIT: $error"
                        return@withLock
                    }
                    if (response == RESPONSE_UPDATE) {
                        status.text = "UPDATE edit dikirim; server tetap menentukan hasil transform."
                    } else {
                        terminalQueued = true
                        setEditorEnabled(false)
                        status.text = if (response == RESPONSE_FINAL) {
                            "FINAL edit dikirim reliable; menunggu server."
                        } else {
                            "CANCEL edit dikirim reliable; menunggu server."
                        }
                        if (closeOnSuccess && dialog.isShowing) dialog.dismiss()
                    }
                }
            }
        }

        fun nudge(positionAxis: Int, delta: Float) {
            val current = working ?: return
            val next = when (positionAxis) {
                0 -> current.copy(x = (current.x + delta).coerceIn(-19_999f, 19_999f))
                1 -> current.copy(y = (current.y + delta).coerceIn(-19_999f, 19_999f))
                2 -> current.copy(z = (current.z + delta).coerceIn(-999f, 199_999f))
                3 -> current.copy(rotationX = (current.rotationX + delta).coerceIn(-360_000f, 360_000f))
                4 -> current.copy(rotationY = (current.rotationY + delta).coerceIn(-360_000f, 360_000f))
                else -> current.copy(rotationZ = (current.rotationZ + delta).coerceIn(-360_000f, 360_000f))
            }
            working = next
            refreshValues()
            send(RESPONSE_UPDATE)
        }

        fun addAxisRow(label: String, axis: Int, rotation: Boolean) {
            val row = LinearLayout(activity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
            }
            val title = TextView(activity).apply {
                text = label
                textSize = 11f
                setTextColor(Color.rgb(195, 229, 241))
                gravity = Gravity.CENTER_VERTICAL
            }
            row.addView(title, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.75f))
            fun addNudgeButton(text: String, sign: Float) {
                val button = Button(activity).apply {
                    this.text = text
                    textSize = 11f
                    contentDescription = "$label ${if (sign < 0f) "kurang" else "tambah"}"
                    setOnClickListener {
                        val amount = if (rotation) {
                            if (fineStep) 5f else 15f
                        } else {
                            if (fineStep) 0.10f else 1f
                        }
                        nudge(axis, sign * amount)
                    }
                }
                editorButtons += button
                row.addView(button, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                    marginStart = (2f * density).toInt()
                    marginEnd = (2f * density).toInt()
                })
            }
            addNudgeButton("−", -1f)
            addNudgeButton("+", 1f)
            axes.addView(row)
        }
        addAxisRow("POS X", 0, rotation = false)
        addAxisRow("POS Y", 1, rotation = false)
        addAxisRow("POS Z", 2, rotation = false)
        addAxisRow("ROT X", 3, rotation = true)
        addAxisRow("ROT Y", 4, rotation = true)
        addAxisRow("ROT Z", 5, rotation = true)

        lateinit var stepButton: Button
        stepButton = Button(activity).apply {
            text = "LANGKAH HALUS · 0.10m / 5°"
            textSize = 11f
            contentDescription = "Ubah ukuran langkah edit objek"
            setOnClickListener {
                fineStep = !fineStep
                text = if (fineStep) "LANGKAH HALUS · 0.10m / 5°" else "LANGKAH BESAR · 1.00m / 15°"
            }
        }
        editorButtons += stepButton
        val resetButton = Button(activity).apply {
            text = "RESET LOKAL"
            textSize = 11f
            contentDescription = "Kembalikan transform edit ke snapshot server saat dialog dibuka"
            setOnClickListener {
                working = baseline
                refreshValues()
                status.text = "Nilai lokal dikembalikan ke snapshot server; belum mengirim paket."
            }
        }
        editorButtons += resetButton
        val finalButton = Button(activity).apply {
            text = "FINAL"
            textSize = 11f
            contentDescription = "Kirim hasil akhir edit objek ke server"
            setOnClickListener { send(RESPONSE_FINAL, closeOnSuccess = true) }
        }
        editorButtons += finalButton
        val cancelButton = Button(activity).apply {
            text = "CANCEL"
            textSize = 11f
            contentDescription = "Batalkan edit objek ke server"
            setOnClickListener { send(RESPONSE_CANCEL, closeOnSuccess = true) }
        }
        editorButtons += cancelButton
        actionRow.addView(resetButton, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        actionRow.addView(finalButton, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        actionRow.addView(cancelButton, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

        root.addView(help)
        root.addView(target)
        root.addView(values)
        root.addView(axes)
        root.addView(stepButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ))
        root.addView(actionRow)
        root.addView(status)

        dialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Edit objek server")
            .setView(root)
            .setNegativeButton("CANCEL SERVER", null)
            .create()
        // A passive back/outside dismiss would strand the server's edit mode.
        // Users explicitly submit FINAL/CANCEL while a resolveable revision is
        // active; a waiting/ended mode may still be safely closed.
        dialog.setCanceledOnTouchOutside(false)
        dialog.setCancelable(false)
        dialog.setOnShowListener {
            setEditorEnabled(false)
            dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_NEGATIVE).setOnClickListener {
                if (!modeActive || working == null || terminalQueued) {
                    dialog.dismiss()
                } else {
                    send(RESPONSE_CANCEL, closeOnSuccess = true)
                }
            }
            pollJob = activity.lifecycleScope.launch {
                while (isActive && dialog.isShowing) {
                    val frame = runCatching {
                        val raw = withContext(Dispatchers.IO) { NativeBridge.nativeSessionWorldSnapshot() }
                        WorldFrame.from(JSONObject(raw))
                    }.getOrElse { WorldFrame(error = it.message ?: "snapshot edit objek gagal") }
                    val edit = frame.objectEdit
                    if (!edit.active) {
                        modeActive = false
                        target.text = "Mode edit objek server tidak aktif."
                        if (!terminalQueued) status.text = frame.error.ifBlank {
                            "Tunggu server membuka PlayerBeginObjectEdit (RPC 117)."
                        }
                        setEditorEnabled(false)
                    } else {
                        modeActive = true
                        // The active object baseline is copied separately by
                        // native, so a player object or an ID beyond the
                        // renderer's 128-item frame cap cannot accidentally
                        // inherit an unrelated/list-missing transform.
                        val sourceTransform = edit.toEditableTransform()
                        if (edit.revision != activeRevision || (working == null && sourceTransform != null)) {
                            activeRevision = edit.revision
                            baseline = sourceTransform
                            working = sourceTransform
                            terminalQueued = false
                        }
                        target.text = "Target #${edit.id} · ${if (edit.playerObject) "PLAYER OBJECT" else "GLOBAL OBJECT"} " +
                            "· revisi ${edit.revision}"
                        if (working == null) {
                            status.text = "Objek #${edit.id} belum resolve dari snapshot; native akan menolak respons sampai tersedia."
                        } else if (!terminalQueued) {
                            status.text = "Pilih −/+ untuk UPDATE bounded; FINAL mengakhiri respons Android untuk revisi ini."
                        }
                        refreshValues()
                        setEditorEnabled(working != null && !terminalQueued)
                    }
                    delay(100)
                }
            }
        }
        dialog.setOnDismissListener { pollJob?.cancel() }
        dialog.show()
    }

    private fun WorldObjectEdit.toEditableTransform(): Transform? {
        if (!transformAvailable) return null
        val values = floatArrayOf(x, y, z, rotationX, rotationY, rotationZ)
        if (values.any { !it.isFinite() }) return null
        if (x <= -20_000f || x >= 20_000f || y <= -20_000f || y >= 20_000f ||
            z <= -1_000f || z >= 200_000f ||
            abs(rotationX) > 360_000f || abs(rotationY) > 360_000f || abs(rotationZ) > 360_000f
        ) return null
        return Transform(x, y, z, rotationX, rotationY, rotationZ)
    }

    private fun format(value: Float): String = "%.2f".format(java.util.Locale.US, value)

}
