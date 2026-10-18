package com.xykal.sampdroid.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.Button
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
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

/**
 * First visible world bridge: an Android-native, renderer-neutral tactical
 * surface. It draws only data that has already passed through native reducers;
 * it does not invent positions, collision, or client movement.
 */
object NativeWorldDialog {

    fun show(activity: AppCompatActivity) {
        val density = activity.resources.displayMetrics.density
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((12f * density).toInt(), (8f * density).toInt(), (12f * density).toInt(), 0)
            setBackgroundColor(Color.rgb(12, 23, 32))
        }
        val intro = TextView(activity).apply {
            text = "DUNIA NATIVE · RADAR ENTITY\n" +
                "Local player, remote stream-in, dan checkpoint dirender dari state native. " +
                "Belum renderer GTA 3D/collision; ini bukan log."
            setTextColor(Color.rgb(215, 237, 247))
            textSize = 12f
        }
        val surfaceHeight = max(
            (280f * density).toInt(),
            (activity.resources.displayMetrics.heightPixels * 0.54f).toInt()
        )
        val surface = NativeWorldSurfaceView(activity).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                surfaceHeight
            ).apply { topMargin = (8f * density).toInt() }
        }
        val controls = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER
            setPadding(0, (6f * density).toInt(), 0, 0)
        }
        fun addButton(label: String, onClick: () -> Unit) {
            controls.addView(Button(activity).apply {
                text = label
                textSize = 11f
                setOnClickListener { onClick() }
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                marginStart = (2f * density).toInt()
                marginEnd = (2f * density).toInt()
            })
        }
        addButton("− ZOOM") { surface.zoomBy(1.45f) }
        addButton("+ ZOOM") { surface.zoomBy(1f / 1.45f) }
        addButton("FOLLOW") { surface.followLocalPlayer() }
        addButton("KONTROL") { NativeControlsDialog.show(activity) }

        val status = TextView(activity).apply {
            setTextColor(Color.rgb(185, 246, 202))
            textSize = 11f
            setPadding(0, (5f * density).toInt(), 0, (5f * density).toInt())
            text = "Memuat snapshot dunia native…"
        }
        root.addView(intro)
        root.addView(surface)
        root.addView(controls)
        root.addView(status)

        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Dunia native")
            .setView(root)
            .setNegativeButton("Tutup", null)
            .create()
        var renderJob: Job? = null
        dialog.setOnShowListener {
            dialog.window?.setLayout(
                ViewGroup.LayoutParams.MATCH_PARENT,
                (activity.resources.displayMetrics.heightPixels * 0.88f).toInt()
            )
            renderJob = activity.lifecycleScope.launch {
                while (isActive && dialog.isShowing) {
                    val frame = runCatching {
                        val raw = withContext(Dispatchers.IO) {
                            NativeBridge.nativeSessionWorldSnapshot()
                        }
                        WorldFrame.from(JSONObject(raw))
                    }.getOrElse { WorldFrame(error = it.message ?: "snapshot world gagal") }
                    surface.submit(frame)
                    status.text = frame.statusLine()
                    // ServerDetailDialog owns the network poll; this loop only
                    // paints copies of reduced state and never blocks UI input.
                    delay(100)
                }
            }
        }
        dialog.setOnDismissListener { renderJob?.cancel() }
        dialog.show()
    }
}

private data class WorldActor(
    val id: Int,
    val rgba: Long,
    val model: Long,
    val onFootKnown: Boolean,
    val x: Float,
    val y: Float,
    val z: Float,
    val angle: Float,
    val health: Float,
    val armour: Float,
    val name: String
)

private data class WorldMarker(
    val active: Boolean = false,
    val x: Float = 0f,
    val y: Float = 0f,
    val z: Float = 0f,
    val size: Float = 0f
)

