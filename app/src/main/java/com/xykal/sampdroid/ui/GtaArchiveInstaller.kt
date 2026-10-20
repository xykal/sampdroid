package com.xykal.sampdroid.ui

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.net.URI
import java.net.URL
import java.security.MessageDigest
import java.util.Locale
import java.util.UUID
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import javax.net.ssl.HttpsURLConnection

/**
 * Installs a player-authorised ZIP into a player-selected SAF tree. This is
 * intentionally not an asset downloader/catalog: the user supplies either an
 * HTTPS URL they control or an already-selected local ZIP, optional expected
 * SHA-256, and removable-storage destination. Nothing is bundled, hosted, or
 * copied from an already-selected original GTA tree.
 */
internal class GtaArchiveInstaller(private val context: Context) {

    data class Request(
        val url: String,
        val expectedSha256: String? = null
    )

    data class Progress(
        val phase: Phase,
        val completedBytes: Long = 0L,
        val totalBytes: Long = -1L,
        val entries: Int = 0,
        val detail: String
    ) {
        enum class Phase { DOWNLOADING, VERIFYING, EXTRACTING, VALIDATING, FINALIZING }
    }

    sealed interface Result {
        data class Installed(
            /** Original SAF tree grant root selected by the player. */
            val destinationTreeUri: Uri,
            /** Bounded child path under destinationTreeUri that is the GTA root. */
            val rootRelativePath: String,
            val archiveSha256: String,
            val verifiedAgainstExpectedHash: Boolean,
            val extractedFiles: Int,
            val extractedBytes: Long,
            val renamedForUser: Boolean
        ) : Result

        data class Failed(val message: String) : Result
    }

    data class LocalRequest(
        /** Existing player-selected ZIP. It is read and copied; never modified. */
        val sourceUri: Uri,
        val expectedSha256: String? = null
    )

    fun install(
        request: Request,
        destinationTreeUri: Uri,
        onProgress: (Progress) -> Unit
    ): Result {
        val expectedHash = runCatching { normalizedExpectedHash(request.expectedSha256) }
            .getOrElse { return Result.Failed(it.message ?: "SHA-256 tidak valid") }
        val validatedUrl = runCatching { validatedHttpsUrl(request.url) }
            .getOrElse { return Result.Failed(it.message ?: "URL HTTPS tidak valid") }
        return installIntoDestination(expectedHash, destinationTreeUri, onProgress) { archive ->
            downloadArchive(validatedUrl, archive, onProgress)
        }
    }

    /**
     * Installs an already downloaded user-selected ZIP. The source document is
     * opened read-only and copied into the same SAF staging tree as HTTPS input,
     * so extraction never alters the original archive or consumes app-internal
     * storage as a hidden second GTA copy.
     */
    fun installLocal(
        request: LocalRequest,
        destinationTreeUri: Uri,
        onProgress: (Progress) -> Unit
    ): Result {
        val expectedHash = runCatching { normalizedExpectedHash(request.expectedSha256) }
            .getOrElse { return Result.Failed(it.message ?: "SHA-256 tidak valid") }
        return installIntoDestination(expectedHash, destinationTreeUri, onProgress) { archive ->
            copyLocalArchive(request.sourceUri, archive, onProgress)
        }
    }

    private fun normalizedExpectedHash(raw: String?): String? {
        val expectedHash = raw?.trim()?.takeIf { it.isNotEmpty() }
        if (expectedHash != null && !SHA_256.matches(expectedHash)) {
            throw InstallFailure("SHA-256 harus tepat 64 karakter heksadesimal, atau dikosongkan jika belum punya.")
        }
        return expectedHash
    }

