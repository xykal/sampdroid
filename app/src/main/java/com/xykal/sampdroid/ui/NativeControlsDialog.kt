package com.xykal.sampdroid.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.os.Build
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.xykal.sampdroid.NativeBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.util.Locale
import kotlin.math.max
import kotlin.math.min

/**
 * First runtime implementation of the native input-layout contract. It emits
 * semantic Android actions into C++ and intentionally shows an OnFoot preview
 * only: movement packets remain disabled until the world/collision bridge owns
 * a real local position.
 */
object NativeControlsDialog {

    fun show(activity: AppCompatActivity) {
        val density = activity.resources.displayMetrics.density
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((16 * density).toInt(), (12 * density).toInt(), (16 * density).toInt(), 0)
        }
        val intro = TextView(activity).apply {
            text = "KONTROL TOUCH NATIVE · PREVIEW AMAN\n" +
                "Joystick, look, dan tombol di bawah masuk sebagai action Android native. " +
                "Belum ada packet pergerakan yang dikirim ke server."
            textSize = 12f
            setTextColor(Color.rgb(210, 225, 235))
        }
        val padHeight = max(
            (160f * density).toInt(),
            (activity.resources.displayMetrics.heightPixels * 0.38f).toInt()
        )
        val pad = NativeControlPadView(activity).apply {
            // Use a bounded screen-relative height instead of assuming one
            // phone/tablet size; this remains usable on compact ARMv7 phones.
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                padHeight
            ).apply { topMargin = (10 * density).toInt() }
        }
        val state = TextView(activity).apply {
            text = "Menunggu input touch…"
            textSize = 12f
            setTextColor(Color.rgb(185, 246, 202))
            setPadding(0, (8 * density).toInt(), 0, (8 * density).toInt())
        }
        root.addView(intro)
        root.addView(pad)
        root.addView(state)

        val inputs = Channel<ControlSample>(Channel.CONFLATED)
        val updateJob: Job = activity.lifecycleScope.launch {
            for (sample in inputs) {
                val response = runCatching {
                    withContext(Dispatchers.IO) {
                        NativeBridge.nativeSessionUpdateControls(
                            sample.moveX,
                            sample.moveY,
                            sample.lookDeltaDegrees,
                            sample.heldActions
                        )
                    }
                }.getOrElse { "{\"error\":\"${it.message ?: "native input gagal"}\"}" }
                val snapshot = runCatching { JSONObject(response) }.getOrNull()
                state.text = snapshot?.let(::formatState) ?: "Input native gagal dibaca"
            }
        }
        pad.onControlSample = { inputs.trySend(it) }

        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Kontrol native")
            .setView(root)
            .setNegativeButton("Tutup", null)
            .create()
        dialog.setOnShowListener {
            val height = (activity.resources.displayMetrics.heightPixels * 0.72f).toInt()
            dialog.window?.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, height)
        }
        dialog.setOnDismissListener {
            pad.clearControls()
            inputs.close()
            updateJob.cancel()
            // Always release held actions even if the conflated channel closes
            // before the final touch-up sample is consumed.
            activity.lifecycleScope.launch(Dispatchers.IO) {
                runCatching {
                    NativeBridge.nativeSessionUpdateControls(0f, 0f, 0f, 0)
                }
            }
        }
        dialog.show()
    }

    private fun formatState(o: JSONObject): String = when {
        !o.optBoolean("active") -> "Sesi native tidak aktif: ${o.optString("error").ifBlank { "buka sesi dulu" }}"
        !o.optBoolean("localPositionKnown") ->
            "Input diterima. Preview menunggu posisi/spawn lokal dari server."
        !o.optBoolean("onFootPreviewReady") ->
            "Input diterima. Preview belum valid; cek state lokal server."
        else -> String.format(
            Locale.US,
            "preview lokal siap · move %.2f / %.2f · heading %.1f° · lr=%d ud=%d keys=0x%04X\n" +
                "Tidak dikirim ke server; world/collision bridge belum ada.",
            o.optDouble("controlMoveX"),
            o.optDouble("controlMoveY"),
            o.optDouble("controlHeading"),
            o.optInt("controlLeftRight"),
            o.optInt("controlUpDown"),
            o.optInt("controlKeys")
        )
    }
}