private data class WorldFrame(
    val active: Boolean = false,
    val localKnown: Boolean = false,
    val localFacingKnown: Boolean = false,
    val localId: Int = 0,
    val localX: Float = 0f,
    val localY: Float = 0f,
    val localZ: Float = 0f,
    val localAngle: Float = 0f,
    val checkpoint: WorldMarker = WorldMarker(),
    val raceCheckpoint: WorldMarker = WorldMarker(),
    val streamedPlayers: Int = 0,
    val remoteOnFootPlayers: Int = 0,
    val serverName: String = "",
    val actors: List<WorldActor> = emptyList(),
    val error: String = ""
) {
    companion object {
        fun from(o: JSONObject): WorldFrame {
            val local = o.optJSONObject("local") ?: JSONObject()
            fun marker(key: String): WorldMarker {
                val value = o.optJSONObject(key) ?: JSONObject()
                return WorldMarker(
                    active = value.optBoolean("active"),
                    x = value.optDouble("x").toFloat(),
                    y = value.optDouble("y").toFloat(),
                    z = value.optDouble("z").toFloat(),
                    size = value.optDouble("size").toFloat()
                )
            }
            val actors = buildList {
                val source = o.optJSONArray("actors")
                if (source != null) {
                    for (i in 0 until source.length()) {
                        val actor = source.optJSONObject(i)
                        if (actor != null) {
                            add(
                                WorldActor(
                                    id = actor.optInt("id"),
                                    rgba = actor.optLong("rgba"),
                                    model = actor.optLong("model"),
                                    onFootKnown = actor.optBoolean("onFootKnown"),
                                    x = actor.optDouble("x").toFloat(),
                                    y = actor.optDouble("y").toFloat(),
                                    z = actor.optDouble("z").toFloat(),
                                    angle = actor.optDouble("angle").toFloat(),
                                    health = actor.optDouble("health", 100.0).toFloat(),
                                    armour = actor.optDouble("armour").toFloat(),
                                    name = actor.optString("name")
                                )
                            )
                        }
                    }
                }
            }
            return WorldFrame(
                active = o.optBoolean("active"),
                localKnown = local.optBoolean("known"),
                localFacingKnown = local.optBoolean("facingKnown"),
                localId = local.optInt("id"),
                localX = local.optDouble("x").toFloat(),
                localY = local.optDouble("y").toFloat(),
                localZ = local.optDouble("z").toFloat(),
                localAngle = local.optDouble("angle").toFloat(),
                checkpoint = marker("checkpoint"),
                raceCheckpoint = marker("raceCheckpoint"),
                streamedPlayers = o.optInt("streamedPlayers"),
                remoteOnFootPlayers = o.optInt("remoteOnFootPlayers"),
                serverName = o.optString("serverName"),
                actors = actors,
                error = o.optString("error")
            )
        }
    }

    fun statusLine(): String = when {
        !active -> "Sesi belum aktif · buka Sesi native dari detail server."
        error.isNotBlank() -> "Sesi: $error"
        !localKnown -> "Sesi aktif · menunggu posisi/spawn lokal dari server."
        else -> "${actors.size}/${streamedPlayers} actor visual · $remoteOnFootPlayers on-foot · drag peta untuk geser kamera"
    }
}

private class NativeWorldSurfaceView(context: Context) : View(context) {

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val mapBounds = RectF()
    private var frame = WorldFrame()
    // Radius in GTA world units represented from the center to one map edge.
    private var worldRadius = 550f
    private var followLocal = true
    private var cameraOffsetX = 0f
    private var cameraOffsetY = 0f
    private var lastTouchX = 0f
    private var lastTouchY = 0f

    init {
        isClickable = true
        contentDescription = "Radar dunia native SampDroid. Drag untuk melihat area lain; gunakan tombol zoom dan follow."
    }

    fun submit(next: WorldFrame) {
        frame = next
        if (!next.localKnown && followLocal) {
            cameraOffsetX = 0f
            cameraOffsetY = 0f
        }
        invalidate()
    }

    fun zoomBy(multiplier: Float) {
        worldRadius = (worldRadius * multiplier).coerceIn(65f, 8000f)
        invalidate()
    }

    fun followLocalPlayer() {
        followLocal = true
        cameraOffsetX = 0f
        cameraOffsetY = 0f
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        canvas.drawColor(Color.rgb(8, 18, 25))
        if (width <= 0 || height <= 0) return

        val inset = dp(10f)
        mapBounds.set(inset, dp(34f), width - inset, height - inset)
        val centerX = mapBounds.centerX()
        val centerY = mapBounds.centerY()
        val scale = min(mapBounds.width(), mapBounds.height()) / (worldRadius * 2f)
        val originX = (if (frame.localKnown) frame.localX else 0f) + cameraOffsetX
        val originY = (if (frame.localKnown) frame.localY else 0f) + cameraOffsetY
        fun screenX(worldX: Float) = centerX + (worldX - originX) * scale
        fun screenY(worldY: Float) = centerY - (worldY - originY) * scale

        drawMapFrame(canvas)
        canvas.save()
        canvas.clipRect(mapBounds)
        drawGrid(canvas, originX, originY, scale, ::screenX, ::screenY)
        if (frame.checkpoint.active) {
            drawCheckpoint(canvas, screenX(frame.checkpoint.x), screenY(frame.checkpoint.y),
                max(dp(8f), frame.checkpoint.size * scale), Color.rgb(255, 222, 62), false)
        }
        if (frame.raceCheckpoint.active) {
            drawCheckpoint(canvas, screenX(frame.raceCheckpoint.x), screenY(frame.raceCheckpoint.y),
                max(dp(8f), frame.raceCheckpoint.size * scale), Color.rgb(255, 132, 54), true)
        }
        frame.actors.forEach { actor ->
            drawActor(canvas, actor, screenX(actor.x), screenY(actor.y))
        }
        if (frame.localKnown) {
            drawLocalActor(canvas, centerX + (-cameraOffsetX) * scale,
                centerY + cameraOffsetY * scale, frame.localAngle)
        }
        canvas.restore()
        drawHud(canvas, originX, originY)
    }

