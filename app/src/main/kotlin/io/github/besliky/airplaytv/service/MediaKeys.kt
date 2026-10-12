package io.github.besliky.airplaytv.service

import android.content.Context
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.SystemClock

/**
 * A media session for the track the sender plays. It shows the track in the system's media
 * controls and routes the remote's media keys to the sender, also while another app is in front.
 */
class MediaKeys(private val context: Context, private val send: (DacpClient.Command) -> Unit) {

    private var session: MediaSession? = null

    fun start() {
        if (session != null) return
        session = MediaSession(context, "AirPlayTV").apply {
            setCallback(object : MediaSession.Callback() {
                // Play and pause both toggle: a pause is only known once the audio has stopped.
                override fun onPlay() = send(DacpClient.Command.PLAY_PAUSE)
                override fun onPause() = send(DacpClient.Command.PLAY_PAUSE)
                override fun onSkipToNext() = send(DacpClient.Command.NEXT)
                override fun onSkipToPrevious() = send(DacpClient.Command.PREVIOUS)
                override fun onFastForward() = send(DacpClient.Command.NEXT)
                override fun onRewind() = send(DacpClient.Command.PREVIOUS)
            })
            isActive = true
        }
    }

    fun update(np: NowPlaying) {
        val session = session ?: return
        val metadata = MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, np.title)
            .putString(MediaMetadata.METADATA_KEY_ARTIST, np.artist)
            .putString(MediaMetadata.METADATA_KEY_ALBUM, np.album)
        if (np.durationMs > 0) metadata.putLong(MediaMetadata.METADATA_KEY_DURATION, np.durationMs)
        np.artwork?.let { metadata.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, it) }
        session.setMetadata(metadata.build())
        val now = SystemClock.elapsedRealtime()
        val position = np.positionAt(now).let { if (it >= 0) it else PlaybackState.PLAYBACK_POSITION_UNKNOWN }
        session.setPlaybackState(
            PlaybackState.Builder()
                .setActions(ACTIONS)
                .setState(
                    if (np.playing) PlaybackState.STATE_PLAYING else PlaybackState.STATE_PAUSED,
                    position, if (np.playing) 1f else 0f, now,
                )
                .build(),
        )
    }

    fun stop() {
        val session = session ?: return
        this.session = null
        session.isActive = false
        session.release()
    }

    private companion object {
        const val ACTIONS = PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE or PlaybackState.ACTION_PLAY_PAUSE or
            PlaybackState.ACTION_SKIP_TO_NEXT or PlaybackState.ACTION_SKIP_TO_PREVIOUS or
            PlaybackState.ACTION_FAST_FORWARD or PlaybackState.ACTION_REWIND
    }
}
