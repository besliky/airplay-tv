package io.github.besliky.airplaytv.service

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import io.github.besliky.airplaytv.Settings
import io.github.besliky.airplaytv.lyrics.LrcLib
import io.github.besliky.airplaytv.service.NowPlaying.LyricsStatus
import kotlin.math.log10
import kotlin.math.pow
import kotlin.math.roundToInt

/**
 * Keeps [ReceiverState]'s [NowPlaying] up to date from what the sender reports: track changes and
 * their direction, the cover and its colours, a position that runs on between the sender's progress
 * updates and stops at a pause, the volume and, when turned on, lyrics. Main thread only.
 *
 * @param applyGain sets the output gain (0..1) of the audio that is playing
 * @param published called after every change, for the media session
 */
class NowPlayingTracker(
    private val settings: Settings,
    private val applyGain: (Float) -> Unit,
    private val published: (NowPlaying) -> Unit,
) {
    private val handler = Handler(Looper.getMainLooper())
    private var sampleRate = 44100
    private var trackKey = ""
    private val history = ArrayList<String>()
    private var artworkToken = 0
    private var coverAt = 0L
    private var album = ""
    private var lyricsToken = 0
    private val lookUpLyrics = Runnable { findLyrics() }
    private var levelBeforeMute = 1f

    /** A new track whose cover does not come has none: the old one must go. */
    private val dropOldCover = Runnable {
        artworkToken++
        update { it.copy(artwork = null, artworkSeq = it.artworkSeq + 1, colors = ArtworkColors.NEUTRAL) }
    }

    /** The position from before the latest progress update, for the stale update a pause brings. */
    private var anchorMs = -1L
    private var anchorAt = 0L
    private var progressAt = 0L

    private val current: NowPlaying get() = ReceiverState.current.nowPlaying

    private fun update(transform: (NowPlaying) -> NowPlaying) {
        ReceiverState.update { it.copy(nowPlaying = transform(it.nowPlaying)) }
        published(current)
    }

    fun sessionStarted() {
        reset()
        update { NowPlaying(lyricsStatus = if (settings.lyricsEnabled) LyricsStatus.NOT_FOUND else LyricsStatus.OFF) }
    }

    fun sessionEnded() {
        reset()
        ReceiverState.update { it.copy(nowPlaying = NowPlaying()) }
    }

    private fun reset() {
        handler.removeCallbacks(lookUpLyrics)
        handler.removeCallbacks(dropOldCover)
        artworkToken++
        lyricsToken++
        coverAt = 0
        album = ""
        trackKey = ""
        history.clear()
        anchorMs = -1
        progressAt = 0
        levelBeforeMute = 1f
    }

    fun audioStarted(rate: Int) {
        if (rate > 0) sampleRate = rate
    }

    fun onTrackInfo(title: String, artist: String, album: String) {
        val key = "$title\u0000$artist"
        val changed = title.isNotBlank() && key != trackKey
        var direction = 1
        if (changed) {
            // Coming back to the track before the current one is "previous", anything else "next".
            direction = if (history.size >= 2 && history[history.size - 2] == key) -1 else 1
            if (direction < 0) history.removeAt(history.size - 1) else history += key
            while (history.size > HISTORY) history.removeAt(0)
            trackKey = key
            // Covers are not sent in a fixed order around the track info: one that came just before
            // belongs to this track, and one may still come. If none comes, the old cover goes, unless
            // the track is from the same album.
            handler.removeCallbacks(dropOldCover)
            val sameAlbum = album.isNotBlank() && album == this.album
            if (!sameAlbum && SystemClock.elapsedRealtime() - coverAt > COVER_EARLY_MS) {
                handler.postDelayed(dropOldCover, COVER_WAIT_MS)
            }
            this.album = album
        }
        update {
            it.copy(
                title = title, artist = artist, album = album,
                track = if (changed) it.track + 1 else it.track,
                direction = if (changed) direction else it.direction,
                lyricsStatus = if (changed && settings.lyricsEnabled) LyricsStatus.SEARCHING else it.lyricsStatus,
                lyrics = if (changed) null else it.lyrics,
            )
        }
        if (changed && settings.lyricsEnabled) {
            handler.removeCallbacks(lookUpLyrics)
            // the length arrives with the first progress update, which helps to pick the right version
            handler.postDelayed(lookUpLyrics, LYRICS_DELAY_MS)
        }
    }

    fun onArtwork(image: ByteArray?) {
        handler.removeCallbacks(dropOldCover)
        coverAt = SystemClock.elapsedRealtime()
        val token = ++artworkToken
        if (image == null) {
            update { it.copy(artwork = null, artworkSeq = it.artworkSeq + 1, colors = ArtworkColors.NEUTRAL) }
            return
        }
        Images.decode(image, Images.COVER_SIDE) { bitmap ->
            if (token != artworkToken) return@decode
            val colors = bitmap?.let { ArtworkColors.from(it) } ?: ArtworkColors.NEUTRAL
            update { it.copy(artwork = bitmap, artworkSeq = it.artworkSeq + 1, colors = colors) }
        }
    }

    fun onProgress(start: Long, position: Long, end: Long) {
        // RTP timestamps are 32-bit and wrap around; only the differences count.
        val rate = sampleRate.toLong()
        val durationMs = ((end - start) and 0xFFFFFFFFL) * 1000 / rate
        val positionMs = ((position - start) and 0xFFFFFFFFL) * 1000 / rate
        if (durationMs <= 0) return
        val now = SystemClock.elapsedRealtime()
        val before = current.positionAt(now)
        anchorMs = before
        anchorAt = now
        progressAt = now
        update { it.copy(durationMs = durationMs, positionMs = positionMs.coerceAtMost(durationMs), positionAt = now) }
    }

    /**
     * The audio stopped ([flowing] false) or came again. Senders do not announce a pause, they stop
     * sending audio; [at] is when the last audio arrived or the first came again.
     */
    fun onAudioFlow(flowing: Boolean, at: Long) {
        val np = current
        if (np.playing == flowing) return
        val now = SystemClock.elapsedRealtime()
        if (flowing) {
            update { it.copy(playing = true, positionAt = now) }
            return
        }
        var position = np.positionMs
        if (position >= 0) {
            // A sender pausing sends a progress update that repeats an old position (seen with YouTube
            // Music: off by the whole time since the update before). If one came around the moment the
            // audio stopped, the position from before it is the better one.
            val stale = progressAt > 0 && now - progressAt <= STALE_PROGRESS_MS && anchorMs >= 0
            position = if (stale) anchorMs + (at - anchorAt) else position + (at - np.positionAt)
            position = position.coerceIn(0, np.durationMs.coerceAtLeast(0))
        }
        update { it.copy(playing = false, positionMs = position, positionAt = now) }
    }

    /** The sender moved its volume slider. [gain] is linear, 0 for mute. */
    fun onSenderVolume(gain: Float) {
        applyGain(gain)
        // AirPlay's slider spans -30 dB to 0 dB.
        val db = if (gain <= 0f) -144f else 20f * log10(gain)
        val level = ((db + 30f) / 30f).coerceIn(0f, 1f)
        update { it.copy(volume = level, volumeSeq = it.volumeSeq + 1) }
    }

    /** The remote's volume buttons: [steps] of 1/16 of the slider up or down. */
    fun stepVolume(steps: Int) {
        val level = ((levelNow() * VOLUME_STEPS).roundToInt() + steps).coerceIn(0, VOLUME_STEPS) / VOLUME_STEPS.toFloat()
        setLevel(level)
    }

    fun toggleMute() {
        val level = levelNow()
        if (level > 0f) {
            levelBeforeMute = level
            setLevel(0f)
        } else {
            setLevel(levelBeforeMute.coerceAtLeast(1f / VOLUME_STEPS))
        }
    }

    private fun levelNow(): Float = current.volume.let { if (it < 0f) 1f else it }

    private fun setLevel(level: Float) {
        applyGain(if (level <= 0f) 0f else 10f.pow((level - 1f) * 30f / 20f))
        update { it.copy(volume = level, volumeSeq = it.volumeSeq + 1) }
    }

    fun lyricsSettingChanged() {
        handler.removeCallbacks(lookUpLyrics)
        lyricsToken++
        if (settings.lyricsEnabled) {
            findLyrics()
        } else {
            update { it.copy(lyricsStatus = LyricsStatus.OFF, lyrics = null) }
        }
    }

    private fun findLyrics() {
        val np = current
        if (!settings.lyricsEnabled || ReceiverState.current.clientName == null) return
        if (np.title.isBlank() || np.artist.isBlank()) {
            update { it.copy(lyricsStatus = LyricsStatus.NOT_FOUND, lyrics = null) }
            return
        }
        val token = ++lyricsToken
        update { it.copy(lyricsStatus = LyricsStatus.SEARCHING, lyrics = null) }
        LrcLib.find(np.title, np.artist, np.durationMs) { lyrics ->
            if (token != lyricsToken) return@find // another track meanwhile
            update { it.copy(lyricsStatus = if (lyrics != null) LyricsStatus.FOUND else LyricsStatus.NOT_FOUND, lyrics = lyrics) }
        }
    }

    private companion object {
        const val HISTORY = 8
        const val COVER_EARLY_MS = 1200L
        const val COVER_WAIT_MS = 1500L
        const val LYRICS_DELAY_MS = 800L
        const val STALE_PROGRESS_MS = 1500L
        const val VOLUME_STEPS = 16
    }
}