    private fun drawMapFrame(canvas: Canvas) {
        paint.style = Paint.Style.FILL
        paint.color = Color.rgb(15, 39, 51)
        canvas.drawRoundRect(mapBounds, dp(8f), dp(8f), paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1.5f)
        paint.color = Color.rgb(62, 140, 169)
        canvas.drawRoundRect(mapBounds, dp(8f), dp(8f), paint)
    }

    private fun drawGrid(
        canvas: Canvas,
        originX: Float,
        originY: Float,
        scale: Float,
        screenX: (Float) -> Float,
        screenY: (Float) -> Float
    ) {
        val step = when {
            worldRadius <= 150f -> 25f
            worldRadius <= 400f -> 50f
            worldRadius <= 1200f -> 100f
            worldRadius <= 3000f -> 250f
            else -> 500f
        }
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1f
        paint.color = Color.rgb(29, 75, 91)
        val minX = originX - worldRadius
        val maxX = originX + worldRadius
        var x = (floor((minX / step).toDouble()) * step.toDouble()).toFloat()
        while (x <= maxX) {
            val sx = screenX(x)
            canvas.drawLine(sx, mapBounds.top, sx, mapBounds.bottom, paint)
            x += step
        }
        val minY = originY - worldRadius
        val maxY = originY + worldRadius
        var y = (floor((minY / step).toDouble()) * step.toDouble()).toFloat()
        while (y <= maxY) {
            val sy = screenY(y)
            canvas.drawLine(mapBounds.left, sy, mapBounds.right, sy, paint)
            y += step
        }
        paint.strokeWidth = dp(1.2f)
        paint.color = Color.rgb(50, 123, 145)
        canvas.drawLine(screenX(0f), mapBounds.top, screenX(0f), mapBounds.bottom, paint)
        canvas.drawLine(mapBounds.left, screenY(0f), mapBounds.right, screenY(0f), paint)
    }

    private fun drawCheckpoint(canvas: Canvas, x: Float, y: Float, radius: Float, color: Int, diamond: Boolean) {
        if (!containsMapPoint(x, y)) return
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(2f)
        paint.color = color
        if (diamond) {
            val path = Path().apply {
                moveTo(x, y - radius)
                lineTo(x + radius, y)
                lineTo(x, y + radius)
                lineTo(x - radius, y)
                close()
            }
            canvas.drawPath(path, paint)
        } else {
            canvas.drawCircle(x, y, radius, paint)
            canvas.drawCircle(x, y, radius * 0.58f, paint)
        }
    }

    private fun drawActor(canvas: Canvas, actor: WorldActor, x: Float, y: Float) {
        if (!containsMapPoint(x, y)) return
        val color = colorFromRgba(actor.rgba)
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(if (actor.onFootKnown) 255 else 145, Color.red(color), Color.green(color), Color.blue(color))
        canvas.drawCircle(x, y, dp(if (actor.onFootKnown) 7f else 5f), paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1.5f)
        paint.color = Color.WHITE
        canvas.drawCircle(x, y, dp(if (actor.onFootKnown) 7f else 5f), paint)
        drawLabel(canvas, actor.name.ifBlank { "#${actor.id}" }, x + dp(9f), y - dp(9f), color)
    }

