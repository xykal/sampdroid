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
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.xykal.sampdroid.NativeBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

private const val RADAR_GAMEPLAY_TICK_DELAY_MS = 40L
private const val RADAR_GAMEPLAY_MAX_STEP_SECONDS = 0.05f
private val WORLD_LABEL_WHITESPACE = Regex("[\r\n\t]+")

/**
 * Android-native tactical world surface. It draws only copied state that has
 * passed native reducers. Its bounded ticker merely asks C++ to advance the
 * collision bridge; Kotlin never invents a position, collision hit, or packet.
 */
object NativeWorldDialog {

    fun show(activity: AppCompatActivity) {
        val density = activity.resources.displayMetrics.density
        // RPC-41 playback is owned by this visible surface and released on
        // every radar/scene transition; it never touches native session state.
        val audioStreamController = NativeAudioStreamController(activity)
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            isFocusable = true
            isFocusableInTouchMode = true
            setPadding((12f * density).toInt(), (8f * density).toInt(), (12f * density).toInt(), 0)
            setBackgroundColor(Color.rgb(12, 23, 32))
        }
        val intro = TextView(activity).apply {
            text = "DUNIA NATIVE · RADAR\n" +
                "Fallback ringan: touch, keyboard Bluetooth, dan gamepad aktif. " +
                "Gerak/sync hanya jalan setelah spawn server dan collision GTA siap."
            setTextColor(Color.rgb(215, 237, 247))
            textSize = 12f
        }
        val surfaceHeight = max(
            (280f * density).toInt(),
            (activity.resources.displayMetrics.heightPixels * 0.54f).toInt()
        )
        val stage = FrameLayout(activity).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                surfaceHeight
            ).apply { topMargin = (8f * density).toInt() }
            setBackgroundColor(Color.rgb(8, 18, 25))
            isFocusable = true
            isFocusableInTouchMode = true
        }
        val surface = NativeWorldSurfaceView(activity)
        val touchControls = NativeGameplayControlsOverlay(activity)
        stage.addView(surface, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ))
        // The same compact semantic overlay is available on the Canvas radar
        // fallback, not just GLES, so a low-end ARMv7 device retains a native
        // touch path. Empty regions still return false and preserve radar drag.
        stage.addView(touchControls, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ))
        // Switching to the GPU scene closes this lightweight radar first so an
        // ARMv7 device never keeps two renderer update loops alive at once.
        var activeDialog: androidx.appcompat.app.AlertDialog? = null
        // Nine utility actions do not fit as weighted micro-buttons on a
        // 320–360dp phone. Keep touch gameplay unobstructed and make this
        // secondary toolbar horizontally scrollable with readable targets.
        val controls = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
            setPadding(0, (6f * density).toInt(), 0, 0)
        }
        val controlsScroller = HorizontalScrollView(activity).apply {
            isHorizontalScrollBarEnabled = false
            isFillViewport = false
            contentDescription = "Toolbar dunia native. Geser mendatar untuk aksi tambahan."
            // HorizontalScrollView inherits FrameLayout; it does not expose a
            // Kotlin-visible LayoutParams nested type of its own.
            addView(controls, FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ))
        }
        fun addButton(label: String, onClick: () -> Unit) {
            controls.addView(Button(activity).apply {
                text = label
                textSize = 11f
                setOnClickListener { onClick() }
            }, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                marginStart = (2f * density).toInt()
                marginEnd = (2f * density).toInt()
            })
        }
        addButton("− ZOOM") { surface.zoomBy(1.45f) }
        addButton("+ ZOOM") { surface.zoomBy(1f / 1.45f) }
        addButton("FOLLOW") { surface.followLocalPlayer() }
        addButton("SCENE GPU") {
            activeDialog?.dismiss()
            activity.window.decorView.post { NativeWorldSceneDialog.show(activity) }
        }
        addButton("CHAT") { NativeChatDialog.show(activity) }
        addButton("SCORES") { NativeScoreboardDialog.show(activity) }
        addButton("KONTROL") { NativeControlsDialog.show(activity) }

        val status = TextView(activity).apply {
            setTextColor(Color.rgb(185, 246, 202))
            textSize = 11f
            setPadding(0, (5f * density).toInt(), 0, (5f * density).toInt())
            text = "Memuat snapshot dunia native…"
        }
        root.addView(intro)
        root.addView(stage)
        root.addView(controlsScroller, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ))
        root.addView(status)

        // Radar is also the low-GLES fallback gameplay surface, so hardware
        // input must not depend on the GPU scene or an on-screen touch pad.
        // The router produces exactly the same bounded semantic samples.
        val inputSamples = Channel<ControlSample>(Channel.CONFLATED)
        val inputJob: Job = activity.lifecycleScope.launch {
            for (sample in inputSamples) {
                runCatching {
                    withContext(Dispatchers.IO) {
                        NativeBridge.nativeSessionUpdateControls(
                            sample.moveX,
                            sample.moveY,
                            sample.lookDeltaDegrees,
                            sample.lookPitchDeltaDegrees,
                            sample.heldActions
                        )
                    }
                }
            }
        }
        var hardwareDrivingMode = false
        val requestNearbyUse: (Boolean) -> Unit = { passengerIntent ->
            activity.lifecycleScope.launch(Dispatchers.IO) {
                runCatching { NativeBridge.nativeSessionUseNearby(passengerIntent) }
            }
        }
        val inputMixer = NativeControlSampleMixer { inputSamples.trySend(it) }
        val gyroLook = NativeGyroLookController(
            activity,
            displayRotation = {
                @Suppress("DEPRECATION")
                activity.windowManager.defaultDisplay.rotation
            },
            canEmit = { activity.hasWindowFocus() },
            onLookDelta = inputMixer::submitGyroLook
        )
        val hardwareControls = NativeHardwareControlRouter(
            onControlSample = inputMixer::submitHardware,
            onUseRequested = requestNearbyUse,
            isDriving = { hardwareDrivingMode }
        )
        hardwareControls.onChatRequested = { initialText ->
            // Keyboard/controller shortcut is UI-only. Flush held movement
            // before the modal receives focus, then preserve a slash seed for
            // a physical command-key workflow.
            hardwareControls.clear()
            NativeChatDialog.show(activity, initialText)
        }
        hardwareControls.onScoreboardRequested = {
            hardwareControls.clear()
            NativeScoreboardDialog.show(activity)
        }
        touchControls.onControlSample = inputMixer::submitTouch
        touchControls.onUseRequested = requestNearbyUse
        val onHardwareMotion: (MotionEvent) -> Boolean = { event ->
            hardwareControls.onGenericMotionEvent(event)
        }
        root.setOnGenericMotionListener { _, event -> onHardwareMotion(event) }
        stage.setOnGenericMotionListener { _, event -> onHardwareMotion(event) }
        surface.setOnGenericMotionListener { _, event -> onHardwareMotion(event) }
        touchControls.setOnGenericMotionListener { _, event -> onHardwareMotion(event) }
        // Editor/settings are appended after the native callbacks exist so they
        // can flush a held physical key before a second modal steals focus.
        addButton("LAYOUT") {
            hardwareControls.clear()
            NativeTouchLayoutEditorDialog.show(activity) {
                touchControls.reloadLayout()
                gyroLook.setUserEnabled(touchControls.gyroLookEnabled())
            }
        }
        addButton("OBJ") {
            hardwareControls.clear()
            touchControls.clearControls()
            activity.lifecycleScope.launch {
                val frame = runCatching {
                    val raw = withContext(Dispatchers.IO) { NativeBridge.nativeSessionWorldSnapshot() }
                    WorldFrame.from(JSONObject(raw))
                }.getOrDefault(WorldFrame())
                when {
                    frame.attachedObjectEdit.active -> NativeAttachedObjectEditDialog.show(activity)
                    frame.objectEdit.active -> NativeObjectEditDialog.show(activity)
                    else -> NativeObjectSelectionDialog.show(activity)
                }
            }
        }

        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Dunia native")
            .setView(root)
            .setNegativeButton("Tutup", null)
            .create()
        dialog.setOnKeyListener { _, _, event -> hardwareControls.onKeyEvent(event) }
        activeDialog = dialog
        var renderJob: Job? = null
        var gameplayJob: Job? = null
        // Both collectors run on the lifecycle main dispatcher. The renderer
        // owns this gate from immutable native snapshots so gameplay ticking
        // stops immediately after a server-side close rather than repeatedly
        // asking native to advance a dead socket.
        var sessionGameplayActive = false
        // Separate a live socket from the stricter PlayerInit + PlayerSpawn ACK
        // gameplay gate. The world may render authoritative setup state while
        // collision ticking and input stay off.
        var gameplayTickEnabled = false
        dialog.setOnShowListener {
            // The radar fallback is a real low-end gameplay surface, not a
            // diagnostic card. Let controls and the world use the full dialog
            // height on small/ARMv7 phones instead of throwing away the lower
            // screen behind a developer-style margin.
            dialog.window?.setLayout(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            dialog.window?.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            stage.requestFocus()
            // Layout opt-in is local; registration waits for the first
            // authoritative non-frozen/non-spectator frame below.
            gyroLook.setUserEnabled(touchControls.gyroLookEnabled())
            // Fail closed until the first immutable native frame confirms that
            // an accepted socket is still active. This avoids a touch/key held
            // during dialog creation leaking into an already-ended session.
            touchControls.setGameplayEnabled(false, "MENUNGGU SESI NATIVE · INPUT OFF")
            hardwareControls.setEnabled(false)
            gyroLook.setGameplayEnabled(false)
            // Keep base gameplay available even on devices without GLES 2.0:
            // this radar has the same collision/native tick as the GPU scene.
            gameplayJob = activity.lifecycleScope.launch {
                var previousTickNanos = System.nanoTime()
                var backgroundControlsCleared = false
                while (isActive && dialog.isShowing) {
                    // lifecycleScope survives STOP, so do not let a hidden
                    // fallback surface keep advancing/sending movement or leave
                    // a held touch/controller action latched in background.
                    val foreground = activity.lifecycle.currentState.isAtLeast(
                        androidx.lifecycle.Lifecycle.State.RESUMED
                    )
                    if (!foreground) {
                        if (!backgroundControlsCleared) {
                            touchControls.setGameplayEnabled(false, "APP BACKGROUND · INPUT OFF")
                            touchControls.clearControls()
                            hardwareControls.setEnabled(false)
                            hardwareControls.clear()
                            gyroLook.setGameplayEnabled(false)
                            inputMixer.clear()
                            // NativeAudioStreamController observes onPause itself;
                            // do not call release here or it could not resume
                            // with this still-visible world dialog.
                            withContext(Dispatchers.IO) {
                                runCatching {
                                    NativeBridge.nativeSessionUpdateControls(0f, 0f, 0f, 0f, 0)
                                }
                            }
                            backgroundControlsCleared = true
                        }
                        previousTickNanos = System.nanoTime()
                        delay(100)
                        continue
                    }
                    backgroundControlsCleared = false
                    if (!gameplayTickEnabled) {
                        previousTickNanos = System.nanoTime()
                        delay(100)
                        continue
                    }
                    val nowNanos = System.nanoTime()
                    val deltaSeconds = ((nowNanos - previousTickNanos) / 1_000_000_000.0)
                        .toFloat()
                        .coerceIn(0.001f, RADAR_GAMEPLAY_MAX_STEP_SECONDS)
                    previousTickNanos = nowNanos
                    withContext(Dispatchers.IO) {
                        runCatching { NativeBridge.nativeSessionAdvanceGameplay(deltaSeconds) }
                    }
                    delay(RADAR_GAMEPLAY_TICK_DELAY_MS)
                }
            }
            renderJob = activity.lifecycleScope.launch {
                while (isActive && dialog.isShowing) {
                    if (!activity.lifecycle.currentState.isAtLeast(
                            androidx.lifecycle.Lifecycle.State.RESUMED
                        )) {
                        delay(100)
                        continue
                    }
                    val frame = runCatching {
                        val raw = withContext(Dispatchers.IO) {
                            NativeBridge.nativeSessionWorldSnapshot()
                        }
                        WorldFrame.from(JSONObject(raw))
                    }.getOrElse { WorldFrame(error = it.message ?: "snapshot world gagal") }
                    if (!activity.lifecycle.currentState.isAtLeast(
                            androidx.lifecycle.Lifecycle.State.RESUMED
                        )) {
                        delay(100)
                        continue
                    }
                    val wasSessionGameplayActive = sessionGameplayActive
                    sessionGameplayActive = frame.active
                    // Never keep a stale server world, dialog/menu, or audio
                    // directive visually alive after native reports its socket
                    // inactive. Preserve only the bounded reason string.
                    val presentationFrame = if (frame.active) frame else WorldFrame(
                        active = false,
                        error = frame.error
                    )
                    if (wasSessionGameplayActive && !sessionGameplayActive) inputMixer.clear()
                    hardwareDrivingMode = presentationFrame.active && !presentationFrame.spectate.active &&
                        presentationFrame.localVehicleKnown && presentationFrame.localInVehicle &&
                        presentationFrame.localVehicleSeat == 0
                    val inputBlockReason = presentationFrame.gameplayInputBlockReason()
                    val gameplayEnabled = inputBlockReason == null
                    val wasGameplayTickEnabled = gameplayTickEnabled
                    gameplayTickEnabled = gameplayEnabled
                    if (wasGameplayTickEnabled && !gameplayTickEnabled) inputMixer.clear()
                    hardwareControls.setEnabled(gameplayEnabled)
                    gyroLook.setGameplayEnabled(gameplayEnabled)
                    touchControls.setDrivingMode(hardwareDrivingMode)
                    touchControls.setGameplayEnabled(
                        gameplayEnabled,
                        inputBlockReason ?: ""
                    )
                    surface.submit(presentationFrame)
                    audioStreamController.submit(presentationFrame)
                    if (presentationFrame.active) {
                        NativeServerDialogController.submit(activity, presentationFrame)
                        NativeServerMenuController.submit(activity, presentationFrame)
                    } else {
                        NativeServerDialogController.clear()
                        NativeServerMenuController.clear()
                    }
                    status.text = presentationFrame.statusLine()
                    // ServerDetailDialog owns the network poll; this loop only
                    // paints copies of reduced state and never blocks UI input.
                    delay(100)
                }
            }
        }
        dialog.setOnDismissListener {
            dialog.window?.clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            gameplayJob?.cancel()
            renderJob?.cancel()
            audioStreamController.release()
            gyroLook.release()
            touchControls.clearControls()
            hardwareControls.clear()
            inputMixer.clear()
            inputSamples.close()
            inputJob.cancel()
            // Do not leave a held Bluetooth key latched if the dialog closes
            // between its ACTION_DOWN and ACTION_UP events.
            activity.lifecycleScope.launch(Dispatchers.IO) {
                runCatching { NativeBridge.nativeSessionUpdateControls(0f, 0f, 0f, 0f, 0) }
            }
        }
        dialog.show()
    }
}