/** Private semantic values mirrored by input_state.h, never raw SA-MP keys. */
private object NativeHeldAction {
    const val USE = 1 shl 0
    const val CROUCH = 1 shl 1
    const val FIRE = 1 shl 2
    const val SPRINT = 1 shl 3
    const val SECONDARY = 1 shl 4
    const val JUMP = 1 shl 5
    const val AIM = 1 shl 6
    const val WALK = 1 shl 7
}

private data class ControlSample(
    val moveX: Float = 0f,
    val moveY: Float = 0f,
    val lookDeltaDegrees: Float = 0f,
    val heldActions: Int = 0
)

/**
 * Lightweight multi-touch canvas control pad. It is not tied to a renderer, so
 * its output stays semantic and can later be fed to touch/gamepad/keyboard
 * bindings through the same native mapper.
 */
private class NativeControlPadView(context: Context) : View(context) {

    var onControlSample: ((ControlSample) -> Unit)? = null

    private data class ActionButton(
        val label: String,
        val action: Int,
        var centerX: Float = 0f,
        var centerY: Float = 0f,
        var radius: Float = 0f
    )

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val actionButtons = listOf(
        ActionButton("JUMP", NativeHeldAction.JUMP),
        ActionButton("RUN", NativeHeldAction.SPRINT),
        ActionButton("FIRE", NativeHeldAction.FIRE),
        ActionButton("AIM", NativeHeldAction.AIM),
        ActionButton("USE", NativeHeldAction.USE),
        ActionButton("DUCK", NativeHeldAction.CROUCH)
    )
    private val actionPointers = mutableMapOf<Int, Int>()

    private var stickCenterX = 0f
    private var stickCenterY = 0f
    private var stickRadius = 1f
    private var knobX = 0f
    private var knobY = 0f
    private var movePointerId = MotionEvent.INVALID_POINTER_ID
    private var lookPointerId = MotionEvent.INVALID_POINTER_ID
    private var lastLookX = 0f
    private var moveX = 0f
    private var moveY = 0f

