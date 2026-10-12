package io.github.besliky.airplaytv.ui

import android.animation.ArgbEvaluator
import android.animation.ValueAnimator
import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.media.AudioManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import android.view.animation.DecelerateInterpolator
import android.view.animation.LinearInterpolator
import android.view.animation.OvershootInterpolator
import android.widget.ImageView
import android.widget.TextView
import io.github.besliky.airplaytv.BuildConfig
import io.github.besliky.airplaytv.R
import io.github.besliky.airplaytv.Settings
import io.github.besliky.airplaytv.service.ArtworkColors
import io.github.besliky.airplaytv.service.DacpClient.Command
import io.github.besliky.airplaytv.service.NowPlaying
import io.github.besliky.airplaytv.service.NowPlaying.LyricsStatus
import io.github.besliky.airplaytv.service.ReceiverService
import io.github.besliky.airplaytv.service.ReceiverState
import io.github.besliky.airplaytv.service.SleepService
import java.util.Locale
import kotlin.math.PI
import kotlin.math.sin

/**
 * What a music sender is playing: cover, title, artist and album, progress with elapsed and
 * remaining time, transport buttons and the volume, laid out like the iPhone's player. It opens by
 * itself when music starts and closes when the session ends; Back leaves the music playing.
 *
 * - The remote controls the sender: play/pause or OK, next or right, previous or left. Down shows
 *   the lyrics when they are turned on, Up or Back hides them.
 * - Tracks change with a slide in the direction of travel, the cover shrinks while paused, and the
 *   backdrop takes a dark tint from the cover.
 * - For OLED panels the layout drifts slowly, dims when nobody uses the remote, turns into a small
 *   card wandering over black, and goes black during a long pause (see [Stage]).
 * - Sleep After Music: when AirPlay turned the TV on and nobody touched the remote, the screen
 *   counts down after the music and then puts the TV back to sleep.
 */
class NowPlayingActivity : Activity() {

    /** How much of the screen is lit. */
    private enum class Stage { ACTIVE, DIM, AMBIENT, BLANK, GOODNIGHT }

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var settings: Settings
    private lateinit var drift: Drift

    private lateinit var backdrop: GradientDrawable
    private lateinit var stage: View
    private lateinit var player: View
    private lateinit var cover: View
    private lateinit var coverFront: ImageView
    private lateinit var coverBack: ImageView
    private lateinit var titleView: TextView
    private lateinit var artistView: TextView
    private lateinit var albumView: TextView
    private lateinit var elapsedView: TextView
    private lateinit var remainingView: TextView
    private lateinit var progressBar: PillBar
    private lateinit var previousButton: View
    private lateinit var playPauseButton: View
    private lateinit var pauseIcon: View
    private lateinit var playIcon: View
    private lateinit var nextButton: View
    private lateinit var volumeLow: ImageView
    private lateinit var volumeHigh: View
    private lateinit var volumeBar: PillBar
    private lateinit var hintView: TextView
    private lateinit var lyricsPage: View
    private lateinit var lyricsCover: ImageView
    private lateinit var lyricsTitle: TextView
    private lateinit var lyricsArtist: TextView
    private lateinit var lyricsView: LyricsView
    private lateinit var lyricsStatus: TextView
    private lateinit var lyricsElapsed: TextView
    private lateinit var lyricsRemaining: TextView
    private lateinit var lyricsProgress: PillBar
    private lateinit var lyricsVolume: View
    private lateinit var lyricsVolumeBar: PillBar
    private lateinit var ambient: View
    private lateinit var ambientCover: ImageView
    private lateinit var ambientTitle: TextView
    private lateinit var ambientArtist: TextView
    private lateinit var goodnightView: TextView

    /** What is on screen; null until the first state arrives. */
    private var shown: NowPlaying? = null
    private var client = ""
    private var lyricsOpen = false
    private var preview = false

