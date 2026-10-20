package com.xykal.sampdroid.ui

import android.app.ActivityManager
import android.content.Context
import android.graphics.Color
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.opengl.Matrix
import android.view.MotionEvent
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.xykal.sampdroid.NativeBridge
import com.xykal.sampdroid.NativeTexturedDffPreview
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin

private const val DFF_PREVIEW_MAX_VERTICES = 120000
private const val DFF_PREVIEW_MAX_TEXTURE_PIXELS = 1024 * 1024
private const val GLES_2_0_VERSION = 0x00020000
private const val GAMEPLAY_TICK_DELAY_MS = 40L
private const val GAMEPLAY_MAX_STEP_SECONDS = 0.05f
private const val MAP_RENDER_REFRESH_DELAY_MS = 1200L
// Cached vehicle/object DFF transforms refresh independently of slower IPL
// streaming, so remote Packet-200 movement does not wait for a map reload.
private const val DYNAMIC_MODEL_REFRESH_DELAY_MS = 400L
private const val MAP_RENDER_RADIUS = 360f
private const val MAP_RENDER_MAX_INSTANCES = 72
// One native DFF batch can now resolve both static server objects and streamed
// vehicles through the user-selected IDE catalog. Keep this below the native
// input bound and leave enough vertex budget for the nearby IPL world batch.
private const val MAP_RENDER_MAX_DYNAMIC_MODELS = 128
private const val MAP_RENDER_MAX_DYNAMIC_MODEL_VERTICES = 60000
private const val MAP_RENDER_MAX_VERTICES = 120000
private const val MAP_RENDER_MAX_TEXTURE_BATCHES = 4
private const val MAP_RENDER_MAX_TEXTURE_TOTAL_PIXELS = 1024 * 1024
// The GLES scene normally renders only on immutable snapshot/camera changes.
// A short 30fps settle sequence makes remote server positions visually smooth
// without permanently burning an ARMv7 GPU at continuous-render cadence.
private const val REMOTE_PRESENTATION_TICK_MS = 40L
private const val REMOTE_PRESENTATION_TICKS = 2
private const val REMOTE_ACTOR_BLEND_NANOS = 90_000_000L
private const val REMOTE_ACTOR_SNAP_DISTANCE = 36f

/**
 * Conservative per-device data/render budget. The hard constants above remain
 * the safety ceiling; this only lowers requests for Android devices with a
 * small Java heap or the platform low-RAM classification, preserving an APK
 * path for 32-bit/older phones instead of assuming desktop-sized buffers.
 */
private data class NativeRenderBudget(
    val label: String,
    val mapRadius: Float,
    val maxMapInstances: Int,
    val maxMapVertices: Int,
    val maxDynamicModels: Int,
    val maxDynamicVertices: Int,
    val maxTextureBatches: Int,
    val maxTexturePixels: Int,
    val maxPreviewVertices: Int
) {
    companion object {
        fun forDevice(context: Context): NativeRenderBudget {
            val activityManager = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
            val memoryClass = activityManager?.memoryClass ?: 256
            val lowRam = activityManager?.isLowRamDevice == true
            return when {
                lowRam || memoryClass <= 128 -> NativeRenderBudget(
                    label = "RENDER LITE ${memoryClass}MB",
                    mapRadius = 180f,
                    maxMapInstances = 32,
                    maxMapVertices = 36_000,
                    maxDynamicModels = 48,
                    maxDynamicVertices = 24_000,
                    maxTextureBatches = 2,
                    maxTexturePixels = 256 * 1024,
                    maxPreviewVertices = 30_000
                )
                memoryClass <= 256 -> NativeRenderBudget(
                    label = "RENDER HEMAT ${memoryClass}MB",
                    mapRadius = 260f,
                    maxMapInstances = 48,
                    maxMapVertices = 72_000,
                    maxDynamicModels = 80,
                    maxDynamicVertices = 40_000,
                    maxTextureBatches = 3,
                    maxTexturePixels = 512 * 1024,
                    maxPreviewVertices = 60_000
                )
                else -> NativeRenderBudget(
                    label = "RENDER PENUH ${memoryClass}MB",
                    mapRadius = MAP_RENDER_RADIUS,
                    maxMapInstances = MAP_RENDER_MAX_INSTANCES,
                    maxMapVertices = MAP_RENDER_MAX_VERTICES,
                    maxDynamicModels = MAP_RENDER_MAX_DYNAMIC_MODELS,
                    maxDynamicVertices = MAP_RENDER_MAX_DYNAMIC_MODEL_VERTICES,
                    maxTextureBatches = MAP_RENDER_MAX_TEXTURE_BATCHES,
                    maxTexturePixels = MAP_RENDER_MAX_TEXTURE_TOTAL_PIXELS,
                    maxPreviewVertices = DFF_PREVIEW_MAX_VERTICES
                )
            }
        }
    }
}

private data class DffPreviewPayload(
    val colorVertices: FloatArray,
    val textured: NativeTexturedDffPreview?
)

/** A copied IPL/DFF map batch stays tied to the local anchor used by native. */
private data class NearbyWorldPayload(
    val vertices: FloatArray,
    val textured: List<NativeTexturedDffPreview>,
    val anchorX: Float,
    val anchorY: Float,
    val anchorZ: Float
)

/** Copied dynamic DFF vertices remain relative to this specific source anchor. */
private data class DynamicModelPayload(
    val vertices: FloatArray,
    val textured: List<NativeTexturedDffPreview>,
    val anchorX: Float,
    val anchorY: Float,
    val anchorZ: Float
)

// UTF-8 source text is capped to 31 bytes by the native reducer. Hex keeps the
// compact semicolon/comma renderer record grammar unambiguous even for commas,
// newlines, or non-ASCII player-authored sign text.
private fun encodeObjectMaterialText(value: String): String {
    val bytes = value.toByteArray(Charsets.UTF_8)
    val digits = "0123456789abcdef"
    return buildString(bytes.size * 2) {
        bytes.forEach { byte ->
            val unsigned = byte.toInt() and 0xFF
            append(digits[unsigned ushr 4])
            append(digits[unsigned and 0x0F])
        }
    }
}

private data class DynamicModelInput(
    val model: Long,
    val x: Float,
    val y: Float,
    val z: Float,
    val rotationX: Float,
    val rotationY: Float,
    val rotationZ: Float,
    val drawDistance: Float,
    // SetPlayerAttachedObject supplies a source scale. Other dynamic entity
    // types stay at their immutable DFF dimensions.
    val scaleX: Float = 1f,
    val scaleY: Float = 1f,
    val scaleZ: Float = 1f,
    // Only server-object/attached-object inputs carry bounded material data.
    // Vehicle/ped inputs retain their DFF/IDE material dictionaries unchanged.
    val materialOverrides: List<WorldObjectMaterial> = emptyList()
)

/** One GLES-projected server spatial text (3D label or nametag); coordinates match Canvas HUD. */
internal data class ProjectedWorldTextLabel(
    val wireId: Int,
    val x: Float,
    val y: Float,
    val rgba: Long,
    val text: String,
    val distance: Float
)

/**
 * Small lock-free hand-off from the GL thread to the Canvas HUD. It holds only
 * freshly copied display coordinates/text, never a native pointer, socket, or
 * mutable WorldFrame. HUD redraw is already bounded by the 10 Hz world poll.
 */
internal class WorldLabelProjection {
    @Volatile
    private var labels: List<ProjectedWorldTextLabel> = emptyList()

    fun submit(next: List<ProjectedWorldTextLabel>) {
        labels = next.toList()
    }

    fun snapshot(): List<ProjectedWorldTextLabel> = labels
}

private fun loadDffPreviewPayload(budget: NativeRenderBudget): DffPreviewPayload = DffPreviewPayload(
    colorVertices = runCatching {
        NativeBridge.nativeGameDataLatestDffPreview(budget.maxPreviewVertices)
    }.getOrElse { FloatArray(0) },
    textured = runCatching {
        NativeBridge.nativeGameDataLatestTexturedDffPreview(
            budget.maxPreviewVertices,
            budget.maxTexturePixels
        )
    }.getOrNull()
)

private fun loadDynamicModelPayload(
    frame: WorldFrame,
    budget: NativeRenderBudget
): DynamicModelPayload? {
    // Dynamic entities have no per-entity interior metadata on this path, so
    // retain the conservative exterior gate. A resolved server spectator target
    // merely changes the visual/asset anchor; it never bypasses that gate.
    val anchor = frame.sceneAnchor()
    if (!anchor.known || (frame.localInteriorKnown && frame.localInterior != 0)) return null
    // Static RPC objects, streamed SA-MP vehicles, player-attached objects,
    // remote player skins, and ShowActor NPC skins can resolve through the
    // user-owned IDE/IMG catalog.
    // `cars`/`peds` map dynamic model IDs to DFF names; this payload is
    // visual-only and never feeds collision/gameplay.
    val dynamicInputs = ArrayList<DynamicModelInput>(
        frame.objects.size + frame.playerAttachedObjects.size + frame.vehicles.size +
            frame.actors.size + frame.serverActors.size + 1
    )
    frame.objects.forEach { objectValue ->
        // `WorldState` already samples MoveObject monotonically and resolves a
        // bounded, fail-closed object/vehicle/player attachment chain into this
        // immutable frame. Reuse that authoritative sampled transform rather
        // than dropping every moving or attached server object to proxy-only
        // geometry between updates.
        if (objectValue.model in 1L..65535L && objectValue.x.isFinite() &&
            objectValue.y.isFinite() && objectValue.z.isFinite() && objectValue.rotationX.isFinite() &&
            objectValue.rotationY.isFinite() && objectValue.rotationZ.isFinite() &&
            objectValue.drawDistance.isFinite() && objectValue.drawDistance >= 0f
        ) {
            dynamicInputs += DynamicModelInput(
                objectValue.model, objectValue.x, objectValue.y, objectValue.z,
                objectValue.rotationX, objectValue.rotationY, objectValue.rotationZ,
                objectValue.drawDistance, materialOverrides = objectValue.materials
            )
        }
    }
    frame.playerAttachedObjects.forEach { attachment ->
        // Native samples an existing streamed/local parent and explicitly uses
        // only a portable approximate bone anchor. Reuse the bounded source
        // scale and its two source ABGR colour channels as color-only material
        // overrides; neither creates a ped skeleton or gameplay hitbox.
        if (attachment.model in 1L..65535L && attachment.x.isFinite() &&
            attachment.y.isFinite() && attachment.z.isFinite() &&
            attachment.rotationX.isFinite() && attachment.rotationY.isFinite() &&
            attachment.rotationZ.isFinite() && attachment.scaleX.isFinite() &&
            attachment.scaleY.isFinite() && attachment.scaleZ.isFinite() &&
            abs(attachment.scaleX) >= 0.001f && abs(attachment.scaleX) <= 100f &&
            abs(attachment.scaleY) >= 0.001f && abs(attachment.scaleY) <= 100f &&
            abs(attachment.scaleZ) >= 0.001f && abs(attachment.scaleZ) <= 100f
        ) {
            dynamicInputs += DynamicModelInput(
                attachment.model, attachment.x, attachment.y, attachment.z,
                attachment.rotationX, attachment.rotationY, attachment.rotationZ,
                budget.mapRadius, attachment.scaleX, attachment.scaleY, attachment.scaleZ,
                materialOverrides = listOf(
                    WorldObjectMaterial(slot = 0, sourceModel = 65535, colourAbgr = attachment.colour1Abgr),
                    WorldObjectMaterial(slot = 1, sourceModel = 65535, colourAbgr = attachment.colour2Abgr)
                )
            )
        }
    }
    if (!frame.spectate.active && frame.localKnown && frame.localSpawnInfoKnown && !frame.localInVehicle &&
        frame.localModel in 0L..65535L && frame.localAngle.isFinite()
    ) {
        // SetSpawnInfo establishes the local model gate; a later exact
        // SetPlayerSkin can replace that retained model. It must still pair
        // with an authoritative local transform: never place a valid CJ/model
        // zero (or any skin) at the default origin before localKnown arrives.
        // Model zero is CJ, so do not use a nonzero-only test here.
        dynamicInputs += DynamicModelInput(
            frame.localModel, frame.localX, frame.localY, frame.localZ,
            0f, 0f, frame.localAngle, budget.mapRadius
        )
    }
    frame.actors.forEach { actor ->
        // Ped ID zero is valid (CJ), unlike an absent static-object ID. Actor
        // state is already bounded by StreamIn/Out before it reaches this UI.
        // A short RPC-166 observer death cue deliberately uses the clear proxy
        // below rather than leaving a stale upright DFF ped through "WASTED".
        if (!actor.inVehicle && !actor.wasted && actor.model in 0L..65535L && actor.x.isFinite() &&
            actor.y.isFinite() && actor.z.isFinite() && actor.angle.isFinite()
        ) {
            dynamicInputs += DynamicModelInput(
                actor.model, actor.x, actor.y, actor.z,
                0f, 0f, actor.angle, budget.mapRadius
            )
        }
    }
    frame.serverActors.forEach { actor ->
        // ShowActor's model/transform is already sourced from exact server
        // RPCs. It is a visual NPC only; this shared DFF path never turns it
        // into a collision obstacle or a target/hit assertion.
        if (actor.model in 0L..65535L && actor.x.isFinite() && actor.y.isFinite() &&
            actor.z.isFinite() && actor.angle.isFinite()
        ) {
            dynamicInputs += DynamicModelInput(
                actor.model, actor.x, actor.y, actor.z,
                0f, 0f, actor.angle, budget.mapRadius
            )
        }
    }
    frame.vehicles.forEach { vehicle ->
        if (vehicle.model in 1L..65535L && vehicle.x.isFinite() && vehicle.y.isFinite() &&
            vehicle.z.isFinite() && vehicle.angle.isFinite() &&
            (!vehicle.attitudeKnown ||
                (vehicle.rotationX.isFinite() && vehicle.rotationY.isFinite() && vehicle.rotationZ.isFinite()))
        ) {
            // Packet-200's compressed quaternion reaches this portable DFF path
            // as a native source-derived GTA X→Y→Z attitude. A StreamIn/RPC-160
            // vehicle remains yaw-only; neither branch invents suspension or
            // physics, and proxy geometry stays the asset-missing fallback.
            dynamicInputs += DynamicModelInput(
                vehicle.model, vehicle.x, vehicle.y, vehicle.z,
                if (vehicle.attitudeKnown) vehicle.rotationX else 0f,
                if (vehicle.attitudeKnown) vehicle.rotationY else 0f,
                if (vehicle.attitudeKnown) vehicle.rotationZ else vehicle.angle,
                budget.mapRadius
            )
        }
    }
    val nearestInputs = dynamicInputs.asSequence()
        .sortedBy { input ->
            val dx = input.x - anchor.x
            val dy = input.y - anchor.y
            val dz = input.z - anchor.z
            dx * dx + dy * dy + dz * dz
        }
        .take(budget.maxDynamicModels)
        .toList()
    if (nearestInputs.isEmpty()) {
        return DynamicModelPayload(FloatArray(0), emptyList(), anchor.x, anchor.y, anchor.z)
    }
    val modelIds = IntArray(nearestInputs.size)
    // Native copies ten floats per dynamic model: transform, draw distance,
    // then bounded object-local scale. Keep this ABI paired with NativeBridge.
    val transforms = FloatArray(nearestInputs.size * 10)
    val materialOverrideSpecs = Array(nearestInputs.size) { "" }
    nearestInputs.forEachIndexed { index, input ->
        modelIds[index] = input.model.toInt()
        val offset = index * 10
        transforms[offset] = input.x
        transforms[offset + 1] = input.y
        transforms[offset + 2] = input.z
        transforms[offset + 3] = input.rotationX
        transforms[offset + 4] = input.rotationY
        transforms[offset + 5] = input.rotationZ
        transforms[offset + 6] = input.drawDistance
        transforms[offset + 7] = input.scaleX
        transforms[offset + 8] = input.scaleY
        transforms[offset + 9] = input.scaleZ
        // The native/Kotlin parser has already rejected separators/path bytes.
        // Preserve the source model too: u16 65535 is the API's color-only
        // model -1 sentinel, so native need not invent a TXD lookup for it.
        // Default `D` records carry lookup names except u16-65535 color-only
        // records, whose paired names are intentionally empty; Text `T` carries
        // exact bounded source fields plus UTF-8 text as hex. This remains one copied
        // string per dynamic model and retains no Java object in native state.
        materialOverrideSpecs[index] = input.materialOverrides
            .sortedBy { it.slot }
            .take(16)
            .joinToString(";") { material ->
                if (material.textActive) {
                    "T,${material.slot},${material.textMaterialSize},${material.textFontSize}," +
                        "${if (material.textBold) 1 else 0},${material.textColourArgb}," +
                        "${material.textBackgroundColourArgb},${material.textAlignment}," +
                        encodeObjectMaterialText(material.text)
                } else {
                    "D,${material.slot},${material.sourceModel},${material.colourAbgr}," +
                        "${material.dictionary},${material.texture}"
                }
            }
    }
    // Fetch the prelit batch first: it also warms the same bounded DFF catalog
    // used by the optional material overlay. Both calls consume identical
    // copied transforms, so an unavailable TXD never moves or invents a model.
    val vertices = runCatching {
        NativeBridge.nativeGameDataServerObjectsPreview(
            anchor.x, anchor.y, anchor.z, budget.mapRadius,
            modelIds, transforms, budget.maxDynamicVertices
        )
    }.getOrElse { FloatArray(0) }
    val textured = runCatching {
        NativeBridge.nativeGameDataServerObjectsTexturedPreviews(
            anchor.x, anchor.y, anchor.z, budget.mapRadius,
            modelIds, transforms, materialOverrideSpecs, budget.maxTextureBatches,
            budget.maxDynamicVertices,
            budget.maxTexturePixels
        )
    }.getOrNull()?.toList().orEmpty()
    return DynamicModelPayload(vertices, textured, anchor.x, anchor.y, anchor.z)
}

