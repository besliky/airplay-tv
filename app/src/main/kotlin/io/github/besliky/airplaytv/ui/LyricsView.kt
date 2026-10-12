package io.github.besliky.airplaytv.ui

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.util.AttributeSet
import android.util.TypedValue
import android.view.View
import android.view.animation.DecelerateInterpolator
import io.github.besliky.airplaytv.lyrics.Lyrics
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/**
 * Scrolling lyrics. With time-stamped lyrics the line being sung is large and bright, its
 * neighbours shrink and fade, and the text glides to keep that line in view. Plain lyrics scroll
 * slowly along with the track instead. The lines are laid out once per size, not per frame.
 */
class LyricsView @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) : View(context, attrs) {

    private val density = resources.displayMetrics.density
    private val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textSize = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 30f, resources.displayMetrics)
        typeface = Typeface.create("sans-serif", Typeface.BOLD)
    }

    private var lyrics: Lyrics? = null
    private var layouts: List<StaticLayout> = emptyList()
    private var tops = FloatArray(0)
    private var contentHeight = 0f

    private var line = -1
    private var focus = -1f
    private var scroll = 0f
    private var glide: ValueAnimator? = null

    init {
        isVerticalFadingEdgeEnabled = true
        setFadingEdgeLength((56 * density).toInt())
    }

    override fun getTopFadingEdgeStrength(): Float = 1f

    override fun getBottomFadingEdgeStrength(): Float = 1f

    fun setLyrics(value: Lyrics?) {
        if (value === lyrics) return
        lyrics = value
        line = -1
        focus = -1f
        scroll = 0f
        glide?.cancel()
        layOut()
        invalidate()
    }

    /** Moves along with the track: to the line sung at [positionMs], or a share of plain lyrics. */
    fun setPosition(positionMs: Long, durationMs: Long) {
        val l = lyrics ?: return
        if (layouts.isEmpty()) return
        if (l.synced) {
            val next = l.indexAt(positionMs)
            if (next != line) {
                line = next
                glideTo(next)
            }
        } else if (durationMs > 0) {
            scroll = (positionMs.toFloat() / durationMs).coerceIn(0f, 1f) * max(0f, contentHeight - height * 0.55f)
            invalidate()
        }
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        layOut()
        glide?.cancel()
        focus = line.toFloat()
        if (lyrics?.synced == true) scroll = scrollFor(line)
    }

    private fun layOut() {
        val l = lyrics
        val width = width - paddingLeft - paddingRight
        if (l == null || width <= 0 || l.lines.isEmpty()) {
            layouts = emptyList()
            tops = FloatArray(0)
            contentHeight = 0f
            return
        }
        val gap = 20f * density
        tops = FloatArray(l.lines.size)
        var y = 0f
        layouts = l.lines.mapIndexed { i, it ->
            val text = it.text.ifBlank { " " }
            val layout = StaticLayout.Builder.obtain(text, 0, text.length, paint, width)
                .setAlignment(Layout.Alignment.ALIGN_NORMAL)
                .setLineSpacing(0f, 1.06f)
                .build()
            tops[i] = y
            y += layout.height + gap
            layout
        }
        contentHeight = y - gap
    }

    /** The scroll offset that puts line [i] at about 40% of the height. */
    private fun scrollFor(i: Int): Float =
        if (i < 0 || i >= layouts.size) -height * 0.25f else tops[i] + layouts[i].height / 2f - height * 0.4f

    private fun glideTo(i: Int) {
        val toFocus = i.toFloat()
        val toScroll = scrollFor(i)
        glide?.cancel()
        if (abs(toFocus - focus) > JUMP_LINES) {
            focus = toFocus
            scroll = toScroll
            invalidate()
            return
        }
        val fromFocus = focus
        val fromScroll = scroll
        glide = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = GLIDE_MS
            interpolator = DecelerateInterpolator(1.5f)
            addUpdateListener {
                val p = it.animatedValue as Float
                focus = fromFocus + (toFocus - fromFocus) * p
                scroll = fromScroll + (toScroll - fromScroll) * p
                invalidate()
            }
            start()
        }
    }

    override fun onDraw(canvas: Canvas) {
        val synced = lyrics?.synced ?: return
        for (i in layouts.indices) {
            val layout = layouts[i]
            val top = tops[i] - scroll
            if (top + layout.height < -16f || top > height + 16f) continue
            var alpha = 0.85f
            var scale = 1f
            if (synced) {
                val d = abs(i - focus)
                alpha = if (d <= 1f) 1f - 0.58f * d else max(0.12f, 0.42f - 0.12f * (d - 1f))
                scale = 1f - 0.1f * min(d, 1f)
            }
            paint.alpha = (alpha * 255f).toInt().coerceIn(0, 255)
            canvas.save()
            canvas.translate(paddingLeft.toFloat(), top)
            canvas.scale(scale, scale, 0f, layout.height / 2f)
            layout.draw(canvas)
            canvas.restore()
        }
    }

    private companion object {
        const val GLIDE_MS = 520L
        const val JUMP_LINES = 6f
    }
}
