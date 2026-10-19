package com.xykal.sampdroid.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Canvas
import android.hardware.Sensor
import android.hardware.SensorManager
import android.graphics.Color
import android.graphics.Paint
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.max
import kotlin.math.min

/** A normalized (screen-relative) placement for one native touch control. */
internal data class NativeTouchPoint(val x: Float, val y: Float)

/**
 * Per-device native touch layout. Only presentation positions/scale live here;
 * action meaning stays compiled and must still pass the JNI/native input gate.
 */
internal data class NativeTouchLayout(
    val stick: NativeTouchPoint,
    val buttons: Map<Int, NativeTouchPoint>,
    val scale: Float,
    // Local-only presentation preference for low-vision/bright-light use.
    // It changes Canvas contrast only; input semantics and all native gates
    // remain exactly the same.
    val highContrast: Boolean = false,
    // Uses Android's physical gyroscope only as bounded *relative* look input.
    // Disabled by default and never supplies movement, a world position, target,
    // hit, or direct network packet.
    val gyroLookEnabled: Boolean = false
) {
    fun pointFor(action: Int): NativeTouchPoint = buttons[action] ?: DEFAULT_BUTTONS.getValue(action)

    fun withPoint(action: Int, point: NativeTouchPoint): NativeTouchLayout =
        if (action == MOVE_CONTROL) copy(stick = point) else copy(buttons = buttons + (action to point))

    fun withScale(next: Float): NativeTouchLayout = copy(scale = next.coerceIn(MIN_SCALE, MAX_SCALE))

    fun withHighContrast(enabled: Boolean): NativeTouchLayout = copy(highContrast = enabled)

    fun withGyroLook(enabled: Boolean): NativeTouchLayout = copy(gyroLookEnabled = enabled)

    /**
     * Compact thumb-side starting layout. It deliberately remains editable:
     * safe-area clamping still happens in the overlay, and this preset changes
     * only local visual/input placement—not semantic action meanings.
     */
    fun withOneHandedPreset(rightHanded: Boolean): NativeTouchLayout {
        val side = if (rightHanded) 1f else -1f
        fun x(rightSideX: Float): Float = if (side > 0f) rightSideX else 1f - rightSideX
        val compact = mapOf(
            NativeHeldAction.JUMP to NativeTouchPoint(x(0.90f), 0.76f),
            NativeHeldAction.SPRINT to NativeTouchPoint(x(0.60f), 0.88f),
            NativeHeldAction.FIRE to NativeTouchPoint(x(0.90f), 0.45f),
            NativeHeldAction.AIM to NativeTouchPoint(x(0.65f), 0.45f),
            NativeHeldAction.USE to NativeTouchPoint(x(0.59f), 0.66f),
            NativeHeldAction.CROUCH to NativeTouchPoint(x(0.84f), 0.62f)
        )
        return copy(stick = NativeTouchPoint(x(0.76f), 0.80f), buttons = compact)
    }

    companion object {
        const val MOVE_CONTROL = -1
        const val MIN_SCALE = 0.72f
        // A large-control ceiling remains bounded by the safe-area clamp while
        // giving low-vision users a usable in-scene option on phones/tablets.
        const val MAX_SCALE = 1.72f

        val DEFAULT_STICK = NativeTouchPoint(0.18f, 0.77f)
        val DEFAULT_BUTTONS: Map<Int, NativeTouchPoint> = linkedMapOf(
            NativeHeldAction.JUMP to NativeTouchPoint(0.84f, 0.74f),
            NativeHeldAction.SPRINT to NativeTouchPoint(0.69f, 0.86f),
            NativeHeldAction.FIRE to NativeTouchPoint(0.87f, 0.52f),
            NativeHeldAction.AIM to NativeTouchPoint(0.70f, 0.53f),
            NativeHeldAction.USE to NativeTouchPoint(0.54f, 0.87f),
            // Keep crouch reachable in the compact scene overlay too; it was
            // already available on keyboard/gamepad and the full controls UI.
            NativeHeldAction.CROUCH to NativeTouchPoint(0.54f, 0.70f)
        )

        fun defaults(): NativeTouchLayout = NativeTouchLayout(
            stick = DEFAULT_STICK,
            buttons = DEFAULT_BUTTONS,
            scale = 1f
        )
    }
}

/** Persisted local layout plus an explicit schema-compatible JSON export. */
internal object NativeTouchLayoutStore {
    private const val PREFS = "native_touch_layout_v1"
    private const val KEY_STICK_X = "stick_x"
    private const val KEY_STICK_Y = "stick_y"
    private const val KEY_SCALE = "scale"
    private const val KEY_HIGH_CONTRAST = "high_contrast"
    private const val KEY_GYRO_LOOK = "gyro_look"

