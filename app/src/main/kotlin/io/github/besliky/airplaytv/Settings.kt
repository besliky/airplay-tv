package io.github.besliky.airplaytv

import android.content.Context
import android.content.SharedPreferences

/** User settings, stored in the app's private preferences. */
class Settings(context: Context) {

    enum class Resolution(val key: String, val width: Int, val height: Int) {
        HD("720p", 1280, 720),
        FULL_HD("1080p", 1920, 1080),
        UHD("4k", 3840, 2160);

        companion object {
            fun fromKey(key: String?): Resolution = entries.firstOrNull { it.key == key } ?: FULL_HD
        }
    }

    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences("settings", Context.MODE_PRIVATE)

    var deviceName: String
        get() = sanitizeName(prefs.getString(KEY_NAME, null)) ?: DEFAULT_NAME
        set(value) = prefs.edit().putString(KEY_NAME, sanitizeName(value) ?: DEFAULT_NAME).apply()

    var enabled: Boolean
        get() = prefs.getBoolean(KEY_ENABLED, true)
        set(value) = prefs.edit().putBoolean(KEY_ENABLED, value).apply()

    var requirePin: Boolean
        get() = prefs.getBoolean(KEY_PIN, false)
        set(value) = prefs.edit().putBoolean(KEY_PIN, value).apply()

    var startAutomatically: Boolean
        get() = prefs.getBoolean(KEY_AUTOSTART, true)
        set(value) = prefs.edit().putBoolean(KEY_AUTOSTART, value).apply()

    var showConnectionInfo: Boolean
        get() = prefs.getBoolean(KEY_CONNECTION_INFO, true)
        set(value) = prefs.edit().putBoolean(KEY_CONNECTION_INFO, value).apply()

    var performanceOverlay: Boolean
        get() = prefs.getBoolean(KEY_OVERLAY, false)
        set(value) = prefs.edit().putBoolean(KEY_OVERLAY, value).apply()

    var verboseLogging: Boolean
        get() = prefs.getBoolean(KEY_VERBOSE, false)
        set(value) = prefs.edit().putBoolean(KEY_VERBOSE, value).apply()

    var resolution: Resolution
        get() = Resolution.fromKey(prefs.getString(KEY_RESOLUTION, null))
        set(value) = prefs.edit().putString(KEY_RESOLUTION, value.key).apply()

    var frameRate: Int
        get() = if (prefs.getInt(KEY_FPS, 60) == 30) 30 else 60
        set(value) = prefs.edit().putInt(KEY_FPS, if (value == 30) 30 else 60).apply()

    /** Look up lyrics of the playing song on lrclib.net. Off by default: it sends the title and artist. */
    var lyricsEnabled: Boolean
        get() = prefs.getBoolean(KEY_LYRICS, false)
        set(value) = prefs.edit().putBoolean(KEY_LYRICS, value).apply()

    /** Minutes after the music ends until a TV that AirPlay turned on goes back to sleep; 0 is never. */
    var sleepAfterMinutes: Int
        get() = prefs.getInt(KEY_SLEEP, 0).takeIf { it in SLEEP_CHOICES } ?: 0
        set(value) = prefs.edit().putInt(KEY_SLEEP, value.takeIf { it in SLEEP_CHOICES } ?: 0).apply()

    /** The remote's volume buttons change the AirPlay volume, for sticks whose buttons change nothing. */
    var remoteVolumeKeys: Boolean
        get() = prefs.getBoolean(KEY_REMOTE_VOLUME, false)
        set(value) = prefs.edit().putBoolean(KEY_REMOTE_VOLUME, value).apply()

    /** Car app: the dock is on the right, for right-hand-drive cars. */
    var dockOnRight: Boolean
        get() = prefs.getBoolean(KEY_DOCK_RIGHT, false)
        set(value) = prefs.edit().putBoolean(KEY_DOCK_RIGHT, value).apply()

    /** Decoder name chosen by the user, empty for automatic selection. */
    var preferredDecoder: String
        get() = prefs.getString(KEY_DECODER, "") ?: ""
        set(value) = prefs.edit().putString(KEY_DECODER, value).apply()

    fun registerListener(l: SharedPreferences.OnSharedPreferenceChangeListener) =
        prefs.registerOnSharedPreferenceChangeListener(l)

    fun unregisterListener(l: SharedPreferences.OnSharedPreferenceChangeListener) =
        prefs.unregisterOnSharedPreferenceChangeListener(l)

    companion object {
        /** "AirPlay TV" or "AirPlay Car". */
        val DEFAULT_NAME: String = BuildConfig.DEFAULT_DEVICE_NAME
        const val MAX_NAME_LENGTH = 40

        const val KEY_NAME = "device_name"
        const val KEY_ENABLED = "enabled"
        const val KEY_PIN = "require_pin"
        const val KEY_AUTOSTART = "start_automatically"
        const val KEY_CONNECTION_INFO = "show_connection_info"
        const val KEY_OVERLAY = "performance_overlay"
        const val KEY_VERBOSE = "verbose_logging"
        const val KEY_RESOLUTION = "resolution"
        const val KEY_FPS = "frame_rate"
        const val KEY_DECODER = "preferred_decoder"
        const val KEY_LYRICS = "lyrics"
        const val KEY_SLEEP = "sleep_after_minutes"
        const val KEY_REMOTE_VOLUME = "remote_volume_keys"
        const val KEY_DOCK_RIGHT = "dock_on_right"

        /** The choices for Sleep After Music, in minutes; 0 is off. */
        val SLEEP_CHOICES = listOf(0, 2, 5, 10, 20)

        /** Settings that require the receiver to be restarted when they change. */
        val RESTART_KEYS = setOf(KEY_NAME, KEY_PIN, KEY_RESOLUTION, KEY_FPS, KEY_DECODER)

        /**
         * Normalises a device name: trims, drops control characters and limits the
         * length so that "<id>@<name>" still fits into a 63-byte DNS label.
         */
        fun sanitizeName(raw: String?): String? {
            if (raw == null) return null
            val cleaned = raw
                .map { if (it.isWhitespace()) ' ' else it }
                .filter { !it.isISOControl() }
                .joinToString("")
                .trim()
                .replace(Regex(" +"), " ")
            if (cleaned.isEmpty()) return null
            var name = cleaned.take(MAX_NAME_LENGTH)
            while (name.toByteArray(Charsets.UTF_8).size > 49) {
                name = name.dropLast(1)
            }
            return name.trim().ifEmpty { null }
        }
    }
}