    private fun installIntoDestination(
        expectedHash: String?,
        destinationTreeUri: Uri,
        onProgress: (Progress) -> Unit,
        acquireArchive: (DocumentFile) -> Downloaded
    ): Result {
        val root = DocumentFile.fromTreeUri(context, destinationTreeUri)
            ?.takeIf { it.isDirectory && it.canWrite() }
            ?: return Result.Failed("Folder tujuan tidak dapat ditulis. Pilih folder SD/internal lewat pemilih Android.")

        // All mutable work occurs below this single temporary directory. It is
        // removed on a failed hash/extraction/structure check, leaving no
        // partially integrated GTA root behind.
        val staging = root.createDirectory("SampDroid-install-${UUID.randomUUID()}")
            ?: return Result.Failed("Provider penyimpanan tidak bisa membuat folder sementara.")
        var installed = false
        try {
            // Keep the copied input outside the extraction root. A ZIP can
            // legitimately contain a file called download.zip, so sharing a
            // directory would let an entry collide with our temporary input.
            val inputDirectory = staging.createDirectory("SampDroid-input")
                ?: return Result.Failed("Provider penyimpanan tidak bisa membuat folder input sementara.")
            val payloadDirectory = staging.createDirectory("SampDroid-payload")
                ?: return Result.Failed("Provider penyimpanan tidak bisa membuat folder ekstraksi sementara.")
            val archive = inputDirectory.createFile("application/zip", "archive.zip")
                ?: return Result.Failed("Provider penyimpanan tidak bisa membuat file input sementara.")
            val acquired = acquireArchive(archive)
            val actualHash = acquired.sha256
            if (expectedHash != null && !actualHash.equals(expectedHash, ignoreCase = true)) {
                return Result.Failed(
                    "SHA-256 tidak cocok. Arsip tidak dipasang.\nDiharapkan: $expectedHash\nDidapat: $actualHash"
                )
            }

            val extraction = extractZip(archive, payloadDirectory, onProgress)
            // The temporary input is never altered. Once its contents have
            // passed extraction, remove only our staging copy so the mounted
            // root contains extracted data, not the original ZIP.
            if (!archive.delete()) return Result.Failed("Arsip sementara tidak bisa dihapus dari folder staging.")
            runCatching { inputDirectory.delete() }

            onProgress(Progress(Progress.Phase.VALIDATING, extraction.bytes, extraction.bytes,
                extraction.files, "Memeriksa struktur GTA hasil ekstrak…"))
            val gameRoot = findGameRoot(payloadDirectory)
                ?: return Result.Failed(
                    "Struktur hasil ekstrak tidak cocok: perlu data/gta.dat dan models/gta3.img di satu folder GTA."
                )
            if (!isNonEmptyFile(gameRoot, "data", "gta.dat") ||
                !hasImgSignature(gameRoot, "models", "gta3.img")) {
                return Result.Failed("gta.dat atau GTA3.IMG tidak lolos pemeriksaan awal. Tidak ada data yang dipasang.")
            }

            onProgress(Progress(Progress.Phase.FINALIZING, extraction.bytes, extraction.bytes,
                extraction.files, "Menyiapkan folder data yang sudah tervalidasi…"))
            // Compute the path while the document still has its known original
            // name. Several SAF providers return true from renameTo() while
            // keeping this DocumentFile instance's cached name/URI stale; a
            // path reconstructed afterwards could therefore point back at the
            // no-longer-existing pre-rename folder on the next app launch.
            val originalRelativePath = relativePath(root, gameRoot)
                ?: return Result.Failed("Provider penyimpanan tidak memberi jalur aman untuk folder hasil ekstrak.")
            val finalName = "SampDroid-GTA-${System.currentTimeMillis()}"
            val renamed = gameRoot.renameTo(finalName)
            val rootRelativePath = if (renamed) {
                renamedLeafPath(originalRelativePath, finalName)
                    ?: return Result.Failed("Provider penyimpanan tidak memberi jalur final yang aman.")
            } else {
                originalRelativePath
            }
            // Do not trust a provider's boolean alone: verify the exact
            // persisted path by resolving it from the original tree grant.
            // If it cannot be reopened now, fail transactionally instead of
            // remembering a tree the next launch cannot mount.
            if (findChildPath(root, *rootRelativePath.split('/').toTypedArray())?.isDirectory != true) {
                return Result.Failed("Folder data akhir tidak dapat diverifikasi ulang pada provider penyimpanan.")
            }
            // A ZIP may contain one top-level GTA folder. Do not delete its
            // staging parent here: SAF providers can recursively delete a
            // non-empty directory, which would also erase the validated child.
            // Keep a relative child path under the original persisted tree
            // grant; a child DocumentFile URI cannot be reopened as a tree root
            // by itself after process death.

            installed = true
            return Result.Installed(
                destinationTreeUri = destinationTreeUri,
                rootRelativePath = rootRelativePath,
                archiveSha256 = actualHash,
                verifiedAgainstExpectedHash = expectedHash != null,
                extractedFiles = extraction.files,
                extractedBytes = extraction.bytes,
                renamedForUser = renamed
            )
        } catch (error: InstallFailure) {
            return Result.Failed(error.message ?: "Instalasi arsip gagal")
        } catch (error: Throwable) {
            return Result.Failed("Instalasi arsip gagal: ${error.message ?: error.javaClass.simpleName}")
        } finally {
            // Every failure path above is contained in the staging directory;
            // remove it rather than leaving an unvalidated partial GTA tree.
            // A successful direct-root install may retain the staging document
            // if its provider does not implement renameTo, so preserve it only
            // after structural validation has completed.
            if (!installed) deleteTreeBestEffort(staging)
        }
    }