    private fun drawLocalActor(canvas: Canvas, x: Float, y: Float, angle: Float) {
        if (!containsMapPoint(x, y)) return
        canvas.save()
        canvas.rotate(angle, x, y)
        val size = dp(12f)
        val triangle = Path().apply {
            moveTo(x, y - size)
            lineTo(x + size * 0.7f, y + size)
            lineTo(x, y + size * 0.48f)
            lineTo(x - size * 0.7f, y + size)
            close()
        }
        paint.style = Paint.Style.FILL
        paint.color = Color.rgb(71, 224, 255)
        canvas.drawPath(triangle, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1.7f)
        paint.color = Color.WHITE
        canvas.drawPath(triangle, paint)
        canvas.restore()
        drawLabel(canvas, "YOU", x + dp(12f), y + dp(14f), Color.rgb(71, 224, 255))
    }

    private fun drawLabel(canvas: Canvas, text: String, x: Float, y: Float, color: Int) {
        val visible = text.take(14)
        paint.textSize = dp(10f)
        paint.textAlign = Paint.Align.LEFT
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(2.5f)
        paint.color = Color.argb(190, 0, 0, 0)
        canvas.drawText(visible, x, y, paint)
        paint.style = Paint.Style.FILL
        paint.color = color
        canvas.drawText(visible, x, y, paint)
    }

    private fun drawHud(canvas: Canvas, originX: Float, originY: Float) {
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(220, 4, 15, 21)
        canvas.drawRoundRect(RectF(mapBounds.left, dp(4f), mapBounds.right, dp(31f)), dp(6f), dp(6f), paint)
        paint.textAlign = Paint.Align.LEFT
        paint.textSize = dp(11f)
        paint.color = Color.rgb(194, 235, 249)
        val heading = if (frame.serverName.isBlank()) "WORLD / RADAR" else frame.serverName.take(22)
        canvas.drawText(heading, mapBounds.left + dp(9f), dp(22f), paint)
        paint.textAlign = Paint.Align.RIGHT
        paint.color = Color.rgb(169, 230, 187)
        canvas.drawText("${frame.actors.size} actor · ${worldRadius.toInt()}m", mapBounds.right - dp(9f), dp(22f), paint)

        paint.textAlign = Paint.Align.CENTER
        paint.textSize = dp(10f)
        paint.color = Color.rgb(185, 225, 238)
        canvas.drawText("N", mapBounds.centerX(), mapBounds.top + dp(14f), paint)
        canvas.drawText("E", mapBounds.right - dp(13f), mapBounds.centerY(), paint)
        canvas.drawText("S", mapBounds.centerX(), mapBounds.bottom - dp(7f), paint)
        canvas.drawText("W", mapBounds.left + dp(13f), mapBounds.centerY(), paint)

        paint.textAlign = Paint.Align.LEFT
        paint.textSize = dp(9f)
        paint.color = Color.rgb(141, 192, 211)
        val follow = if (followLocal) "FOLLOW" else "FREE CAMERA"
        canvas.drawText("$follow  x=${originX.toInt()} y=${originY.toInt()}", mapBounds.left + dp(8f), mapBounds.bottom - dp(8f), paint)
        if (!frame.localKnown) {
            paint.textAlign = Paint.Align.CENTER
            paint.textSize = dp(13f)
            paint.color = Color.rgb(255, 210, 92)
            canvas.drawText("MENUNGGU POSISI LOKAL", mapBounds.centerX(), mapBounds.centerY(), paint)
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastTouchX = event.x
                lastTouchY = event.y
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val scale = min(mapBounds.width(), mapBounds.height()) / (worldRadius * 2f)
                if (scale > 0f) {
                    // Moving the finger right means the camera looks left in
                    // world space; no player/network state is modified.
                    cameraOffsetX -= (event.x - lastTouchX) / scale
                    cameraOffsetY += (event.y - lastTouchY) / scale
                    followLocal = false
                    lastTouchX = event.x
                    lastTouchY = event.y
                    invalidate()
                }
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> return true
        }
        return super.onTouchEvent(event)
    }

    private fun containsMapPoint(x: Float, y: Float): Boolean =
        x >= mapBounds.left - dp(16f) && x <= mapBounds.right + dp(16f) &&
            y >= mapBounds.top - dp(16f) && y <= mapBounds.bottom + dp(16f)

    private fun colorFromRgba(rgba: Long): Int {
        if (rgba == 0L) return Color.rgb(190, 104, 255)
        val r = ((rgba shr 24) and 0xFF).toInt()
        val g = ((rgba shr 16) and 0xFF).toInt()
        val b = ((rgba shr 8) and 0xFF).toInt()
        val a = (rgba and 0xFF).toInt()
        return Color.argb(if (a < 48) 255 else a, r, g, b)
    }

    private fun dp(value: Float): Float = value * resources.displayMetrics.density
}
