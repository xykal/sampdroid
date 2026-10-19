package com.xykal.sampdroid.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.os.Build
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import kotlin.math.max
import kotlin.math.min

/**
 * Compact in-scene touch layer. It produces the same bounded semantic samples
 * as the full controls dialog. The right-side zone emits relative native yaw/pitch look;
 * remaining empty stage space stays available to the GPU/radar visual camera.
 * It has no direct network/transmit authority.
 */
internal class NativeGameplayControlsOverlay(context: Context) : View(context) {

    var onControlSample: ((ControlSample) -> Unit)? = null
    /** One-shot tap edge for actions that must not repeat while held. */
    var onActionPressed: ((Int) -> Unit)? = null
    /** false=tap USE/EXIT; true=held USE requests passenger vehicle intent. */
    var onUseRequested: ((Boolean) -> Unit)? = null

    private data class ActionButton(
        var label: String,
        val action: Int,
        var x: Float = 0f,
        var y: Float = 0f,
        var radius: Float = 0f
    )

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val buttons = listOf(
        ActionButton("JUMP", NativeHeldAction.JUMP),
        ActionButton("RUN", NativeHeldAction.SPRINT),
        ActionButton("FIRE", NativeHeldAction.FIRE),
        ActionButton("AIM", NativeHeldAction.AIM),
        ActionButton("USE", NativeHeldAction.USE),
        ActionButton("DUCK", NativeHeldAction.CROUCH)
    )
    private val actionPointers = mutableMapOf<Int, Int>()
    private val actionDownAtMs = mutableMapOf<Int, Long>()

    private var stickCenterX = 0f
    private var stickCenterY = 0f
    private var stickRadius = 1f
    private var knobX = 0f
    private var knobY = 0f
    private var movePointerId = MotionEvent.INVALID_POINTER_ID
    private var lookPointerId = MotionEvent.INVALID_POINTER_ID
    private var lastLookX = 0f
    private var lastLookY = 0f
    private var moveX = 0f
    private var moveY = 0f
    private var gameplayEnabled = true
    private var disabledStatusText = "SERVER FREEZE · INPUT OFF"
    private var drivingMode = false
    private var layout = NativeTouchLayoutStore.load(context)

    init {
        isClickable = true
        contentDescription = "Kontrol in-scene SampDroid: joystick, drag kanan untuk look yaw dan pitch, lompat, lari, duck, tembak, aim, use, dan tahan use untuk meminta kursi penumpang."
    }

    fun setControlsVisible(visible: Boolean) {
        if (!visible) clearControls()
        visibility = if (visible) VISIBLE else GONE
    }

    /** Reloads only local visual placement after the on-device layout editor saves. */
    fun reloadLayout() {
        layout = NativeTouchLayoutStore.load(context)
        layoutControls(width, height)
        invalidate()
    }

    /** Whether the locally persisted relative-gyro opt-in is active. */
    fun gyroLookEnabled(): Boolean = layout.gyroLookEnabled

    /**
     * Switches visible labels only after the server has put this player in the
     * driver seat. The semantic bits remain native-owned: stick is steer and
     * throttle/reverse, JUMP/SPRINT become braking, and USE asks the server to
     * exit through the existing authoritative interaction path.
     */
    fun setDrivingMode(enabled: Boolean) {
        if (drivingMode == enabled) return
        drivingMode = enabled
        // Crouch has no safe vehicle-pilot semantic in the bounded Packet-200
        // bridge. Drop a held touch if the server just seated this player.
        if (enabled && actionPointers.values.any { it == NativeHeldAction.CROUCH }) clearControls()
        buttons[0].label = if (enabled) "BRAKE" else "JUMP"
        buttons[1].label = if (enabled) "HAND" else "RUN"
        buttons[4].label = if (enabled) "EXIT" else "USE"
        contentDescription = if (enabled) {
            "Kontrol kendaraan SampDroid: joystick untuk steer dan gas atau mundur, drag kanan untuk look yaw dan pitch, rem, handbrake, tembak, aim, dan keluar."
        } else {
            "Kontrol in-scene SampDroid: joystick, drag kanan untuk look yaw dan pitch, lompat, lari, duck, tembak, aim, use, dan tahan use untuk meminta kursi penumpang."
        }
        invalidate()
    }

