package com.xykal.sampdroid.ui

import android.content.Context
import android.graphics.Color
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.opengl.Matrix
import android.view.MotionEvent
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
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
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin

/**
 * A small GPU-backed scene bridge for the Android-native client.
 *
 * This deliberately draws generated grid, markers, and simple actor geometry
 * from copied native state. It is not a GTA map/model renderer and never
 * fabricates collision, local movement, or outbound sync.
 */
object NativeWorldSceneDialog {

    fun show(activity: AppCompatActivity) {
        val density = activity.resources.displayMetrics.density
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((12f * density).toInt(), (8f * density).toInt(), (12f * density).toInt(), 0)
            setBackgroundColor(Color.rgb(8, 17, 24))
        }
        val intro = TextView(activity).apply {
            text = "DUNIA NATIVE · SCENE GPU\n" +
                "Actor, marker, dan posisi datang dari snapshot native. Grid/geometri dibuat APK; " +
                "belum map atau model GTA."
            textSize = 12f
            setTextColor(Color.rgb(215, 237, 247))
        }
        val surfaceHeight = max(
            (292f * density).toInt(),
            (activity.resources.displayMetrics.heightPixels * 0.54f).toInt()
        )
        val stage = FrameLayout(activity).apply {
            setBackgroundColor(Color.rgb(5, 12, 18))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                surfaceHeight
            ).apply { topMargin = (8f * density).toInt() }
        }
        val scene = NativeWorldSceneSurface(activity)
        stage.addView(scene, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ))
        val legend = TextView(activity).apply {
            text = "GPU SCENE · drag = kamera · cubit = zoom\n" +
                "cyan = kamu · warna = remote · kuning/oranye = checkpoint"
            textSize = 10f
            setTextColor(Color.rgb(205, 234, 245))
            setPadding((9f * density).toInt(), (6f * density).toInt(), (9f * density).toInt(), (6f * density).toInt())
            setBackgroundColor(Color.argb(190, 1, 9, 13))
        }
        stage.addView(legend, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply {
            gravity = android.view.Gravity.TOP or android.view.Gravity.START
            topMargin = (8f * density).toInt()
            marginStart = (8f * density).toInt()
        })

        val controls = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER
            setPadding(0, (6f * density).toInt(), 0, 0)
        }
        fun addButton(label: String, onClick: () -> Unit) {
            controls.addView(Button(activity).apply {
                text = label
                textSize = 10f
                setOnClickListener { onClick() }
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = (2f * density).toInt()
                marginEnd = (2f * density).toInt()
            })
        }
        addButton("− ZOOM") { scene.zoomBy(1.35f) }
        addButton("+ ZOOM") { scene.zoomBy(1f / 1.35f) }
        addButton("RESET") { scene.resetCamera() }
        addButton("KONTROL") { NativeControlsDialog.show(activity) }

        val status = TextView(activity).apply {
            text = "Memuat scene native…"
            textSize = 11f
            setTextColor(Color.rgb(185, 246, 202))
            setPadding(0, (5f * density).toInt(), 0, (5f * density).toInt())
        }
        root.addView(intro)
        root.addView(stage)
        root.addView(controls)
        root.addView(status)

        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Dunia native · scene GPU")
            .setView(root)
            .setNegativeButton("Tutup", null)
            .create()
        var renderJob: Job? = null
        dialog.setOnShowListener {
            dialog.window?.setLayout(
                ViewGroup.LayoutParams.MATCH_PARENT,
                (activity.resources.displayMetrics.heightPixels * 0.88f).toInt()
            )
            scene.onResume()
            renderJob = activity.lifecycleScope.launch {
                while (isActive && dialog.isShowing) {
                    val frame = runCatching {
                        val raw = withContext(Dispatchers.IO) {
                            NativeBridge.nativeSessionWorldSnapshot()
                        }
                        WorldFrame.from(JSONObject(raw))
                    }.getOrElse { WorldFrame(error = it.message ?: "snapshot world gagal") }
                    scene.submit(frame)
                    status.text = frame.sceneStatusLine()
                    // ServerDetailDialog retains and polls UDP. The renderer
                    // receives immutable copies only; it has no socket or sync ownership.
                    delay(100)
                }
            }
        }
        dialog.setOnDismissListener {
            renderJob?.cancel()
            scene.onPause()
        }
        dialog.show()
    }
}