private fun loadNearbyWorldPayload(
    frame: WorldFrame,
    budget: NativeRenderBudget
): NearbyWorldPayload? {
    val anchor = frame.sceneAnchor()
    if (!anchor.known) return null
    // Unknown interior retains the conservative exterior default. Once RPC 156
    // is known, native selects only IPL placements bearing that exact interior
    // ID; coordinates alone can never blend two GTA interiors together.
    val interior = if (frame.localInteriorKnown) frame.localInterior else 0
    if (interior !in 0..255) return null
    val vertices = runCatching {
        NativeBridge.nativeGameDataNearbyWorldPreview(
            anchor.x,
            anchor.y,
            anchor.z,
            budget.mapRadius,
            budget.maxMapInstances,
            budget.maxMapVertices,
            interior
        )
    }.getOrElse { FloatArray(0) }
    // The color batch runs first and lazily discovers nearby IPL DFFs. The
    // optional overlays then resolve a few distinct IDE/TXD materials without
    // making absence/unsupported formats fatal to the map scene.
    val textured = runCatching {
        NativeBridge.nativeGameDataNearbyWorldTexturedPreviews(
            anchor.x,
            anchor.y,
            anchor.z,
            budget.mapRadius,
            budget.maxTextureBatches,
            budget.maxMapVertices,
            budget.maxTexturePixels,
            interior
        )
    }.getOrNull()?.toList().orEmpty()
    return NearbyWorldPayload(vertices, textured, anchor.x, anchor.y, anchor.z)
}

/**
 * A small GPU-backed scene bridge for the Android-native client.
 *
 * This draws generated actor/authority markers plus copied, bounded IPL/DFF
 * placements for the server-selected interior and dynamic object/vehicle/actor
 * DFF batches from native
 * user-owned data. A few safely resolved IDE/TXD material overlays may texture static or dynamic
 * subsets; it can also preview one manually selected portable DFF mesh. The scene owns no map data itself: its
 * gameplay ticker delegates only collision-grounded movement to native code. It
 * is not yet a complete textured GTA renderer or full GTA simulation.
 */
object NativeWorldSceneDialog {

