package com.xykal.sampdroid.ui

import android.graphics.Color
import android.view.ViewGroup
import android.widget.Button
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
 * Android presentation for source-verified legacy menus (RPC 76/77/78).
 * It receives only immutable decoded rows and can return only a current row or
 * exit against native's server-issued revision; Android never writes menu wire
 * bytes, guesses a menu ID, or treats a row as local gameplay state.
 */
internal object NativeServerMenuController {

    private data class MenuKey(val revision: Long, val id: Int)

    private var visible: AlertDialog? = null
    private var visibleKey: MenuKey? = null
    private var suppressedKey: MenuKey? = null
    private val serverDismissKeys = mutableSetOf<MenuKey>()
    private var dismissingForServer = false

    fun clear() {
        dismissForServer()
        suppressedKey = null
    }

    fun submit(activity: AppCompatActivity, frame: WorldFrame) {
        val menu = frame.menu
        // Avoid presenting two server-owned modal surfaces at once. A visible
        // dialog has precedence; the active menu remains native-owned and can
        // safely be shown again after the dialog is closed or replaced.
        if (frame.dialog.active || !menu.active || menu.revision <= 0L) {
            dismissForServer()
            if (!menu.active) suppressedKey = null
            return
        }
        val key = MenuKey(menu.revision, menu.id)
        if (key == suppressedKey) return
        if (visible?.isShowing == true && visibleKey == key) return
        dismissForServer()
        show(activity, menu, key)
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

    private fun show(activity: AppCompatActivity, menu: WorldMenu, key: MenuKey) {
        var responded = false
        lateinit var dialog: AlertDialog

        fun queueSelection(row: Int) {
            if (responded || dismissingForServer || row !in 0..11 || !menu.enabled ||
                !menu.rowEnabled.getOrElse(row) { false }
            ) return
            responded = true
            suppressedKey = key
            activity.lifecycleScope.launch {
                val error = runCatching {
                    val raw = withContext(Dispatchers.IO) {
                        NativeBridge.nativeSessionSelectMenu(menu.id, menu.revision, row)
                    }
                    JSONObject(raw).optString("error")
                }.getOrElse { it.message ?: "pilihan menu gagal" }
                if (error.isNotBlank()) {
                    if (suppressedKey == key) suppressedKey = null
                    Toast.makeText(activity, "Pilihan menu belum terkirim: $error", Toast.LENGTH_SHORT).show()
                }
            }
        }

        fun queueExit() {
            if (responded || dismissingForServer) return
            responded = true
            suppressedKey = key
            activity.lifecycleScope.launch {
                val error = runCatching {
                    val raw = withContext(Dispatchers.IO) {
                        NativeBridge.nativeSessionExitMenu(menu.id, menu.revision)
                    }
                    JSONObject(raw).optString("error")
                }.getOrElse { it.message ?: "keluar menu gagal" }
                if (error.isNotBlank()) {
                    if (suppressedKey == key) suppressedKey = null
                    Toast.makeText(activity, "Keluar menu belum terkirim: $error", Toast.LENGTH_SHORT).show()
                }
            }
        }

        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            val padding = (16f * resources.displayMetrics.density).toInt()
            setPadding(padding, 0, padding, 0)
        }
        val title = menu.title.ifBlank { "Menu server" }
        val columns = maxOf(menu.column1.size, menu.column2.size).coerceAtMost(12)
        if (columns == 0) {
            root.addView(TextView(activity).apply {
                text = "Menu server tidak memiliki baris yang dapat dipilih."
                textSize = 15f
                setTextColor(Color.rgb(229, 237, 241))
                setPadding(0, dp(activity, 12f), 0, dp(activity, 12f))
            })
        } else {
            if (menu.column1Header.isNotBlank() || (menu.twoColumns && menu.column2Header.isNotBlank())) {
                root.addView(TextView(activity).apply {
                    text = buildString {
                        append(menu.column1Header.ifBlank { "Pilihan" })
                        if (menu.twoColumns) append("     ").append(menu.column2Header)
                    }
                    textSize = 13f
                    setTextColor(Color.rgb(130, 211, 240))
                    setPadding(0, dp(activity, 8f), 0, dp(activity, 4f))
                })
            }
            for (row in 0 until columns) {
                val left = menu.column1.getOrElse(row) { "" }
                val right = if (menu.twoColumns) menu.column2.getOrElse(row) { "" } else ""
                val selectable = menu.enabled && menu.rowEnabled.getOrElse(row) { false }
                root.addView(Button(activity).apply {
                    text = if (menu.twoColumns) "$left     $right" else left
                    isEnabled = selectable
                    contentDescription = if (selectable) {
                        "Pilih baris ${row + 1} menu server: $left"
                    } else {
                        "Baris ${row + 1} menu server tidak aktif: $left"
                    }
                    setOnClickListener {
                        queueSelection(row)
                        dialog.dismiss()
                    }
                }, LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                ))
            }
        }

        val scroll = ScrollView(activity).apply { addView(root) }
        dialog = MaterialAlertDialogBuilder(activity)
            .setTitle(title.take(31))
            .setView(scroll)
            .setNegativeButton("KELUAR", null)
            .setCancelable(true)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener {
                queueExit()
                dialog.dismiss()
            }
        }
        dialog.setOnCancelListener { queueExit() }
        dialog.setOnDismissListener {
            val closedByServer = serverDismissKeys.remove(key) || dismissingForServer
            if (!responded && !closedByServer) queueExit()
            if (visible === dialog) {
                visible = null
                visibleKey = null
            }
        }
        visible = dialog
        visibleKey = key
        dialog.show()
    }

    private fun dp(activity: AppCompatActivity, value: Float): Int =
        (value * activity.resources.displayMetrics.density).toInt()
}