private fun WorldFrame.sceneStatusLine(): String = when {
    !active -> "Sesi belum aktif · buka Sesi native dari detail server."
    error.isNotBlank() -> "Sesi: $error"
    !localKnown -> "Sesi aktif · menunggu posisi/spawn lokal dari server."
    else -> "${actors.size}/${streamedPlayers} actor scene · $remoteOnFootPlayers on-foot · geometri hanya visual"
}

/**
 * Touch input here changes only this scene camera. It is intentionally separate
 * from NativeControlsDialog's semantic gameplay-intent mapper.
 */
private class NativeWorldSceneSurface(context: Context) : GLSurfaceView(context) {

    private val sceneRenderer = NativeWorldSceneRenderer()
    private var activePointerId = MotionEvent.INVALID_POINTER_ID
    private var lastX = 0f
    private var lastY = 0f
    private var lastSpan = 0f
    private var touchMoved = false

    init {
        setEGLContextClientVersion(2)
        setRenderer(sceneRenderer)
        // World state changes at 10 Hz and camera changes only during touch.
        // Rendering on demand avoids burning an ARMv7 GPU while the scene is idle.
        renderMode = RENDERMODE_WHEN_DIRTY
        preserveEGLContextOnPause = true
        isClickable = true
        contentDescription = "Scene GPU dunia native SampDroid. Drag untuk mengubah kamera, cubit untuk zoom."
    }

    fun submit(frame: WorldFrame) {
        sceneRenderer.submit(frame)
        requestRender()
    }

    fun zoomBy(multiplier: Float) {
        sceneRenderer.zoomBy(multiplier)
        requestRender()
    }

    fun resetCamera() {
        sceneRenderer.resetCamera()
        requestRender()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        parent?.requestDisallowInterceptTouchEvent(true)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                activePointerId = event.getPointerId(0)
                lastX = event.x
                lastY = event.y
                lastSpan = 0f
                touchMoved = false
                return true
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                if (event.pointerCount >= 2) {
                    lastSpan = pointerSpan(event)
                }
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (event.pointerCount >= 2) {
                    val span = pointerSpan(event)
                    if (lastSpan > 0f && span > 0f) {
                        // Spreading fingers approaches the scene; pinching out
                        // backs it away. This changes no session/player state.
                        sceneRenderer.zoomBy((lastSpan / span).coerceIn(0.78f, 1.28f))
                        touchMoved = true
                    }
                    lastSpan = span
                } else {
                    val index = event.findPointerIndex(activePointerId)
                    if (index >= 0) {
                        val x = event.getX(index)
                        val y = event.getY(index)
                        val dx = x - lastX
                        val dy = y - lastY
                        if (dx != 0f || dy != 0f) {
                            sceneRenderer.rotateCamera(dx * 0.32f, dy * 0.19f)
                            lastX = x
                            lastY = y
                            touchMoved = true
                        }
                    }
                }
                requestRender()
                return true
            }
            MotionEvent.ACTION_POINTER_UP -> {
                if (event.getPointerId(event.actionIndex) == activePointerId) {
                    val replacement = if (event.actionIndex == 0) 1 else 0
                    if (replacement < event.pointerCount) {
                        activePointerId = event.getPointerId(replacement)
                        lastX = event.getX(replacement)
                        lastY = event.getY(replacement)
                    }
                }
                lastSpan = 0f
                return true
            }
            MotionEvent.ACTION_UP -> {
                if (!touchMoved) performClick()
                activePointerId = MotionEvent.INVALID_POINTER_ID
                lastSpan = 0f
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                activePointerId = MotionEvent.INVALID_POINTER_ID
                lastSpan = 0f
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    private fun pointerSpan(event: MotionEvent): Float {
        if (event.pointerCount < 2) return 0f
        return Math.hypot(
            (event.getX(1) - event.getX(0)).toDouble(),
            (event.getY(1) - event.getY(0)).toDouble()
        ).toFloat()
    }
}

/**
 * GLES 2.0 renderer with no asset dependency. Each draw frame reads a copied
 * WorldFrame, turns relative coordinates into generated geometry, then draws
 * it with an Android GPU. Network polling and state mutation stay out of here.
 */
private class NativeWorldSceneRenderer : GLSurfaceView.Renderer {

