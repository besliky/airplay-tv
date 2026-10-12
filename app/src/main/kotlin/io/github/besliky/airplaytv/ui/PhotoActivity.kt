package io.github.besliky.airplaytv.ui

import android.app.Activity
import android.graphics.Bitmap
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.KeyEvent
import android.view.View
import android.view.animation.DecelerateInterpolator
import android.widget.ImageView
import io.github.besliky.airplaytv.R
import io.github.besliky.airplaytv.service.ReceiverState

/**
 * A photo from the iPhone's Photos app, fitted to the screen on black. A new photo dissolves in
 * over the old one. The screen closes when the sender ends the photo session or Back is pressed.
 * For OLED panels the picture drifts slowly, dims after a few minutes without a new photo, and the
 * screen closes after a long time without one.
 */
class PhotoActivity : Activity() {

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var stage: View
    private lateinit var front: ImageView
    private lateinit var back: ImageView
    private lateinit var drift: Drift
    private var shownSeq = -1
    private var lastChange = 0L
    private var dimmed = false

    private val stateListener: (ReceiverState.Snapshot) -> Unit = { render(it) }

    private val care = object : Runnable {
        override fun run() {
            val idle = SystemClock.elapsedRealtime() - lastChange
            if (idle >= CLOSE_AFTER_MS) {
                finish()
                return
            }
            if (!dimmed && idle >= DIM_AFTER_MS) {
                dimmed = true
                stage.animate().alpha(DIM_ALPHA).setDuration(2000).start()
            }
            handler.postDelayed(this, CARE_TICK_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        showOverStandby()
        setContentView(R.layout.activity_photo)
        stage = findViewById(R.id.stage)
        front = findViewById(R.id.photoA)
        back = findViewById(R.id.photoB)
        drift = Drift(findViewById(R.id.drift), 14f, 9f)
        lastChange = SystemClock.elapsedRealtime()
    }

    override fun onStart() {
        super.onStart()
        ReceiverState.observe(stateListener)
        drift.start()
        handler.postDelayed(care, CARE_TICK_MS)
    }

    override fun onStop() {
        ReceiverState.remove(stateListener)
        drift.stop()
        handler.removeCallbacksAndMessages(null)
        super.onStop()
    }

    private fun render(s: ReceiverState.Snapshot) {
        val photo = s.photo
        if (photo == null) {
            finish()
            return
        }
        if (s.photoSeq == shownSeq) return
        show(photo, animate = shownSeq >= 0)
        shownSeq = s.photoSeq
        noteChange()
    }

    /** Dissolves from the photo on screen to [bitmap]. */
    private fun show(bitmap: Bitmap, animate: Boolean) {
        val incoming = back
        val outgoing = front
        front = incoming
        back = outgoing
        incoming.animate().cancel()
        outgoing.animate().cancel()
        incoming.setImageBitmap(bitmap)
        incoming.bringToFront()
        if (!animate) {
            incoming.alpha = 1f
            outgoing.alpha = 0f
            return
        }
        incoming.alpha = 0f
        incoming.animate().alpha(1f).setDuration(DISSOLVE_MS).setInterpolator(DecelerateInterpolator()).start()
        outgoing.animate().alpha(0f).setDuration(DISSOLVE_MS).start()
    }

    private fun noteChange() {
        lastChange = SystemClock.elapsedRealtime()
        if (dimmed) {
            dimmed = false
            stage.animate().alpha(1f).setDuration(500).start()
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val wasDimmed = dimmed
        noteChange()
        // the first press only brightens a dimmed photo
        if (wasDimmed && keyCode != KeyEvent.KEYCODE_BACK) return true
        return super.onKeyDown(keyCode, event)
    }

    private companion object {
        const val DISSOLVE_MS = 700L
        const val CARE_TICK_MS = 5_000L
        const val DIM_AFTER_MS = 3 * 60_000L
        const val DIM_ALPHA = 0.5f
        const val CLOSE_AFTER_MS = 45 * 60_000L
    }
}
