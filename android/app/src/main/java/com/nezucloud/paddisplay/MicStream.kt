// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Process
import android.os.SystemClock
import android.util.Log
import java.io.DataInputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * Sends this tablet's microphone to the PC on its own connection, where PadDisplay hands it to a virtual microphone (its own driver, or VB-CABLE)
 * so apps there see a microphone. Raw 16-bit mono PCM at 48 kHz in 10 ms packets: no codec, so no encode delay.
 *
 * The microphone only runs while an app on the PC records from that virtual microphone: the PC says so with MIC_STATE, and
 * the tablet opens its microphone on 1 and closes it on 0, like the camera. Nothing is captured otherwise, and Android's
 * microphone indicator is off. Only created when the user switched the microphone on.
 */
class MicStream(
    private val host: String,
    private val port: Int,
    private val pin: Int,
    private val trust: HostTrust?, // null over USB; otherwise the same pinned TLS as the video connection
    private val listener: Listener,
) {
    interface Listener {
        fun onMicState(active: Boolean)
        fun onMicNotice(message: String)
    }

    companion object {
        private const val TAG = "PadMic"
        private const val RATE = 48000
        private const val SAMPLES = 480 // 10 ms
        private const val KEEPALIVE_MS = 2000L // while the microphone is off: the PC treats 10 s of silence as a lost tablet
        // A PC whose PadDisplay is from before MIC_STATE never sends it. It expects audio all the time, so after this
        // long without a word from the PC the microphone runs anyway. The PC sends its first MIC_STATE within milliseconds.
        private const val OLD_PC_AFTER_MS = 3000L
    }

    @Volatile private var closed = false
    @Volatile private var noticed = false
    @Volatile private var socket: Socket? = null
    @Volatile private var rawSocket: Socket? = null
    private var thread: Thread? = null

    private val lock = ReentrantLock()
    private val pcSpoke = lock.newCondition()
    private var wanted = false  // an app on the PC is recording (guarded by lock)
    private var pcSaid = false  // the PC has sent a MIC_STATE (guarded by lock)

    fun start() {
        thread = Thread({ run() }, "pd-mic").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun close() {
        closed = true
        Tls.hangUp(rawSocket)
        lock.withLock { pcSpoke.signalAll() }
    }

    private fun onPcState(recording: Boolean) = lock.withLock {
        wanted = recording
        pcSaid = true
        pcSpoke.signalAll()
    }

    /** Should the microphone run now? Waits up to [waitMs] for the PC to say something new when the answer is no. */
    private fun shouldCapture(connectedAtMs: Long, waitMs: Long): Boolean = lock.withLock {
        fun answer() = wanted || (!pcSaid && SystemClock.elapsedRealtime() - connectedAtMs >= OLD_PC_AFTER_MS)
        if (!answer() && !closed && waitMs > 0) pcSpoke.await(waitMs, TimeUnit.MILLISECONDS)
        answer()
    }

    // VOICE_COMMUNICATION turns on the tablet's echo cancellation and noise suppression where it has them: without
    // it, PC audio playing on this tablet's speaker would be heard again by the microphone and sent back to the PC.
    @SuppressLint("MissingPermission") // the switch asks for RECORD_AUDIO before it can be turned on
    private fun openMicrophone(): AudioRecord {
        val minBuf = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val record = AudioRecord.Builder()
            .setAudioSource(MediaRecorder.AudioSource.VOICE_COMMUNICATION)
            .setAudioFormat(AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT).setSampleRate(RATE)
                .setChannelMask(AudioFormat.CHANNEL_IN_MONO).build())
            .setBufferSizeInBytes(maxOf(minBuf, SAMPLES * 2 * 4))
            .build()
        if (record.state != AudioRecord.STATE_INITIALIZED) {
            record.release()
            throw IOException("The microphone could not be opened")
        }
        record.startRecording()
        return record
    }

    private fun closeMicrophone(record: AudioRecord) {
        try { record.stop() } catch (_: IllegalStateException) {}
        record.release()
    }

    private fun run() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        var record: AudioRecord? = null
        try {
            val raw = Socket()
            rawSocket = raw
            socket = raw
            raw.tcpNoDelay = true
            raw.connect(InetSocketAddress(host, port), 3000)
            val s: Socket = if (trust != null) Tls.wrap(raw, host, port, trust).also { socket = it } else raw
            val out = s.getOutputStream()
            out.write(Protocol.message(Protocol.MIC_HELLO, 7) {
                it.putShort(Protocol.VERSION.toShort())
                it.putInt(pin)
                it.put(0)
            })
            out.flush()
            val connectedAt = SystemClock.elapsedRealtime()
            // The PC says when to capture, and complains when it cannot take the microphone at all: read that on a side thread.
            Thread({ readFromPc(s) }, "pd-mic-rx").apply { isDaemon = true; start() }
            Log.i(TAG, "microphone connected to $host:$port, waiting for an app on the PC to record")

            val msg = ByteArray(Protocol.HEADER_SIZE + 8 + SAMPLES * 2) // one buffer reused for every packet: no allocation while streaming
            val bb = ByteBuffer.wrap(msg).order(ByteOrder.LITTLE_ENDIAN)
            bb.put(0, Protocol.MIC_DATA.toByte())
            bb.putInt(1, 8 + SAMPLES * 2)
            val keepalive = Protocol.message(Protocol.MIC_DATA, 8) { it.putLong(0) } // no samples: the PC only notes that we are here
            val pcm = ShortArray(SAMPLES)
            var lastSent = connectedAt
            while (!closed) {
                if (!shouldCapture(connectedAt, if (record == null) KEEPALIVE_MS / 2 else 0)) {
                    record?.let {
                        closeMicrophone(it)
                        record = null
                        listener.onMicState(false)
                        Log.i(TAG, "microphone off: no app on the PC records")
                    }
                    if (SystemClock.elapsedRealtime() - lastSent >= KEEPALIVE_MS) {
                        out.write(keepalive)
                        out.flush()
                        lastSent = SystemClock.elapsedRealtime()
                    }
                    continue
                }
                val mic = record ?: openMicrophone().also {
                    record = it
                    listener.onMicState(true)
                    Log.i(TAG, "microphone on: an app on the PC records")
                }
                var got = 0
                while (got < SAMPLES && !closed) {
                    val n = mic.read(pcm, got, SAMPLES - got) // blocks for the 10 ms of audio
                    if (n < 0) throw IOException("microphone read failed ($n)")
                    got += n
                }
                if (closed) break
                bb.putLong(Protocol.HEADER_SIZE, SystemClock.elapsedRealtimeNanos() / 1000)
                bb.position(Protocol.HEADER_SIZE + 8)
                bb.asShortBuffer().put(pcm)
                out.write(msg)
                out.flush()
                lastSent = SystemClock.elapsedRealtime()
            }
        } catch (e: IOException) {
            // The PC's own explanation (no virtual microphone, switched off) is better than "connection reset": show that one if it came.
            if (!closed && !noticed) listener.onMicNotice(e.message ?: "The microphone connection ended")
            Log.w(TAG, "microphone ended: $e")
        } catch (e: RuntimeException) {
            if (!closed) listener.onMicNotice("The microphone could not be used: ${e.message}")
            Log.w(TAG, "microphone failed: $e")
        } finally {
            closed = true
            record?.let { closeMicrophone(it) }
            try { rawSocket?.close() } catch (_: IOException) {}
            try { socket?.close() } catch (_: IOException) {}
            listener.onMicState(false)
        }
    }

    private fun readFromPc(s: Socket) {
        try {
            val input = DataInputStream(s.getInputStream())
            val header = ByteArray(Protocol.HEADER_SIZE)
            val hb = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
            while (!closed) {
                input.readFully(header)
                val type = header[0].toInt() and 0xFF
                val len = hb.getInt(1)
                if (len < 0 || len > 64 shl 10) return
                val payload = ByteArray(len)
                input.readFully(payload)
                when (type) {
                    Protocol.MIC_STATE -> if (len >= 1) onPcState(payload[0].toInt() != 0)
                    Protocol.NOTICE -> { noticed = true; listener.onMicNotice(String(payload, Charsets.UTF_8)) }
                    Protocol.ERROR -> if (len > 1) { noticed = true; listener.onMicNotice(String(payload, 1, len - 1, Charsets.UTF_8)) }
                }
            }
        } catch (_: IOException) {
        }
    }
}
