package com.xykal.sampdroid

/**
 * One bounded, copied DFF triangle batch and a matching decoded TXD level-zero
 * RGBA texture. It is a renderer handoff only; neither array aliases native
 * catalog memory or identifies a filesystem path.
 */
data class NativeTexturedDffPreview(
    val vertices: FloatArray,
    val textureRgba: ByteArray,
    val textureWidth: Int,
    val textureHeight: Int
)

/** One bounded TXD RGBA texture copy for an actual font-4 sprite textdraw. */
data class NativeTexturePreview(
    val textureRgba: ByteArray,
    val textureWidth: Int,
    val textureHeight: Int
)

/**
 * Jembatan ke native library (C++):
 *  - query protocol SA-MP (i / r / c / d / p)
 *  - RakNet 2.x probe and explicit retained native session
 *    (handshake + obfuscation + bounded protocol state)
 */
object NativeBridge {

    init {
        System.loadLibrary("sampdroid")
    }

    external fun nativeVersion(): String

    /**
     * Indexes a user-selected, seekable GTA SA IMG v2 descriptor read-only.
     * The caller keeps ownership of [fileDescriptor]; native duplicates it only
     * for this synchronous inspection and returns structural JSON counts.
     */
    external fun nativeInspectGtaImgFile(fileDescriptor: Int): String

    /**
     * Read-only game-data runtime assembly. Each file descriptor must originate
     * from a user-selected Android SAF document; native duplicates only what it
     * needs and never copies game data into the APK.
     */
    external fun nativeGameDataStatus(): String
    external fun nativeGameDataClear(): String
    external fun nativeGameDataClearMapSources(): String
    external fun nativeGameDataLoadGtaDatFile(fileDescriptor: Int): String
    /** Parses a selected user-owned data/handling.cfg without copying it. */
    external fun nativeGameDataLoadHandlingFile(fileDescriptor: Int): String
    external fun nativeGameDataManifest(): String
    external fun nativeGameDataMountImgFile(fileDescriptor: Int): String
    /**
     * Adds bounded read-only auxiliary IMG archives (for example stock
     * models/cutscene.img) after the primary GTA3 IMG. They extend DFF/TXD
     * lookup and participate in the later bounded full-COL sync; GTA3 remains
     * the sole streaming-IPL authority so parent-stream paths are never guessed.
     */
    external fun nativeGameDataMountAdditionalImgFiles(fileDescriptors: IntArray): String
    external fun nativeGameDataLoadImgCollisionMember(memberName: String): String
    /**
     * Explicit bounded sync for stock roots that keep collision chunks inside
     * GTA3.IMG instead of declaring COLFILE paths. Native only reads selected
     * SAF data and returns structural JSON; it never copies or repackages it.
     */
    external fun nativeGameDataLoadAllImgCollisionMembers(): String
    external fun nativeGameDataLoadImgDffMember(memberName: String): String
    external fun nativeGameDataLoadImgTxdMember(memberName: String): String
    /** Copies only the latest bounded DFF position/RGBA preview into Java. */
    external fun nativeGameDataLatestDffPreview(maxVertices: Int): FloatArray

    /**
     * Copies a bounded IPL/DFF batch around a GTA-space local anchor for the
     * server-authoritative [interior]. Vertices are position/RGBA tuples
     * relative to that anchor; an empty result means map assets are
     * unavailable, unsupported, or out of range and is safe for GLES to ignore.
     */
    external fun nativeGameDataNearbyWorldPreview(
        anchorX: Float,
        anchorY: Float,
        anchorZ: Float,
        radius: Float,
        maxInstances: Int,
        maxVertices: Int,
        interior: Int
    ): FloatArray

    /**
     * Resolves bounded static object, streamed vehicle, or remote-ped transforms
     * through loaded user-owned IDE/IMG data. [transforms] stores
     * x,y,z,rx,ry,rz,drawDistance,scaleX,scaleY,scaleZ per model; model ID 0
     * is valid for CJ. The native session reducer already supplies bounded
     * sampled moving/resolvable object and renderer-only attached-object poses.
     */
    external fun nativeGameDataServerObjectsPreview(
        anchorX: Float,
        anchorY: Float,
        anchorZ: Float,
        radius: Float,
        modelIds: IntArray,
        transforms: FloatArray,
        maxVertices: Int
    ): FloatArray

