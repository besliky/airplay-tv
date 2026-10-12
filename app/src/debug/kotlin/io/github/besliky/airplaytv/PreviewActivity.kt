package io.github.besliky.airplaytv

import android.app.Activity
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Shader
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import io.github.besliky.airplaytv.lyrics.Lyrics
import io.github.besliky.airplaytv.service.ArtworkColors
import io.github.besliky.airplaytv.service.NowPlaying
import io.github.besliky.airplaytv.service.ReceiverState
import io.github.besliky.airplaytv.ui.NowPlayingActivity
import io.github.besliky.airplaytv.ui.PhotoActivity

/**
 * Debug builds only: shows the Now Playing and photo screens with made-up content, without a phone.
 *
 *     adb shell am start -n io.github.besliky.airplaytv/.PreviewActivity --es show now
 *
 * `show` is one of now, pause, lyrics, dim, ambient, blank, goodnight, volume, next, previous,
 * photo and photo2. Nothing is sent over the network. The receiver's real state is put back when
 * the preview is left with Back.
 */
class PreviewActivity : Activity() {

    private class Track(val title: String, val artist: String, val album: String, val hue: Float, val seconds: Long)

    private val tracks = listOf(
        Track("Midnight on the Harbour", "The Paper Lanterns", "Slow Tide", 215f, 212),
        Track("Northern Lights", "Aurora Fields", "Polar Nights", 340f, 250),
        Track("A Very Long Song Title That Needs Two Lines to Fit on the Screen", "Various Artists", "Compilation, Vol. 2", 120f, 241),
    )

    private val handler = Handler(Looper.getMainLooper())
    private var saved: ReceiverState.Snapshot? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        saved = ReceiverState.current
        val show = intent.getStringExtra("show") ?: "now"
        if (show.startsWith("photo")) {
            ReceiverState.update { it.copy(photo = landscape(200f), photoSeq = it.photoSeq + 1) }
            if (show == "photo2") later(5000) { ReceiverState.update { it.copy(photo = landscape(20f), photoSeq = it.photoSeq + 1) } }
            startActivity(Intent(this, PhotoActivity::class.java))
            return
        }
        play(tracks[0], show == "pause")
        when (show) {
            "volume" -> later(3000) { ReceiverState.update { it.copy(nowPlaying = it.nowPlaying.copy(volume = 0.35f, volumeSeq = it.nowPlaying.volumeSeq + 1)) } }
            "next" -> later(6000) { change(tracks[1], 1) }
            "previous" -> later(6000) { change(tracks[2], -1) }
        }
        startActivity(Intent(this, NowPlayingActivity::class.java).putExtra(NowPlayingActivity.EXTRA_PREVIEW, show))
    }

    override fun onRestart() {
        super.onRestart()
        // back from the preview: put the real state back
        handler.removeCallbacksAndMessages(null)
        saved?.let { real -> ReceiverState.update { real } }
        finish()
    }

    private fun later(delayMs: Long, block: () -> Unit) {
        handler.postDelayed(block, delayMs)
    }

    private fun play(track: Track, paused: Boolean) {
        val art = cover(track.hue)
        ReceiverState.update {
            it.copy(
                status = ReceiverState.Status.CONNECTED,
                clientName = "iPhone",
                audioActive = true,
                videoActive = false,
                nowPlaying = NowPlaying(
                    title = track.title, artist = track.artist, album = track.album, track = 1,
                    artwork = art, artworkSeq = 1, colors = ArtworkColors.from(art),
                    durationMs = track.seconds * 1000, positionMs = 93_000, positionAt = SystemClock.elapsedRealtime(),
                    playing = !paused, volume = 0.8f, volumeSeq = 1,
                    lyricsStatus = NowPlaying.LyricsStatus.FOUND, lyrics = lyrics(),
                ),
            )
        }
    }

    private fun change(track: Track, direction: Int) {
        ReceiverState.update {
            it.copy(
                nowPlaying = it.nowPlaying.copy(
                    title = track.title, artist = track.artist, album = track.album,
                    track = it.nowPlaying.track + 1, direction = direction,
                    durationMs = track.seconds * 1000, positionMs = 0, positionAt = SystemClock.elapsedRealtime(),
                ),
            )
        }
        later(250) {
            val art = cover(track.hue)
            ReceiverState.update {
                it.copy(nowPlaying = it.nowPlaying.copy(artwork = art, artworkSeq = it.nowPlaying.artworkSeq + 1, colors = ArtworkColors.from(art)))
            }
        }
    }

    /** Invented lines, timed so that one is highlighted around 1:33. */
    private fun lyrics(): Lyrics {
        val words = listOf(
            "Neon rivers run beneath the sleeping town",
            "Every window holds a story folded small",
            "We were chasing echoes down the stairs",
            "Counting streetlights like they were stars",
            "Hold the evening a little longer now",
            "Let the radio carry what we cannot say",
            "Paper lanterns drifting out to sea",
            "Tell me that the morning can wait for us",
            "Slow the clock and keep the night in tune",
            "Nothing here is ending, nothing here is lost",
        )
        return Lyrics(words.mapIndexed { i, text -> Lyrics.Line(70_000L + i * 6_500L, text) }, synced = true)
    }

    /** A made-up cover: a diagonal gradient of one hue with a few translucent circles. */
    private fun cover(hue: Float): Bitmap {
        val size = 600
        val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.shader = LinearGradient(
            0f, 0f, size.toFloat(), size.toFloat(),
            Color.HSVToColor(floatArrayOf(hue, 0.75f, 0.95f)), Color.HSVToColor(floatArrayOf((hue + 40f) % 360f, 0.85f, 0.45f)),
            Shader.TileMode.CLAMP,
        )
        canvas.drawRect(0f, 0f, size.toFloat(), size.toFloat(), paint)
        paint.shader = null
        paint.color = Color.argb(60, 255, 255, 255)
        canvas.drawCircle(size * 0.30f, size * 0.35f, size * 0.28f, paint)
        paint.color = Color.argb(45, 0, 0, 0)
        canvas.drawCircle(size * 0.70f, size * 0.70f, size * 0.34f, paint)
        paint.color = Color.argb(90, 255, 255, 255)
        canvas.drawCircle(size * 0.62f, size * 0.30f, size * 0.10f, paint)
        return bitmap
    }

    /** A made-up 3:2 photo: a sky, a sun and two hills. */
    private fun landscape(hue: Float): Bitmap {
        val w = 1800
        val h = 1200
        val bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.shader = LinearGradient(
            0f, 0f, 0f, h * 0.7f,
            Color.HSVToColor(floatArrayOf(hue, 0.55f, 0.35f)), Color.HSVToColor(floatArrayOf((hue + 25f) % 360f, 0.35f, 0.95f)),
            Shader.TileMode.CLAMP,
        )
        canvas.drawRect(0f, 0f, w.toFloat(), h.toFloat(), paint)
        paint.shader = null
        paint.color = Color.argb(230, 255, 240, 200)
        canvas.drawCircle(w * 0.72f, h * 0.42f, h * 0.12f, paint)
        paint.color = Color.HSVToColor(floatArrayOf((hue + 120f) % 360f, 0.5f, 0.25f))
        canvas.drawOval(-w * 0.2f, h * 0.62f, w * 0.7f, h * 1.3f, paint)
        paint.color = Color.HSVToColor(floatArrayOf((hue + 140f) % 360f, 0.5f, 0.16f))
        canvas.drawOval(w * 0.3f, h * 0.7f, w * 1.3f, h * 1.4f, paint)
        return bitmap
    }
}
