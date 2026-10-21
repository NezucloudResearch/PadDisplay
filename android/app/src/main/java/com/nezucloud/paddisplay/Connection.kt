// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.os.SystemClock
import android.util.Log
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * TCP link to the host. A reader thread parses host messages and calls [Listener] on that
 * thread; outgoing messages are queued and written by a writer thread (safe from the UI thread).
 * With a [trust] (every connection except USB) the link is TLS, pinned to the PC's certificate.
 */
class Connection(private val host: String, private val port: Int, private val trust: HostTrust?, private val listener: Listener) {

    interface Listener {
        fun onFingerprintChanged(fingerprint: String)
        fun onHostHello(version: Int, codec: Int, width: Int, height: Int, fps: Int, bitrateKbps: Int)
        fun onConfig(data: ByteArray, length: Int)
        fun onFrame(ptsUs: Long, keyframe: Boolean, data: ByteArray, offset: Int, length: Int)
        fun onPing(rttUs: Int, bitrateKbps: Int)
        fun onError(code: Int, message: String)
        fun onRumble(index: Int, large: Int, small: Int)
        fun onNotice(message: String)
        fun onLockState(frozen: Boolean) // the PC is locked and cannot be shown (true) or streaming again (false)
        fun onClosed(reason: String?)
    }

    companion object {
        private const val TAG = "PadConn"
        // An older PadDisplay on the PC treats the TLS hello as garbage and hangs up, which looks like any other handshake failure.
        const val HANDSHAKE_FAILED = "Could not make a secure connection. If PadDisplay on the PC is older than this app, update it there."
    }

    private val queue = LinkedBlockingQueue<ByteArray>()
    @Volatile private var socket: Socket? = null
    @Volatile private var rawSocket: Socket? = null // the TCP socket under the TLS one
    @Volatile private var closed = false
    private var reader: Thread? = null
    private var writer: Thread? = null

