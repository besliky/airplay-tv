package io.github.besliky.airplaytv.car

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.Settings as SystemSettings
import android.view.Gravity
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import io.github.besliky.airplaytv.R
import io.github.besliky.airplaytv.Settings
import io.github.besliky.airplaytv.core.NativeBridge
import io.github.besliky.airplaytv.service.DacpClient
import io.github.besliky.airplaytv.service.NowPlaying
import io.github.besliky.airplaytv.service.ReceiverService
import io.github.besliky.airplaytv.service.ReceiverState
import io.github.besliky.airplaytv.service.ReceiverState.Status
import io.github.besliky.airplaytv.ui.Dialogs
import io.github.besliky.airplaytv.ui.MainActivity
import io.github.besliky.airplaytv.ui.Page
import io.github.besliky.airplaytv.ui.PerformanceOverlay
import io.github.besliky.airplaytv.ui.showOverStandby

/**
 * The car app in one window, laid out like a car's own phone integration: a dock along the
 * driver's side (clock, network, Home, the phone's screen, Now Playing, Settings) and the page
 * next to it.
 *
 * - Home: the clock, what is playing with touch controls, and what to do next to connect.
 * - Music: Now Playing with large controls; they reach the phone over DACP, like the steering
 *   wheel buttons do through the media session.
 * - Mirror: the phone's picture, decoded straight into a SurfaceView of this window. A portrait
 *   picture stands next to the dock with a side panel; a landscape picture takes the whole screen
 *   and the dock steps aside until the screen is touched.
 * - Setup: how to connect, step by step.
 *
 * The receiver opens this activity for mirroring and music ([EXTRA_PAGE]); the session goes on
 * when the driver switches pages, only the picture is not drawn while another page is open.
 */
class CarActivity : Activity(), SurfaceHolder.Callback {

    private enum class Screen { HOME, MUSIC, MIRROR, SETUP }

    private lateinit var settings: Settings
    private val handler = Handler(Looper.getMainLooper())

    private lateinit var content: LinearLayout
    private lateinit var dock: View
    private lateinit var pages: FrameLayout
    private lateinit var dockSignal: ImageView
    private lateinit var dockHome: View
    private lateinit var dockMirror: View
    private lateinit var dockMusic: View
    private lateinit var dockCover: ImageView
    private lateinit var dockMusicIcon: ImageView
    private lateinit var peek: LinearLayout
    private lateinit var peekMusic: View

    private lateinit var pageHome: View
    private lateinit var pageMusic: View
    private lateinit var pageMirror: FrameLayout
    private lateinit var pageSetup: View

    private lateinit var homeTrack: View
    private lateinit var homeNoTrack: View
    private lateinit var homeNoTrackHint: TextView
    private lateinit var homeStatusDot: View
    private lateinit var homeStatus: TextView
    private lateinit var homeStatusDetail: TextView
    private lateinit var homeNetwork: TextView
    private lateinit var homeAction: Button
    private lateinit var homeSecondary: Button

    private lateinit var surface: SurfaceView
    private lateinit var side: View
    private lateinit var sideTrack: View
    private lateinit var sideClient: TextView
    private lateinit var standby: View
    private lateinit var standbyText: TextView
    private lateinit var pinView: TextView
    private lateinit var performance: PerformanceOverlay

    private lateinit var homeViews: TrackViews
    private lateinit var musicViews: TrackViews
    private lateinit var sideViews: TrackViews
    private lateinit var musicElapsed: TextView
    private lateinit var musicRemaining: TextView

    private lateinit var setupAllow: Button

    private var screen = Screen.HOME
    private var last = ReceiverState.Snapshot()
    private var shownArtworkSeq = -1

    /** A landscape picture fills the screen; a touch brings up a few buttons over it for a few seconds. */
    private var dockPeek = false
    private val hidePeek = Runnable {
        dockPeek = false
        render(ReceiverState.current)
    }