    private var screenStage = Stage.ACTIVE
    private var lastInteraction = 0L
    private var pausedSince = 0L
    private var holdStageUntil = 0L
    private var goodnightEnd = 0L

    private var backdropTop = ArtworkColors.NEUTRAL.backdropTop
    private var backdropBottom = ArtworkColors.NEUTRAL.backdropBottom
    private var accent = ArtworkColors.NEUTRAL.accent
    private var colors = ArtworkColors.NEUTRAL
    private var paletteAnimator: ValueAnimator? = null

    private val stateListener: (ReceiverState.Snapshot) -> Unit = { render(it) }

    private val tick = object : Runnable {
        override fun run() {
            updateProgress()
            handler.postDelayed(this, TICK_MS)
        }
    }

    private val stageTick = object : Runnable {
        override fun run() {
            chooseStage()
            handler.postDelayed(this, STAGE_TICK_MS)
        }
    }

    private val ambientTick = object : Runnable {
        override fun run() {
            if (screenStage == Stage.AMBIENT) placeAmbient(animate = true)
            handler.postDelayed(this, AMBIENT_STEP_MS)
        }
    }

    private val goodnightTick = object : Runnable {
        override fun run() {
            val left = goodnightEnd - SystemClock.elapsedRealtime()
            if (left <= 0) {
                goToSleep()
                return
            }
            goodnightView.text = getString(R.string.goodnight_countdown, formatTime(left + 999))
            handler.postDelayed(this, 1000)
        }
    }

    private val settleVolume = Runnable { volumeBar.setActive(false) }
    private val hideLyricsVolume = Runnable { lyricsVolume.animate().alpha(0f).setDuration(300).start() }
    private val hideHint = Runnable { hintView.animate().alpha(0f).setDuration(300).start() }

