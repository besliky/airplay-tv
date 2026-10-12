package io.github.besliky.airplaytv.lyrics

import android.os.Handler
import android.os.Looper
import io.github.besliky.airplaytv.BuildConfig
import io.github.besliky.airplaytv.Log
import io.github.besliky.airplaytv.Log.Category.SESSION
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.concurrent.Executors
import kotlin.math.abs

/**
 * Finds lyrics on LRCLIB (https://lrclib.net), a free community database. Only the title and the
 * artist of the song are sent. This is used only when the user has turned Lyrics on.
 *
 * Candidates are ranked by whether they are time-stamped, how close their length is to the track's
 * (another recording would drift) and whether title and artist match exactly. Answers, including
 * "nothing found", are remembered for the life of the process; failed lookups are not.
 */
object LrcLib {

    private const val SEARCH_URL = "https://lrclib.net/api/search"
    private val userAgent = "AirPlayTV/${BuildConfig.VERSION_NAME} (https://github.com/besliky/airplay-tv)"
    private const val MAX_RESPONSE_BYTES = 3_000_000
    private const val MAX_LENGTH_DIFFERENCE_S = 10.0
    private const val CACHE_SIZE = 40

    private val main = Handler(Looper.getMainLooper())
    private val executor = Executors.newSingleThreadExecutor { task -> Thread(task, "AirPlayLyrics").apply { isDaemon = true } }

    private val cache = object : LinkedHashMap<String, Lyrics?>(CACHE_SIZE, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Lyrics?>?): Boolean = size > CACHE_SIZE
    }

    /** Looks up the lyrics and hands them, or null when there are none, to [done] on the main thread. */
    fun find(title: String, artist: String, durationMs: Long, done: (Lyrics?) -> Unit) {
        val key = "${norm(title)}|${norm(artist)}|${durationMs / 5000}"
        executor.execute {
            val cached = synchronized(cache) { if (cache.containsKey(key)) Result.success(cache[key]) else null }
            val result = cached ?: runCatching { search(title, artist, durationMs) }.onSuccess { lyrics ->
                synchronized(cache) { cache[key] = lyrics }
            }.onFailure { e ->
                Log.w(SESSION, "lyrics lookup failed: ${e.javaClass.simpleName}")
            }
            main.post { done(result.getOrNull()) }
        }
    }

    private fun search(title: String, artist: String, durationMs: Long): Lyrics? {
        val url = URL("$SEARCH_URL?track_name=${encode(title)}&artist_name=${encode(artist)}")
        val connection = url.openConnection() as HttpURLConnection
        connection.connectTimeout = 6000
        connection.readTimeout = 9000
        connection.setRequestProperty("User-Agent", userAgent)
        connection.setRequestProperty("Accept", "application/json")
        try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                throw IOException("HTTP ${connection.responseCode}")
            }
            val results = JSONArray(connection.inputStream.use { readLimited(it) })
            val lyrics = pick(results, title, artist, durationMs)
            Log.i(SESSION, "lyrics: ${results.length()} candidates, " +
                when {
                    lyrics == null -> "none usable"
                    lyrics.synced -> "time-stamped"
                    else -> "plain"
                })
            return lyrics
        } finally {
            connection.disconnect()
        }
    }

    private fun readLimited(stream: InputStream): String {
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(8192)
        while (true) {
            val n = stream.read(buffer)
            if (n < 0) break
            out.write(buffer, 0, n)
            if (out.size() > MAX_RESPONSE_BYTES) throw IOException("response too large")
        }
        return out.toString("UTF-8")
    }

    private fun pick(results: JSONArray, title: String, artist: String, durationMs: Long): Lyrics? {
        val wantSeconds = if (durationMs > 0) durationMs / 1000.0 else -1.0
        var best: JSONObject? = null
        var bestScore = Double.NEGATIVE_INFINITY
        for (i in 0 until results.length()) {
            val r = results.optJSONObject(i) ?: continue
            val synced = text(r, "syncedLyrics")
            val plain = text(r, "plainLyrics")
            val instrumental = r.optBoolean("instrumental", false)
            var score = when {
                synced.isNotBlank() -> 100.0
                plain.isNotBlank() -> 40.0
                instrumental -> 10.0
                else -> continue
            }
            val seconds = r.optDouble("duration", -1.0)
            if (wantSeconds > 0 && seconds > 0) {
                val difference = abs(seconds - wantSeconds)
                if (difference > MAX_LENGTH_DIFFERENCE_S) continue
                score -= difference * 4
            }
            if (norm(r.optString("trackName")) == norm(title)) score += 20
            if (norm(r.optString("artistName")) == norm(artist)) score += 15
            if (score > bestScore) {
                bestScore = score
                best = r
            }
        }
        val chosen = best ?: return null
        Lyrics.parseLrc(text(chosen, "syncedLyrics")).takeIf { it.isNotEmpty() }?.let { return Lyrics(it, synced = true) }
        Lyrics.parsePlain(text(chosen, "plainLyrics")).takeIf { it.isNotEmpty() }?.let { return Lyrics(it, synced = false) }
        return if (chosen.optBoolean("instrumental", false)) Lyrics.INSTRUMENTAL else null
    }

    /** A string field that may be missing or JSON null (which optString would turn into "null"). */
    private fun text(o: JSONObject, name: String): String = if (o.isNull(name)) "" else o.optString(name, "")

    private fun norm(s: String): String = s.trim().lowercase()

    private fun encode(s: String): String = URLEncoder.encode(s, "UTF-8")
}
