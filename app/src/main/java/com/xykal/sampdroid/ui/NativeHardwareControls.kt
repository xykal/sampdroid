package com.xykal.sampdroid.ui

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs

/**
 * Stateful Android hardware-input adapter for the same semantic contract used
 * by the touch pads. It deliberately has no JNI/network dependency: callers
 * decide when a resulting [ControlSample] may reach the native session.
 *
 * Keyboard: WASD/arrow keys move, Space jumps, Shift runs, C crouches, F
 * fires, Q aims, R uses secondary action, and E/Enter requests USE. T or /
 * opens native chat (slash prefilled for commands), while Tab opens scores.
 * Bluetooth controllers map left stick/D-pad to movement, right stick to look,
 * A/B/X/Y to jump/use/fire/run, shoulder triggers to aim/fire, and Start to
 * chat. All gameplay values remain semantic and are bounded again by C++
 * input_state; chat/score shortcuts never write protocol bytes here.
 */
internal class NativeHardwareControlRouter(
    private val onControlSample: (ControlSample) -> Unit,
    private val onUseRequested: (passengerIntent: Boolean) -> Unit,
    private val isDriving: () -> Boolean,
    var onChatRequested: ((initialText: String) -> Unit)? = null,
    var onScoreboardRequested: (() -> Unit)? = null
) {
    private val heldKeys = LinkedHashSet<Int>()
    private var leftStickX = 0f
    private var leftStickY = 0f
    private var hatX = 0f
    private var hatY = 0f
    private var triggerActions = 0
    private val usePressedAtMs = mutableMapOf<Int, Long>()
    private var enabled = true

    /** Clears stale physical holds when server freeze or scene dismissal happens. */
    fun clear() {
        heldKeys.clear()
        leftStickX = 0f
        leftStickY = 0f
        hatX = 0f
        hatY = 0f
        triggerActions = 0
        usePressedAtMs.clear()
        emit()
    }

    /** Server-authoritative freeze disables and flushes hardware intent too. */
    fun setEnabled(next: Boolean) {
        if (enabled == next) return
        enabled = next
        if (!enabled) clear() else emit()
    }

    /**
     * Returns true only for shortcuts owned by this native scene. Presentation
     * shortcuts remain available while gameplay is frozen, because opening a
     * chat or score sheet does not create input/sync authority. They dispatch
     * on key-up exactly once, preventing Android keyboard repeat from opening
     * nested dialogs.
     */
    fun onKeyEvent(event: KeyEvent): Boolean {
        val keyCode = event.keyCode
        chatInitialTextForKey(keyCode)?.let { initialText ->
            if (event.action == KeyEvent.ACTION_UP && event.repeatCount == 0) {
                onChatRequested?.invoke(initialText)
            }
            return onChatRequested != null
        }
        if (isScoreboardKey(keyCode)) {
            if (event.action == KeyEvent.ACTION_UP && event.repeatCount == 0) {
                onScoreboardRequested?.invoke()
            }
            return onScoreboardRequested != null
        }
        if (!isGameplayKey(keyCode)) return false
        if (!enabled) return true
        when (event.action) {
            KeyEvent.ACTION_DOWN -> {
                if (event.repeatCount == 0) {
                    heldKeys += keyCode
                    if (isUseKey(keyCode)) usePressedAtMs[keyCode] = event.eventTime
                    emit()
                }
                return true
            }
            KeyEvent.ACTION_UP -> {
                val wasHeld = heldKeys.remove(keyCode)
                if (isUseKey(keyCode)) {
                    val pressedAt = usePressedAtMs.remove(keyCode)
                    if (wasHeld && pressedAt != null) {
                        onUseRequested(!isDriving() && event.eventTime - pressedAt >= USE_PASSENGER_HOLD_MS)
                    }
                }
                emit()
                return true
            }
        }
        return true
    }

    /**
     * Processes a joystick event without assuming a particular controller
     * vendor. Hat values compose with D-pad keys; controller drift under each
     * device's advertised flat range is discarded.
     */
    fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if ((event.source and InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK ||
            event.actionMasked != MotionEvent.ACTION_MOVE) {
            return false
        }
        if (!enabled) return true
        leftStickX = centeredAxis(event, MotionEvent.AXIS_X)
        leftStickY = centeredAxis(event, MotionEvent.AXIS_Y)
        hatX = centeredAxis(event, MotionEvent.AXIS_HAT_X)
        hatY = centeredAxis(event, MotionEvent.AXIS_HAT_Y)
        // Most pads expose right stick as Z/RZ; some use RX/RY. Keep axis
        // pairs intact so vertical stick motion cannot masquerade as yaw.
        val z = centeredAxis(event, MotionEvent.AXIS_Z)
        val rz = centeredAxis(event, MotionEvent.AXIS_RZ)
        val rx = centeredAxis(event, MotionEvent.AXIS_RX)
        val ry = centeredAxis(event, MotionEvent.AXIS_RY)
        val useZPair = abs(z) >= AXIS_DEAD_ZONE || abs(rz) >= AXIS_DEAD_ZONE
        val rightX = if (useZPair) z else rx
        val rightY = if (useZPair) rz else ry
        val aimTrigger = firstNonZero(
            triggerAxis(event, MotionEvent.AXIS_LTRIGGER),
            triggerAxis(event, MotionEvent.AXIS_BRAKE)
        )
        val fireTrigger = firstNonZero(
            triggerAxis(event, MotionEvent.AXIS_RTRIGGER),
            triggerAxis(event, MotionEvent.AXIS_GAS)
        )
        triggerActions =
            (if (aimTrigger > TRIGGER_THRESHOLD) NativeHeldAction.AIM else 0) or
                (if (fireTrigger > TRIGGER_THRESHOLD) NativeHeldAction.FIRE else 0)
        // Motion events normally arrive continuously while a stick is held. A
        // small bounded increment keeps yaw/pitch relative just like touch;
        // it never turns a controller axis into an absolute world camera.
        emit(
            (rightX * GAMEPAD_LOOK_DEGREES_PER_EVENT).coerceIn(-MAX_LOOK_DELTA, MAX_LOOK_DELTA),
            (-rightY * GAMEPAD_LOOK_PITCH_DEGREES_PER_EVENT).coerceIn(-MAX_LOOK_PITCH_DELTA, MAX_LOOK_PITCH_DELTA)
        )
        return true
    }

    private fun emit(lookDeltaDegrees: Float = 0f, lookPitchDeltaDegrees: Float = 0f) {
        val keyboardX = digitalAxis(
            held(KeyEvent.KEYCODE_A) || held(KeyEvent.KEYCODE_DPAD_LEFT),
            held(KeyEvent.KEYCODE_D) || held(KeyEvent.KEYCODE_DPAD_RIGHT)
        )
        val keyboardY = digitalAxis(
            held(KeyEvent.KEYCODE_W) || held(KeyEvent.KEYCODE_DPAD_UP),
            held(KeyEvent.KEYCODE_S) || held(KeyEvent.KEYCODE_DPAD_DOWN)
        )
        // Analog stick wins over digital input on the same axis; this avoids a
        // barely-held physical key fighting a controller's intended direction.
        val moveX = selectAxis(leftStickX, hatX, keyboardX)
        // Android joystick Y grows downward, while semantic moveY is forward-positive.
        val moveY = selectAxis(-leftStickY, -hatY, keyboardY)
        onControlSample(
            ControlSample(
                moveX = moveX,
                moveY = moveY,
                lookDeltaDegrees = lookDeltaDegrees,
                lookPitchDeltaDegrees = lookPitchDeltaDegrees,
                heldActions = heldActionMask()
            )
        )
    }

    private fun heldActionMask(): Int {
        var mask = triggerActions
        heldKeys.forEach { keyCode ->
            mask = mask or actionForKey(keyCode)
        }
        return mask
    }

    private fun held(keyCode: Int): Boolean = keyCode in heldKeys

    private fun selectAxis(primary: Float, secondary: Float, digital: Float): Float = when {
        abs(primary) >= AXIS_DEAD_ZONE -> primary
        abs(secondary) >= AXIS_DEAD_ZONE -> secondary
        else -> digital
    }.coerceIn(-1f, 1f)

    private fun centeredAxis(event: MotionEvent, axis: Int): Float {
        val value = event.getAxisValue(axis)
        val flat = event.device?.getMotionRange(axis, event.source)?.flat ?: AXIS_DEAD_ZONE
        return if (abs(value) <= maxOf(flat, AXIS_DEAD_ZONE)) 0f else value.coerceIn(-1f, 1f)
    }

    private fun triggerAxis(event: MotionEvent, axis: Int): Float {
        val raw = event.getAxisValue(axis)
        val range = event.device?.getMotionRange(axis, event.source)
        // A minority of older Android controller drivers report trigger at
        // [-1,1]. Use the advertised range rather than guessing from a zero
        // value, which is valid in both encodings.
        val normalized = if (range != null && range.min < 0f && range.max > range.min) {
            (raw - range.min) / (range.max - range.min)
        } else {
            raw
        }
        return normalized.coerceIn(0f, 1f)
    }

    private fun digitalAxis(negative: Boolean, positive: Boolean): Float = when {
        negative && !positive -> -1f
        positive && !negative -> 1f
        else -> 0f
    }

    private fun firstNonZero(first: Float, second: Float): Float =
        if (abs(first) >= AXIS_DEAD_ZONE) first else second

    private fun isGameplayKey(keyCode: Int): Boolean =
        movementKey(keyCode) || actionForKey(keyCode) != 0

    /** Null means this physical key is not a native-chat shortcut. */
    private fun chatInitialTextForKey(keyCode: Int): String? = when (keyCode) {
        KeyEvent.KEYCODE_T,
        KeyEvent.KEYCODE_BUTTON_START -> ""
        KeyEvent.KEYCODE_SLASH -> "/"
        else -> null
    }

    private fun isScoreboardKey(keyCode: Int): Boolean = keyCode == KeyEvent.KEYCODE_TAB

    private fun movementKey(keyCode: Int): Boolean = when (keyCode) {
        KeyEvent.KEYCODE_W,
        KeyEvent.KEYCODE_A,
        KeyEvent.KEYCODE_S,
        KeyEvent.KEYCODE_D,
        KeyEvent.KEYCODE_DPAD_UP,
        KeyEvent.KEYCODE_DPAD_DOWN,
        KeyEvent.KEYCODE_DPAD_LEFT,
        KeyEvent.KEYCODE_DPAD_RIGHT -> true
        else -> false
    }

    private fun isUseKey(keyCode: Int): Boolean = actionForKey(keyCode) == NativeHeldAction.USE

    private fun actionForKey(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_SPACE,
        KeyEvent.KEYCODE_BUTTON_A -> NativeHeldAction.JUMP

        KeyEvent.KEYCODE_SHIFT_LEFT,
        KeyEvent.KEYCODE_SHIFT_RIGHT,
        KeyEvent.KEYCODE_BUTTON_Y -> NativeHeldAction.SPRINT

        KeyEvent.KEYCODE_C -> NativeHeldAction.CROUCH

        KeyEvent.KEYCODE_F,
        KeyEvent.KEYCODE_BUTTON_X,
        KeyEvent.KEYCODE_BUTTON_R1,
        KeyEvent.KEYCODE_BUTTON_R2 -> NativeHeldAction.FIRE

        KeyEvent.KEYCODE_Q,
        KeyEvent.KEYCODE_BUTTON_L1,
        KeyEvent.KEYCODE_BUTTON_L2 -> NativeHeldAction.AIM

        KeyEvent.KEYCODE_R,
        KeyEvent.KEYCODE_BUTTON_SELECT -> NativeHeldAction.SECONDARY

        KeyEvent.KEYCODE_E,
        KeyEvent.KEYCODE_ENTER,
        KeyEvent.KEYCODE_DPAD_CENTER,
        KeyEvent.KEYCODE_BUTTON_B -> NativeHeldAction.USE

        else -> 0
    }

    private companion object {
        const val USE_PASSENGER_HOLD_MS = 500L
        const val AXIS_DEAD_ZONE = 0.12f
        const val TRIGGER_THRESHOLD = 0.20f
        const val GAMEPAD_LOOK_DEGREES_PER_EVENT = 5.5f
        const val GAMEPAD_LOOK_PITCH_DEGREES_PER_EVENT = 4.5f
        const val MAX_LOOK_DELTA = 18f
        const val MAX_LOOK_PITCH_DELTA = 16f
    }
}