    fun start(hello: ByteArray) {
        reader = Thread({ readLoop(hello) }, "pd-reader").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun send(msg: ByteArray) {
        if (!closed) queue.offer(msg)
    }

    fun close() {
        closed = true
        Tls.hangUp(rawSocket) // this runs on the UI thread
        writer?.interrupt()
    }

    private fun readLoop(hello: ByteArray) {
        var reason: String? = null
        var handshaking = false
        try {
            val raw = Socket()
            rawSocket = raw
            socket = raw
            raw.tcpNoDelay = true
            raw.trafficClass = 0xA0 // DSCP CS5 -> Wi-Fi WMM video access category
            raw.receiveBufferSize = 1 shl 20
            raw.sendBufferSize = 64 shl 10
            raw.soTimeout = 10_000 // host pings every 500 ms
            val t0 = SystemClock.elapsedRealtime()
            raw.connect(InetSocketAddress(host, port), 3000)
            Log.i(TAG, "connected to $host:$port in ${SystemClock.elapsedRealtime() - t0} ms")
            if (closed) return
            val s: Socket = if (trust != null) {
                val t1 = SystemClock.elapsedRealtime()
                handshaking = true
                val ssl = Tls.wrap(raw, host, port, trust)
                handshaking = false
                socket = ssl
                Log.i(TAG, "TLS ${ssl.session.protocol} ${ssl.session.cipherSuite} in ${SystemClock.elapsedRealtime() - t1} ms")
                ssl
            } else raw
            if (closed) return
            s.getOutputStream().apply { write(hello); flush() }
            writer = Thread({ writeLoop(s) }, "pd-writer").apply { start() }

            val input = DataInputStream(BufferedInputStream(s.getInputStream(), 256 shl 10))
            val header = ByteArray(Protocol.HEADER_SIZE)
            val hb = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
            var payload = ByteArray(512 shl 10)
            var pb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
            var firstMessage = true
            var firstFrame = true
            while (!closed) {
                input.readFully(header)
                val type = header[0].toInt() and 0xFF
                val len = hb.getInt(1)
                if (len < 0 || len > 64 shl 20) throw IOException("bad message length $len")
                if (len > payload.size) {
                    payload = ByteArray(len + (len shr 1))
                    pb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
                }
                input.readFully(payload, 0, len)
                // Anything but an error means the PC accepted the PIN, so this is the PC to remember.
                if (firstMessage && type != Protocol.ERROR) trust?.confirm()
                if (firstMessage || (firstFrame && type == Protocol.FRAME)) {
                    Log.i(TAG, "first ${if (type == Protocol.FRAME) "frame" else "message 0x%02x".format(type)} " +
                        "${SystemClock.elapsedRealtime() - t0} ms after connect")
                    firstMessage = false
                    if (type == Protocol.FRAME) firstFrame = false
                }
                when (type) {
                    Protocol.HOST_HELLO -> if (len >= 12) listener.onHostHello(
                        pb.getShort(0).toInt() and 0xFFFF,
                        payload[2].toInt() and 0xFF,
                        pb.getShort(3).toInt() and 0xFFFF,
                        pb.getShort(5).toInt() and 0xFFFF,
                        payload[7].toInt() and 0xFF,
                        pb.getInt(8),
                    )
                    Protocol.CONFIG -> listener.onConfig(payload, len)
                    Protocol.FRAME -> if (len > 9) {
                        val pts = pb.getLong(0)
                        listener.onFrame(pts, (payload[8].toInt() and 1) != 0, payload, 9, len - 9)
                        // Ack after the frame is queued to the decoder; the host limits unacked frames,
                        // which keeps the network queue (and so latency) to a few frames.
                        send(Protocol.message(Protocol.FRAME_ACK, 8) { it.putLong(pts) })
                    }
                    Protocol.PING -> if (len >= 16) {
                        val hostTime = pb.getLong(0)
                        send(Protocol.message(Protocol.PONG, 8) { it.putLong(hostTime) })
                        listener.onPing(pb.getInt(8), pb.getInt(12))
                    }
                    Protocol.RUMBLE -> if (len >= 3) listener.onRumble(
                        payload[0].toInt() and 0xFF, payload[1].toInt() and 0xFF, payload[2].toInt() and 0xFF)
                    Protocol.NOTICE -> listener.onNotice(String(payload, 0, len, Charsets.UTF_8))
                    Protocol.LOCK_STATE -> if (len >= 1) listener.onLockState(payload[0].toInt() != 0)
                    Protocol.ERROR -> {
                        val code = if (len > 0) payload[0].toInt() and 0xFF else 0
                        val text = if (len > 1) String(payload, 1, len - 1, Charsets.UTF_8) else "Error $code"
                        listener.onError(code, text)
                        closed = true
                    }
                }
            }
        } catch (e: IOException) {
            val changed = Tls.fingerprintChange(e)
            if (changed != null) {
                // Not a failure to retry: the user has to decide whether this is still their PC.
                closed = true
                listener.onFingerprintChanged(changed.seen)
            } else if (!closed) {
                reason = if (handshaking) HANDSHAKE_FAILED else e.message ?: e.javaClass.simpleName
            }
            Log.w(TAG, "connection ended: $e")
        } finally {
            closed = true
            try { rawSocket?.close() } catch (_: IOException) {}
            try { socket?.close() } catch (_: IOException) {}
            writer?.interrupt()
            listener.onClosed(reason)
        }
    }

    private fun writeLoop(s: Socket) {
        try {
            val out = BufferedOutputStream(s.getOutputStream(), 16 shl 10)
            // Some ROMs (HarmonyOS) doze the Wi-Fi radio despite Wi-Fi locks; the router then holds
            // or drops packets for the tablet until it transmits. Sending a tiny heartbeat whenever
            // nothing else went out for 20 ms keeps the downlink flowing.
            val keepalive = Protocol.message(Protocol.KEEPALIVE, 0)
            while (!closed) {
                val first = queue.poll(20, TimeUnit.MILLISECONDS) ?: keepalive
                out.write(first)
                // Coalesce whatever else is already queued into the same flush.
                while (true) out.write(queue.poll() ?: break)
                out.flush()
            }
        } catch (_: InterruptedException) {
        } catch (_: IOException) {
            try { s.close() } catch (_: IOException) {}
        }
    }
}
