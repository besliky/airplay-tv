package io.github.besliky.airplaytv.service

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Handler
import android.os.Looper
import io.github.besliky.airplaytv.Log
import io.github.besliky.airplaytv.Log.Category.SESSION
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.Executors

/**
 * Sends transport commands back to the sender over DACP, the remote-control protocol of iTunes, so
 * the TV remote can pause and skip what the phone plays.
 *
 * The sender announces its DACP service over mDNS as `iTunes_Ctrl_<DACP-ID>` and names that id in
 * its requests, together with a token every command must carry (Active-Remote). A command is a
 * plain `GET /ctrl-int/1/<command>`. The service is looked up when the identity arrives; a command
 * sent before it was found starts the lookup again. Main thread only.
 */
class DacpClient(context: Context) {

    enum class Command(val path: String) {
        /** A toggle: it works whatever the receiver believes the state to be. */
        PLAY_PAUSE("playpause"),
        NEXT("nextitem"),
        PREVIOUS("previtem"),
    }

    private val nsd = context.applicationContext.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val main = Handler(Looper.getMainLooper())
    private val executor = Executors.newSingleThreadExecutor { task -> Thread(task, "AirPlayRemote").apply { isDaemon = true } }

    private var serviceName: String? = null
    private var token: String? = null
    private var address: InetSocketAddress? = null
    private var discovery: NsdManager.DiscoveryListener? = null
    private val stopLookup = Runnable { stopDiscovery() }

    fun configure(dacpId: String, activeRemote: String) {
        val name = "iTunes_Ctrl_$dacpId"
        token = activeRemote
        if (name.equals(serviceName, ignoreCase = true) && (address != null || discovery != null)) return
        serviceName = name
        address = null
        startDiscovery()
    }

    /** Forgets the sender at the end of a session. */
    fun clear() {
        stopDiscovery()
        serviceName = null
        token = null
        address = null
    }

    fun send(command: Command) {
        val activeRemote = token ?: return
        val target = address
        if (target == null) {
            Log.w(SESSION, "remote command ${command.path} dropped: the sender's remote service was not found yet")
            if (discovery == null && serviceName != null) startDiscovery()
            return
        }
        executor.execute {
            // A plain socket: HttpURLConnection refuses cleartext HTTP, and DACP is cleartext on the local
            // network by design. The app-wide cleartext policy stays strict.
            try {
                Socket().use { socket ->
                    socket.connect(target, TIMEOUT_MS)
                    socket.soTimeout = TIMEOUT_MS
                    val host = target.address.hostAddress
                    val request = "GET /ctrl-int/1/${command.path} HTTP/1.1\r\nHost: $host:${target.port}\r\n" +
                        "Active-Remote: $activeRemote\r\nConnection: close\r\n\r\n"
                    socket.getOutputStream().write(request.toByteArray(Charsets.US_ASCII))
                    val status = socket.getInputStream().bufferedReader(Charsets.US_ASCII).readLine()
                    Log.i(SESSION, "remote command ${command.path}: $status")
                }
            } catch (e: Exception) {
                Log.w(SESSION, "remote command ${command.path} failed", e)
            }
        }
    }

    fun shutdown() {
        clear()
        executor.shutdownNow()
    }

    private fun startDiscovery() {
        stopDiscovery()
        val wanted = serviceName ?: return
        val listener = object : NsdManager.DiscoveryListener {
            override fun onServiceFound(info: NsdServiceInfo) {
                if (info.serviceName.equals(wanted, ignoreCase = true)) main.post { resolve(info) }
            }

            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                Log.w(SESSION, "remote service lookup failed to start ($errorCode)")
                main.post { if (discovery === this) discovery = null }
            }

            override fun onDiscoveryStarted(serviceType: String) = Unit
            override fun onDiscoveryStopped(serviceType: String) = Unit
            override fun onServiceLost(info: NsdServiceInfo) = Unit
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) = Unit
        }
        try {
            nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, listener)
            discovery = listener
            main.postDelayed(stopLookup, LOOKUP_MS)
        } catch (e: RuntimeException) {
            Log.w(SESSION, "remote service lookup failed", e)
        }
    }

    @Suppress("DEPRECATION") // registerServiceInfoCallback needs API 34
    private fun resolve(info: NsdServiceInfo) {
        if (!info.serviceName.equals(serviceName, ignoreCase = true)) return // the session ended meanwhile
        try {
            nsd.resolveService(info, object : NsdManager.ResolveListener {
                override fun onServiceResolved(resolved: NsdServiceInfo) {
                    val host = resolved.host ?: return
                    main.post {
                        if (!resolved.serviceName.equals(serviceName, ignoreCase = true)) return@post
                        address = InetSocketAddress(host, resolved.port)
                        Log.i(SESSION, "sender's remote service found")
                        stopDiscovery()
                    }
                }

                override fun onResolveFailed(failed: NsdServiceInfo, errorCode: Int) {
                    Log.w(SESSION, "remote service could not be resolved ($errorCode)")
                }
            })
        } catch (e: RuntimeException) {
            Log.w(SESSION, "remote service could not be resolved", e)
        }
    }

    private fun stopDiscovery() {
        main.removeCallbacks(stopLookup)
        val listener = discovery ?: return
        discovery = null
        try {
            nsd.stopServiceDiscovery(listener)
        } catch (_: RuntimeException) {
            // already stopped
        }
    }

    private companion object {
        const val SERVICE_TYPE = "_dacp._tcp"
        const val TIMEOUT_MS = 2000
        const val LOOKUP_MS = 30_000L
    }
}