    fun load(context: Context): NativeTouchLayout {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val defaults = NativeTouchLayout.defaults()
        val buttons = defaults.buttons.mapValues { (action, point) ->
            val prefix = keyPrefix(action)
            NativeTouchPoint(
                prefs.getFloat("${prefix}_x", point.x).coerceIn(MIN_COORDINATE, MAX_COORDINATE),
                prefs.getFloat("${prefix}_y", point.y).coerceIn(MIN_COORDINATE, MAX_COORDINATE)
            )
        }
        return NativeTouchLayout(
            stick = NativeTouchPoint(
                prefs.getFloat(KEY_STICK_X, defaults.stick.x).coerceIn(MIN_COORDINATE, MAX_COORDINATE),
                prefs.getFloat(KEY_STICK_Y, defaults.stick.y).coerceIn(MIN_COORDINATE, MAX_COORDINATE)
            ),
            buttons = buttons,
            scale = prefs.getFloat(KEY_SCALE, defaults.scale).coerceIn(
                NativeTouchLayout.MIN_SCALE,
                NativeTouchLayout.MAX_SCALE
            ),
            highContrast = prefs.getBoolean(KEY_HIGH_CONTRAST, defaults.highContrast),
            gyroLookEnabled = prefs.getBoolean(KEY_GYRO_LOOK, defaults.gyroLookEnabled)
        )
    }

    fun save(context: Context, layout: NativeTouchLayout) {
        val editor = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
        editor.putFloat(KEY_STICK_X, layout.stick.x.coerceIn(MIN_COORDINATE, MAX_COORDINATE))
        editor.putFloat(KEY_STICK_Y, layout.stick.y.coerceIn(MIN_COORDINATE, MAX_COORDINATE))
        editor.putFloat(KEY_SCALE, layout.scale.coerceIn(NativeTouchLayout.MIN_SCALE, NativeTouchLayout.MAX_SCALE))
        editor.putBoolean(KEY_HIGH_CONTRAST, layout.highContrast)
        editor.putBoolean(KEY_GYRO_LOOK, layout.gyroLookEnabled)
        NativeTouchLayout.DEFAULT_BUTTONS.forEach { (action, _) ->
            val point = layout.pointFor(action)
            val prefix = keyPrefix(action)
            editor.putFloat("${prefix}_x", point.x.coerceIn(MIN_COORDINATE, MAX_COORDINATE))
            editor.putFloat("${prefix}_y", point.y.coerceIn(MIN_COORDINATE, MAX_COORDINATE))
        }
        editor.apply()
    }

    fun reset(context: Context) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().clear().apply()
    }

    /**
     * Exports a data-only subset using the documented v1 schema. Import is not
     * executed automatically; users/contributors can inspect/copy the text and
     * no external script, binary, or PC asset is ever loaded from it.
     */
    fun exportJson(layout: NativeTouchLayout): String {
        val controls = JSONArray()
        controls.put(controlJson("move", "stick", "move", layout.stick, 0.18f * layout.scale, "MOVE"))
        NativeTouchLayout.DEFAULT_BUTTONS.forEach { (action, _) ->
            val spec = buttonSpec(action)
            controls.put(
                controlJson(
                    id = spec.id,
                    kind = if (action == NativeHeldAction.FIRE || action == NativeHeldAction.AIM) "trigger" else "button",
                    action = spec.action,
                    point = layout.pointFor(action),
                    size = 0.115f * layout.scale,
                    label = spec.label
                ).put("hold", action != NativeHeldAction.JUMP && action != NativeHeldAction.USE)
            )
        }
        return JSONObject()
            .put("schema", 1)
            .put("id", "org.sampdroid.local-touch-layout")
            .put("name", "SampDroid local touch layout")
            .put("orientation", "any")
            .put("safeArea", true)
            .put("highContrast", layout.highContrast)
            .put("gyroLook", layout.gyroLookEnabled)
            .put("controls", controls)
            .toString(2)
    }

    private fun controlJson(
        id: String,
        kind: String,
        action: String,
        point: NativeTouchPoint,
        size: Float,
        label: String
    ): JSONObject = JSONObject()
        .put("id", id)
        .put("kind", kind)
        .put("action", action)
        .put("anchor", "center")
        .put("x", point.x.coerceIn(MIN_COORDINATE, MAX_COORDINATE))
        .put("y", point.y.coerceIn(MIN_COORDINATE, MAX_COORDINATE))
        .put("size", size.coerceIn(0.04f, 0.5f))
        .put("opacity", 0.72f)
        .put("label", label)

    private fun keyPrefix(action: Int): String = "button_$action"

    private data class ButtonSpec(val id: String, val action: String, val label: String)

    private fun buttonSpec(action: Int): ButtonSpec = when (action) {
        NativeHeldAction.JUMP -> ButtonSpec("jump", "jump", "JUMP")
        NativeHeldAction.SPRINT -> ButtonSpec("sprint", "sprint", "RUN")
        NativeHeldAction.FIRE -> ButtonSpec("fire", "fire", "FIRE")
        NativeHeldAction.AIM -> ButtonSpec("aim", "aim", "AIM")
        NativeHeldAction.USE -> ButtonSpec("use", "enter-vehicle", "USE")
        NativeHeldAction.CROUCH -> ButtonSpec("crouch", "crouch", "DUCK")
        else -> ButtonSpec("action-$action", "action-$action", "ACTION")
    }

    private const val MIN_COORDINATE = 0.04f
    private const val MAX_COORDINATE = 0.96f
}