    // ---------------------------------------------------------------- lifecycle

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        showOverStandby()
        setContentView(R.layout.activity_now_playing)
        settings = Settings(this)
        bindViews()
        drift = Drift(findViewById(R.id.drift), 22f, 14f)
        backdrop = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(backdropTop, backdropBottom))
        findViewById<View>(R.id.root).background = backdrop
        for (rounded in listOf(cover, lyricsCover, ambientCover)) rounded.clipToOutline = true
        applyPalette()
        preview = BuildConfig.DEBUG && intent.hasExtra(EXTRA_PREVIEW)
    }

    private fun bindViews() {
        stage = findViewById(R.id.stage)
        player = findViewById(R.id.player)
        cover = findViewById(R.id.cover)
        coverFront = findViewById(R.id.coverA)
        coverBack = findViewById(R.id.coverB)
        titleView = findViewById(R.id.title)
        artistView = findViewById(R.id.artist)
        albumView = findViewById(R.id.album)
        elapsedView = findViewById(R.id.elapsed)
        remainingView = findViewById(R.id.remaining)
        progressBar = findViewById(R.id.progress)
        previousButton = findViewById(R.id.previous)
        playPauseButton = findViewById(R.id.playPause)
        pauseIcon = findViewById(R.id.pauseIcon)
        playIcon = findViewById(R.id.playIcon)
        nextButton = findViewById(R.id.next)
        volumeLow = findViewById(R.id.volumeLow)
        volumeHigh = findViewById(R.id.volumeHigh)
        volumeBar = findViewById(R.id.volume)
        hintView = findViewById(R.id.hint)
        lyricsPage = findViewById(R.id.lyricsPage)
        lyricsCover = findViewById(R.id.lyricsCover)
        lyricsTitle = findViewById(R.id.lyricsTitle)
        lyricsArtist = findViewById(R.id.lyricsArtist)
        lyricsView = findViewById(R.id.lyrics)
        lyricsStatus = findViewById(R.id.lyricsStatus)
        lyricsElapsed = findViewById(R.id.lyricsElapsed)
        lyricsRemaining = findViewById(R.id.lyricsRemaining)
        lyricsProgress = findViewById(R.id.lyricsProgress)
        lyricsVolume = findViewById(R.id.lyricsVolume)
        lyricsVolumeBar = findViewById(R.id.lyricsVolumeBar)
        ambient = findViewById(R.id.ambient)
        ambientCover = findViewById(R.id.ambientCover)
        ambientTitle = findViewById(R.id.ambientTitle)
        ambientArtist = findViewById(R.id.ambientArtist)
        goodnightView = findViewById(R.id.goodnight)
    }

    override fun onStart() {
        super.onStart()
        lastInteraction = SystemClock.elapsedRealtime()
        ReceiverState.observe(stateListener)
        handler.post(tick)
        handler.postDelayed(stageTick, STAGE_TICK_MS)
        handler.postDelayed(ambientTick, AMBIENT_STEP_MS)
        drift.start()
        if (preview) intent.getStringExtra(EXTRA_PREVIEW)?.let(::showPreview)
    }

    override fun onStop() {
        ReceiverState.remove(stateListener)
        handler.removeCallbacksAndMessages(null)
        drift.stop()
        super.onStop()
    }

    // ---------------------------------------------------------------- rendering

    private fun render(s: ReceiverState.Snapshot) {
        if (s.videoActive) {
            finish()
            return
        }
        if (s.clientName == null) {
            musicEnded(s)
            return
        }
        if (screenStage == Stage.GOODNIGHT) setStage(Stage.ACTIVE) // a new session came in time
        client = s.clientName
        val np = s.nowPlaying
        val before = shown
        shown = np
        if (before == null) showAll(np) else showChanges(before, np)
        updateProgress()
    }

    private fun displayTitle(np: NowPlaying): String =
        if (np.hasTrack) np.title else getString(R.string.now_playing_from, client)

    private fun showAll(np: NowPlaying) {
        showTexts(np)
        showCover(np.artwork, np.direction, animate = false)
        animateColors(np.colors, animate = false)
        showPlaying(np.playing, animate = false)
        val level = if (np.volume >= 0f) np.volume else 1f
        volumeBar.setFraction(level)
        volumeLow.setImageResource(if (level <= 0f) R.drawable.ic_volume_muted else R.drawable.ic_volume_low)
        showLyrics(np)
    }

    private fun showChanges(before: NowPlaying, np: NowPlaying) {
        if (np.track != before.track) {
            trackChanged(np)
        } else if (np.title != before.title || np.artist != before.artist || np.album != before.album) {
            showTexts(np)
        }
        if (np.artworkSeq != before.artworkSeq) {
            showCover(np.artwork, np.direction, animate = true)
            animateColors(np.colors, animate = true)
        }
        if (np.playing != before.playing) {
            showPlaying(np.playing, animate = true)
            if (np.playing && screenStage == Stage.BLANK) setStage(Stage.AMBIENT)
        }
        if (np.volumeSeq != before.volumeSeq && np.volume >= 0f) volumeChanged(np.volume)
        if (np.lyrics !== before.lyrics || np.lyricsStatus != before.lyricsStatus) showLyrics(np)
    }

    private fun showTexts(np: NowPlaying) {
        setText(titleView, displayTitle(np))
        setText(artistView, np.artist)
        setText(albumView, np.album)
        showSmallTexts(np)
    }

    /** The texts of the lyrics header and the ambient card, which change without animation. */
    private fun showSmallTexts(np: NowPlaying) {
        lyricsTitle.text = displayTitle(np)
        lyricsArtist.text = np.artist
        ambientTitle.text = displayTitle(np)
        ambientArtist.text = np.artist
        ambientArtist.visibility = if (np.artist.isBlank()) View.GONE else View.VISIBLE
    }

    private fun setText(view: TextView, text: String) {
        view.animate().cancel()
        view.alpha = 1f
        view.translationX = 0f
        view.tag = text
        view.text = text
        view.visibility = if (text.isBlank()) View.GONE else View.VISIBLE
    }

    private fun trackChanged(np: NowPlaying) {
        swapText(titleView, displayTitle(np), np.direction, 0)
        swapText(artistView, np.artist, np.direction, 50)
        swapText(albumView, np.album, np.direction, 100)
        showSmallTexts(np)
        progressBar.setFraction(0f)
        lyricsProgress.setFraction(0f)
        if (screenStage == Stage.DIM) setStage(Stage.ACTIVE)
    }

    /** Slides the old text out and the new one in, in the direction of travel. */
    private fun swapText(view: TextView, text: String, direction: Int, delayMs: Long) {
        if (view.tag == text) return
        view.tag = text
        val shift = TEXT_SHIFT_DP * resources.displayMetrics.density * direction
        view.animate().cancel()
        view.animate().setStartDelay(0).alpha(0f).translationX(-shift).setDuration(TEXT_OUT_MS)
            .setInterpolator(DecelerateInterpolator())
            .withEndAction {
                view.text = text
                view.visibility = if (text.isBlank()) View.GONE else View.VISIBLE
                view.translationX = shift
                view.animate().setStartDelay(delayMs).alpha(1f).translationX(0f).setDuration(TEXT_IN_MS)
                    .setInterpolator(DecelerateInterpolator(1.6f)).start()
            }
            .start()
    }

    /** Cross-slides to a new cover in the direction of travel. */
    private fun showCover(bitmap: Bitmap?, direction: Int, animate: Boolean) {
        val incoming = coverBack
        val outgoing = coverFront
        coverFront = incoming
        coverBack = outgoing
        incoming.animate().cancel()
        outgoing.animate().cancel()
        incoming.setImageBitmap(bitmap)
        incoming.bringToFront()
        lyricsCover.setImageBitmap(bitmap)
        ambientCover.setImageBitmap(bitmap)
        if (!animate) {
            incoming.alpha = 1f
            incoming.translationX = 0f
            incoming.scaleX = 1f
            incoming.scaleY = 1f
            outgoing.alpha = 0f
            return
        }
        val shift = COVER_SHIFT_DP * resources.displayMetrics.density * direction
        incoming.alpha = 0f
        incoming.translationX = shift
        incoming.scaleX = 0.92f
        incoming.scaleY = 0.92f
        incoming.animate().alpha(1f).translationX(0f).scaleX(1f).scaleY(1f).setDuration(COVER_IN_MS)
            .setInterpolator(DecelerateInterpolator(1.8f)).start()
        outgoing.animate().alpha(0f).translationX(-shift * 0.6f).scaleX(0.92f).scaleY(0.92f).setDuration(COVER_OUT_MS).start()
    }

    /** Play and pause cross-fade in place; the cover shrinks a little while paused. */
    private fun showPlaying(playing: Boolean, animate: Boolean) {
        pausedSince = if (playing) 0L else SystemClock.elapsedRealtime()
        val visible = if (playing) pauseIcon else playIcon
        val hidden = if (playing) playIcon else pauseIcon
        val scale = if (playing) 1f else PAUSED_COVER_SCALE
        visible.animate().cancel()
        hidden.animate().cancel()
        cover.animate().cancel()
        if (!animate) {
            visible.alpha = 1f
            hidden.alpha = 0f
            cover.scaleX = scale
            cover.scaleY = scale
            return
        }
        visible.scaleX = 0.82f
        visible.scaleY = 0.82f
        visible.animate().alpha(1f).scaleX(1f).scaleY(1f).setDuration(200).setInterpolator(DecelerateInterpolator()).start()
        hidden.animate().alpha(0f).scaleX(0.82f).scaleY(0.82f).setDuration(140).setInterpolator(DecelerateInterpolator()).start()
        cover.animate().scaleX(scale).scaleY(scale)
            .setDuration(if (playing) 420 else 300)
            .setInterpolator(if (playing) OvershootInterpolator(1.5f) else DecelerateInterpolator(1.4f))
            .start()
    }

    private fun updateProgress() {
        val np = shown ?: return
        val position = np.positionAt(SystemClock.elapsedRealtime())
        val known = position >= 0
        val fraction = if (known) position.toFloat() / np.durationMs else 0f
        val elapsed = if (known) formatTime(position) else ""
        val remaining = if (known) MINUS + formatTime(np.durationMs - position) else ""
        progressBar.setFraction(fraction)
        elapsedView.text = elapsed
        remainingView.text = remaining
        if (lyricsOpen) {
            lyricsProgress.setFraction(fraction)
            lyricsElapsed.text = elapsed
            lyricsRemaining.text = remaining
            if (known) lyricsView.setPosition(position + LYRICS_LEAD_MS, np.durationMs)
        }
    }

    private fun formatTime(ms: Long): String {
        val s = ms / 1000
        return if (s >= 3600) {
            String.format(Locale.US, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60)
        } else {
            String.format(Locale.US, "%d:%02d", s / 60, s % 60)
        }
    }

    private fun volumeChanged(level: Float) {
        wake()
        volumeBar.setFraction(level, animate = true)
        volumeBar.setActive(true)
        volumeLow.setImageResource(if (level <= 0f) R.drawable.ic_volume_muted else R.drawable.ic_volume_low)
        volumeHigh.animate().cancel()
        volumeHigh.animate().scaleX(1.18f).scaleY(1.18f).setDuration(90)
            .withEndAction { volumeHigh.animate().scaleX(1f).scaleY(1f).setDuration(180).start() }
            .start()
        handler.removeCallbacks(settleVolume)
        handler.postDelayed(settleVolume, VOLUME_SETTLE_MS)
        if (lyricsOpen) {
            lyricsVolumeBar.setFraction(level, animate = true)
            lyricsVolume.animate().cancel()
            lyricsVolume.animate().alpha(1f).setDuration(160).start()
            handler.removeCallbacks(hideLyricsVolume)
            handler.postDelayed(hideLyricsVolume, VOLUME_SETTLE_MS + 400)
        }
    }

    // ---------------------------------------------------------------- colours

    private fun animateColors(next: ArtworkColors, animate: Boolean) {
        colors = next
        val dark = screenStage == Stage.AMBIENT || screenStage == Stage.BLANK || screenStage == Stage.GOODNIGHT
        val top = if (dark) Color.BLACK else next.backdropTop
        val bottom = if (dark) Color.BLACK else next.backdropBottom
        paletteAnimator?.cancel()
        if (!animate) {
            backdropTop = top
            backdropBottom = bottom
            accent = next.accent
            applyPalette()
            return
        }
        val fromTop = backdropTop
        val fromBottom = backdropBottom
        val fromAccent = accent
        val evaluator = ArgbEvaluator()
        paletteAnimator = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = PALETTE_MS
            addUpdateListener {
                val p = it.animatedValue as Float
                backdropTop = evaluator.evaluate(p, fromTop, top) as Int
                backdropBottom = evaluator.evaluate(p, fromBottom, bottom) as Int
                accent = evaluator.evaluate(p, fromAccent, next.accent) as Int
                applyPalette()
            }
            start()
        }
    }

    private fun applyPalette() {
        backdrop.colors = intArrayOf(backdropTop, backdropBottom)
        val fill = blend(accent, Color.WHITE, 0.45f)
        val track = blend(BAR_TRACK, accent, 0.14f)
        for (bar in listOf(progressBar, volumeBar, lyricsProgress, lyricsVolumeBar)) bar.setColors(track, fill)
    }

    private fun blend(from: Int, to: Int, amount: Float): Int = Color.rgb(
        (Color.red(from) + (Color.red(to) - Color.red(from)) * amount).toInt(),
        (Color.green(from) + (Color.green(to) - Color.green(from)) * amount).toInt(),
        (Color.blue(from) + (Color.blue(to) - Color.blue(from)) * amount).toInt(),
    )

    // ---------------------------------------------------------------- lyrics

    private fun showLyrics(np: NowPlaying) {
        lyricsView.setLyrics(np.lyrics)
        val status = when (np.lyricsStatus) {
            LyricsStatus.OFF -> getString(R.string.lyrics_off)
            LyricsStatus.SEARCHING -> getString(R.string.lyrics_searching)
            LyricsStatus.NOT_FOUND -> getString(R.string.lyrics_not_found)
            LyricsStatus.FOUND -> if (np.lyrics?.instrumental == true) getString(R.string.lyrics_instrumental) else ""
        }
        lyricsStatus.text = status
        lyricsStatus.visibility = if (status.isEmpty()) View.GONE else View.VISIBLE
        val readable = np.lyricsStatus == LyricsStatus.FOUND && np.lyrics?.instrumental != true
        if (readable && !lyricsOpen) {
            showHint(getString(R.string.lyrics_hint), HINT_MS)
        } else if (!readable) {
            hintView.alpha = 0f
        }
    }

    private fun showHint(text: String, durationMs: Long) {
        hintView.text = text
        hintView.animate().cancel()
        hintView.animate().alpha(0.75f).setDuration(250).start()
        handler.removeCallbacks(hideHint)
        handler.postDelayed(hideHint, durationMs)
    }

    private fun openLyrics() {
        if (shown?.lyricsStatus == LyricsStatus.OFF) {
            showHint(getString(R.string.lyrics_turn_on), 4000)
            return
        }
        if (lyricsOpen) return
        lyricsOpen = true
        lyricsPage.animate().cancel()
        player.animate().cancel()
        lyricsPage.visibility = View.VISIBLE
        lyricsPage.animate().alpha(1f).setDuration(320).start()
        player.animate().alpha(0f).setDuration(260).start()
        hintView.alpha = 0f
        updateProgress()
    }

    private fun closeLyrics() {
        if (!lyricsOpen) return
        lyricsOpen = false
        lyricsPage.animate().cancel()
        player.animate().cancel()
        player.animate().alpha(1f).setDuration(320).start()
        lyricsPage.animate().alpha(0f).setDuration(260).withEndAction { lyricsPage.visibility = View.GONE }.start()
    }

    // ---------------------------------------------------------------- remote

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (screenStage == Stage.GOODNIGHT) {
            // Someone is here after all: stay awake.
            ReceiverState.update { it.copy(wokeScreen = false) }
            finish()
            return true
        }
        if (ReceiverState.current.wokeScreen) ReceiverState.update { it.copy(wokeScreen = false) }
        val wasDark = screenStage == Stage.AMBIENT || screenStage == Stage.BLANK
        wake()
        if (wasDark && keyCode != KeyEvent.KEYCODE_BACK) return true // the first press only lights the screen up
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_DOWN -> openLyrics()
            KeyEvent.KEYCODE_DPAD_UP -> closeLyrics()
            KeyEvent.KEYCODE_BACK -> if (lyricsOpen) closeLyrics() else return super.onKeyDown(keyCode, event)
            else -> {
                val command = commandFor(keyCode) ?: return super.onKeyDown(keyCode, event)
                if (event.repeatCount == 0) {
                    ReceiverService.sendRemoteCommand(this, command)
                    pulse(
                        when (command) {
                            Command.NEXT -> nextButton
                            Command.PREVIOUS -> previousButton
                            Command.PLAY_PAUSE -> playPauseButton
                        },
                    )
                }
            }
        }
        return true
    }

    private fun commandFor(keyCode: Int): Command? = when (keyCode) {
        KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, KeyEvent.KEYCODE_MEDIA_PLAY, KeyEvent.KEYCODE_MEDIA_PAUSE,
        KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> Command.PLAY_PAUSE
        KeyEvent.KEYCODE_MEDIA_NEXT, KeyEvent.KEYCODE_MEDIA_FAST_FORWARD, KeyEvent.KEYCODE_DPAD_RIGHT -> Command.NEXT
        KeyEvent.KEYCODE_MEDIA_PREVIOUS, KeyEvent.KEYCODE_MEDIA_REWIND, KeyEvent.KEYCODE_DPAD_LEFT -> Command.PREVIOUS
        else -> null
    }

    /** A short pulse acknowledges a key before the sender has answered. */
    private fun pulse(view: View) {
        view.animate().cancel()
        view.animate().scaleX(1.22f).scaleY(1.22f).setDuration(110)
            .withEndAction { view.animate().scaleX(1f).scaleY(1f).setDuration(220).setInterpolator(OvershootInterpolator(2.5f)).start() }
            .start()
    }

    // ---------------------------------------------------------------- OLED care

    /** Something the viewer should see happened: back to full brightness. */
    private fun wake() {
        lastInteraction = SystemClock.elapsedRealtime()
        holdStageUntil = 0
        if (screenStage != Stage.ACTIVE && screenStage != Stage.GOODNIGHT) setStage(Stage.ACTIVE)
    }

    private fun chooseStage() {
        val now = SystemClock.elapsedRealtime()
        if (screenStage == Stage.GOODNIGHT || now < holdStageUntil) return
        val idle = now - lastInteraction
        val longPause = pausedSince > 0 && now - pausedSince >= BLANK_AFTER_PAUSE_MS
        setStage(
            when {
                idle >= AMBIENT_AFTER_MS && (longPause || screenStage == Stage.BLANK) -> Stage.BLANK
                idle >= AMBIENT_AFTER_MS -> Stage.AMBIENT
                idle >= DIM_AFTER_MS -> Stage.DIM
                else -> Stage.ACTIVE
            },
        )
    }

    private fun setStage(next: Stage) {
        if (next == screenStage) return
        val fromDark = screenStage != Stage.ACTIVE && screenStage != Stage.DIM
        screenStage = next
        stage.animate().cancel()
        if (next == Stage.BLANK) {
            // black, and the system may let the screen sleep by its own timer
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
        when (next) {
            Stage.ACTIVE -> stage.animate().alpha(1f).setDuration(if (fromDark) 900 else 300).start()
            Stage.DIM -> stage.animate().alpha(DIM_ALPHA).setDuration(1500).start()
            Stage.AMBIENT -> stage.animate().alpha(0f).setDuration(1800).start()
            Stage.BLANK -> stage.animate().alpha(0f).setDuration(1200).start()
            Stage.GOODNIGHT -> stage.animate().alpha(0f).setDuration(1500).start()
        }
        if (next == Stage.AMBIENT) showAmbient() else hideAmbient()
        if (next == Stage.GOODNIGHT) {
            goodnightView.visibility = View.VISIBLE
            goodnightView.animate().alpha(0.55f).setDuration(1500).start()
        } else {
            handler.removeCallbacks(goodnightTick)
            goodnightView.animate().alpha(0f).setDuration(300).withEndAction {
                if (screenStage != Stage.GOODNIGHT) goodnightView.visibility = View.GONE
            }.start()
        }
        animateColors(colors, animate = true)
    }

    private fun showAmbient() {
        ambient.visibility = View.VISIBLE
        ambient.animate().cancel()
        ambient.animate().alpha(AMBIENT_ALPHA).setDuration(1800).start()
        placeAmbient(animate = false)
    }

    private fun hideAmbient() {
        ambient.animate().cancel()
        ambient.animate().alpha(0f).setDuration(600).withEndAction {
            if (screenStage != Stage.AMBIENT) ambient.visibility = View.GONE
        }.start()
    }

    /** Moves the ambient card along a slow loop over the whole screen. */
    private fun placeAmbient(animate: Boolean) {
        ambient.post {
            val t = (SystemClock.elapsedRealtime() + if (animate) AMBIENT_STEP_MS else 0L) / 60_000.0
            val margin = 40 * resources.displayMetrics.density
            val root = ambient.parent as View
            val unspecified = View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED)
            ambient.measure(unspecified, unspecified) // it may not have been laid out since it appeared
            val maxX = (root.width - ambient.measuredWidth - 2 * margin).coerceAtLeast(0f)
            val maxY = (root.height - ambient.measuredHeight - 2 * margin).coerceAtLeast(0f)
            val x = margin + maxX * ((sin(2 * PI * t / AMBIENT_PERIOD_X_MIN) + 1) / 2).toFloat()
            val y = margin + maxY * ((sin(2 * PI * t / AMBIENT_PERIOD_Y_MIN + 1.3) + 1) / 2).toFloat()
            if (animate) {
                ambient.animate().translationX(x).translationY(y).setDuration(AMBIENT_STEP_MS)
                    .setInterpolator(LinearInterpolator()).start()
            } else {
                ambient.translationX = x
                ambient.translationY = y
            }
        }
    }

    // ---------------------------------------------------------------- sleep after music

    private fun musicEnded(s: ReceiverState.Snapshot) {
        if (screenStage == Stage.GOODNIGHT) return
        val minutes = settings.sleepAfterMinutes
        if (s.wokeScreen && minutes > 0 && SleepService.available) startGoodnight(minutes * 60_000L) else finish()
    }

    private fun startGoodnight(durationMs: Long) {
        goodnightEnd = SystemClock.elapsedRealtime() + durationMs
        setStage(Stage.GOODNIGHT)
        handler.removeCallbacks(goodnightTick)
        handler.post(goodnightTick)
    }

    /** Time is up: the TV goes to sleep, unless something else is playing music by now. */
    private fun goToSleep() {
        val audio = getSystemService(AUDIO_SERVICE) as AudioManager
        if (!preview && !audio.isMusicActive) SleepService.sleep()
        finish()
    }

    // ---------------------------------------------------------------- preview (debug builds)

    /** Shows a part of the screen that otherwise needs time or a remote, for the preview tool. */
    private fun showPreview(part: String) {
        val hold = { stage: Stage ->
            holdStageUntil = SystemClock.elapsedRealtime() + PREVIEW_HOLD_MS
            setStage(stage)
        }
        when (part) {
            "lyrics" -> handler.postDelayed({ openLyrics() }, 700)
            "dim" -> hold(Stage.DIM)
            "ambient" -> hold(Stage.AMBIENT)
            "blank" -> hold(Stage.BLANK)
            "goodnight" -> startGoodnight(5 * 60_000L)
        }
    }

    companion object {
        /** Debug builds only: a part of the screen to show at once, see [showPreview]. */
        const val EXTRA_PREVIEW = "preview"

        private const val MINUS = "−"
        private const val BAR_TRACK = 0xFF2E2E32.toInt()
        private const val PAUSED_COVER_SCALE = 0.9f

        private const val TICK_MS = 250L
        private const val STAGE_TICK_MS = 5_000L
        private const val LYRICS_LEAD_MS = 150L
        private const val VOLUME_SETTLE_MS = 1400L
        private const val HINT_MS = 7000L
        private const val PREVIEW_HOLD_MS = 120_000L

        private const val TEXT_SHIFT_DP = 28f
        private const val TEXT_OUT_MS = 130L
        private const val TEXT_IN_MS = 300L
        private const val COVER_SHIFT_DP = 56f
        private const val COVER_IN_MS = 460L
        private const val COVER_OUT_MS = 380L
        private const val PALETTE_MS = 700L

        // OLED care: when each stage begins, and how the ambient card wanders
        private const val DIM_AFTER_MS = 90_000L
        private const val AMBIENT_AFTER_MS = 5 * 60_000L
        private const val BLANK_AFTER_PAUSE_MS = 15 * 60_000L
        private const val DIM_ALPHA = 0.45f
        private const val AMBIENT_ALPHA = 0.6f
        private const val AMBIENT_STEP_MS = 20_000L
        private const val AMBIENT_PERIOD_X_MIN = 2.9
        private const val AMBIENT_PERIOD_Y_MIN = 4.1
    }
}
