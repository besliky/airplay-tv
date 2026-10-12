package io.github.besliky.airplaytv.ui

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.KeyEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.FrameLayout
import android.widget.TextView
import io.github.besliky.airplaytv.R
import io.github.besliky.airplaytv.Settings
import io.github.besliky.airplaytv.core.NativeBridge
import io.github.besliky.airplaytv.service.ReceiverState

/**
 * Full-screen playback. The decoder renders straight into [surface]; this activity only
 * sizes the surface to the picture's aspect ratio and shows the standby, PIN and
 * performance overlays around it. Back ends the mirroring session.
 */
class MirrorActivity : Activity(), SurfaceHolder.Callback {

    private lateinit var root: FrameLayout
    private lateinit var surface: SurfaceView
    private lateinit var standby: View
    private lateinit var standbyText: TextView
    private lateinit var standbyInfo: TextView
    private lateinit var pinView: TextView
    private lateinit var overlay: TextView
    private lateinit var settings: Settings

    private val handler = Handler(Looper.getMainLooper())
    private var overlayStats: OverlayStats? = null

    private val stateListener: (ReceiverState.Snapshot) -> Unit = { render(it) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_mirror)
        showOverStandby()
        settings = Settings(this)
        root = findViewById(R.id.root)
        surface = findViewById(R.id.surface)
        standby = findViewById(R.id.standby)
        standbyText = findViewById(R.id.standbyText)
        standbyInfo = findViewById(R.id.standbyInfo)
        pinView = findViewById(R.id.pin)
        overlay = findViewById(R.id.overlay)
        surface.holder.addCallback(this)
        root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> layoutSurface(ReceiverState.current) }
    }

    override fun onStart() {
        super.onStart()
        ReceiverState.observe(stateListener)
        if (settings.performanceOverlay) {
            overlay.visibility = View.VISIBLE
            overlayStats = OverlayStats().also { handler.post(overlayTick) }
        } else {
            overlay.visibility = View.GONE
        }
    }

    override fun onStop() {
        ReceiverState.remove(stateListener)
        handler.removeCallbacks(overlayTick)
        overlayStats = null
        super.onStop()
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

    private fun layoutSurface(s: ReceiverState.Snapshot) {
        val pw = root.width
        val ph = root.height
        if (pw <= 0 || ph <= 0) return
        val lp = surface.layoutParams as FrameLayout.LayoutParams
        var w = pw
        var h = ph
        if (s.videoWidth > 0 && s.videoHeight > 0) {
            val videoAspect = s.videoWidth.toDouble() / s.videoHeight
            if (pw.toDouble() / ph > videoAspect) {
                w = (ph * videoAspect).toInt()
            } else {
                h = (pw / videoAspect).toInt()
            }
        }
        if (lp.width != w || lp.height != h || lp.gravity != Gravity.CENTER) {
            lp.width = w
            lp.height = h
            lp.gravity = Gravity.CENTER
            surface.layoutParams = lp
        }
    }

    // ---- state ----

    private fun render(s: ReceiverState.Snapshot) {
        val sessionOver = s.clientName == null && s.pin == null && !s.videoActive
        if (sessionOver) {
            finish()
            return
        }
        layoutSurface(s)

        val client = s.clientName ?: "iPhone"
        val showPicture = s.videoActive && s.videoWidth > 0 && s.pin == null
        standby.visibility = if (showPicture) View.GONE else View.VISIBLE
        if (s.pin != null) {
            pinView.visibility = View.VISIBLE
            pinView.text = s.pin
            standbyText.text = getString(R.string.pin_hint)
        } else {
            pinView.visibility = View.GONE
            standbyText.text = when {
                !s.videoActive && s.audioActive -> getString(R.string.playback_audio_only, client)
                s.clientName == null -> getString(R.string.playback_connecting)
                else -> getString(R.string.playback_waiting_for, client)
            }
        }
        val address = s.addresses.firstOrNull()
        standbyInfo.text = if (settings.showConnectionInfo && address != null) "${s.publishedName.ifEmpty { s.deviceName }}  ·  $address" else ""
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            // Back stops mirroring, like on an Apple TV.
            if (NativeBridge.loaded) NativeBridge.nativeDisconnect()
            finish()
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    // ---- performance overlay ----

    private class OverlayStats {
        val previous = LongArray(STATS_SIZE)
        val current = LongArray(STATS_SIZE)
        var lastTime = 0L
    }

    private val overlayTick = object : Runnable {
        override fun run() {
            val stats = overlayStats ?: return
            if (NativeBridge.loaded) {
                NativeBridge.nativeStats(stats.current)
                val now = System.nanoTime()
                if (stats.lastTime != 0L) {
                    overlay.text = formatStats(stats.previous, stats.current, (now - stats.lastTime) / 1e9)
                }
                stats.current.copyInto(stats.previous)
                stats.lastTime = now
            }
            handler.postDelayed(this, 1000)
        }
    }

    private fun formatStats(a: LongArray, b: LongArray, seconds: Double): String {
        fun rate(i: Int) = (b[i] - a[i]) / seconds
        fun avg(total: Int, count: Int): Double {
            val n = b[count] - a[count]
            return if (n > 0) (b[total] - a[total]) / n.toDouble() / 1000.0 else 0.0
        }
        val codec = when (b[12]) { 1L -> "H.264"; 2L -> "H.265"; else -> "—" }
        val audio = when (b[19]) { 2L -> "ALAC"; 4L -> "AAC-LC"; 8L -> "AAC-ELD"; else -> "—" }
        return buildString {
            append("video   %s %dx%d\n".format(codec, b[10], b[11]))
            append("in      %5.1f fps  %5.2f Mbit/s\n".format(rate(0), rate(1) * 8 / 1e6))
            append("shown   %5.1f fps  dropped %d\n".format(rate(4), b[5]))
            append("decode  %5.1f ms\n".format(avg(6, 7)))
            if (b[22] != 0L) append("delay   %5.1f ms (sender to decoder)\n".format(avg(8, 9)))
            append("resets  %d  keyframes %d\n".format(b[13], b[2]))
            append("audio   %s  buffer %d ms\n".format(audio, b[20]))
            append("packets %d lost  %d dropped frames".format(b[15], b[18]))
        }
    }

    companion object {
        private const val STATS_SIZE = 25
    }
}