/** On-device drag editor for the compact in-scene touch overlay. */
internal object NativeTouchLayoutEditorDialog {
    fun show(activity: AppCompatActivity, onSaved: () -> Unit) {
        val density = activity.resources.displayMetrics.density
        var working = NativeTouchLayoutStore.load(activity)
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((14f * density).toInt(), (8f * density).toInt(), (14f * density).toInt(), 0)
        }
        val help = TextView(activity).apply {
            text = "LAYOUT TOUCH NATIVE\nGeser MOVE atau tombol ke posisi nyaman. " +
                "Ukuran hingga 172%, preset satu tangan, KONTRAS, dan GYRO LOOK opsional membantu perangkat berbeda; data hanya disimpan lokal di APK."
            textSize = 12f
            setTextColor(Color.rgb(215, 237, 247))
        }
        val editor = NativeTouchLayoutEditorView(activity, working) { next ->
            working = next
        }.apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                max((225f * density).toInt(), (activity.resources.displayMetrics.heightPixels * 0.34f).toInt())
            ).apply { topMargin = (8f * density).toInt() }
        }
        val scaleRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
        }
        var scaleStatus: TextView? = null
        fun scaleButton(label: String, delta: Float) {
            val button = TextView(activity).apply {
                text = label
                textSize = 12f
                gravity = android.view.Gravity.CENTER
                setTextColor(Color.rgb(224, 245, 252))
                setPadding((8f * density).toInt(), (7f * density).toInt(), (8f * density).toInt(), (7f * density).toInt())
                setBackgroundColor(Color.rgb(34, 83, 105))
                isClickable = true
                contentDescription = "$label ukuran kontrol touch"
                setOnClickListener {
                    working = working.withScale(working.scale + delta)
                    editor.setLayout(working)
                    scaleStatus?.text = scaleLabel(working.scale)
                }
            }
            scaleRow.addView(button, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = (2f * density).toInt()
                marginEnd = (2f * density).toInt()
            })
        }
        scaleButton("− KECIL", -0.10f)
        val initialScaleStatus = TextView(activity).apply {
            text = scaleLabel(working.scale)
            textSize = 12f
            gravity = android.view.Gravity.CENTER
            setTextColor(Color.rgb(185, 246, 202))
        }
        scaleStatus = initialScaleStatus
        scaleRow.addView(initialScaleStatus, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.85f))
        scaleButton("+ BESAR", 0.10f)
        fun contrastLabel(layout: NativeTouchLayout): String =
            if (layout.highContrast) "KONTRAS TINGGI: ON" else "KONTRAS TINGGI: OFF"
        val contrastToggle = TextView(activity).apply {
            text = contrastLabel(working)
            textSize = 12f
            gravity = android.view.Gravity.CENTER
            setTextColor(Color.rgb(255, 244, 180))
            setPadding((8f * density).toInt(), (8f * density).toInt(), (8f * density).toInt(), (8f * density).toInt())
            setBackgroundColor(Color.rgb(49, 75, 34))
            isClickable = true
            contentDescription = "Aktifkan atau matikan kontras tinggi kontrol touch"
            setOnClickListener {
                working = working.withHighContrast(!working.highContrast)
                editor.setLayout(working)
                text = contrastLabel(working)
            }
        }
        val gyroAvailable = (activity.getSystemService(Context.SENSOR_SERVICE) as? SensorManager)
            ?.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null
        fun gyroLabel(layout: NativeTouchLayout): String = when {
            !gyroAvailable -> "GYRO TIDAK TERSEDIA"
            layout.gyroLookEnabled -> "GYRO LOOK: ON"
            else -> "GYRO LOOK: OFF"
        }
        val gyroToggle = TextView(activity).apply {
            text = gyroLabel(working)
            textSize = 12f
            gravity = android.view.Gravity.CENTER
            setTextColor(Color.rgb(191, 229, 255))
            setPadding((8f * density).toInt(), (8f * density).toInt(), (8f * density).toInt(), (8f * density).toInt())
            setBackgroundColor(if (gyroAvailable) Color.rgb(31, 67, 98) else Color.rgb(67, 67, 67))
            isClickable = gyroAvailable
            isEnabled = gyroAvailable
            alpha = if (gyroAvailable) 1f else 0.62f
            contentDescription = if (gyroAvailable) {
                "Aktifkan atau matikan gyro look relatif"
            } else {
                "Perangkat ini tidak memiliki sensor gyroscope"
            }
            if (gyroAvailable) {
                setOnClickListener {
                    working = working.withGyroLook(!working.gyroLookEnabled)
                    editor.setLayout(working)
                    text = gyroLabel(working)
                }
            }
        }
        val oneHandRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER
        }
        fun addOneHandPreset(label: String, rightHanded: Boolean) {
            oneHandRow.addView(TextView(activity).apply {
                text = label
                textSize = 11f
                gravity = android.view.Gravity.CENTER
                setTextColor(Color.rgb(232, 244, 252))
                setPadding((6f * density).toInt(), (8f * density).toInt(), (6f * density).toInt(), (8f * density).toInt())
                setBackgroundColor(Color.rgb(55, 75, 102))
                isClickable = true
                contentDescription = "Gunakan preset kontrol satu tangan $label"
                setOnClickListener {
                    working = working.withOneHandedPreset(rightHanded)
                    editor.setLayout(working)
                }
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = (2f * density).toInt()
                marginEnd = (2f * density).toInt()
            })
        }
        addOneHandPreset("1 TANGAN KIRI", rightHanded = false)
        addOneHandPreset("1 TANGAN KANAN", rightHanded = true)
        val status = TextView(activity).apply {
            text = "Simpan untuk menerapkan. Reset memulihkan posisi bawaan."
            textSize = 11f
            setTextColor(Color.rgb(255, 202, 96))
            setPadding(0, (6f * density).toInt(), 0, 0)
        }
        root.addView(help)
        root.addView(editor)
        root.addView(scaleRow)
        root.addView(contrastToggle, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = (5f * density).toInt() })
        root.addView(gyroToggle, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = (5f * density).toInt() })
        root.addView(oneHandRow, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = (5f * density).toInt() })
        root.addView(status)

        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Atur kontrol touch")
            .setView(root)
            .setNegativeButton("Reset", null)
            .setNeutralButton("Salin JSON", null)
            .setPositiveButton("Simpan", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                NativeTouchLayoutStore.save(activity, working)
                onSaved()
                dialog.dismiss()
            }
            dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_NEGATIVE).setOnClickListener {
                working = NativeTouchLayout.defaults()
                editor.setLayout(working)
                scaleStatus?.text = scaleLabel(working.scale)
                contrastToggle.text = contrastLabel(working)
                gyroToggle.text = gyroLabel(working)
                status.text = "Bawaan dipratinjau; tekan Simpan untuk menerapkan."
            }
            dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
                val clipboard = activity.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
                clipboard?.setPrimaryClip(ClipData.newPlainText("SampDroid native touch layout", NativeTouchLayoutStore.exportJson(working)))
                status.text = if (clipboard == null) {
                    "Clipboard Android tidak tersedia di perangkat ini."
                } else {
                    "JSON layout native disalin ke clipboard; tidak ada script/asset eksternal."
                }
            }
        }
        dialog.show()
    }

    private fun scaleLabel(scale: Float): String = "${(scale * 100f).toInt()}%"
}