/**
 * Ephemeral server-broadcast Packet-203 camera direction for one remote actor.
 * This is visualization only: it does not carry a projectile, target, raycast,
 * hit, damage, or any local gameplay authority.
 */
internal data class WorldRemoteAim(
    val active: Boolean = false,
    val cameraMode: Int = 0,
    val weaponState: Int = 0,
    val frontX: Float = 0f,
    val frontY: Float = 0f,
    val frontZ: Float = 0f,
    val positionX: Float = 0f,
    val positionY: Float = 0f,
    val positionZ: Float = 0f
)

/**
 * Short-lived direction from a server-relayed remote Packet-206 only. The
 * client excludes wire hit IDs/types here, so this cannot be treated as a local
 * target, hit, damage, ammo, or firing result.
 */
internal data class WorldRemoteShot(
    val active: Boolean = false,
    val weaponId: Int = 0,
    val originX: Float = 0f,
    val originY: Float = 0f,
    val originZ: Float = 0f,
    val directionX: Float = 0f,
    val directionY: Float = 0f,
    val directionZ: Float = 0f
)

/**
 * Bounded server ApplyPlayerAnimation/ApplyActorAnimation hand-off. This is a
 * presentation directive only until a user-owned GTA IFP/skeleton renderer can
 * execute it; it never freezes Android input or changes local collision.
 */
internal data class WorldForcedAnimation(
    val active: Boolean = false,
    val revision: Long = 0L,
    val library: String = "",
    val name: String = "",
    val delta: Float = 0f,
    val loop: Boolean = false,
    val lockX: Boolean = false,
    val lockY: Boolean = false,
    val freeze: Boolean = false,
    val durationMs: Long = 0L
)

internal data class WorldActor(
    val id: Int,
    val rgba: Long,
    val model: Long,
    val onFootKnown: Boolean,
    val remoteAim: WorldRemoteAim = WorldRemoteAim(),
    val remoteShot: WorldRemoteShot = WorldRemoteShot(),
    // Brief RPC-166 observer cue only; it cannot establish local death/respawn.
    val wasted: Boolean = false,
    val forcedAnimation: WorldForcedAnimation = WorldForcedAnimation(),
    // Packet-200 driver / source-verified Packet-211 passenger attachment.
    val inVehicle: Boolean,
    val vehicleDriver: Boolean,
    val vehicleId: Int,
    val vehicleSeat: Int,
    val nameTagVisible: Boolean,
    val chatBubble: WorldChatBubble,
    val x: Float,
    val y: Float,
    val z: Float,
    val angle: Float,
    val health: Float,
    val armour: Float,
    val name: String
)

/**
 * Visual-only NPC established by ShowActorForPlayer. It deliberately has a
 * separate server-actor ID namespace from [WorldActor] remote players, and
 * never becomes a touch target, collision obstacle, or local damage claim.
 */
internal data class WorldServerActor(
    val id: Int,
    val model: Long,
    val x: Float,
    val y: Float,
    val z: Float,
    val angle: Float,
    val health: Float,
    val invulnerable: Boolean,
    val forcedAnimation: WorldForcedAnimation = WorldForcedAnimation()
)

/** One server-provided RPC-155 roster row; no world transform is inferred. */
internal data class WorldScoreEntry(
    val id: Int,
    val rgba: Long,
    val npc: Boolean,
    val score: Int,
    val ping: Long,
    val name: String
)

internal data class WorldVehicle(
    val id: Int,
    val dynamicSyncKnown: Boolean,
    // Player ID zero is valid, so this must not be inferred from driverId.
    val driverKnown: Boolean,
    val driverId: Int,
    val model: Long,
    val colour1: Int,
    val colour2: Int,
    val paintjob: Int,
    val x: Float,
    val y: Float,
    val z: Float,
    val angle: Float,
    // Packet-200 quaternion-derived DFF attitude. False keeps the compatible
    // StreamIn/RPC-160 yaw-only fallback rather than inventing a tilt.
    val attitudeKnown: Boolean = false,
    val rotationX: Float = 0f,
    val rotationY: Float = 0f,
    val rotationZ: Float = 0f,
    // Exact full SetVehicleParams facts retained native-side. `-1` is an
    // explicit server unset state, never an Android-owned toggle.
    val parametersKnown: Boolean = false,
    val engine: Int = -1,
    val lights: Int = -1,
    val doors: Int = -1,
    val objective: Int = -1,
    val siren: Int = -1,
    val health: Float
)

/** One bounded server object material slot. It is either Default texture data
 * or a source Text=2 record; they never coexist for the same retained slot. */
internal data class WorldObjectMaterial(
    val slot: Int,
    val sourceModel: Int = 0,
    // Numeric ABGR comes from the Default source writer and stays raw until GLES color modulation.
    val colourAbgr: Long = 0L,
    val dictionary: String = "",
    val texture: String = "",
    val textActive: Boolean = false,
    val textMaterialSize: Int = 0,
    val textFontSize: Int = 0,
    val textBold: Boolean = false,
    // Text font/background colours are source ARGB, unlike Default ABGR.
    val textColourArgb: Long = 0L,
    val textBackgroundColourArgb: Long = 0L,
    val textAlignment: Int = 0,
    val textFont: String = "",
    val text: String = ""
)

/** Server object transform sampled from Create/Set/Move/DestroyObject RPCs. */
internal data class WorldObject(
    val id: Int,
    val model: Long,
    val moving: Boolean,
    val x: Float,
    val y: Float,
    val z: Float,
    val rotationX: Float,
    val rotationY: Float,
    val rotationZ: Float,
    val drawDistance: Float,
    val materials: List<WorldObjectMaterial> = emptyList()
)

/** Copied visual SetPlayerAttachedObject sample; bone anchoring is approximate. */
internal data class WorldPlayerAttachedObject(
    val playerId: Int,
    val slot: Int,
    val model: Long,
    val bone: Int,
    val x: Float,
    val y: Float,
    val z: Float,
    val rotationX: Float,
    val rotationY: Float,
    val rotationZ: Float,
    val scaleX: Float,
    val scaleY: Float,
    val scaleZ: Float,
    val colour1Abgr: Long,
    val colour2Abgr: Long
)

/** Mirrors the native TXD lookup-key boundary; these values are never paths. */
private fun isPortableObjectMaterialName(value: String): Boolean =
    value.length in 1..31 && value.all { character ->
        character.code in 0x20..0x7E && character != '/' && character != '\\' && character != ':'
    }

// Text arrives from a HybridString<32> source record. Preserve ordinary UTF-8
// plus explicit line breaks, but refuse NUL/other controls before it crosses
// the compact renderer contract as hexadecimal bytes.
private fun isPortableObjectMaterialText(value: String): Boolean {
    val bytes = value.toByteArray(Charsets.UTF_8)
    return bytes.size <= 31 && value.all { character ->
        character.code >= 0x20 || character == '\n' || character == '\r' || character == '\t'
    }
}

private fun isObjectMaterialTextSize(value: Int): Boolean = value in 10..140 && value % 10 == 0

/** Bounded global pickup copied from server Create/DestroyPickup RPCs. */
internal data class WorldPickup(
    val id: Int,
    val model: Int,
    val spawnType: Int,
    val x: Float,
    val y: Float,
    val z: Float
)

/** Per-recipient Show/Hide/FlashGangZone state for the native radar surface. */
internal data class WorldGangZone(
    val id: Int,
    val minX: Float,
    val minY: Float,
    val maxX: Float,
    val maxY: Float,
    // Numeric ABGR is preserved from the exact server RPC writer.
    val colourAbgr: Long,
    val flashing: Boolean,
    val flashColourAbgr: Long
)

/** Decoded PlayerShowTextLabel fact. Position is a static point or attachment offset. */
internal data class WorldTextLabel(
    val wireId: Int,
    val playerTextLabel: Boolean,
    // Text-label writers emit numeric RGBA, unlike gang-zone/textdraw ABGR.
    val rgba: Long,
    val x: Float,
    val y: Float,
    val z: Float,
    val drawDistance: Float,
    val los: Boolean,
    val playerAttachId: Int,
    val vehicleAttachId: Int,
    val text: String
)

/** A finite world-space label after attachment resolution against one copied frame. */
internal data class ResolvedWorldTextLabel(
    val source: WorldTextLabel,
    val x: Float,
    val y: Float,
    val z: Float,
    val distance: Float
)

/** Player name-tag point after PlayerInit/RPC-80 policy and distance filtering. */
internal data class ResolvedWorldNameTag(
    val actor: WorldActor,
    val distance: Float
)

/** A policy-free, server-expiry-filtered chat bubble anchor for the scene HUD. */
internal data class ResolvedWorldChatBubble(
    val wireId: Int,
    val x: Float,
    val y: Float,
    val z: Float,
    val bubble: WorldChatBubble,
    val distance: Float
)

/** Per-recipient SetPlayerMapIcon slot; type/style stay raw for a portable marker. */
internal data class WorldMapIcon(
    val id: Int,
    val x: Float,
    val y: Float,
    val z: Float,
    val type: Int,
    val rgba: Long,
    val style: Int
)

/** Sampled server camera/interpolation state; never a local movement fact. */
internal data class WorldCamera(
    val positionKnown: Boolean = false,
    val lookAtKnown: Boolean = false,
    val interpolatingPosition: Boolean = false,
    val interpolatingLookAt: Boolean = false,
    val positionX: Float = 0f,
    val positionY: Float = 0f,
    val positionZ: Float = 0f,
    val lookAtX: Float = 0f,
    val lookAtY: Float = 0f,
    val lookAtZ: Float = 0f
) {
    val serverControlled: Boolean get() = positionKnown || lookAtKnown
}

/**
 * Server-owned spectator lifecycle (RPC 124/126/127). `mode` is retained for
 * display only; Android follows only a copied streamed target and never turns
 * it into local movement, damage, collision, or outbound sync authority.
 */
internal data class WorldSpectate(
    val known: Boolean = false,
    val active: Boolean = false,
    val targetKnown: Boolean = false,
    val targetVehicle: Boolean = false,
    val targetId: Int = 0,
    val mode: Int = 0
)

/** PlayerInit name-tag policy; per-actor visibility comes from RPC 80. */
internal data class WorldNameTags(
    val show: Boolean = false,
    val disableLos: Boolean = false,
    val drawDistance: Float = 0f
)

/** Bounded SetPlayerChatBubble payload, already expiry-filtered in native. */
internal data class WorldChatBubble(
    val active: Boolean = false,
    val rgba: Long = 0L,
    val drawDistance: Float = 0f,
    val text: String = ""
)

/** Screen-space server textdraw, reduced before crossing JNI. */
internal data class WorldTextDraw(
    val wireId: Int,
    val playerTextDraw: Boolean,
    val useBox: Boolean,
    val alignment: Int,
    val letterWidth: Float,
    val letterHeight: Float,
    val letterColourAbgr: Long,
    val textWidth: Float,
    val textHeight: Float,
    val boxColourAbgr: Long,
    val shadow: Int,
    val outline: Int,
    val backgroundColourAbgr: Long,
    val style: Int,
    val selectable: Boolean,
    val x: Float,
    val y: Float,
    // Exact font-5 preview metadata. Canvas may draw only a bounded prelit
    // user-owned DFF fallback; it is not GTA's screen-space frontend renderer.
    val model: Long = 0L,
    val rotationX: Float = 0f,
    val rotationY: Float = 0f,
    val rotationZ: Float = 0f,
    val zoom: Float = 0f,
    val colour1: Int = 0,
    val colour2: Int = 0,
    val text: String
)

internal data class WorldMarker(
    val active: Boolean = false,
    val x: Float = 0f,
    val y: Float = 0f,
    val z: Float = 0f,
    val size: Float = 0f
)

/** A bounded copied line for the Android-native HUD; it never owns socket state. */
internal data class WorldChat(
    val rgba: Long,
    val text: String
)

/**
 * One short-lived global RPC-55 kill-feed row. It is server presentation only:
 * `reason` remains raw, and the row cannot imply damage, local death, ammo, or
 * a respawn outcome on Android.
 */
internal data class WorldDeathMessage(
    val hasKiller: Boolean,
    val killerId: Int,
    val victimId: Int,
    val reason: Int,
    val killerName: String,
    val victimName: String
)

/** Server-authored ShowDialog view already decoded and bounded by native code. */
internal data class WorldDialog(
    val active: Boolean = false,
    val revision: Long = 0L,
    val id: Int = -1,
    val style: Int = 0,
    val title: String = "",
    val button1: String = "",
    val button2: String = "",
    val body: String = ""
)

/**
 * Legacy server menu decoded from RPC 76/77/78. Its row values are display
 * copies; the Android controller returns only a revision-bound row number to
 * native RPC 132 or a revision-bound exit RPC 140.
 */
internal data class WorldMenu(
    val active: Boolean = false,
    val revision: Long = 0L,
    val id: Int = 0,
    val twoColumns: Boolean = false,
    val enabled: Boolean = true,
    val rowEnabled: List<Boolean> = List(12) { false },
    val title: String = "",
    val column1Header: String = "",
    val column2Header: String = "",
    val column1: List<String> = emptyList(),
    val column2: List<String> = emptyList()
)

/** Timed RPC-73 text, intentionally rendered as a portable Canvas banner proxy. */
internal data class WorldGameText(
    val active: Boolean = false,
    val revision: Long = 0L,
    val style: Int = 0,
    val durationMs: Long = 0L,
    val text: String = ""
)

/**
 * Current server-authorized ObjectEdit tuple. Transform values are intentionally
 * absent: Android derives a display baseline from the copied current object and
 * returns edits only through the native RPC-117 gate.
 */
internal data class WorldObjectEdit(
    val active: Boolean = false,
    val playerObject: Boolean = false,
    val id: Int = -1,
    val revision: Long = 0L,
    // Dedicated native-resolved baseline: unlike the ordinary object list, it
    // is not lost behind the renderer's bounded/capped snapshot ordering.
    val transformAvailable: Boolean = false,
    val x: Float = 0f,
    val y: Float = 0f,
    val z: Float = 0f,
    val rotationX: Float = 0f,
    val rotationY: Float = 0f,
    val rotationZ: Float = 0f
)

