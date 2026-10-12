package io.github.besliky.airplaytv.update

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.Process
import io.github.besliky.airplaytv.BuildConfig
import io.github.besliky.airplaytv.Log
import io.github.besliky.airplaytv.Log.Category.SERVICE
import io.github.besliky.airplaytv.R
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.concurrent.Executors

/**
 * Updates the app from its GitHub releases on the TV itself, so a new version needs no USB drive or
 * computer. Nothing happens unless the user asks: [check] reads the list of releases, [install]
 * downloads the package for this TV, verifies it against the release's SHA256SUMS and hands it to
 * the system installer, which only accepts it if it is signed with the same key as the installed app.
 *
 * Test versions (`-rc.1` and the like) are offered only to a TV that runs a test version itself.
 */
object Updater {

    class Release(val version: Version, val notes: String, val apk: Asset, val checksums: Asset)

    class Asset(val name: String, val url: String, val size: Long)

    /** Why an update did not happen: a string resource and the value for its placeholder, if any. */
    class Problem(val message: Int, val detail: String = "")

    sealed interface CheckResult {
        object UpToDate : CheckResult
        class Available(val release: Release) : CheckResult
        class Failed(val problem: Problem) : CheckResult
    }

    private const val MAX_APK_BYTES = 64L * 1024 * 1024
    private const val RELAUNCH_WINDOW_MS = 10 * 60_000L
    private const val PREFS = "update"
    private const val KEY_STARTED = "started_at"

    private val main = Handler(Looper.getMainLooper())
    private val executor = Executors.newSingleThreadExecutor { task -> Thread(task, "AirPlayUpdate").apply { isDaemon = true } }
    private val userAgent = "AirPlayTV/${BuildConfig.VERSION_NAME}"

    @Volatile
    private var cancelled = false

    /** Told when the system installer rejects an update, while a screen listens. Main thread. */
    var onInstallProblem: ((Problem) -> Unit)? = null

    val installed: Version? get() = Version.parse(BuildConfig.VERSION_NAME)

    /** Looks for a newer release; [done] runs on the main thread. */
    fun check(done: (CheckResult) -> Unit) {
        executor.execute {
            val result = try {
                newest()?.let { CheckResult.Available(it) } ?: CheckResult.UpToDate
            } catch (e: Problems) {
                CheckResult.Failed(e.problem)
            } catch (e: Exception) {
                Log.w(SERVICE, "update check failed", e)
                CheckResult.Failed(Problem(R.string.update_failed_network))
            }
            main.post { done(result) }
        }
    }

    /**
     * Downloads and verifies [release] and starts the system installer. [progress] gets the download
     * in percent; [done] gets null once the installer has the package, or the problem. Both run on
     * the main thread. The installer's answer arrives in [InstallReceiver].
     */
    fun install(context: Context, release: Release, progress: (Int) -> Unit, done: (Problem?) -> Unit) {
        val app = context.applicationContext
        cancelled = false
        executor.execute {
            val problem = try {
                val apk = download(app, release) { percent -> main.post { progress(percent) } }
                if (!cancelled) {
                    commit(app, apk)
                    app.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                        .putLong(KEY_STARTED, System.currentTimeMillis()).apply()
                }
                null
            } catch (e: Problems) {
                e.problem
            } catch (e: Exception) {
                Log.w(SERVICE, "update failed", e)
                Problem(R.string.update_failed_download)
            }
            main.post { if (!cancelled) done(problem) }
        }
    }

    /** Stops a download that is under way; its callbacks are not called any more. */
    fun cancel() {
        cancelled = true
    }

    /** True right after this app installed an update of itself, so it can open its screen again. */
    fun justUpdated(context: Context): Boolean {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val started = prefs.getLong(KEY_STARTED, 0L)
        prefs.edit().remove(KEY_STARTED).apply()
        return System.currentTimeMillis() - started in 0..RELAUNCH_WINDOW_MS
    }

    internal fun installProblem(problem: Problem) {
        main.post { onInstallProblem?.invoke(problem) }
    }

    // ---- release list ----

    private class Problems(val problem: Problem) : Exception()

    private fun newest(): Release? {
        val current = installed ?: return null
        val url = "https://api.github.com/repos/${BuildConfig.UPDATE_REPOSITORY}/releases?per_page=20"
        val releases = JSONArray(String(fetch(url, "application/vnd.github+json", 4_000_000), Charsets.UTF_8))
        var best: Pair<Version, JSONObject>? = null
        for (i in 0 until releases.length()) {
            val r = releases.optJSONObject(i) ?: continue
            if (r.optBoolean("draft")) continue
            val version = Version.parse(r.optString("tag_name")) ?: continue
            if (version.isPrerelease && !current.isPrerelease) continue
            if (version > current && (best == null || version > best.first)) best = version to r
        }
        val (version, r) = best ?: return null
        val assets = r.optJSONArray("assets") ?: JSONArray()
        fun asset(name: String): Asset? = (0 until assets.length()).map { assets.getJSONObject(it) }
            .firstOrNull { it.optString("name") == name }
            ?.let { Asset(name, it.optString("browser_download_url"), it.optLong("size")) }
            ?.takeIf { it.url.startsWith("https://") }
        val label = packageLabel()
        val apk = asset("AirPlayTV-v${version.name}-$label.apk") ?: asset("AirPlayTV-v${version.name}-universal.apk")
            ?: throw Problems(Problem(R.string.update_failed_no_apk, label))
        val checksums = asset("SHA256SUMS") ?: throw Problems(Problem(R.string.update_failed_no_apk, label))
        val notes = changes(if (r.isNull("body")) "" else r.optString("body"))
        Log.i(SERVICE, "update ${version.name} available")
        return Release(version, notes, apk, checksums)
    }

