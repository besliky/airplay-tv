package io.github.besliky.airplaytv.ui

import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.TextView
import io.github.besliky.airplaytv.core.NativeBridge

/** The performance overlay over the picture: rates and timings of the stream, refreshed every second. */
class PerformanceOverlay(private val view: TextView) {

    private val handler = Handler(Looper.getMainLooper())
    private val previous = LongArray(STATS_SIZE)
    private val current = LongArray(STATS_SIZE)
    private var lastTime = 0L
    private var running = false

    private val tick = object : Runnable {
        override fun run() {
            if (!running) return
            if (NativeBridge.loaded) {
                NativeBridge.nativeStats(current)
                val now = System.nanoTime()
                if (lastTime != 0L) view.text = format(previous, current, (now - lastTime) / 1e9)
                current.copyInto(previous)
                lastTime = now
            }
            handler.postDelayed(this, 1000)
        }
    }

    fun show(on: Boolean) {
        handler.removeCallbacks(tick)
        running = on
        lastTime = 0L
        view.visibility = if (on) View.VISIBLE else View.GONE
        if (on) handler.post(tick)
    }

    private fun format(a: LongArray, b: LongArray, seconds: Double): String {
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

    private companion object {
        const val STATS_SIZE = 25
    }
}
