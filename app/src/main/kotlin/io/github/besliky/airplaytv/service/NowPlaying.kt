package io.github.besliky.airplaytv.service

import android.graphics.Bitmap
import io.github.besliky.airplaytv.lyrics.Lyrics

/** What an audio sender is playing, as far as it has told the receiver. Reset when the session ends. */
data class NowPlaying(
    val title: String = "",
    val artist: String = "",
    val album: String = "",
    /** Counts track changes; [direction] is -1 when the track before came back, otherwise 1. */
    val track: Int = 0,
    val direction: Int = 1,
    val artwork: Bitmap? = null,
    /** Counts the covers the sender sent, so a new one is noticed even when it looks the same. */
    val artworkSeq: Int = 0,
    val colors: ArtworkColors = ArtworkColors.NEUTRAL,
    /** Track length, and the position at [positionAt] (SystemClock.elapsedRealtime); -1 while unknown. */
    val durationMs: Long = -1,
    val positionMs: Long = -1,
    val positionAt: Long = 0,
    /** False from the moment the sender stops sending audio (a pause) until it sends again. */
    val playing: Boolean = true,
    /** The volume as a slider position, 0..1, or -1 while unknown. */
    val volume: Float = -1f,
    /** Counts volume changes, so the screen shows the bar even when a change ends at the same level. */
    val volumeSeq: Int = 0,
    val lyricsStatus: LyricsStatus = LyricsStatus.OFF,
    val lyrics: Lyrics? = null,
) {
    enum class LyricsStatus { OFF, SEARCHING, FOUND, NOT_FOUND }

    val hasTrack: Boolean get() = title.isNotBlank()

    /** The position at [now], running on while playing; -1 while unknown. */
    fun positionAt(now: Long): Long {
        if (durationMs <= 0 || positionMs < 0) return -1
        val running = if (playing) now - positionAt else 0
        return (positionMs + running).coerceIn(0, durationMs)
    }
}
