package com.xykal.sampdroid.ui

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.View
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
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
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.Inet4Address
import java.net.InetAddress

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
    private lateinit var dataSetupTitle: TextView
    private lateinit var dataSetupDetail: TextView
    private lateinit var profileDetail: TextView
    private lateinit var favoriteButton: TextView

    private sealed interface PendingArchiveInstall {
        data class Remote(val request: GtaArchiveInstaller.Request) : PendingArchiveInstall
        data class Local(val request: GtaArchiveInstaller.LocalRequest) : PendingArchiveInstall
    }

    private data class PendingLocalArchiveSource(val expectedSha256: String?)

    private data class PersistedTreeRestore(
        val mounted: Boolean,
        val problem: String?,
        val mapSourcesNeedSetup: Boolean
    )

    // Native game-data runtime owns process-global descriptor/map state. SAF
    // restoration, a manual mount, and the archive hand-off must therefore
    // never mutate it concurrently on different IO coroutines.
    private val gameDataMutex = Mutex()
    // A restore launched during Activity creation must not mount an old SAF
    // tree after the player has already begun choosing a replacement source.
    // It is read from the IO restore coroutine, hence the visibility guarantee.
    @Volatile
    private var gameDataSelectionEpoch = 0L

    private var pendingArchiveInstall: PendingArchiveInstall? = null
    private var pendingLocalArchiveSource: PendingLocalArchiveSource? = null
    private var archiveInstallActive = false
    // `null` means a persisted tree has not been remounted in this process yet.
    // Never present a stale SAF URI as an active GTA mount while an SD card is
    // absent or its provider has revoked the grant.
    private var persistedTreeMountReady: Boolean? = null
    private var persistedTreeMountProblem: String? = null

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
        if (uri != null && ensurePersistedTreePermission(uri, requireWrite = false)) {
            inspectUserOwnedGtaTree(uri)
        }
    }
    private val gtaArchiveZipPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val pending = pendingLocalArchiveSource
        pendingLocalArchiveSource = null
        if (uri != null && pending != null) {
            // The local ZIP is copied immediately into staging and never stored
            // as a runtime source; persist read access when available but do not
            // require it from providers that support the current selection only.
            runCatching {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            pendingArchiveInstall = PendingArchiveInstall.Local(
                GtaArchiveInstaller.LocalRequest(uri, pending.expectedSha256)
            )
            archiveDestinationTreePicker.launch(null)
        }
    }
    private val archiveDestinationTreePicker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        val pending = pendingArchiveInstall
        pendingArchiveInstall = null
        if (uri != null && pending != null && ensurePersistedTreePermission(uri, requireWrite = true)) {
            when (pending) {
                is PendingArchiveInstall.Remote -> installArchiveIntoSelectedTree(pending.request, uri)
                is PendingArchiveInstall.Local -> installLocalArchiveIntoSelectedTree(pending.request, uri)
            }
        }
    }

    /**
     * A selected SD-card tree must survive process death; a session-only grant
     * would let a player finish a long install and then falsely look mounted on
     * the next launch. Fail before writing/downloading instead of persisting a
     * URI that cannot be reopened.
     */
    private fun ensurePersistedTreePermission(uri: Uri, requireWrite: Boolean): Boolean {
        val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or
            (if (requireWrite) Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
        if (runCatching { contentResolver.takePersistableUriPermission(uri, flags) }.isSuccess) {
            return true
        }
        status.text = "Folder tidak memberi izin SAF permanen"
        showGtaImgReport(
            "Folder belum dapat dipakai",
            "Pilih folder lewat pemilih Android yang memberi akses permanen read-only" +
                (if (requireWrite) " dan write untuk folder tujuan instalasi." else ".")
        )
        return false
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
        dataSetupTitle = findViewById(R.id.tvDataSetupTitle)
        dataSetupDetail = findViewById(R.id.tvDataSetupDetail)
        profileDetail = findViewById(R.id.tvProfileDetail)
        favoriteButton = findViewById(R.id.btnFavoriteUif)

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
        findViewById<View>(R.id.btnDataSetup).setOnClickListener { showGameDataMenu() }
        findViewById<View>(R.id.btnFavoriteUif).setOnClickListener { connectUifFavorite() }
        findViewById<View>(R.id.btnManualConnect).setOnClickListener { showManualConnectDialog() }
        findViewById<View>(R.id.btnEditProfile).setOnClickListener { showNicknameEditor() }
        updateDashboard()

        // The persisted SAF grant can point at removable SD storage too. Mount
        // it lazily on every process start without copying game files internally.
        restorePersistedGtaTree()
        loadServers()
    }

    private fun loadServers() {
        if (!archiveInstallActive) {
            progress.visibility = View.VISIBLE
            status.text = "Mengambil daftar server dari open.mp…"
        }
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching { ServerListRepository.fetch() }
            }
            result.onSuccess { servers ->
                adapter.submit(servers)
                if (!archiveInstallActive) {
                    progress.visibility = View.GONE
                    status.text = "${servers.size} server ditemukan"
                }
            }.onFailure { e ->
                if (!archiveInstallActive) {
                    progress.visibility = View.GONE
                    status.text = "Gagal memuat: ${e.message}"
                }
            }
        }
    }

    private fun openDetail(server: ServerEntry) {
        ServerDetailDialog.show(this, server)
    }

    private fun updateDashboard() {
        val preferences = getSharedPreferences(GAME_DATA_PREFS, MODE_PRIVATE)
        val treeReady = preferences.getString(KEY_GTA_DATA_TREE_URI, null) != null
        val imgReady = preferences.getString(KEY_GTA_IMG_URI, null) != null
        when {
            treeReady && persistedTreeMountReady == false -> {
                dataSetupTitle.text = "Folder GTA perlu disambungkan ulang"
                dataSetupDetail.text = persistedTreeMountProblem
                    ?: "Grant SAF/SD card yang tersimpan tidak dapat dibuka. Data asli tidak diubah; pilih ulang folder GTA-nya."
            }
            treeReady && persistedTreeMountReady == null -> {
                dataSetupTitle.text = "Memeriksa folder GTA tersimpan…"
                dataSetupDetail.text = "Membuka ulang grant Android SAF secara read-only. Ini penting bila data berada di SD card yang bisa dilepas."
            }
            treeReady -> {
                val digest = preferences.getString(KEY_GTA_ARCHIVE_SHA256, null)
                val verified = preferences.getBoolean(KEY_GTA_ARCHIVE_HASH_VERIFIED, false)
                val worldHint = if (preferences.getBoolean(KEY_GTA_MAP_SOURCES_SYNCED, false)) {
                    " World & collision yang sudah dipilih akan dipulihkan saat aplikasi dibuka."
                } else {
                    " Berikutnya pilih ‘Siapkan world & collision’ agar gameplay memakai source map yang tersedia."
                }
                dataSetupTitle.text = "Data GTA terhubung"
                dataSetupDetail.text = if (digest != null) {
                    "Folder GTA dibaca read-only dari Android SAF/SD card. Arsip tercatat ${if (verified) "terverifikasi SHA-256" else "dengan digest SHA-256"}: ${digest.take(12)}…$worldHint"
                } else {
                    "Folder GTA dipakai read-only lewat Android SAF. Lokasi SD card tetap dipakai langsung, tanpa salin ke memori internal.$worldHint"
                }
            }
            imgReady -> {
                dataSetupTitle.text = "IMG GTA terpasang terbatas"
                dataSetupDetail.text = "Pilih folder GTA lengkap untuk gta.dat, asset pendamping, dan data map/collision dari lokasi SD card."
            }
            else -> {
                dataSetupTitle.text = "Data game belum disiapkan"
                dataSetupDetail.text = "Pilih folder GTA yang sudah diekstrak, pasang ZIP lokal milikmu, atau unduh ZIP dari URL HTTPS milikmu ke SD card / penyimpanan pilihan."
            }
        }
        profileDetail.text = "Nickname: ${PlayerProfile.nickname(this)} · dipakai saat ClientJoin"
        val favorite = ServerFavorites.primary(this)
        favoriteButton.text = "Favorit: ${favorite.label}"
    }

    private fun clearPersistedGameDataSelection() {
        ++gameDataSelectionEpoch
        getSharedPreferences(GAME_DATA_PREFS, MODE_PRIVATE).edit()
            .remove(KEY_GTA_IMG_URI)
            .remove(KEY_GTA_DATA_TREE_URI)
            .remove(KEY_GTA_DATA_TREE_RELATIVE_PATH)
            .remove(KEY_GTA_MAP_SOURCES_SYNCED)
            .remove(KEY_GTA_ARCHIVE_SHA256)
            .remove(KEY_GTA_ARCHIVE_HASH_VERIFIED)
            .apply()
        persistedTreeMountReady = null
        persistedTreeMountProblem = null
        updateDashboard()
    }

    private fun showNicknameEditor() {
        val input = EditText(this).apply {
            setSingleLine(true)
            inputType = InputType.TYPE_CLASS_TEXT
            setText(PlayerProfile.nickname(this@MainActivity))
            setSelection(text.length)
            hint = "3–24 karakter ASCII tanpa spasi"
        }
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Nickname pemain")
            .setMessage("Nama ini dikirim pada handshake ClientJoin. Gunakan 3–24 karakter ASCII terlihat tanpa spasi.")
            .setView(input)
            .setNegativeButton("Batal", null)
            .setPositiveButton("Simpan", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                if (!PlayerProfile.saveNickname(this, input.text?.toString().orEmpty())) {
                    input.error = "Gunakan 3–24 karakter ASCII terlihat tanpa spasi."
                } else {
                    updateDashboard()
                    dialog.dismiss()
                }
            }
        }
        dialog.show()
    }

    private fun showArchiveInstallDialog() {
        if (archiveInstallActive) {
            showGtaImgReport("Instalasi masih berjalan", "Tunggu unduhan atau ekstraksi saat ini selesai dulu.")
            return
        }
        val padding = (20 * resources.displayMetrics.density).toInt()
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, 0, padding, 0)
        }
        val url = EditText(this).apply {
            hint = "https://domain-kamu.example/gta-data.zip"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            setSingleLine(true)
        }
        val checksum = EditText(this).apply {
            hint = "SHA-256 opsional (64 hex)"
            inputType = InputType.TYPE_CLASS_TEXT
            setSingleLine(true)
        }
        container.addView(url)
        container.addView(checksum)
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Unduh & pasang arsip milikmu")
            .setMessage(
                "Masukkan URL ZIP HTTPS yang kamu berhak pakai. Arsip diunduh ke folder pilihan, " +
                    "diekstrak ke folder sementara, diperiksa, lalu baru dicoba dipasang read-only. " +
                    "Tidak ada asset GTA yang dibundel atau di-host aplikasi. SHA-256 sangat disarankan."
            )
            .setView(container)
            .setNegativeButton("Batal", null)
            .setPositiveButton("Pilih folder tujuan", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val rawUrl = url.text?.toString()?.trim().orEmpty()
                val rawHash = checksum.text?.toString()?.trim().orEmpty()
                if (rawUrl.isBlank()) {
                    url.error = "URL HTTPS wajib diisi"
                    return@setOnClickListener
                }
                if (rawHash.isNotBlank() && !rawHash.matches(Regex("^[0-9a-fA-F]{64}$"))) {
                    checksum.error = "SHA-256 harus 64 karakter heksadesimal"
                    return@setOnClickListener
                }
                pendingArchiveInstall = PendingArchiveInstall.Remote(
                    GtaArchiveInstaller.Request(rawUrl, rawHash.ifBlank { null })
                )
                dialog.dismiss()
                // The user deliberately selects an SD-card or internal folder;
                // no broad storage permission or silent internal copy is used.
                archiveDestinationTreePicker.launch(null)
            }
        }
        dialog.show()
    }

    private fun showLocalArchiveInstallDialog() {
        if (archiveInstallActive) {
            showGtaImgReport("Instalasi masih berjalan", "Tunggu unduhan atau ekstraksi saat ini selesai dulu.")
            return
        }
        val checksum = EditText(this).apply {
            hint = "SHA-256 opsional (64 hex)"
            inputType = InputType.TYPE_CLASS_TEXT
            setSingleLine(true)
        }
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Pasang ZIP milikmu yang sudah ada")
            .setMessage(
                "Pilih arsip ZIP GTA yang kamu berhak pakai. Arsip asli hanya dibaca, " +
                    "disalin ke staging di folder tujuan, diekstrak sementara, lalu diperiksa sebelum dicoba dipasang. " +
                    "SHA-256 sangat disarankan bila kamu punya nilai pembanding."
            )
            .setView(checksum)
            .setNegativeButton("Batal", null)
            .setPositiveButton("Pilih ZIP", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val rawHash = checksum.text?.toString()?.trim().orEmpty()
                if (rawHash.isNotBlank() && !rawHash.matches(Regex("^[0-9a-fA-F]{64}$"))) {
                    checksum.error = "SHA-256 harus 64 karakter heksadesimal"
                    return@setOnClickListener
                }
                pendingLocalArchiveSource = PendingLocalArchiveSource(rawHash.ifBlank { null })
                dialog.dismiss()
                gtaArchiveZipPicker.launch(
                    arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream")
                )
            }
        }
        dialog.show()
    }

    private fun installArchiveIntoSelectedTree(request: GtaArchiveInstaller.Request, destination: Uri) {
        startArchiveInstall { installer, onProgress ->
            installer.install(request, destination, onProgress)
        }
    }

    private fun installLocalArchiveIntoSelectedTree(
        request: GtaArchiveInstaller.LocalRequest,
        destination: Uri
    ) {
        startArchiveInstall { installer, onProgress ->
            installer.installLocal(request, destination, onProgress)
        }
    }

    private fun startArchiveInstall(
        install: (GtaArchiveInstaller, (GtaArchiveInstaller.Progress) -> Unit) -> GtaArchiveInstaller.Result
    ) {
        if (archiveInstallActive) return
        ++gameDataSelectionEpoch
        archiveInstallActive = true
        progress.isIndeterminate = true
        progress.visibility = View.VISIBLE
        status.text = "Menyiapkan instalasi arsip di folder pilihan…"
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                install(
                    GtaArchiveInstaller(this@MainActivity),
                    { event ->
                        runOnUiThread {
                            if (!isFinishing && !isDestroyed) {
                                status.text = formatArchiveProgress(event)
                            }
                        }
                    }
                )
            }
            when (result) {
                is GtaArchiveInstaller.Result.Failed -> {
                    archiveInstallActive = false
                    progress.visibility = View.GONE
                    status.text = "Arsip tidak dipasang"
                    showGtaImgReport("Instalasi data dibatalkan", result.message)
                }
                is GtaArchiveInstaller.Result.Installed -> {
                    status.text = "Arsip lolos pemeriksaan awal. Memeriksa GTA runtime…"
                    val mounted = withContext(Dispatchers.IO) {
                        gameDataMutex.withLock {
                            runCatching {
                                mountUserOwnedGtaTree(
                                    result.destinationTreeUri,
                                    rememberTree = true,
                                    rootRelativePath = result.rootRelativePath
                                )
                            }
                        }
                    }
                    archiveInstallActive = false
                    progress.visibility = View.GONE
                    val mountJson = mounted.getOrNull()?.let { raw -> runCatching { JSONObject(raw) }.getOrNull() }
                    if (mountJson?.optBoolean("ok") == true) {
                        getSharedPreferences(GAME_DATA_PREFS, MODE_PRIVATE).edit()
                            .putString(KEY_GTA_ARCHIVE_SHA256, result.archiveSha256)
                            .putBoolean(KEY_GTA_ARCHIVE_HASH_VERIFIED, result.verifiedAgainstExpectedHash)
                            .apply()
                        updateDashboard()
                        val verification = if (result.verifiedAgainstExpectedHash) {
                            "SHA-256 cocok dengan nilai yang kamu masukkan."
                        } else {
                            "SHA-256 dihitung tetapi tidak dibandingkan karena kamu tidak memasukkan nilai pembanding."
                        }
                        val destinationNote = if (result.renamedForUser) {
                            ""
                        } else {
                            "\nProvider penyimpanan tidak mendukung nama folder akhir, tetapi root tervalidasi tetap dipakai dari staging SD/card yang sama."
                        }
                        status.text = "Data GTA terhubung dari folder pilihan"
                        showGtaImgReport(
                            "Data GTA terpasang",
                            "$verification\nDigest: ${result.archiveSha256}\n" +
                                "Entry diekstrak: ${result.extractedFiles} · ${formatBytes(result.extractedBytes)}\n" +
                                "$destinationNote\n" +
                                "Data dibaca langsung dari folder pilihan (termasuk SD card), tidak disalin ke memori internal. " +
                                "Berikutnya pilih ‘Siapkan world & collision’ dari kartu Data GTA agar source map/collision yang tersedia ikut dimuat."
                        )
                    } else {
                        // A structural ZIP check is not proof that native IMG/
                        // gta.dat parsing accepts it. The mount attempt can have
                        // replaced native-only source bindings partway through,
                        // so clear that uncertain runtime and its stale saved
                        // selection rather than claim an older tree is active.
                        withContext(Dispatchers.IO) {
                            gameDataMutex.withLock {
                                runCatching { NativeBridge.nativeGameDataClear() }
                            }
                        }
                        clearPersistedGameDataSelection()
                        status.text = "Ekstraksi selesai, tetapi data belum diintegrasikan"
                        showGtaImgReport(
                            "Data hasil ekstrak belum cocok",
                            "Folder hasil ekstrak tetap ada di penyimpanan pilihan, tetapi tidak dipasang ke aplikasi karena validator runtime menolaknya. " +
                                "Konfigurasi data aktif dilepas agar tidak menampilkan mount lama yang belum tentu masih valid. " +
                                "${mountJson?.optString("error").orEmpty().ifBlank { mounted.exceptionOrNull()?.message ?: "Periksa gta.dat dan GTA3.IMG." }}"
                        )
                    }
                }
            }
        }
    }

    private fun formatArchiveProgress(event: GtaArchiveInstaller.Progress): String = when (event.phase) {
        GtaArchiveInstaller.Progress.Phase.DOWNLOADING -> {
            val total = event.totalBytes.takeIf { it > 0 }?.let { " / ${formatBytes(it)}" }.orEmpty()
            "${event.detail.ifBlank { "Memindahkan arsip…" }} ${formatBytes(event.completedBytes)}$total"
        }
        GtaArchiveInstaller.Progress.Phase.VERIFYING -> event.detail.ifBlank { "Memverifikasi SHA-256 arsip…" }
        GtaArchiveInstaller.Progress.Phase.EXTRACTING ->
            "Mengekstrak ${event.entries} entry · ${formatBytes(event.completedBytes)}"
        GtaArchiveInstaller.Progress.Phase.VALIDATING -> "Memeriksa struktur GTA hasil ekstrak…"
        GtaArchiveInstaller.Progress.Phase.FINALIZING -> "Menyiapkan data GTA yang tervalidasi…"
    }

    private fun connectUifFavorite() {
        // UIF is the first-run convenience candidate. A player can replace it
        // from the manual-address flow; either way the endpoint is queried
        // before the session UI opens, so no saved listing is treated as live
        // proof of reachability or compatibility.
        val favorite = ServerFavorites.primary(this)
        resolveAndOpenServer(favorite.host, favorite.port, favorite.label)
    }

    private fun showManualConnectDialog() {
        val input = EditText(this).apply {
            hint = "host:port  (contoh: play.example.net:7777)"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            setSingleLine(true)
        }
        val favorite = CheckBox(this).apply {
            text = "Simpan sebagai favorit utama"
            isChecked = false
        }
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val padding = (20 * resources.displayMetrics.density).toInt()
            setPadding(padding, 0, padding, 0)
            addView(input)
            addView(favorite)
        }
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Hubungkan alamat server")
            .setMessage("Masukkan hostname/IPv4 dan port SA-MP. Aplikasi akan melakukan query UDP lebih dulu sebelum membuka layar sesi.")
            .setView(container)
            .setNegativeButton("Batal", null)
            .setPositiveButton("Cek server", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(androidx.appcompat.app.AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val endpoint = parseEndpoint(input.text?.toString().orEmpty())
                if (endpoint == null) {
                    input.error = "Gunakan hostname atau IPv4, lalu :port (1–65535)."
                } else {
                    dialog.dismiss()
                    resolveAndOpenServer(
                        endpoint.first,
                        endpoint.second,
                        endpoint.first,
                        saveAsPrimaryFavorite = favorite.isChecked
                    )
                }
            }
        }
        dialog.show()
    }

    private fun parseEndpoint(raw: String): Pair<String, Int>? {
        val value = raw.trim()
        val separator = value.lastIndexOf(':')
        if (separator <= 0 || separator == value.lastIndex || value.indexOf(':') != separator) return null
        val host = value.substring(0, separator).trim()
        val port = value.substring(separator + 1).toIntOrNull()
        return host.takeIf { it.isNotBlank() && it.length <= 253 }
            ?.let { safeHost -> port?.takeIf { it in 1..65_535 }?.let { safeHost to it } }
    }

    private fun resolveAndOpenServer(
        host: String,
        port: Int,
        fallbackName: String,
        saveAsPrimaryFavorite: Boolean = false
    ) {
        progress.visibility = View.VISIBLE
        status.text = "Memeriksa $host:$port…"
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    val ipv4 = InetAddress.getAllByName(host)
                        .filterIsInstance<Inet4Address>()
                        .firstOrNull()
                        ?: error("Hostname tidak memberi alamat IPv4; transport legacy saat ini memakai IPv4.")
                    val info = JSONObject(NativeBridge.nativeQueryInfo(ipv4.hostAddress, port, 2_500))
                    if (!info.optBoolean("ok")) error(info.optString("error").ifBlank { "Query server gagal" })
                    ServerEntry(
                        ip = ipv4.hostAddress,
                        port = port,
                        hostname = info.optString("hostname").ifBlank { fallbackName },
                        players = info.optInt("players"),
                        maxPlayers = info.optInt("maxPlayers"),
                        gamemode = info.optString("gamemode"),
                        language = info.optString("language"),
                        passworded = info.optBoolean("password"),
                        isOpenMp = false,
                        version = info.optString("version")
                    )
                }
            }
            progress.visibility = View.GONE
            result.onFailure { failure ->
                status.text = "Server belum dapat dijangkau"
                showGtaImgReport("Query server gagal", failure.message ?: "Tidak ada respons query UDP.")
            }.onSuccess { server ->
                if (saveAsPrimaryFavorite) {
                    // Preserve a safe player-provided label rather than trusting
                    // arbitrary server MOTD/colour markup as preferences UI.
                    ServerFavorites.savePrimary(this@MainActivity, host, port, fallbackName)
                    updateDashboard()
                }
                status.text = "Server merespons query. Pilih koneksi native di layar berikutnya."
                openDetail(server)
            }
        }
    }

    private fun inspectUserOwnedImg(uri: Uri) {
        ++gameDataSelectionEpoch
        // OpenDocument normally grants this. Persisting is best effort because
        // some providers intentionally offer a session-only descriptor.
        runCatching {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        status.text = "Memeriksa indeks IMG milik kamu secara read-only…"
        lifecycleScope.launch {
            val report = withContext(Dispatchers.IO) {
                gameDataMutex.withLock {
                    runCatching {
                        contentResolver.openFileDescriptor(uri, "r")?.use { descriptor ->
                            NativeBridge.nativeGameDataMountImgFile(descriptor.fd)
                        } ?: error("File yang dipilih tidak memberi file descriptor")
                    }
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
                    .remove(KEY_GTA_DATA_TREE_RELATIVE_PATH)
                    .remove(KEY_GTA_MAP_SOURCES_SYNCED)
                    .remove(KEY_GTA_ARCHIVE_SHA256)
                    .remove(KEY_GTA_ARCHIVE_HASH_VERIFIED)
                    .apply()
                persistedTreeMountReady = null
                persistedTreeMountProblem = null
                updateDashboard()
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

    private fun mountUserOwnedGtaTree(
        uri: Uri,
        rememberTree: Boolean,
        rootRelativePath: String? = null
    ): String {
        var tree = GtaDataTreeResolver.fromTreeUri(this, uri)
            ?: error("Folder SAF yang dipilih tidak dapat dibaca")
        if (!rootRelativePath.isNullOrBlank()) {
            tree = tree.subdirectory(rootRelativePath)
                ?: error("Folder data hasil ekstrak sudah tidak dapat dibaca dari grant SAF.")
        }
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

        // handling.cfg stays in the selected tree/SD card and is parsed only
        // as a bounded read-only source for the local driver envelope. Its
        // cars.ide model join arrives later during explicit world sync; an
        // absent file keeps the safe generic vehicle fallback for minimal
        // custom trees, while a present malformed file is reported honestly.
        tree.firstFile(listOf("data/handling.cfg", "handling.cfg"))?.let { handling ->
            finalStatus = contentResolver.openFileDescriptor(handling.uri, "r")?.use { descriptor ->
                NativeBridge.nativeGameDataLoadHandlingFile(descriptor.fd)
            } ?: error("handling.cfg tidak memberi descriptor read-only")
            if (!runCatching { JSONObject(finalStatus).optBoolean("ok") }.getOrDefault(false)) {
                return finalStatus
            }
        }

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
                .apply {
                    if (rootRelativePath.isNullOrBlank()) remove(KEY_GTA_DATA_TREE_RELATIVE_PATH)
                    else putString(KEY_GTA_DATA_TREE_RELATIVE_PATH, rootRelativePath)
                }
                .remove(KEY_GTA_IMG_URI)
                // Mounting deliberately clears native map sources. They become
                // persistent only after the player explicitly syncs this tree.
                .remove(KEY_GTA_MAP_SOURCES_SYNCED)
                // A manually selected tree has no archive provenance record.
                // The downloader writes a new record only after its native
                // mount below succeeds.
                .remove(KEY_GTA_ARCHIVE_SHA256)
                .remove(KEY_GTA_ARCHIVE_HASH_VERIFIED)
                .apply()
            runOnUiThread {
                persistedTreeMountReady = true
                persistedTreeMountProblem = null
                updateDashboard()
            }
        }
        return finalStatus
    }

    private fun inspectUserOwnedGtaTree(uri: Uri) {
        ++gameDataSelectionEpoch
        persistedTreeMountReady = null
        persistedTreeMountProblem = null
        updateDashboard()
        runGameDataOperation(
            loadingLabel = "Membaca folder GTA hasil ekstrak secara read-only",
            successTitle = "Folder GTA terhubung",
            successIntro = "gta.dat dan IMG dibuka read-only dari folder yang kamu pilih. " +
                "Pilih ‘Siapkan world & collision’ setelah ini untuk memuat source map yang tersedia.",
            onCompleted = { mounted ->
                if (!mounted) {
                    // A failed replacement can leave the native source state
                    // uncertain; never keep presenting a previous SAF record
                    // as a live game mount. The original files remain intact.
                    persistedTreeMountReady = false
                    persistedTreeMountProblem = "Folder GTA yang baru dipilih belum dapat dipasang. " +
                        "Data asli tidak diubah; pilih ulang folder GTA yang valid."
                    updateDashboard()
                }
            }
        ) {
            mountUserOwnedGtaTree(uri, rememberTree = true)
        }
    }

    private fun restorePersistedGtaTree() {
        val preferences = getSharedPreferences(GAME_DATA_PREFS, MODE_PRIVATE)
        val uri = preferences.getString(KEY_GTA_DATA_TREE_URI, null)
            ?.let(Uri::parse)
            ?: return
        val restoreMapSources = preferences.getBoolean(KEY_GTA_MAP_SOURCES_SYNCED, false)
        val rootRelativePath = preferences.getString(KEY_GTA_DATA_TREE_RELATIVE_PATH, null)
        val restoreEpoch = gameDataSelectionEpoch
        lifecycleScope.launch {
            val restored = withContext(Dispatchers.IO) {
                gameDataMutex.withLock {
                    val selectedUri = preferences.getString(KEY_GTA_DATA_TREE_URI, null)
                    val selectedRelativePath = preferences.getString(KEY_GTA_DATA_TREE_RELATIVE_PATH, null)
                    if (gameDataSelectionEpoch != restoreEpoch || selectedUri != uri.toString() ||
                        selectedRelativePath != rootRelativePath) {
                        return@withLock null
                    }
                    val result = runCatching {
                        mountUserOwnedGtaTree(uri, rememberTree = false, rootRelativePath = rootRelativePath)
                    }
                    val rawStatus = result.getOrNull()
                    val mounted = rawStatus
                        ?.let { status -> runCatching { JSONObject(status).optBoolean("ok") }.getOrDefault(false) }
                        ?: false
                    // Recreate the native-only map assembly after process death.
                    // A stale SD-card grant or malformed source is left recoverable
                    // through the explicit Data GTA menu rather than crashing startup.
                    val mapSourcesNeedSetup = if (mounted && restoreMapSources) {
                        val sourceStatus = runCatching { synchronizeMapSourcesFromTree(uri) }
                        val sourcesRestored = sourceStatus.getOrNull()
                            ?.let { status -> runCatching { JSONObject(status).optBoolean("ok") }.getOrDefault(false) }
                            ?: false
                        if (!sourcesRestored) {
                            // A new mount clears native map bindings. Do not retain
                            // a persisted promise to restore them if this particular
                            // SD-card/provider session cannot rebuild the sources.
                            preferences.edit().remove(KEY_GTA_MAP_SOURCES_SYNCED).apply()
                        }
                        !sourcesRestored
                    } else {
                        false
                    }
                    val problem = if (mounted) null else {
                        rawStatus?.let { status ->
                            runCatching { JSONObject(status).optString("error") }.getOrNull()
                        }?.takeIf { it.isNotBlank() }
                            ?: result.exceptionOrNull()?.message
                            ?: "Grant SAF/SD card tidak dapat dibuka. Pilih ulang folder GTA yang sama."
                    }
                    PersistedTreeRestore(mounted, problem, mapSourcesNeedSetup)
                }
            }
            // A player action superseded this startup restore while it waited
            // for the serialized native runtime. Leave its newer dashboard and
            // mount result alone.
            if (restored == null) return@launch
            persistedTreeMountReady = restored.mounted
            persistedTreeMountProblem = restored.problem
            updateDashboard()
            when {
                !restored.mounted && !archiveInstallActive -> {
                    status.text = "Folder GTA tersimpan tidak dapat dibuka"
                }
                restored.mapSourcesNeedSetup && !archiveInstallActive -> {
                    status.text = "Folder GTA terhubung; siapkan world & collision lagi"
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
        var tree = GtaDataTreeResolver.fromTreeUri(this, treeUri)
            ?: error("Grant folder GTA sudah tidak dapat dibaca; pilih ulang foldernya")
        val rootRelativePath = getSharedPreferences(GAME_DATA_PREFS, MODE_PRIVATE)
            .getString(KEY_GTA_DATA_TREE_RELATIVE_PATH, null)
        if (!rootRelativePath.isNullOrBlank()) {
            tree = tree.subdirectory(rootRelativePath)
                ?: error("Folder data hasil ekstrak sudah tidak dapat dibaca dari grant SAF.")
        }
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
            try {
                val status = synchronizeMapSourcesFromTree(treeUri)
                val synchronized = runCatching { JSONObject(status).optBoolean("ok") }.getOrDefault(false)
                preferences.edit().apply {
                    if (synchronized) putBoolean(KEY_GTA_MAP_SOURCES_SYNCED, true)
                    else remove(KEY_GTA_MAP_SOURCES_SYNCED)
                }.apply()
                runOnUiThread { updateDashboard() }
                status
            } catch (error: Throwable) {
                // Synchronization clears/rebuilds native map sources. If the
                // provider disappears or one source fails mid-path, do not
                // restore a stale "synced" promise on the next app launch.
                preferences.edit().remove(KEY_GTA_MAP_SOURCES_SYNCED).apply()
                runOnUiThread { updateDashboard() }
                throw error
            }
        }
    }

    private fun JSONObject.jsonPathList(key: String): List<String> {
        val array = optJSONArray(key) ?: return emptyList()
        return List(array.length()) { index -> array.optString(index) }
            .filter { it.isNotBlank() }
    }

    private fun showGameDataMenu() {
        if (archiveInstallActive) {
            showGtaImgReport("Instalasi masih berjalan", "Tunggu unduhan atau ekstraksi arsip selesai dulu.")
            return
        }
        val actions = arrayOf(
            "Unduh & pasang arsip ZIP dari URL milikmu",
            "Pasang arsip ZIP yang sudah ada di perangkat",
            "Hubungkan folder GTA hasil ekstrak",
            "Siapkan world & collision dari folder terhubung",
            "Lihat status data game",
            "Lepas data game dari aplikasi",
            "Alat data lanjutan"
        )
        MaterialAlertDialogBuilder(this)
            .setTitle("Siapkan data GTA milikmu")
            .setMessage(
                "Pilih folder GTA yang sudah diekstrak, pasang ZIP lokal milikmu, atau gunakan URL HTTPS yang kamu kontrol. " +
                    "Data akhir tetap dibaca lewat Android SAF dari folder pilihan/SD card; APK tidak membundel asset GTA, " +
                    "tidak menyalin tree ke memori internal, dan tidak menjalankan RAR/EXE PC."
            )
            .setItems(actions) { _, selected ->
                when (selected) {
                    0 -> showArchiveInstallDialog()
                    1 -> showLocalArchiveInstallDialog()
                    2 -> gtaDataTreePicker.launch(null)
                    3 -> loadMapSourcesFromSelectedTree()
                    4 -> runGameDataOperation("Membaca status data game") {
                        NativeBridge.nativeGameDataStatus()
                    }
                    5 -> confirmClearGameData()
                    6 -> showAdvancedGameDataMenu()
                }
            }
            .setNegativeButton("Batal", null)
            .show()
    }

    private fun confirmClearGameData() {
        MaterialAlertDialogBuilder(this)
            .setTitle("Lepas data game?")
            .setMessage(
                "Aplikasi akan melepas mount/runtime dan konfigurasi SAF-nya. Folder maupun file GTA asli di penyimpananmu tidak dihapus."
            )
            .setNegativeButton("Batal", null)
            .setPositiveButton("Lepas") { _, _ ->
                clearPersistedGameDataSelection()
                runGameDataOperation("Melepas data GTA runtime") {
                    NativeBridge.nativeGameDataClear()
                }
            }
            .show()
    }

    private fun showAdvancedGameDataMenu() {
        val actions = arrayOf(
            "Pasang / ganti GTA3.IMG",
            "Tambah file COL collision",
            "Tambah file IDE object",
            "Tambah file IPL (text / bnry)",
            "Load member .col dari IMG",
            "Load member .dff dari IMG",
            "Load member .txd dari IMG"
        )
        MaterialAlertDialogBuilder(this)
            .setTitle("Alat data lanjutan")
            .setMessage(
                "Ini untuk troubleshooting atau source manual. Untuk main normal, pakai folder GTA lalu ‘Siapkan world & collision’ di menu sebelumnya.")
            .setItems(actions) { _, selected ->
                when (selected) {
                    0 -> gtaImgPicker.launch(arrayOf("*/*"))
                    1 -> gtaColPicker.launch(arrayOf("*/*"))
                    2 -> gtaIdePicker.launch(arrayOf("*/*"))
                    3 -> gtaIplPicker.launch(arrayOf("*/*"))
                    4 -> promptImgCollisionMember()
                    5 -> promptImgDffMember()
                    6 -> promptImgTxdMember()
                }
            }
            .setNegativeButton("Kembali", null)
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

    private fun runGameDataOperation(
        loadingLabel: String,
        successTitle: String = "World data runtime diperbarui",
        successIntro: String? = null,
        onCompleted: ((Boolean) -> Unit)? = null,
        operation: () -> String
    ) {
        status.text = "$loadingLabel…"
        lifecycleScope.launch {
            val response = withContext(Dispatchers.IO) {
                gameDataMutex.withLock { runCatching(operation) }
            }
            response.onFailure {
                onCompleted?.invoke(false)
                status.text = "World data tidak bisa diperbarui"
                showGtaImgReport(
                    "Data GTA belum bisa dipakai",
                    "Android atau file yang dipilih tidak memberi data read-only yang valid."
                )
            }.onSuccess { rawJson ->
                val json = runCatching { JSONObject(rawJson) }.getOrNull()
                if (json == null) {
                    onCompleted?.invoke(false)
                    status.text = "Respons world data tidak valid"
                    showGtaImgReport("Data GTA belum bisa dipakai", "Respons runtime native tidak valid.")
                    return@onSuccess
                }
                val error = json.optString("error")
                if (!json.optBoolean("ok")) {
                    onCompleted?.invoke(false)
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
                val handlingProfiles = json.optInt("handlingProfiles")
                val placements = json.optInt("iplPlacements")
                val instances = json.optInt("collisionInstances")
                val missing = json.optInt("missingCollisionPlacements")
                onCompleted?.invoke(true)
                status.text = "World data: $models COL · $instances placement collision"
                showGtaImgReport(
                    successTitle,
                    successIntro?.let { "$it\n\n" }.orEmpty() +
                        "IMG utama entry: $entries · IMG tambahan: $additionalImgArchives ($additionalImgEntries entry, ${formatBytes(additionalImgBytes)})\n" +
                        "COL model: $models · IDE object: $ide · profile handling: $handlingProfiles\n" +
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
        private const val KEY_GTA_DATA_TREE_RELATIVE_PATH = "gta_data_tree_relative_path"
        private const val KEY_GTA_MAP_SOURCES_SYNCED = "gta_map_sources_synced"
        private const val KEY_GTA_ARCHIVE_SHA256 = "gta_archive_sha256"
        private const val KEY_GTA_ARCHIVE_HASH_VERIFIED = "gta_archive_hash_verified"
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