    init {
        isClickable = true
        contentDescription = "Kontrol touch native SampDroid: joystick, look, lompat, lari, tembak, aim, dan use."
        setBackgroundColor(Color.rgb(16, 29, 40))
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        val shortSide = min(w, h).toFloat()
        stickRadius = max(dp(52f), shortSide * 0.20f)
        stickCenterX = w * 0.20f
        stickCenterY = h * 0.64f
        knobX = stickCenterX + moveX * stickRadius
        knobY = stickCenterY - moveY * stickRadius

        val r = max(dp(28f), shortSide * 0.105f)
        val slots = arrayOf(
            floatArrayOf(0.67f, 0.68f), // JUMP
            floatArrayOf(0.84f, 0.78f), // RUN
            floatArrayOf(0.86f, 0.46f), // FIRE
            floatArrayOf(0.69f, 0.43f), // AIM
            floatArrayOf(0.57f, 0.82f), // USE
            floatArrayOf(0.72f, 0.91f)  // DUCK
        )
        actionButtons.forEachIndexed { index, button ->
            button.centerX = w * slots[index][0]
            button.centerY = h * slots[index][1]
            button.radius = r
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        paint.style = Paint.Style.FILL
        paint.color = Color.rgb(28, 54, 70)
        canvas.drawCircle(stickCenterX, stickCenterY, stickRadius, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(2f)
        paint.color = Color.rgb(116, 210, 255)
        canvas.drawCircle(stickCenterX, stickCenterY, stickRadius, paint)
        paint.style = Paint.Style.FILL
        paint.color = Color.rgb(90, 185, 230)
        canvas.drawCircle(knobX, knobY, stickRadius * 0.35f, paint)

        actionButtons.forEach { button ->
            val active = (heldActionMask() and button.action) != 0
            paint.color = if (active) Color.rgb(255, 149, 71) else Color.rgb(51, 91, 112)
            canvas.drawCircle(button.centerX, button.centerY, button.radius, paint)
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = dp(1.5f)
            paint.color = Color.rgb(214, 238, 250)
            canvas.drawCircle(button.centerX, button.centerY, button.radius, paint)
            paint.style = Paint.Style.FILL
            paint.textAlign = Paint.Align.CENTER
            paint.textSize = max(dp(10f), button.radius * 0.29f)
            paint.color = Color.WHITE
            val baseline = button.centerY - (paint.descent() + paint.ascent()) / 2f
            canvas.drawText(button.label, button.centerX, baseline, paint)
        }

        paint.textAlign = Paint.Align.LEFT
        paint.textSize = dp(11f)
        paint.color = Color.rgb(189, 223, 239)
        canvas.drawText("GERAK", stickCenterX - dp(22f), stickCenterY - stickRadius - dp(10f), paint)
        canvas.drawText("geser area kosong kanan untuk LOOK", width * 0.43f, dp(22f), paint)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        parent?.requestDisallowInterceptTouchEvent(true)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_POINTER_DOWN -> handlePointerDown(event, event.actionIndex)

            MotionEvent.ACTION_MOVE -> handleMove(event)

            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP -> handlePointerUp(event, event.actionIndex)

            MotionEvent.ACTION_CANCEL -> {
                resetPointers()
                emit()
            }
        }
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    fun clearControls() {
        resetPointers()
        emit()
    }

    private fun handlePointerDown(event: MotionEvent, index: Int) {
        val pointerId = event.getPointerId(index)
        val x = event.getX(index)
        val y = event.getY(index)
        val button = actionButtons.firstOrNull { hit(it, x, y) }
        when {
            button != null -> {
                actionPointers[pointerId] = button.action
                haptic()
                emit()
            }
            movePointerId == MotionEvent.INVALID_POINTER_ID && hitStick(x, y) -> {
                movePointerId = pointerId
                updateStick(x, y)
                haptic()
                emit()
            }
            lookPointerId == MotionEvent.INVALID_POINTER_ID -> {
                lookPointerId = pointerId
                lastLookX = x
            }
        }
        invalidate()
    }

    private fun handleMove(event: MotionEvent) {
        var lookDelta = 0f
        for (index in 0 until event.pointerCount) {
            val pointerId = event.getPointerId(index)
            val x = event.getX(index)
            val y = event.getY(index)
            if (pointerId == movePointerId) updateStick(x, y)
            if (pointerId == lookPointerId) {
                // Relative horizontal swipe only. The native mapper bounds a
                // malicious/bad sample again before accumulating heading.
                lookDelta += ((x - lastLookX) * 0.25f).coerceIn(-90f, 90f)
                lastLookX = x
            }
        }
        emit(lookDelta)
    }

    private fun handlePointerUp(event: MotionEvent, index: Int) {
        val pointerId = event.getPointerId(index)
        actionPointers.remove(pointerId)
        if (pointerId == movePointerId) {
            movePointerId = MotionEvent.INVALID_POINTER_ID
            moveX = 0f
            moveY = 0f
            knobX = stickCenterX
            knobY = stickCenterY
        }
        if (pointerId == lookPointerId) lookPointerId = MotionEvent.INVALID_POINTER_ID
        performClick()
        emit()
        invalidate()
    }

    private fun updateStick(x: Float, y: Float) {
        var dx = (x - stickCenterX) / stickRadius
        var dy = (stickCenterY - y) / stickRadius
        val length = distance(dx, dy)
        if (length > 1f) {
            dx /= length
            dy /= length
        }
        moveX = dx
        moveY = dy
        knobX = stickCenterX + dx * stickRadius
        knobY = stickCenterY - dy * stickRadius
    }

    private fun resetPointers() {
        actionPointers.clear()
        movePointerId = MotionEvent.INVALID_POINTER_ID
        lookPointerId = MotionEvent.INVALID_POINTER_ID
        moveX = 0f
        moveY = 0f
        knobX = stickCenterX
        knobY = stickCenterY
        invalidate()
    }

    private fun heldActionMask(): Int = actionPointers.values.fold(0) { mask, action -> mask or action }

    private fun emit(lookDelta: Float = 0f) {
        onControlSample?.invoke(
            ControlSample(
                moveX = moveX,
                moveY = moveY,
                lookDeltaDegrees = lookDelta,
                heldActions = heldActionMask()
            )
        )
    }

    private fun hit(button: ActionButton, x: Float, y: Float): Boolean =
        distance(x - button.centerX, y - button.centerY) <= button.radius

    private fun hitStick(x: Float, y: Float): Boolean =
        distance(x - stickCenterX, y - stickCenterY) <= stickRadius * 1.25f

    private fun distance(x: Float, y: Float): Float =
        Math.hypot(x.toDouble(), y.toDouble()).toFloat()

    private fun haptic() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
        }
    }

    private fun dp(value: Float): Float = value * resources.displayMetrics.density
}