/** Raw local attachment facts exposed only while server RPC 116 opens editing. */
internal data class WorldAttachedObjectEdit(
    val active: Boolean = false,
    val slot: Int = -1,
    val revision: Long = 0L,
    // Lifecycle can be active before a local slot is resolvable. Keep that
    // separate so UI routes to the safe waiting editor, never the RPC-27 picker.
    val transformAvailable: Boolean = false,
    val model: Long = 0L,
    val bone: Int = -1,
    val offsetX: Float = 0f,
    val offsetY: Float = 0f,
    val offsetZ: Float = 0f,
    val rotationX: Float = 0f,
    val rotationY: Float = 0f,
    val rotationZ: Float = 0f,
    val scaleX: Float = 1f,
    val scaleY: Float = 1f,
    val scaleZ: Float = 1f
)

/**
 * Copied RPC-41/42 audio directive. [playable] is an Android safety decision,
 * not a server acknowledgement: only a bounded HTTPS URL may reach MediaPlayer.
 */
internal data class WorldAudioStream(
    val active: Boolean = false,
    val revision: Long = 0L,
    val playable: Boolean = false,
    val url: String = "",
    val positional: Boolean = false,
    val x: Float = 0f,
    val y: Float = 0f,
    val z: Float = 0f,
    val distance: Float = 0f
)

