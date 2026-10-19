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
        val normalized = relativePath.trim().replace('\\', '/')
        if (normalized.isBlank() || normalized.startsWith('/')) return null
        val segments = normalized
            .split('/')
            .filter { it.isNotBlank() }
        if (segments.isEmpty() || segments.size > MAX_PATH_SEGMENTS ||
            segments.any { it == "." || it == ".." || it.indexOf('\u0000') >= 0 }
        ) return null

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

    companion object {
        private const val MAX_PATH_SEGMENTS = 24

        fun fromTreeUri(context: Context, uri: Uri): GtaDataTreeResolver? =
            DocumentFile.fromTreeUri(context, uri)
                ?.takeIf { it.isDirectory }
                ?.let(::GtaDataTreeResolver)
    }
}
