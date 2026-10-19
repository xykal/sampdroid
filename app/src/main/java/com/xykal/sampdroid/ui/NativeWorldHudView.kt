package com.xykal.sampdroid.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffColorFilter
import android.graphics.RectF
import android.os.SystemClock
import android.view.View
import com.xykal.sampdroid.NativeBridge
import java.nio.ByteBuffer
import java.util.LinkedHashMap
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

/**
 * Lightweight HUD layered above the GPU scene. It renders only bounded copies
 * from WorldFrame: health/armour/money, server chat, local model metadata,
 * actor counts, server-camera status, plus bounded textdraws. Font-4 may use a
 * copied user-owned TXD sprite and font-5 a small copied prelit DFF mesh when
 * available; neither is claimed as full GTA frontend fidelity. This view owns
 * neither sockets nor player simulation; native revalidates any selectable tap.
 */
internal class NativeWorldHudView(context: Context) : View(context) {

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val panel = RectF()
    private val selectableTextDrawRects = mutableListOf<Pair<Int, RectF>>()
    private val respawnButtonRect = RectF()
    private var selectedPointerTextDrawId: Int? = null
    private var respawnPointerDown = false
    private var frame = WorldFrame()
    private var controlsEnabled = true

    // A style-4 textdraw is a real user-owned TXD sprite only after this
    // bounded cache has a decoded native copy. It never treats server text as
    // a path, network URL, or Android resource name.
    private data class TextDrawSpriteKey(val dictionary: String, val texture: String)
    private val textDrawSprites = object : LinkedHashMap<TextDrawSpriteKey, Bitmap>(
        MAX_TEXTDRAW_SPRITE_CACHE, 0.75f, true
    ) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<TextDrawSpriteKey, Bitmap>?): Boolean {
            val evict = size > MAX_TEXTDRAW_SPRITE_CACHE
            if (evict) eldest?.value?.recycle()
            return evict
        }
    }
    private val pendingTextDrawSprites = mutableSetOf<TextDrawSpriteKey>()
    private val spriteRetryAfterMs = mutableMapOf<TextDrawSpriteKey, Long>()

    // Font-5 keeps exact model/rotation/zoom wire facts. A tiny prelit DFF
    // mesh preview can be copied from the same user-owned catalog, but it is
    // consciously not labelled as GTA's full textdraw camera/material path.
    private data class TextDrawModelMesh(
        val vertices: FloatArray,
        val centerX: Float,
        val centerY: Float,
        val centerZ: Float,
        val extent: Float
    )
    private val textDrawModels = object : LinkedHashMap<Int, TextDrawModelMesh>(
        MAX_TEXTDRAW_MODEL_CACHE, 0.75f, true
    ) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<Int, TextDrawModelMesh>?): Boolean =
            size > MAX_TEXTDRAW_MODEL_CACHE
    }
    private val pendingTextDrawModels = mutableSetOf<Int>()
    private val modelRetryAfterMs = mutableMapOf<Int, Long>()
    // Canvas consumes arrays synchronously, so one UI-thread scratch pair
    // avoids per-frame allocations when several cached model textdraws redraw.
    private var textDrawModelScreenVertices = FloatArray(0)
    private var textDrawModelColours = IntArray(0)
    private var spriteExecutor: ExecutorService? = null
    private var spriteGeneration = 0L

    /** Emits only a copied raw wire ID; native rechecks selection state. */
    var onTextDrawSelected: ((Int) -> Unit)? = null

    /**
     * Requests the existing native RequestSpawn handshake. This HUD never
     * chooses a class, asserts a death packet, or treats a tap as a respawn.
     */
    var onRespawnRequested: (() -> Unit)? = null

    /** Optional GLES-to-Canvas hand-off for resolved server 3D labels. */
    var worldLabelProjection: WorldLabelProjection? = null

    init {
        // Return false for all non-selectable regions, preserving camera/touch
        // controls below this HUD. Selectable server textdraws and the native
        // respawn affordance are the only deliberate hit targets here.
        isClickable = true
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    fun submit(next: WorldFrame) {
        frame = next
        if (!next.textDrawSelectionActive) selectedPointerTextDrawId = null
        if (!isServerDead(next)) {
            respawnPointerDown = false
            respawnButtonRect.setEmpty()
        }
        invalidate()
    }

    fun setControlsEnabled(enabled: Boolean) {
        controlsEnabled = enabled
        invalidate()
    }

    override fun onDetachedFromWindow() {
        // Decodes are optional renderer work. Cancel queued sprite lookups when
        // this dialog/HUD leaves the view tree; no socket/session state lives
        // in this executor.
        spriteGeneration += 1L
        spriteExecutor?.shutdownNow()
        spriteExecutor = null
        pendingTextDrawSprites.clear()
        spriteRetryAfterMs.clear()
        pendingTextDrawModels.clear()
        modelRetryAfterMs.clear()
        textDrawModels.clear()
        textDrawModelScreenVertices = FloatArray(0)
        textDrawModelColours = IntArray(0)
        textDrawSprites.values.forEach { bitmap ->
            if (!bitmap.isRecycled) bitmap.recycle()
        }
        textDrawSprites.clear()
        super.onDetachedFromWindow()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        if (width <= 0 || height <= 0) return

        drawHeader(canvas)
        drawVitals(canvas)
        drawAimReticle(canvas)
        drawGameText(canvas)
        drawChat(canvas)
        drawProjectedWorldLabels(canvas)
        drawTextDraws(canvas)
        // Keep the native death action visually above server textdraw proxies;
        // input handling applies the same priority.
        drawRespawnPrompt(canvas)
        drawFooter(canvas)
    }

    private fun drawHeader(canvas: Canvas) {
        val title = frame.serverName.ifBlank { "SAMPDROID NATIVE" }.take(34)
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(190, 2, 12, 18)
        panel.set(dp(8f), dp(8f), min(width - dp(8f), dp(274f)), dp(52f))
        canvas.drawRoundRect(panel, dp(6f), dp(6f), paint)
        paint.textAlign = Paint.Align.LEFT
        paint.textSize = dp(12f)
        paint.color = Color.rgb(220, 244, 250)
        canvas.drawText(title, panel.left + dp(8f), panel.top + dp(17f), paint)
        paint.textSize = dp(9f)
        paint.color = Color.rgb(135, 205, 225)
        val state = when {
            !frame.active -> "SESI OFFLINE"
            frame.spectate.active -> when {
                frame.spectate.targetKnown && frame.sceneAnchor().followingSpectateTarget &&
                    frame.spectate.targetVehicle -> "SPECTATOR · VEHICLE #${frame.spectate.targetId}"
                frame.spectate.targetKnown && frame.sceneAnchor().followingSpectateTarget ->
                    "SPECTATOR · PLAYER #${frame.spectate.targetId}"
                frame.spectate.targetKnown -> "SPECTATOR · TARGET BELUM STREAMED"
                else -> "SPECTATOR · MENUNGGU TARGET"
            }
            isServerDead() -> if (frame.spawnRequestPending) {
                "TUMBANG · REQUEST SPAWN"
            } else {
                "TUMBANG · RESPAWN"
            }
            !frame.localKnown -> "MENUNGGU SPAWN"
            frame.localControllableKnown && !frame.localControllable -> "SERVER FREEZE"
            frame.camera.serverControlled -> when {
                frame.camera.interpolatingPosition || frame.camera.interpolatingLookAt -> "KAMERA SERVER · INTERPOLASI"
                else -> "KAMERA SERVER"
            }
            frame.localVehicleKnown && frame.localInVehicle && frame.localVehicleSeat != 0 ->
                "PASSENGER #${frame.localVehicleId}"
            frame.localVehicleKnown && frame.localInVehicle && frame.localPredictionActive ->
                "DRIVER #${frame.localVehicleId} · PACKET-200"
            frame.localVehicleKnown && frame.localInVehicle -> "DRIVER #${frame.localVehicleId} · MENUNGGU COL"
            frame.localPredictionActive && frame.localGrounded -> "LIVE · GERAK COLLISION"
            frame.localPredictionActive -> "LIVE · LOMPAT COLLISION"
            else -> "LIVE · ${frame.actors.size} P · ${frame.serverActors.size} NPC · ${frame.vehicles.size} V · ${frame.playerAttachedObjects.size} A"
        }
        val local = if (frame.localSpawnInfoKnown) " · SKIN ${frame.localModel}" else ""
        val environment = buildString {
            if (frame.localInteriorKnown) append(" · INT ${frame.localInterior}")
            if (frame.localVirtualWorldKnown) append(" · VW ${frame.localVirtualWorld}")
            if (frame.localArmedWeaponKnown) {
                append(" · WPN ${frame.localArmedWeapon}")
                if (frame.localArmedAmmoKnown) append("/${frame.localArmedAmmo}")
            }
            val clockVisible = !frame.playerClockKnown || frame.playerClockEnabled
            if (clockVisible && frame.playerTimeKnown) {
                append(" · ${frame.playerHour.toString().padStart(2, '0')}:${frame.playerMinute.toString().padStart(2, '0')}")
            } else if (clockVisible && frame.worldTimeKnown) {
                append(" · ${frame.worldTime.toString().padStart(2, '0')}:00")
            }
            if (frame.weatherKnown) append(" · WX ${frame.weather}")
            if (frame.audioStream.active) {
                append(if (frame.audioStream.playable) " · AUD HTTPS" else " · AUD BLOCK")
            }
            if (frame.localForcedAnimation.active) {
                // A directive indicator only: the current renderer has no GTA
                // IFP skeleton executor and does not claim local animation play.
                append(" · ANIM ${frame.localForcedAnimation.name.take(12)}")
            }
            if (frame.gravityKnown) append(" · G ${String.format(java.util.Locale.US, "%.3f", frame.gravity)}")
            // RPC 17 is an exterior-only native prediction gate. This compact
            // indicator reports that a server rectangle exists; it is not a
            // claim of a collision, push-back, or server movement confirmation.
            if (frame.worldBoundsKnown) append(" · BND EXT")
        }
        canvas.drawText("$state$local$environment", panel.left + dp(8f), panel.top + dp(34f), paint)
    }

    private fun drawVitals(canvas: Canvas) {
        val widthPx = min(dp(150f), width * 0.43f)
        val left = width - widthPx - dp(8f)
        val top = dp(8f)
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(190, 2, 12, 18)
        panel.set(left, top, width - dp(8f), top + dp(96f))
        canvas.drawRoundRect(panel, dp(6f), dp(6f), paint)
        val hp = if (frame.localHealthKnown) frame.localHealth else 0f
        val armour = if (frame.localArmourKnown) frame.localArmour else 0f
        drawVitalBar(
            canvas,
            left + dp(8f),
            top + dp(10f),
            widthPx - dp(16f),
            hp,
            frame.localHealthKnown,
            "HP",
            Color.rgb(96, 230, 128)
        )
        drawVitalBar(
            canvas,
            left + dp(8f),
            top + dp(33f),
            widthPx - dp(16f),
            armour,
            frame.localArmourKnown,
            "ARMOUR",
            Color.rgb(78, 174, 255)
        )
        drawMoney(canvas, left + dp(8f), top + dp(61f))
        drawWantedLevel(canvas, left + dp(8f), top + dp(82f))
    }

    private fun drawRespawnPrompt(canvas: Canvas) {
        if (!isServerDead(frame)) {
            respawnButtonRect.setEmpty()
            return
        }
        val cardWidth = min(dp(236f), width - dp(32f))
        val cardHeight = dp(112f)
        val left = (width - cardWidth) * 0.5f
        val top = height * 0.40f - cardHeight * 0.5f
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(232, 28, 4, 9)
        panel.set(left, top, left + cardWidth, top + cardHeight)
        canvas.drawRoundRect(panel, dp(10f), dp(10f), paint)

        paint.textAlign = Paint.Align.CENTER
        paint.typeface = android.graphics.Typeface.DEFAULT_BOLD
        paint.textSize = dp(17f)
        paint.color = Color.rgb(255, 144, 144)
        canvas.drawText("KAMU TUMBANG", width * 0.5f, top + dp(28f), paint)
        paint.typeface = android.graphics.Typeface.DEFAULT
        paint.textSize = dp(10f)
        paint.color = Color.rgb(232, 223, 225)
        val detail = if (frame.spawnRequestPending) {
            "Menunggu jawaban spawn dari server…"
        } else {
            "Server menetapkan HP 0 · minta respawn"
        }
        canvas.drawText(detail, width * 0.5f, top + dp(48f), paint)

        respawnButtonRect.set(
            left + dp(20f),
            top + dp(61f),
            left + cardWidth - dp(20f),
            top + dp(98f)
        )
        paint.color = if (frame.spawnRequestPending) {
            Color.rgb(73, 87, 98)
        } else if (respawnPointerDown) {
            Color.rgb(192, 47, 55)
        } else {
            Color.rgb(226, 64, 70)
        }
        canvas.drawRoundRect(respawnButtonRect, dp(6f), dp(6f), paint)
        paint.textSize = dp(12f)
        paint.typeface = android.graphics.Typeface.DEFAULT_BOLD
        paint.color = Color.WHITE
        canvas.drawText(
            if (frame.spawnRequestPending) "MENUNGGU SERVER" else "RESPAWN",
            respawnButtonRect.centerX(),
            respawnButtonRect.centerY() + dp(4f),
            paint
        )
        paint.typeface = android.graphics.Typeface.DEFAULT
    }

    private fun drawWantedLevel(canvas: Canvas, x: Float, baseline: Float) {
        paint.style = Paint.Style.FILL
        paint.textAlign = Paint.Align.LEFT
        paint.textSize = dp(11f)
        val level = frame.localWantedLevel.coerceIn(0, 6)
        paint.color = when {
            !frame.localWantedLevelKnown -> Color.rgb(176, 192, 202)
            level == 0 -> Color.rgb(137, 171, 184)
            else -> Color.rgb(255, 205, 72)
        }
        val stars = if (frame.localWantedLevelKnown) {
            "★".repeat(level) + "☆".repeat(6 - level)
        } else {
            "------"
        }
        canvas.drawText("WANTED $stars", x, baseline, paint)
    }

    private fun drawAimReticle(canvas: Canvas) {
        if (!controlsEnabled || frame.spectate.active || isServerDead() || !frame.localAimHeld) return
        // This is a presentation-only reticle for the held semantic AIM state.
        // It intentionally has no entity target marker or server-hit cue. A
        // brief COL pulse means only that native socket-send accepted a close
        // static user-data contact; it never means server receipt/damage/ammo.
        // Passenger drive-by is only a server-seat + server-armed-SMG Packet-211
        // presentation state; neither path accepts a Kotlin-selected target.
        val centerX = width * 0.5f
        val centerY = height * 0.5f
        val ready = frame.localAimSyncReady
        val passengerDriveBy = frame.localInVehicle && frame.localPassengerDriveByActive
        val fireBlockedByLagComp = ready && frame.localFireHeld && !frame.bulletSyncServerEnabled
        val staticWorldContact = ready && frame.localFireHeld && frame.bulletSyncServerEnabled &&
            frame.staticWorldContactActive
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1.6f)
        paint.color = when {
            fireBlockedByLagComp -> Color.rgb(255, 188, 92)
            staticWorldContact -> Color.rgb(174, 255, 120)
            passengerDriveBy -> Color.rgb(222, 158, 255)
            ready -> Color.rgb(125, 239, 255)
            else -> Color.rgb(255, 188, 92)
        }
        val gap = dp(4f)
        val arm = dp(13f)
        canvas.drawLine(centerX - arm, centerY, centerX - gap, centerY, paint)
        canvas.drawLine(centerX + gap, centerY, centerX + arm, centerY, paint)
        canvas.drawLine(centerX, centerY - arm, centerX, centerY - gap, paint)
        canvas.drawLine(centerX, centerY + gap, centerX, centerY + arm, paint)
        canvas.drawCircle(centerX, centerY, dp(1.2f), paint)

        paint.style = Paint.Style.FILL
        paint.textAlign = Paint.Align.CENTER
        paint.textSize = dp(9f)
        val state = when {
            ready && frame.localFireHeld && !frame.bulletSyncServerEnabled ->
                "AIM+FIRE · lagcomp server OFF"
            staticWorldContact -> "AIM+FIRE · COL dekat (lokal)"
            passengerDriveBy && frame.localFireHeld -> "DRIVE-BY · SMG + FIRE"
            passengerDriveBy -> "DRIVE-BY · SMG siap"
            ready && frame.localFireHeld -> "AIM+FIRE · Packet-206 no-target"
            ready -> "AIM · Packet-203 siap"
            frame.camera.serverControlled -> "AIM · kamera server"
            frame.localInVehicle -> "AIM · passenger SMG diperlukan"
            else -> "AIM · menunggu collision/weapon"
        }
        val sendDiagnostic = if (ready && frame.localFireHeld && frame.bulletSyncServerEnabled) {
            " · kirim #${frame.outboundNoTargetBulletPackets}"
        } else {
            ""
        }
        canvas.drawText(
            "$state$sendDiagnostic · ${String.format(java.util.Locale.US, "%+.0f°", frame.localAimPitchDegrees)}",
            centerX,
            centerY + dp(28f),
            paint
        )
    }

    private fun drawMoney(canvas: Canvas, x: Float, baseline: Float) {
        paint.style = Paint.Style.FILL
        paint.textAlign = Paint.Align.LEFT
        paint.textSize = dp(11f)
        paint.color = when {
            !frame.localMoneyKnown -> Color.rgb(176, 192, 202)
            frame.localMoney < 0L -> Color.rgb(255, 132, 112)
            else -> Color.rgb(126, 238, 145)
        }
        val value = if (frame.localMoneyKnown) "${'$'}${frame.localMoney}" else "${'$'}--"
        canvas.drawText("MONEY $value", x, baseline, paint)
    }

    private fun drawVitalBar(
        canvas: Canvas,
        x: Float,
        y: Float,
        barWidth: Float,
        value: Float,
        known: Boolean,
        label: String,
        color: Int
    ) {
        val h = dp(11f)
        paint.style = Paint.Style.FILL
        paint.color = Color.rgb(25, 48, 59)
        canvas.drawRoundRect(RectF(x, y, x + barWidth, y + h), h / 2f, h / 2f, paint)
        if (known) {
            paint.color = color
            val filled = (barWidth * (value / 100f).coerceIn(0f, 1f)).coerceAtLeast(dp(1f))
            canvas.drawRoundRect(RectF(x, y, x + filled, y + h), h / 2f, h / 2f, paint)
        }
        paint.textAlign = Paint.Align.LEFT
        paint.textSize = dp(8f)
        paint.color = Color.WHITE
        val display = if (known) "$label ${value.toInt()}" else "$label --"
        canvas.drawText(display, x + dp(4f), y + h - dp(2f), paint)
    }

    private fun drawGameText(canvas: Canvas) {
        val gameText = frame.gameText
        if (!gameText.active) return
        // SA-MP colour markup is not an Android font/style implementation. Keep
        // common colour tokens from becoming visible litter while preserving a
        // bounded readable text/banner proxy and native timing authority.
        val normalized = sanitize(gameText.text)
            .replace(GAME_TEXT_COLOR_TOKEN, "")
            .replace(GAME_TEXT_NEWLINE_TOKEN, "\n")
            .take(MAX_GAMETEXT_CHARS)
            .trim()
        if (normalized.isEmpty()) return
        val lines = normalized.lineSequence().take(MAX_GAMETEXT_LINES).map { it.take(40) }.toList()
        if (lines.isEmpty()) return
        val size = when (gameText.style) {
            0, 1 -> dp(22f)
            2, 3 -> dp(18f)
            else -> dp(15f)
        }
        paint.typeface = android.graphics.Typeface.DEFAULT_BOLD
        paint.textAlign = Paint.Align.CENTER
        paint.textSize = size
        paint.style = Paint.Style.FILL
        val lineHeight = size * 1.12f
        val centerX = width / 2f
        val firstBaseline = height * 0.20f
        lines.forEachIndexed { index, line ->
            val baseline = firstBaseline + index * lineHeight
            paint.color = Color.argb(210, 0, 0, 0)
            canvas.drawText(line, centerX + dp(1.5f), baseline + dp(1.5f), paint)
            paint.color = Color.argb(245, 245, 245, 238)
            canvas.drawText(line, centerX, baseline, paint)
        }
        paint.typeface = android.graphics.Typeface.DEFAULT
    }

    private fun drawChat(canvas: Canvas) {
        if (frame.chat.isEmpty()) return
        val left = dp(10f)
        var baseline = dp(73f)
        frame.chat.takeLast(MAX_VISIBLE_CHAT).forEach { line ->
            val text = sanitize(line.text).take(MAX_CHAT_CHARS)
            if (text.isEmpty()) return@forEach
            paint.textSize = dp(11f)
            val textWidth = min(width - left - dp(16f), paint.measureText(text) + dp(14f))
            paint.style = Paint.Style.FILL
            paint.color = Color.argb(175, 1, 10, 15)
            panel.set(left, baseline - dp(14f), left + textWidth, baseline + dp(5f))
            canvas.drawRoundRect(panel, dp(4f), dp(4f), paint)
            paint.color = colorFromRgba(line.rgba)
            canvas.drawText(text, left + dp(6f), baseline, paint)
            baseline += dp(21f)
        }
    }

    /**
     * Canvas typography is used only after GLES projects a bounded decoded
     * PlayerShowTextLabel anchor, server-expiry-filtered chat bubble, or
     * policy-filtered player nametag into this exact overlay coordinate space.
     * It does not claim GTA font/LOS fidelity
     * and never owns attachment state.
     */
    private fun drawProjectedWorldLabels(canvas: Canvas) {
        val labels = worldLabelProjection?.snapshot().orEmpty()
        if (labels.isEmpty()) return
        labels.take(MAX_PROJECTED_WORLD_LABELS).forEach { label ->
            if (!label.x.isFinite() || !label.y.isFinite()) return@forEach
            val text = sanitize(label.text).replace(WORLD_LABEL_WHITESPACE, " ").take(MAX_WORLD_LABEL_CHARS)
            if (text.isBlank()) return@forEach
            val x = label.x.coerceIn(-width * 0.2f, width * 1.2f)
            val y = label.y.coerceIn(-height * 0.2f, height * 1.2f)
            val color = colorFromRgba(label.rgba)
            paint.typeface = android.graphics.Typeface.DEFAULT_BOLD
            paint.textAlign = Paint.Align.CENTER
            paint.textSize = dp(10f)
            val textWidth = paint.measureText(text)
            paint.style = Paint.Style.FILL
            paint.color = Color.argb(178, 0, 0, 0)
            panel.set(x - textWidth * 0.5f - dp(4f), y - dp(20f), x + textWidth * 0.5f + dp(4f), y - dp(5f))
            canvas.drawRoundRect(panel, dp(3f), dp(3f), paint)
            paint.color = Color.argb(Color.alpha(color).coerceAtLeast(168), Color.red(color), Color.green(color), Color.blue(color))
            canvas.drawText(text, x, y - dp(8f), paint)
            paint.typeface = android.graphics.Typeface.DEFAULT
        }
    }

    private fun textDrawSpriteKey(rawText: String): TextDrawSpriteKey? {
        // Font-4 text is conventionally `txd:texture`. Keep the same 31-byte,
        // printable no-separator token surface enforced again by native TXD
        // lookup; color tags, paths, and multi-colon strings are not sprites.
        val token = rawText.trim()
        val separator = token.indexOf(':')
        if (separator <= 0 || separator != token.lastIndexOf(':')) return null
        val dictionary = token.substring(0, separator)
        val texture = token.substring(separator + 1)
        val valid = { value: String ->
            value.length in 1..31 && value.none { character ->
                character.code !in 0x20..0x7e || character == '/' ||
                    character == '\\' || character == ':' || character == '.'
            }
        }
        if (!valid(dictionary) || !valid(texture)) return null
        return TextDrawSpriteKey(
            dictionary.lowercase(Locale.US),
            texture.lowercase(Locale.US)
        )
    }

    private fun requestTextDrawSprite(key: TextDrawSpriteKey) {
        if (key in textDrawSprites || key in pendingTextDrawSprites) return
        val now = SystemClock.uptimeMillis()
        if (now < (spriteRetryAfterMs[key] ?: 0L)) return
        pendingTextDrawSprites += key
        val requestGeneration = spriteGeneration
        val executor = spriteExecutor ?: Executors.newSingleThreadExecutor { runnable ->
            Thread(runnable, "SampDroidTextDrawSprite").apply { isDaemon = true }
        }.also { spriteExecutor = it }
        executor.execute {
            val preview = runCatching {
                NativeBridge.nativeGameDataTextDrawSpritePreview(
                    key.dictionary,
                    key.texture,
                    MAX_TEXTDRAW_SPRITE_PIXELS
                )
            }.getOrNull()
            val bitmap = preview?.let { source ->
                val expected = source.textureWidth.toLong() * source.textureHeight.toLong() * 4L
                if (source.textureWidth !in 1..MAX_TEXTDRAW_SPRITE_DIMENSION ||
                    source.textureHeight !in 1..MAX_TEXTDRAW_SPRITE_DIMENSION ||
                    expected != source.textureRgba.size.toLong()
                ) {
                    null
                } else {
                    runCatching {
                        Bitmap.createBitmap(
                            source.textureWidth,
                            source.textureHeight,
                            Bitmap.Config.ARGB_8888
                        ).also { image ->
                            image.copyPixelsFromBuffer(ByteBuffer.wrap(source.textureRgba))
                        }
                    }.getOrNull()
                }
            }
            post {
                if (requestGeneration != spriteGeneration) {
                    if (bitmap != null && !bitmap.isRecycled) bitmap.recycle()
                    return@post
                }
                pendingTextDrawSprites -= key
                if (bitmap != null) {
                    textDrawSprites[key] = bitmap
                    spriteRetryAfterMs.remove(key)
                } else {
                    // A mounted user-data catalog can change later, but an
                    // absent/unsupported sprite must not trigger native IMG
                    // work every 10 Hz world snapshot.
                    spriteRetryAfterMs[key] = SystemClock.uptimeMillis() + SPRITE_RETRY_MS
                }
                invalidate()
            }
        }
    }

    private fun createTextDrawModelMesh(raw: FloatArray): TextDrawModelMesh? {
        if (raw.isEmpty() || raw.size % DFF_PREVIEW_FLOATS_PER_VERTEX != 0 ||
            raw.size / DFF_PREVIEW_FLOATS_PER_VERTEX > MAX_TEXTDRAW_MODEL_VERTICES
        ) return null
        var minX = Float.POSITIVE_INFINITY
        var minY = Float.POSITIVE_INFINITY
        var minZ = Float.POSITIVE_INFINITY
        var maxX = Float.NEGATIVE_INFINITY
        var maxY = Float.NEGATIVE_INFINITY
        var maxZ = Float.NEGATIVE_INFINITY
        var offset = 0
        while (offset < raw.size) {
            val x = raw[offset]
            val y = raw[offset + 1]
            val z = raw[offset + 2]
            val red = raw[offset + 3]
            val green = raw[offset + 4]
            val blue = raw[offset + 5]
            val alpha = raw[offset + 6]
            if (!x.isFinite() || !y.isFinite() || !z.isFinite() ||
                !red.isFinite() || !green.isFinite() || !blue.isFinite() || !alpha.isFinite() ||
                red !in 0f..1f || green !in 0f..1f || blue !in 0f..1f || alpha !in 0f..1f
            ) return null
            minX = min(minX, x)
            minY = min(minY, y)
            minZ = min(minZ, z)
            maxX = max(maxX, x)
            maxY = max(maxY, y)
            maxZ = max(maxZ, z)
            offset += DFF_PREVIEW_FLOATS_PER_VERTEX
        }
        val extent = max(max(maxX - minX, maxY - minY), maxZ - minZ)
        if (!extent.isFinite() || extent < 0.0001f || extent > 100_000f) return null
        return TextDrawModelMesh(
            vertices = raw,
            centerX = (minX + maxX) * 0.5f,
            centerY = (minY + maxY) * 0.5f,
            centerZ = (minZ + maxZ) * 0.5f,
            extent = extent
        )
    }

    private fun requestTextDrawModel(model: Int) {
        // ID 0 is CJ in GTA's ped catalog and is intentionally valid in the
        // generic dynamic-DFF bridge; an unknown zero still fails closed there.
        if (model !in 0..65535 || model in textDrawModels || model in pendingTextDrawModels) return
        val now = SystemClock.uptimeMillis()
        if (now < (modelRetryAfterMs[model] ?: 0L)) return
        pendingTextDrawModels += model
        val requestGeneration = spriteGeneration
        val executor = spriteExecutor ?: Executors.newSingleThreadExecutor { runnable ->
            Thread(runnable, "SampDroidTextDrawAsset").apply { isDaemon = true }
        }.also { spriteExecutor = it }
        executor.execute {
            // Reuse the bounded dynamic-model bridge. Model transforms are all
            // neutral here; Canvas applies the server's font-5 preview rotation
            // and zoom only as a display transform, never as world gameplay.
            val raw = runCatching {
                NativeBridge.nativeGameDataServerObjectsPreview(
                    0f, 0f, 0f, TEXTDRAW_MODEL_RADIUS,
                    intArrayOf(model),
                    floatArrayOf(
                        0f, 0f, 0f,
                        0f, 0f, 0f,
                        TEXTDRAW_MODEL_RADIUS,
                        1f, 1f, 1f
                    ),
                    MAX_TEXTDRAW_MODEL_VERTICES
                )
            }.getOrElse { FloatArray(0) }
            val mesh = createTextDrawModelMesh(raw)
            post {
                if (requestGeneration != spriteGeneration) return@post
                pendingTextDrawModels -= model
                if (mesh != null) {
                    textDrawModels[model] = mesh
                    modelRetryAfterMs.remove(model)
                } else {
                    modelRetryAfterMs[model] = SystemClock.uptimeMillis() + MODEL_RETRY_MS
                }
                invalidate()
            }
        }
    }

    private fun drawTextDrawModelPreview(
        canvas: Canvas,
        mesh: TextDrawModelMesh,
        draw: WorldTextDraw,
        bounds: RectF
    ) {
        if (bounds.width() < 2f || bounds.height() < 2f) return
        // This is an actual prelit DFF triangle copy, projected by a small
        // Canvas camera. It intentionally does not claim exact GTA textdraw
        // lighting, material/texture, vehicle colours, or frontend camera.
        val vertexCount = mesh.vertices.size / DFF_PREVIEW_FLOATS_PER_VERTEX
        if (textDrawModelScreenVertices.size < vertexCount * 2) {
            textDrawModelScreenVertices = FloatArray(vertexCount * 2)
        }
        if (textDrawModelColours.size < vertexCount) {
            textDrawModelColours = IntArray(vertexCount)
        }
        val screenVertices = textDrawModelScreenVertices
        val colours = textDrawModelColours
        val xRad = Math.toRadians(draw.rotationX.toDouble())
        val yRad = Math.toRadians(draw.rotationY.toDouble())
        val zRad = Math.toRadians(draw.rotationZ.toDouble())
        val cosX = cos(xRad).toFloat()
        val sinX = sin(xRad).toFloat()
        val cosY = cos(yRad).toFloat()
        val sinY = sin(yRad).toFloat()
        val cosZ = cos(zRad).toFloat()
        val sinZ = sin(zRad).toFloat()
        val zoom = if (draw.zoom > 0.001f) draw.zoom.coerceIn(0.15f, 6f) else 1f
        val scale = min(bounds.width(), bounds.height()) * zoom / (mesh.extent * 2.8f)
        val cameraDistance = mesh.extent * 2.2f
        var source = 0
        var output = 0
        while (source < mesh.vertices.size) {
            var x = mesh.vertices[source] - mesh.centerX
            var y = mesh.vertices[source + 1] - mesh.centerY
            var z = mesh.vertices[source + 2] - mesh.centerZ
            val yAfterX = y * cosX - z * sinX
            val zAfterX = y * sinX + z * cosX
            val xAfterY = x * cosY + zAfterX * sinY
            val zAfterY = -x * sinY + zAfterX * cosY
            val xAfterZ = xAfterY * cosZ - yAfterX * sinZ
            val yAfterZ = xAfterY * sinZ + yAfterX * cosZ
            val perspective = (cameraDistance / (cameraDistance + zAfterY)).coerceIn(0.25f, 4f)
            screenVertices[output] = bounds.centerX() + xAfterZ * scale * perspective
            screenVertices[output + 1] = bounds.centerY() - yAfterZ * scale * perspective
            val red = (mesh.vertices[source + 3] * 255f).toInt().coerceIn(0, 255)
            val green = (mesh.vertices[source + 4] * 255f).toInt().coerceIn(0, 255)
            val blue = (mesh.vertices[source + 5] * 255f).toInt().coerceIn(0, 255)
            val alpha = (mesh.vertices[source + 6] * 220f).toInt().coerceIn(0, 220)
            colours[output / 2] = Color.argb(alpha, red, green, blue)
            source += DFF_PREVIEW_FLOATS_PER_VERTEX
            output += 2
        }
        canvas.save()
        canvas.clipRect(bounds)
        paint.style = Paint.Style.FILL
        paint.colorFilter = null
        paint.alpha = 255
        @Suppress("DEPRECATION")
        canvas.drawVertices(
            Canvas.VertexMode.TRIANGLES,
            vertexCount * 2,
            screenVertices,
            0,
            null,
            0,
            colours,
            0,
            null,
            0,
            0,
            paint
        )
        canvas.restore()
    }

    private fun drawTextDraws(canvas: Canvas) {
        selectableTextDrawRects.clear()
        if (frame.textDraws.isEmpty()) return
        frame.textDraws.forEach { draw ->
            if (!draw.x.isFinite() || !draw.y.isFinite()) return@forEach
            val x = (draw.x / TEXTDRAW_VIRTUAL_WIDTH * width).coerceIn(-width.toFloat(), width * 2f)
            val y = (draw.y / TEXTDRAW_VIRTUAL_HEIGHT * height).coerceIn(-height.toFloat(), height * 2f)
            val rawText = sanitize(draw.text).take(MAX_TEXTDRAW_CHARS)
            val spriteKey = if (draw.style == TEXTDRAW_STYLE_SPRITE) textDrawSpriteKey(rawText) else null
            val sprite = spriteKey?.let { key ->
                textDrawSprites[key] ?: run {
                    requestTextDrawSprite(key)
                    null
                }
            }
            val modelMesh = if (draw.style == TEXTDRAW_STYLE_MODEL && draw.model in 0L..65535L) {
                val modelId = draw.model.toInt()
                textDrawModels[modelId] ?: run {
                    requestTextDrawModel(modelId)
                    null
                }
            } else {
                null
            }
            val displayText = when {
                draw.style <= TEXTDRAW_STYLE_PRICEDOWN -> rawText
                draw.style == TEXTDRAW_STYLE_SPRITE && sprite != null -> ""
                draw.style == TEXTDRAW_STYLE_SPRITE && spriteKey != null ->
                    "[SPRITE ${spriteKey.dictionary}:${spriteKey.texture}]"
                draw.style == TEXTDRAW_STYLE_SPRITE && rawText.isBlank() -> "[SPRITE KOSONG]"
                draw.style == TEXTDRAW_STYLE_SPRITE -> "[SPRITE INVALID]"
                draw.style == TEXTDRAW_STYLE_MODEL && modelMesh != null -> ""
                draw.style == TEXTDRAW_STYLE_MODEL && rawText.isBlank() ->
                    "[MODEL #${draw.model} PREVIEW BELUM ADA]"
                draw.style == TEXTDRAW_STYLE_MODEL ->
                    "[MODEL #${draw.model} PREVIEW BELUM ADA] $rawText"
                rawText.isBlank() -> "[TEXTDRAW STYLE ${draw.style}]"
                else -> "[STYLE ${draw.style}] $rawText"
            }
            if (displayText.isBlank() && !draw.useBox && sprite == null && modelMesh == null) return@forEach

            val fontSize = (draw.letterHeight * dp(15f)).coerceIn(dp(8f), dp(34f))
            paint.typeface = android.graphics.Typeface.DEFAULT
            paint.textSize = fontSize
            paint.style = Paint.Style.FILL
            paint.textAlign = when (draw.alignment) {
                2 -> Paint.Align.CENTER
                3 -> Paint.Align.RIGHT
                else -> Paint.Align.LEFT
            }
            val measuredWidth = paint.measureText(displayText)
            val lineHeight = fontSize * 1.18f
            val textLeft = when (paint.textAlign) {
                Paint.Align.CENTER -> x - measuredWidth * 0.5f
                Paint.Align.RIGHT -> x - measuredWidth
                else -> x
            }
            val textRight = textLeft + measuredWidth
            val defaultTop = y - lineHeight
            val defaultBottom = y + dp(4f)
            val targetX = draw.textWidth / TEXTDRAW_VIRTUAL_WIDTH * width
            val targetY = draw.textHeight / TEXTDRAW_VIRTUAL_HEIGHT * height
            val targetDefinesRect = targetX.isFinite() && targetY.isFinite() &&
                abs(targetX - x) >= dp(2f) && abs(targetY - y) >= dp(2f)
            val spriteRect = sprite?.let { bitmap ->
                if (targetDefinesRect) {
                    RectF(min(x, targetX), min(y, targetY), max(x, targetX), max(y, targetY))
                } else {
                    // No server target size: use the TXD's real aspect ratio
                    // mapped through the same 640×480 textdraw virtual canvas.
                    val fallbackWidth = (bitmap.width / TEXTDRAW_VIRTUAL_WIDTH * width)
                        .coerceIn(dp(8f), width.toFloat())
                    val fallbackHeight = (bitmap.height / TEXTDRAW_VIRTUAL_HEIGHT * height)
                        .coerceIn(dp(8f), height.toFloat())
                    RectF(x, y, x + fallbackWidth, y + fallbackHeight)
                }
            }
            val modelRect = modelMesh?.let {
                if (targetDefinesRect) {
                    RectF(min(x, targetX), min(y, targetY), max(x, targetX), max(y, targetY))
                } else {
                    val side = min(width, height).toFloat().coerceAtLeast(dp(48f)) * 0.18f
                    RectF(x, y, x + side, y + side)
                }
            }
            val box = when {
                modelRect != null -> modelRect
                spriteRect != null -> spriteRect
                draw.useBox && targetDefinesRect ->
                    RectF(min(x, targetX), min(y, targetY), max(x, targetX), max(y, targetY))
                else -> RectF(textLeft - dp(3f), defaultTop - dp(2f), textRight + dp(3f), defaultBottom)
            }
            if (draw.useBox) {
                paint.style = Paint.Style.FILL
                paint.color = colorFromAbgr(draw.boxColourAbgr)
                canvas.drawRect(box, paint)
            }

            val baseColor = colorFromAbgr(draw.letterColourAbgr)
            if (modelMesh != null && modelRect != null) {
                drawTextDrawModelPreview(canvas, modelMesh, draw, modelRect)
            } else if (sprite != null && spriteRect != null) {
                // This is actual decoded RGBA from the user-selected TXD. The
                // textdraw colour acts as a bounded Canvas modulation; no
                // Android drawable/resource is substituted for the GTA sprite.
                paint.isFilterBitmap = true
                paint.alpha = Color.alpha(baseColor)
                paint.colorFilter = PorterDuffColorFilter(
                    Color.rgb(Color.red(baseColor), Color.green(baseColor), Color.blue(baseColor)),
                    PorterDuff.Mode.MULTIPLY
                )
                canvas.drawBitmap(sprite, null, spriteRect, paint)
                paint.colorFilter = null
                paint.alpha = 255
                paint.isFilterBitmap = false
            } else {
                // Shadow/outline values are reduced as a small bounded offset.
                // This remains Canvas text—not a claim of GTA font fidelity.
                if (draw.outline > 0 || draw.shadow > 0) {
                    paint.color = Color.argb(
                        Color.alpha(baseColor).coerceAtLeast(96), 0, 0, 0
                    )
                    canvas.drawText(displayText, x + dp(1.2f), y + dp(1.2f), paint)
                }
                paint.color = baseColor
                canvas.drawText(displayText, x, y, paint)
            }

            if (frame.textDrawSelectionActive && draw.selectable) {
                val hitBox = RectF(box).apply { inset(-dp(4f), -dp(4f)) }
                selectableTextDrawRects += draw.wireId to hitBox
                paint.style = Paint.Style.STROKE
                paint.strokeWidth = dp(1.1f)
                // RPC 83 carries this affordance colour as RGBA. Preserve it
                // rather than inventing a permanent client-only highlight.
                paint.color = colorFromExactRgba(frame.textDrawSelectionColourRgba)
                canvas.drawRoundRect(hitBox, dp(3f), dp(3f), paint)
            }
        }
        paint.colorFilter = null
        paint.alpha = 255
        paint.isFilterBitmap = false
        paint.typeface = android.graphics.Typeface.DEFAULT
    }

    override fun onTouchEvent(event: android.view.MotionEvent): Boolean {
        when (event.actionMasked) {
            android.view.MotionEvent.ACTION_DOWN -> {
                // Death/respawn takes precedence over a selectable textdraw in
                // the same screen area. A pending native request consumes the
                // tap too, preventing a camera drag beneath a disabled button.
                if (isServerDead() && respawnButtonRect.contains(event.x, event.y)) {
                    respawnPointerDown = true
                    invalidate()
                    return true
                }
                if (!frame.textDrawSelectionActive) return false
                val hit = selectableTextDrawRects.asReversed().firstOrNull { (_, rect) ->
                    rect.contains(event.x, event.y)
                } ?: return false
                selectedPointerTextDrawId = hit.first
                return true
            }
            android.view.MotionEvent.ACTION_UP -> {
                if (respawnPointerDown) {
                    val shouldRequest = isServerDead() && !frame.spawnRequestPending &&
                        respawnButtonRect.contains(event.x, event.y)
                    respawnPointerDown = false
                    invalidate()
                    if (shouldRequest) onRespawnRequested?.invoke()
                    performClick()
                    return true
                }
                val wireId = selectedPointerTextDrawId
                selectedPointerTextDrawId = null
                if (wireId != null) {
                    val stillHit = selectableTextDrawRects.any { (id, rect) ->
                        id == wireId && rect.contains(event.x, event.y)
                    }
                    if (stillHit) onTextDrawSelected?.invoke(wireId)
                }
                if (wireId != null) performClick()
                return wireId != null
            }
            android.view.MotionEvent.ACTION_CANCEL -> {
                val consumed = respawnPointerDown || selectedPointerTextDrawId != null
                respawnPointerDown = false
                selectedPointerTextDrawId = null
                if (consumed) invalidate()
                return consumed
            }
        }
        return respawnPointerDown || selectedPointerTextDrawId != null
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    private fun drawFooter(canvas: Canvas) {
        val text = when {
            !frame.active -> "Buka sesi native untuk menerima state dunia"
            isServerDead() && frame.spawnRequestPending -> "RESPAWN · menunggu jawaban authoritative server"
            isServerDead() -> "RESPAWN · tap tombol, lalu tunggu server"
            frame.error.isNotBlank() -> "SESI: ${frame.error.take(40)}"
            frame.textDrawSelectionActive -> "PILIH TEXTDRAW · tap kotak selectable server"
            frame.objectEdit.active -> "EDIT OBJEK SERVER #${frame.objectEdit.id} · buka OBJ untuk transform bounded"
            frame.objectSelectionActive -> "PILIH OBJEK SERVER · buka OBJ untuk daftar bounded"
            !frame.localKnown -> "Menunggu posisi authoritative dari server"
            frame.localControllableKnown && !frame.localControllable -> "SERVER FREEZE · input gameplay dihentikan secara authoritative"
            frame.camera.serverControlled -> "KAMERA SERVER · drag scene tidak mengubah view sampai server mengembalikan kamera"
            frame.localPredictionActive && frame.localGrounded ->
                "TOUCH ON · gerak collision native + Packet-207 rate-gated"
            frame.localPredictionActive -> "TOUCH ON · gerak collision native di udara"
            controlsEnabled -> "TOUCH ON · menunggu collision GTA yang cocok di posisi spawn"
            else -> "TOUCH OFF · drag layar bebas untuk kamera scene"
        }
        paint.style = Paint.Style.FILL
        paint.textAlign = Paint.Align.CENTER
        paint.textSize = dp(9f)
        paint.color = Color.argb(225, 208, 235, 245)
        canvas.drawText(text, width / 2f, height - dp(10f), paint)
    }

    private fun isServerDead(value: WorldFrame = frame): Boolean =
        value.active && !value.spectate.active && value.localHealthKnown &&
            value.localHealth.isFinite() && value.localHealth <= 0f

    private fun sanitize(input: String): String = buildString(input.length) {
        input.forEach { character -> append(if (character.isISOControl()) ' ' else character) }
    }.trim()

    private fun colorFromRgba(rgba: Long): Int {
        if (rgba == 0L) return Color.rgb(222, 231, 235)
        val r = ((rgba shr 24) and 0xFF).toInt()
        val g = ((rgba shr 16) and 0xFF).toInt()
        val b = ((rgba shr 8) and 0xFF).toInt()
        val a = (rgba and 0xFF).toInt()
        return Color.argb(max(96, a), r, g, b)
    }

    /** RPC 83 selection colour is numeric RGBA, unlike textdraw's ABGR fields. */
    private fun colorFromExactRgba(rgba: Long): Int {
        val value = rgba and 0xFFFF_FFFFL
        val r = ((value shr 24) and 0xFF).toInt()
        val g = ((value shr 16) and 0xFF).toInt()
        val b = ((value shr 8) and 0xFF).toInt()
        val a = (value and 0xFF).toInt()
        return Color.argb(a, r, g, b)
    }

    /** Network textdraw colours are emitted as numeric ABGR. */
    private fun colorFromAbgr(abgr: Long): Int {
        val value = abgr and 0xFFFF_FFFFL
        val a = ((value shr 24) and 0xFF).toInt()
        val b = ((value shr 16) and 0xFF).toInt()
        val g = ((value shr 8) and 0xFF).toInt()
        val r = (value and 0xFF).toInt()
        return Color.argb(a, r, g, b)
    }

    private fun dp(value: Float): Float = value * resources.displayMetrics.density

    private companion object {
        const val MAX_VISIBLE_CHAT = 5
        const val MAX_CHAT_CHARS = 80
        // Compiled once: this view can redraw at display cadence while a timed
        // GameText banner is active. `h` is GTA's common highlight marker;
        // Canvas deliberately treats all these markers as presentation-only.
        val GAME_TEXT_COLOR_TOKEN = Regex("~[rRgGbByYpPwWlLsShH]~")
        val GAME_TEXT_NEWLINE_TOKEN = Regex("~[nN]~")
        val WORLD_LABEL_WHITESPACE = Regex("[\r\n\t]+")
        const val MAX_GAMETEXT_CHARS = 64
        const val MAX_GAMETEXT_LINES = 3
        const val MAX_TEXTDRAW_CHARS = 128
        const val TEXTDRAW_STYLE_PRICEDOWN = 3
        const val TEXTDRAW_STYLE_SPRITE = 4
        const val TEXTDRAW_STYLE_MODEL = 5
        const val MAX_TEXTDRAW_SPRITE_CACHE = 16
        const val MAX_TEXTDRAW_SPRITE_PIXELS = 512 * 512
        const val MAX_TEXTDRAW_SPRITE_DIMENSION = 512
        const val SPRITE_RETRY_MS = 5_000L
        const val MAX_TEXTDRAW_MODEL_CACHE = 8
        const val MAX_TEXTDRAW_MODEL_VERTICES = 3_600
        const val DFF_PREVIEW_FLOATS_PER_VERTEX = 7
        const val TEXTDRAW_MODEL_RADIUS = 1_000f
        const val MODEL_RETRY_MS = 5_000L
        const val MAX_PROJECTED_WORLD_LABELS = 24
        const val MAX_WORLD_LABEL_CHARS = 48
        const val TEXTDRAW_VIRTUAL_WIDTH = 640f
        const val TEXTDRAW_VIRTUAL_HEIGHT = 480f
    }
}
