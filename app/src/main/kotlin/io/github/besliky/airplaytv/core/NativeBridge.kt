package io.github.besliky.airplaytv.core

import android.os.Handler
import android.os.Looper
import android.view.Surface
import java.nio.ByteBuffer

/**
 * Entry points of the native receiver (libairplaytv.so).
 *
 * Calls from native code arrive on receiver threads and are re-posted to the main
 * thread before they reach [listener], so listeners never block the network or
 * decoder threads and may safely call back into this object.
 */
object NativeBridge {

    interface Listener {
        fun onSessionStarted(clientName: String, clientModel: String)
        fun onSessionEnded()
        fun onPin(pin: String?)
        fun onClientPaired(key: String, clientName: String)
        fun onVideoStarted()
        fun onVideoStopped()
        fun onVideoSize(width: Int, height: Int)
        fun onAudioStarted(sampleRate: Int, channels: Int, lowLatency: Boolean)
        fun onAudioStopped()
        fun onVolume(gain: Float)

        /** Track info of the active session; empty strings when the sender left a field out. */
        fun onTrackInfo(title: String, artist: String, album: String)

        /** Encoded cover art (JPEG or PNG), or null when the track has none. */
        fun onArtwork(image: ByteArray?)

        /** Playback position in RTP timestamp units (unsigned 32-bit values). */
        fun onProgress(start: Long, current: Long, end: Long)

        /** The sender's remote-control (DACP) service id and the token its commands must carry. */
        fun onRemoteControl(dacpId: String, activeRemote: String)

        /** An encoded photo (JPEG or PNG) from the sender's Photos app. */
        fun onPhoto(image: ByteArray)

        fun onPhotoStop()
    }

    /** Decoder low-latency options, see video_decoder.h. */
    const val DECODER_LOW_LATENCY = 0x1
    const val DECODER_VENDOR_KEYS = 0x2
    const val DECODER_REALTIME = 0x4

    const val LOG_ERROR = 0
    const val LOG_WARN = 1
    const val LOG_INFO = 2
    const val LOG_DEBUG = 3

    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile
    var listener: Listener? = null

    val loaded: Boolean = try {
        System.loadLibrary("airplaytv")
        true
    } catch (e: UnsatisfiedLinkError) {
        android.util.Log.e("AirPlayTV", "native library unavailable", e)
        false
    }

    private fun post(block: (Listener) -> Unit) {
        mainHandler.post { listener?.let(block) }
    }

    private fun ByteArray?.utf8(): String = this?.toString(Charsets.UTF_8) ?: ""

    // ---- calls from native code ----

    @JvmStatic
    fun onSessionStarted(name: ByteArray?, model: ByteArray?) {
        val n = name.utf8()
        val m = model.utf8()
        post { it.onSessionStarted(n, m) }
    }

    @JvmStatic
    fun onSessionEnded() = post { it.onSessionEnded() }

    @JvmStatic
    fun onPin(pin: ByteArray?) {
        val p = pin?.utf8()
        post { it.onPin(p) }
    }

    @JvmStatic
    fun onClientPaired(key: ByteArray?, name: ByteArray?) {
        val k = key.utf8()
        val n = name.utf8()
        post { it.onClientPaired(k, n) }
    }

    @JvmStatic
    fun onVideoStarted() = post { it.onVideoStarted() }

    @JvmStatic
    fun onVideoStopped() = post { it.onVideoStopped() }

    @JvmStatic
    fun onVideoSize(width: Int, height: Int) = post { it.onVideoSize(width, height) }

    @JvmStatic
    fun onAudioStarted(sampleRate: Int, channels: Int, lowLatency: Boolean) =
        post { it.onAudioStarted(sampleRate, channels, lowLatency) }

    @JvmStatic
    fun onAudioStopped() = post { it.onAudioStopped() }

    @JvmStatic
    fun onVolume(gain: Float) = post { it.onVolume(gain) }

    @JvmStatic
    fun onTrackInfo(title: ByteArray?, artist: ByteArray?, album: ByteArray?) {
        val t = title.utf8()
        val a = artist.utf8()
        val l = album.utf8()
        post { it.onTrackInfo(t, a, l) }
    }

    @JvmStatic
    fun onArtwork(image: ByteArray?) = post { it.onArtwork(image) }

    @JvmStatic
    fun onProgress(start: Long, current: Long, end: Long) = post { it.onProgress(start, current, end) }

    @JvmStatic
    fun onRemoteControl(dacpId: ByteArray?, activeRemote: ByteArray?) {
        val d = dacpId.utf8()
        val a = activeRemote.utf8()
        post { it.onRemoteControl(d, a) }
    }

    @JvmStatic
    fun onPhoto(image: ByteArray) = post { it.onPhoto(image) }

    @JvmStatic
    fun onPhotoStop() = post { it.onPhotoStop() }

    // ---- native methods ----

    @JvmStatic
    external fun nativeSetLogLevel(level: Int)

    @JvmStatic
    external fun nativeLog(level: Int, category: Int, message: String)

    /** Starts the receiver. Returns the RTSP port, or -1 on failure. */
    @JvmStatic
    external fun nativeStart(
        name: ByteArray,
        deviceId: ByteArray,
        publicId: String,
        identitySeed: ByteArray,
        requirePin: Boolean,
        port: Int,
        width: Int,
        height: Int,
        fps: Int,
        hevc: Boolean,
    ): Int

    @JvmStatic
    external fun nativeStop()

    @JvmStatic
    external fun nativePublicKey(): String?

    /** TXT record of the _raop._tcp (true) or _airplay._tcp (false) service as key, value pairs. */
    @JvmStatic
    external fun nativeTxtRecords(raop: Boolean): Array<String>?

    @JvmStatic
    external fun nativeSetPairedClients(keys: Array<String>)

    @JvmStatic
    external fun nativeDisconnect()

    @JvmStatic
    external fun nativeSetSurface(surface: Surface?)

    @JvmStatic
    external fun nativeSetDecoderPreferences(avcDecoder: String?, hevcDecoder: String?, options: Int)

    /** Blocks up to [timeoutMs] for decoded PCM. Returns bytes read, 0 on timeout, -1 at end of stream. */
    @JvmStatic
    external fun nativeReadAudio(buffer: ByteBuffer, maxBytes: Int, timeoutMs: Int): Int

    @JvmStatic
    external fun nativeStats(out: LongArray)

    @JvmStatic
    external fun nativeLogHistory(): String?

    @JvmStatic
    external fun nativeSessionActive(): Boolean
}
