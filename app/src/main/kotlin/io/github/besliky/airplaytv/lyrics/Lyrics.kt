package io.github.besliky.airplaytv.lyrics

/** Lyrics of one track: time-stamped lines when [synced], otherwise plain lines in order. */
class Lyrics(val lines: List<Line>, val synced: Boolean, val instrumental: Boolean = false) {

    /** One line; [timeMs] is when it is sung, or -1 in plain lyrics. Empty text is a paragraph break. */
    class Line(val timeMs: Long, val text: String)

    /** The index of the line sung at [positionMs], or -1 before the first line and in plain lyrics. */
    fun indexAt(positionMs: Long): Int {
        if (!synced) return -1
        var low = 0
        var high = lines.size - 1
        var found = -1
        while (low <= high) {
            val mid = (low + high) ushr 1
            if (lines[mid].timeMs <= positionMs) {
                found = mid
                low = mid + 1
            } else {
                high = mid - 1
            }
        }
        return found
    }

    companion object {
        val INSTRUMENTAL = Lyrics(emptyList(), synced = false, instrumental = true)

        private val TIME_TAG = Regex("""^\[(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?]""")

        /**
         * Parses LRC: `[mm:ss.xx] words`, one or more time tags per line, and an optional
         * `[offset:±ms]` tag (positive shows the lines earlier). Lines without words are dropped.
         */
        fun parseLrc(text: String): List<Line> {
            var offsetMs = 0L
            val out = ArrayList<Line>()
            for (raw in text.lineSequence()) {
                var rest = raw.trim()
                if (rest.startsWith("[offset:", ignoreCase = true)) {
                    offsetMs = rest.substringAfter(':').substringBefore(']').trim().toLongOrNull() ?: 0L
                    continue
                }
                val times = ArrayList<Long>(1)
                while (true) {
                    val tag = TIME_TAG.find(rest) ?: break
                    val (minutes, seconds, fraction) = tag.destructured
                    val fractionMs = when (fraction.length) {
                        0 -> 0L
                        1 -> fraction.toLong() * 100
                        2 -> fraction.toLong() * 10
                        else -> fraction.toLong()
                    }
                    times += minutes.toLong() * 60_000 + seconds.toLong() * 1000 + fractionMs
                    rest = rest.substring(tag.range.last + 1)
                }
                val words = rest.trim()
                if (words.isEmpty()) continue
                times.forEach { out += Line((it - offsetMs).coerceAtLeast(0L), words) }
            }
            out.sortBy { it.timeMs }
            return out
        }

        /** Plain lyrics: one line per text line, runs of blank lines kept as one paragraph break. */
        fun parsePlain(text: String): List<Line> {
            val out = ArrayList<Line>()
            for (raw in text.lines()) {
                val line = raw.trim()
                if (line.isNotEmpty()) {
                    out += Line(-1, line)
                } else if (out.isNotEmpty() && out.last().text.isNotEmpty()) {
                    out += Line(-1, "")
                }
            }
            while (out.isNotEmpty() && out.last().text.isEmpty()) out.removeAt(out.size - 1)
            return out
        }
    }
}
