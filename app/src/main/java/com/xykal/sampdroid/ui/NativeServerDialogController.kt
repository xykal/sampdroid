package com.xykal.sampdroid.ui

import android.text.InputType
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.xykal.sampdroid.NativeBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * Presents a bounded, native-decoded ShowDialog state as ordinary Android UI.
 * It never parses packet bytes itself: WorldState owns decompression, limits,
 * revisioning, and the matching-response check. Calls are idempotent so both
 * the retained session view and the world renderers may submit the same frame.
 */
internal object NativeServerDialogController {

    private data class DialogKey(val revision: Long, val id: Int)

    private var visible: AlertDialog? = null
    private var visibleKey: DialogKey? = null
    private var suppressedKey: DialogKey? = null
    private val serverDismissKeys = mutableSetOf<DialogKey>()
    private var dismissingForServer = false

    fun clear() {
        dismissForServer()
        suppressedKey = null
    }

    fun submit(activity: AppCompatActivity, frame: WorldFrame) {
        val serverDialog = frame.dialog
        if (!serverDialog.active) {
            dismissForServer()
            suppressedKey = null
            return
        }
        val key = DialogKey(serverDialog.revision, serverDialog.id)
        if (key == suppressedKey) return
        if (visible?.isShowing == true && visibleKey == key) return
        dismissForServer()
        show(activity, serverDialog, key)
    }

    private fun dismissForServer() {
        val current = visible
        if (current != null && current.isShowing) {
            visibleKey?.let { serverDismissKeys += it }
            dismissingForServer = true
            current.dismiss()
            dismissingForServer = false
        }
        visible = null
        visibleKey = null
    }

    private fun show(activity: AppCompatActivity, serverDialog: WorldDialog, key: DialogKey) {
        var responded = false
        var selectedItem = -1
        var input: EditText? = null
        val sourceRows = serverDialog.body.replace("\r", "").split('\n')
        val tabHeader = if (serverDialog.style == STYLE_TABLIST_HEADERS) sourceRows.firstOrNull().orEmpty() else ""
        val responseRows = if (serverDialog.style in LIST_STYLES) {
            if (serverDialog.style == STYLE_TABLIST_HEADERS) sourceRows.drop(1) else sourceRows
        } else {
            emptyList()
        }
        val rows = responseRows.map { row ->
            // A generated Android list has no SA-MP tab columns, but keeping
            // fields legible is better than exposing raw tab control glyphs.
            // Keep responseRows intact: SA-MP-compatible servers may compare
            // the selected tab-separated inputText in addition to listItem.
            row.replace('\t', ' ').ifBlank { " " }
        }
        if (rows.isNotEmpty()) selectedItem = 0

        fun queueResponse(accepted: Boolean) {
            if (responded || dismissingForServer) return
            responded = true
            suppressedKey = key
            val responseText = when {
                accepted && input != null -> input?.text?.toString().orEmpty()
                accepted && serverDialog.style in LIST_STYLES && selectedItem in responseRows.indices ->
                    responseRows[selectedItem]
                else -> ""
            }
            activity.lifecycleScope.launch {
                val error = runCatching {
                    val raw = withContext(Dispatchers.IO) {
                        NativeBridge.nativeSessionRespondDialog(
                            serverDialog.id,
                            serverDialog.revision,
                            accepted,
                            if (accepted) selectedItem else -1,
                            responseText
                        )
                    }
                    JSONObject(raw).optString("error")
                }.getOrElse { it.message ?: "dialog response gagal" }
                if (error.isNotBlank()) {
                    // The native reducer kept the dialog open when the packet
                    // was not queued, so let the next world frame present it again.
                    if (suppressedKey == key) suppressedKey = null
                    Toast.makeText(activity, "Dialog belum terkirim: $error", Toast.LENGTH_SHORT).show()
                }
            }
        }

        val dialogTitle = serverDialog.title.ifBlank { "Dialog server" }.take(MAX_TITLE_CHARS)
        val builder = MaterialAlertDialogBuilder(activity)
            .setTitle(if (tabHeader.isBlank()) dialogTitle else "$dialogTitle\n${tabHeader.replace('\t', ' ').take(MAX_TITLE_CHARS)}")
            .setCancelable(true)

        when {
            serverDialog.style in LIST_STYLES -> {
                if (rows.isEmpty()) {
                    builder.setView(messageScrollView(activity, "Tidak ada pilihan dari server."))
                } else {
                    builder.setSingleChoiceItems(rows.toTypedArray(), 0) { _, which ->
                        selectedItem = which
                    }
                }
            }
            serverDialog.style == STYLE_INPUT || serverDialog.style == STYLE_PASSWORD -> {
                val content = LinearLayout(activity).apply {
                    orientation = LinearLayout.VERTICAL
                    val pad = (20f * resources.displayMetrics.density).toInt()
                    setPadding(pad, 0, pad, 0)
                }
                if (serverDialog.body.isNotBlank()) content.addView(messageView(activity, serverDialog.body))
                input = EditText(activity).apply {
                    maxLines = 1
                    inputType = if (serverDialog.style == STYLE_PASSWORD) {
                        InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
                    } else {
                        InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
                    }
                    contentDescription = if (serverDialog.style == STYLE_PASSWORD) {
                        "Input password untuk dialog server"
                    } else {
                        "Input untuk dialog server"
                    }
                }
                content.addView(input, LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                ))
                builder.setView(content)
            }
            else -> builder.setView(messageScrollView(activity, serverDialog.body))
        }

        builder.setPositiveButton(serverDialog.button1.ifBlank { "OK" }) { _, _ -> queueResponse(true) }
        if (serverDialog.button2.isNotBlank()) {
            builder.setNegativeButton(serverDialog.button2) { _, _ -> queueResponse(false) }
        }
        val dialog = builder.create()
        dialog.setOnCancelListener { queueResponse(false) }
        dialog.setOnDismissListener {
            val closedByServer = serverDismissKeys.remove(key) || dismissingForServer
            if (!responded && !closedByServer) queueResponse(false)
            if (visible === dialog) {
                visible = null
                visibleKey = null
            }
        }
        visible = dialog
        visibleKey = key
        dialog.show()
    }

    private fun messageScrollView(activity: AppCompatActivity, body: String): ScrollView =
        ScrollView(activity).apply {
            val pad = (8f * resources.displayMetrics.density).toInt()
            setPadding(pad, 0, pad, 0)
            addView(messageView(activity, body))
        }

    private fun messageView(activity: AppCompatActivity, body: String): TextView =
        TextView(activity).apply {
            text = body.take(MAX_BODY_CHARS)
            textSize = 15f
            setTextIsSelectable(true)
            val vertical = (10f * resources.displayMetrics.density).toInt()
            setPadding(0, vertical, 0, vertical)
        }

    private const val STYLE_INPUT = 1
    private const val STYLE_PASSWORD = 3
    private const val STYLE_TABLIST_HEADERS = 5
    private val LIST_STYLES = setOf(2, 4, STYLE_TABLIST_HEADERS)
    private const val MAX_TITLE_CHARS = 64
    private const val MAX_BODY_CHARS = 1024
}
