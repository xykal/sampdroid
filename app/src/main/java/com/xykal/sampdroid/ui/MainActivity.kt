package com.xykal.sampdroid.ui

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.DividerItemDecoration
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.floatingactionbutton.FloatingActionButton
import com.xykal.sampdroid.NativeBridge
import com.xykal.sampdroid.R
import com.xykal.sampdroid.model.ServerEntry
import com.xykal.sampdroid.net.ServerListRepository
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

private enum class GameDataDocumentKind(val label: String) {
    COL("COL collision"),
    IDE("IDE object"),
    IPL("IPL placement")
}

class MainActivity : AppCompatActivity() {

    private lateinit var adapter: ServerAdapter
    private lateinit var progress: ProgressBar
    private lateinit var status: TextView
    private lateinit var list: RecyclerView

    // SAF grants access only to a file the player explicitly chooses; this
    // avoids broad storage permission and never copies game data into the APK.
    private val gtaImgPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) inspectUserOwnedImg(uri)
    }
    private val gtaColPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) loadGameDataDocument(uri, GameDataDocumentKind.COL)
    }
    private val gtaIdePicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) loadGameDataDocument(uri, GameDataDocumentKind.IDE)
    }
    private val gtaIplPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) loadGameDataDocument(uri, GameDataDocumentKind.IPL)
    }
    private val gtaDataTreePicker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) inspectUserOwnedGtaTree(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        findViewById<MaterialToolbar>(R.id.toolbar).apply {
            title = "SampDroid"
            subtitle = "native SA-MP client · v" + try {
                NativeBridge.nativeVersion()
            } catch (t: Throwable) {
                "native error: ${t.message}"
            }
            inflateMenu(R.menu.menu_main)
            setOnMenuItemClickListener { item ->
                when (item.itemId) {
                    R.id.action_select_gta_img -> {
                        showGameDataMenu()
                        true
                    }
                    else -> false
                }
            }
        }

        progress = findViewById(R.id.progress)
        status = findViewById(R.id.tvStatus)
        list = findViewById(R.id.recycler)

        adapter = ServerAdapter { openDetail(it) }
        list.layoutManager = LinearLayoutManager(this)
        list.adapter = adapter
        list.addItemDecoration(DividerItemDecoration(this, DividerItemDecoration.VERTICAL))

        findViewById<EditText>(R.id.etSearch).addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) = Unit
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) =
                adapter.filter(s?.toString() ?: "")
            override fun afterTextChanged(s: Editable?) = Unit
        })

        findViewById<FloatingActionButton>(R.id.fabRefresh).setOnClickListener { loadServers() }

        // The persisted SAF grant can point at removable SD storage too. Mount
        // it lazily on every process start without copying game files internally.
        restorePersistedGtaTree()
        loadServers()
    }

    private fun loadServers() {
        progress.visibility = View.VISIBLE
        status.text = "Mengambil daftar server dari open.mp…"
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching { ServerListRepository.fetch() }
            }
            progress.visibility = View.GONE
            result.onSuccess { servers ->
                adapter.submit(servers)
                status.text = "${servers.size} server ditemukan"
            }.onFailure { e ->
                status.text = "Gagal memuat: ${e.message}"
            }
        }
    }

    private fun openDetail(server: ServerEntry) {
        ServerDetailDialog.show(this, server)
    }

    private fun inspectUserOwnedImg(uri: Uri) {
        // OpenDocument normally grants this. Persisting is best effort because
        // some providers intentionally offer a session-only descriptor.
        runCatching {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        status.text = "Memeriksa indeks IMG milik kamu secara read-only…"
        lifecycleScope.launch {
            val report = withContext(Dispatchers.IO) {
                runCatching {
                    contentResolver.openFileDescriptor(uri, "r")?.use { descriptor ->
                        NativeBridge.nativeGameDataMountImgFile(descriptor.fd)
                    } ?: error("File yang dipilih tidak memberi file descriptor")
                }
            }
            report.onFailure {
                status.text = "Data GTA tidak bisa diperiksa"
                showGtaImgReport(
                    "Data GTA belum bisa dipakai",
                    "Android tidak memberi descriptor read-only yang seekable untuk file itu. " +
                        "Pilih file GTA3.IMG dari data game yang kamu miliki."
                )
            }.onSuccess { rawJson ->
                val json = runCatching { JSONObject(rawJson) }.getOrNull()
                if (json == null) {
                    status.text = "Respons data GTA tidak valid"
                    showGtaImgReport("Data GTA belum bisa dipakai", "Respons validator native tidak valid.")
                    return@onSuccess
                }
                val error = json.optString("error")
                if (!json.optBoolean("ok")) {
                    status.text = "IMG GTA ditolak: $error"
                    showGtaImgReport("IMG belum cocok", if (error.isBlank()) {
                        "File bukan IMG v2 GTA SA yang didukung."
                    } else {
                        error
                    })
                    return@onSuccess
                }

                getSharedPreferences(GAME_DATA_PREFS, MODE_PRIVATE).edit()
                    .putString(KEY_GTA_IMG_URI, uri.toString())
                    .remove(KEY_GTA_DATA_TREE_URI)
                    .remove(KEY_GTA_MAP_SOURCES_SYNCED)
                    .apply()
                val bytes = json.optLong("imgBytes", json.optLong("bytes"))
                val entries = json.optInt("imgEntries", json.optInt("entries"))
                val collisionModels = json.optInt("collisionModels")
                val placements = json.optInt("iplPlacements")
                status.text = "IMG GTA dipasang read-only ($entries entry)"
                showGtaImgReport(
                    "IMG GTA terpasang untuk world runtime",
                    "${formatBytes(bytes)} · $entries entry\n" +
                        "Model collision: $collisionModels · placement IPL: $placements\n\n" +
                        "Descriptor IMG tetap read-only dan tidak disalin. Kamu bisa memuat source satuan atau " +
                        "sinkronkan COL/IDE/IPL dari gta.dat serta parent_stream IPL bnry dari IMG; renderer map dan gameplay penuh belum aktif."
                )
            }
        }
    }

    private fun mountUserOwnedGtaTree(uri: Uri, rememberTree: Boolean): String {
        val tree = GtaDataTreeResolver.fromTreeUri(this, uri)
            ?: error("Folder SAF yang dipilih tidak dapat dibaca")
        val gtaDat = tree.firstFile(listOf("data/gta.dat", "gta.dat"))
            ?: error("Tidak menemukan data/gta.dat. Pilih folder GTA yang sudah diekstrak, bukan file RAR.")
        val manifestStatus = contentResolver.openFileDescriptor(gtaDat.uri, "r")?.use { descriptor ->
            NativeBridge.nativeGameDataLoadGtaDatFile(descriptor.fd)
        } ?: error("gta.dat tidak memberi descriptor read-only")
        val manifestJson = JSONObject(manifestStatus)
        if (!manifestJson.optBoolean("ok")) return manifestStatus

        val manifest = JSONObject(NativeBridge.nativeGameDataManifest())
        val candidates = buildList {
            add("models/gta3.img")
            manifest.optJSONArray("imgPaths")?.let { paths ->
                for (index in 0 until paths.length()) {
                    paths.optString(index).takeIf { it.isNotBlank() }?.let(::add)
                }
            }
        }.distinct()
        val img = tree.firstFile(candidates)
            ?: error("Tidak menemukan IMG utama dari manifest. Pastikan models/gta3.img ikut diekstrak.")
        val mountStatus = contentResolver.openFileDescriptor(img.uri, "r")?.use { descriptor ->
            NativeBridge.nativeGameDataMountImgFile(descriptor.fd)
        } ?: error("IMG dari folder tidak memberi descriptor read-only")
        if (!JSONObject(mountStatus).optBoolean("ok")) return mountStatus
        // A mount deliberately clears stale source bindings. Parse the same
        // user-selected manifest again after the new IMG commits.
        var finalStatus = contentResolver.openFileDescriptor(gtaDat.uri, "r")?.use { descriptor ->
            NativeBridge.nativeGameDataLoadGtaDatFile(descriptor.fd)
        } ?: error("gta.dat tidak memberi descriptor read-only setelah IMG dipasang")
        if (!runCatching { JSONObject(finalStatus).optBoolean("ok") }.getOrDefault(false)) return finalStatus

        // GTA3 remains the streaming-IPL authority, while bounded
        // `models/*.img` companions extend DFF/TXD lookup and the later shared
        // COL sync. This lets stock cutscene/collision assets resolve without
        // copying anything out of the selected (including SD-card) tree.
        val mountedManifest = JSONObject(NativeBridge.nativeGameDataManifest())
        val auxiliaryImg = tree.resolveFiles(
            mountedManifest.jsonPathList("imgPaths").filter { path ->
                path.startsWith("models/") && path.endsWith(".img")
            },
            MAX_TREE_AUX_IMG_FILES
        ).files
            .filterNot { file -> file.uri.toString() == img.uri.toString() }
            .distinctBy { file -> file.uri.toString() }
        if (auxiliaryImg.isNotEmpty()) {
            val descriptors = ArrayList<android.os.ParcelFileDescriptor>()
            try {
                auxiliaryImg.forEach { file ->
                    descriptors += contentResolver.openFileDescriptor(file.uri, "r")
                        ?: error("${file.name ?: "IMG tambahan"} tidak memberi descriptor read-only")
                }
                finalStatus = NativeBridge.nativeGameDataMountAdditionalImgFiles(
                    descriptors.map { descriptor -> descriptor.fd }.toIntArray()
                )
            } finally {
                descriptors.forEach { descriptor -> runCatching { descriptor.close() } }
            }
            if (!runCatching { JSONObject(finalStatus).optBoolean("ok") }.getOrDefault(false)) return finalStatus
        }
        if (rememberTree) {
            getSharedPreferences(GAME_DATA_PREFS, MODE_PRIVATE).edit()
                .putString(KEY_GTA_DATA_TREE_URI, uri.toString())
                .remove(KEY_GTA_IMG_URI)
                // Mounting deliberately clears native map sources. They become
                // persistent only after the player explicitly syncs this tree.
                .remove(KEY_GTA_MAP_SOURCES_SYNCED)
                .apply()
        }
        return finalStatus
    }

    private fun inspectUserOwnedGtaTree(uri: Uri) {
        runCatching {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        runGameDataOperation("Membaca folder GTA hasil ekstrak secara read-only") {
            mountUserOwnedGtaTree(uri, rememberTree = true)
        }
    }

    private fun restorePersistedGtaTree() {
        val preferences = getSharedPreferences(GAME_DATA_PREFS, MODE_PRIVATE)
        val uri = preferences.getString(KEY_GTA_DATA_TREE_URI, null)
            ?.let(Uri::parse)
            ?: return
        val restoreMapSources = preferences.getBoolean(KEY_GTA_MAP_SOURCES_SYNCED, false)
        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                val mounted = runCatching { mountUserOwnedGtaTree(uri, rememberTree = false) }
                    .getOrNull()
                    ?.let { status -> runCatching { JSONObject(status).optBoolean("ok") }.getOrDefault(false) }
                    ?: false
                // Recreate the native-only map assembly after process death.
                // A stale SD-card grant or malformed source is left recoverable
                // through the explicit Data GTA menu rather than crashing startup.
                if (mounted && restoreMapSources) {
                    runCatching { synchronizeMapSourcesFromTree(uri) }
                }
            }
        }
    }

    private fun loadTreeFileBatches(
        files: List<DocumentFile>,
        load: (IntArray) -> String
    ): String {
        var latest = NativeBridge.nativeGameDataStatus()
        files.chunked(TREE_BATCH_SIZE).forEach { chunk ->
            val descriptors = chunk.map { file ->
                contentResolver.openFileDescriptor(file.uri, "r")
                    ?: error("${file.name ?: "file map"} tidak memberi descriptor read-only")
            }
            try {
                val response = load(descriptors.map { it.fd }.toIntArray())
                val json = JSONObject(response)
                if (!json.optBoolean("ok")) return response
                latest = response
            } finally {
                descriptors.forEach { descriptor -> runCatching { descriptor.close() } }
            }
        }
        return latest
    }

    private fun synchronizeMapSourcesFromTree(treeUri: Uri): String {
        val tree = GtaDataTreeResolver.fromTreeUri(this, treeUri)
            ?: error("Grant folder GTA sudah tidak dapat dibaca; pilih ulang foldernya")
        val manifest = JSONObject(NativeBridge.nativeGameDataManifest())
        val col = tree.resolveFiles(manifest.jsonPathList("colPaths"), MAX_TREE_MAP_FILES_PER_KIND)
        // GTA SA loads vehicle/ped resolver tables outside gta.dat. They are
        // optional for a custom/minimal tree, but fixed read-only relative
        // paths for a normal stock root so streamed SA-MP model IDs can resolve
        // without scanning arbitrary documents or requiring a second picker.
        val dynamicIde = tree.resolveFiles(STOCK_DYNAMIC_IDE_PATHS, STOCK_DYNAMIC_IDE_PATHS.size)
        val ide = tree.resolveFiles(
            manifest.jsonPathList("idePaths"),
            (MAX_TREE_MAP_FILES_PER_KIND - dynamicIde.files.size).coerceAtLeast(0)
        )
        val ideFiles = (ide.files + dynamicIde.files)
            .distinctBy { file -> file.uri.toString() }
        val ipl = tree.resolveFiles(manifest.jsonPathList("iplPaths"), MAX_TREE_MAP_FILES_PER_KIND)
        if (col.files.isEmpty() && ideFiles.isEmpty() && ipl.files.isEmpty()) {
            error("Tidak ada COL/IDE/IPL dari gta.dat yang ditemukan di folder pilihan")
        }
        var latest = NativeBridge.nativeGameDataClearMapSources()
        if (!JSONObject(latest).optBoolean("ok")) return latest
        if (ideFiles.isNotEmpty()) latest = loadTreeFileBatches(ideFiles, NativeBridge::nativeGameDataLoadIdeFiles)
        if (!JSONObject(latest).optBoolean("ok")) return latest
        if (col.files.isNotEmpty()) latest = loadTreeFileBatches(col.files, NativeBridge::nativeGameDataLoadColFiles)
        if (!JSONObject(latest).optBoolean("ok")) return latest
        if (ipl.files.isNotEmpty()) latest = loadTreeFileBatches(ipl.files, NativeBridge::nativeGameDataLoadIplFiles)
        if (!JSONObject(latest).optBoolean("ok")) return latest
        // Stock SA keeps many placement chunks inside gta3.img as
        // parent_streamN.ipl and COL chunks, while its gta.dat normally has
        // no COLFILE directives. Both native scans are explicitly bounded,
        // read-only, and never copy the selected SD-card tree.
        latest = NativeBridge.nativeGameDataLoadImgStreamingIpls()
        if (!JSONObject(latest).optBoolean("ok")) return latest
        return NativeBridge.nativeGameDataLoadAllImgCollisionMembers()
    }

    private fun loadMapSourcesFromSelectedTree() {
        val preferences = getSharedPreferences(GAME_DATA_PREFS, MODE_PRIVATE)
        val treeUri = preferences.getString(KEY_GTA_DATA_TREE_URI, null)
            ?.let(Uri::parse)
        if (treeUri == null) {
            showGtaImgReport(
                "Folder GTA belum dipilih",
                "Pilih dulu ‘Hubungkan folder GTA hasil ekstrak’, lalu coba sinkronkan source map lagi."
            )
            return
        }
        runGameDataOperation("Memuat source map dari gta.dat") {
            val status = synchronizeMapSourcesFromTree(treeUri)
            if (runCatching { JSONObject(status).optBoolean("ok") }.getOrDefault(false)) {
                preferences.edit().putBoolean(KEY_GTA_MAP_SOURCES_SYNCED, true).apply()
            }
            status
        }
    }

    private fun JSONObject.jsonPathList(key: String): List<String> {
        val array = optJSONArray(key) ?: return emptyList()
        return List(array.length()) { index -> array.optString(index) }
            .filter { it.isNotBlank() }
    }

    private fun showGameDataMenu() {
        val actions = arrayOf(
            "Hubungkan folder GTA hasil ekstrak",
            "Sinkronkan COL/IDE/IPL + collision/stream IMG stock",
            "Pasang / ganti GTA3.IMG",
            "Tambah file COL collision",
            "Tambah file IDE object",
            "Tambah file IPL (text / bnry)",
            "Load member .col dari IMG",
            "Load member .dff dari IMG",
            "Load member .txd dari IMG",
            "Status world data",
            "Lepas semua data runtime"
        )
        MaterialAlertDialogBuilder(this)
            .setTitle("Data GTA milik kamu")
            .setMessage(
                "Semua file dibaca lewat Android SAF secara read-only. Kamu boleh pilih folder di SD card; " +
                    "APK tidak menyalin data ke memori internal. Pilih folder GTA yang sudah diekstrak—" +
                    "file RAR/EXE PC tidak dibundel atau dieksekusi oleh APK."
            )
            .setItems(actions) { _, selected ->
                when (selected) {
                    0 -> gtaDataTreePicker.launch(null)
                    1 -> loadMapSourcesFromSelectedTree()
                    2 -> gtaImgPicker.launch(arrayOf("*/*"))
                    3 -> gtaColPicker.launch(arrayOf("*/*"))
                    4 -> gtaIdePicker.launch(arrayOf("*/*"))
                    5 -> gtaIplPicker.launch(arrayOf("*/*"))
                    6 -> promptImgCollisionMember()
                    7 -> promptImgDffMember()
                    8 -> promptImgTxdMember()
                    9 -> runGameDataOperation("Membaca status world data") {
                        NativeBridge.nativeGameDataStatus()
                    }
                    10 -> {
                        getSharedPreferences(GAME_DATA_PREFS, MODE_PRIVATE).edit()
                            .remove(KEY_GTA_IMG_URI)
                            .remove(KEY_GTA_DATA_TREE_URI)
                            .remove(KEY_GTA_MAP_SOURCES_SYNCED)
                            .apply()
                        runGameDataOperation("Melepas data GTA runtime") {
                            NativeBridge.nativeGameDataClear()
                        }
                    }
                }
            }
            .setNegativeButton("Batal", null)
            .show()
    }

    private fun promptImgCollisionMember() {
        val input = EditText(this).apply {
            hint = "contoh: generic.col"
            setSingleLine(true)
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("Member collision dari IMG")
            .setMessage("Masukkan nama member .col yang memang ada di IMG yang sudah dipasang.")
            .setView(input)
            .setNegativeButton("Batal", null)
            .setPositiveButton("Load") { _, _ ->
                val member = input.text?.toString()?.trim().orEmpty()
                if (member.isBlank()) {
                    showGtaImgReport("Member belum dimuat", "Nama member .col tidak boleh kosong.")
                } else {
                    runGameDataOperation("Memuat collision $member") {
                        NativeBridge.nativeGameDataLoadImgCollisionMember(member)
                    }
                }
            }
            .show()
    }

    private fun promptImgDffMember() {
        val input = EditText(this).apply {
            hint = "contoh: testfloor.dff"
            setSingleLine(true)
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("Member model dari IMG")
            .setMessage("Masukkan nama member .dff yang memang ada di IMG yang sudah dipasang.")
            .setView(input)
            .setNegativeButton("Batal", null)
            .setPositiveButton("Load") { _, _ ->
                val member = input.text?.toString()?.trim().orEmpty()
                if (member.isBlank()) {
                    showGtaImgReport("Model belum dimuat", "Nama member .dff tidak boleh kosong.")
                } else {
                    runGameDataOperation("Memuat mesh DFF $member") {
                        NativeBridge.nativeGameDataLoadImgDffMember(member)
                    }
                }
            }
            .show()
    }

    private fun promptImgTxdMember() {
        val input = EditText(this).apply {
            hint = "contoh: generic.txd"
            setSingleLine(true)
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("Dictionary texture dari IMG")
            .setMessage("Masukkan nama member .txd yang memang ada di IMG yang sudah dipasang.")
            .setView(input)
            .setNegativeButton("Batal", null)
            .setPositiveButton("Load") { _, _ ->
                val member = input.text?.toString()?.trim().orEmpty()
                if (member.isBlank()) {
                    showGtaImgReport("Texture belum dimuat", "Nama member .txd tidak boleh kosong.")
                } else {
                    runGameDataOperation("Memuat texture TXD $member") {
                        NativeBridge.nativeGameDataLoadImgTxdMember(member)
                    }
                }
            }
            .show()
    }

    private fun loadGameDataDocument(uri: Uri, kind: GameDataDocumentKind) {
        runCatching {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        runGameDataOperation("Memuat ${kind.label} secara read-only") {
            contentResolver.openFileDescriptor(uri, "r")?.use { descriptor ->
                when (kind) {
                    GameDataDocumentKind.COL -> NativeBridge.nativeGameDataLoadColFile(descriptor.fd)
                    GameDataDocumentKind.IDE -> NativeBridge.nativeGameDataLoadIdeFile(descriptor.fd)
                    GameDataDocumentKind.IPL -> NativeBridge.nativeGameDataLoadIplFile(descriptor.fd)
                }
            } ?: error("File yang dipilih tidak memberi file descriptor")
        }
    }

    private fun runGameDataOperation(loadingLabel: String, operation: () -> String) {
        status.text = "$loadingLabel…"
        lifecycleScope.launch {
            val response = withContext(Dispatchers.IO) { runCatching(operation) }
            response.onFailure {
                status.text = "World data tidak bisa diperbarui"
                showGtaImgReport(
                    "Data GTA belum bisa dipakai",
                    "Android atau file yang dipilih tidak memberi data read-only yang valid."
                )
            }.onSuccess { rawJson ->
                val json = runCatching { JSONObject(rawJson) }.getOrNull()
                if (json == null) {
                    status.text = "Respons world data tidak valid"
                    showGtaImgReport("Data GTA belum bisa dipakai", "Respons runtime native tidak valid.")
                    return@onSuccess
                }
                val error = json.optString("error")
                if (!json.optBoolean("ok")) {
                    status.text = "World data ditolak: $error"
                    showGtaImgReport("Data GTA belum dimuat", if (error.isBlank()) {
                        "Runtime native menolak file atau member itu."
                    } else {
                        error
                    })
                    return@onSuccess
                }
                val entries = json.optInt("imgEntries")
                val additionalImgArchives = json.optInt("additionalImgArchives")
                val additionalImgEntries = json.optInt("additionalImgEntries")
                val additionalImgBytes = json.optLong("additionalImgBytes")
                val models = json.optInt("collisionModels")
                val dffModels = json.optInt("dffModels")
                val dffMeshes = json.optInt("dffMeshes")
                val dffVertices = json.optInt("dffVertices")
                val txdDictionaries = json.optInt("txdDictionaries")
                val txdParentRelations = json.optInt("txdParentRelations")
                val textures = json.optInt("textures")
                val texturePixels = json.optInt("texturePixels")
                val manifestIde = json.optInt("manifestIdePaths")
                val manifestIpl = json.optInt("manifestIplPaths")
                val manifestCol = json.optInt("manifestColPaths")
                val manifestImg = json.optInt("manifestImgPaths")
                val ide = json.optInt("ideObjects")
                val placements = json.optInt("iplPlacements")
                val instances = json.optInt("collisionInstances")
                val missing = json.optInt("missingCollisionPlacements")
                status.text = "World data: $models COL · $instances placement collision"
                showGtaImgReport(
                    "World data runtime diperbarui",
                    "IMG utama entry: $entries · IMG tambahan: $additionalImgArchives ($additionalImgEntries entry, ${formatBytes(additionalImgBytes)})\n" +
                        "COL model: $models · IDE object: $ide\n" +
                        "DFF model: $dffModels · mesh: $dffMeshes · vertex: $dffVertices\n" +
                        "TXD dictionary: $txdDictionaries · parent TXD: $txdParentRelations · texture: $textures · pixel: $texturePixels\n" +
                        "Manifest gta.dat — IMG: $manifestImg · IDE: $manifestIde · IPL: $manifestIpl · COL: $manifestCol\n" +
                        "IPL placement: $placements · collision terpasang: $instances\n" +
                        "Placement menunggu COL: $missing\n\n" +
                        "Data tetap tinggal di folder yang kamu pilih (termasuk SD card) dan tidak dicopy ke internal. " +
                        "Preview DFF/TXD satu batch sudah tersedia; resolver folder dapat memasang sampai empat IMG models tambahan " +
                        "untuk lookup DFF/TXD, serta memuat COL/IDE/IPL text maupun IPL bnry parent_stream dari IMG utama. " +
                        "Streaming asset/collision penuh masih dikerjakan."
                )
            }
        }
    }

    private fun showGtaImgReport(title: String, message: String) {
        if (!isFinishing && !isDestroyed) {
            MaterialAlertDialogBuilder(this)
                .setTitle(title)
                .setMessage(message)
                .setPositiveButton("OK", null)
                .show()
        }
    }

    private fun formatBytes(bytes: Long): String = when {
        bytes < 1024L -> "$bytes B"
        bytes < 1024L * 1024L -> "${bytes / 1024L} KiB"
        bytes < 1024L * 1024L * 1024L -> "${bytes / (1024L * 1024L)} MiB"
        else -> "${bytes / (1024L * 1024L * 1024L)} GiB"
    }

    companion object {
        private const val GAME_DATA_PREFS = "game_data"
        private const val KEY_GTA_IMG_URI = "gta_img_uri"
        private const val KEY_GTA_DATA_TREE_URI = "gta_data_tree_uri"
        private const val KEY_GTA_MAP_SOURCES_SYNCED = "gta_map_sources_synced"
        private const val TREE_BATCH_SIZE = 32
        private const val MAX_TREE_MAP_FILES_PER_KIND = 512
        private const val MAX_TREE_AUX_IMG_FILES = 4
        // These stock global dynamic-model tables are not listed by gta.dat.
        // Missing files are optional so a user-selected custom/minimal root
        // remains usable for its available static world data.
        private val STOCK_DYNAMIC_IDE_PATHS = listOf("data/vehicles.ide", "data/peds.ide")

        const val TAG = "SampDroid"
    }
}