    private val stateListener: (ReceiverState.Snapshot) -> Unit = { render(it) }
    private val prefsListener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
        if (key == Settings.KEY_DOCK_RIGHT) placeDock()
        render(ReceiverState.current)
    }
    private val tick = object : Runnable {
        override fun run() {
            updateProgress(ReceiverState.current.nowPlaying)
            handler.postDelayed(this, TICK_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_car)
        settings = Settings(this)

        content = findViewById(R.id.content)
        dock = findViewById(R.id.dock)
        pages = findViewById(R.id.pages)
        dockSignal = findViewById(R.id.dockSignal)
        dockHome = findViewById(R.id.dockHome)
        dockMirror = findViewById(R.id.dockMirror)
        dockMusic = findViewById(R.id.dockMusic)
        dockCover = findViewById(R.id.dockCover)
        dockMusicIcon = findViewById(R.id.dockMusicIcon)
        peek = findViewById(R.id.peek)
        peekMusic = findViewById(R.id.peekMusic)
        pageHome = findViewById(R.id.pageHome)
        pageMusic = findViewById(R.id.pageMusic)
        pageMirror = findViewById(R.id.pageMirror)
        pageSetup = findViewById(R.id.pageSetup)

        homeTrack = findViewById(R.id.homeTrack)
        homeNoTrack = findViewById(R.id.homeNoTrack)
        homeNoTrackHint = findViewById(R.id.homeNoTrackHint)
        homeStatusDot = findViewById(R.id.homeStatusDot)
        homeStatus = findViewById(R.id.homeStatus)
        homeStatusDetail = findViewById(R.id.homeStatusDetail)
        homeNetwork = findViewById(R.id.homeNetwork)
        homeAction = findViewById(R.id.homeAction)
        homeSecondary = findViewById(R.id.homeSecondary)

        surface = findViewById(R.id.surface)
        side = findViewById(R.id.side)
        sideTrack = findViewById(R.id.sideTrack)
        sideClient = findViewById(R.id.sideClient)
        standby = findViewById(R.id.standby)
        standbyText = findViewById(R.id.standbyText)
        pinView = findViewById(R.id.pin)
        performance = PerformanceOverlay(findViewById(R.id.overlay))

        homeViews = TrackViews(R.id.homeCover, R.id.homeTitle, R.id.homeArtist, null, R.id.homeProgress,
            R.id.homePrevious, R.id.homePlay, R.id.homeNext)
        musicViews = TrackViews(R.id.musicCover, R.id.musicTitle, R.id.musicArtist, R.id.musicAlbum, R.id.musicProgress,
            R.id.musicPrevious, R.id.musicPlay, R.id.musicNext)
        sideViews = TrackViews(R.id.sideCover, R.id.sideTitle, R.id.sideArtist, null, null,
            R.id.sidePrevious, R.id.sidePlay, R.id.sideNext)
        for (cover in listOf(homeViews.cover, musicViews.cover, sideViews.cover, dockCover)) cover.clipToOutline = true
        musicElapsed = findViewById(R.id.musicElapsed)
        musicRemaining = findViewById(R.id.musicRemaining)

        dockHome.setOnClickListener { show(Screen.HOME) }
        dockMirror.setOnClickListener { show(Screen.MIRROR) }
        dockMusic.setOnClickListener { show(Screen.MUSIC) }
        findViewById<View>(R.id.dockSettings).setOnClickListener { openSettings() }
        findViewById<View>(R.id.peekHome).setOnClickListener { show(Screen.HOME) }
        peekMusic.setOnClickListener { show(Screen.MUSIC) }
        findViewById<View>(R.id.peekStop).setOnClickListener { disconnect() }
        findViewById<View>(R.id.homeMusicCard).setOnClickListener {
            if (ReceiverState.current.nowPlaying.hasTrack) show(Screen.MUSIC)
        }
        sideTrack.setOnClickListener { show(Screen.MUSIC) }
        findViewById<View>(R.id.sideDisconnect).setOnClickListener { disconnect() }
        // the picture cannot be touched through to the phone; a touch only brings the dock back
        pageMirror.setOnClickListener { peekDock() }

        surface.holder.addCallback(this)
        pageMirror.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> layoutMirror(ReceiverState.current) }
        pageMusic.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> sizeMusicCover() }
        buildSetup()
        placeDock()

        show(pageFrom(intent) ?: Screen.HOME, fromUser = false)
        dockHome.requestFocus()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        pageFrom(intent)?.let { show(it, fromUser = false) }
    }

    override fun onStart() {
        super.onStart()
        if (settings.enabled) ReceiverService.start(this)
        settings.registerListener(prefsListener)
        placeDock() // the side may have changed in Settings
        ReceiverState.observe(stateListener)
        handler.post(tick)
        bindSetup()
    }

    override fun onStop() {
        ReceiverState.remove(stateListener)
        settings.unregisterListener(prefsListener)
        handler.removeCallbacks(tick)
        handler.removeCallbacks(hidePeek)
        performance.show(false)
        super.onStop()
    }

    private fun pageFrom(intent: Intent?): Screen? = when (intent?.getStringExtra(EXTRA_PAGE)) {
        Page.MIRROR.name -> Screen.MIRROR
        Page.MUSIC.name -> Screen.MUSIC
        Page.HOME.name -> Screen.HOME
        else -> null
    }

    // ---- pages ----

    private fun show(target: Screen, fromUser: Boolean = true) {
        val s = ReceiverState.current
        // Pages without content fall back to Home, e.g. a music request that arrives after the music ended.
        val next = when {
            target == Screen.MIRROR && !hasMirror(s) && !fromUser -> Screen.HOME
            target == Screen.MUSIC && !s.nowPlaying.hasTrack && !s.audioActive && !fromUser -> Screen.HOME
            else -> target
        }
        if (next == Screen.MIRROR) showOverStandby()
        screen = next
        dockPeek = false
        handler.removeCallbacks(hidePeek)
        render(s)
    }

    private fun hasMirror(s: ReceiverState.Snapshot) = s.videoActive || s.pin != null

    private fun placeDock() {
        val wanted = if (settings.dockOnRight) 1 else 0
        if (content.indexOfChild(dock) != wanted) {
            content.removeView(dock)
            content.addView(dock, wanted)
        }
    }

    private fun peekDock() {
        dockPeek = true
        handler.removeCallbacks(hidePeek)
        handler.postDelayed(hidePeek, PEEK_MS)
        render(ReceiverState.current)
    }

    private fun openSettings() {
        startActivity(Intent(this, MainActivity::class.java))
    }

    private fun disconnect() {
        ReceiverService.disconnect(this)
        show(Screen.HOME)
    }

    // ---- state ----

    private fun render(s: ReceiverState.Snapshot) {
        val before = last
        last = s
        // The picture starts: show it, as a TV with AirPlay does. It ends: back to what is left.
        if (hasMirror(s) && !hasMirror(before)) screen = Screen.MIRROR
        if (screen == Screen.MIRROR && !hasMirror(s)) screen = if (s.nowPlaying.hasTrack && s.audioActive) Screen.MUSIC else Screen.HOME

        val mirroring = screen == Screen.MIRROR
        pageHome.visibility = if (screen == Screen.HOME) View.VISIBLE else View.GONE
        pageMusic.visibility = if (screen == Screen.MUSIC) View.VISIBLE else View.GONE
        pageSetup.visibility = if (screen == Screen.SETUP) View.VISIBLE else View.GONE
        // Hiding the SurfaceView releases its surface; the decoder then keeps the session alive off screen.
        pageMirror.visibility = if (mirroring) View.VISIBLE else View.GONE
        performance.show(mirroring && settings.performanceOverlay)
        if (s.clientName != null || mirroring) {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }

        renderDock(s)
        renderTrack(s.nowPlaying)
        when (screen) {
            Screen.HOME -> renderHome(s)
            Screen.MIRROR -> renderMirror(s)
            else -> Unit
        }
    }

    private fun renderDock(s: ReceiverState.Snapshot) {
        // Maps and other landscape pictures get the whole screen; the buttons float over them on a touch,
        // so the picture never moves under the driver's eyes.
        val landscapePicture = screen == Screen.MIRROR && pictureShown(s) && s.videoWidth > s.videoHeight * 1.2
        dock.visibility = if (landscapePicture) View.GONE else View.VISIBLE
        peek.visibility = if (landscapePicture && dockPeek) View.VISIBLE else View.GONE
        peekMusic.visibility = if (s.nowPlaying.hasTrack) View.VISIBLE else View.GONE
        (peek.layoutParams as FrameLayout.LayoutParams).let { lp ->
            val gravity = (if (settings.dockOnRight) Gravity.END else Gravity.START) or Gravity.CENTER_VERTICAL
            if (lp.gravity != gravity) {
                lp.gravity = gravity
                lp.marginEnd = lp.marginStart
                peek.layoutParams = lp
            }
        }
        dockHome.isSelected = screen == Screen.HOME || screen == Screen.SETUP
        dockMirror.isSelected = screen == Screen.MIRROR
        dockMusic.isSelected = screen == Screen.MUSIC
        dockMirror.visibility = if (hasMirror(s)) View.VISIBLE else View.GONE
        // Mirroring sound alone is no music to show; the music page needs a track.
        dockMusic.visibility = if (s.nowPlaying.hasTrack) View.VISIBLE else View.GONE
        val online = settings.enabled && s.status != Status.NO_NETWORK && s.status != Status.OFF && s.addresses.isNotEmpty()
        dockSignal.setColorFilter(getColor(if (online) R.color.text_primary else R.color.text_secondary))
        dockSignal.alpha = if (online) 1f else 0.45f
    }

    private fun renderTrack(np: NowPlaying) {
        if (np.artworkSeq != shownArtworkSeq) {
            shownArtworkSeq = np.artworkSeq
            for (v in listOf(homeViews, musicViews, sideViews)) v.cover.setImageBitmap(np.artwork)
            dockCover.setImageBitmap(np.artwork)
            pageMusic.background = backdrop(np)
        }
        dockCover.visibility = if (np.artwork != null) View.VISIBLE else View.GONE
        dockMusicIcon.visibility = if (np.artwork != null) View.INVISIBLE else View.VISIBLE
        for (v in listOf(homeViews, musicViews, sideViews)) v.bind(np)
        updateProgress(np)
    }

    /** The cover is a square as tall as the page allows, leaving the texts at least half the width. */
    private fun sizeMusicCover() {
        val cover = musicViews.cover
        val side = minOf(
            pageMusic.height - pageMusic.paddingTop - pageMusic.paddingBottom,
            (pageMusic.width - pageMusic.paddingLeft - pageMusic.paddingRight) / 2,
            resources.getDimensionPixelSize(R.dimen.car_music_cover_max),
        )
        if (side <= 0) return
        val lp = cover.layoutParams
        if (lp.width != side || lp.height != side) {
            lp.width = side
            lp.height = side
            cover.post { cover.layoutParams = lp }
        }
    }

    private fun updateProgress(np: NowPlaying) {
        val now = SystemClock.elapsedRealtime()
        val position = np.positionAt(now)
        val fraction = if (position >= 0 && np.durationMs > 0) (position * 1000 / np.durationMs).toInt() else 0
        homeViews.progress?.progress = fraction
        musicViews.progress?.progress = fraction
        val known = position >= 0
        musicElapsed.text = if (known) clock(position) else ""
        musicRemaining.text = if (known) "−" + clock(np.durationMs - position) else ""
    }

    private fun renderHome(s: ReceiverState.Snapshot) {
        val np = s.nowPlaying
        val name = s.publishedName.ifEmpty { settings.deviceName }
        homeTrack.visibility = if (np.hasTrack) View.VISIBLE else View.GONE
        homeNoTrack.visibility = if (np.hasTrack) View.GONE else View.VISIBLE
        homeNoTrackHint.text = getString(R.string.car_nothing_playing_hint, name)

        val status = if (!settings.enabled) Status.OFF else s.status
        val client = s.clientName ?: "iPhone"
        homeStatus.text = when (status) {
            Status.OFF -> getString(R.string.car_off)
            Status.STARTING -> getString(R.string.car_starting)
            Status.READY -> getString(R.string.car_ready, name)
            Status.NO_NETWORK -> getString(R.string.car_no_network)
            Status.ERROR -> getString(R.string.status_error)
            Status.CONNECTED -> getString(R.string.car_connected, client)
        }
        homeStatusDetail.text = when (status) {
            Status.OFF -> getString(R.string.car_off_detail)
            Status.NO_NETWORK -> getString(R.string.car_no_network_detail)
            Status.ERROR -> getString(R.string.car_error_detail)
            Status.CONNECTED -> getString(if (s.videoActive) R.string.car_mirroring_detail else R.string.car_music_detail)
            else -> getString(R.string.car_ready_detail, name)
        }
        homeStatusDot.background.mutate().setTint(
            getColor(
                when (status) {
                    Status.READY, Status.CONNECTED -> R.color.status_ready
                    Status.OFF -> R.color.text_secondary
                    else -> R.color.status_warning
                },
            ),
        )
        val address = s.addresses.firstOrNull()
        homeNetwork.text = if (address != null && settings.showConnectionInfo) getString(R.string.car_network_wifi, address) else ""
        homeNetwork.visibility = if (homeNetwork.text.isEmpty()) View.GONE else View.VISIBLE

        when {
            status == Status.CONNECTED && s.videoActive -> {
                action(homeAction, R.string.car_action_show) { show(Screen.MIRROR) }
                action(homeSecondary, R.string.car_action_disconnect) { disconnect() }
            }
            status == Status.CONNECTED -> {
                action(homeAction, R.string.car_action_disconnect) { disconnect() }
                action(homeSecondary, R.string.car_action_setup) { show(Screen.SETUP) }
            }
            status == Status.NO_NETWORK -> {
                action(homeAction, R.string.car_action_wifi) { openWifiSettings() }
                action(homeSecondary, R.string.car_action_setup) { show(Screen.SETUP) }
            }
            status == Status.OFF || status == Status.ERROR -> {
                action(homeAction, R.string.car_action_settings) { openSettings() }
                action(homeSecondary, R.string.car_action_setup) { show(Screen.SETUP) }
            }
            !canOpenAutomatically() -> {
                action(homeAction, R.string.car_action_allow) { requestAutoOpen() }
                action(homeSecondary, R.string.car_action_setup) { show(Screen.SETUP) }
            }
            else -> {
                action(homeAction, R.string.car_action_setup) { show(Screen.SETUP) }
                action(homeSecondary, R.string.car_action_wifi) { openWifiSettings() }
            }
        }
    }

    private fun action(button: Button, label: Int, onClick: () -> Unit) {
        button.setText(label)
        button.setOnClickListener { onClick() }
    }

    private fun pictureShown(s: ReceiverState.Snapshot) = s.videoActive && s.videoWidth > 0 && s.pin == null

    private fun renderMirror(s: ReceiverState.Snapshot) {
        val client = s.clientName ?: "iPhone"
        standby.visibility = if (pictureShown(s)) View.GONE else View.VISIBLE
        if (s.pin != null) {
            pinView.visibility = View.VISIBLE
            pinView.text = s.pin
            standbyText.text = getString(R.string.pin_hint)
        } else {
            pinView.visibility = View.GONE
            standbyText.text = if (s.clientName == null) getString(R.string.playback_connecting) else getString(R.string.car_waiting_for, client)
        }
        sideClient.text = getString(R.string.car_mirroring_from, client)
        sideTrack.visibility = if (s.nowPlaying.hasTrack) View.VISIBLE else View.GONE
        layoutMirror(s)
    }

    /**
     * Fits the picture into the page. A portrait picture (navigation held upright) stands next to the
     * dock, close to the driver, and the side panel takes the room that is left when there is enough
     * of it; otherwise the picture is centred.
     */
    private fun layoutMirror(s: ReceiverState.Snapshot) {
        val pw = pageMirror.width
        val ph = pageMirror.height
        if (pw <= 0 || ph <= 0) return
        var w = pw
        var h = ph
        if (s.videoWidth > 0 && s.videoHeight > 0) {
            val aspect = s.videoWidth.toDouble() / s.videoHeight
            if (pw.toDouble() / ph > aspect) w = (ph * aspect).toInt() else h = (pw / aspect).toInt()
        }
        val room = pw - w
        val minSide = resources.getDimensionPixelSize(R.dimen.car_side_min_width)
        val maxSide = resources.getDimensionPixelSize(R.dimen.car_side_max_width)
        val withSide = pictureShown(s) && room >= minSide
        val sideWidth = if (withSide) room else 0
        val dockRight = settings.dockOnRight
        val pictureLeft = when {
            !withSide -> (pw - w) / 2
            dockRight -> sideWidth
            else -> 0
        }

        val lp = surface.layoutParams as FrameLayout.LayoutParams
        val top = (ph - h) / 2
        if (lp.width != w || lp.height != h || lp.leftMargin != pictureLeft || lp.topMargin != top || lp.gravity != (Gravity.TOP or Gravity.START)) {
            lp.width = w
            lp.height = h
            lp.gravity = Gravity.TOP or Gravity.START
            lp.leftMargin = pictureLeft
            lp.topMargin = top
            surface.layoutParams = lp
        }
        side.visibility = if (withSide) View.VISIBLE else View.GONE
        if (withSide) {
            val sp = side.layoutParams as FrameLayout.LayoutParams
            val sideLeft = if (dockRight) 0 else w
            // the panel's content keeps a comfortable width in the middle of a wide room
            val inset = maxOf(resources.getDimensionPixelSize(R.dimen.car_page_padding), (sideWidth - maxSide) / 2)
            side.setPadding(inset, side.paddingTop, inset, side.paddingBottom)
            if (sp.width != sideWidth || sp.leftMargin != sideLeft || sp.gravity != (Gravity.TOP or Gravity.START)) {
                sp.width = sideWidth
                sp.gravity = Gravity.TOP or Gravity.START
                sp.leftMargin = sideLeft
                side.layoutParams = sp
            }
        }
    }

    // ---- surface ----

    override fun surfaceCreated(holder: SurfaceHolder) {
        NativeBridge.nativeSetSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // Must be synchronous: the decoder moves to a background surface before this returns.
        NativeBridge.nativeSetSurface(null)
    }

    // ---- setup guide ----

    private fun buildSetup() {
        val steps = findViewById<LinearLayout>(R.id.setupSteps)
        val name = settings.deviceName
        fun step(number: String, title: Int, text: String, button: Int = 0, onClick: (() -> Unit)? = null): Button? {
            val view = LayoutInflater.from(this).inflate(R.layout.view_car_step, steps, false)
            view.findViewById<TextView>(R.id.stepNumber).text = number
            view.findViewById<TextView>(R.id.stepTitle).setText(title)
            view.findViewById<TextView>(R.id.stepText).text = text
            val b = view.findViewById<Button>(R.id.stepButton)
            if (onClick != null) {
                b.setText(button)
                b.setOnClickListener { onClick() }
                b.visibility = View.VISIBLE
            }
            steps.addView(view)
            return b.takeIf { onClick != null }
        }
        step("1", R.string.car_setup_1_title, getString(R.string.car_setup_1_text))
        step("2", R.string.car_setup_2_title, getString(R.string.car_setup_2_text), R.string.car_setup_wifi) { openWifiSettings() }
        setupAllow = step("3", R.string.car_setup_3_title, getString(R.string.car_setup_3_text), R.string.car_setup_allow) {
            requestAutoOpen()
        }!!
        step("4", R.string.car_setup_4_title, getString(R.string.car_setup_4_text, name))
        step("5", R.string.car_setup_maps_title, getString(R.string.car_setup_maps_text))
        step("6", R.string.car_setup_5_title, getString(R.string.car_setup_5_text, name))
        step("i", R.string.car_setup_tip_title, getString(R.string.car_setup_tip_text))
    }

    private fun bindSetup() {
        val allowed = canOpenAutomatically()
        setupAllow.setText(if (allowed) R.string.car_setup_allowed else R.string.car_setup_allow)
        setupAllow.isEnabled = !allowed
        setupAllow.alpha = if (allowed) 0.5f else 1f
    }

    private fun canOpenAutomatically(): Boolean = Build.VERSION.SDK_INT < 29 || SystemSettings.canDrawOverlays(this)

    private fun requestAutoOpen() {
        if (canOpenAutomatically()) return
        val intent = Intent(SystemSettings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName"))
        try {
            startActivity(intent)
        } catch (_: ActivityNotFoundException) {
            Dialogs.message(this, getString(R.string.dialog_overlay_title), getString(R.string.dialog_overlay_message, packageName))
        } catch (_: SecurityException) {
            Dialogs.message(this, getString(R.string.dialog_overlay_title), getString(R.string.dialog_overlay_message, packageName))
        }
    }

    private fun openWifiSettings() {
        for (action in listOf(SystemSettings.ACTION_WIFI_SETTINGS, SystemSettings.ACTION_WIRELESS_SETTINGS, SystemSettings.ACTION_SETTINGS)) {
            try {
                startActivity(Intent(action))
                return
            } catch (_: ActivityNotFoundException) {
            } catch (_: SecurityException) {
            }
        }
    }

    // ---- keys ----

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && screen != Screen.HOME) {
            // Back leaves the page, not the session: music and mirroring go on, Stop Mirroring ends them.
            show(Screen.HOME)
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    // ---- helpers ----

    /** The cover, texts, progress and buttons of one place that shows the track. */
    private inner class TrackViews(
        coverId: Int, titleId: Int, artistId: Int, albumId: Int?, progressId: Int?,
        previousId: Int, playId: Int, nextId: Int,
    ) {
        val cover: ImageView = findViewById(coverId)
        val title: TextView = findViewById(titleId)
        val artist: TextView = findViewById(artistId)
        val album: TextView? = albumId?.let { findViewById(it) }
        val progress: ProgressBar? = progressId?.let { findViewById(it) }
        val play: ImageButton = findViewById(playId)

        init {
            findViewById<View>(previousId).setOnClickListener { send(DacpClient.Command.PREVIOUS) }
            play.setOnClickListener { send(DacpClient.Command.PLAY_PAUSE) }
            findViewById<View>(nextId).setOnClickListener { send(DacpClient.Command.NEXT) }
        }

        fun bind(np: NowPlaying) {
            title.text = np.title
            artist.text = np.artist
            album?.text = np.album
            album?.visibility = if (np.album.isBlank()) View.GONE else View.VISIBLE
            play.setImageResource(if (np.playing) R.drawable.ic_pause else R.drawable.ic_play)
        }
    }

    private fun send(command: DacpClient.Command) = ReceiverService.sendRemoteCommand(this, command)

    private fun backdrop(np: NowPlaying) = android.graphics.drawable.GradientDrawable(
        android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM,
        intArrayOf(np.colors.backdropTop, np.colors.backdropBottom),
    )

    private fun clock(ms: Long): String {
        val seconds = (ms.coerceAtLeast(0) + 500) / 1000
        return "%d:%02d".format(seconds / 60, seconds % 60)
    }

    companion object {
        const val EXTRA_PAGE = "page"
        private const val TICK_MS = 500L
        private const val PEEK_MS = 5_000L
    }
}