    /** Reflects server freeze or server-owned spectator lifecycle, never a local override. */
    fun setGameplayEnabled(enabled: Boolean, disabledStatus: String = "SERVER FREEZE · INPUT OFF") {
        val wasEnabled = gameplayEnabled
        val changed = wasEnabled != enabled || disabledStatusText != disabledStatus
        gameplayEnabled = enabled
        disabledStatusText = disabledStatus
        if (!enabled && wasEnabled) clearControls()
        if (!changed) return
        contentDescription = when {
            !enabled -> "Kontrol gameplay dihentikan sementara oleh server."
            drivingMode -> "Kontrol kendaraan SampDroid: steer, gas atau mundur, drag kanan untuk look yaw dan pitch, rem, handbrake, tembak, aim, dan keluar."
            else -> "Kontrol in-scene SampDroid: joystick, drag kanan untuk look yaw dan pitch, lompat, lari, duck, tembak, aim, use, dan tahan use untuk meminta kursi penumpang."
        }
        invalidate()
    }

    fun clearControls() {
        actionPointers.clear()
        actionDownAtMs.clear()
        movePointerId = MotionEvent.INVALID_POINTER_ID
        lookPointerId = MotionEvent.INVALID_POINTER_ID
        lastLookX = 0f
        lastLookY = 0f
        moveX = 0f
        moveY = 0f
        knobX = stickCenterX
        knobY = stickCenterY
        emit()
        invalidate()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        layoutControls(w, h)
    }

