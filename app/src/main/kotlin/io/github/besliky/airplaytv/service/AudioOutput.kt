package io.github.besliky.airplaytv.service

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import io.github.besliky.airplaytv.Log
import io.github.besliky.airplaytv.Log.Category.AUDIO
import io.github.besliky.airplaytv.core.NativeBridge
import java.nio.ByteBuffer

/**
 * Plays the decoded PCM of the current session through an [AudioTrack].
 *
 * A dedicated thread pulls blocks from the native pipeline and writes them with
 * WRITE_BLOCKING, so the AudioTrack buffer paces the pipeline. Mirroring audio uses a
 * small low-latency buffer, music a larger one that rides out Wi-Fi hiccups.
 *
 * Senders do not announce a pause; they stop sending audio. The writer therefore reports when the
 * audio stops for a moment and when it comes again, see [onFlowChanged].
 */
class AudioOutput(context: Context) {

    private val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private var track: AudioTrack? = null
    private var thread: Thread? = null
    private var focusRequest: AudioFocusRequest? = null
    private val main = Handler(Looper.getMainLooper())

    /** Counts output streams, so a report from a writer that has been replaced is dropped. */
    @Volatile
    private var stream = 0

    /**
     * Called on the main thread with false when no audio has come for [PAUSE_MS] and with true when it
     * comes again, together with the time it stopped or came back (SystemClock.elapsedRealtime).
     */
    var onFlowChanged: ((flowing: Boolean, at: Long) -> Unit)? = null

    @Volatile
    private var running = false

    @Volatile
    private var gain = 1f

    /** 0 while another app holds the audio focus for a moment (a call, a parking sensor, a navigation prompt), else 1. */
    @Volatile
    private var focusGain = 1f

    @Volatile
    var bufferMs: Int = 0
        private set

    val active: Boolean get() = running

    fun start(sampleRate: Int, channels: Int, lowLatency: Boolean) {
        stop()
        val channelMask = if (channels == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
        val frameBytes = 2 * (if (channels == 1) 1 else 2)
        val minBuffer = AudioTrack.getMinBufferSize(sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT)
            .coerceAtLeast(frameBytes * 256)
        val bufferBytes = if (lowLatency) {
            (minBuffer * 2).coerceAtMost(sampleRate * frameBytes * 80 / 1000).coerceAtLeast(minBuffer)
        } else {
            maxOf(minBuffer * 4, sampleRate * frameBytes / 4)
        }
        val attributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
            .build()
        val t = try {
            AudioTrack.Builder()
                .setAudioAttributes(attributes)
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(sampleRate)
                        .setChannelMask(channelMask)
                        .build()
                )
                .setBufferSizeInBytes(bufferBytes)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setPerformanceMode(
                    if (lowLatency) AudioTrack.PERFORMANCE_MODE_LOW_LATENCY else AudioTrack.PERFORMANCE_MODE_NONE
                )
                .build()
        } catch (e: Exception) {
            Log.e(AUDIO, "cannot create AudioTrack", e)
            return
        }
        if (t.state != AudioTrack.STATE_INITIALIZED) {
            Log.e(AUDIO, "AudioTrack not initialized")
            t.release()
            return
        }
        t.setVolume(gain * focusGain)
        bufferMs = t.bufferSizeInFrames * 1000 / sampleRate
        track = t
        requestFocus(attributes)
        running = true
        val id = ++stream
        thread = Thread({ writerLoop(t, id) }, "AirPlayAudioOut").apply { start() }
        Log.i(AUDIO, "audio output ${sampleRate}Hz, buffer ${bufferMs}ms${if (lowLatency) ", low latency" else ""}")
    }

    private fun writerLoop(t: AudioTrack, id: Int) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        val buffer = ByteBuffer.allocateDirect(8192)
        var playing = false
        var lastAudioAt = 0L
        var flowing = true
        try {
            while (running) {
                buffer.clear()
                val n = NativeBridge.nativeReadAudio(buffer, buffer.capacity(), 50)
                if (n < 0) break
                if (n == 0) {
                    if (flowing && lastAudioAt > 0 && SystemClock.elapsedRealtime() - lastAudioAt >= PAUSE_MS) {
                        flowing = false
                        reportFlow(id, false, lastAudioAt)
                    }
                    continue
                }
                lastAudioAt = SystemClock.elapsedRealtime()
                if (!flowing) {
                    flowing = true
                    reportFlow(id, true, lastAudioAt)
                }
                if (!playing) {
                    t.play()
                    playing = true
                }
                buffer.limit(n)
                while (running && buffer.hasRemaining()) {
                    val written = t.write(buffer, buffer.remaining(), AudioTrack.WRITE_BLOCKING)
                    if (written < 0) {
                        Log.w(AUDIO, "AudioTrack write failed ($written)")
                        running = false
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(AUDIO, "audio output failed", e)
        }
        try {
            t.pause()
            t.flush()
        } catch (_: IllegalStateException) {
        }
    }

    private fun reportFlow(id: Int, flowing: Boolean, at: Long) {
        Log.d(AUDIO, if (flowing) "audio flows again" else "no audio from the sender")
        main.post { if (id == stream && running) onFlowChanged?.invoke(flowing, at) }
    }

    fun setVolume(value: Float) {
        gain = value.coerceIn(0f, 1f)
        track?.setVolume(gain * focusGain)
    }

    fun stop() {
        running = false
        thread?.let {
            it.interrupt()
            it.join(1000)
        }
        thread = null
        track?.release()
        track = null
        abandonFocus()
    }

    private fun requestFocus(attributes: AudioAttributes) {
        val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(attributes)
            // Ducking is left to the system; a short exclusive interruption silences the stream until it ends.
            .setOnAudioFocusChangeListener { change ->
                focusGain = if (change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) 0f else 1f
                track?.setVolume(gain * focusGain)
            }
            .build()
        focusRequest = request
        focusGain = 1f
        audioManager.requestAudioFocus(request)
    }

    private fun abandonFocus() {
        focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        focusRequest = null
    }

    private companion object {
        /** Silence this long from a sender that was playing counts as a pause. */
        const val PAUSE_MS = 300L
    }
}