internal data class WorldFrame(
    val active: Boolean = false,
    // Native-owned lifecycle copies. These only gate presentation/input; Kotlin
    // never gets authority to select a class, approve spawn, or acknowledge a
    // reliable packet from them.
    val playerInit: Boolean = false,
    val classSelectionKnown: Boolean = false,
    val classRequestPending: Boolean = false,
    val spawnResponseKnown: Boolean = false,
    val spawnAllowed: Boolean = false,
    // Session-owned retry gate for the explicit respawn action. It is not a
    // claim that the server accepted or completed a spawn.
    val spawnRequestPending: Boolean = false,
    val spawnConfirmationSent: Boolean = false,
    val spawnConfirmationAcked: Boolean = false,
    val localKnown: Boolean = false,
    // Distinguishes valid model ID 0 (CJ) from a frame that never received
    // SetSpawnInfo and must not guess a local ped DFF.
    val localSpawnInfoKnown: Boolean = false,
    val localFacingKnown: Boolean = false,
    val localHealthKnown: Boolean = false,
    val localArmourKnown: Boolean = false,
    val localMoneyKnown: Boolean = false,
    // Exact RPC-133 HUD fact, not locally simulated police/gameplay state.
    val localWantedLevelKnown: Boolean = false,
    val localScoreKnown: Boolean = false,
    val localArmedWeaponKnown: Boolean = false,
    val localArmedAmmoKnown: Boolean = false,
    val localControllableKnown: Boolean = false,
    val localControllable: Boolean = true,
    // Server-supplied interior ID. The scene selects only matching parsed IPL
    // placements; prediction/collision still stays fail-closed until an exact
    // interior collision world exists.
    val localInteriorKnown: Boolean = false,
    // RPC 48 virtual world is an independent server routing value, not an
    // interior/collision partition. Android displays it but does not locally
    // filter world entities or invent a map transition from it.
    val localVirtualWorldKnown: Boolean = false,
    // Only server Put/RemovePlayerFromVehicle may set these facts. Seat zero
    // can enable the bounded native kinematic driver bridge; passenger remains
    // server-transform-only and never emits driver sync.
    val localVehicleKnown: Boolean = false,
    val localInVehicle: Boolean = false,
    // Native Packet-211 passenger presentation state only; it neither selects
    // a target nor confirms a shot/damage outcome.
    val localPassengerDriveByActive: Boolean = false,
    val localInterior: Int = 0,
    val localVirtualWorld: Int = 0,
    val localVehicleId: Int = 0,
    val localVehicleSeat: Int = 0,
    // True only after native collision-grounded movement accepted a local
    // prediction; authoritative server position updates reset it.
    val localPredictionActive: Boolean = false,
    val localGrounded: Boolean = false,
    // HUD-only semantic aim/fire presentation. aimSyncReady is a bounded native
    // candidate state; FIRE can only afford the no-target Packet-206 path and
    // is never a target/hit/damage assertion.
    val localAimHeld: Boolean = false,
    val localFireHeld: Boolean = false,
    val localAimSyncReady: Boolean = false,
    // PlayerInit-advertised server gate for the conservative no-target Packet-206
    // path, not an outcome/receipt/hit indicator.
    val bulletSyncServerEnabled: Boolean = false,
    // Brief native socket-send feedback for a close static-COL contact only;
    // never receipt, entity target, hit/damage, or local ammo authority.
    val staticWorldContactActive: Boolean = false,
    // Native socket-send count only; it does not mean receipt, hit, damage,
    // target resolution, or local ammunition authority.
    val outboundNoTargetBulletPackets: Long = 0L,
    val localAimPitchDegrees: Float = 0f,
    // Render/camera anchor copied from an authoritative streamed vehicle while
    // seated; this is deliberately not claimed as an exact GTA seat pose.
    val localVehicleAnchorKnown: Boolean = false,
    val localId: Int = 0,
    val localModel: Long = 0L,
    val localX: Float = 0f,
    val localY: Float = 0f,
    val localZ: Float = 0f,
    val localAngle: Float = 0f,
    val localHealth: Float = 100f,
    val localArmour: Float = 0f,
    val localMoney: Long = 0L,
    val localWantedLevel: Int = 0,
    val localScore: Int = 0,
    val localPing: Long = 0L,
    val localArmedWeapon: Int = 0,
    val localArmedAmmo: Long = 0L,
    val localChatBubble: WorldChatBubble = WorldChatBubble(),
    val localForcedAnimation: WorldForcedAnimation = WorldForcedAnimation(),
    val audioStream: WorldAudioStream = WorldAudioStream(),
    val worldTimeKnown: Boolean = false,
    // RPC 29/30 are recipient-specific presentation facts. They never change
    // native movement/collision timing and Kotlin has no write path for them.
    val playerTimeKnown: Boolean = false,
    val playerClockKnown: Boolean = false,
    val playerClockEnabled: Boolean = true,
    val weatherKnown: Boolean = false,
    // PlayerInit/RPC-146 gravity in GTA units. It is HUD/state only here;
    // C++ controls whether the bounded collision pilot can use its range.
    val gravityKnown: Boolean = false,
    // Recipient-specific exterior rectangle copied from RPC 17. Kotlin only
    // displays it; C++ owns validation and the prediction gate.
    val worldBoundsKnown: Boolean = false,
    val worldTime: Int = 12,
    val playerHour: Int = 12,
    val playerMinute: Int = 0,
    val weather: Int = 0,
    val gravity: Float = 0.008f,
    val worldMaxX: Float = 20000f,
    val worldMinX: Float = -20000f,
    val worldMaxY: Float = 20000f,
    val worldMinY: Float = -20000f,
    val nameTags: WorldNameTags = WorldNameTags(),
    val camera: WorldCamera = WorldCamera(),
    val spectate: WorldSpectate = WorldSpectate(),
    val dialog: WorldDialog = WorldDialog(),
    val menu: WorldMenu = WorldMenu(),
    val gameText: WorldGameText = WorldGameText(),
    val checkpoint: WorldMarker = WorldMarker(),
    val raceCheckpoint: WorldMarker = WorldMarker(),
    val streamedPlayers: Int = 0,
    val remoteOnFootPlayers: Int = 0,
    val streamedVehicles: Int = 0,
    val trackedServerActors: Int = 0,
    val serverObjects: Int = 0,
    val trackedPickups: Int = 0,
    val trackedTextDraws: Int = 0,
    val trackedTextLabels: Int = 0,
    val trackedMapIcons: Int = 0,
    val trackedGangZones: Int = 0,
    val textDrawSelectionActive: Boolean = false,
    // RPC 27 is separate from textdraw selection; it enables only the native
    // bounded object picker, never a Java-authored model or hit coordinate.
    val objectSelectionActive: Boolean = false,
    // Monotonic native token copied by the selector to fail closed if a server
    // Begin/Cancel/replacement happens between an Android render and its tap.
    val objectSelectionRevision: Long = 0L,
    // Distinct RPC-117 object editor lifecycle. Object ID/kind/revision remain
    // native-owned; Kotlin has no authority to nominate any of those values.
    val objectEdit: WorldObjectEdit = WorldObjectEdit(),
    // Distinct RPC-116 local attachment editor. The raw offsets are supplied
    // by native only while active; Kotlin cannot nominate model/bone/colours.
    val attachedObjectEdit: WorldAttachedObjectEdit = WorldAttachedObjectEdit(),
    // RPC 83 selection colour is encoded as unsigned RGBA on the wire.
    val textDrawSelectionColourRgba: Long = 0L,
    val serverName: String = "",
    val actors: List<WorldActor> = emptyList(),
    val serverActors: List<WorldServerActor> = emptyList(),
    val scoreboard: List<WorldScoreEntry> = emptyList(),
    val vehicles: List<WorldVehicle> = emptyList(),
    val objects: List<WorldObject> = emptyList(),
    // Bounded renderer-only accessories; exact GTA ped bone placement is not
    // available without a native ped animation/skeleton implementation.
    val playerAttachedObjects: List<WorldPlayerAttachedObject> = emptyList(),
    val pickups: List<WorldPickup> = emptyList(),
    val gangZones: List<WorldGangZone> = emptyList(),
    val textLabels: List<WorldTextLabel> = emptyList(),
    val mapIcons: List<WorldMapIcon> = emptyList(),
    val textDraws: List<WorldTextDraw> = emptyList(),
    val chat: List<WorldChat> = emptyList(),
    // Native filters RPC-55 expiration before this copied UI-only tail.
    val deathMessages: List<WorldDeathMessage> = emptyList(),
    val error: String = ""
) {
    companion object {
        fun from(o: JSONObject): WorldFrame {
            val local = o.optJSONObject("local") ?: JSONObject()
            val environment = o.optJSONObject("environment") ?: JSONObject()
            val audioStream = parseWorldAudioStream(o.optJSONObject("audioStream"))
            // Treat JSON as an untrusted presentation boundary even though the
            // native reducer already validates RPC 17. A malformed copied
            // rectangle must not become a misleading HUD state.
            val environmentWorldMaxX = environment.optDouble("worldMaxX", 20000.0).toFloat()
            val environmentWorldMinX = environment.optDouble("worldMinX", -20000.0).toFloat()
            val environmentWorldMaxY = environment.optDouble("worldMaxY", 20000.0).toFloat()
            val environmentWorldMinY = environment.optDouble("worldMinY", -20000.0).toFloat()
            val environmentBoundsValid = environment.optBoolean("worldBoundsKnown") &&
                environmentWorldMaxX.isFinite() && environmentWorldMinX.isFinite() &&
                environmentWorldMaxY.isFinite() && environmentWorldMinY.isFinite() &&
                environmentWorldMaxX > environmentWorldMinX &&
                environmentWorldMaxY > environmentWorldMinY &&
                environmentWorldMaxX <= 20000f && environmentWorldMinX >= -20000f &&
                environmentWorldMaxY <= 20000f && environmentWorldMinY >= -20000f
            val nameTagsJson = o.optJSONObject("nameTags") ?: JSONObject()
            val cameraJson = o.optJSONObject("camera") ?: JSONObject()
            val spectateJson = o.optJSONObject("spectate") ?: JSONObject()
            val dialogJson = o.optJSONObject("dialog") ?: JSONObject()
            val menuJson = o.optJSONObject("menu") ?: JSONObject()
            val gameTextJson = o.optJSONObject("gameText") ?: JSONObject()
            val objectEditJson = o.optJSONObject("objectEdit") ?: JSONObject()
            val attachedObjectEditJson = o.optJSONObject("attachedObjectEdit") ?: JSONObject()
            fun menuStrings(key: String): List<String> {
                val source = menuJson.optJSONArray(key) ?: return emptyList()
                return buildList {
                    for (index in 0 until minOf(source.length(), 12)) {
                        add(source.optString(index).take(31))
                    }
                }
            }
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
            val animationLibraryPattern = Regex("[A-Za-z0-9_-]{1,15}")
            val animationNamePattern = Regex("[A-Za-z0-9_-]{1,23}")
            fun forcedAnimation(value: JSONObject?): WorldForcedAnimation {
                if (value == null || !value.optBoolean("active")) return WorldForcedAnimation()
                val library = value.optString("library")
                val name = value.optString("name")
                val delta = value.optDouble("delta").toFloat()
                val revision = value.optLong("revision", 0L)
                val durationMs = value.optLong("durationMs", -1L)
                // Native accepts the source-compatible bounded token surface;
                // independently re-check it here because JSON is an untrusted
                // renderer boundary, not permission to execute an animation.
                if (!animationLibraryPattern.matches(library) || !animationNamePattern.matches(name) ||
                    !delta.isFinite() || kotlin.math.abs(delta) > 100f ||
                    revision <= 0L || durationMs !in 0L..4_294_967_295L
                ) {
                    return WorldForcedAnimation()
                }
                return WorldForcedAnimation(
                    active = true,
                    revision = revision,
                    library = library,
                    name = name,
                    delta = delta,
                    loop = value.optBoolean("loop"),
                    lockX = value.optBoolean("lockX"),
                    lockY = value.optBoolean("lockY"),
                    freeze = value.optBoolean("freeze"),
                    durationMs = durationMs
                )
            }
            val actors = buildList {
                val source = o.optJSONArray("actors")
                if (source != null) {
                    for (i in 0 until source.length()) {
                        val actor = source.optJSONObject(i)
                        if (actor != null) {
                            val aimFrontX = actor.optDouble("aimCameraFrontX").toFloat()
                            val aimFrontY = actor.optDouble("aimCameraFrontY").toFloat()
                            val aimFrontZ = actor.optDouble("aimCameraFrontZ").toFloat()
                            val aimPositionX = actor.optDouble("aimCameraPositionX").toFloat()
                            val aimPositionY = actor.optDouble("aimCameraPositionY").toFloat()
                            val aimPositionZ = actor.optDouble("aimCameraPositionZ").toFloat()
                            val aimLengthSquared = aimFrontX * aimFrontX + aimFrontY * aimFrontY +
                                aimFrontZ * aimFrontZ
                            val aimCameraMode = actor.optInt("aimCameraMode")
                            val compatibleAimMode = aimCameraMode in 3..64 &&
                                aimCameraMode !in 9..13 && aimCameraMode !in 19..21 &&
                                aimCameraMode !in 23..28 && aimCameraMode !in 30..45 &&
                                aimCameraMode !in 48..50 && aimCameraMode != 5 &&
                                aimCameraMode != 6 && aimCameraMode != 17 &&
                                aimCameraMode != 52 && aimCameraMode != 54 &&
                                aimCameraMode != 60 && aimCameraMode != 61
                            // Native already validates Packet-203. Keep an
                            // independent JSON/UI boundary so malformed data
                            // cannot yield a direction marker or GL NaN.
                            val aimActive = actor.optBoolean("aimKnown") && compatibleAimMode &&
                                actor.optInt("aimWeaponState") in 0..3 &&
                                aimFrontX.isFinite() && aimFrontY.isFinite() && aimFrontZ.isFinite() &&
                                aimPositionX.isFinite() && aimPositionY.isFinite() && aimPositionZ.isFinite() &&
                                aimPositionX > -20_000f && aimPositionX < 20_000f &&
                                aimPositionY > -20_000f && aimPositionY < 20_000f &&
                                aimPositionZ > -1_000f && aimPositionZ < 200_000f &&
                                aimLengthSquared.isFinite() && aimLengthSquared > 0f && aimLengthSquared < 1.5f
                            val bulletOriginX = actor.optDouble("bulletOriginX").toFloat()
                            val bulletOriginY = actor.optDouble("bulletOriginY").toFloat()
                            val bulletOriginZ = actor.optDouble("bulletOriginZ").toFloat()
                            val bulletDirectionX = actor.optDouble("bulletDirectionX").toFloat()
                            val bulletDirectionY = actor.optDouble("bulletDirectionY").toFloat()
                            val bulletDirectionZ = actor.optDouble("bulletDirectionZ").toFloat()
                            val bulletLengthSquared = bulletDirectionX * bulletDirectionX +
                                bulletDirectionY * bulletDirectionY + bulletDirectionZ * bulletDirectionZ
                            val bulletWeaponId = actor.optInt("bulletWeaponId")
                            // The receiver deliberately gets only a native
                            // short-lived direction cue, never the packet's
                            // target/hit fields. Revalidate it before GLES.
                            val bulletActive = actor.optBoolean("bulletKnown") &&
                                (bulletWeaponId in 22..34 || bulletWeaponId == 38) &&
                                bulletOriginX.isFinite() && bulletOriginY.isFinite() && bulletOriginZ.isFinite() &&
                                bulletOriginX > -20_000f && bulletOriginX < 20_000f &&
                                bulletOriginY > -20_000f && bulletOriginY < 20_000f &&
                                bulletOriginZ > -1_000f && bulletOriginZ < 200_000f &&
                                bulletLengthSquared.isFinite() && bulletLengthSquared > 0f &&
                                bulletLengthSquared <= 40_000f * 40_000f
                            add(
                                WorldActor(
                                    id = actor.optInt("id"),
                                    rgba = actor.optLong("rgba"),
                                    model = actor.optLong("model"),
                                    onFootKnown = actor.optBoolean("onFootKnown"),
                                    remoteAim = WorldRemoteAim(
                                        active = aimActive,
                                        cameraMode = if (aimActive) aimCameraMode else 0,
                                        weaponState = if (aimActive) actor.optInt("aimWeaponState") else 0,
                                        frontX = if (aimActive) aimFrontX else 0f,
                                        frontY = if (aimActive) aimFrontY else 0f,
                                        frontZ = if (aimActive) aimFrontZ else 0f,
                                        positionX = if (aimActive) aimPositionX else 0f,
                                        positionY = if (aimActive) aimPositionY else 0f,
                                        positionZ = if (aimActive) aimPositionZ else 0f
                                    ),
                                    remoteShot = WorldRemoteShot(
                                        active = bulletActive,
                                        weaponId = if (bulletActive) bulletWeaponId else 0,
                                        originX = if (bulletActive) bulletOriginX else 0f,
                                        originY = if (bulletActive) bulletOriginY else 0f,
                                        originZ = if (bulletActive) bulletOriginZ else 0f,
                                        directionX = if (bulletActive) bulletDirectionX else 0f,
                                        directionY = if (bulletActive) bulletDirectionY else 0f,
                                        directionZ = if (bulletActive) bulletDirectionZ else 0f
                                    ),
                                    wasted = actor.optBoolean("wasted"),
                                    forcedAnimation = forcedAnimation(actor.optJSONObject("animation")),
                                    inVehicle = actor.optBoolean("inVehicle"),
                                    vehicleDriver = actor.optBoolean("vehicleDriver"),
                                    vehicleId = actor.optInt("vehicleId").coerceIn(0, 65535),
                                    vehicleSeat = actor.optInt("vehicleSeat").coerceIn(0, 63),
                                    nameTagVisible = actor.optBoolean("nameTagVisible", true),
                                    chatBubble = WorldChatBubble(
                                        active = actor.optBoolean("chatBubbleActive"),
                                        rgba = actor.optLong("chatBubbleRgba"),
                                        drawDistance = actor.optDouble("chatBubbleDrawDistance").toFloat()
                                            .takeIf { it.isFinite() && it >= 0f && it <= 50_000f } ?: 0f,
                                        text = actor.optString("chatBubbleText").take(128)
                                    ),
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
            val serverActors = buildList {
                val source = o.optJSONArray("serverActors")
                if (source != null) {
                    for (i in 0 until source.length().coerceAtMost(64)) {
                        val actor = source.optJSONObject(i) ?: continue
                        val id = actor.optInt("id", -1)
                        val model = actor.optLong("model", -1L)
                        val x = actor.optDouble("x").toFloat()
                        val y = actor.optDouble("y").toFloat()
                        val z = actor.optDouble("z").toFloat()
                        val angle = actor.optDouble("angle").toFloat()
                        val health = actor.optDouble("health", 100.0).toFloat()
                        // Native already validates exact RPC layouts. Recheck
                        // every cross-JNI visual field so a malformed JSON
                        // handoff cannot create a GL/Canvas NaN or fake actor.
                        if (id in 0..65535 && model in 0L..65535L &&
                            x.isFinite() && y.isFinite() && z.isFinite() &&
                            x > -20_000f && x < 20_000f &&
                            y > -20_000f && y < 20_000f &&
                            z > -1_000f && z < 200_000f &&
                            angle.isFinite() && angle >= -360_000f && angle <= 360_000f &&
                            health.isFinite() && health >= 0f && health <= 65_535f
                        ) {
                            add(
                                WorldServerActor(
                                    id = id,
                                    model = model,
                                    x = x,
                                    y = y,
                                    z = z,
                                    angle = angle,
                                    health = health,
                                    invulnerable = actor.optBoolean("invulnerable"),
                                    forcedAnimation = forcedAnimation(actor.optJSONObject("animation"))
                                )
                            )
                        }
                    }
                }
            }
            val scoreboard = buildList {
                val source = o.optJSONArray("scoreboard")
                if (source != null) {
                    for (i in 0 until source.length().coerceAtMost(64)) {
                        val entry = source.optJSONObject(i) ?: continue
                        add(
                            WorldScoreEntry(
                                id = entry.optInt("id").coerceIn(0, 65535),
                                rgba = entry.optLong("rgba"),
                                npc = entry.optBoolean("npc"),
                                score = entry.optInt("score"),
                                ping = entry.optLong("ping").coerceIn(0L, 4_294_967_295L),
                                name = entry.optString("name").take(24)
                            )
                        )
                    }
                }
            }
            val vehicles = buildList {
                val source = o.optJSONArray("vehicles")
                if (source != null) {
                    for (i in 0 until source.length()) {
                        val vehicle = source.optJSONObject(i)
                        if (vehicle != null) {
                            val rotationX = vehicle.optDouble("rotationX").toFloat()
                            val rotationY = vehicle.optDouble("rotationY").toFloat()
                            val rotationZ = vehicle.optDouble("rotationZ").toFloat()
                            // This is visual data only. Recheck the compact
                            // native quaternion-to-Euler hand-off before a
                            // malformed JSON copy can tilt a GLES DFF wildly.
                            val attitudeKnown = vehicle.optBoolean("attitudeKnown") &&
                                rotationX.isFinite() && rotationY.isFinite() && rotationZ.isFinite() &&
                                abs(rotationX) <= 180.001f && abs(rotationY) <= 90.001f &&
                                abs(rotationZ) <= 180.001f
                            // Never let malformed JSON invent an enabled local
                            // engine/siren. Absent or out-of-domain values stay
                            // at the source-compatible -1 sentinel.
                            fun vehicleParam(name: String): Int =
                                vehicle.optInt(name, -1).takeIf { it in -1..1 } ?: -1
                            val engine = vehicleParam("engine")
                            val lights = vehicleParam("lights")
                            val doors = vehicleParam("doors")
                            val objective = vehicleParam("objective")
                            val siren = vehicleParam("siren")
                            val parametersKnown = vehicle.optBoolean("parametersKnown")
                            add(
                                WorldVehicle(
                                    id = vehicle.optInt("id"),
                                    dynamicSyncKnown = vehicle.optBoolean("dynamicSyncKnown"),
                                    driverKnown = vehicle.optBoolean("driverKnown"),
                                    driverId = vehicle.optInt("driverId"),
                                    model = vehicle.optLong("model"),
                                    colour1 = vehicle.optInt("colour1"),
                                    colour2 = vehicle.optInt("colour2"),
                                    paintjob = vehicle.optInt("paintjob"),
                                    x = vehicle.optDouble("x").toFloat(),
                                    y = vehicle.optDouble("y").toFloat(),
                                    z = vehicle.optDouble("z").toFloat(),
                                    angle = vehicle.optDouble("angle").toFloat(),
                                    attitudeKnown = attitudeKnown,
                                    rotationX = if (attitudeKnown) rotationX else 0f,
                                    rotationY = if (attitudeKnown) rotationY else 0f,
                                    rotationZ = if (attitudeKnown) rotationZ else 0f,
                                    parametersKnown = parametersKnown,
                                    engine = engine,
                                    lights = lights,
                                    doors = doors,
                                    objective = objective,
                                    siren = siren,
                                    health = vehicle.optDouble("health", 1000.0).toFloat()
                                )
                            )
                        }
                    }
                }
            }
            val objects = buildList {
                val source = o.optJSONArray("objects")
                if (source != null) {
                    for (i in 0 until source.length()) {
                        val objectValue = source.optJSONObject(i)
                        if (objectValue != null) {
                            val materials = buildList {
                                val sourceMaterials = objectValue.optJSONArray("materials")
                                val seenSlots = BooleanArray(16)
                                if (sourceMaterials != null) {
                                    for (materialIndex in 0 until sourceMaterials.length()) {
                                        val material = sourceMaterials.optJSONObject(materialIndex)
                                        if (material != null) {
                                            val slot = material.optInt("slot", -1)
                                            if (slot in 0..15 && !seenSlots[slot]) {
                                                when (material.optString("kind", "default")) {
                                                "default" -> {
                                                    val sourceModel = material.optInt("sourceModel", -1)
                                                    val dictionary = material.optString("dictionary")
                                                    val texture = material.optString("texture")
                                                    val colour = material.optLong("colourAbgr", -1L)
                                                    // API model -1 is retained as u16 65535 and
                                                    // is a color-only base-material update: no TXD
                                                    // names are required or consulted for it.
                                                    val colorOnly = sourceModel == 65535 &&
                                                        dictionary.isEmpty() && texture.isEmpty()
                                                    if (sourceModel in 0..65535 && colour in 0L..0xFFFF_FFFFL &&
                                                        (colorOnly ||
                                                            (isPortableObjectMaterialName(dictionary) &&
                                                                isPortableObjectMaterialName(texture)))
                                                    ) {
                                                        seenSlots[slot] = true
                                                        add(
                                                            WorldObjectMaterial(
                                                                slot = slot,
                                                                sourceModel = sourceModel,
                                                                colourAbgr = colour,
                                                                dictionary = dictionary,
                                                                texture = texture
                                                            )
                                                        )
                                                    }
                                                }
                                                "text" -> {
                                                    val size = material.optInt("materialSize", 0)
                                                    val fontSize = material.optInt("fontSize", -1)
                                                    val alignment = material.optInt("alignment", -1)
                                                    val fontColour = material.optLong("fontColourArgb", -1L)
                                                    val backgroundColour = material.optLong("backgroundColourArgb", -1L)
                                                    val font = material.optString("font")
                                                    val text = material.optString("text")
                                                    if (isObjectMaterialTextSize(size) && fontSize in 0..255 &&
                                                        alignment in 0..2 &&
                                                        fontColour in 0L..0xFFFF_FFFFL &&
                                                        backgroundColour in 0L..0xFFFF_FFFFL &&
                                                        isPortableObjectMaterialText(font) &&
                                                        isPortableObjectMaterialText(text)
                                                    ) {
                                                        seenSlots[slot] = true
                                                        add(
                                                            WorldObjectMaterial(
                                                                slot = slot,
                                                                textActive = true,
                                                                textMaterialSize = size,
                                                                textFontSize = fontSize,
                                                                textBold = material.optBoolean("bold"),
                                                                textColourArgb = fontColour,
                                                                textBackgroundColourArgb = backgroundColour,
                                                                textAlignment = alignment,
                                                                textFont = font,
                                                                text = text
                                                            )
                                                        )
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    }
                                }
                            }
                            add(
                                WorldObject(
                                    id = objectValue.optInt("id"),
                                    model = objectValue.optLong("model"),
                                    moving = objectValue.optBoolean("moving"),
                                    x = objectValue.optDouble("x").toFloat(),
                                    y = objectValue.optDouble("y").toFloat(),
                                    z = objectValue.optDouble("z").toFloat(),
                                    rotationX = objectValue.optDouble("rotationX").toFloat(),
                                    rotationY = objectValue.optDouble("rotationY").toFloat(),
                                    rotationZ = objectValue.optDouble("rotationZ").toFloat(),
                                    drawDistance = objectValue.optDouble("drawDistance").toFloat(),
                                    materials = materials
                                )
                            )
                        }
                    }
                }
            }
            val playerAttachedObjects = buildList {
                val source = o.optJSONArray("playerAttachedObjects")
                val seenSlots = HashSet<Pair<Int, Int>>()
                if (source != null) {
                    for (i in 0 until min(64, source.length())) {
                        val attachment = source.optJSONObject(i) ?: continue
                        val playerId = attachment.optInt("playerId", -1)
                        val slot = attachment.optInt("slot", -1)
                        val model = attachment.optLong("model", -1L)
                        val bone = attachment.optInt("bone", -1)
                        val x = attachment.optDouble("x").toFloat()
                        val y = attachment.optDouble("y").toFloat()
                        val z = attachment.optDouble("z").toFloat()
                        val rotationX = attachment.optDouble("rotationX").toFloat()
                        val rotationY = attachment.optDouble("rotationY").toFloat()
                        val rotationZ = attachment.optDouble("rotationZ").toFloat()
                        val scaleX = attachment.optDouble("scaleX").toFloat()
                        val scaleY = attachment.optDouble("scaleY").toFloat()
                        val scaleZ = attachment.optDouble("scaleZ").toFloat()
                        val colour1 = attachment.optLong("colour1Abgr", -1L)
                        val colour2 = attachment.optLong("colour2Abgr", -1L)
                        val key = playerId to slot
                        if (playerId !in 0..65535 || slot !in 0..9 || model !in 1L..65535L ||
                            bone !in 0..18 || key in seenSlots ||
                            !x.isFinite() || !y.isFinite() || !z.isFinite() ||
                            x !in -20_000f..20_000f || y !in -20_000f..20_000f ||
                            z !in -1_000f..200_000f ||
                            !rotationX.isFinite() || !rotationY.isFinite() || !rotationZ.isFinite() ||
                            !scaleX.isFinite() || !scaleY.isFinite() || !scaleZ.isFinite() ||
                            abs(scaleX) !in 0.001f..100f || abs(scaleY) !in 0.001f..100f ||
                            abs(scaleZ) !in 0.001f..100f ||
                            colour1 !in 0L..0xFFFF_FFFFL || colour2 !in 0L..0xFFFF_FFFFL
                        ) continue
                        seenSlots.add(key)
                        add(
                            WorldPlayerAttachedObject(
                                playerId, slot, model, bone, x, y, z,
                                rotationX, rotationY, rotationZ, scaleX, scaleY, scaleZ,
                                colour1, colour2
                            )
                        )
                    }
                }
            }
            val pickups = buildList {
                val source = o.optJSONArray("pickups")
                if (source != null) {
                    for (i in 0 until source.length()) {
                        val pickup = source.optJSONObject(i)
                        if (pickup != null) {
                            add(
                                WorldPickup(
                                    id = pickup.optInt("id"),
                                    model = pickup.optInt("model"),
                                    spawnType = pickup.optInt("spawnType"),
                                    x = pickup.optDouble("x").toFloat(),
                                    y = pickup.optDouble("y").toFloat(),
                                    z = pickup.optDouble("z").toFloat()
                                )
                            )
                        }
                    }
                }
            }
            val textLabels = buildList {
                val source = o.optJSONArray("textLabels")
                if (source != null) {
                    for (i in 0 until source.length()) {
                        val label = source.optJSONObject(i)
                        if (label != null) {
                            val x = label.optDouble("x").toFloat()
                            val y = label.optDouble("y").toFloat()
                            val z = label.optDouble("z").toFloat()
                            val distance = label.optDouble("drawDistance").toFloat()
                            val wireId = label.optInt("wireId")
                            if (wireId in 0..2047 && x.isFinite() && y.isFinite() && z.isFinite() &&
                                x > -20_000f && x < 20_000f && y > -20_000f && y < 20_000f &&
                                z > -1_000f && z < 200_000f &&
                                distance.isFinite() && distance >= 0f && distance <= 50_000f
                            ) {
                                add(
                                    WorldTextLabel(
                                        wireId = wireId,
                                        playerTextLabel = label.optBoolean("playerTextLabel"),
                                        rgba = label.optLong("rgba"),
                                        x = x,
                                        y = y,
                                        z = z,
                                        drawDistance = distance,
                                        los = label.optBoolean("los"),
                                        playerAttachId = label.optInt("playerAttachId", 65535).coerceIn(0, 65535),
                                        vehicleAttachId = label.optInt("vehicleAttachId", 65535).coerceIn(0, 65535),
                                        text = label.optString("text").take(64)
                                    )
                                )
                            }
                        }
                    }
                }
            }
            val mapIcons = buildList {
                val source = o.optJSONArray("mapIcons")
                if (source != null) {
                    for (i in 0 until source.length()) {
                        val icon = source.optJSONObject(i)
                        if (icon != null) {
                            val x = icon.optDouble("x").toFloat()
                            val y = icon.optDouble("y").toFloat()
                            val z = icon.optDouble("z").toFloat()
                            val id = icon.optInt("id", -1)
                            val type = icon.optInt("type", -1)
                            val style = icon.optInt("style", -1)
                            if (id in 0..99 && type in 0..255 && style in 0..255 &&
                                x.isFinite() && y.isFinite() && z.isFinite() &&
                                x > -20_000f && x < 20_000f && y > -20_000f && y < 20_000f &&
                                z > -1_000f && z < 200_000f
                            ) {
                                add(WorldMapIcon(id, x, y, z, type, icon.optLong("rgba"), style))
                            }
                        }
                    }
                }
            }
            val gangZones = buildList {
                val source = o.optJSONArray("gangZones")
                if (source != null) {
                    for (i in 0 until source.length()) {
                        val zone = source.optJSONObject(i)
                        if (zone != null) {
                            val minX = zone.optDouble("minX").toFloat()
                            val minY = zone.optDouble("minY").toFloat()
                            val maxX = zone.optDouble("maxX").toFloat()
                            val maxY = zone.optDouble("maxY").toFloat()
                            val id = zone.optInt("id")
                            // JNI already bounded this surface; retain a second
                            // Kotlin safety gate so malformed JSON can never
                            // make Canvas draw an unbounded/inverted rectangle.
                            if (id in 0..1023 && minX.isFinite() && minY.isFinite() &&
                                maxX.isFinite() && maxY.isFinite() && minX < maxX && minY < maxY
                            ) {
                                add(
                                    WorldGangZone(
                                        id = id,
                                        minX = minX,
                                        minY = minY,
                                        maxX = maxX,
                                        maxY = maxY,
                                        colourAbgr = zone.optLong("colourAbgr"),
                                        flashing = zone.optBoolean("flashing"),
                                        flashColourAbgr = zone.optLong("flashColourAbgr")
                                    )
                                )
                            }
                        }
                    }
                }
            }
            val textDraws = buildList {
                val source = o.optJSONArray("textDraws")
                if (source != null) {
                    for (i in 0 until source.length()) {
                        val draw = source.optJSONObject(i)
                        if (draw != null) {
                            add(
                                WorldTextDraw(
                                    wireId = draw.optInt("wireId").coerceIn(0, 2303),
                                    playerTextDraw = draw.optBoolean("playerTextDraw"),
                                    useBox = draw.optBoolean("useBox"),
                                    alignment = draw.optInt("alignment").coerceIn(0, 3),
                                    letterWidth = draw.optDouble("letterWidth").toFloat(),
                                    letterHeight = draw.optDouble("letterHeight").toFloat(),
                                    letterColourAbgr = draw.optLong("letterColourAbgr"),
                                    textWidth = draw.optDouble("textWidth").toFloat(),
                                    textHeight = draw.optDouble("textHeight").toFloat(),
                                    boxColourAbgr = draw.optLong("boxColourAbgr"),
                                    shadow = draw.optInt("shadow").coerceIn(0, 255),
                                    outline = draw.optInt("outline").coerceIn(0, 255),
                                    backgroundColourAbgr = draw.optLong("backgroundColourAbgr"),
                                    style = draw.optInt("style").coerceIn(0, 5),
                                    selectable = draw.optBoolean("selectable"),
                                    x = draw.optDouble("x").toFloat(),
                                    y = draw.optDouble("y").toFloat(),
                                    model = draw.optLong("model").coerceIn(0L, 65535L),
                                    rotationX = draw.optDouble("rotationX").toFloat()
                                        .takeIf { it.isFinite() && abs(it) <= 360_000f } ?: 0f,
                                    rotationY = draw.optDouble("rotationY").toFloat()
                                        .takeIf { it.isFinite() && abs(it) <= 360_000f } ?: 0f,
                                    rotationZ = draw.optDouble("rotationZ").toFloat()
                                        .takeIf { it.isFinite() && abs(it) <= 360_000f } ?: 0f,
                                    zoom = draw.optDouble("zoom").toFloat()
                                        .takeIf { it.isFinite() && abs(it) <= 10_000f } ?: 0f,
                                    colour1 = draw.optInt("colour1").coerceIn(-32768, 32767),
                                    colour2 = draw.optInt("colour2").coerceIn(-32768, 32767),
                                    text = draw.optString("text")
                                )
                            )
                        }
                    }
                }
            }
            val chat = buildList {
                val source = o.optJSONArray("chat")
                if (source != null) {
                    for (i in 0 until source.length()) {
                        val line = source.optJSONObject(i)
                        if (line != null) {
                            add(WorldChat(line.optLong("rgba"), line.optString("text")))
                        }
                    }
                }
            }
            val deathMessages = buildList {
                val source = o.optJSONArray("deathMessages")
                if (source != null) {
                    for (i in 0 until minOf(source.length(), 8)) {
                        val message = source.optJSONObject(i) ?: continue
                        val hasKiller = message.optBoolean("hasKiller")
                        val killerId = message.optInt("killerId", -1)
                        val victimId = message.optInt("victimId", -1)
                        val reason = message.optInt("reason", -1)
                        if (victimId !in 0..65534 || reason !in 0..255 ||
                            (hasKiller && killerId !in 0..65534)
                        ) continue
                        // IDs/names are native-owned. Strip controls and cap a
                        // copied renderer string; do not look up or invent an
                        // identity from actors on this untrusted JSON boundary.
                        fun feedName(raw: String, fallback: String): String = raw
                            .filterNot { it.isISOControl() }
                            .take(24)
                            .ifBlank { fallback }
                        add(
                            WorldDeathMessage(
                                hasKiller = hasKiller,
                                killerId = if (hasKiller) killerId else 65535,
                                victimId = victimId,
                                reason = reason,
                                killerName = if (hasKiller) {
                                    feedName(message.optString("killerName"), "#$killerId")
                                } else "",
                                victimName = feedName(message.optString("victimName"), "#$victimId")
                            )
                        )
                    }
                }
            }
            val objectSelectionRevision = o.optLong("objectSelectionRevision")
            val objectSelectionActive = o.optBoolean("objectSelectionActive") &&
                objectSelectionRevision > 0L
            val objectEditId = objectEditJson.optInt("id", -1)
            val objectEditRevision = objectEditJson.optLong("revision")
            val objectEditActive = objectEditJson.optBoolean("active") &&
                objectEditId in 1..65534 && objectEditRevision > 0L
            val objectEditX = objectEditJson.optDouble("x").toFloat()
            val objectEditY = objectEditJson.optDouble("y").toFloat()
            val objectEditZ = objectEditJson.optDouble("z").toFloat()
            val objectEditRotationX = objectEditJson.optDouble("rotationX").toFloat()
            val objectEditRotationY = objectEditJson.optDouble("rotationY").toFloat()
            val objectEditRotationZ = objectEditJson.optDouble("rotationZ").toFloat()
            val objectEditTransformAvailable = objectEditActive &&
                objectEditJson.optBoolean("transformAvailable") &&
                objectEditX.isFinite() && objectEditY.isFinite() && objectEditZ.isFinite() &&
                objectEditX > -20_000f && objectEditX < 20_000f &&
                objectEditY > -20_000f && objectEditY < 20_000f &&
                objectEditZ > -1_000f && objectEditZ < 200_000f &&
                objectEditRotationX.isFinite() && objectEditRotationY.isFinite() &&
                objectEditRotationZ.isFinite() && abs(objectEditRotationX) <= 360_000f &&
                abs(objectEditRotationY) <= 360_000f && abs(objectEditRotationZ) <= 360_000f
            val attachedEditSlot = attachedObjectEditJson.optInt("slot", -1)
            val attachedEditRevision = attachedObjectEditJson.optLong("revision")
            val attachedEditModel = attachedObjectEditJson.optLong("model", -1L)
            val attachedEditBone = attachedObjectEditJson.optInt("bone", -1)
            val attachedOffsetX = attachedObjectEditJson.optDouble("offsetX").toFloat()
            val attachedOffsetY = attachedObjectEditJson.optDouble("offsetY").toFloat()
            val attachedOffsetZ = attachedObjectEditJson.optDouble("offsetZ").toFloat()
            val attachedRotationX = attachedObjectEditJson.optDouble("rotationX").toFloat()
            val attachedRotationY = attachedObjectEditJson.optDouble("rotationY").toFloat()
            val attachedRotationZ = attachedObjectEditJson.optDouble("rotationZ").toFloat()
            val attachedScaleX = attachedObjectEditJson.optDouble("scaleX", 1.0).toFloat()
            val attachedScaleY = attachedObjectEditJson.optDouble("scaleY", 1.0).toFloat()
            val attachedScaleZ = attachedObjectEditJson.optDouble("scaleZ", 1.0).toFloat()
            val attachedEditModeActive = attachedObjectEditJson.optBoolean("active") &&
                attachedEditSlot in 0..9 && attachedEditRevision > 0L
            val attachedEditTransformAvailable = attachedEditModeActive &&
                attachedEditModel in 1L..65535L && attachedEditBone in 0..18 &&
                attachedOffsetX.isFinite() && attachedOffsetY.isFinite() && attachedOffsetZ.isFinite() &&
                abs(attachedOffsetX) <= 10_000f && abs(attachedOffsetY) <= 10_000f &&
                abs(attachedOffsetZ) <= 10_000f &&
                attachedRotationX.isFinite() && attachedRotationY.isFinite() && attachedRotationZ.isFinite() &&
                abs(attachedRotationX) <= 360_000f && abs(attachedRotationY) <= 360_000f &&
                abs(attachedRotationZ) <= 360_000f &&
                attachedScaleX.isFinite() && attachedScaleY.isFinite() && attachedScaleZ.isFinite() &&
                abs(attachedScaleX) in 0.001f..100f && abs(attachedScaleY) in 0.001f..100f &&
                abs(attachedScaleZ) in 0.001f..100f
            return WorldFrame(
                active = o.optBoolean("active"),
                playerInit = o.optBoolean("playerInit"),
                classSelectionKnown = o.optBoolean("classSelectionKnown"),
                classRequestPending = o.optBoolean("classRequestPending"),
                spawnResponseKnown = o.optBoolean("spawnResponseKnown"),
                spawnAllowed = o.optBoolean("spawnAllowed"),
                spawnRequestPending = o.optBoolean("spawnRequestPending"),
                spawnConfirmationSent = o.optBoolean("spawnConfirmationSent"),
                spawnConfirmationAcked = o.optBoolean("spawnConfirmationAcked"),
                localKnown = local.optBoolean("known"),
                localSpawnInfoKnown = local.optBoolean("spawnInfoKnown"),
                localFacingKnown = local.optBoolean("facingKnown"),
                localHealthKnown = local.optBoolean("healthKnown"),
                localArmourKnown = local.optBoolean("armourKnown"),
                localMoneyKnown = local.optBoolean("moneyKnown"),
                localWantedLevelKnown = local.optBoolean("wantedLevelKnown"),
                localScoreKnown = local.optBoolean("scoreKnown"),
                localArmedWeaponKnown = local.optBoolean("armedWeaponKnown"),
                localArmedAmmoKnown = local.optBoolean("armedAmmoKnown"),
                localControllableKnown = local.optBoolean("controllableKnown"),
                localControllable = local.optBoolean("controllable", true),
                localInteriorKnown = local.optBoolean("interiorKnown"),
                localVirtualWorldKnown = local.optBoolean("virtualWorldKnown"),
                localVehicleKnown = local.optBoolean("vehicleKnown"),
                localInVehicle = local.optBoolean("inVehicle"),
                localPassengerDriveByActive = local.optBoolean("passengerDriveByActive"),
                localInterior = local.optInt("interior").coerceIn(0, 255),
                localVirtualWorld = local.optInt("virtualWorld"),
                localVehicleId = local.optInt("vehicleId").coerceIn(0, 65535),
                localVehicleSeat = local.optInt("vehicleSeat").coerceIn(0, 8),
                localPredictionActive = local.optBoolean("predictionActive"),
                localGrounded = local.optBoolean("grounded"),
                localAimHeld = local.optBoolean("aimHeld"),
                localFireHeld = local.optBoolean("fireHeld"),
                localAimSyncReady = local.optBoolean("aimSyncReady"),
                bulletSyncServerEnabled = local.optBoolean("bulletSyncServerEnabled"),
                staticWorldContactActive = local.optBoolean("staticWorldContactActive"),
                outboundNoTargetBulletPackets = local.optLong("outboundNoTargetBulletPackets")
                    .coerceAtLeast(0L),
                localAimPitchDegrees = local.optDouble("aimPitchDegrees").toFloat()
                    .takeIf { it.isFinite() && it in -75f..75f } ?: 0f,
                localVehicleAnchorKnown = local.optBoolean("vehicleAnchorKnown"),
                localId = local.optInt("id"),
                localModel = local.optLong("model"),
                localX = local.optDouble("x").toFloat(),
                localY = local.optDouble("y").toFloat(),
                localZ = local.optDouble("z").toFloat(),
                localAngle = local.optDouble("angle").toFloat(),
                localHealth = local.optDouble("health", 100.0).toFloat(),
                localArmour = local.optDouble("armour").toFloat(),
                localMoney = local.optLong("money"),
                localWantedLevel = local.optInt("wantedLevel").coerceIn(0, 6),
                localScore = local.optInt("score"),
                localPing = local.optLong("ping").coerceIn(0L, 4_294_967_295L),
                localArmedWeapon = local.optInt("armedWeapon").coerceIn(0, 46),
                localArmedAmmo = local.optLong("armedAmmo").coerceAtLeast(0L),
                localChatBubble = WorldChatBubble(
                    active = local.optBoolean("chatBubbleActive"),
                    rgba = local.optLong("chatBubbleRgba"),
                    drawDistance = local.optDouble("chatBubbleDrawDistance").toFloat()
                        .takeIf { it.isFinite() && it >= 0f && it <= 50_000f } ?: 0f,
                    text = local.optString("chatBubbleText").take(128)
                ),
                localForcedAnimation = forcedAnimation(local.optJSONObject("animation")),
                audioStream = audioStream,
                worldTimeKnown = environment.optBoolean("timeKnown"),
                playerTimeKnown = environment.optBoolean("playerTimeKnown"),
                playerClockKnown = environment.optBoolean("playerClockKnown"),
                playerClockEnabled = environment.optBoolean("playerClockEnabled", true),
                weatherKnown = environment.optBoolean("weatherKnown"),
                gravityKnown = environment.optBoolean("gravityKnown"),
                worldBoundsKnown = environmentBoundsValid,
                worldTime = environment.optInt("worldTime", 12).coerceIn(0, 23),
                playerHour = environment.optInt("playerHour", 12).coerceIn(0, 23),
                playerMinute = environment.optInt("playerMinute", 0).coerceIn(0, 59),
                weather = environment.optInt("weather"),
                gravity = environment.optDouble("gravity", 0.008).toFloat()
                    .takeIf { it.isFinite() && it >= 0.0001f && it <= 0.1f } ?: 0.008f,
                worldMaxX = environmentWorldMaxX,
                worldMinX = environmentWorldMinX,
                worldMaxY = environmentWorldMaxY,
                worldMinY = environmentWorldMinY,
                nameTags = WorldNameTags(
                    show = nameTagsJson.optBoolean("show"),
                    disableLos = nameTagsJson.optBoolean("disableLos"),
                    drawDistance = nameTagsJson.optDouble("drawDistance").toFloat()
                        .takeIf { it.isFinite() && it >= 0f && it <= 50_000f } ?: 0f
                ),
                camera = WorldCamera(
                    positionKnown = cameraJson.optBoolean("positionKnown"),
                    lookAtKnown = cameraJson.optBoolean("lookAtKnown"),
                    interpolatingPosition = cameraJson.optBoolean("interpolatingPosition"),
                    interpolatingLookAt = cameraJson.optBoolean("interpolatingLookAt"),
                    positionX = cameraJson.optDouble("positionX").toFloat(),
                    positionY = cameraJson.optDouble("positionY").toFloat(),
                    positionZ = cameraJson.optDouble("positionZ").toFloat(),
                    lookAtX = cameraJson.optDouble("lookAtX").toFloat(),
                    lookAtY = cameraJson.optDouble("lookAtY").toFloat(),
                    lookAtZ = cameraJson.optDouble("lookAtZ").toFloat()
                ),
                spectate = WorldSpectate(
                    known = spectateJson.optBoolean("known"),
                    active = spectateJson.optBoolean("active"),
                    targetKnown = spectateJson.optBoolean("targetKnown"),
                    targetVehicle = spectateJson.optBoolean("targetVehicle"),
                    targetId = spectateJson.optInt("targetId").coerceIn(0, 65535),
                    mode = spectateJson.optInt("mode").coerceIn(0, 255)
                ),
                dialog = WorldDialog(
                    active = dialogJson.optBoolean("active"),
                    revision = dialogJson.optLong("revision"),
                    id = dialogJson.optInt("id", -1),
                    style = dialogJson.optInt("style"),
                    title = dialogJson.optString("title"),
                    button1 = dialogJson.optString("button1"),
                    button2 = dialogJson.optString("button2"),
                    body = dialogJson.optString("body")
                ),
                menu = WorldMenu(
                    active = menuJson.optBoolean("active"),
                    revision = menuJson.optLong("revision"),
                    id = menuJson.optInt("id").coerceIn(0, 127),
                    twoColumns = menuJson.optBoolean("twoColumns"),
                    enabled = menuJson.optBoolean("enabled", true),
                    rowEnabled = List(12) { index ->
                        (menuJson.optJSONArray("rowEnabled")?.optBoolean(index) ?: false)
                    },
                    title = menuJson.optString("title").take(31),
                    column1Header = menuJson.optString("column1Header").take(31),
                    column2Header = menuJson.optString("column2Header").take(31),
                    column1 = menuStrings("column1"),
                    column2 = menuStrings("column2")
                ),
                gameText = WorldGameText(
                    active = gameTextJson.optBoolean("active"),
                    revision = gameTextJson.optLong("revision"),
                    style = gameTextJson.optInt("style").coerceIn(0, 255),
                    durationMs = gameTextJson.optLong("durationMs").coerceIn(0L, 3_600_000L),
                    text = gameTextJson.optString("text").take(64)
                ),
                checkpoint = marker("checkpoint"),
                raceCheckpoint = marker("raceCheckpoint"),
                streamedPlayers = o.optInt("streamedPlayers"),
                remoteOnFootPlayers = o.optInt("remoteOnFootPlayers"),
                streamedVehicles = o.optInt("streamedVehicles"),
                trackedServerActors = o.optInt("trackedServerActors").coerceIn(0, 1000),
                serverObjects = o.optInt("serverObjects"),
                trackedPickups = o.optInt("trackedPickups"),
                trackedTextDraws = o.optInt("trackedTextDraws"),
                trackedTextLabels = o.optInt("trackedTextLabels"),
                trackedMapIcons = o.optInt("trackedMapIcons"),
                trackedGangZones = o.optInt("trackedGangZones"),
                textDrawSelectionActive = o.optBoolean("textDrawSelectionActive"),
                objectSelectionActive = objectSelectionActive,
                objectSelectionRevision = if (objectSelectionActive) objectSelectionRevision else 0L,
                objectEdit = WorldObjectEdit(
                    active = objectEditActive,
                    playerObject = objectEditActive && objectEditJson.optBoolean("playerObject"),
                    id = if (objectEditActive) objectEditId else -1,
                    revision = if (objectEditActive) objectEditRevision else 0L,
                    transformAvailable = objectEditTransformAvailable,
                    x = if (objectEditTransformAvailable) objectEditX else 0f,
                    y = if (objectEditTransformAvailable) objectEditY else 0f,
                    z = if (objectEditTransformAvailable) objectEditZ else 0f,
                    rotationX = if (objectEditTransformAvailable) objectEditRotationX else 0f,
                    rotationY = if (objectEditTransformAvailable) objectEditRotationY else 0f,
                    rotationZ = if (objectEditTransformAvailable) objectEditRotationZ else 0f
                ),
                attachedObjectEdit = WorldAttachedObjectEdit(
                    active = attachedEditModeActive,
                    slot = if (attachedEditModeActive) attachedEditSlot else -1,
                    revision = if (attachedEditModeActive) attachedEditRevision else 0L,
                    transformAvailable = attachedEditTransformAvailable,
                    model = if (attachedEditTransformAvailable) attachedEditModel else 0L,
                    bone = if (attachedEditTransformAvailable) attachedEditBone else -1,
                    offsetX = if (attachedEditTransformAvailable) attachedOffsetX else 0f,
                    offsetY = if (attachedEditTransformAvailable) attachedOffsetY else 0f,
                    offsetZ = if (attachedEditTransformAvailable) attachedOffsetZ else 0f,
                    rotationX = if (attachedEditTransformAvailable) attachedRotationX else 0f,
                    rotationY = if (attachedEditTransformAvailable) attachedRotationY else 0f,
                    rotationZ = if (attachedEditTransformAvailable) attachedRotationZ else 0f,
                    scaleX = if (attachedEditTransformAvailable) attachedScaleX else 1f,
                    scaleY = if (attachedEditTransformAvailable) attachedScaleY else 1f,
                    scaleZ = if (attachedEditTransformAvailable) attachedScaleZ else 1f
                ),
                textDrawSelectionColourRgba = o.optLong("textDrawSelectionColourRgba"),
                serverName = o.optString("serverName"),
                actors = actors,
                serverActors = serverActors,
                scoreboard = scoreboard,
                vehicles = vehicles,
                objects = objects,
                playerAttachedObjects = playerAttachedObjects,
                pickups = pickups,
                gangZones = gangZones,
                textLabels = textLabels,
                mapIcons = mapIcons,
                textDraws = textDraws,
                chat = chat,
                deathMessages = deathMessages,
                error = o.optString("error")
            )
        }
    }

    /**
     * Human-readable facts from the currently authoritative local driver
     * vehicle only. The app has no engine/light/siren controls here: these are
     * retained RPC-24 values, and an absent/-1 value is intentionally silent.
     */
    fun localDriverVehicleParameterSummary(compact: Boolean = false): String {
        if (!localVehicleKnown || !localInVehicle || localVehicleSeat != 0) return ""
        val vehicle = vehicles.firstOrNull { it.id == localVehicleId } ?: return ""
        if (!vehicle.parametersKnown) return ""
        val server = if (compact) "" else " SERVER"
        return buildList {
            when (vehicle.engine) {
                0 -> add("MESIN${server} MATI")
                1 -> add("MESIN${server} NYALA")
            }
            when (vehicle.lights) {
                0 -> add("LAMPU${server} MATI")
                1 -> add("LAMPU${server} NYALA")
            }
            when (vehicle.siren) {
                0 -> add("SIREN${server} MATI")
                1 -> add("SIREN${server} NYALA")
            }
            if (!compact) {
                when (vehicle.doors) {
                    0 -> add("PINTU${server} TERBUKA")
                    1 -> add("PINTU${server} TERKUNCI")
                }
                if (vehicle.objective == 1) add("OBJEKTIF${server}")
            }
        }.joinToString(" · ")
    }

    fun statusLine(): String = when {
        // A server PlayerClose/transport refusal makes the socket inactive, but
        // its native error is the actionable explanation. Do not mask it with
        // the generic inactive hint, otherwise a world surface looks like it
        // simply never opened after a server explicitly ended the session.
        error.isNotBlank() -> "Sesi: $error"
        !active -> "Sesi belum aktif · buka Sesi native dari detail server."
        !playerInit -> "Transport aktif · menunggu PlayerInit dari server sebelum dunia/kontrol gameplay dibuka."
        !spawnConfirmationAcked && !spectate.active -> when {
            spawnConfirmationSent -> "PlayerSpawn terkirim · menunggu ACK transport server sebelum kontrol gameplay aktif."
            spawnRequestPending -> "Meminta izin spawn ke server · kontrol gameplay masih terkunci."
            spawnResponseKnown && !spawnAllowed -> "Server menolak spawn saat ini · pilih class lain atau ikuti instruksi server."
            classRequestPending -> "Meminta class ke server · menunggu respons sebelum Spawn."
            classSelectionKnown -> "Class diterima · tekan SPAWN untuk meminta izin server."
            else -> "Sesi terinisialisasi · menunggu class/spawn server sebelum kontrol gameplay aktif."
        }
        spectate.active -> when {
            spectate.targetKnown && sceneAnchor().followingSpectateTarget && spectate.targetVehicle ->
                "SPECTATOR · mengikuti kendaraan #${spectate.targetId} dari stream server."
            spectate.targetKnown && sceneAnchor().followingSpectateTarget ->
                "SPECTATOR · mengikuti player #${spectate.targetId} dari stream server."
            spectate.targetKnown -> "SPECTATOR · target #${spectate.targetId} belum streamed; anchor aman belum diubah."
            else -> "SPECTATOR · menunggu target stream server."
        }
        attachedObjectEdit.active && !attachedObjectEdit.transformAvailable ->
            "EDIT ATTACHMENT SERVER slot ${attachedObjectEdit.slot} menunggu resolve native."
        attachedObjectEdit.active -> "EDIT ATTACHMENT SERVER slot ${attachedObjectEdit.slot} aktif · buka OBJ untuk transform local bounded."
        objectEdit.active && !objectEdit.transformAvailable ->
            "EDIT OBJEK SERVER #${objectEdit.id} menunggu resolve attachment/object native."
        objectEdit.active -> "EDIT OBJEK SERVER #${objectEdit.id} aktif · buka OBJ untuk transform native bounded."
        objectSelectionActive -> "PILIH OBJEK SERVER aktif · buka scene GPU lalu OBJ untuk daftar bounded."
        !localKnown -> "Sesi aktif · menunggu posisi/spawn lokal dari server."
        localControllableKnown && !localControllable -> "SERVER FREEZE · input gameplay dihentikan oleh state server."
        localVehicleKnown && localInVehicle && localVehicleSeat != 0 && localVehicleAnchorKnown ->
            "Passenger #$localVehicleId kursi $localVehicleSeat · kamera mengikuti transform vehicle server."
        localVehicleKnown && localInVehicle && localVehicleSeat != 0 ->
            "Passenger #$localVehicleId kursi $localVehicleSeat · menunggu transform server."
        localVehicleKnown && localInVehicle && localPredictionActive ->
            "Driver #$localVehicleId · kinematic collision + Packet-200 native aktif." +
                localDriverVehicleParameterSummary().let { if (it.isBlank()) "" else " · $it" }
        localVehicleKnown && localInVehicle ->
            "Driver #$localVehicleId · menunggu StreamInVehicle/COL native." +
                localDriverVehicleParameterSummary().let { if (it.isBlank()) "" else " · $it" }
        else -> {
            val dynamicVehicles = vehicles.count { it.dynamicSyncKnown }
            "${actors.size}/${streamedPlayers} player · ${serverActors.size}/$trackedServerActors NPC · " +
                "${vehicles.size}/${streamedVehicles} kendaraan ($dynamicVehicles sync) · " +
                "${objects.size}/$serverObjects objek · " +
                "${pickups.size}/$trackedPickups pickup · ${gangZones.size}/$trackedGangZones zona · " +
                "${textLabels.size}/$trackedTextLabels label · ${mapIcons.size}/$trackedMapIcons ikon · " +
                "${textDraws.size}/$trackedTextDraws textdraw · " +
                "drag peta untuk geser kamera" + (when {
                    audioStream.active && audioStream.playable -> " · AUDIO HTTPS"
                    audioStream.active -> " · AUDIO DIBLOKIR"
                    else -> ""
                })
        }
    }
}

/**
 * Presentation-only gameplay gate shared by the GLES scene and Canvas fallback.
 * It mirrors native lifecycle facts so a touch/controller never looks enabled
 * before PlayerInit, PlayerSpawn, and the reliable PlayerSpawn ACK. The native
 * bridge still repeats every gate before advancing collision or transmitting.
 */
internal fun WorldFrame.gameplayInputBlockReason(): String? = when {
    !active -> "SESI BERAKHIR · INPUT OFF"
    !playerInit -> "MENUNGGU PLAYERINIT · INPUT OFF"
    !spawnConfirmationAcked && !spectate.active -> when {
        spawnConfirmationSent -> "MENUNGGU ACK PLAYERSPAWN · INPUT OFF"
        spawnRequestPending -> "MEMINTA IZIN SPAWN · INPUT OFF"
        spawnResponseKnown && !spawnAllowed -> "SPAWN DITOLAK SERVER · INPUT OFF"
        classRequestPending -> "MEMUAT CLASS SERVER · INPUT OFF"
        classSelectionKnown -> "CLASS SIAP · TEKAN SPAWN · INPUT OFF"
        else -> "MENUNGGU CLASS / SPAWN SERVER · INPUT OFF"
    }
    spectate.active -> "SPECTATOR · INPUT OFF"
    localHealthKnown && localHealth.isFinite() && localHealth <= 0f -> "RESPAWN SERVER · INPUT OFF"
    localControllableKnown && !localControllable -> "SERVER FREEZE · INPUT OFF"
    else -> null
}

/** One copied world-space origin used by radar/GLES while spectator is active. */
internal data class WorldSceneAnchor(
    val known: Boolean = false,
    val x: Float = 0f,
    val y: Float = 0f,
    val z: Float = 0f,
    val angle: Float = 0f,
    val followingSpectateTarget: Boolean = false
)

/**
 * Resolves spectator target IDs only against the immutable current frame. A
 * missing/stale target safely falls back to local anchor rather than creating a
 * position from an ID. This helper is visual-only; native remains sole owner of
 * spectator state and refuses local gameplay sync while it is active.
 */
internal fun WorldFrame.sceneAnchor(): WorldSceneAnchor {
    if (spectate.active && spectate.targetKnown) {
        if (spectate.targetVehicle) {
            val vehicle = vehicles.firstOrNull { it.id == spectate.targetId }
            if (vehicle != null && vehicle.x.isFinite() && vehicle.y.isFinite() &&
                vehicle.z.isFinite() && vehicle.angle.isFinite()
            ) {
                return WorldSceneAnchor(true, vehicle.x, vehicle.y, vehicle.z, vehicle.angle, true)
            }
        } else {
            if (spectate.targetId == localId && localVehicleKnown && localInVehicle) {
                val vehicle = vehicles.firstOrNull { it.id == localVehicleId }
                if (vehicle != null && vehicle.x.isFinite() && vehicle.y.isFinite() &&
                    vehicle.z.isFinite() && vehicle.angle.isFinite()
                ) {
                    return WorldSceneAnchor(true, vehicle.x, vehicle.y, vehicle.z, vehicle.angle, true)
                }
            }
            if (spectate.targetId == localId && localKnown && localX.isFinite() && localY.isFinite() &&
                localZ.isFinite() && localAngle.isFinite()
            ) {
                return WorldSceneAnchor(true, localX, localY, localZ, localAngle, true)
            }
            val actor = actors.firstOrNull { it.id == spectate.targetId }
            // A PlayerSpectatePlayer target can currently be seated. When the
            // paired remote vehicle is streamed, its exact copied transform is
            // the only safe visual anchor; do not use a stale free-ped point or
            // manufacture a seat offset. This remains renderer-only and does
            // not reclassify the player target as local vehicle authority.
            if (actor != null && actor.inVehicle) {
                val vehicle = vehicles.firstOrNull { it.id == actor.vehicleId }
                if (vehicle != null && vehicle.x.isFinite() && vehicle.y.isFinite() &&
                    vehicle.z.isFinite() && vehicle.angle.isFinite()
                ) {
                    return WorldSceneAnchor(true, vehicle.x, vehicle.y, vehicle.z, vehicle.angle, true)
                }
            }
            if (actor != null && actor.onFootKnown && actor.x.isFinite() && actor.y.isFinite() &&
                actor.z.isFinite() && actor.angle.isFinite()
            ) {
                return WorldSceneAnchor(true, actor.x, actor.y, actor.z, actor.angle, true)
            }
        }
    }
    return if (localKnown && localX.isFinite() && localY.isFinite() && localZ.isFinite() &&
        localAngle.isFinite()
    ) {
        WorldSceneAnchor(true, localX, localY, localZ, localAngle)
    } else {
        WorldSceneAnchor()
    }
}

/**
 * Resolves one label's static world point or documented player/vehicle offset
 * against immutable snapshot entities. Unknown/stale attachments are omitted
 * rather than rendered at an invented origin. LOS is intentionally not
 * simulated until the native collision renderer can evaluate it.
 */
internal fun WorldFrame.resolvedTextLabels(maxLabels: Int = 24): List<ResolvedWorldTextLabel> {
    val anchor = sceneAnchor()
    if (!anchor.known || maxLabels <= 0) return emptyList()
    return textLabels.asSequence().mapNotNull { label ->
        var baseX = 0f
        var baseY = 0f
        var baseZ = 0f
        when {
            label.playerAttachId != 65535 -> {
                if (label.playerAttachId == localId) {
                    baseX = localX
                    baseY = localY
                    baseZ = localZ
                } else {
                    val actor = actors.firstOrNull { it.id == label.playerAttachId } ?: return@mapNotNull null
                    baseX = actor.x
                    baseY = actor.y
                    baseZ = actor.z
                }
            }
            label.vehicleAttachId != 65535 -> {
                val vehicle = vehicles.firstOrNull { it.id == label.vehicleAttachId } ?: return@mapNotNull null
                baseX = vehicle.x
                baseY = vehicle.y
                baseZ = vehicle.z
            }
        }
        val x = baseX + label.x
        val y = baseY + label.y
        val z = baseZ + label.z
        if (!x.isFinite() || !y.isFinite() || !z.isFinite() || label.drawDistance <= 0f) {
            return@mapNotNull null
        }
        val dx = x - anchor.x
        val dy = y - anchor.y
        val dz = z - anchor.z
        val distanceSquared = dx * dx + dy * dy + dz * dz
        if (!distanceSquared.isFinite() || distanceSquared > label.drawDistance * label.drawDistance) {
            return@mapNotNull null
        }
        ResolvedWorldTextLabel(label, x, y, z, kotlin.math.sqrt(distanceSquared))
    }.sortedBy { it.distance }.take(maxLabels).toList()
}

/**
 * Applies only server name-tag policy/visibility and distance. The current
 * portable renderer deliberately does not pretend to perform GTA LOS tests;
 * `disableLos` is retained for a future collision-backed evaluator.
 */
internal fun WorldFrame.resolvedNameTags(maxTags: Int = 24): List<ResolvedWorldNameTag> {
    val anchor = sceneAnchor()
    if (!anchor.known || !nameTags.show || nameTags.drawDistance <= 0f || maxTags <= 0) {
        return emptyList()
    }
    val maximumSquared = nameTags.drawDistance * nameTags.drawDistance
    return actors.asSequence().mapNotNull { actor ->
        if (!actor.nameTagVisible) return@mapNotNull null
        val dx = actor.x - anchor.x
        val dy = actor.y - anchor.y
        val dz = actor.z - anchor.z
        val squared = dx * dx + dy * dy + dz * dz
        if (!squared.isFinite() || squared > maximumSquared) return@mapNotNull null
        ResolvedWorldNameTag(actor, kotlin.math.sqrt(squared))
    }.sortedBy { it.distance }.take(maxTags).toList()
}

/**
 * Native removes expired bubbles before this snapshot. Here we only enforce
 * current render distance and a small deterministic scene cap.
 */
internal fun WorldFrame.resolvedChatBubbles(maxBubbles: Int = 12): List<ResolvedWorldChatBubble> {
    val anchor = sceneAnchor()
    if (!anchor.known || maxBubbles <= 0) return emptyList()
    val resolved = ArrayList<ResolvedWorldChatBubble>(maxBubbles)
    fun add(
        wireId: Int,
        x: Float,
        y: Float,
        z: Float,
        bubble: WorldChatBubble
    ) {
        if (resolved.size >= maxBubbles || !bubble.active || bubble.text.isBlank() ||
            bubble.drawDistance <= 0f
        ) return
        val dx = x - anchor.x
        val dy = y - anchor.y
        val dz = z - anchor.z
        val squared = dx * dx + dy * dy + dz * dz
        if (!squared.isFinite() || squared > bubble.drawDistance * bubble.drawDistance) return
        resolved += ResolvedWorldChatBubble(wireId, x, y, z, bubble, kotlin.math.sqrt(squared))
    }
    if (!spectate.active) add(0x3_0000 + localId, localX, localY, localZ, localChatBubble)
    actors.asSequence().filter { it.chatBubble.active }.map { actor ->
        val dx = actor.x - anchor.x
        val dy = actor.y - anchor.y
        val dz = actor.z - anchor.z
        actor to (dx * dx + dy * dy + dz * dz)
    }.filter { (_, squared) -> squared.isFinite() }
        .sortedBy { (_, squared) -> squared }
        .forEach { (actor, _) ->
            add(0x2_0000 + actor.id, actor.x, actor.y, actor.z, actor.chatBubble)
        }
    return resolved
}

private class NativeWorldSurfaceView(context: Context) : View(context) {

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val markerPath = Path()
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
        if (!next.sceneAnchor().known && followLocal) {
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
        val anchor = frame.sceneAnchor()
        val originX = (if (anchor.known) anchor.x else 0f) + cameraOffsetX
        val originY = (if (anchor.known) anchor.y else 0f) + cameraOffsetY
        fun screenX(worldX: Float) = centerX + (worldX - originX) * scale
        fun screenY(worldY: Float) = centerY - (worldY - originY) * scale

        drawMapFrame(canvas)
        canvas.save()
        canvas.clipRect(mapBounds)
        drawGrid(canvas, originX, originY, scale, ::screenX, ::screenY)
        frame.gangZones.forEach { zone ->
            drawGangZone(canvas, zone, screenX(zone.minX), screenY(zone.maxY),
                screenX(zone.maxX), screenY(zone.minY))
        }
        frame.mapIcons.forEach { icon ->
            drawMapIcon(canvas, icon, screenX(icon.x), screenY(icon.y))
        }
        if (frame.checkpoint.active) {
            drawCheckpoint(canvas, screenX(frame.checkpoint.x), screenY(frame.checkpoint.y),
                max(dp(8f), frame.checkpoint.size * scale), Color.rgb(255, 222, 62), false)
        }
        if (frame.raceCheckpoint.active) {
            drawCheckpoint(canvas, screenX(frame.raceCheckpoint.x), screenY(frame.raceCheckpoint.y),
                max(dp(8f), frame.raceCheckpoint.size * scale), Color.rgb(255, 132, 54), true)
        }
        frame.pickups.forEach { pickup ->
            drawPickup(canvas, pickup, screenX(pickup.x), screenY(pickup.y))
        }
        frame.objects.forEach { objectValue ->
            drawServerObject(canvas, objectValue, screenX(objectValue.x), screenY(objectValue.y))
        }
        frame.playerAttachedObjects.forEach { attachment ->
            drawPlayerAttachedObject(
                canvas,
                attachment,
                screenX(attachment.x),
                screenY(attachment.y)
            )
        }
        frame.actors.forEach { actor ->
            val dx = actor.x - (if (anchor.known) anchor.x else 0f)
            val dy = actor.y - (if (anchor.known) anchor.y else 0f)
            val nameDistance = kotlin.math.sqrt(dx * dx + dy * dy)
            val showName = anchor.known && frame.nameTags.show && actor.nameTagVisible &&
                frame.nameTags.drawDistance > 0f && nameDistance.isFinite() &&
                nameDistance <= frame.nameTags.drawDistance
            drawActor(canvas, actor, screenX(actor.x), screenY(actor.y), showName)
        }
        frame.serverActors.forEach { actor ->
            drawServerActor(canvas, actor, screenX(actor.x), screenY(actor.y))
        }
        frame.vehicles.forEach { vehicle ->
            drawVehicle(canvas, vehicle, screenX(vehicle.x), screenY(vehicle.y))
        }
        frame.resolvedTextLabels().forEach { label ->
            drawWorldTextLabel(canvas, label, screenX(label.x), screenY(label.y))
        }
        if (!frame.spectate.active && frame.localKnown) {
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

    private fun drawGangZone(
        canvas: Canvas,
        zone: WorldGangZone,
        left: Float,
        top: Float,
        right: Float,
        bottom: Float
    ) {
        if (!left.isFinite() || !top.isFinite() || !right.isFinite() || !bottom.isFinite()) return
        val rect = RectF(min(left, right), min(top, bottom), max(left, right), max(top, bottom))
        if (!RectF.intersects(rect, mapBounds)) return
        // SA-MP flashes a territory between its base and flash colour. The
        // phase is only Android presentation timing; colour/visibility still
        // come exclusively from the native server RPC reducer.
        val flashPhase = (System.nanoTime() / 420_000_000L) % 2L == 0L
        val base = colorFromAbgr(
            if (zone.flashing && flashPhase) zone.flashColourAbgr else zone.colourAbgr
        )
        val alpha = Color.alpha(base)
        paint.style = Paint.Style.FILL
        paint.color = Color.argb((alpha * 0.34f).toInt().coerceIn(18, 130),
            Color.red(base), Color.green(base), Color.blue(base))
        canvas.drawRect(rect, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(if (zone.flashing && flashPhase) 2.0f else 1.25f)
        paint.color = Color.argb(alpha.coerceAtLeast(105),
            Color.red(base), Color.green(base), Color.blue(base))
        canvas.drawRect(rect, paint)
    }

    private fun drawMapIcon(canvas: Canvas, icon: WorldMapIcon, x: Float, y: Float) {
        if (!containsMapPoint(x, y)) return
        val color = colorFromRgba(icon.rgba)
        val radius = dp(if (icon.style == 2 || icon.style == 3) 7f else 5f)
        markerPath.reset()
        markerPath.moveTo(x, y - radius)
        markerPath.lineTo(x + radius, y)
        markerPath.lineTo(x, y + radius)
        markerPath.lineTo(x - radius, y)
        markerPath.close()
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(Color.alpha(color).coerceAtLeast(185), Color.red(color), Color.green(color), Color.blue(color))
        canvas.drawPath(markerPath, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1.2f)
        paint.color = Color.argb(232, 238, 248, 255)
        canvas.drawPath(markerPath, paint)
        // Type is deliberately not mislabelled as a GTA sprite. The small
        // center dot keeps overlapping raw-type markers distinguishable.
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(235, 6, 15, 22)
        canvas.drawCircle(x, y, dp(1.3f), paint)
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

    private fun drawPickup(canvas: Canvas, pickup: WorldPickup, x: Float, y: Float) {
        if (!containsMapPoint(x, y)) return
        val radius = dp(5f)
        paint.style = Paint.Style.FILL
        paint.color = if (pickup.spawnType == 14) Color.argb(230, 255, 174, 80)
        else Color.argb(230, 104, 229, 152)
        canvas.drawCircle(x, y, radius, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1.3f)
        paint.color = Color.rgb(225, 255, 255)
        canvas.drawCircle(x, y, radius, paint)
        paint.strokeWidth = dp(1.1f)
        canvas.drawLine(x - radius * 0.5f, y, x + radius * 0.5f, y, paint)
        canvas.drawLine(x, y - radius * 0.5f, x, y + radius * 0.5f, paint)
    }

    private fun drawServerObject(canvas: Canvas, objectValue: WorldObject, x: Float, y: Float) {
        if (!containsMapPoint(x, y)) return
        // Object transforms may be actively MoveObject-interpolated. This radar
        // marker deliberately does not pretend to be the GTA model itself.
        val size = dp(4.5f)
        canvas.save()
        canvas.rotate(objectValue.rotationZ, x, y)
        paint.style = Paint.Style.FILL
        paint.color = if (objectValue.moving) Color.argb(220, 255, 111, 74) else Color.argb(205, 246, 184, 53)
        canvas.drawRect(x - size, y - size, x + size, y + size, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1f)
        paint.color = Color.rgb(255, 229, 151)
        canvas.drawRect(x - size, y - size, x + size, y + size, paint)
        canvas.restore()
    }

    private fun drawPlayerAttachedObject(
        canvas: Canvas,
        attachment: WorldPlayerAttachedObject,
        x: Float,
        y: Float
    ) {
        if (!containsMapPoint(x, y)) return
        // Same renderer-only authority marker as the GLES fallback. The icon
        // is intentionally small: it signals a retained server accessory,
        // never claims exact GTA bone/mesh fidelity on this Canvas radar.
        val sourceColour = if (attachment.colour1Abgr != 0L) {
            attachment.colour1Abgr
        } else {
            attachment.colour2Abgr
        }
        val color = if (sourceColour == 0L) Color.rgb(255, 186, 68) else colorFromAbgr(sourceColour)
        val size = dp(2.6f)
        canvas.save()
        canvas.rotate(attachment.rotationZ, x, y)
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(210, Color.red(color), Color.green(color), Color.blue(color))
        canvas.drawRect(x - size, y - size, x + size, y + size, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(0.9f)
        paint.color = Color.WHITE
        canvas.drawRect(x - size, y - size, x + size, y + size, paint)
        canvas.restore()
    }

    private fun drawActor(canvas: Canvas, actor: WorldActor, x: Float, y: Float, showName: Boolean) {
        if (!containsMapPoint(x, y)) return
        val color = if (actor.wasted) Color.rgb(244, 72, 72) else colorFromRgba(actor.rgba)
        val radius = dp(if (actor.onFootKnown) 7f else 5f)
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(if (actor.wasted || actor.onFootKnown) 255 else 145,
            Color.red(color), Color.green(color), Color.blue(color))
        canvas.drawCircle(x, y, radius, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1.5f)
        paint.color = Color.WHITE
        canvas.drawCircle(x, y, radius, paint)
        if (actor.wasted) {
            // RPC 166 is remote observer presentation only. The red cross is
            // intentionally not a target, damage state, or local death screen.
            paint.strokeWidth = dp(2f)
            paint.color = Color.rgb(255, 228, 228)
            canvas.drawLine(x - radius * 0.55f, y - radius * 0.55f, x + radius * 0.55f, y + radius * 0.55f, paint)
            canvas.drawLine(x + radius * 0.55f, y - radius * 0.55f, x - radius * 0.55f, y + radius * 0.55f, paint)
            drawLabel(canvas, "WASTED #${actor.id}", x + dp(9f), y - dp(9f), color)
        } else {
            if (showName) {
                drawLabel(canvas, actor.name.ifBlank { "#${actor.id}" }, x + dp(9f), y - dp(9f), color)
            }
            drawForcedAnimationCue(canvas, actor.forcedAnimation, x, y, radius, color)
        }
    }

    private fun drawForcedAnimationCue(
        canvas: Canvas,
        animation: WorldForcedAnimation,
        x: Float,
        y: Float,
        baseRadius: Float,
        sourceColor: Int
    ) {
        if (!animation.active) return
        // This intentionally calls itself an ANIM cue rather than pretending
        // Canvas can execute a GTA IFP skeleton. It preserves server intent
        // visibly while actual asset-backed ped animation remains separate.
        val cueColor = if (animation.freeze) Color.rgb(255, 176, 72) else Color.rgb(111, 234, 255)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(if (animation.loop) 1.8f else 1.2f)
        paint.color = cueColor
        canvas.drawCircle(x, y, baseRadius + dp(3.2f), paint)
        if (animation.lockX || animation.lockY) {
            canvas.drawCircle(x, y, baseRadius + dp(5.1f), paint)
        }
        val token = "${animation.library}/${animation.name}".take(44)
        drawLabel(canvas, "ANIM $token", x + dp(9f), y + dp(7f), sourceColor)
    }

    private fun drawServerActor(canvas: Canvas, actor: WorldServerActor, x: Float, y: Float) {
        if (!containsMapPoint(x, y)) return
        val color = if (actor.invulnerable) Color.rgb(255, 202, 70) else Color.rgb(255, 126, 62)
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(235, Color.red(color), Color.green(color), Color.blue(color))
        canvas.drawRect(x - dp(5f), y - dp(5f), x + dp(5f), y + dp(5f), paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(if (actor.invulnerable) 2f else 1.2f)
        paint.color = Color.WHITE
        canvas.drawRect(x - dp(5f), y - dp(5f), x + dp(5f), y + dp(5f), paint)
        drawLabel(canvas, "NPC #${actor.id}", x + dp(8f), y - dp(8f), color)
        drawForcedAnimationCue(canvas, actor.forcedAnimation, x, y, dp(5f), color)
    }

    private fun drawVehicle(canvas: Canvas, vehicle: WorldVehicle, x: Float, y: Float) {
        if (!containsMapPoint(x, y)) return
        val bodyColor = vehicleMarkerColor(vehicle.colour1, vehicle.colour2)
        val objective = vehicle.parametersKnown && vehicle.objective == 1
        val locked = vehicle.parametersKnown && vehicle.doors == 1
        if (objective) {
            // Legacy SetVehicleParamsForPlayer/RPC-24 objective is a
            // recipient-specific GTA arrow. Canvas has no GTA arrow asset, so
            // draw a clearly portable yellow diamond cue from that exact fact.
            val radius = dp(12f)
            markerPath.reset()
            markerPath.moveTo(x, y - radius)
            markerPath.lineTo(x + radius * 0.7f, y)
            markerPath.lineTo(x, y + radius)
            markerPath.lineTo(x - radius * 0.7f, y)
            markerPath.close()
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = dp(1.8f)
            paint.color = Color.rgb(255, 222, 62)
            canvas.drawPath(markerPath, paint)
        }
        val halfWidth = dp(8f)
        val halfHeight = dp(4.5f)
        canvas.save()
        canvas.rotate(vehicle.angle, x, y)
        paint.style = Paint.Style.FILL
        paint.color = if (vehicle.dynamicSyncKnown) bodyColor else Color.argb(
            155, Color.red(bodyColor), Color.green(bodyColor), Color.blue(bodyColor)
        )
        canvas.drawRoundRect(
            RectF(x - halfWidth, y - halfHeight, x + halfWidth, y + halfHeight),
            dp(2f),
            dp(2f),
            paint
        )
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1.2f)
        paint.color = Color.WHITE
        canvas.drawRoundRect(
            RectF(x - halfWidth, y - halfHeight, x + halfWidth, y + halfHeight),
            dp(2f),
            dp(2f),
            paint
        )
        paint.color = Color.argb(235, 10, 23, 30)
        canvas.drawLine(x, y - halfHeight, x, y + halfHeight, paint)
        canvas.restore()
        val label = buildString {
            append(if (vehicle.dynamicSyncKnown) "V${vehicle.id} SYNC" else "V${vehicle.id}")
            if (objective) append(" · OBJ")
            if (locked) append(" · LOCK")
        }
        drawLabel(canvas, label, x + dp(10f), y + dp(13f), bodyColor)
    }

    private fun drawWorldTextLabel(
        canvas: Canvas,
        label: ResolvedWorldTextLabel,
        x: Float,
        y: Float
    ) {
        if (!containsMapPoint(x, y)) return
        val text = label.source.text.replace(WORLD_LABEL_WHITESPACE, " ").trim().take(20)
        if (text.isEmpty()) return
        val color = colorFromRgba(label.source.rgba)
        paint.textAlign = Paint.Align.CENTER
        paint.textSize = dp(9f)
        paint.style = Paint.Style.FILL
        paint.color = Color.argb(215, 2, 8, 12)
        val width = paint.measureText(text) + dp(7f)
        canvas.drawRoundRect(RectF(x - width * 0.5f, y - dp(22f), x + width * 0.5f, y - dp(9f)),
            dp(3f), dp(3f), paint)
        paint.color = color
        canvas.drawText(text, x, y - dp(12f), paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(1f)
        paint.color = Color.argb(210, Color.red(color), Color.green(color), Color.blue(color))
        canvas.drawLine(x, y - dp(8f), x, y - dp(2f), paint)
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
        canvas.drawText("${frame.actors.size} P · ${frame.serverActors.size} NPC · ${frame.vehicles.size} V · ${frame.playerAttachedObjects.size} A · ${frame.gangZones.size} Z · ${worldRadius.toInt()}m", mapBounds.right - dp(9f), dp(22f), paint)

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

    /** Gang-zone writers use numeric ABGR just like textdraw colour fields. */
    private fun colorFromAbgr(abgr: Long): Int {
        val value = abgr and 0xFFFF_FFFFL
        val a = ((value shr 24) and 0xFF).toInt()
        val b = ((value shr 16) and 0xFF).toInt()
        val g = ((value shr 8) and 0xFF).toInt()
        val r = (value and 0xFF).toInt()
        return Color.argb(a, r, g, b)
    }

    private fun colorFromRgba(rgba: Long): Int {
        if (rgba == 0L) return Color.rgb(190, 104, 255)
        val r = ((rgba shr 24) and 0xFF).toInt()
        val g = ((rgba shr 16) and 0xFF).toInt()
        val b = ((rgba shr 8) and 0xFF).toInt()
        val a = (rgba and 0xFF).toInt()
        return Color.argb(if (a < 48) 255 else a, r, g, b)
    }

    private fun vehicleMarkerColor(primary: Int, secondary: Int): Int {
        // SA-MP sends palette indices, not direct RGBA. Until user-owned model
        // data/palette mapping exists, derive a stable visible proxy color from
        // the authoritative IDs rather than pretending it is GTA paint.
        val hue = ((primary * 37 + secondary * 13) % 360).toFloat()
        return Color.HSVToColor(floatArrayOf(hue, 0.58f, 0.96f))
    }

    private fun dp(value: Float): Float = value * resources.displayMetrics.density
}
