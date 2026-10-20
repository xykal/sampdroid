package com.xykal.sampdroid.ui

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile

/**
 * Resolves only normalized relative GTA paths inside a user-approved SAF tree.
 * It never follows filesystem paths, escapes the selected root, or copies data.
 */
internal class GtaDataTreeResolver private constructor(
    private val root: DocumentFile
) {

    fun findFile(relativePath: String): DocumentFile? {
        val segments = safeSegments(relativePath) ?: return null

        var current = root
        segments.forEachIndexed { index, segment ->
            // Android providers differ in case behavior. gta.dat has already
            // normalized paths natively, so resolve a segment case-insensitively
            // without ever allowing a traversal component.
            val child = current.listFiles().firstOrNull { file ->
                file.name?.equals(segment, ignoreCase = true) == true
            } ?: return null
            if (index < segments.lastIndex && !child.isDirectory) return null
            current = child
        }
        return current.takeIf { it.isFile }
    }

    /**
     * Creates a resolver rooted at a bounded relative child directory. This is
     * used for an installer staging result while retaining the original SAF tree
     * URI/grant; DocumentFile.fromTreeUri intentionally always re-roots at the
     * grant root and cannot represent an arbitrary child by itself.
     */
    fun subdirectory(relativePath: String): GtaDataTreeResolver? {
        val segments = safeSegments(relativePath) ?: return null
        var current = root
        for (segment in segments) {
            current = current.listFiles().firstOrNull { child ->
                child.name?.equals(segment, ignoreCase = true) == true
            }?.takeIf { it.isDirectory } ?: return null
        }
        return GtaDataTreeResolver(current)
    }

    fun firstFile(paths: Iterable<String>): DocumentFile? = paths.firstNotNullOfOrNull(::findFile)

    fun resolveFiles(paths: Iterable<String>, maxFiles: Int): FileResolution {
        val files = ArrayList<DocumentFile>()
        val missing = ArrayList<String>()
        val seen = HashSet<String>()
        for (path in paths) {
            val normalized = path.trim().lowercase()
            if (normalized.isBlank() || !seen.add(normalized)) continue
            if (files.size >= maxFiles) {
                missing += normalized
                continue
            }
            val file = findFile(normalized)
            if (file == null) missing += normalized else files += file
        }
        return FileResolution(files, missing)
    }

    data class FileResolution(
        val files: List<DocumentFile>,
        val missingPaths: List<String>
    )

    private fun safeSegments(relativePath: String): List<String>? {
        val normalized = relativePath.trim().replace('\\', '/')
        if (normalized.isBlank() || normalized.length > MAX_PATH_CHARS || normalized.startsWith('/')) {
            return null
        }
        val segments = normalized.split('/').filter { it.isNotBlank() }
        if (segments.isEmpty() || segments.size > MAX_PATH_SEGMENTS || segments.any { segment ->
                segment == "." || segment == ".." || segment.indexOf('\u0000') >= 0 ||
                    segment.length > MAX_PATH_SEGMENT_CHARS
            }) {
            return null
        }
        return segments
    }

    companion object {
        private const val MAX_PATH_SEGMENTS = 24
        private const val MAX_PATH_CHARS = 512
        private const val MAX_PATH_SEGMENT_CHARS = 128

        /** Opens the exact ACTION_OPEN_DOCUMENT_TREE grant root. */
        fun fromTreeUri(context: Context, uri: Uri): GtaDataTreeResolver? =
            DocumentFile.fromTreeUri(context, uri)
                ?.takeIf { it.isDirectory }
                ?.let(::GtaDataTreeResolver)
    }
}