    private fun layoutControls(w: Int, h: Int) {
        if (w <= 0 || h <= 0) return
        val shortSide = min(w, h).toFloat()
        val scale = layout.scale
        // Large layouts must still fit narrow landscape phones. Clamp radius
        // before centers so an accessible 172% setup cannot create a control
        // that begins outside the actual touchable view.
        stickRadius = min(
            shortSide * 0.38f,
            max(dp(42f) * scale, min(dp(68f) * scale, shortSide * 0.16f * scale))
        )
        stickCenterX = (w * layout.stick.x).coerceIn(stickRadius, w - stickRadius)
        stickCenterY = (h * layout.stick.y).coerceIn(stickRadius, h - stickRadius)
        knobX = stickCenterX + moveX * stickRadius
        knobY = stickCenterY - moveY * stickRadius

        val radius = min(
            shortSide * 0.28f,
            max(dp(24f) * scale, min(dp(42f) * scale, shortSide * 0.092f * scale))
        )
        buttons.forEach { button ->
            val point = layout.pointFor(button.action)
            button.x = (w * point.x).coerceIn(radius, w - radius)
            button.y = (h * point.y).coerceIn(radius, h - radius)
            button.radius = radius
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        if (visibility != VISIBLE) return
        if (!gameplayEnabled) {
            paint.style = Paint.Style.FILL
            paint.textAlign = Paint.Align.CENTER
            paint.textSize = dp(12f)
            paint.color = Color.argb(230, 255, 190, 88)
            canvas.drawText(disabledStatusText, width / 2f, height - dp(28f), paint)
            return
        }
        val highContrast = layout.highContrast
        paint.style = Paint.Style.FILL
        paint.color = if (highContrast) Color.argb(238, 0, 77, 137) else Color.argb(78, 36, 109, 139)
        canvas.drawCircle(stickCenterX, stickCenterY, stickRadius, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = if (highContrast) dp(2.7f) else dp(1.7f)
        paint.color = if (highContrast) Color.rgb(255, 235, 59) else Color.argb(220, 137, 229, 255)
        canvas.drawCircle(stickCenterX, stickCenterY, stickRadius, paint)
        paint.style = Paint.Style.FILL
        paint.color = if (highContrast) Color.rgb(255, 193, 7) else Color.argb(184, 70, 194, 237)
        canvas.drawCircle(knobX, knobY, stickRadius * 0.35f, paint)

        buttons.forEach { button ->
            if (!buttonAvailable(button)) return@forEach
            val active = heldMask() and button.action != 0
            paint.color = when {
                highContrast && active -> Color.rgb(255, 111, 0)
                highContrast -> Color.rgb(10, 64, 112)
                active -> Color.argb(224, 255, 133, 66)
                else -> Color.argb(135, 38, 95, 122)
            }
            canvas.drawCircle(button.x, button.y, button.radius, paint)
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = if (highContrast) dp(2.3f) else dp(1.4f)
            paint.color = if (highContrast) Color.rgb(255, 235, 59) else Color.argb(220, 226, 245, 252)
            canvas.drawCircle(button.x, button.y, button.radius, paint)
            paint.style = Paint.Style.FILL
            paint.textAlign = Paint.Align.CENTER
            paint.textSize = max(if (highContrast) dp(10f) else dp(8f), button.radius * 0.27f)
            paint.color = Color.WHITE
            val baseline = button.y - (paint.descent() + paint.ascent()) / 2f
            canvas.drawText(button.label, button.x, baseline, paint)
        }

        paint.textAlign = Paint.Align.CENTER
        paint.textSize = if (highContrast) dp(10f) else dp(8f)
        paint.color = if (highContrast) Color.rgb(255, 235, 59) else Color.argb(215, 217, 241, 250)
        canvas.drawText("MOVE", stickCenterX, stickCenterY - stickRadius - dp(7f), paint)
        // Right-side empty space is an intentional heading/look zone. Left-side
        // empty space can still pass through to the visual scene/radar camera.
        canvas.drawText("DRAG LOOK YAW/PITCH", width * 0.62f, height * 0.18f, paint)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!gameplayEnabled) return false
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> return handleDown(event, event.actionIndex, allowPassThrough = true)
            MotionEvent.ACTION_POINTER_DOWN -> {
                handleDown(event, event.actionIndex, allowPassThrough = false)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                var changed = false
                var lookDelta = 0f
                var lookPitchDelta = 0f
                for (index in 0 until event.pointerCount) {
                    val pointerId = event.getPointerId(index)
                    if (pointerId == movePointerId) {
                        updateStick(event.getX(index), event.getY(index))
                        changed = true
                    }
                    if (pointerId == lookPointerId) {
                        // Native mapper clamps/normalizes yaw and pitch again.
                        // Keep only bounded relative deltas: touch never sets
                        // a world position, absolute camera, target, or hit.
                        lookDelta += ((event.getX(index) - lastLookX) * LOOK_DEGREES_PER_PIXEL)
                            .coerceIn(-MAX_LOOK_DELTA_DEGREES, MAX_LOOK_DELTA_DEGREES)
                        lookPitchDelta += ((lastLookY - event.getY(index)) * LOOK_PITCH_DEGREES_PER_PIXEL)
                            .coerceIn(-MAX_LOOK_PITCH_DELTA_DEGREES, MAX_LOOK_PITCH_DELTA_DEGREES)
                        lastLookX = event.getX(index)
                        lastLookY = event.getY(index)
                        changed = true
                    }
                }
                if (changed) {
                    emit(lookDelta, lookPitchDelta)
                    invalidate()
                }
                return true
            }
            MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_UP -> {
                releasePointer(event.getPointerId(event.actionIndex), event.eventTime)
                performClick()
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                clearControls()
                return true
            }
        }
        return false
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    private fun handleDown(event: MotionEvent, index: Int, allowPassThrough: Boolean): Boolean {
        val pointerId = event.getPointerId(index)
        val x = event.getX(index)
        val y = event.getY(index)
        val button = buttons.firstOrNull { buttonAvailable(it) && hit(it, x, y) }
        when {
            button != null -> {
                actionPointers[pointerId] = button.action
                actionDownAtMs[pointerId] = event.eventTime
                // Dispatch USE on release: a hold can request passenger intent
                // without first sending an accidental driver request. Its bit
                // still remains held for the native input/sync mapper.
                if (button.action != NativeHeldAction.USE) onActionPressed?.invoke(button.action)
                haptic()
                emit()
                invalidate()
                return true
            }
            movePointerId == MotionEvent.INVALID_POINTER_ID && hitStick(x, y) -> {
                movePointerId = pointerId
                updateStick(x, y)
                haptic()
                emit()
                invalidate()
                return true
            }
            lookPointerId == MotionEvent.INVALID_POINTER_ID && isLookZone(x, y) -> {
                lookPointerId = pointerId
                lastLookX = x
                lastLookY = y
                return true
            }
            allowPassThrough -> return false
            else -> return true
        }
    }

    private fun releasePointer(pointerId: Int, releasedAtMs: Long) {
        val releasedAction = actionPointers.remove(pointerId)
        val pressedAtMs = actionDownAtMs.remove(pointerId)
        if (releasedAction == NativeHeldAction.USE) {
            val passengerIntent = !drivingMode && pressedAtMs != null &&
                releasedAtMs - pressedAtMs >= USE_PASSENGER_HOLD_MS
            onUseRequested?.invoke(passengerIntent)
        }
        if (pointerId == movePointerId) {
            movePointerId = MotionEvent.INVALID_POINTER_ID
            moveX = 0f
            moveY = 0f
            knobX = stickCenterX
            knobY = stickCenterY
        }
        if (pointerId == lookPointerId) {
            lookPointerId = MotionEvent.INVALID_POINTER_ID
            lastLookX = 0f
            lastLookY = 0f
        }
        emit()
        invalidate()
    }

    private fun updateStick(x: Float, y: Float) {
        var dx = (x - stickCenterX) / stickRadius
        var dy = (stickCenterY - y) / stickRadius
        val length = Math.hypot(dx.toDouble(), dy.toDouble()).toFloat()
        if (length > 1f) {
            dx /= length
            dy /= length
        }
        moveX = dx
        moveY = dy
        knobX = stickCenterX + dx * stickRadius
        knobY = stickCenterY - dy * stickRadius
    }

    private fun heldMask(): Int = actionPointers.values.fold(0) { mask, action -> mask or action }

    private fun emit(lookDeltaDegrees: Float = 0f, lookPitchDeltaDegrees: Float = 0f) {
        onControlSample?.invoke(
            ControlSample(
                moveX = moveX,
                moveY = moveY,
                lookDeltaDegrees = lookDeltaDegrees,
                lookPitchDeltaDegrees = lookPitchDeltaDegrees,
                heldActions = heldMask()
            )
        )
    }

    private fun buttonAvailable(button: ActionButton): Boolean =
        !drivingMode || button.action != NativeHeldAction.CROUCH

    private fun hit(button: ActionButton, x: Float, y: Float): Boolean =
        Math.hypot((x - button.x).toDouble(), (y - button.y).toDouble()).toFloat() <= button.radius

    private fun hitStick(x: Float, y: Float): Boolean =
        Math.hypot((x - stickCenterX).toDouble(), (y - stickCenterY).toDouble()).toFloat() <= stickRadius * 1.28f

    private fun isLookZone(x: Float, y: Float): Boolean =
        x >= width * LOOK_ZONE_START_X && y >= height * 0.08f && y <= height * 0.70f

    private fun haptic() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
        }
    }

    private fun dp(value: Float): Float = value * resources.displayMetrics.density

    private companion object {
        const val USE_PASSENGER_HOLD_MS = 500L
        const val LOOK_ZONE_START_X = 0.42f
        const val LOOK_DEGREES_PER_PIXEL = 0.25f
        const val LOOK_PITCH_DEGREES_PER_PIXEL = 0.20f
        const val MAX_LOOK_DELTA_DEGREES = 45f
        const val MAX_LOOK_PITCH_DELTA_DEGREES = 40f
    }
}
