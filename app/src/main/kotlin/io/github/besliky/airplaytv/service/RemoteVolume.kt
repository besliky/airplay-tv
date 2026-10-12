package io.github.besliky.airplaytv.service

import android.content.Context
import android.media.AudioManager
import android.os.Handler
import android.os.Looper
import io.github.besliky.airplaytv.Log
import io.github.besliky.airplaytv.Log.Category.AUDIO

/**
 * Lets the remote's volume and mute buttons change the AirPlay volume, for streaming sticks whose
 * buttons change nothing you can hear: over HDMI they leave the output at full scale and expect the
 * TV to follow over HDMI-CEC, which some TVs do not.
 *
 * Apps never see the volume keys, but the system music volume moves when they are pressed. So it is
 * watched while a session runs: each step away from the middle is reported as one step, and the
 * system volume is put back in the middle so the buttons keep working at both ends. A press of mute
 * mutes the system volume, which is reported and undone. The system volume is restored when the
 * watch stops. Main thread only.
 */
class RemoteVolume(context: Context, private val onStep: (Int) -> Unit, private val onMute: () -> Unit) {

    private val audio = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val handler = Handler(Looper.getMainLooper())
    private var center = -1
    private var restoreTo = -1

    private val poll = object : Runnable {
        override fun run() {
            check()
            handler.postDelayed(this, POLL_MS)
        }
    }

    val active: Boolean get() = restoreTo >= 0

    /** Starts watching. Does nothing on devices whose volume cannot be changed by apps. */
    fun start() {
        if (active) return
        try {
            if (audio.isVolumeFixed) {
                Log.i(AUDIO, "remote volume keys: the system volume is fixed on this device")
                return
            }
            val max = audio.getStreamMaxVolume(STREAM)
            val current = audio.getStreamVolume(STREAM)
            center = max / 2
            if (audio.isStreamMute(STREAM)) audio.adjustStreamVolume(STREAM, AudioManager.ADJUST_UNMUTE, 0)
            audio.setStreamVolume(STREAM, center, 0)
            if (audio.getStreamVolume(STREAM) != center) {
                Log.i(AUDIO, "remote volume keys: the system volume cannot be set on this device")
                return
            }
            restoreTo = current
        } catch (e: RuntimeException) {
            Log.w(AUDIO, "remote volume keys unavailable", e)
            return
        }
        handler.postDelayed(poll, POLL_MS)
    }

    fun stop() {
        handler.removeCallbacks(poll)
        if (!active) return
        try {
            audio.setStreamVolume(STREAM, restoreTo, 0)
        } catch (e: RuntimeException) {
            Log.w(AUDIO, "cannot restore the system volume", e)
        }
        restoreTo = -1
    }

    private fun check() {
        try {
            if (audio.isStreamMute(STREAM)) {
                audio.adjustStreamVolume(STREAM, AudioManager.ADJUST_UNMUTE, 0)
                onMute()
                return
            }
            val index = audio.getStreamVolume(STREAM)
            if (index != center) {
                audio.setStreamVolume(STREAM, center, 0)
                onStep(index - center)
            }
        } catch (e: RuntimeException) {
            Log.w(AUDIO, "cannot read the system volume", e)
        }
    }

    private companion object {
        const val STREAM = AudioManager.STREAM_MUSIC
        const val POLL_MS = 150L
    }
}
