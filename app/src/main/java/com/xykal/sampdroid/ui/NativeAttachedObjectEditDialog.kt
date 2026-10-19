package com.xykal.sampdroid.ui

import android.graphics.Color
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
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject
import kotlin.math.abs

/**
 * Touch fallback for server PlayerBeginAttachedObjectEdit (RPC 116).
 *
 * The renderer's bone/world pose is deliberately never reverse-engineered.
 * This dialog sees only native-copied attachment-local offset/rotation/scale
 * and asks C++ to reattach the active slot's model, bone, and colours while
 * sending one server-compatible save/cancel response.
 */
internal object NativeAttachedObjectEditDialog {
    private const val RESPONSE_CANCEL = 0
    private const val RESPONSE_SAVE = 1

    private data class Transform(
        val offsetX: Float,
        val offsetY: Float,
        val offsetZ: Float,
        val rotationX: Float,
        val rotationY: Float,
        val rotationZ: Float,
        val scaleX: Float,
        val scaleY: Float,
        val scaleZ: Float
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
        val outboundMutex = Mutex()
        val editorButtons = mutableListOf<Button>()

        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((14f * density).toInt(), (8f * density).toInt(), (14f * density).toInt(), 0)
        }
        val help = TextView(activity).apply {
            text = "EDIT ATTACHMENT NATIVE\nServer membuka RPC 116. Ubah offset, rotasi, atau scale lokal; " +
                "slot/model/bone/warna tetap berasal dari native dan server."
            textSize = 12f
            setTextColor(Color.rgb(215, 237, 247))
        }
        val target = TextView(activity).apply {
            text = "Menunggu mode edit attachment dari server…"
            textSize = 12f
            setTextColor(Color.rgb(185, 246, 202))
            setPadding(0, (8f * density).toInt(), 0, 0)
        }
        val values = TextView(activity).apply {
            text = "Transform attachment belum tersedia."
            textSize = 11f
            setTextColor(Color.rgb(225, 232, 238))
            setPadding(0, (4f * density).toInt(), 0, 0)
        }
        val axes = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, (6f * density).toInt(), 0, 0)
        }
        val status = TextView(activity).apply {
            text = "SAVE/CANCEL mengirim satu respons reliable; server tetap pemilik slot attachment."
            textSize = 11f
            setTextColor(Color.rgb(255, 202, 96))
            setPadding(0, (7f * density).toInt(), 0, 0)
        }
        val actionRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, (7f * density).toInt(), 0, 0)
        }

        fun labelFor(value: Transform?): String = if (value == null) {
            "Transform attachment belum tersedia."
        } else {
            "OFFSET  X ${format(value.offsetX)}  Y ${format(value.offsetY)}  Z ${format(value.offsetZ)}\n" +
                "ROT     X ${format(value.rotationX)}°  Y ${format(value.rotationY)}°  Z ${format(value.rotationZ)}°\n" +
                "SCALE   X ${format(value.scaleX)}  Y ${format(value.scaleY)}  Z ${format(value.scaleZ)}"
        }
        fun refreshValues() {
            values.text = labelFor(working)
        }
        fun setEditorEnabled(enabled: Boolean) {
            editorButtons.forEach { it.isEnabled = enabled }
        }

        lateinit var dialog: androidx.appcompat.app.AlertDialog
        fun send(response: Int, closeOnSuccess: Boolean) {
            val current = working
            // Revision is copied, not caller-selected identity. Native compares
            // it under the session lock to reject delayed taps from an old slot
            // lifecycle before they can affect a newly opened server editor.
            val revision = activeRevision
            if (current == null || revision <= 0L) {
                status.text = "Attachment belum resolve dari state server."
                return
            }
            activity.lifecycleScope.launch {
                outboundMutex.withLock {
                    val result = runCatching {
                        withContext(Dispatchers.IO) {
                            NativeBridge.nativeSessionEditAttachedObject(
                                response,
                                revision,
                                current.offsetX, current.offsetY, current.offsetZ,
                                current.rotationX, current.rotationY, current.rotationZ,
                                current.scaleX, current.scaleY, current.scaleZ
                            )
                        }
                    }
                    val error = result.exceptionOrNull()?.message
                        ?: result.getOrNull()?.let { raw ->
                            runCatching { JSONObject(raw).optString("error") }
                                .getOrDefault("respons edit attachment tidak dapat dibaca")
                        }.orEmpty()
                    if (error.isNotBlank()) {
                        status.text = "ATTACHMENT: $error"
                        return@withLock
                    }
                    terminalQueued = true
                    setEditorEnabled(false)
                    status.text = if (response == RESPONSE_SAVE) {
                        "SAVE attachment dikirim reliable; menunggu server."
                    } else {
                        "CANCEL attachment dikirim reliable; menunggu server."
                    }
                    if (closeOnSuccess && dialog.isShowing) dialog.dismiss()
                }
            }
        }

        fun safeScale(value: Float): Float {
            val sign = if (value < 0f) -1f else 1f
            return if (abs(value) < 0.001f) sign * 0.001f else value.coerceIn(-100f, 100f)
        }
        fun nudge(axis: Int, delta: Float) {
            val current = working ?: return
            working = when (axis) {
                0 -> current.copy(offsetX = (current.offsetX + delta).coerceIn(-10_000f, 10_000f))
                1 -> current.copy(offsetY = (current.offsetY + delta).coerceIn(-10_000f, 10_000f))
                2 -> current.copy(offsetZ = (current.offsetZ + delta).coerceIn(-10_000f, 10_000f))
                3 -> current.copy(rotationX = (current.rotationX + delta).coerceIn(-360_000f, 360_000f))
                4 -> current.copy(rotationY = (current.rotationY + delta).coerceIn(-360_000f, 360_000f))
                5 -> current.copy(rotationZ = (current.rotationZ + delta).coerceIn(-360_000f, 360_000f))
                6 -> current.copy(scaleX = safeScale(current.scaleX + delta))
                7 -> current.copy(scaleY = safeScale(current.scaleY + delta))
                else -> current.copy(scaleZ = safeScale(current.scaleZ + delta))
            }
            refreshValues()
        }

        fun addAxisRow(label: String, axis: Int, kind: Int) {
            val row = LinearLayout(activity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
            }
            row.addView(TextView(activity).apply {
                text = label
                textSize = 11f
                setTextColor(Color.rgb(195, 229, 241))
                gravity = Gravity.CENTER_VERTICAL
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.85f))
            fun addButton(text: String, sign: Float) {
                val button = Button(activity).apply {
                    this.text = text
                    textSize = 11f
                    contentDescription = "$label ${if (sign < 0f) "kurang" else "tambah"}"
                    setOnClickListener {
                        val amount = when (kind) {
                            0 -> if (fineStep) 0.05f else 0.5f
                            1 -> if (fineStep) 5f else 15f
                            else -> if (fineStep) 0.05f else 0.2f
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
            addButton("−", -1f)
            addButton("+", 1f)
            axes.addView(row)
        }
        addAxisRow("OFFSET X", 0, 0)
        addAxisRow("OFFSET Y", 1, 0)
        addAxisRow("OFFSET Z", 2, 0)
        addAxisRow("ROT X", 3, 1)
        addAxisRow("ROT Y", 4, 1)
        addAxisRow("ROT Z", 5, 1)
        addAxisRow("SCALE X", 6, 2)
        addAxisRow("SCALE Y", 7, 2)
        addAxisRow("SCALE Z", 8, 2)

        val stepButton = Button(activity).apply {
            text = "LANGKAH HALUS · 0.05 / 5° / 0.05"
            textSize = 11f
            contentDescription = "Ubah ukuran langkah edit attachment"
            setOnClickListener {
                fineStep = !fineStep
                text = if (fineStep) "LANGKAH HALUS · 0.05 / 5° / 0.05" else
                    "LANGKAH BESAR · 0.50 / 15° / 0.20"
            }
        }
        editorButtons += stepButton
        val resetButton = Button(activity).apply {
            text = "RESET LOKAL"
            textSize = 11f
            setOnClickListener {
                working = baseline
                refreshValues()
                status.text = "Nilai lokal dikembalikan; belum mengirim paket."
            }
        }
        editorButtons += resetButton
        val saveButton = Button(activity).apply {
            text = "SAVE"
            textSize = 11f
            setOnClickListener { send(RESPONSE_SAVE, closeOnSuccess = true) }
        }
        editorButtons += saveButton
        val cancelButton = Button(activity).apply {
            text = "CANCEL"
            textSize = 11f
            setOnClickListener { send(RESPONSE_CANCEL, closeOnSuccess = true) }
        }
        editorButtons += cancelButton
        actionRow.addView(resetButton, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        actionRow.addView(saveButton, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        actionRow.addView(cancelButton, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

        content.addView(help)
        content.addView(target)
        content.addView(values)
        content.addView(axes)
        content.addView(stepButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ))
        content.addView(actionRow)
        content.addView(status)
        val scroll = ScrollView(activity).apply { addView(content) }

        dialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Edit attachment server")
            .setView(scroll)
            .setNegativeButton("CANCEL SERVER", null)
            .create()
        // Keep a resolveable server editor from being abandoned by Back/outside
        // dismissal. The explicit negative action sends CANCEL; a mode that
        // never resolved or has already ended can still close locally.
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
                    }.getOrElse { WorldFrame(error = it.message ?: "snapshot edit attachment gagal") }
                    val edit = frame.attachedObjectEdit
                    if (!edit.active) {
                        modeActive = false
                        target.text = "Mode edit attachment server tidak aktif atau slot belum resolve."
                        if (!terminalQueued) status.text = frame.error.ifBlank {
                            "Tunggu server membuka PlayerBeginAttachedObjectEdit (RPC 116)."
                        }
                        setEditorEnabled(false)
                    } else {
                        modeActive = true
                        val nativeTransform = if (edit.transformAvailable) Transform(
                            edit.offsetX, edit.offsetY, edit.offsetZ,
                            edit.rotationX, edit.rotationY, edit.rotationZ,
                            edit.scaleX, edit.scaleY, edit.scaleZ
                        ) else null
                        if (edit.revision != activeRevision) {
                            activeRevision = edit.revision
                            baseline = nativeTransform
                            working = nativeTransform
                            terminalQueued = false
                        } else if (working == null && nativeTransform != null) {
                            baseline = nativeTransform
                            working = nativeTransform
                        }
                        target.text = if (edit.transformAvailable) {
                            "Slot ${edit.slot} · model ${edit.model} · bone ${edit.bone} · revisi ${edit.revision}"
                        } else {
                            "Slot ${edit.slot} · revisi ${edit.revision} · attachment belum resolve native"
                        }
                        if (!terminalQueued) {
                            status.text = if (nativeTransform == null) {
                                "Slot attachment belum tersedia/valid; native menolak respons sampai resolve."
                            } else {
                                "Atur lokal lalu SAVE; tidak ada mutasi attachment optimistis di renderer."
                            }
                        }
                        refreshValues()
                        setEditorEnabled(nativeTransform != null && !terminalQueued)
                    }
                    delay(100)
                }
            }
        }
        dialog.setOnDismissListener { pollJob?.cancel() }
        dialog.show()
    }

    private fun format(value: Float): String = "%.2f".format(java.util.Locale.US, value)
}
