package com.xykal.sampdroid.ui

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GtaArchivePathPolicyTest {

    @Test
    fun `matches audio only as a complete case insensitive path segment`() {
        assertTrue(GtaArchivePathPolicy.isGtaAudioEntry("audio/streams/aa"))
        assertTrue(GtaArchivePathPolicy.isGtaAudioEntry("GTA/AuDiO/SFX/FEET"))
        assertTrue(GtaArchivePathPolicy.isGtaAudioEntry("audio"))
    }

    @Test
    fun `does not classify merely similar names as GTA audio`() {
        assertFalse(GtaArchivePathPolicy.isGtaAudioEntry("audiofix/readme.txt"))
        assertFalse(GtaArchivePathPolicy.isGtaAudioEntry("data/audionotes.dat"))
        assertFalse(GtaArchivePathPolicy.isGtaAudioEntry("models/audio.txd"))
        assertFalse(GtaArchivePathPolicy.isGtaAudioEntry("GTA/audio2/SFX"))
    }
}