    /** The "Changes" section of the release notes as plain lines; the rest is install help for computers. */
    internal fun changes(notes: String): String {
        val items = ArrayList<String>()
        var inside = false
        for (raw in notes.lines()) {
            val line = raw.trim()
            if (line.startsWith("## ")) {
                if (inside) break
                inside = line.equals("## Changes", ignoreCase = true)
            } else if (inside && line.isNotEmpty()) {
                when {
                    line.startsWith("- ") || line.startsWith("* ") -> items += "•  " + line.substring(2)
                    items.isNotEmpty() -> items[items.size - 1] += " $line" // a wrapped item
                    else -> items += line
                }
            }
        }
        return items.joinToString("\n")
    }

    /** The release package that matches the code this process runs: arm64, armv7 or x86_64. */
    private fun packageLabel(): String {
        val abi = if (Process.is64Bit()) Build.SUPPORTED_64_BIT_ABIS.firstOrNull() else Build.SUPPORTED_32_BIT_ABIS.firstOrNull()
        return when (abi) {
            "arm64-v8a" -> "arm64"
            "armeabi-v7a" -> "armv7"
            "x86_64" -> "x86_64"
            else -> "universal"
        }
    }

    // ---- download and installation ----

    private fun download(context: Context, release: Release, progress: (Int) -> Unit): File {
        if (release.apk.size <= 0 || release.apk.size > MAX_APK_BYTES) throw Problems(Problem(R.string.update_failed_download))
        val sums = String(fetch(release.checksums.url, "application/octet-stream", 64 * 1024), Charsets.UTF_8)
        val expected = sums.lineSequence()
            .map { it.trim().split(Regex("\\s+"), limit = 2) }
            .firstOrNull { it.size == 2 && it[1].removePrefix("*") == release.apk.name }
            ?.get(0)?.lowercase()
            ?: throw Problems(Problem(R.string.update_failed_download))

        val dir = File(context.cacheDir, "update").apply {
            deleteRecursively()
            mkdirs()
        }
        val file = File(dir, release.apk.name)
        val digest = MessageDigest.getInstance("SHA-256")
        var written = 0L
        val connection = open(release.apk.url, "application/octet-stream")
        try {
            connection.inputStream.use { input ->
                file.outputStream().use { output ->
                    val buffer = ByteArray(64 * 1024)
                    var lastPercent = -1
                    while (!cancelled) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        written += n
                        if (written > release.apk.size) throw Problems(Problem(R.string.update_failed_download))
                        digest.update(buffer, 0, n)
                        output.write(buffer, 0, n)
                        val percent = (written * 100 / release.apk.size).toInt()
                        if (percent != lastPercent) {
                            lastPercent = percent
                            progress(percent)
                        }
                    }
                }
            }
        } finally {
            connection.disconnect()
        }
        if (cancelled) return file
        val actual = digest.digest().joinToString("") { "%02x".format(it) }
        if (written != release.apk.size || actual != expected) {
            Log.w(SERVICE, "update download damaged ($written of ${release.apk.size} bytes)")
            throw Problems(Problem(R.string.update_failed_download))
        }
        return file
    }

    /** Hands the package to the system installer; its answer arrives in [InstallReceiver]. */
    private fun commit(context: Context, apk: File) {
        val installer = context.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(context.packageName)
            setSize(apk.length())
            // Once this app has installed itself, Android 12+ lets it update without asking again.
            if (Build.VERSION.SDK_INT >= 31) setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        }
        val id = installer.createSession(params)
        installer.openSession(id).use { session ->
            apk.inputStream().use { input ->
                session.openWrite("base.apk", 0, apk.length()).use { output ->
                    input.copyTo(output)
                    session.fsync(output)
                }
            }
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
            val status = PendingIntent.getBroadcast(context, id, Intent(context, InstallReceiver::class.java), flags)
            session.commit(status.intentSender)
        }
        apk.delete()
        Log.i(SERVICE, "update handed to the installer")
    }

    private fun open(url: String, accept: String): HttpURLConnection {
        val connection = URL(url).openConnection() as HttpURLConnection
        connection.connectTimeout = 10_000
        connection.readTimeout = 20_000
        connection.setRequestProperty("User-Agent", userAgent)
        connection.setRequestProperty("Accept", accept)
        val code = try {
            connection.responseCode
        } catch (e: IOException) {
            connection.disconnect()
            Log.w(SERVICE, "update server unreachable", e)
            throw Problems(Problem(R.string.update_failed_network))
        }
        if (code != HttpURLConnection.HTTP_OK) {
            connection.disconnect()
            Log.w(SERVICE, "update server answered HTTP $code")
            throw Problems(Problem(R.string.update_failed_network))
        }
        return connection
    }

    private fun fetch(url: String, accept: String, limit: Int): ByteArray {
        val connection = open(url, accept)
        try {
            connection.inputStream.use { input ->
                val out = ByteArrayOutputStream()
                val buffer = ByteArray(8192)
                while (true) {
                    val n = input.read(buffer)
                    if (n < 0) return out.toByteArray()
                    out.write(buffer, 0, n)
                    if (out.size() > limit) throw IOException("answer too large")
                }
            }
        } finally {
            connection.disconnect()
        }
    }
}