    private val lock = Any()
    private var worldFrame = WorldFrame()
    private var userYawDegrees = 0f
    private var userPitchDegrees = 20f
    private var cameraDistance = 28f

    private var program = 0
    private var positionHandle = -1
    private var mvpHandle = -1
    private var colorHandle = -1
    private lateinit var gridBuffer: FloatBuffer
    private lateinit var axisBuffer: FloatBuffer
    private lateinit var cubeBuffer: FloatBuffer
    private var gridVertexCount = 0
    private var axisVertexCount = 0
    private val projection = FloatArray(16)
    private val view = FloatArray(16)
    private val viewProjection = FloatArray(16)
    private val model = FloatArray(16)
    private val modelViewProjection = FloatArray(16)

    fun submit(frame: WorldFrame) = synchronized(lock) {
        worldFrame = frame
    }

    fun zoomBy(multiplier: Float) = synchronized(lock) {
        if (multiplier.isFinite() && multiplier > 0f) {
            cameraDistance = (cameraDistance * multiplier).coerceIn(7f, 120f)
        }
    }

    fun rotateCamera(yawDelta: Float, pitchDelta: Float) = synchronized(lock) {
        if (yawDelta.isFinite()) userYawDegrees = normalizeDegrees(userYawDegrees + yawDelta)
        if (pitchDelta.isFinite()) userPitchDegrees = (userPitchDegrees + pitchDelta).coerceIn(7f, 72f)
    }

    fun resetCamera() = synchronized(lock) {
        userYawDegrees = 0f
        userPitchDegrees = 20f
        cameraDistance = 28f
    }

    override fun onSurfaceCreated(gl: javax.microedition.khronos.opengles.GL10?, config: javax.microedition.khronos.egl.EGLConfig?) {
        GLES20.glClearColor(0.018f, 0.045f, 0.067f, 1f)
        GLES20.glEnable(GLES20.GL_DEPTH_TEST)
        GLES20.glDepthFunc(GLES20.GL_LEQUAL)
        GLES20.glDisable(GLES20.GL_CULL_FACE)
        GLES20.glEnable(GLES20.GL_BLEND)
        GLES20.glBlendFunc(GLES20.GL_SRC_ALPHA, GLES20.GL_ONE_MINUS_SRC_ALPHA)

        program = buildProgram(VERTEX_SHADER, FRAGMENT_SHADER)
        positionHandle = GLES20.glGetAttribLocation(program, "aPosition")
        mvpHandle = GLES20.glGetUniformLocation(program, "uMvp")
        colorHandle = GLES20.glGetUniformLocation(program, "uColor")
        gridBuffer = floatBuffer(makeGrid(220f, 20f))
        gridVertexCount = gridBuffer.capacity() / 3
        axisBuffer = floatBuffer(floatArrayOf(
            -220f, 0.02f, 0f, 220f, 0.02f, 0f,
            0f, 0.02f, -220f, 0f, 0.02f, 220f
        ))
        axisVertexCount = axisBuffer.capacity() / 3
        cubeBuffer = floatBuffer(CUBE_VERTICES)
    }

    override fun onSurfaceChanged(gl: javax.microedition.khronos.opengles.GL10?, width: Int, height: Int) {
        GLES20.glViewport(0, 0, width, height)
        val aspect = if (height > 0) width.toFloat() / height.toFloat() else 1f
        Matrix.perspectiveM(projection, 0, 61f, aspect, 0.45f, 1200f)
    }

