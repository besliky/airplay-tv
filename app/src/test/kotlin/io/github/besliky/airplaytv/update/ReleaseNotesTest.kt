package io.github.besliky.airplaytv.update

import org.junit.Assert.assertEquals
import org.junit.Test

class ReleaseNotesTest {

    @Test
    fun `only the changes are shown, wrapped items joined`() {
        val notes = """
            ## Install

            Most Android TV devices need **AirPlayTV-v0.2.0-arm64.apk**.

            ## Changes

            - Now Playing screen for music
            - Track info, cover and progress from the sender, transport buttons
              that control the phone
            * Lyrics

            ## SHA-256

            ```
            abc  AirPlayTV-v0.2.0-arm64.apk
            ```
        """.trimIndent()
        assertEquals(
            "•  Now Playing screen for music\n" +
                "•  Track info, cover and progress from the sender, transport buttons that control the phone\n" +
                "•  Lyrics",
            Updater.changes(notes),
        )
    }

    @Test
    fun `notes without changes give nothing`() {
        assertEquals("", Updater.changes("## Install\n\nadb install x.apk\n"))
        assertEquals("", Updater.changes(""))
    }
}
