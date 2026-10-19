package com.xykal.sampdroid.ui

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.Looper
import android.view.Surface
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import kotlin.math.abs

/**
 * Optional Android-native relative gyro look.
 *
 * This translates a physical gyroscope's angular velocity into small, bounded
 * yaw/pitch pulses for the same [ControlSample] path as touch and gamepads. It
 * never supplies movement, an absolute camera/world orientation, a target, or
 * a packet. Registration exists only while a visible world surface has both a
 * user opt-in and server-authorized gameplay input enabled.
 */
internal class NativeGyroLookController(
    context: Context,
    private val displayRotation: () -> Int,
    private val canEmit: () -> Boolean = { true },
    private val onLookDelta: (yawDegrees: Float, pitchDegrees: Float) -> Unit
) : SensorEventListener {
    private val sensorManager = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
    private val gyroscope = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
    private val mainHandler = Handler(Looper.getMainLooper())
    private val lifecycleOwner = context as? LifecycleOwner

    private var requestedEnabled = false
    private var gameplayEnabled = false
    private var hostResumed = lifecycleOwner?.lifecycle?.currentState
        ?.isAtLeast(Lifecycle.State.RESUMED) ?: true
    private var registered = false
    private var lastTimestampNanos = 0L
    private val lifecycleObserver = object : DefaultLifecycleObserver {
        override fun onResume(owner: LifecycleOwner) {
            hostResumed = true
            updateRegistration()
        }

        override fun onPause(owner: LifecycleOwner) {
            hostResumed = false
            updateRegistration()
        }
    }

    init {
        lifecycleOwner?.lifecycle?.addObserver(lifecycleObserver)
    }

    val available: Boolean get() = gyroscope != null

    /** Changes only local sensor registration; it cannot bypass native gates. */
    fun setUserEnabled(enabled: Boolean) {
        if (requestedEnabled == enabled) return
        requestedEnabled = enabled
        updateRegistration()
    }

    /** Mirrors server freeze/spectate/modal state from the world surface. */
    fun setGameplayEnabled(enabled: Boolean) {
        if (gameplayEnabled == enabled) return
        gameplayEnabled = enabled
        lastTimestampNanos = 0L
        updateRegistration()
    }

    /** Must run when the containing radar/GLES dialog is dismissed. */
    fun release() {
        if (registered) sensorManager?.unregisterListener(this)
        lifecycleOwner?.lifecycle?.removeObserver(lifecycleObserver)
        registered = false
        gameplayEnabled = false
        hostResumed = false
        lastTimestampNanos = 0L
    }

    private fun updateRegistration() {
        val sensor = gyroscope
        val shouldRegister = requestedEnabled && gameplayEnabled && hostResumed && sensor != null
        if (shouldRegister == registered) return
        lastTimestampNanos = 0L
        if (shouldRegister && sensor != null) {
            // Explicit main looper keeps the mixer/UI lifecycle single-threaded.
            registered = sensorManager?.registerListener(
                this,
                sensor,
                SensorManager.SENSOR_DELAY_GAME,
                mainHandler
            ) == true
        } else {
            sensorManager?.unregisterListener(this)
            registered = false
        }
    }

    override fun onSensorChanged(event: SensorEvent) {
        if (!registered || !canEmit() || event.sensor.type != Sensor.TYPE_GYROSCOPE || event.values.size < 3) {
            lastTimestampNanos = 0L
            return
        }
        val previous = lastTimestampNanos
        lastTimestampNanos = event.timestamp
        if (previous == 0L || event.timestamp <= previous) return
        // Do not convert a background/resume gap into an unwanted full turn.
        val seconds = ((event.timestamp - previous).toDouble() / NANOS_PER_SECOND)
            .toFloat()
            .coerceIn(0f, MAX_STEP_SECONDS)
        if (seconds <= 0f) return

        // TYPE_GYROSCOPE axes are device-fixed. Remap the two screen-plane axes
        // for common display rotations before deriving a *relative* game look.
        // Z (twist/roll) is deliberately ignored so a tilted phone cannot turn
        // into unbounded yaw. Signs are chosen to match drag-right yaw and
        // drag-up pitch in NativeGameplayControlsOverlay.
        val x = boundedAngularVelocity(event.values[0])
        val y = boundedAngularVelocity(event.values[1])
        val (yawVelocity, pitchVelocity) = when (displayRotation()) {
            Surface.ROTATION_90 -> -x to -y
            Surface.ROTATION_180 -> -y to x
            Surface.ROTATION_270 -> x to y
            else -> y to -x
        }
        val yaw = (yawVelocity * seconds * RADIANS_TO_DEGREES * SENSITIVITY)
            .coerceIn(-MAX_DEGREES_PER_SAMPLE, MAX_DEGREES_PER_SAMPLE)
        val pitch = (pitchVelocity * seconds * RADIANS_TO_DEGREES * SENSITIVITY)
            .coerceIn(-MAX_DEGREES_PER_SAMPLE, MAX_DEGREES_PER_SAMPLE)
        if (!yaw.isFinite() || !pitch.isFinite() || (abs(yaw) < MIN_DEGREES && abs(pitch) < MIN_DEGREES)) {
            return
        }
        onLookDelta(yaw, pitch)
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    private fun boundedAngularVelocity(value: Float): Float =
        if (value.isFinite()) value.coerceIn(-MAX_RADIANS_PER_SECOND, MAX_RADIANS_PER_SECOND) else 0f

    private companion object {
        const val NANOS_PER_SECOND = 1_000_000_000.0
        const val MAX_STEP_SECONDS = 0.05f
        const val MAX_RADIANS_PER_SECOND = 12f
        const val RADIANS_TO_DEGREES = 57.29578f
        const val SENSITIVITY = 0.80f
        const val MAX_DEGREES_PER_SAMPLE = 12f
        const val MIN_DEGREES = 0.01f
    }
}
