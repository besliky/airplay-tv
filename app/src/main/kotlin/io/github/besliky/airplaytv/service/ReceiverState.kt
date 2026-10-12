package io.github.besliky.airplaytv.service

import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import java.util.concurrent.CopyOnWriteArraySet

/** Observable state of the receiver, shared between the service and the UI (main thread). */
object ReceiverState {

    enum class Status { OFF, STARTING, READY, NO_NETWORK, ERROR, CONNECTED }

    data class Snapshot(
        val status: Status = Status.OFF,
        val deviceName: String = "",
        val publishedName: String = "",
        val addresses: List<String> = emptyList(),
        val transport: String = "",
        val port: Int = 0,
        val clientName: String? = null,
        val clientModel: String? = null,
        val videoActive: Boolean = false,
        val videoWidth: Int = 0,
        val videoHeight: Int = 0,
        val audioActive: Boolean = false,
        val pin: String? = null,
        val error: String? = null,
        val nowPlaying: NowPlaying = NowPlaying(),
        /** AirPlay turned the screen on for this session and nobody has used the remote since. */
        val wokeScreen: Boolean = false,
        /** The photo shown from the sender's Photos app; [photoSeq] counts the photos received. */
        val photo: Bitmap? = null,
        val photoSeq: Int = 0,
    )

    private val handler = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArraySet<(Snapshot) -> Unit>()

    @Volatile
    var current = Snapshot()
        private set

    fun update(transform: (Snapshot) -> Snapshot) {
        val next = transform(current)
        if (next == current) return
        current = next
        if (Looper.myLooper() == Looper.getMainLooper()) {
            listeners.forEach { it(next) }
        } else {
            handler.post { listeners.forEach { it(current) } }
        }
    }

    fun observe(listener: (Snapshot) -> Unit) {
        listeners += listener
        listener(current)
    }

    fun remove(listener: (Snapshot) -> Unit) {
        listeners -= listener
    }
}