    override fun onDrawFrame(gl: javax.microedition.khronos.opengles.GL10?) {
        val snapshot: SceneCameraState = synchronized(lock) {
            SceneCameraState(worldFrame, userYawDegrees, userPitchDegrees, cameraDistance)
        }
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT or GLES20.GL_DEPTH_BUFFER_BIT)
        if (program == 0 || positionHandle < 0 || mvpHandle < 0 || colorHandle < 0) return

        val heading = if (snapshot.frame.localFacingKnown) snapshot.frame.localAngle else 0f
        val yawRadians = Math.toRadians((heading + snapshot.userYawDegrees).toDouble())
        val pitchRadians = Math.toRadians(snapshot.userPitchDegrees.toDouble())
        val horizontalDistance = snapshot.cameraDistance * cos(pitchRadians).toFloat()
        val eyeX = -sin(yawRadians).toFloat() * horizontalDistance
        val eyeY = 1.8f + sin(pitchRadians).toFloat() * snapshot.cameraDistance
        val eyeZ = cos(yawRadians).toFloat() * horizontalDistance
        val targetX = sin(yawRadians).toFloat() * 6f
        val targetY = 1.1f
        val targetZ = -cos(yawRadians).toFloat() * 6f
        Matrix.setLookAtM(view, 0, eyeX, eyeY, eyeZ, targetX, targetY, targetZ, 0f, 1f, 0f)
        Matrix.multiplyMM(viewProjection, 0, projection, 0, view, 0)

        draw(gridBuffer, gridVertexCount, GLES20.GL_LINES, viewProjection, floatArrayOf(0.08f, 0.29f, 0.35f, 0.76f), 1f)
        draw(axisBuffer, axisVertexCount, GLES20.GL_LINES, viewProjection, floatArrayOf(0.16f, 0.63f, 0.74f, 0.92f), 2f)

        if (snapshot.frame.localKnown) {
            // The local origin is intentionally a visual anchor, not an
            // integrated client position. It only moves when server state does.
            drawActor(0f, 0f, 0f, heading, floatArrayOf(0.28f, 0.88f, 1f, 1f), true)
        } else {
            drawActor(0f, 0f, 0f, 0f, floatArrayOf(0.65f, 0.71f, 0.74f, 0.72f), true)
        }

        val localX = snapshot.frame.localX
        val localY = snapshot.frame.localY
        val localZ = snapshot.frame.localZ
        snapshot.frame.actors.forEach { actor ->
            val x = actor.x - localX
            val y = actor.z - localZ
            val z = -(actor.y - localY)
            if (x * x + z * z <= MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE) {
                val base = rgba(actor.rgba, if (actor.onFootKnown) 1f else 0.58f)
                drawActor(x, y, z, actor.angle, base, false)
            }
        }
        if (snapshot.frame.checkpoint.active) {
            drawMarker(
                snapshot.frame.checkpoint.x - localX,
                snapshot.frame.checkpoint.z - localZ,
                -(snapshot.frame.checkpoint.y - localY),
                snapshot.frame.checkpoint.size.coerceIn(2f, 18f),
                floatArrayOf(1f, 0.84f, 0.18f, 0.95f)
            )
        }
        if (snapshot.frame.raceCheckpoint.active) {
            drawMarker(
                snapshot.frame.raceCheckpoint.x - localX,
                snapshot.frame.raceCheckpoint.z - localZ,
                -(snapshot.frame.raceCheckpoint.y - localY),
                snapshot.frame.raceCheckpoint.size.coerceIn(2f, 18f),
                floatArrayOf(1f, 0.38f, 0.12f, 0.95f)
            )
        }
    }

    private fun drawActor(x: Float, y: Float, z: Float, angleDegrees: Float, color: FloatArray, local: Boolean) {
        Matrix.setIdentityM(model, 0)
        Matrix.translateM(model, 0, x, y, z)
        Matrix.rotateM(model, 0, -angleDegrees, 0f, 1f, 0f)
        val scale = if (local) 1.15f else 0.95f
        Matrix.scaleM(model, 0, scale, scale, scale)
        Matrix.multiplyMM(modelViewProjection, 0, viewProjection, 0, model, 0)
        draw(cubeBuffer, CUBE_VERTICES.size / 3, GLES20.GL_TRIANGLES, modelViewProjection, color, 1f)
        drawHeadingArrow(x, y, z, angleDegrees, if (local) floatArrayOf(0.9f, 0.98f, 1f, 1f) else color)
    }

