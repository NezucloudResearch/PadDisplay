// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket

/**
 * The connection the audio, microphone and camera streams each open to the PC: plain TCP over USB, where [trust] is null,
 * otherwise TLS pinned to the PC's certificate, like the video connection.
 */
class PcSocket(private val host: String, private val port: Int, private val trust: HostTrust?) {

    private companion object {
        const val CONNECT_TIMEOUT_MS = 3000
    }

    @Volatile private var tcp: Socket? = null
    @Volatile private var top: Socket? = null // TLS over [tcp], or [tcp] itself

    /** Blocking: on the stream's own thread. [tune] sets socket options that have to be in place before the connect. */
    fun connect(tune: (Socket) -> Unit = {}): Socket {
        val raw = Socket()
        tcp = raw
        top = raw
        raw.tcpNoDelay = true
        tune(raw)
        raw.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
        val s: Socket = if (trust != null) Tls.wrap(raw, host, port, trust).also { top = it } else raw
        return s
    }

    /** Ends it from any thread, the UI thread included: see [Tls.hangUp]. */
    fun hangUp() = Tls.hangUp(tcp)

    /** On the stream's own thread, once it has finished. */
    fun closeQuietly() {
        try { tcp?.close() } catch (_: IOException) {}
        try { top?.close() } catch (_: IOException) {}
    }
}
