package io.github.besliky.airplaytv.lyrics

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class LyricsTest {

    @Test
    fun `time tags of every precision are read`() {
        val lines = Lyrics.parseLrc(
            """
            [ar: Someone]
            [00:01] one
            [00:02.5] two
            [00:03.25] three
            [01:04.125] four
            [00:05:50] five
            """.trimIndent(),
        )
        assertEquals(listOf(1000L, 2500L, 3250L, 5500L, 64125L), lines.map { it.timeMs })
        assertEquals(listOf("one", "two", "three", "five", "four"), lines.map { it.text })
    }

    @Test
    fun `a line with several tags is repeated, empty lines are dropped`() {
        val lines = Lyrics.parseLrc("[00:10.00][00:30.00] chorus\n[00:20.00]\n[00:20.50]   \n[00:25.00] verse")
        assertEquals(listOf(10_000L, 25_000L, 30_000L), lines.map { it.timeMs })
        assertEquals(listOf("chorus", "verse", "chorus"), lines.map { it.text })
    }

    @Test
    fun `offset moves the lines earlier`() {
        val lines = Lyrics.parseLrc("[offset:+500]\n[00:01.00] a\n[00:00.20] b")
        assertEquals(listOf(0L, 500L), lines.map { it.timeMs })
    }

    @Test
    fun `the line being sung is found`() {
        val lyrics = Lyrics(Lyrics.parseLrc("[00:01] a\n[00:05] b\n[00:09] c"), synced = true)
        assertEquals(-1, lyrics.indexAt(500))
        assertEquals(0, lyrics.indexAt(1000))
        assertEquals(1, lyrics.indexAt(8999))
        assertEquals(2, lyrics.indexAt(60_000))
        assertEquals(-1, Lyrics(lyrics.lines, synced = false).indexAt(5000))
        assertEquals(-1, Lyrics(emptyList(), synced = true).indexAt(5000))
    }

    @Test
    fun `plain lyrics keep one break between paragraphs`() {
        val lines = Lyrics.parsePlain("\n\n  first  \nsecond\n\n\n\nthird\n\n")
        assertEquals(listOf("first", "second", "", "third"), lines.map { it.text })
        assertTrue(lines.all { it.timeMs == -1L })
        assertTrue(Lyrics.parsePlain(" \n \n").isEmpty())
    }
}