    private fun drawHeadingArrow(x: Float, y: Float, z: Float, angleDegrees: Float, color: FloatArray) {
        val radians = Math.toRadians(angleDegrees.toDouble())
        val forwardX = sin(radians).toFloat()
        val forwardZ = -cos(radians).toFloat()
        val vertices = floatBuffer(floatArrayOf(
            x, y + 2.25f, z,
            x + forwardX * 1.7f, y + 2.25f, z + forwardZ * 1.7f
        ))
        draw(vertices, 2, GLES20.GL_LINES, viewProjection, color.copyOf().also { it[3] = 0.92f }, 2f)
    }

    private fun drawMarker(x: Float, y: Float, z: Float, radius: Float, color: FloatArray) {
        val count = 32
        val vertices = FloatArray(count * 3)
        for (i in 0 until count) {
            val theta = (Math.PI * 2.0 * i / count).toFloat()
            vertices[i * 3] = x + cos(theta.toDouble()).toFloat() * radius
            vertices[i * 3 + 1] = y + 0.08f
            vertices[i * 3 + 2] = z + sin(theta.toDouble()).toFloat() * radius
        }
        draw(floatBuffer(vertices), count, GLES20.GL_LINE_LOOP, viewProjection, color, 2.4f)
        val beacon = floatBuffer(floatArrayOf(
            x, y + 0.1f, z,
            x, y + max(4f, radius * 1.7f), z
        ))
        draw(beacon, 2, GLES20.GL_LINES, viewProjection, color, 2f)
    }

    private fun draw(
        vertices: FloatBuffer,
        count: Int,
        mode: Int,
        transform: FloatArray,
        color: FloatArray,
        lineWidth: Float
    ) {
        GLES20.glUseProgram(program)
        GLES20.glUniformMatrix4fv(mvpHandle, 1, false, transform, 0)
        GLES20.glUniform4fv(colorHandle, 1, color, 0)
        vertices.position(0)
        GLES20.glEnableVertexAttribArray(positionHandle)
        GLES20.glVertexAttribPointer(positionHandle, 3, GLES20.GL_FLOAT, false, 0, vertices)
        if (mode == GLES20.GL_LINES || mode == GLES20.GL_LINE_LOOP) GLES20.glLineWidth(lineWidth)
        GLES20.glDrawArrays(mode, 0, count)
        GLES20.glDisableVertexAttribArray(positionHandle)
    }

    private fun rgba(value: Long, alphaMultiplier: Float): FloatArray {
        if (value == 0L) return floatArrayOf(0.75f, 0.41f, 1f, alphaMultiplier)
        val r = ((value shr 24) and 0xFF).toFloat() / 255f
        val g = ((value shr 16) and 0xFF).toFloat() / 255f
        val b = ((value shr 8) and 0xFF).toFloat() / 255f
        val a = (value and 0xFF).toFloat() / 255f
        return floatArrayOf(r, g, b, max(0.22f, a) * alphaMultiplier)
    }

    private fun buildProgram(vertexSource: String, fragmentSource: String): Int {
        val vertex = compileShader(GLES20.GL_VERTEX_SHADER, vertexSource)
        val fragment = compileShader(GLES20.GL_FRAGMENT_SHADER, fragmentSource)
        if (vertex == 0 || fragment == 0) {
            if (vertex != 0) GLES20.glDeleteShader(vertex)
            if (fragment != 0) GLES20.glDeleteShader(fragment)
            return 0
        }
        val candidate = GLES20.glCreateProgram()
        if (candidate == 0) {
            GLES20.glDeleteShader(vertex)
            GLES20.glDeleteShader(fragment)
            return 0
        }
        GLES20.glAttachShader(candidate, vertex)
        GLES20.glAttachShader(candidate, fragment)
        GLES20.glLinkProgram(candidate)
        GLES20.glDeleteShader(vertex)
        GLES20.glDeleteShader(fragment)
        val status = IntArray(1)
        GLES20.glGetProgramiv(candidate, GLES20.GL_LINK_STATUS, status, 0)
        if (status[0] == 0) {
            GLES20.glDeleteProgram(candidate)
            return 0
        }
        return candidate
    }

