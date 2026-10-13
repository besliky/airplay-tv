package io.github.besliky.airplaytv

import io.github.besliky.airplaytv.core.StreamSize
import org.junit.Assert.assertEquals
import org.junit.Test

class StreamSizeTest {

    @Test
    fun keepsCarScreensAsTheyAre() {
        assertEquals(1024 to 600, StreamSize.fit(1024, 600, 1920))
        assertEquals(1920 to 720, StreamSize.fit(1920, 720, 1920))
        assertEquals(1280 to 480, StreamSize.fit(1280, 480, 1920))
    }

    @Test
    fun scalesLargeScreensDownKeepingTheShape() {
        assertEquals(1920 to 1152, StreamSize.fit(2000, 1200, 1920))
        assertEquals(1280 to 480, StreamSize.fit(1920, 720, 1280))
    }

    @Test
    fun turnsPortraitScreensIntoLandscape() {
        assertEquals(1920 to 1080, StreamSize.fit(1080, 1920, 1920))
    }

    @Test
    fun roundsToEvenNumbers() {
        assertEquals(1280 to 748, StreamSize.fit(1333, 781, 1280))
    }

    @Test
    fun fallsBackForUnknownScreens() {
        assertEquals(1920 to 1080, StreamSize.fit(0, 0, 1920))
    }
}