    private data class Downloaded(val sha256: String)
    private data class Extraction(val files: Int, val bytes: Long)

    private fun downloadArchive(
        url: URL,
        archive: DocumentFile,
        onProgress: (Progress) -> Unit
    ): Downloaded {
        val connection = openFollowingHttpsRedirects(url)
        try {
            val declaredLength = connection.contentLengthLong
            if (declaredLength > MAX_ARCHIVE_BYTES) {
                throw InstallFailure("Arsip terlalu besar (${formatBytes(declaredLength)}). Batas aman unduhan adalah ${formatBytes(MAX_ARCHIVE_BYTES)}.")
            }
            if (connection.responseCode !in 200..299) {
                throw InstallFailure("Server memberi HTTP ${connection.responseCode}; unduhan tidak dimulai.")
            }
            val digest = MessageDigest.getInstance("SHA-256")
            connection.inputStream.use { rawInput ->
                BufferedInputStream(rawInput, BUFFER_SIZE).use { input ->
                    context.contentResolver.openOutputStream(archive.uri, "w")?.use { rawOutput ->
                        BufferedOutputStream(rawOutput, BUFFER_SIZE).use { output ->
                            val buffer = ByteArray(BUFFER_SIZE)
                            var total = 0L
                            var lastUpdate = 0L
                            while (true) {
                                val read = input.read(buffer)
                                if (read < 0) break
                                total += read
                                if (total > MAX_ARCHIVE_BYTES) {
                                    throw InstallFailure("Unduhan melewati batas aman ${formatBytes(MAX_ARCHIVE_BYTES)}.")
                                }
                                digest.update(buffer, 0, read)
                                output.write(buffer, 0, read)
                                if (total - lastUpdate >= PROGRESS_GRANULARITY_BYTES) {
                                    lastUpdate = total
                                    onProgress(Progress(
                                        phase = Progress.Phase.DOWNLOADING,
                                        completedBytes = total,
                                        totalBytes = declaredLength,
                                        detail = "Mengunduh arsip ke penyimpanan pilihan…"
                                    ))
                                }
                            }
                            output.flush()
                            onProgress(Progress(
                                phase = Progress.Phase.VERIFYING,
                                completedBytes = total,
                                totalBytes = declaredLength,
                                detail = "Memverifikasi SHA-256 arsip…"
                            ))
                        }
                    } ?: throw InstallFailure("Provider penyimpanan tidak membuka file sementara untuk ditulis.")
                }
            }
            return Downloaded(digest.digest().joinToString("") { byte ->
                "%02x".format(byte.toInt() and 0xff)
            })
        } finally {
            connection.disconnect()
        }
    }

    private fun copyLocalArchive(
        sourceUri: Uri,
        archive: DocumentFile,
        onProgress: (Progress) -> Unit
    ): Downloaded {
        val declaredLength = runCatching {
            context.contentResolver.openAssetFileDescriptor(sourceUri, "r")?.use { it.length }
        }.getOrNull() ?: -1L
        if (declaredLength > MAX_ARCHIVE_BYTES) {
            throw InstallFailure(
                "Arsip lokal terlalu besar (${formatBytes(declaredLength)}). Batas aman adalah ${formatBytes(MAX_ARCHIVE_BYTES)}."
            )
        }
        val digest = MessageDigest.getInstance("SHA-256")
        val input = context.contentResolver.openInputStream(sourceUri)
            ?: throw InstallFailure("Arsip ZIP lokal tidak dapat dibaca dari Android SAF.")
        input.use { rawInput ->
            BufferedInputStream(rawInput, BUFFER_SIZE).use { bufferedInput ->
                context.contentResolver.openOutputStream(archive.uri, "w")?.use { rawOutput ->
                    BufferedOutputStream(rawOutput, BUFFER_SIZE).use { output ->
                        val buffer = ByteArray(BUFFER_SIZE)
                        var total = 0L
                        var lastUpdate = 0L
                        while (true) {
                            val read = bufferedInput.read(buffer)
                            if (read < 0) break
                            total += read
                            if (total > MAX_ARCHIVE_BYTES) {
                                throw InstallFailure("Arsip lokal melewati batas aman ${formatBytes(MAX_ARCHIVE_BYTES)}.")
                            }
                            digest.update(buffer, 0, read)
                            output.write(buffer, 0, read)
                            if (total - lastUpdate >= PROGRESS_GRANULARITY_BYTES) {
                                lastUpdate = total
                                onProgress(Progress(
                                    phase = Progress.Phase.DOWNLOADING,
                                    completedBytes = total,
                                    totalBytes = declaredLength,
                                    detail = "Menyalin arsip ZIP lokal ke folder sementara…"
                                ))
                            }
                        }
                        output.flush()
                        onProgress(Progress(
                            phase = Progress.Phase.VERIFYING,
                            completedBytes = total,
                            totalBytes = declaredLength,
                            detail = "Memverifikasi SHA-256 arsip lokal…"
                        ))
                    }
                } ?: throw InstallFailure("Provider penyimpanan tidak membuka file sementara untuk ditulis.")
            }
        }
        return Downloaded(digest.digest().joinToString("") { byte ->
            "%02x".format(byte.toInt() and 0xff)
        })
    }

