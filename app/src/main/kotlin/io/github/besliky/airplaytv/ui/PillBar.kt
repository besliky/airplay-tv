package io.github.besliky.airplaytv.ui

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View
import android.view.animation.DecelerateInterpolator

/**
 * A rounded bar like the iPhone's progress and volume sliders: a dark track with a lighter fill and
 * round ends. The fill can glide to a new value, and the bar swells while it is being changed.
 */
class PillBar @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) : View(context, attrs) {

    private val density = resources.displayMetrics.density
    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF3A3A3E.toInt() }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFA6A6AC.toInt() }
    private val bar = RectF()
    private val clip = Path()

    private var fraction = 0f
    private var fractionAnimator: ValueAnimator? = null
    private var thickness = REST_DP * density
    private var thicknessAnimator: ValueAnimator? = null

    fun setColors(track: Int, fill: Int) {
        trackPaint.color = track
        fillPaint.color = fill
        invalidate()
    }

    /** Moves the fill to [value] (0..1), gliding there when [animate] is set. */
    fun setFraction(value: Float, animate: Boolean = false) {
        val target = value.coerceIn(0f, 1f)
        fractionAnimator?.cancel()
        if (!animate || width == 0) {
            fraction = target
            invalidate()
            return
        }
        fractionAnimator = ValueAnimator.ofFloat(fraction, target).apply {
            duration = FILL_MS
            interpolator = DecelerateInterpolator()
            addUpdateListener {
                fraction = it.animatedValue as Float
                invalidate()
            }
            start()
        }
    }

    /** Swells the bar while it is being changed and lets it settle afterwards. */
    fun setActive(active: Boolean) {
        thicknessAnimator?.cancel()
        thicknessAnimator = ValueAnimator.ofFloat(thickness, (if (active) ACTIVE_DP else REST_DP) * density).apply {
            duration = SWELL_MS
            interpolator = DecelerateInterpolator()
            addUpdateListener {
                thickness = it.animatedValue as Float
                invalidate()
            }
            start()
        }
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return
        val t = thickness.coerceAtMost(h)
        val top = (h - t) / 2f
        val radius = t / 2f
        bar.set(0f, top, w, top + t)
        canvas.drawRoundRect(bar, radius, radius, trackPaint)
        if (fraction > 0f) {
            clip.reset()
            clip.addRoundRect(bar, radius, radius, Path.Direction.CW)
            canvas.save()
            canvas.clipPath(clip)
            canvas.drawRect(0f, top, w * fraction, top + t, fillPaint)
            canvas.restore()
        }
    }

    private companion object {
        const val REST_DP = 8f
        const val ACTIVE_DP = 13f
        const val FILL_MS = 160L
        const val SWELL_MS = 170L
    }
}