    /**
     * Returns up to four bounded TXD/material overlays for the same dynamic
     * object, vehicle, and actor transform input accepted by
     * [nativeGameDataServerObjectsPreview]. [materialOverrideSpecs] is one
     * already-sanitized bounded string per model containing `D` Default
     * `slot,sourceModel,ABGR,TXD,texture` records and/or `T` Text records with
     * source fields plus hex UTF-8. A Default source-model 65535 color-only
     * record may deliberately leave both lookup names empty. Empty entries
     * otherwise preserve the IDE/DFF material.
     * An empty/null result keeps the prelit dynamic mesh path valid when a DFF/TXD cannot be resolved.
     */
    external fun nativeGameDataServerObjectsTexturedPreviews(
        anchorX: Float,
        anchorY: Float,
        anchorZ: Float,
        radius: Float,
        modelIds: IntArray,
        transforms: FloatArray,
        materialOverrideSpecs: Array<String>,
        maxBatches: Int,
        maxVertices: Int,
        maxTexturePixels: Int
    ): Array<NativeTexturedDffPreview>?

    /**
     * Returns one safely resolved TXD/material overlay for the nearby IPL/DFF
     * batch. It is optional and remains null until a portable DFF + IDE TXD
     * relation can be decoded from the mounted user-owned IMG.
     */
    external fun nativeGameDataNearbyWorldTexturedPreview(
        anchorX: Float,
        anchorY: Float,
        anchorZ: Float,
        radius: Float,
        maxVertices: Int,
        maxTexturePixels: Int,
        interior: Int
    ): NativeTexturedDffPreview?

    /**
     * Returns up to a small bounded set of independently textured nearby
     * material overlays. Each item owns copied arrays; an empty/null result is
     * safe and does not mean the untextured nearby-world batch failed.
     */
    external fun nativeGameDataNearbyWorldTexturedPreviews(
        anchorX: Float,
        anchorY: Float,
        anchorZ: Float,
        radius: Float,
        maxBatches: Int,
        maxVertices: Int,
        maxTexturePixels: Int,
        interior: Int
    ): Array<NativeTexturedDffPreview>?

    /**
     * Returns one bounded texture/material batch only when its TXD name resolves
     * safely; `null` means absent, unsupported, oversized, or ambiguous.
     */
    external fun nativeGameDataLatestTexturedDffPreview(
        maxVertices: Int,
        maxTexturePixels: Int
    ): NativeTexturedDffPreview?

    /**
     * Resolves a strict `dictionary:texture` font-4 textdraw sprite from
     * already user-approved GTA IMG/TXD data. Null is a safe optional-asset
     * miss; this API neither exposes a path nor fetches network content.
     */
    external fun nativeGameDataTextDrawSpritePreview(
        dictionary: String,
        texture: String,
        maxPixels: Int
    ): NativeTexturePreview?
    external fun nativeGameDataLoadColFile(fileDescriptor: Int): String
    external fun nativeGameDataLoadColFiles(fileDescriptors: IntArray): String
    external fun nativeGameDataLoadIdeFile(fileDescriptor: Int): String
    external fun nativeGameDataLoadIdeFiles(fileDescriptors: IntArray): String
    external fun nativeGameDataLoadIplFile(fileDescriptor: Int): String
    external fun nativeGameDataLoadIplFiles(fileDescriptors: IntArray): String
    /** Discovers strict parent_streamN.ipl placement members from mounted IMG. */
    external fun nativeGameDataLoadImgStreamingIpls(): String

    /** @return JSON: {ok,error,password,players,maxPlayers,hostname,gamemode,language} */
    external fun nativeQueryInfo(ip: String, port: Int, timeoutMs: Int): String

    /** @return JSON array: [{name,value}] */
    external fun nativeQueryRules(ip: String, port: Int, timeoutMs: Int): String

    /** @return JSON array: [{id,name,score,ping}] */
    external fun nativeQueryPlayers(ip: String, port: Int, detailed: Boolean, timeoutMs: Int): String

    /** @return ping ms, atau -1 kalau gagal */
    external fun nativeQueryPing(ip: String, port: Int, timeoutMs: Int): Int

    /**
     * @return JSON status handshake + stream: connection/auth/join, fragment
     * reassembly, RPC summary, and PlayerInit data when the server sends it.
     */
    external fun nativeRaknetProbe(
        ip: String,
        port: Int,
        timeoutMs: Int,
        advertisedServerVersion: String
    ): String

    /**
     * Explicit user-initiated persistent native session. Calls must run on
     * Dispatchers.IO; native bounds open/cookie/auth/post-accept before it
     * retains the UDP socket for normal lifecycle polling. `advertisedServerVersion`
     * comes only from a fresh server query; native selects its DL R1 join profile
     * only when it is an exact 0.3.DL-R1 match and otherwise stays legacy.
     */
    external fun nativeSessionConnect(
        ip: String,
        port: Int,
        timeoutMs: Int,
        nickname: String,
        advertisedServerVersion: String
    ): String

    /** Receives one datagram, ACKs reliable traffic, and returns reduced state JSON. */
    external fun nativeSessionPoll(timeoutMs: Int): String

    /** Sends PlayerRequestClass on source-compatible reliable ordered SyncRPC channel 2. */
    external fun nativeSessionRequestClass(classId: Int): String

