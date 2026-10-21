package com.xykal.sampdroid.ui

/**
 * Pure path policy for the opt-in Lite archive installer. Callers must sanitize
 * a ZIP name before asking this question; this policy deliberately classifies
 * only a complete path segment, never a filename prefix/suffix.
 */
internal object GtaArchivePathPolicy {
    fun isGtaAudioEntry(path: String): Boolean =
        path.split('/').any { segment -> segment.equals("audio", ignoreCase = true) }
}