private class NativeTouchLayoutEditorView(
    context: Context,
    initial: NativeTouchLayout,
    private val onChanged: (NativeTouchLayout) -> Unit
) : View(context) {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private var layout = initial
    private var activeControl: Int? = null

    init {
        isClickable = true
        contentDescription = "Editor posisi kontrol touch SampDroid. Geser joystick MOVE atau tombol berlabel."
        setBackgroundColor(Color.rgb(11, 28, 38))
    }

    fun setLayout(next: NativeTouchLayout) {
        layout = next
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        if (layout.highContrast) canvas.drawColor(Color.BLACK)
        val radius = buttonRadius()
        drawControl(canvas, NativeTouchLayout.MOVE_CONTROL, "MOVE", layout.stick, radius * 1.26f,
            if (layout.highContrast) Color.rgb(0, 104, 174) else Color.rgb(62, 172, 213))
        NativeTouchLayout.DEFAULT_BUTTONS.forEach { (action, _) ->
            drawControl(canvas, action, labelFor(action), layout.pointFor(action), radius,
                if (layout.highContrast) Color.rgb(18, 76, 124) else Color.rgb(55, 105, 130))
        }
        paint.textAlign = Paint.Align.CENTER
        paint.textSize = dp(10f)
        paint.color = Color.rgb(172, 222, 239)
        canvas.drawText("Drag tiap kontrol · area kosong tetap kamera/look", width * 0.5f, dp(18f), paint)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                activeControl = hitControl(event.x, event.y)
                if (activeControl != null) {
                    updateActive(event.x, event.y)
                    return true
                }
                return false
            }
            MotionEvent.ACTION_MOVE -> {
                if (activeControl != null) {
                    updateActive(event.x, event.y)
                    return true
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (activeControl != null) {
                    activeControl = null
                    performClick()
                    return true
                }
            }
        }
        return activeControl != null
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    private fun hitControl(x: Float, y: Float): Int? {
        val candidates = ArrayList<Int>(NativeTouchLayout.DEFAULT_BUTTONS.size + 1)
        candidates.add(NativeTouchLayout.MOVE_CONTROL)
        candidates.addAll(NativeTouchLayout.DEFAULT_BUTTONS.keys)
        return candidates.minByOrNull { action ->
            val point = if (action == NativeTouchLayout.MOVE_CONTROL) layout.stick else layout.pointFor(action)
            val dx = x - point.x * width
            val dy = y - point.y * height
            dx * dx + dy * dy
        }?.takeIf { action ->
            val point = if (action == NativeTouchLayout.MOVE_CONTROL) layout.stick else layout.pointFor(action)
            val dx = x - point.x * width
            val dy = y - point.y * height
            Math.hypot(dx.toDouble(), dy.toDouble()).toFloat() <=
                if (action == NativeTouchLayout.MOVE_CONTROL) buttonRadius() * 1.45f else buttonRadius() * 1.25f
        }
    }

    private fun updateActive(x: Float, y: Float) {
        val action = activeControl ?: return
        if (width <= 0 || height <= 0) return
        val point = NativeTouchPoint(
            (x / width).coerceIn(EDITOR_MARGIN, 1f - EDITOR_MARGIN),
            (y / height).coerceIn(EDITOR_MARGIN, 1f - EDITOR_MARGIN)
        )
        layout = layout.withPoint(action, point)
        onChanged(layout)
        invalidate()
    }

    private fun drawControl(
        canvas: Canvas,
        action: Int,
        label: String,
        point: NativeTouchPoint,
        radius: Float,
        color: Int
    ) {
        val x = point.x * width
        val y = point.y * height
        paint.style = Paint.Style.FILL
        paint.color = if (activeControl == action) Color.rgb(255, 139, 65) else color
        canvas.drawCircle(x, y, radius, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = if (layout.highContrast) dp(2.5f) else dp(1.4f)
        paint.color = if (layout.highContrast) Color.rgb(255, 235, 59) else Color.rgb(224, 244, 252)
        canvas.drawCircle(x, y, radius, paint)
        paint.style = Paint.Style.FILL
        paint.textAlign = Paint.Align.CENTER
        paint.textSize = max(if (layout.highContrast) dp(10f) else dp(8f), radius * 0.31f)
        paint.color = Color.WHITE
        val baseline = y - (paint.descent() + paint.ascent()) * 0.5f
        canvas.drawText(label, x, baseline, paint)
    }

    private fun buttonRadius(): Float =
        max(
            dp(20f) * layout.scale,
            min(dp(54f) * layout.scale, min(width, height).toFloat() * 0.095f * layout.scale)
        )

    private fun labelFor(action: Int): String = when (action) {
        NativeHeldAction.JUMP -> "JUMP"
        NativeHeldAction.SPRINT -> "RUN"
        NativeHeldAction.FIRE -> "FIRE"
        NativeHeldAction.AIM -> "AIM"
        NativeHeldAction.USE -> "USE"
        NativeHeldAction.CROUCH -> "DUCK"
        else -> "ACTION"
    }

    private fun dp(value: Float): Float = value * resources.displayMetrics.density

    private companion object {
        const val EDITOR_MARGIN = 0.05f
    }
}