    /** Starts the explicit RequestSpawn → PlayerSpawn native handshake. */
    external fun nativeSessionRequestSpawn(): String

    /**
     * Sends one validated normal chat line or slash-prefixed command on the
     * native ordered SyncRPC channel 2. Native rejects unsafe/control bytes and never
     * includes the user text in diagnostic summaries.
     */
    external fun nativeSessionSendChat(message: String): String

    /**
     * Responds to the currently active server ShowDialog on reliable ordered
     * SyncRPC channel 2. The decoded revision must still match so a stale Android UI
     * cannot answer a reissued ID. `listItem` is -1 for non-list dialogs; text
     * is DynStr8-bounded natively and is never retained in diagnostics.
     */
    external fun nativeSessionRespondDialog(
        dialogId: Int,
        dialogRevision: Long,
        response: Boolean,
        listItem: Int,
        text: String
    ): String

    /** Selects/exits only the current server-owned legacy menu revision. */
    external fun nativeSessionSelectMenu(menuId: Int, menuRevision: Long, row: Int): String
    external fun nativeSessionExitMenu(menuId: Int, menuRevision: Long): String

    /**
     * Resolves one user-confirmed USE edge natively: nearby pickup first, then
     * driver-seat vehicle entry, or vehicle exit after server seating. It never
     * grants/changes local vehicle state before authoritative server RPCs.
     */
    /** false=tap driver/pickup USE; true=long-press passenger vehicle intent. */
    external fun nativeSessionUseNearby(passengerIntent: Boolean = false): String

    /** Requests a server-authoritative score/ping roster (RPC 155). */
    external fun nativeSessionRequestScores(): String

    /**
     * Sends an RPC-83 click only for a currently server-selectable textdraw.
     * The raw wire ID is revalidated natively against the copied reducer state.
     */
    external fun nativeSessionSelectTextDraw(wireId: Int): String

    /**
     * Sends a current server-authorized object choice. Android supplies only a
     * bounded object ID and copied selection revision; native re-resolves its
     * current model/origin and rejects a Begin/Cancel-stale RPC-27 list tap.
     */
    external fun nativeSessionSelectObject(objectId: Int, expectedRevision: Long): String

    /**
     * Replies only to the exact active server ObjectEdit mode (RPC 117). Java
     * may submit a bounded presentation transform, copied revision, and response
     * enum Cancel=0/Final=1/Update=2; native owns target ID/kind, lifecycle,
     * reliable channel, stale-revision gate, transform guards, and update throttle.
     */
    external fun nativeSessionEditObject(
        response: Int,
        expectedRevision: Long,
        x: Float,
        y: Float,
        z: Float,
        rotationX: Float,
        rotationY: Float,
        rotationZ: Float
    ): String

    /**
     * Replies to the server's PlayerBeginAttachedObjectEdit (RPC 116). Java
     * provides only copied revision plus local attachment offset/rotation/scale
     * and Cancel=0 or Save=1; C++ re-samples active slot/model/bone/colours,
     * lifecycle, stale-revision, transform bounds, and reliable channel.
     */
    external fun nativeSessionEditAttachedObject(
        response: Int,
        expectedRevision: Long,
        offsetX: Float,
        offsetY: Float,
        offsetZ: Float,
        rotationX: Float,
        rotationY: Float,
        rotationZ: Float,
        scaleX: Float,
        scaleY: Float,
        scaleZ: Float
    ): String

    /**
     * Updates bounded Android-native semantic touch/controller state and returns
     * an OnFoot preview. Horizontal look changes heading; vertical look feeds
     * only the bounded Packet-203 aim pitch path. This does not transmit by itself.
     */
    external fun nativeSessionUpdateControls(
        moveX: Float,
        moveY: Float,
        lookDeltaDegrees: Float,
        lookPitchDeltaDegrees: Float,
        heldActions: Int
    ): String

    /**
     * Advances collision-grounded on-foot or server-authorized driver gameplay
     * once from the latest native touch state. A driver may use a model-matched,
     * bounded user-owned handling.cfg envelope when available, then sends only
     * source-validated sync candidates at the server-advertised rate. It is
     * intentionally void to avoid allocating JSON every gameplay frame;
     * world/session snapshots expose its status.
     */
    external fun nativeSessionAdvanceGameplay(deltaSeconds: Float)

    /**
     * Copies bounded local/remote actor and checkpoint state for the native
     * world/radar surface. This does not read UDP or transmit gameplay data.
     */
    external fun nativeSessionWorldSnapshot(): String

    /** Releases the retained UDP socket. Safe if no session is active. */
    external fun nativeSessionDisconnect(): String

    /** Returns native session state without doing network IO. */
    external fun nativeSessionStatus(): String
}
