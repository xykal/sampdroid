package com.xykal.sampdroid.ui

import android.graphics.Color
import android.text.InputFilter
import android.text.InputType
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.xykal.sampdroid.NativeBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * Ordered native chat/command entry. A leading slash is classified and encoded
 * by C++, so Android never writes RPC bytes or bypasses session state.
 */
object NativeChatDialog {

    /**
     * [initialText] is a local keyboard convenience only (for example "/" from
     * a physical slash shortcut). C++ still validates the full submitted line,
     * classifies commands, and owns the ordered native-session send.
     */
    fun show(activity: AppCompatActivity, initialText: String = "") {
        val density = activity.resources.displayMetrics.density
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((18f * density).toInt(), (8f * density).toInt(), (18f * density).toInt(), 0)
        }
        val help = TextView(activity).apply {
            text = "CHAT NATIVE\nKetik pesan biasa atau command berawalan / seperti /login. " +
                "Dikirim ordered dari socket native; text tidak masuk log diagnostik."
            textSize = 12f
            setTextColor(Color.rgb(212, 231, 240))
        }
        val input = EditText(activity).apply {
            hint = "Pesan atau /command"
            setSingleLine(true)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
            imeOptions = EditorInfo.IME_ACTION_SEND
            filters = arrayOf(InputFilter.LengthFilter(256))
            setText(initialText.take(256))
            setSelection(text.length)
            contentDescription = "Pesan chat atau command native SampDroid"
        }
        val status = TextView(activity).apply {
            text = ""
            textSize = 11f
            setTextColor(Color.rgb(255, 202, 96))
            setPadding(0, (6f * density).toInt(), 0, (4f * density).toInt())
        }
        root.addView(help)
        root.addView(input, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = (8f * density).toInt() })
        root.addView(status)

        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Chat native")
            .setView(root)
            .setNegativeButton("Batal", null)
            .setPositiveButton("Kirim", null)
            .create()
        dialog.setOnShowListener {
            var sending = false
            fun setSending(next: Boolean) {
                sending = next
                input.isEnabled = !next
                dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_POSITIVE).isEnabled = !next
            }
            fun submit() {
                if (sending) return
                val message = input.text.toString().trim()
                if (message.isEmpty()) {
                    status.text = "Tulis pesan atau command dulu."
                    return
                }
                setSending(true)
                status.text = "Mengirim lewat sesi native…"
                activity.lifecycleScope.launch {
                    val rawResponse = runCatching {
                        withContext(Dispatchers.IO) { NativeBridge.nativeSessionSendChat(message) }
                    }.getOrElse {
                        if (dialog.isShowing) {
                            setSending(false)
                            status.text = it.message ?: "Chat native gagal dikirim."
                        }
                        return@launch
                    }
                    if (!dialog.isShowing) return@launch
                    val snapshot = runCatching { JSONObject(rawResponse) }.getOrNull()
                    val error = snapshot?.optString("error").orEmpty()
                    if (snapshot == null) {
                        setSending(false)
                        status.text = "Balasan chat native tidak terbaca."
                    } else if (!snapshot.optBoolean("active") || error.isNotBlank()) {
                        setSending(false)
                        status.text = error.ifBlank { "Sesi native belum aktif." }
                    } else {
                        dialog.dismiss()
                    }
                }
            }
            dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_POSITIVE).setOnClickListener { submit() }
            input.setOnEditorActionListener { _, actionId, _ ->
                if (actionId == EditorInfo.IME_ACTION_SEND) {
                    submit()
                    true
                } else {
                    false
                }
            }
            input.requestFocus()
        }
        dialog.show()
    }
}