    fun show(activity: AppCompatActivity) {
        val activityManager = activity.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
        val glEsVersion = activityManager?.deviceConfigurationInfo?.reqGlEsVersion ?: 0
        if (glEsVersion < GLES_2_0_VERSION) {
            // Do not dead-end the primary "Buka dunia" action on older ARMv7
            // hardware. The Canvas surface retains the same native session,
            // HUD, controls, and collision-tick path without pretending it is
            // the GPU scene.
            NativeWorldDialog.show(activity)
            return
        }
        val renderBudget = NativeRenderBudget.forDevice(activity)
        val density = activity.resources.displayMetrics.density
        // Scoped to this scene dialog; all RPC-41 media is stopped before a
        // renderer/modal transition and has no native gameplay write path.
        val audioStreamController = NativeAudioStreamController(activity)
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((12f * density).toInt(), (8f * density).toInt(), (12f * density).toInt(), 0)
            setBackgroundColor(Color.rgb(8, 17, 24))
        }
        val intro = TextView(activity).apply {
            text = "DUNIA NATIVE · SCENE GPU + HUD\n" +
                "Touch, keyboard Bluetooth, dan gamepad siap dipakai setelah spawn server + collision siap. " +
                "Asset yang belum portable tetap ditampilkan sebagai marker aman. ${renderBudget.label}."
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
        val labelProjection = WorldLabelProjection()
        val scene = NativeWorldSceneSurface(activity, labelProjection)
        val hud = NativeWorldHudView(activity).apply {
            worldLabelProjection = labelProjection
        }
        val touchControls = NativeGameplayControlsOverlay(activity)
        stage.addView(scene, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ))
        // HUD and controls consume only copied world/input state. The scene
        // below keeps camera gestures in empty screen regions.
        stage.addView(hud, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ))
        stage.addView(touchControls, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        ))

        // Input samples are conflated and reduced in native code. A separate
        // bounded gameplay tick may transmit only collision-grounded state after
        // the server's spawn/ACK gate; touch alone never fabricates a position.
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
        var hardwareDrivingMode = false
        val requestNearbyUse: (Boolean) -> Unit = { passengerIntent ->
            activity.lifecycleScope.launch(Dispatchers.IO) {
                // Native rechecks spawn, controllable/interior, range, and
                // throttling; this Android edge owns no pickup/vehicle state.
                // A hold changes only EnterVehicle passenger intent; server
                // RPC 70/71 remains the sole seat authority.
                runCatching { NativeBridge.nativeSessionUseNearby(passengerIntent) }
            }
        }
        val hardwareControls = NativeHardwareControlRouter(
            onControlSample = inputMixer::submitHardware,
            onUseRequested = requestNearbyUse,
            isDriving = { hardwareDrivingMode }
        )
        hardwareControls.onChatRequested = { initialText ->
            // This is only a local focus transition; the chat dialog still
            // submits through the native ordered-session gate.
            hardwareControls.clear()
            NativeChatDialog.show(activity, initialText)
        }
        hardwareControls.onScoreboardRequested = {
            hardwareControls.clear()
            NativeScoreboardDialog.show(activity)
        }
        touchControls.onControlSample = inputMixer::submitTouch
        hud.onTextDrawSelected = { wireId ->
            activity.lifecycleScope.launch(Dispatchers.IO) {
                // Native validates selection mode, current visibility, and the
                // selectable bit again before an RPC-83 packet can be queued.
                runCatching { NativeBridge.nativeSessionSelectTextDraw(wireId) }
            }
        }
        hud.onRespawnRequested = {
            activity.lifecycleScope.launch(Dispatchers.IO) {
                // A HUD tap requests only the existing native handshake. The
                // server still owns class eligibility, allow/deny, PlayerSpawn,
                // transport ACK, health, and all later movement authority.
                runCatching { NativeBridge.nativeSessionRequestSpawn() }
            }
        }
        touchControls.onUseRequested = requestNearbyUse
        // Hardware events may be delivered to the currently focused child.
        // Forward them from every stage layer so hiding touch controls does not
        // make a Bluetooth controller silently stop working.
        val onHardwareMotion: (MotionEvent) -> Boolean = { event ->
            hardwareControls.onGenericMotionEvent(event)
        }
        stage.isFocusable = true
        stage.isFocusableInTouchMode = true
        stage.setOnGenericMotionListener { _, event -> onHardwareMotion(event) }
        scene.setOnGenericMotionListener { _, event -> onHardwareMotion(event) }
        touchControls.setOnGenericMotionListener { _, event -> onHardwareMotion(event) }
        var touchControlsVisible = true
        var gameplayInputEnabled = false

        // Utility actions stay readable on narrow phones instead of being
        // compressed into nine unusable weighted buttons beneath the scene.
        val controls = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
            setPadding(0, (6f * density).toInt(), 0, 0)
        }
        val controlsScroller = HorizontalScrollView(activity).apply {
            isHorizontalScrollBarEnabled = false
            isFillViewport = false
            contentDescription = "Toolbar scene native. Geser mendatar untuk aksi tambahan."
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
                textSize = 10f
                setOnClickListener { onClick() }
            }, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                marginStart = (2f * density).toInt()
                marginEnd = (2f * density).toInt()
            })
        }
        addButton("− ZOOM") { scene.zoomBy(1.35f) }
        addButton("+ ZOOM") { scene.zoomBy(1f / 1.35f) }
        addButton("RESET") { scene.resetCamera() }
        addButton("TOUCH") {
            touchControlsVisible = !touchControlsVisible
            touchControls.setControlsVisible(touchControlsVisible)
            gyroLook.setGameplayEnabled(gameplayInputEnabled && touchControlsVisible)
            hud.setControlsEnabled(touchControlsVisible)
        }
        addButton("LAYOUT") {
            // A modal editor must not inherit a held controller action from
            // the scene beneath it. Saving changes visual coordinates only.
            hardwareControls.clear()
            NativeTouchLayoutEditorDialog.show(activity) {
                touchControls.reloadLayout()
                gyroLook.setUserEnabled(touchControls.gyroLookEnabled())
            }
        }
        addButton("CHAT") {
            hardwareControls.clear()
            NativeChatDialog.show(activity)
        }
        addButton("SCORES") {
            hardwareControls.clear()
            NativeScoreboardDialog.show(activity)
        }
        addButton("OBJ") {
            // RPC 27 selection plus RPC 116/117 edit modes are independent
            // server lifecycles. Read one copied frame only to choose UI;
            // native still owns each mode/ID/kind/revision before packet queue.
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
        addButton("MODEL") {
            activity.lifecycleScope.launch {
                val preview = withContext(Dispatchers.IO) { loadDffPreviewPayload(renderBudget) }
                scene.submitDffPreview(preview.colorVertices)
                scene.submitTexturedDffPreview(preview.textured)
            }
        }

        val status = TextView(activity).apply {
            text = "Memuat scene native…"
            textSize = 11f
            setTextColor(Color.rgb(185, 246, 202))
            setPadding(0, (5f * density).toInt(), 0, (5f * density).toInt())
        }
        root.addView(intro)
        root.addView(stage)
        root.addView(controlsScroller, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ))
        root.addView(status)

        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Dunia native · scene GPU")
            .setView(root)
            .setNegativeButton("Tutup", null)
            .create()
        // Dialog-level keys cover Bluetooth keyboards/controllers regardless of
        // which Canvas/GL child currently owns focus. Unmapped keys (including
        // Back) retain ordinary Android dialog behavior.
        dialog.setOnKeyListener { _, _, event -> hardwareControls.onKeyEvent(event) }
        root.setOnGenericMotionListener { _, event -> onHardwareMotion(event) }
        var renderJob: Job? = null
        var gameplayJob: Job? = null
        var mapRenderJob: Job? = null
        var dynamicModelRenderJob: Job? = null
        // Every job is launched on the lifecycle main dispatcher; only copied
        // immutable frames cross into their IO calls.
        var latestWorldFrame = WorldFrame()
        // Renderer-owned snapshot gate: a PlayerClose/disconnect must halt the
        // separate gameplay ticker as soon as presentation observes it.
        var sessionGameplayActive = false
        val activityForeground = {
            activity.lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.RESUMED)
        }
        // GLSurfaceView does not automatically follow the host Activity when
        // embedded inside an AlertDialog. Own the EGL pause/resume explicitly
        // so a backgrounded native scene cannot retain an active GL thread.
        var sceneSurfaceResumed = false
        fun resumeSceneSurface() {
            if (!sceneSurfaceResumed) {
                scene.onResume()
                sceneSurfaceResumed = true
            }
        }
        fun pauseSceneSurface() {
            if (sceneSurfaceResumed) {
                scene.onPause()
                sceneSurfaceResumed = false
            }
        }
        val sceneLifecycleObserver = object : DefaultLifecycleObserver {
            override fun onResume(owner: LifecycleOwner) {
                if (dialog.isShowing) resumeSceneSurface()
            }

            override fun onPause(owner: LifecycleOwner) {
                pauseSceneSurface()
            }
        }
        dialog.setOnShowListener {
            // This is the in-session play surface, not a diagnostics preview:
            // retain every available dialog pixel for the scene, HUD, and
            // touch layout on both phones and compact landscape devices.
            dialog.window?.setLayout(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            dialog.window?.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            activity.lifecycle.addObserver(sceneLifecycleObserver)
            stage.requestFocus()
            gyroLook.setUserEnabled(touchControls.gyroLookEnabled())
            // Wait for the first native snapshot before accepting a held touch,
            // keyboard, controller, or gyro input. An old/ended session must
            // never receive a creation-time sample from this full-screen UI.
            touchControls.setGameplayEnabled(false, "MENUNGGU SESI NATIVE · INPUT OFF")
            hardwareControls.setEnabled(false)
            gyroLook.setGameplayEnabled(false)
            if (activityForeground()) resumeSceneSurface()
            activity.lifecycleScope.launch {
                val preview = withContext(Dispatchers.IO) { loadDffPreviewPayload(renderBudget) }
                if (dialog.isShowing && activityForeground()) {
                    scene.submitDffPreview(preview.colorVertices)
                    scene.submitTexturedDffPreview(preview.textured)
                }
            }
            gameplayJob = activity.lifecycleScope.launch {
                var previousTickNanos = System.nanoTime()
                var backgroundControlsCleared = false
                while (isActive && dialog.isShowing) {
                    // lifecycleScope remains alive through STOP. Freeze this
                    // visible-session bridge while the activity is hidden so a
                    // held touch/gamepad sample can never keep moving/syncing
                    // in background; its audio controller handles onPause itself.
                    if (!activityForeground()) {
                        if (!backgroundControlsCleared) {
                            touchControls.setGameplayEnabled(false, "APP BACKGROUND · INPUT OFF")
                            touchControls.clearControls()
                            hardwareControls.setEnabled(false)
                            hardwareControls.clear()
                            gyroLook.setGameplayEnabled(false)
                            inputMixer.clear()
                            // The controller owns its own activity onPause/onResume
                            // transition; release belongs only to dialog dismiss.
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
                    if (!sessionGameplayActive) {
                        previousTickNanos = System.nanoTime()
                        delay(100)
                        continue
                    }
                    val nowNanos = System.nanoTime()
                    // Clamp delayed/background frames. Native applies the same
                    // bound before collision stepping, so resume jank cannot
                    // turn into a fake long-distance movement packet.
                    val deltaSeconds = ((nowNanos - previousTickNanos) / 1_000_000_000.0)
                        .toFloat()
                        .coerceIn(0.001f, GAMEPLAY_MAX_STEP_SECONDS)
                    previousTickNanos = nowNanos
                    withContext(Dispatchers.IO) {
                        runCatching { NativeBridge.nativeSessionAdvanceGameplay(deltaSeconds) }
                    }
                    delay(GAMEPLAY_TICK_DELAY_MS)
                }
            }
            mapRenderJob = activity.lifecycleScope.launch {
                while (isActive && dialog.isShowing) {
                    if (!activityForeground()) {
                        delay(100)
                        continue
                    }
                    val frameForMap = latestWorldFrame
                    if (!frameForMap.active) {
                        delay(MAP_RENDER_REFRESH_DELAY_MS)
                        continue
                    }
                    val payload = withContext(Dispatchers.IO) { loadNearbyWorldPayload(frameForMap, renderBudget) }
                    // A terminal RPC/session close may arrive while bounded IMG
                    // work runs off the UI thread. Re-check the current native
                    // presentation gate before submitting so stale static GTA
                    // geometry cannot repopulate after the terminal clear.
                    if (dialog.isShowing && activityForeground() && latestWorldFrame.active) {
                        scene.submitNearbyWorldBatch(payload)
                    }
                    // Native may lazily decode only a bounded DFF subset per
                    // pass. Refreshing here gradually fills nearby placement
                    // geometry without doing filesystem work on the GL thread.
                    delay(MAP_RENDER_REFRESH_DELAY_MS)
                }
            }
            dynamicModelRenderJob = activity.lifecycleScope.launch {
                while (isActive && dialog.isShowing) {
                    if (!activityForeground()) {
                        delay(100)
                        continue
                    }
                    val frameForDynamicModels = latestWorldFrame
                    if (!frameForDynamicModels.active) {
                        delay(DYNAMIC_MODEL_REFRESH_DELAY_MS)
                        continue
                    }
                    val payload = withContext(Dispatchers.IO) {
                        loadDynamicModelPayload(frameForDynamicModels, renderBudget)
                    }
                    // Keep the same post-I/O terminal gate as the static map:
                    // an inactive world is never allowed to re-install stale
                    // server object/vehicle/actor geometry after cleanup.
                    if (dialog.isShowing && activityForeground() && latestWorldFrame.active) {
                        scene.submitDynamicModelBatch(payload)
                    }
                    delay(DYNAMIC_MODEL_REFRESH_DELAY_MS)
                }
            }
            renderJob = activity.lifecycleScope.launch {
                while (isActive && dialog.isShowing) {
                    if (!activityForeground()) {
                        delay(100)
                        continue
                    }
                    val frame = runCatching {
                        val raw = withContext(Dispatchers.IO) {
                            NativeBridge.nativeSessionWorldSnapshot()
                        }
                        WorldFrame.from(JSONObject(raw))
                    }.getOrElse { WorldFrame(error = it.message ?: "snapshot world gagal") }
                    if (!activityForeground()) {
                        delay(100)
                        continue
                    }
                    val wasSessionGameplayActive = sessionGameplayActive
                    sessionGameplayActive = frame.active
                    // Do not leave the last live world/assets/dialog/audio on
                    // screen after PlayerClose or another terminal transport
                    // result. The error remains for an actionable status line.
                    val presentationFrame = if (frame.active) frame else WorldFrame(
                        active = false,
                        error = frame.error
                    )
                    latestWorldFrame = presentationFrame
                    if (wasSessionGameplayActive && !sessionGameplayActive) {
                        inputMixer.clear()
                        // Map/model batches are cached separately from the
                        // immutable entity frame. Explicitly clear each one on
                        // the terminal transition so no old GTA geometry hangs
                        // behind the ended-session status.
                        scene.submitNearbyWorldBatch(null)
                        scene.submitDynamicModelBatch(null)
                        scene.submitDffPreview(FloatArray(0))
                        scene.submitTexturedDffPreview(null)
                    }
                    scene.submit(presentationFrame)
                    hud.submit(presentationFrame)
                    audioStreamController.submit(presentationFrame)
                    hardwareDrivingMode = presentationFrame.active && !presentationFrame.spectate.active &&
                        presentationFrame.localVehicleKnown && presentationFrame.localInVehicle &&
                        presentationFrame.localVehicleSeat == 0
                    val serverMarkedDead = presentationFrame.localHealthKnown &&
                        presentationFrame.localHealth.isFinite() && presentationFrame.localHealth <= 0f &&
                        !presentationFrame.spectate.active
                    val gameplayEnabled = presentationFrame.active &&
                        (!presentationFrame.localControllableKnown || presentationFrame.localControllable) &&
                        !presentationFrame.spectate.active && !serverMarkedDead
                    touchControls.setDrivingMode(hardwareDrivingMode)
                    touchControls.setGameplayEnabled(
                        gameplayEnabled,
                        when {
                            !presentationFrame.active -> "SESI BERAKHIR · INPUT OFF"
                            presentationFrame.spectate.active -> "SPECTATOR · INPUT OFF"
                            serverMarkedDead -> "RESPAWN SERVER · INPUT OFF"
                            else -> "SERVER FREEZE · INPUT OFF"
                        }
                    )
                    hardwareControls.setEnabled(gameplayEnabled)
                    gameplayInputEnabled = gameplayEnabled
                    gyroLook.setGameplayEnabled(gameplayEnabled && touchControlsVisible)
                    if (presentationFrame.active) {
                        NativeServerDialogController.submit(activity, presentationFrame)
                        NativeServerMenuController.submit(activity, presentationFrame)
                    } else {
                        NativeServerDialogController.clear()
                        NativeServerMenuController.clear()
                    }
                    status.text = "${presentationFrame.sceneStatusLine()} · ${renderBudget.label}"
                    // ServerDetailDialog retains and polls UDP. The renderer
                    // receives immutable copies only; it has no socket or sync ownership.
                    delay(100)
                }
            }
        }
        dialog.setOnDismissListener {
            dialog.window?.clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            gameplayJob?.cancel()
            renderJob?.cancel()
            mapRenderJob?.cancel()
            dynamicModelRenderJob?.cancel()
            audioStreamController.release()
            gyroLook.release()
            touchControls.clearControls()
            hardwareControls.clear()
            inputMixer.clear()
            inputSamples.close()
            inputJob.cancel()
            // Clear any held semantic action even if the conflated channel was
            // closed before its final sample reached native code.
            activity.lifecycleScope.launch(Dispatchers.IO) {
                runCatching { NativeBridge.nativeSessionUpdateControls(0f, 0f, 0f, 0f, 0) }
            }
            labelProjection.submit(emptyList())
            activity.lifecycle.removeObserver(sceneLifecycleObserver)
            pauseSceneSurface()
        }
        dialog.show()
    }
}

private fun WorldFrame.sceneStatusLine(): String = when {
    // Terminal session errors remain useful after `active` turns false; render
    // them before the generic idle prompt so PlayerClose is not masked.
    error.isNotBlank() -> "Sesi: $error"
    !active -> "Sesi belum aktif · buka Sesi native dari detail server."
    localHealthKnown && localHealth.isFinite() && localHealth <= 0f && !spectate.active ->
        if (spawnRequestPending) {
            "HP server 0 · request respawn terkirim, menunggu respons authoritative."
        } else {
            "HP server 0 · input/sync lokal dihentikan; gunakan tombol RESPAWN."
        }
    spectate.active -> when {
        spectate.targetKnown && sceneAnchor().followingSpectateTarget && spectate.targetVehicle ->
            "SPECTATOR server · kamera/asset anchor mengikuti vehicle #${spectate.targetId}; sync lokal berhenti."
        spectate.targetKnown && sceneAnchor().followingSpectateTarget ->
            "SPECTATOR server · kamera/asset anchor mengikuti player #${spectate.targetId}; sync lokal berhenti."
        spectate.targetKnown ->
            "SPECTATOR server · target #${spectate.targetId} belum streamed; anchor aman belum diubah, sync lokal berhenti."
        else -> "SPECTATOR server · menunggu target masih streamed; sync lokal berhenti."
    }
    objectEdit.active -> "EDIT OBJEK SERVER #${objectEdit.id} aktif · buka OBJ untuk transform native bounded."
    objectSelectionActive -> "PILIH OBJEK SERVER aktif · buka tombol OBJ untuk daftar objek native yang sedang tersedia."
    !localKnown -> "Sesi aktif · menunggu posisi/spawn lokal dari server."
    camera.serverControlled -> {
        val phase = when {
            camera.interpolatingPosition && camera.interpolatingLookAt -> "interpolasi posisi + target"
            camera.interpolatingPosition -> "interpolasi posisi"
            camera.interpolatingLookAt -> "interpolasi target"
            else -> "posisi/target tetap"
        }
        "KAMERA SERVER aktif · $phase · gerak tetap tunduk pada state server."
    }
    localInteriorKnown && localInterior != 0 ->
        "Interior $localInterior dari server · IPL/COL dengan ID sama dipakai; berhenti aman bila floor interior belum termuat."
    localControllableKnown && !localControllable -> "SERVER FREEZE · touch gameplay dihentikan oleh state server."
    localVehicleKnown && localInVehicle && localVehicleSeat != 0 && localVehicleAnchorKnown ->
        "Passenger kendaraan #$localVehicleId kursi $localVehicleSeat · kamera mengikuti vehicle server · EXIT untuk minta keluar."
    localVehicleKnown && localInVehicle && localVehicleSeat != 0 ->
        "Passenger kendaraan #$localVehicleId kursi $localVehicleSeat · menunggu vehicle stream · EXIT untuk minta keluar."
    localVehicleKnown && localInVehicle && localPredictionActive ->
        "Driver kendaraan #$localVehicleId · steer/gas/rem collision native + Packet-200 rate-gated."
    localVehicleKnown && localInVehicle ->
        "Driver kendaraan #$localVehicleId · menunggu StreamInVehicle dan collision GTA untuk Packet-200."
    else -> {
        val environment = buildString {
            if (worldTimeKnown) append(" · ${worldTime.toString().padStart(2, '0')}:00")
            if (weatherKnown) append(" · WX $weather")
            if (gravityKnown) append(" · G ${String.format(java.util.Locale.US, "%.3f", gravity)}")
        }
        val dynamicVehicles = vehicles.count { it.dynamicSyncKnown }
        val movement = when {
            localPredictionActive && localGrounded -> " · gerak collision grounded"
            localPredictionActive -> " · gerak collision airborne"
            else -> " · menunggu collision GTA"
        }
        "${actors.size}/${streamedPlayers} player · ${serverActors.size}/$trackedServerActors NPC · " +
            "${vehicles.size}/${streamedVehicles} kendaraan ($dynamicVehicles sync) · ${objects.size}/$serverObjects objek · " +
            "${playerAttachedObjects.size} aksesori · ${pickups.size}/$trackedPickups pickup · " +
            "${mapIcons.size}/$trackedMapIcons ikon · " +
            "${textLabels.size}/$trackedTextLabels label · ${gangZones.size}/$trackedGangZones zona · " +
            "${textDraws.size}/$trackedTextDraws TD$environment$movement" +
            " · USE untuk pickup/kendaraan dekat (tahan untuk penumpang) · memuat batch map IPL/DFF sekitar"
    }
}

/**
 * Touch input here changes only this scene camera. It is intentionally separate
 * from NativeControlsDialog's semantic gameplay-intent mapper.
 */
private class NativeWorldSceneSurface(
    context: Context,
    labelProjection: WorldLabelProjection
) : GLSurfaceView(context) {

    private val sceneRenderer = NativeWorldSceneRenderer(labelProjection)
    private var activePointerId = MotionEvent.INVALID_POINTER_ID
    private var lastX = 0f
    private var lastY = 0f
    private var lastSpan = 0f
    private var touchMoved = false
    private var pendingPresentationTicks = 0
    private val presentationTick = object : Runnable {
        override fun run() {
            if (pendingPresentationTicks <= 0) return
            pendingPresentationTicks -= 1
            requestRender()
            if (pendingPresentationTicks > 0) {
                postDelayed(this, REMOTE_PRESENTATION_TICK_MS)
            }
        }
    }

    init {
        setEGLContextClientVersion(2)
        setRenderer(sceneRenderer)
        // World snapshots arrive at 10 Hz and camera changes only during touch.
        // Rendering remains on demand; submit schedules only two short blend
        // frames, avoiding permanent continuous GPU load on ARMv7 hardware.
        renderMode = RENDERMODE_WHEN_DIRTY
        preserveEGLContextOnPause = true
        isClickable = true
        contentDescription = "Scene GPU dunia native SampDroid. Drag untuk mengubah kamera, cubit untuk zoom."
    }

    fun submit(frame: WorldFrame) {
        sceneRenderer.submit(frame)
        requestRender()
        // Do not switch the surface to permanent continuous rendering: a short
        // sequence is enough to blend this new immutable remote snapshot, and
        // it automatically stops on devices that only inspect the scene.
        removeCallbacks(presentationTick)
        pendingPresentationTicks = REMOTE_PRESENTATION_TICKS
        postDelayed(presentationTick, REMOTE_PRESENTATION_TICK_MS)
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(presentationTick)
        pendingPresentationTicks = 0
        super.onDetachedFromWindow()
    }

    fun submitDffPreview(vertices: FloatArray) {
        sceneRenderer.submitDffPreview(vertices)
        requestRender()
    }

    fun submitNearbyWorldBatch(payload: NearbyWorldPayload?) {
        sceneRenderer.submitNearbyWorldBatch(payload)
        requestRender()
    }

    fun submitDynamicModelBatch(payload: DynamicModelPayload?) {
        sceneRenderer.submitDynamicModelBatch(payload)
        requestRender()
    }

    fun submitTexturedDffPreview(preview: NativeTexturedDffPreview?) {
        sceneRenderer.submitTexturedDffPreview(preview)
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
 * GLES 2.0 renderer. Each draw frame reads copied state and an optional copied,
 * triangle-expanded DFF preview, then draws them with Android GPU calls.
 * Network polling and state mutation stay out of here.
 */
private class NativeWorldSceneRenderer(
    private val labelProjection: WorldLabelProjection
) : GLSurfaceView.Renderer {

    private val lock = Any()
    private var worldFrame = WorldFrame()
    // Only prior copied remote transforms cross this presentation boundary.
    // They are never fed to native movement, collision, hit, or networking.
    private var priorRemoteActors: Map<Int, RemoteActorPresentation> = emptyMap()
    private var priorVehicles: Map<Int, VehiclePresentation> = emptyMap()
    private var remoteActorBlendStartedNanos = 0L
    private var userYawDegrees = 0f
    private var userPitchDegrees = 20f
    private var cameraDistance = 28f

    private var program = 0
    private var positionHandle = -1
    private var vertexColorHandle = -1
    private var mvpHandle = -1
    private var colorHandle = -1
    private var texturedProgram = 0
    private var texturedPositionHandle = -1
    private var texturedColorHandle = -1
    private var texturedUvHandle = -1
    private var texturedMvpHandle = -1
    private var texturedSamplerHandle = -1
    private var dffPreviewBuffer: FloatBuffer? = null
    private var dffPreviewVertexCount = 0
    // Placement-aware IPL/DFF batch, already copied and relative to this
    // anchor. A small anchor correction keeps it stable between refreshes.
    private var nearbyWorldBuffer: FloatBuffer? = null
    private var nearbyWorldVertexCount = 0
    // Dynamic server-object + streamed-vehicle + remote-ped DFF batch, copied
    // relative to its own recent local anchor.
    private var nearbyDynamicModelBuffer: FloatBuffer? = null
    private var nearbyDynamicModelVertexCount = 0
    private var nearbyDynamicModelAnchorX = 0f
    private var nearbyDynamicModelAnchorY = 0f
    private var nearbyDynamicModelAnchorZ = 0f
    // Dynamic server objects, vehicles, and actor skins share the same four
    // bounded material slots as map overlays, but keep independent GL texture
    // ownership so a 400 ms entity refresh cannot evict map textures.
    private var nearbyDynamicModelTextured: List<TexturedDffPreviewState> = emptyList()
    private var nextNearbyDynamicModelTextureVersion = 0L
    private val nearbyDynamicModelTextureIds = IntArray(MAP_RENDER_MAX_TEXTURE_BATCHES)
    private val uploadedNearbyDynamicModelTextureVersions =
        LongArray(MAP_RENDER_MAX_TEXTURE_BATCHES) { Long.MIN_VALUE }
    private val uploadedNearbyDynamicModelTextureSources =
        arrayOfNulls<UploadedTextureSource>(MAP_RENDER_MAX_TEXTURE_BATCHES)
    private var nearbyWorldAnchorX = 0f
    private var nearbyWorldAnchorY = 0f
    private var nearbyWorldAnchorZ = 0f
    private var nearbyWorldTextured: List<TexturedDffPreviewState> = emptyList()
    private var nextNearbyWorldTextureVersion = 0L
    private val nearbyWorldTextureIds = IntArray(MAP_RENDER_MAX_TEXTURE_BATCHES)
    private val uploadedNearbyWorldTextureVersions =
        LongArray(MAP_RENDER_MAX_TEXTURE_BATCHES) { Long.MIN_VALUE }
    private val uploadedNearbyWorldTextureSources =
        arrayOfNulls<UploadedTextureSource>(MAP_RENDER_MAX_TEXTURE_BATCHES)
    private var texturedDffPreview: TexturedDffPreviewState? = null
    private var nextTexturedDffPreviewVersion = 0L
    private var uploadedTexturedDffPreviewVersion = Long.MIN_VALUE
    private var texturedPreviewTextureId = 0
    private lateinit var gridBuffer: FloatBuffer
    private lateinit var axisBuffer: FloatBuffer
    private lateinit var cubeBuffer: FloatBuffer
    private var gridVertexCount = 0
    private var axisVertexCount = 0
    private val projection = FloatArray(16)
    private val view = FloatArray(16)
    private var viewportWidth = 0
    private var viewportHeight = 0
    private val viewProjection = FloatArray(16)
    private val model = FloatArray(16)
    private val modelViewProjection = FloatArray(16)

    fun submit(frame: WorldFrame) = synchronized(lock) {
        // Keep one immutable predecessor per actor ID. A packet can introduce,
        // stream out, teleport, or seat a player; the draw path handles those
        // cases by snapping rather than inventing an intermediate world fact.
        priorRemoteActors = worldFrame.actors.associate { actor ->
            actor.id to RemoteActorPresentation(
                x = actor.x,
                y = actor.y,
                z = actor.z,
                angle = actor.angle,
                onFootKnown = actor.onFootKnown,
                inVehicle = actor.inVehicle
            )
        }
        priorVehicles = worldFrame.vehicles.associate { vehicle ->
            vehicle.id to VehiclePresentation(
                x = vehicle.x,
                y = vehicle.y,
                z = vehicle.z,
                angle = vehicle.angle,
                dynamicSyncKnown = vehicle.dynamicSyncKnown
            )
        }
        worldFrame = frame
        remoteActorBlendStartedNanos = System.nanoTime()
    }

    fun submitDffPreview(vertices: FloatArray) = synchronized(lock) {
        if (vertices.isEmpty() ||
            vertices.size > DFF_PREVIEW_MAX_VERTICES * DFF_PREVIEW_FLOATS_PER_VERTEX ||
            vertices.size % DFF_PREVIEW_FLOATS_PER_VERTEX != 0
        ) {
            dffPreviewBuffer = null
            dffPreviewVertexCount = 0
        } else {
            // The JNI array is copied into a direct buffer before any GL draw;
            // native catalog memory is never retained by the renderer.
            dffPreviewBuffer = floatBuffer(vertices)
            dffPreviewVertexCount = vertices.size / DFF_PREVIEW_FLOATS_PER_VERTEX
        }
    }

    fun submitNearbyWorldBatch(payload: NearbyWorldPayload?) = synchronized(lock) {
        val safePayload = payload
        if (safePayload == null) {
            nearbyWorldBuffer = null
            nearbyWorldVertexCount = 0
            nearbyWorldTextured = emptyList()
            return@synchronized
        }
        val anchorValid = safePayload.anchorX.isFinite() && safePayload.anchorY.isFinite() &&
            safePayload.anchorZ.isFinite()
        if (!anchorValid) {
            nearbyWorldBuffer = null
            nearbyWorldVertexCount = 0
            nearbyWorldTextured = emptyList()
            return@synchronized
        }
        nearbyWorldAnchorX = safePayload.anchorX
        nearbyWorldAnchorY = safePayload.anchorY
        nearbyWorldAnchorZ = safePayload.anchorZ

        val vertices = safePayload.vertices
        val colorValid = vertices.size <= MAP_RENDER_MAX_VERTICES * DFF_PREVIEW_FLOATS_PER_VERTEX &&
            vertices.size % DFF_PREVIEW_FLOATS_PER_VERTEX == 0
        nearbyWorldBuffer = if (colorValid && vertices.isNotEmpty()) floatBuffer(vertices) else null
        nearbyWorldVertexCount = if (colorValid) vertices.size / DFF_PREVIEW_FLOATS_PER_VERTEX else 0

        nextNearbyWorldTextureVersion += 1L
        nearbyWorldTextured = copyTexturedPreviewStates(
            safePayload.textured,
            MAP_RENDER_MAX_VERTICES,
            nextNearbyWorldTextureVersion
        )
    }

    fun submitDynamicModelBatch(payload: DynamicModelPayload?) = synchronized(lock) {
        val safePayload = payload
        if (safePayload == null || !safePayload.anchorX.isFinite() ||
            !safePayload.anchorY.isFinite() || !safePayload.anchorZ.isFinite()
        ) {
            nearbyDynamicModelBuffer = null
            nearbyDynamicModelVertexCount = 0
            nearbyDynamicModelTextured = emptyList()
            return@synchronized
        }
        val vertices = safePayload.vertices
        val valid = vertices.size <= MAP_RENDER_MAX_DYNAMIC_MODEL_VERTICES * DFF_PREVIEW_FLOATS_PER_VERTEX &&
            vertices.size % DFF_PREVIEW_FLOATS_PER_VERTEX == 0
        nearbyDynamicModelBuffer = if (valid && vertices.isNotEmpty()) floatBuffer(vertices) else null
        nearbyDynamicModelVertexCount = if (valid) vertices.size / DFF_PREVIEW_FLOATS_PER_VERTEX else 0
        nearbyDynamicModelAnchorX = safePayload.anchorX
        nearbyDynamicModelAnchorY = safePayload.anchorY
        nearbyDynamicModelAnchorZ = safePayload.anchorZ
        nextNearbyDynamicModelTextureVersion += 1L
        nearbyDynamicModelTextured = copyTexturedPreviewStates(
            safePayload.textured,
            MAP_RENDER_MAX_DYNAMIC_MODEL_VERTICES,
            nextNearbyDynamicModelTextureVersion
        )
    }

    /**
     * Turns JNI-owned arrays into direct renderer buffers after independently
     * validating every material. The whole collection shares one vertex and
     * decoded-pixel cap; malformed optional material data is skipped rather
     * than clearing a valid color batch.
     */
    private fun copyTexturedPreviewStates(
        previews: List<NativeTexturedDffPreview>,
        maxVertices: Int,
        generation: Long
    ): List<TexturedDffPreviewState> {
        var totalVertices = 0
        var totalPixels = 0L
        val accepted = ArrayList<TexturedDffPreviewState>(MAP_RENDER_MAX_TEXTURE_BATCHES)
        previews.take(MAP_RENDER_MAX_TEXTURE_BATCHES).forEachIndexed { index, textured ->
            val width = textured.textureWidth.toLong()
            val height = textured.textureHeight.toLong()
            if (width <= 0L || height <= 0L ||
                width > DFF_PREVIEW_MAX_TEXTURE_PIXELS.toLong() / height
            ) return@forEachIndexed
            val pixels = width * height
            val bytes = pixels * 4L
            val vertexCount = textured.vertices.size / DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX
            val textureValid = textured.vertices.isNotEmpty() &&
                textured.vertices.size % DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX == 0 &&
                bytes == textured.textureRgba.size.toLong() &&
                vertexCount <= maxVertices - totalVertices &&
                pixels <= MAP_RENDER_MAX_TEXTURE_TOTAL_PIXELS.toLong() - totalPixels
            if (!textureValid) return@forEachIndexed
            totalVertices += vertexCount
            totalPixels += pixels
            accepted += TexturedDffPreviewState(
                generation * MAP_RENDER_MAX_TEXTURE_BATCHES + index,
                floatBuffer(textured.vertices),
                byteBuffer(textured.textureRgba),
                textured.textureWidth,
                textured.textureHeight,
                vertexCount
            )
        }
        return accepted.toList()
    }

    fun submitTexturedDffPreview(preview: NativeTexturedDffPreview?) {
        synchronized(lock) {
            nextTexturedDffPreviewVersion += 1L
            if (preview == null) {
                texturedDffPreview = null
            } else {
                val expectedPixels = preview.textureWidth.toLong() * preview.textureHeight.toLong()
                val expectedBytes = expectedPixels * 4L
                val valid = preview.vertices.isNotEmpty() &&
                    preview.vertices.size <= DFF_PREVIEW_MAX_VERTICES * DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX &&
                    preview.vertices.size % DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX == 0 &&
                    preview.textureWidth > 0 && preview.textureHeight > 0 &&
                    expectedPixels in 1L..DFF_PREVIEW_MAX_TEXTURE_PIXELS.toLong() &&
                    expectedBytes == preview.textureRgba.size.toLong()
                texturedDffPreview = if (valid) {
                    TexturedDffPreviewState(
                        nextTexturedDffPreviewVersion,
                        floatBuffer(preview.vertices),
                        byteBuffer(preview.textureRgba),
                        preview.textureWidth,
                        preview.textureHeight,
                        preview.vertices.size / DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX
                    )
                } else {
                    null
                }
            }
        }
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
        vertexColorHandle = GLES20.glGetAttribLocation(program, "aVertexColor")
        mvpHandle = GLES20.glGetUniformLocation(program, "uMvp")
        colorHandle = GLES20.glGetUniformLocation(program, "uColor")
        texturedProgram = buildProgram(TEXTURED_VERTEX_SHADER, TEXTURED_FRAGMENT_SHADER)
        texturedPositionHandle = GLES20.glGetAttribLocation(texturedProgram, "aPosition")
        texturedColorHandle = GLES20.glGetAttribLocation(texturedProgram, "aVertexColor")
        texturedUvHandle = GLES20.glGetAttribLocation(texturedProgram, "aTexCoord")
        texturedMvpHandle = GLES20.glGetUniformLocation(texturedProgram, "uMvp")
        texturedSamplerHandle = GLES20.glGetUniformLocation(texturedProgram, "uTexture")
        texturedPreviewTextureId = 0
        uploadedTexturedDffPreviewVersion = Long.MIN_VALUE
        nearbyWorldTextureIds.fill(0)
        uploadedNearbyWorldTextureVersions.fill(Long.MIN_VALUE)
        uploadedNearbyWorldTextureSources.fill(null)
        nearbyDynamicModelTextureIds.fill(0)
        uploadedNearbyDynamicModelTextureVersions.fill(Long.MIN_VALUE)
        uploadedNearbyDynamicModelTextureSources.fill(null)
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
        viewportWidth = width.coerceAtLeast(0)
        viewportHeight = height.coerceAtLeast(0)
        GLES20.glViewport(0, 0, width, height)
        val aspect = if (height > 0) width.toFloat() / height.toFloat() else 1f
        Matrix.perspectiveM(projection, 0, 61f, aspect, 0.45f, 1200f)
    }

    override fun onDrawFrame(gl: javax.microedition.khronos.opengles.GL10?) {
        val snapshot: SceneCameraState = synchronized(lock) {
            SceneCameraState(
                worldFrame,
                priorRemoteActors,
                priorVehicles,
                remoteActorBlendStartedNanos,
                userYawDegrees,
                userPitchDegrees,
                cameraDistance,
                dffPreviewBuffer,
                dffPreviewVertexCount,
                nearbyWorldBuffer,
                nearbyWorldVertexCount,
                nearbyDynamicModelBuffer,
                nearbyDynamicModelVertexCount,
                nearbyDynamicModelAnchorX,
                nearbyDynamicModelAnchorY,
                nearbyDynamicModelAnchorZ,
                nearbyDynamicModelTextured,
                nearbyWorldAnchorX,
                nearbyWorldAnchorY,
                nearbyWorldAnchorZ,
                nearbyWorldTextured,
                texturedDffPreview
            )
        }
        val sky = environmentClearColor(snapshot.frame)
        GLES20.glClearColor(sky[0], sky[1], sky[2], 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT or GLES20.GL_DEPTH_BUFFER_BIT)
        if (program == 0 || positionHandle < 0 || vertexColorHandle < 0 || mvpHandle < 0 || colorHandle < 0) return

        val sceneAnchor = snapshot.frame.sceneAnchor()
        val anchorX = if (sceneAnchor.known) sceneAnchor.x else 0f
        val anchorY = if (sceneAnchor.known) sceneAnchor.y else 0f
        val anchorZ = if (sceneAnchor.known) sceneAnchor.z else 0f
        val heading = if (sceneAnchor.known) sceneAnchor.angle else 0f
        val yawRadians = Math.toRadians((heading + snapshot.userYawDegrees).toDouble())
        val pitchRadians = Math.toRadians(snapshot.userPitchDegrees.toDouble())
        val horizontalDistance = snapshot.cameraDistance * cos(pitchRadians).toFloat()
        val fallbackEyeX = -sin(yawRadians).toFloat() * horizontalDistance
        val fallbackEyeY = 1.8f + sin(pitchRadians).toFloat() * snapshot.cameraDistance
        val fallbackEyeZ = cos(yawRadians).toFloat() * horizontalDistance
        val fallbackTargetX = sin(yawRadians).toFloat() * 6f
        val fallbackTargetY = 1.1f
        val fallbackTargetZ = -cos(yawRadians).toFloat() * 6f

        // Server SetPlayerCameraPosition/LookAt and InterpolateCamera take
        // visual precedence over a user's free scene camera. Endpoint values
        // remain in GTA x/y/z until this exact scene-space conversion:
        // GTA (x,y,z) -> GLES (x,z,-y), relative to the local scene anchor.
        // Missing one endpoint deliberately falls back to the normal native
        // camera rather than inventing a server target/position.
        val serverCamera = snapshot.frame.camera
        // When the native session has actually accepted an on-foot Packet-203
        // candidate, present its same yaw/pitch camera as a local first-person
        // aim view. This is render-only: it does not add a target, raycast, or
        // BulletSync. Any server camera endpoint still has absolute priority.
        val aimCameraActive = !snapshot.frame.spectate.active && snapshot.frame.localAimHeld &&
            snapshot.frame.localAimSyncReady && !serverCamera.serverControlled && !snapshot.frame.localInVehicle
        var eyeX = fallbackEyeX
        var eyeY = fallbackEyeY
        var eyeZ = fallbackEyeZ
        var targetX = fallbackTargetX
        var targetY = fallbackTargetY
        var targetZ = fallbackTargetZ
        if (aimCameraActive) {
            val aimHeadingRadians = Math.toRadians(heading.toDouble())
            val aimPitchRadians = Math.toRadians(snapshot.frame.localAimPitchDegrees.toDouble())
            val horizontalFront = cos(aimPitchRadians).toFloat()
            val aimDistance = 32f
            eyeX = 0f
            eyeY = 0.70f
            eyeZ = 0f
            targetX = sin(aimHeadingRadians).toFloat() * horizontalFront * aimDistance
            targetY = eyeY + sin(aimPitchRadians).toFloat() * aimDistance
            targetZ = -cos(aimHeadingRadians).toFloat() * horizontalFront * aimDistance
        }
        if (serverCamera.positionKnown && serverCamera.positionX.isFinite() &&
            serverCamera.positionY.isFinite() && serverCamera.positionZ.isFinite()
        ) {
            eyeX = serverCamera.positionX - anchorX
            eyeY = serverCamera.positionZ - anchorZ
            eyeZ = -(serverCamera.positionY - anchorY)
        }
        if (serverCamera.lookAtKnown && serverCamera.lookAtX.isFinite() &&
            serverCamera.lookAtY.isFinite() && serverCamera.lookAtZ.isFinite()
        ) {
            targetX = serverCamera.lookAtX - anchorX
            targetY = serverCamera.lookAtZ - anchorZ
            targetZ = -(serverCamera.lookAtY - anchorY)
        }
        val cameraDx = targetX - eyeX
        val cameraDy = targetY - eyeY
        val cameraDz = targetZ - eyeZ
        if (cameraDx * cameraDx + cameraDy * cameraDy + cameraDz * cameraDz < 0.0001f) {
            // A malformed/degenerate server camera pair should not make the GL
            // look-at matrix singular. Preserve its valid eye if present while
            // taking a deterministic native forward target.
            targetX = fallbackTargetX
            targetY = fallbackTargetY
            targetZ = fallbackTargetZ
        }
        Matrix.setLookAtM(view, 0, eyeX, eyeY, eyeZ, targetX, targetY, targetZ, 0f, 1f, 0f)
        Matrix.multiplyMM(viewProjection, 0, projection, 0, view, 0)
        publishWorldTextLabels(snapshot.frame)

        draw(gridBuffer, gridVertexCount, GLES20.GL_LINES, viewProjection, floatArrayOf(0.08f, 0.29f, 0.35f, 0.76f), 1f)
        draw(axisBuffer, axisVertexCount, GLES20.GL_LINES, viewProjection, floatArrayOf(0.16f, 0.63f, 0.74f, 0.92f), 2f)
        snapshot.nearbyWorldBuffer?.let { buffer ->
            if (snapshot.nearbyWorldVertexCount > 0) {
                // Native vertices are relative to the anchor at copy time;
                // correct them against the latest local state without reloading
                // an IMG asset for every movement tick.
                Matrix.setIdentityM(model, 0)
                Matrix.translateM(
                    model, 0,
                    snapshot.nearbyWorldAnchorX - anchorX,
                    snapshot.nearbyWorldAnchorZ - anchorZ,
                    -(snapshot.nearbyWorldAnchorY - anchorY)
                )
                Matrix.multiplyMM(modelViewProjection, 0, viewProjection, 0, model, 0)
                drawDffPreview(buffer, snapshot.nearbyWorldVertexCount, modelViewProjection)
            }
        }
        snapshot.nearbyDynamicModelBuffer?.let { buffer ->
            if (snapshot.nearbyDynamicModelVertexCount > 0) {
                Matrix.setIdentityM(model, 0)
                Matrix.translateM(
                    model, 0,
                    snapshot.nearbyDynamicModelAnchorX - anchorX,
                    snapshot.nearbyDynamicModelAnchorZ - anchorZ,
                    -(snapshot.nearbyDynamicModelAnchorY - anchorY)
                )
                Matrix.multiplyMM(modelViewProjection, 0, viewProjection, 0, model, 0)
                drawDffPreview(buffer, snapshot.nearbyDynamicModelVertexCount, modelViewProjection)
            }
        }
        if (snapshot.nearbyDynamicModelTextured.isNotEmpty()) {
            Matrix.setIdentityM(model, 0)
            Matrix.translateM(
                model, 0,
                snapshot.nearbyDynamicModelAnchorX - anchorX,
                snapshot.nearbyDynamicModelAnchorZ - anchorZ,
                -(snapshot.nearbyDynamicModelAnchorY - anchorY)
            )
            Matrix.multiplyMM(modelViewProjection, 0, viewProjection, 0, model, 0)
            snapshot.nearbyDynamicModelTextured.forEachIndexed { slot, preview ->
                if (preview.vertexCount > 0 && ensureNearbyDynamicModelTexture(slot, preview)) {
                    drawNearbyWorldTextured(preview, nearbyDynamicModelTextureIds[slot], modelViewProjection)
                }
            }
        }
        clearUnusedNearbyDynamicModelTextureSlots(snapshot.nearbyDynamicModelTextured.size)
        if (snapshot.nearbyWorldTextured.isNotEmpty()) {
            Matrix.setIdentityM(model, 0)
            Matrix.translateM(
                model, 0,
                snapshot.nearbyWorldAnchorX - anchorX,
                snapshot.nearbyWorldAnchorZ - anchorZ,
                -(snapshot.nearbyWorldAnchorY - anchorY)
            )
            Matrix.multiplyMM(modelViewProjection, 0, viewProjection, 0, model, 0)
            snapshot.nearbyWorldTextured.forEachIndexed { slot, preview ->
                if (preview.vertexCount > 0 && ensureNearbyWorldTexture(slot, preview)) {
                    drawNearbyWorldTextured(preview, nearbyWorldTextureIds[slot], modelViewProjection)
                }
            }
        }
        clearUnusedNearbyWorldTextureSlots(snapshot.nearbyWorldTextured.size)
        snapshot.dffPreviewBuffer?.let { buffer ->
            if (snapshot.dffPreviewVertexCount > 0) {
                drawDffPreview(buffer, snapshot.dffPreviewVertexCount, viewProjection)
            }
        }
        snapshot.texturedDffPreview?.let { preview ->
            if (preview.vertexCount > 0 && ensureTexturedPreviewTexture(preview)) {
                drawTexturedDffPreview(preview, viewProjection)
            }
        }

        if (!snapshot.frame.spectate.active && snapshot.frame.localKnown &&
            !snapshot.frame.localInVehicle && !aimCameraActive) {
            // The local actor remains the camera anchor. Its copied world
            // position may now be collision-predicted between authoritative
            // corrections, shifting remote entities/markers around the anchor.
            drawActor(0f, 0f, 0f, heading, floatArrayOf(0.28f, 0.88f, 1f, 1f), true)
            drawForcedAnimationCue(0f, 0f, 0f, snapshot.frame.localForcedAnimation)
        } else if (!snapshot.frame.spectate.active && !snapshot.frame.localKnown) {
            drawActor(0f, 0f, 0f, 0f, floatArrayOf(0.65f, 0.71f, 0.74f, 0.72f), true)
        }
        // A seated player deliberately does not draw a free-standing local ped
        // at a guessed seat offset. The vehicle proxy/DFF is the visual anchor.

        // Every copied entity is rendered relative to the resolved spectator
        // target when available, otherwise the authoritative local position.
        val localX = anchorX
        val localY = anchorY
        val localZ = anchorZ
        val remoteActorBlend = remoteActorBlendFactor(snapshot.remoteActorBlendStartedNanos)
        snapshot.frame.actors.forEach { actor ->
            if (actor.remoteShot.active) {
                // This is a very short, server-relayed Packet-206 direction
                // cue. It deliberately has no rendered endpoint/impact, hit
                // marker, target, damage, or local ammo implication.
                drawRemoteShotDirection(
                    actor.remoteShot.originX - localX,
                    actor.remoteShot.originZ - localZ,
                    -(actor.remoteShot.originY - localY),
                    actor.remoteShot.directionX,
                    actor.remoteShot.directionZ,
                    -actor.remoteShot.directionY
                )
            }
            // Packet-200 driver / Packet-211 passenger occupancy is exact.
            // Leave ped proxy/DFF out rather than drawing it through a vehicle
            // at an invented seat offset; nametag/bubble keep packet anchors.
            if (actor.inVehicle) return@forEach
            val presentation = interpolatedRemoteActor(
                actor,
                snapshot.priorRemoteActors[actor.id],
                remoteActorBlend
            )
            val x = presentation.x - localX
            val y = presentation.z - localZ
            val z = -(presentation.y - localY)
            if (x * x + z * z <= MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE) {
                // RPC-166 is a short observer cue, not an inferred ragdoll or
                // local death. Draw an unmistakable red proxy while the normal
                // DFF batch is intentionally suppressed above.
                val base = if (actor.wasted) {
                    floatArrayOf(0.92f, 0.16f, 0.16f, 0.88f)
                } else {
                    rgba(actor.rgba, if (actor.onFootKnown) 1f else 0.58f)
                }
                drawActor(x, y, z, presentation.angle, base, false)
                if (!actor.wasted) {
                    drawForcedAnimationCue(x, y, z, actor.forcedAnimation)
                }
                if (actor.remoteAim.active && !actor.wasted) {
                    // Uses the exact server-broadcast Packet-203 camera pose;
                    // the short segment is a camera-direction affordance, not
                    // a bullet trace, target, raycast, or hit confirmation.
                    drawRemoteAimDirection(
                        actor.remoteAim.positionX - localX,
                        actor.remoteAim.positionZ - localZ,
                        -(actor.remoteAim.positionY - localY),
                        actor.remoteAim.frontX,
                        actor.remoteAim.frontZ,
                        -actor.remoteAim.frontY
                    )
                }
            }
        }
        snapshot.frame.serverActors.forEach { actor ->
            val x = actor.x - localX
            val y = actor.z - localZ
            val z = -(actor.y - localY)
            if (x * x + z * z <= MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE) {
                val color = if (actor.invulnerable) {
                    floatArrayOf(1f, 0.80f, 0.25f, 1f)
                } else {
                    floatArrayOf(1f, 0.40f, 0.20f, 1f)
                }
                drawActor(x, y, z, actor.angle, color, false)
                drawForcedAnimationCue(x, y, z, actor.forcedAnimation)
            }
        }
        snapshot.frame.vehicles.forEach { vehicle ->
            val presentation = interpolatedVehicle(
                vehicle,
                snapshot.priorVehicles[vehicle.id],
                remoteActorBlend
            )
            val x = presentation.x - localX
            val y = presentation.z - localZ
            val z = -(presentation.y - localY)
            if (x * x + z * z <= MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE) {
                drawVehicleProxy(
                    x, y, z, presentation.angle, vehicleColour(vehicle.colour1, vehicle.colour2),
                    vehicle.dynamicSyncKnown
                )
                if (vehicle.parametersKnown && vehicle.objective == 1) {
                    // Server's per-recipient vehicle objective normally uses a
                    // GTA arrow. This portable yellow marker keeps it visible
                    // without claiming an asset-faithful arrow model.
                    drawMarker(x, y + 4.0f, z, 1.25f, floatArrayOf(1f, 0.84f, 0.18f, 0.94f))
                }
            }
        }
        snapshot.frame.pickups.forEach { pickup ->
            val x = pickup.x - localX
            val y = pickup.z - localZ
            val z = -(pickup.y - localY)
            if (x * x + z * z <= MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE) {
                drawPickupProxy(x, y, z, pickup)
            }
        }
        // The model batch can resolve only a bounded subset and does not return
        // per-record success. Keep a small authoritative transform marker for
        // every object (especially moving ones) rather than accidentally
        // hiding an unsupported server entity when another model did resolve.
        snapshot.frame.objects.forEach { objectValue ->
            val x = objectValue.x - localX
            val y = objectValue.z - localZ
            val z = -(objectValue.y - localY)
            val requestedDistance = if (objectValue.drawDistance > 0f) objectValue.drawDistance else 300f
            val visibleDistance = requestedDistance.coerceIn(8f, MAX_RENDER_DISTANCE)
            if (x * x + y * y + z * z <= visibleDistance * visibleDistance) {
                drawServerObjectProxy(x, y, z, objectValue)
            }
        }
        // A loaded DFF has the primary visual path above. This small accessory
        // proxy stays visible when an optional model/IDE/TXD batch cannot be
        // resolved, matching the existing server-object authority-marker rule.
        snapshot.frame.playerAttachedObjects.forEach { attachment ->
            val x = attachment.x - localX
            val y = attachment.z - localZ
            val z = -(attachment.y - localY)
            if (x * x + y * y + z * z <= MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE) {
                drawPlayerAttachedObjectProxy(x, y, z, attachment)
            }
        }
        // SetPlayerMapIcon has no portable GTA sprite atlas here. Its exact server
        // coordinate/colour still gets a bounded world beacon, while the radar
        // shows the corresponding 2D diamond rather than claiming a sprite match.
        snapshot.frame.mapIcons.forEach { icon ->
            val x = icon.x - localX
            val y = icon.z - localZ
            val z = -(icon.y - localY)
            if (x * x + z * z <= MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE) {
                val radius = if (icon.style == 2 || icon.style == 3) 2.5f else 1.35f
                drawMarker(x, y, z, radius, rgba(icon.rgba, 0.90f))
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

    /**
     * Projects decoded 3D-label anchors, expiry-filtered chat bubbles, and
     * policy-filtered player nametags into the Canvas HUD coordinate space.
     * They remain presentation copies; no GLES/UI path owns a player pointer
     * or evaluates collision LOS.
     */
    private fun publishWorldTextLabels(frame: WorldFrame) {
        val anchor = frame.sceneAnchor()
        if (viewportWidth <= 0 || viewportHeight <= 0 || !anchor.known) {
            labelProjection.submit(emptyList())
            return
        }
        val projected = ArrayList<ProjectedWorldTextLabel>(24)
        fun projectAnchor(
            wireId: Int,
            worldX: Float,
            worldY: Float,
            worldZ: Float,
            rgba: Long,
            text: String,
            distance: Float,
            height: Float
        ) {
            if (projected.size >= 24) return
            val sceneX = worldX - anchor.x
            val sceneY = worldZ - anchor.z + height
            val sceneZ = -(worldY - anchor.y)
            val clip = FloatArray(4)
            Matrix.multiplyMV(clip, 0, viewProjection, 0, floatArrayOf(sceneX, sceneY, sceneZ, 1f), 0)
            val w = clip[3]
            if (!w.isFinite() || w <= 0.001f) return
            val ndcX = clip[0] / w
            val ndcY = clip[1] / w
            val ndcZ = clip[2] / w
            // Give a small edge margin to text just entering the visible
            // frustum; HUD clipping protects Canvas from farther values.
            if (!ndcX.isFinite() || !ndcY.isFinite() || !ndcZ.isFinite() ||
                ndcX < -1.12f || ndcX > 1.12f || ndcY < -1.12f || ndcY > 1.12f ||
                ndcZ < -1f || ndcZ > 1f
            ) return
            projected += ProjectedWorldTextLabel(
                wireId = wireId,
                x = ((ndcX + 1f) * 0.5f * viewportWidth).coerceIn(-viewportWidth * 0.2f, viewportWidth * 1.2f),
                y = ((1f - ndcY) * 0.5f * viewportHeight).coerceIn(-viewportHeight * 0.2f, viewportHeight * 1.2f),
                rgba = rgba,
                text = text,
                distance = distance
            )
        }
        // Bubbles are short-lived social feedback, so let them win the fixed
        // overlay budget before persistent labels/names consume it.
        frame.resolvedChatBubbles().forEach { bubble ->
            projectAnchor(
                wireId = bubble.wireId,
                worldX = bubble.x,
                worldY = bubble.y,
                worldZ = bubble.z,
                rgba = bubble.bubble.rgba,
                text = bubble.bubble.text,
                distance = bubble.distance,
                height = 2.45f
            )
        }
        frame.resolvedTextLabels().forEach { label ->
            projectAnchor(
                wireId = label.source.wireId,
                worldX = label.x,
                worldY = label.y,
                worldZ = label.z,
                rgba = label.source.rgba,
                text = label.source.text,
                distance = label.distance,
                height = 1.25f
            )
        }
        frame.resolvedNameTags((24 - projected.size).coerceAtLeast(0)).forEach { tag ->
            val actor = tag.actor
            projectAnchor(
                wireId = 0x1_0000 + actor.id,
                worldX = actor.x,
                worldY = actor.y,
                worldZ = actor.z,
                rgba = actor.rgba,
                text = actor.name.ifBlank { "#${actor.id}" },
                distance = tag.distance,
                height = 1.85f
            )
        }
        labelProjection.submit(projected)
    }

    private fun remoteActorBlendFactor(startedNanos: Long): Float {
        if (startedNanos <= 0L) return 1f
        val elapsed = (System.nanoTime() - startedNanos).coerceAtLeast(0L)
        return (elapsed.toDouble() / REMOTE_ACTOR_BLEND_NANOS.toDouble())
            .toFloat()
            .coerceIn(0f, 1f)
    }

    /**
     * Renderer-only interpolation between two immutable remote snapshots. It
     * never extrapolates Packet-207 velocity, changes the native snapshot, or
     * bridges a teleport/vehicle transition. A short blend makes the marker
     * readable while preserving source-authoritative current state for every
     * interaction and protocol path.
     */
    private fun interpolatedRemoteActor(
        actor: WorldActor,
        prior: RemoteActorPresentation?,
        blend: Float
    ): RemoteActorPresentation {
        val current = RemoteActorPresentation(
            x = actor.x,
            y = actor.y,
            z = actor.z,
            angle = actor.angle,
            onFootKnown = actor.onFootKnown,
            inVehicle = actor.inVehicle
        )
        if (prior == null || !current.onFootKnown || !prior.onFootKnown ||
            current.inVehicle || prior.inVehicle || blend >= 1f ||
            !current.x.isFinite() || !current.y.isFinite() || !current.z.isFinite() ||
            !current.angle.isFinite() || !prior.x.isFinite() || !prior.y.isFinite() ||
            !prior.z.isFinite() || !prior.angle.isFinite()
        ) {
            return current
        }
        val dx = current.x - prior.x
        val dy = current.y - prior.y
        val dz = current.z - prior.z
        val distanceSquared = dx * dx + dy * dy + dz * dz
        if (!distanceSquared.isFinite() ||
            distanceSquared > REMOTE_ACTOR_SNAP_DISTANCE * REMOTE_ACTOR_SNAP_DISTANCE
        ) {
            return current
        }
        var angleDelta = (current.angle - prior.angle) % 360f
        if (angleDelta > 180f) angleDelta -= 360f
        if (angleDelta < -180f) angleDelta += 360f
        return RemoteActorPresentation(
            x = prior.x + dx * blend,
            y = prior.y + dy * blend,
            z = prior.z + dz * blend,
            angle = prior.angle + angleDelta * blend,
            onFootKnown = true,
            inVehicle = false
        )
    }

    /** Same bounded presentation blend for dynamic streamed vehicle snapshots. */
    private fun interpolatedVehicle(
        vehicle: WorldVehicle,
        prior: VehiclePresentation?,
        blend: Float
    ): VehiclePresentation {
        val current = VehiclePresentation(
            x = vehicle.x,
            y = vehicle.y,
            z = vehicle.z,
            angle = vehicle.angle,
            dynamicSyncKnown = vehicle.dynamicSyncKnown
        )
        if (prior == null || !current.dynamicSyncKnown || !prior.dynamicSyncKnown ||
            blend >= 1f || !current.x.isFinite() || !current.y.isFinite() ||
            !current.z.isFinite() || !current.angle.isFinite() ||
            !prior.x.isFinite() || !prior.y.isFinite() || !prior.z.isFinite() ||
            !prior.angle.isFinite()
        ) {
            return current
        }
        val dx = current.x - prior.x
        val dy = current.y - prior.y
        val dz = current.z - prior.z
        val distanceSquared = dx * dx + dy * dy + dz * dz
        if (!distanceSquared.isFinite() ||
            distanceSquared > REMOTE_ACTOR_SNAP_DISTANCE * REMOTE_ACTOR_SNAP_DISTANCE
        ) {
            return current
        }
        var angleDelta = (current.angle - prior.angle) % 360f
        if (angleDelta > 180f) angleDelta -= 360f
        if (angleDelta < -180f) angleDelta += 360f
        return VehiclePresentation(
            x = prior.x + dx * blend,
            y = prior.y + dy * blend,
            z = prior.z + dz * blend,
            angle = prior.angle + angleDelta * blend,
            dynamicSyncKnown = true
        )
    }

    private fun drawActor(x: Float, y: Float, z: Float, angleDegrees: Float, color: FloatArray, local: Boolean) {
        Matrix.setIdentityM(model, 0)
        Matrix.translateM(model, 0, x, y, z)
        Matrix.rotateM(model, 0, -angleDegrees, 0f, 1f, 0f)
        val scale = if (local) 1.15f else 0.95f
        Matrix.scaleM(model, 0, scale, scale, scale)
        Matrix.multiplyMM(modelViewProjection, 0, viewProjection, 0, model, 0)
        // A remote actor may now have a real peds DFF in the dynamic batch.
        // Keep this cube as a translucent authority marker without writing
        // depth, so it cannot mask the resolved user-owned mesh.
        val markerColor = if (local) color else color.copyOf().also {
            it[3] = it[3].coerceAtMost(0.42f)
        }
        if (local) {
            draw(cubeBuffer, CUBE_VERTICES.size / 3, GLES20.GL_TRIANGLES, modelViewProjection, markerColor, 1f)
        } else {
            drawOverlay(cubeBuffer, CUBE_VERTICES.size / 3, GLES20.GL_TRIANGLES, modelViewProjection, markerColor, 1f)
        }
        drawHeadingArrow(
            x, y, z, angleDegrees,
            if (local) floatArrayOf(0.9f, 0.98f, 1f, 1f) else markerColor,
            overlay = !local
        )
    }

    private fun drawVehicleProxy(
        x: Float,
        y: Float,
        z: Float,
        angleDegrees: Float,
        color: FloatArray,
        dynamicSyncKnown: Boolean
    ) {
        // StreamInVehicle establishes identity; Packet 200/209 may update the
        // same proxy transform. A user-owned `cars` IDE/DFF may render behind
        // this small authority marker, but the marker remains when an asset is
        // missing, unsupported, or outside the bounded dynamic model batch.
        Matrix.setIdentityM(model, 0)
        Matrix.translateM(model, 0, x, y, z)
        Matrix.rotateM(model, 0, -angleDegrees, 0f, 1f, 0f)
        Matrix.scaleM(model, 0, 2.2f, 0.58f, 1.12f)
        Matrix.multiplyMM(modelViewProjection, 0, viewProjection, 0, model, 0)
        val renderedColor = color.copyOf().also {
            it[3] = if (dynamicSyncKnown) 0.42f else 0.28f
        }
        drawOverlay(cubeBuffer, CUBE_VERTICES.size / 3, GLES20.GL_TRIANGLES, modelViewProjection, renderedColor, 1f)
        drawHeadingArrow(
            x, y, z, angleDegrees,
            renderedColor.copyOf().also { it[3] = 0.74f },
            overlay = true
        )
    }

    private fun drawPickupProxy(x: Float, y: Float, z: Float, pickup: WorldPickup) {
        // This is an intentional interaction marker, not a substituted GTA
        // pickup model. Server authority remains with RPC 131 after USE.
        Matrix.setIdentityM(model, 0)
        Matrix.translateM(model, 0, x, y + 0.8f, z)
        Matrix.rotateM(model, 0, 45f, 0f, 1f, 0f)
        Matrix.rotateM(model, 0, 45f, 1f, 0f, 0f)
        Matrix.scaleM(model, 0, 0.43f, 0.43f, 0.43f)
        Matrix.multiplyMM(modelViewProjection, 0, viewProjection, 0, model, 0)
        val color = if (pickup.spawnType == 14) {
            floatArrayOf(1f, 0.62f, 0.20f, 0.96f)
        } else {
            floatArrayOf(0.34f, 0.98f, 0.66f, 0.96f)
        }
        draw(cubeBuffer, CUBE_VERTICES.size / 3, GLES20.GL_TRIANGLES, modelViewProjection, color, 1f)
        val beacon = floatBuffer(floatArrayOf(x, y + 0.12f, z, x, y + 2.0f, z))
        draw(beacon, 2, GLES20.GL_LINES, viewProjection, color.copyOf().also { it[3] = 0.78f }, 1.4f)
    }

    private fun drawServerObjectProxy(x: Float, y: Float, z: Float, objectValue: WorldObject) {
        // CreateObject gives a real server transform, but this translucent
        // primitive is never claimed as the object's GTA DFF. It remains an
        // authority marker even if a bounded asset batch resolves the model.
        Matrix.setIdentityM(model, 0)
        Matrix.translateM(model, 0, x, y, z)
        // GTA (x,y,z) maps to GLES (x,z,-y): rotations around GTA X/Z keep
        // their scene axes, while GTA Y maps to the negative scene Z axis.
        Matrix.rotateM(model, 0, objectValue.rotationX, 1f, 0f, 0f)
        Matrix.rotateM(model, 0, objectValue.rotationY, 0f, 0f, -1f)
        Matrix.rotateM(model, 0, objectValue.rotationZ, 0f, 1f, 0f)
        Matrix.scaleM(model, 0, 0.82f, 0.82f, 0.82f)
        Matrix.multiplyMM(modelViewProjection, 0, viewProjection, 0, model, 0)
        drawOverlay(cubeBuffer, CUBE_VERTICES.size / 3, GLES20.GL_TRIANGLES, modelViewProjection,
            floatArrayOf(0.96f, 0.67f, 0.17f, 0.42f), 1f)
    }

    private fun drawPlayerAttachedObjectProxy(
        x: Float,
        y: Float,
        z: Float,
        attachment: WorldPlayerAttachedObject
    ) {
        // This is deliberately an authority/accessibility marker, not a claim
        // of GTA bone, mesh, or material fidelity. It shares the sampled native
        // transform and source scale with the DFF path, so an unsupported user
        // asset does not erase a server-owned accessory entirely.
        Matrix.setIdentityM(model, 0)
        Matrix.translateM(model, 0, x, y, z)
        Matrix.rotateM(model, 0, attachment.rotationX, 1f, 0f, 0f)
        Matrix.rotateM(model, 0, attachment.rotationY, 0f, 0f, -1f)
        Matrix.rotateM(model, 0, attachment.rotationZ, 0f, 1f, 0f)
        Matrix.scaleM(
            model,
            0,
            0.30f * attachment.scaleX,
            0.30f * attachment.scaleY,
            0.30f * attachment.scaleZ
        )
        Matrix.multiplyMM(modelViewProjection, 0, viewProjection, 0, model, 0)
        val sourceColour = if (attachment.colour1Abgr != 0L) {
            attachment.colour1Abgr
        } else {
            attachment.colour2Abgr
        }
        drawOverlay(
            cubeBuffer,
            CUBE_VERTICES.size / 3,
            GLES20.GL_TRIANGLES,
            modelViewProjection,
            abgr(sourceColour, 0.48f),
            1f
        )
    }

    private fun drawHeadingArrow(
        x: Float,
        y: Float,
        z: Float,
        angleDegrees: Float,
        color: FloatArray,
        overlay: Boolean
    ) {
        val radians = Math.toRadians(angleDegrees.toDouble())
        val forwardX = sin(radians).toFloat()
        val forwardZ = -cos(radians).toFloat()
        val vertices = floatBuffer(floatArrayOf(
            x, y + 2.25f, z,
            x + forwardX * 1.7f, y + 2.25f, z + forwardZ * 1.7f
        ))
        val arrowColor = color.copyOf().also { it[3] = 0.92f }
        if (overlay) {
            drawOverlay(vertices, 2, GLES20.GL_LINES, viewProjection, arrowColor, 2f)
        } else {
            draw(vertices, 2, GLES20.GL_LINES, viewProjection, arrowColor, 2f)
        }
    }

    private fun drawRemoteAimDirection(
        originX: Float,
        originY: Float,
        originZ: Float,
        frontX: Float,
        frontY: Float,
        frontZ: Float
    ) {
        val lengthSquared = frontX * frontX + frontY * frontY + frontZ * frontZ
        if (!originX.isFinite() || !originY.isFinite() || !originZ.isFinite() ||
            !lengthSquared.isFinite() || lengthSquared <= 0f ||
            originX * originX + originZ * originZ > MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE
        ) return
        val inverseLength = 1f / kotlin.math.sqrt(lengthSquared)
        if (!inverseLength.isFinite()) return
        // This intentionally terminates after a very short normalized segment.
        // It is not a collision query or a visible claim that anything was hit.
        val distance = 6f
        val vertices = floatBuffer(floatArrayOf(
            originX, originY, originZ,
            originX + frontX * inverseLength * distance,
            originY + frontY * inverseLength * distance,
            originZ + frontZ * inverseLength * distance
        ))
        drawOverlay(
            vertices,
            2,
            GLES20.GL_LINES,
            viewProjection,
            floatArrayOf(1f, 0.78f, 0.25f, 0.74f),
            1.6f
        )
    }

    private fun drawRemoteShotDirection(
        originX: Float,
        originY: Float,
        originZ: Float,
        directionX: Float,
        directionY: Float,
        directionZ: Float
    ) {
        val lengthSquared = directionX * directionX + directionY * directionY +
            directionZ * directionZ
        if (!originX.isFinite() || !originY.isFinite() || !originZ.isFinite() ||
            !lengthSquared.isFinite() || lengthSquared <= 0f ||
            originX * originX + originY * originY + originZ * originZ >
                MAX_RENDER_DISTANCE * MAX_RENDER_DISTANCE
        ) return
        val inverseLength = 1f / kotlin.math.sqrt(lengthSquared)
        if (!inverseLength.isFinite()) return
        // Keep this well short of the wire HitPos. The renderer is showing a
        // relayed shot direction, not declaring that a target was struck.
        val distance = 9f
        val vertices = floatBuffer(floatArrayOf(
            originX, originY, originZ,
            originX + directionX * inverseLength * distance,
            originY + directionY * inverseLength * distance,
            originZ + directionZ * inverseLength * distance
        ))
        drawOverlay(
            vertices,
            2,
            GLES20.GL_LINES,
            viewProjection,
            floatArrayOf(1f, 0.40f, 0.12f, 0.94f),
            2.2f
        )
    }

    private fun drawForcedAnimationCue(
        x: Float,
        y: Float,
        z: Float,
        animation: WorldForcedAnimation
    ) {
        if (!animation.active) return
        // This ring/beacon is deliberately an ANIM directive cue, not a claim
        // that the proxy or DFF mesh is being skeleton-animated. The native
        // reducer preserves the exact server library/name for a future
        // user-owned IFP path while keeping today's GLES behavior honest.
        val pulse = if (animation.loop) {
            0.12f * ((sin(System.nanoTime() / 180_000_000.0) + 1.0) * 0.5).toFloat()
        } else {
            0f
        }
        val color = if (animation.freeze) {
            floatArrayOf(1f, 0.67f, 0.20f, 0.90f)
        } else {
            floatArrayOf(0.26f, 0.88f, 1f, 0.82f)
        }
        drawMarker(x, y, z, 1.12f + pulse, color)
        if (animation.lockX || animation.lockY) {
            drawMarker(x, y, z, 1.45f + pulse, color.copyOf().also { it[3] = 0.54f })
        }
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

    private fun ensureTexturedPreviewTexture(preview: TexturedDffPreviewState): Boolean {
        if (texturedProgram == 0 || texturedPositionHandle < 0 || texturedColorHandle < 0 ||
            texturedUvHandle < 0 || texturedMvpHandle < 0 || texturedSamplerHandle < 0
        ) return false
        if (texturedPreviewTextureId != 0 &&
            uploadedTexturedDffPreviewVersion == preview.version
        ) return true
        if (texturedPreviewTextureId != 0) {
            GLES20.glDeleteTextures(1, intArrayOf(texturedPreviewTextureId), 0)
            texturedPreviewTextureId = 0
        }
        val textureId = IntArray(1)
        GLES20.glGenTextures(1, textureId, 0)
        if (textureId[0] == 0) return false
        val repeat = isPowerOfTwo(preview.textureWidth) && isPowerOfTwo(preview.textureHeight)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId[0])
        GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D,
            GLES20.GL_TEXTURE_WRAP_S,
            if (repeat) GLES20.GL_REPEAT else GLES20.GL_CLAMP_TO_EDGE
        )
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D,
            GLES20.GL_TEXTURE_WRAP_T,
            if (repeat) GLES20.GL_REPEAT else GLES20.GL_CLAMP_TO_EDGE
        )
        preview.textureRgba.position(0)
        GLES20.glTexImage2D(
            GLES20.GL_TEXTURE_2D,
            0,
            GLES20.GL_RGBA,
            preview.textureWidth,
            preview.textureHeight,
            0,
            GLES20.GL_RGBA,
            GLES20.GL_UNSIGNED_BYTE,
            preview.textureRgba
        )
        preview.textureRgba.position(0)
        if (GLES20.glGetError() != GLES20.GL_NO_ERROR) {
            GLES20.glDeleteTextures(1, textureId, 0)
            return false
        }
        texturedPreviewTextureId = textureId[0]
        uploadedTexturedDffPreviewVersion = preview.version
        return true
    }

    private fun clearUnusedTexturedSlots(
        textureIds: IntArray,
        uploadedVersions: LongArray,
        uploadedSources: Array<UploadedTextureSource?>,
        usedSlots: Int
    ) {
        val start = usedSlots.coerceIn(0, textureIds.size)
        for (slot in start until textureIds.size) {
            if (textureIds[slot] != 0) {
                GLES20.glDeleteTextures(1, intArrayOf(textureIds[slot]), 0)
                textureIds[slot] = 0
            }
            uploadedVersions[slot] = Long.MIN_VALUE
            uploadedSources[slot] = null
        }
    }

    /** Exact RGBA comparison avoids reuploading an immutable TXD every dynamic refresh. */
    private fun sameUploadedTexture(
        source: UploadedTextureSource?,
        preview: TexturedDffPreviewState
    ): Boolean {
        if (source == null || source.width != preview.textureWidth ||
            source.height != preview.textureHeight ||
            source.rgba.capacity() != preview.textureRgba.capacity()
        ) return false
        for (index in 0 until source.rgba.capacity()) {
            if (source.rgba.get(index) != preview.textureRgba.get(index)) return false
        }
        return true
    }

    private fun ensureTexturedSlot(
        slot: Int,
        preview: TexturedDffPreviewState,
        textureIds: IntArray,
        uploadedVersions: LongArray,
        uploadedSources: Array<UploadedTextureSource?>
    ): Boolean {
        if (slot !in textureIds.indices || texturedProgram == 0 ||
            texturedPositionHandle < 0 || texturedColorHandle < 0 || texturedUvHandle < 0 ||
            texturedMvpHandle < 0 || texturedSamplerHandle < 0
        ) return false
        if (textureIds[slot] != 0) {
            if (uploadedVersions[slot] == preview.version ||
                sameUploadedTexture(uploadedSources[slot], preview)
            ) {
                // A newly copied vertex payload may carry the same immutable
                // TXD pixels. Update only the generation and reuse the GL id.
                uploadedVersions[slot] = preview.version
                return true
            }
            GLES20.glDeleteTextures(1, intArrayOf(textureIds[slot]), 0)
            textureIds[slot] = 0
            uploadedVersions[slot] = Long.MIN_VALUE
            uploadedSources[slot] = null
        }
        val textureId = IntArray(1)
        GLES20.glGenTextures(1, textureId, 0)
        if (textureId[0] == 0) return false
        val repeat = isPowerOfTwo(preview.textureWidth) && isPowerOfTwo(preview.textureHeight)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId[0])
        GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D,
            GLES20.GL_TEXTURE_WRAP_S,
            if (repeat) GLES20.GL_REPEAT else GLES20.GL_CLAMP_TO_EDGE
        )
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D,
            GLES20.GL_TEXTURE_WRAP_T,
            if (repeat) GLES20.GL_REPEAT else GLES20.GL_CLAMP_TO_EDGE
        )
        preview.textureRgba.position(0)
        GLES20.glTexImage2D(
            GLES20.GL_TEXTURE_2D,
            0,
            GLES20.GL_RGBA,
            preview.textureWidth,
            preview.textureHeight,
            0,
            GLES20.GL_RGBA,
            GLES20.GL_UNSIGNED_BYTE,
            preview.textureRgba
        )
        preview.textureRgba.position(0)
        if (GLES20.glGetError() != GLES20.GL_NO_ERROR) {
            GLES20.glDeleteTextures(1, textureId, 0)
            return false
        }
        textureIds[slot] = textureId[0]
        uploadedVersions[slot] = preview.version
        uploadedSources[slot] = UploadedTextureSource(
            preview.textureWidth,
            preview.textureHeight,
            preview.textureRgba.asReadOnlyBuffer().apply { position(0) }
        )
        return true
    }

    private fun clearUnusedNearbyWorldTextureSlots(usedSlots: Int) = clearUnusedTexturedSlots(
        nearbyWorldTextureIds,
        uploadedNearbyWorldTextureVersions,
        uploadedNearbyWorldTextureSources,
        usedSlots
    )

    private fun clearUnusedNearbyDynamicModelTextureSlots(usedSlots: Int) = clearUnusedTexturedSlots(
        nearbyDynamicModelTextureIds,
        uploadedNearbyDynamicModelTextureVersions,
        uploadedNearbyDynamicModelTextureSources,
        usedSlots
    )

    private fun ensureNearbyWorldTexture(slot: Int, preview: TexturedDffPreviewState): Boolean =
        ensureTexturedSlot(
            slot,
            preview,
            nearbyWorldTextureIds,
            uploadedNearbyWorldTextureVersions,
            uploadedNearbyWorldTextureSources
        )

    private fun ensureNearbyDynamicModelTexture(slot: Int, preview: TexturedDffPreviewState): Boolean =
        ensureTexturedSlot(
            slot,
            preview,
            nearbyDynamicModelTextureIds,
            uploadedNearbyDynamicModelTextureVersions,
            uploadedNearbyDynamicModelTextureSources
        )

    private fun drawNearbyWorldTextured(
        preview: TexturedDffPreviewState,
        textureId: Int,
        transform: FloatArray
    ) {
        GLES20.glUseProgram(texturedProgram)
        GLES20.glUniformMatrix4fv(texturedMvpHandle, 1, false, transform, 0)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId)
        GLES20.glUniform1i(texturedSamplerHandle, 0)
        preview.vertices.position(0)
        GLES20.glEnableVertexAttribArray(texturedPositionHandle)
        GLES20.glVertexAttribPointer(
            texturedPositionHandle,
            3,
            GLES20.GL_FLOAT,
            false,
            DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX * 4,
            preview.vertices
        )
        preview.vertices.position(3)
        GLES20.glEnableVertexAttribArray(texturedColorHandle)
        GLES20.glVertexAttribPointer(
            texturedColorHandle,
            4,
            GLES20.GL_FLOAT,
            false,
            DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX * 4,
            preview.vertices
        )
        preview.vertices.position(7)
        GLES20.glEnableVertexAttribArray(texturedUvHandle)
        GLES20.glVertexAttribPointer(
            texturedUvHandle,
            2,
            GLES20.GL_FLOAT,
            false,
            DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX * 4,
            preview.vertices
        )
        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, preview.vertexCount)
        GLES20.glDisableVertexAttribArray(texturedPositionHandle)
        GLES20.glDisableVertexAttribArray(texturedColorHandle)
        GLES20.glDisableVertexAttribArray(texturedUvHandle)
        preview.vertices.position(0)
    }

    private fun drawTexturedDffPreview(preview: TexturedDffPreviewState, transform: FloatArray) {
        GLES20.glUseProgram(texturedProgram)
        GLES20.glUniformMatrix4fv(texturedMvpHandle, 1, false, transform, 0)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texturedPreviewTextureId)
        GLES20.glUniform1i(texturedSamplerHandle, 0)
        preview.vertices.position(0)
        GLES20.glEnableVertexAttribArray(texturedPositionHandle)
        GLES20.glVertexAttribPointer(
            texturedPositionHandle,
            3,
            GLES20.GL_FLOAT,
            false,
            DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX * 4,
            preview.vertices
        )
        preview.vertices.position(3)
        GLES20.glEnableVertexAttribArray(texturedColorHandle)
        GLES20.glVertexAttribPointer(
            texturedColorHandle,
            4,
            GLES20.GL_FLOAT,
            false,
            DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX * 4,
            preview.vertices
        )
        preview.vertices.position(7)
        GLES20.glEnableVertexAttribArray(texturedUvHandle)
        GLES20.glVertexAttribPointer(
            texturedUvHandle,
            2,
            GLES20.GL_FLOAT,
            false,
            DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX * 4,
            preview.vertices
        )
        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, preview.vertexCount)
        GLES20.glDisableVertexAttribArray(texturedPositionHandle)
        GLES20.glDisableVertexAttribArray(texturedColorHandle)
        GLES20.glDisableVertexAttribArray(texturedUvHandle)
        preview.vertices.position(0)
    }

    private fun drawDffPreview(vertices: FloatBuffer, count: Int, transform: FloatArray) {
        GLES20.glUseProgram(program)
        GLES20.glUniformMatrix4fv(mvpHandle, 1, false, transform, 0)
        GLES20.glUniform4f(colorHandle, 1f, 1f, 1f, 1f)
        vertices.position(0)
        GLES20.glEnableVertexAttribArray(positionHandle)
        GLES20.glVertexAttribPointer(
            positionHandle,
            3,
            GLES20.GL_FLOAT,
            false,
            DFF_PREVIEW_FLOATS_PER_VERTEX * 4,
            vertices
        )
        vertices.position(3)
        GLES20.glEnableVertexAttribArray(vertexColorHandle)
        GLES20.glVertexAttribPointer(
            vertexColorHandle,
            4,
            GLES20.GL_FLOAT,
            false,
            DFF_PREVIEW_FLOATS_PER_VERTEX * 4,
            vertices
        )
        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, count)
        GLES20.glDisableVertexAttribArray(positionHandle)
        GLES20.glDisableVertexAttribArray(vertexColorHandle)
        vertices.position(0)
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
        GLES20.glDisableVertexAttribArray(vertexColorHandle)
        GLES20.glVertexAttrib4f(vertexColorHandle, 1f, 1f, 1f, 1f)
        vertices.position(0)
        GLES20.glEnableVertexAttribArray(positionHandle)
        GLES20.glVertexAttribPointer(positionHandle, 3, GLES20.GL_FLOAT, false, 0, vertices)
        if (mode == GLES20.GL_LINES || mode == GLES20.GL_LINE_LOOP) GLES20.glLineWidth(lineWidth)
        GLES20.glDrawArrays(mode, 0, count)
        GLES20.glDisableVertexAttribArray(positionHandle)
    }

    /** Draws a fallback authority marker without allowing it to occlude a DFF. */
    private fun drawOverlay(
        vertices: FloatBuffer,
        count: Int,
        mode: Int,
        transform: FloatArray,
        color: FloatArray,
        lineWidth: Float
    ) {
        GLES20.glDepthMask(false)
        draw(vertices, count, mode, transform, color, lineWidth)
        GLES20.glDepthMask(true)
    }

    private fun environmentClearColor(frame: WorldFrame): FloatArray {
        // A recipient-specific SetPlayerTime overrides SetWorldTime only for
        // this local visual sample. TogglePlayerClock hides the HUD readout,
        // not the server's time-of-day environment. Weather remains a neutral
        // haze proxy; no claim is made to reproduce GTA's art/palette.
        val hour = when {
            frame.playerTimeKnown -> frame.playerHour.coerceIn(0, 23) +
                frame.playerMinute.coerceIn(0, 59) / 60f
            frame.worldTimeKnown -> frame.worldTime.coerceIn(0, 23).toFloat()
            else -> 12f
        }
        val sun = max(0f, sin(((hour - 6f) / 24f * Math.PI * 2.0).toFloat()))
        val haze = if (frame.weatherKnown) 0.035f + (frame.weather % 4) * 0.018f else 0f
        val dim = (1f - haze).coerceAtLeast(0.78f)
        return floatArrayOf(
            (0.014f + 0.105f * sun) * dim,
            (0.031f + 0.255f * sun) * dim,
            (0.061f + 0.315f * sun) * dim
        )
    }

    private fun vehicleColour(primary: Int, secondary: Int): FloatArray {
        // Vehicle RPC colors are SA-MP palette IDs. This stable hue proxy makes
        // them distinguishable without claiming the exact user-owned GTA paint.
        val hue = ((primary * 37 + secondary * 13) % 360).toFloat()
        val color = Color.HSVToColor(floatArrayOf(hue, 0.58f, 0.96f))
        return floatArrayOf(
            Color.red(color) / 255f,
            Color.green(color) / 255f,
            Color.blue(color) / 255f,
            0.92f
        )
    }

    /** Converts a source/wire ABGR integer, unlike ordinary actor RGBA fields. */
    private fun abgr(value: Long, alphaMultiplier: Float): FloatArray {
        if (value == 0L) return floatArrayOf(0.96f, 0.67f, 0.17f, alphaMultiplier)
        val r = (value and 0xFF).toFloat() / 255f
        val g = ((value shr 8) and 0xFF).toFloat() / 255f
        val b = ((value shr 16) and 0xFF).toFloat() / 255f
        val a = ((value shr 24) and 0xFF).toFloat() / 255f
        return floatArrayOf(r, g, b, max(0.22f, a) * alphaMultiplier)
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

    /** Retained CPU view of exactly the RGBA bytes behind one uploaded GL texture. */
    private data class UploadedTextureSource(
        val width: Int,
        val height: Int,
        val rgba: ByteBuffer
    )

    private data class TexturedDffPreviewState(
        val version: Long,
        val vertices: FloatBuffer,
        val textureRgba: ByteBuffer,
        val textureWidth: Int,
        val textureHeight: Int,
        val vertexCount: Int
    )

    private data class RemoteActorPresentation(
        val x: Float,
        val y: Float,
        val z: Float,
        val angle: Float,
        val onFootKnown: Boolean,
        val inVehicle: Boolean
    )

    private data class VehiclePresentation(
        val x: Float,
        val y: Float,
        val z: Float,
        val angle: Float,
        val dynamicSyncKnown: Boolean
    )

    private data class SceneCameraState(
        val frame: WorldFrame,
        val priorRemoteActors: Map<Int, RemoteActorPresentation>,
        val priorVehicles: Map<Int, VehiclePresentation>,
        val remoteActorBlendStartedNanos: Long,
        val userYawDegrees: Float,
        val userPitchDegrees: Float,
        val cameraDistance: Float,
        val dffPreviewBuffer: FloatBuffer?,
        val dffPreviewVertexCount: Int,
        val nearbyWorldBuffer: FloatBuffer?,
        val nearbyWorldVertexCount: Int,
        val nearbyDynamicModelBuffer: FloatBuffer?,
        val nearbyDynamicModelVertexCount: Int,
        val nearbyDynamicModelAnchorX: Float,
        val nearbyDynamicModelAnchorY: Float,
        val nearbyDynamicModelAnchorZ: Float,
        val nearbyDynamicModelTextured: List<TexturedDffPreviewState>,
        val nearbyWorldAnchorX: Float,
        val nearbyWorldAnchorY: Float,
        val nearbyWorldAnchorZ: Float,
        val nearbyWorldTextured: List<TexturedDffPreviewState>,
        val texturedDffPreview: TexturedDffPreviewState?
    )

    companion object {
        private const val MAX_RENDER_DISTANCE = 650f
        private const val DFF_PREVIEW_FLOATS_PER_VERTEX = 7
        private const val DFF_TEXTURED_PREVIEW_FLOATS_PER_VERTEX = 9
        private const val VERTEX_SHADER = """
            uniform mat4 uMvp;
            uniform vec4 uColor;
            attribute vec3 aPosition;
            attribute vec4 aVertexColor;
            varying vec4 vColor;
            void main() {
                gl_Position = uMvp * vec4(aPosition, 1.0);
                vColor = uColor * aVertexColor;
            }
        """
        private const val FRAGMENT_SHADER = """
            precision mediump float;
            varying vec4 vColor;
            void main() {
                gl_FragColor = vColor;
            }
        """
        private const val TEXTURED_VERTEX_SHADER = """
            uniform mat4 uMvp;
            attribute vec3 aPosition;
            attribute vec4 aVertexColor;
            attribute vec2 aTexCoord;
            varying vec4 vColor;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = uMvp * vec4(aPosition, 1.0);
                vColor = aVertexColor;
                vTexCoord = aTexCoord;
            }
        """
        private const val TEXTURED_FRAGMENT_SHADER = """
            precision mediump float;
            uniform sampler2D uTexture;
            varying vec4 vColor;
            varying vec2 vTexCoord;
            void main() {
                gl_FragColor = texture2D(uTexture, vTexCoord) * vColor;
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

        private fun byteBuffer(values: ByteArray): ByteBuffer =
            ByteBuffer.allocateDirect(values.size)
                .order(ByteOrder.nativeOrder())
                .apply {
                    put(values)
                    position(0)
                }

        private fun isPowerOfTwo(value: Int): Boolean = value > 0 && (value and (value - 1)) == 0

        private fun normalizeDegrees(value: Float): Float {
            var normalized = value % 360f
            if (normalized > 180f) normalized -= 360f
            if (normalized < -180f) normalized += 360f
            return normalized
        }
    }
}