/**
 * Combines a touch pad's persistent state with a hardware router's persistent
 * state. Relative yaw/pitch values are one-event pulses and therefore
 * intentionally never retained: a later joystick sample cannot replay a swipe.
 */
internal class NativeControlSampleMixer(
    private val consumer: (ControlSample) -> Unit
) {
    private var touch = ControlSample()
    private var hardware = ControlSample()

    fun submitTouch(sample: ControlSample) {
        touch = sample.copy(lookDeltaDegrees = 0f, lookPitchDeltaDegrees = 0f)
        emit(sample.lookDeltaDegrees, sample.lookPitchDeltaDegrees)
    }

    fun submitHardware(sample: ControlSample) {
        hardware = sample.copy(lookDeltaDegrees = 0f, lookPitchDeltaDegrees = 0f)
        emit(sample.lookDeltaDegrees, sample.lookPitchDeltaDegrees)
    }

    /** Gyro is another bounded relative-look pulse, never persistent state. */
    fun submitGyroLook(yawDegrees: Float, pitchDegrees: Float) {
        emit(yawDegrees, pitchDegrees)
    }

    fun clear() {
        touch = ControlSample()
        hardware = ControlSample()
        emit()
    }

    private fun emit(lookDeltaDegrees: Float = 0f, lookPitchDeltaDegrees: Float = 0f) {
        consumer(
            ControlSample(
                moveX = (touch.moveX + hardware.moveX).coerceIn(-1f, 1f),
                moveY = (touch.moveY + hardware.moveY).coerceIn(-1f, 1f),
                lookDeltaDegrees = lookDeltaDegrees.coerceIn(-MAX_LOOK_DELTA, MAX_LOOK_DELTA),
                lookPitchDeltaDegrees = lookPitchDeltaDegrees.coerceIn(-MAX_LOOK_DELTA, MAX_LOOK_DELTA),
                heldActions = touch.heldActions or hardware.heldActions
            )
        )
    }

    private companion object {
        const val MAX_LOOK_DELTA = 90f
    }
}