    private fun compileShader(type: Int, source: String): Int {
        val shader = GLES20.glCreateShader(type)
        if (shader == 0) return 0
        GLES20.glShaderSource(shader, source)
        GLES20.glCompileShader(shader)
        val status = IntArray(1)
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, status, 0)
        if (status[0] == 0) {
            GLES20.glDeleteShader(shader)
            return 0
        }
        return shader
    }

    private data class SceneCameraState(
        val frame: WorldFrame,
        val userYawDegrees: Float,
        val userPitchDegrees: Float,
        val cameraDistance: Float
    )

    companion object {
        private const val MAX_RENDER_DISTANCE = 650f
        private const val VERTEX_SHADER = """
            uniform mat4 uMvp;
            attribute vec3 aPosition;
            void main() {
                gl_Position = uMvp * vec4(aPosition, 1.0);
            }
        """
        private const val FRAGMENT_SHADER = """
            precision mediump float;
            uniform vec4 uColor;
            void main() {
                gl_FragColor = uColor;
            }
        """
        // Base at y=0, top at y=2: a generated actor proxy, never a GTA model.
        private val CUBE_VERTICES = floatArrayOf(
            -0.48f, 0f, -0.38f,  0.48f, 0f, -0.38f,  0.48f, 2f, -0.38f,
            -0.48f, 0f, -0.38f,  0.48f, 2f, -0.38f, -0.48f, 2f, -0.38f,
            -0.48f, 0f,  0.38f, -0.48f, 2f,  0.38f,  0.48f, 2f,  0.38f,
            -0.48f, 0f,  0.38f,  0.48f, 2f,  0.38f,  0.48f, 0f,  0.38f,
            -0.48f, 0f, -0.38f, -0.48f, 2f, -0.38f, -0.48f, 2f,  0.38f,
            -0.48f, 0f, -0.38f, -0.48f, 2f,  0.38f, -0.48f, 0f,  0.38f,
             0.48f, 0f, -0.38f,  0.48f, 0f,  0.38f,  0.48f, 2f,  0.38f,
             0.48f, 0f, -0.38f,  0.48f, 2f,  0.38f,  0.48f, 2f, -0.38f,
            -0.48f, 2f, -0.38f,  0.48f, 2f, -0.38f,  0.48f, 2f,  0.38f,
            -0.48f, 2f, -0.38f,  0.48f, 2f,  0.38f, -0.48f, 2f,  0.38f,
            -0.48f, 0f, -0.38f,  0.48f, 0f,  0.38f,  0.48f, 0f, -0.38f,
            -0.48f, 0f, -0.38f, -0.48f, 0f,  0.38f,  0.48f, 0f,  0.38f
        )

        private fun makeGrid(radius: Float, step: Float): FloatArray {
            val lines = ((radius * 2f / step).toInt() + 1) * 2
            val result = FloatArray(lines * 2 * 3)
            var at = 0
            var value = -radius
            while (value <= radius + 0.01f) {
                result[at++] = value
                result[at++] = 0f
                result[at++] = -radius
                result[at++] = value
                result[at++] = 0f
                result[at++] = radius
                result[at++] = -radius
                result[at++] = 0f
                result[at++] = value
                result[at++] = radius
                result[at++] = 0f
                result[at++] = value
                value += step
            }
            return result
        }

        private fun floatBuffer(values: FloatArray): FloatBuffer =
            ByteBuffer.allocateDirect(values.size * 4)
                .order(ByteOrder.nativeOrder())
                .asFloatBuffer()
                .apply {
                    put(values)
                    position(0)
                }

        private fun normalizeDegrees(value: Float): Float {
            var normalized = value % 360f
            if (normalized > 180f) normalized -= 360f
            if (normalized < -180f) normalized += 360f
            return normalized
        }
    }
}
