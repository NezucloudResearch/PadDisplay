// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.SocketTimeoutException

/** Listens for the host's UDP broadcast beacon ("PADDISPLAY <version> <port> <name>"). */
class Discovery(context: Context, private val onUpdate: (List<Host>) -> Unit) {

    data class Host(val name: String, val address: String, val port: Int, val lastSeenMs: Long)

    private val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private val main = Handler(Looper.getMainLooper())
    private val hosts = LinkedHashMap<String, Host>()
    @Volatile private var running = false
    private var socket: DatagramSocket? = null
    private var thread: Thread? = null
    private var lock: WifiManager.MulticastLock? = null

    fun start() {
        if (running) return
        running = true
        // Some devices filter broadcast packets unless a multicast lock is held.
        lock = wifi.createMulticastLock("paddisplay").apply { setReferenceCounted(false); acquire() }
        thread = Thread({ loop() }, "pd-discovery").apply { start() }
    }

    fun stop() {
        running = false
        socket?.close()
        thread?.join(1500)
        lock?.release()
        lock = null
    }

    private fun loop() {
        val buf = ByteArray(512)
        while (running) {
            try {
                DatagramSocket(null).use { s ->
                    socket = s
                    s.reuseAddress = true
                    s.broadcast = true
                    s.soTimeout = 1000
                    s.bind(InetSocketAddress(Protocol.DISCOVERY_PORT))
                    while (running) {
                        try {
                            val p = DatagramPacket(buf, buf.size)
                            s.receive(p)
                            parse(String(p.data, 0, p.length, Charsets.UTF_8), p.address.hostAddress ?: continue)
                        } catch (_: SocketTimeoutException) {
                        }
                        prune()
                    }
                }
            } catch (e: Exception) {
                if (running) SystemClock.sleep(1000)
            }
        }
    }

    private fun parse(msg: String, address: String) {
        val parts = msg.trim().split(" ", limit = 4)
        if (parts.size < 4 || parts[0] != "PADDISPLAY") return
        val port = parts[2].toIntOrNull() ?: return
        synchronized(hosts) { hosts[address] = Host(parts[3], address, port, SystemClock.elapsedRealtime()) }
        publish()
    }

    private fun prune() {
        val now = SystemClock.elapsedRealtime()
        val removed = synchronized(hosts) { hosts.values.removeIf { now - it.lastSeenMs > 5000 } }
        if (removed) publish()
    }

    private fun publish() {
        val list = synchronized(hosts) { hosts.values.toList() }
        main.post { onUpdate(list) }
    }
}
