package io.github.besliky.airplaytv.update

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class VersionTest {

    private fun v(text: String) = Version.parse(text)!!

    @Test
    fun `tags and version names parse`() {
        assertEquals("0.2.0", v("v0.2.0").name)
        assertEquals("1.10.3-rc.2", v("1.10.3-rc.2").name)
        assertTrue(v("0.2.1-rc.1").isPrerelease)
        assertFalse(v("0.2.1").isPrerelease)
        assertNull(Version.parse("0.2"))
        assertNull(Version.parse("latest"))
        assertNull(Version.parse("1.2.3-"))
        assertNull(Version.parse("1.2.3-rc..1"))
    }

    @Test
    fun `numbers compare as numbers`() {
        assertTrue(v("0.10.0") > v("0.9.9"))
        assertTrue(v("1.0.0") > v("0.99.99"))
        assertTrue(v("0.2.1") > v("0.2.0"))
        assertEquals(v("v0.2.0"), v("0.2.0"))
    }

    @Test
    fun `a release is newer than its test versions`() {
        assertTrue(v("0.3.0") > v("0.3.0-rc.9"))
        assertTrue(v("0.3.0-rc.1") > v("0.2.9"))
        assertTrue(v("0.3.0-rc.10") > v("0.3.0-rc.9"))
        assertTrue(v("0.3.0-rc.1") > v("0.3.0-beta.5"))
        assertTrue(v("0.3.0-rc.1.1") > v("0.3.0-rc.1"))
        assertTrue(v("0.3.0-rc") > v("0.3.0-1"))
    }

    @Test
    fun `build metadata is ignored`() {
        assertEquals(v("1.2.3"), v("1.2.3+build.7"))
        assertEquals("1.2.3", v("1.2.3+build.7").name)
    }
}
