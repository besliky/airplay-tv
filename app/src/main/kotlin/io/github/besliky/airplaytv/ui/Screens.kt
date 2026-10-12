package io.github.besliky.airplaytv.ui

import android.app.Activity
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.View
import android.view.WindowManager
import android.view.animation.LinearInterpolator
import kotlin.math.PI
import kotlin.math.sin

/**
 * For the screens a sender brings up: keeps the screen on, and turns it on when the TV was in
 * standby (a sender can connect while the network stays up).
 */
fun Activity.showOverStandby() {
    window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    if (Build.VERSION.SDK_INT >= 27) {
        setTurnScreenOn(true)
        setShowWhenLocked(true)
    } else {
        @Suppress("DEPRECATION")
        window.addFlags(WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON or WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED)
    }
}

/**
 * Moves [view] a few dozen pixels along a slow loop that takes minutes, so no pixel of a still
 * picture stays lit for hours on an OLED panel. Nobody notices the motion.
 */
class Drift(private val view: View, private val xDp: Float, private val yDp: Float) {

    private val handler = Handler(Looper.getMainLooper())
    private val step = object : Runnable {
        override fun run() {
            val t = (SystemClock.elapsedRealtime() + STEP_MS) / 60_000.0
            val density = view.resources.displayMetrics.density
            val x = xDp * density * sin(2 * PI * t / PERIOD_X_MIN).toFloat()
            val y = yDp * density * sin(2 * PI * t / PERIOD_Y_MIN + 0.7).toFloat()
            view.animate().translationX(x).translationY(y).setDuration(STEP_MS).setInterpolator(LinearInterpolator()).start()
            handler.postDelayed(this, STEP_MS)
        }
    }

    fun start() {
        handler.removeCallbacks(step)
        handler.postDelayed(step, STEP_MS)
    }

    fun stop() {
        handler.removeCallbacks(step)
    }

    private companion object {
        const val STEP_MS = 10_000L
        const val PERIOD_X_MIN = 11.0
        const val PERIOD_Y_MIN = 7.0
    }
}