    private fun extractZip(
        archive: DocumentFile,
        staging: DocumentFile,
        onProgress: (Progress) -> Unit
    ): Extraction {
        var files = 0
        var totalBytes = 0L
        val seen = HashSet<String>()
        context.contentResolver.openInputStream(archive.uri)?.use { raw ->
            ZipInputStream(BufferedInputStream(raw, BUFFER_SIZE)).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    try {
                        if (++files > MAX_ENTRY_COUNT) throw InstallFailure("ZIP memiliki terlalu banyak entry.")
                        val path = sanitizeZipPath(entry)
                        if (!seen.add(path.lowercase(Locale.ROOT))) {
                            throw InstallFailure("ZIP memiliki entry duplikat/bertabrakan: $path")
                        }
                        if (entry.size > MAX_ENTRY_BYTES) {
                            throw InstallFailure("Satu file ZIP terlalu besar: $path")
                        }
                        val parentSegments = path.split('/')
                        val target = if (entry.isDirectory) {
                            ensureDirectory(staging, parentSegments)
                        } else {
                            val parent = ensureDirectory(staging, parentSegments.dropLast(1))
                            createFreshFile(parent, parentSegments.last())
                        }
                        if (!entry.isDirectory) {
                            context.contentResolver.openOutputStream(target.uri, "w")?.use { rawOutput ->
                                BufferedOutputStream(rawOutput, BUFFER_SIZE).use { output ->
                                    val buffer = ByteArray(BUFFER_SIZE)
                                    var entryBytes = 0L
                                    while (true) {
                                        val read = zip.read(buffer)
                                        if (read < 0) break
                                        entryBytes += read
                                        totalBytes += read
                                        if (entryBytes > MAX_ENTRY_BYTES || totalBytes > MAX_EXTRACTED_BYTES) {
                                            throw InstallFailure("Ekstraksi melewati batas aman ukuran data.")
                                        }
                                        output.write(buffer, 0, read)
                                    }
                                    output.flush()
                                    val compressed = entry.compressedSize
                                    if (compressed > 0L && entryBytes > compressed * MAX_COMPRESSION_RATIO + ZIP_RATIO_GRACE_BYTES) {
                                        throw InstallFailure("ZIP terindikasi compression bomb pada $path")
                                    }
                                }
                            } ?: throw InstallFailure("Tidak bisa menulis entry ZIP: $path")
                        }
                        if (files % EXTRACTION_PROGRESS_EVERY_ENTRIES == 0) {
                            onProgress(Progress(
                                phase = Progress.Phase.EXTRACTING,
                                completedBytes = totalBytes,
                                entries = files,
                                detail = "Mengekstrak file $files ke folder sementara…"
                            ))
                        }
                    } finally {
                        zip.closeEntry()
                    }
                }
            }
        } ?: throw InstallFailure("Arsip sementara tidak dapat dibaca ulang.")
        if (files == 0) throw InstallFailure("ZIP kosong.")
        return Extraction(files, totalBytes)
    }

    private fun findGameRoot(staging: DocumentFile): DocumentFile? {
        val candidates = ArrayList<DocumentFile>()
        candidates += staging
        staging.listFiles().filter { it.isDirectory }.take(MAX_ROOT_CANDIDATES).forEach { child ->
            candidates += child
            child.listFiles().filter { it.isDirectory }.take(MAX_ROOT_CANDIDATES).forEach { grandchild ->
                candidates += grandchild
            }
        }
        return candidates.firstOrNull { root ->
            isNonEmptyFile(root, "data", "gta.dat") && findChildPath(root, "models", "gta3.img")?.isFile == true
        }
    }

    private fun hasImgSignature(root: DocumentFile, vararg path: String): Boolean {
        val img = findChildPath(root, *path) ?: return false
        if (!img.isFile || img.length() in 1 until IMG_MINIMUM_BYTES) return false
        val header = ByteArray(4)
        val count = context.contentResolver.openInputStream(img.uri)?.use { it.read(header) } ?: return false
        return count == 4 && header.contentEquals(byteArrayOf('V'.code.toByte(), 'E'.code.toByte(), 'R'.code.toByte(), '2'.code.toByte()))
    }

    private fun isNonEmptyFile(root: DocumentFile, vararg path: String): Boolean {
        val file = findChildPath(root, *path) ?: return false
        if (!file.isFile) return false
        // Some SAF providers report zero for an unknown length, so prove the
        // document is readable with one byte rather than trusting metadata.
        return context.contentResolver.openInputStream(file.uri)?.use { input -> input.read() >= 0 } ?: false
    }

    private fun findChildPath(root: DocumentFile, vararg segments: String): DocumentFile? {
        var current = root
        for (segment in segments) {
            current = current.listFiles().firstOrNull { child ->
                child.name?.equals(segment, ignoreCase = true) == true
            } ?: return null
        }
        return current
    }

    private fun relativePath(root: DocumentFile, child: DocumentFile): String? {
        val rootUri = root.uri.toString()
        var current: DocumentFile? = child
        val segments = ArrayList<String>()
        while (true) {
            val node = current ?: break
            if (node.uri.toString() == rootUri) break
            val name = node.name?.takeIf { it.isNotBlank() } ?: return null
            if (name.length > MAX_PATH_SEGMENT_CHARS || name.contains('/') || name.contains('\\') ||
                name == "." || name == ".." || name.indexOf('\u0000') >= 0) return null
            segments += name
            current = node.parentFile
            if (segments.size > MAX_PATH_SEGMENTS) return null
        }
        if (current == null || segments.isEmpty()) return null
        return segments.asReversed().joinToString("/")
    }

    /**
     * Replaces only the already-sanitized terminal segment of a persisted
     * tree-relative path after a successful same-parent SAF rename. This keeps
     * the result independent from a provider's potentially stale DocumentFile
     * instance and cannot introduce traversal into the persisted preference.
     */
    private fun renamedLeafPath(original: String, replacement: String): String? {
        if (replacement.isBlank() || replacement.length > MAX_PATH_SEGMENT_CHARS ||
            replacement.contains('/') || replacement.contains('\\') ||
            replacement == "." || replacement == ".." || replacement.indexOf('\u0000') >= 0) {
            return null
        }
        val segments = original.split('/')
        if (segments.isEmpty() || segments.any { it.isBlank() || it == "." || it == ".." }) return null
        return (segments.dropLast(1) + replacement).joinToString("/")
    }

    private fun deleteTreeBestEffort(document: DocumentFile) {
        if (document.isDirectory) {
            document.listFiles().forEach(::deleteTreeBestEffort)
        }
        runCatching { document.delete() }
    }

    private fun ensureDirectory(root: DocumentFile, segments: List<String>): DocumentFile {
        var current = root
        for (segment in segments) {
            // ZIP names are collision-checked case-insensitively. Use the same
            // lookup here so `foo/a` and `FOO/b` cannot create ambiguous trees
            // on providers with case-insensitive document names.
            val existing = current.listFiles().firstOrNull {
                it.name?.equals(segment, ignoreCase = true) == true
            }
            current = when {
                existing == null -> current.createDirectory(segment)
                    ?: throw InstallFailure("Tidak bisa membuat folder ZIP: $segment")
                existing.isDirectory -> existing
                else -> throw InstallFailure("ZIP bertabrakan dengan file saat membuat folder: $segment")
            }
        }
        return current
    }

    private fun createFreshFile(parent: DocumentFile, name: String): DocumentFile {
        val existing = parent.listFiles().firstOrNull {
            it.name?.equals(name, ignoreCase = true) == true
        }
        if (existing != null) {
            // A staging directory starts empty, so an existing document is
            // always a ZIP collision, never a file we should overwrite/delete.
            throw InstallFailure("ZIP bertabrakan dengan entry sementara: $name")
        }
        return parent.createFile("application/octet-stream", name)
            ?: throw InstallFailure("Tidak bisa membuat entry ZIP: $name")
    }

    private fun sanitizeZipPath(entry: ZipEntry): String {
        val raw = entry.name.replace('\\', '/').trim()
        if (raw.isEmpty() || raw.startsWith('/') || raw.length > MAX_ENTRY_PATH_CHARS) {
            throw InstallFailure("Nama entry ZIP tidak aman.")
        }
        val segments = raw.split('/').filter { it.isNotEmpty() }
        if (segments.isEmpty() || segments.size > MAX_PATH_SEGMENTS || segments.any { segment ->
                segment == "." || segment == ".." || segment.indexOf('\u0000') >= 0 ||
                    segment.contains(':') || segment.length > MAX_PATH_SEGMENT_CHARS
            }) {
            throw InstallFailure("Entry ZIP mencoba traversal atau nama path tidak didukung: $raw")
        }
        return segments.joinToString("/")
    }

    private fun openFollowingHttpsRedirects(initial: URL): HttpsURLConnection {
        var url = initial
        repeat(MAX_REDIRECTS + 1) {
            val connection = url.openConnection() as? HttpsURLConnection
                ?: throw InstallFailure("Hanya HTTPS yang diizinkan untuk unduhan data.")
            connection.instanceFollowRedirects = false
            connection.connectTimeout = CONNECT_TIMEOUT_MS
            connection.readTimeout = READ_TIMEOUT_MS
            connection.requestMethod = "GET"
            connection.setRequestProperty("Accept", "application/zip, application/octet-stream")
            val code = connection.responseCode
            if (code !in REDIRECT_CODES) return connection
            val location = connection.getHeaderField("Location")
                ?: run {
                    connection.disconnect()
                    throw InstallFailure("Redirect HTTPS tidak memberi Location.")
                }
            connection.disconnect()
            url = validatedHttpsUrl(URL(url, location).toString())
        }
        throw InstallFailure("Terlalu banyak redirect HTTPS.")
    }

    private fun validatedHttpsUrl(raw: String): URL {
        val uri = URI(raw.trim())
        if (!uri.scheme.equals("https", ignoreCase = true) || uri.host.isNullOrBlank() ||
            !uri.userInfo.isNullOrBlank()) {
            throw InstallFailure("Masukkan URL HTTPS langsung tanpa kredensial di URL.")
        }
        return uri.toURL()
    }

    private fun formatBytes(bytes: Long): String = when {
        bytes < 1024L -> "$bytes B"
        bytes < 1024L * 1024L -> "${bytes / 1024L} KiB"
        bytes < 1024L * 1024L * 1024L -> "${bytes / (1024L * 1024L)} MiB"
        else -> "${bytes / (1024L * 1024L * 1024L)} GiB"
    }

    private class InstallFailure(message: String) : Exception(message)

    private companion object {
        val SHA_256 = Regex("^[0-9a-fA-F]{64}$")
        const val BUFFER_SIZE = 128 * 1024
        const val PROGRESS_GRANULARITY_BYTES = 512L * 1024L
        const val EXTRACTION_PROGRESS_EVERY_ENTRIES = 16
        const val MAX_ARCHIVE_BYTES = 6L * 1024L * 1024L * 1024L
        const val MAX_EXTRACTED_BYTES = 8L * 1024L * 1024L * 1024L
        const val MAX_ENTRY_BYTES = 3L * 1024L * 1024L * 1024L
        const val MAX_COMPRESSION_RATIO = 200L
        const val ZIP_RATIO_GRACE_BYTES = 4L * 1024L * 1024L
        const val MAX_ENTRY_COUNT = 50_000
        const val MAX_PATH_SEGMENTS = 24
        const val MAX_ENTRY_PATH_CHARS = 512
        const val MAX_PATH_SEGMENT_CHARS = 128
        const val MAX_ROOT_CANDIDATES = 64
        const val IMG_MINIMUM_BYTES = 8L
        const val CONNECT_TIMEOUT_MS = 20_000
        const val READ_TIMEOUT_MS = 30_000
        val REDIRECT_CODES = setOf(301, 302, 303, 307, 308)
    }
}
