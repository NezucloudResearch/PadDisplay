// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build
import android.util.Log
import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.sign

/**
 * Optional PC audio: a second connection that only receives 16-bit stereo PCM.
 *
 * Smooth playback without drifting behind the picture:
 * - A small jitter buffer is kept near a target of 30 ms (the mixer pulls audio in bursts, so the
 *   queue naturally swings by ~30 ms). A real underrun during continuous sound raises the target by
 *   10 ms (up to 80 ms, for Wi-Fi jitter); it shrinks back by 5 ms per stable 20 s.
 * - While the queue is above target, silent packets are skipped (inaudible) to catch up quickly.
 * - The PC's and the tablet's audio clocks differ slightly, so the queue would slowly creep. Instead
 *   of dropping chunks (audible clicks), playback speed is nudged by up to +2 / -1 % through a tiny
 *   linear resampler: inaudible, and it also absorbs network jitter.
 * - After silence or a starved buffer, the cushion is refilled with silence before new audio, so
 *   the start of a sound doesn't crackle. Hard drops only happen beyond [hardCapMs] (e.g. the
 *   tablet's audio system stalling).
 */
class AudioStream(private val host: String, private val port: Int, private val pin: Int, private val mutePc: Boolean) {

    companion object {
        private const val TAG = "PadAudio"
        private const val minTargetMs = 30.0   // USB; grows automatically on jittery links
        private const val maxTargetMs = 80.0
        private const val deadbandMs = 8.0
        private const val gainPerMs = 0.0005   // 1 % speed change per 20 ms of error beyond the deadband
        private const val maxFaster = 0.02
        private const val maxSlower = 0.01
        private const val hardCapMs = 200.0
        private const val trackBufferMs = 300  // room for the hard cap; latency is set by the target, not this
        private const val startThresholdMs = 15
    }

    @Volatile private var closed = false
    @Volatile private var socket: Socket? = null
    private var thread: Thread? = null

    // Stats for the overlay.
    @Volatile var queuedMs = 0
        private set
    @Volatile var droppedMs = 0L
        private set
    @Volatile var speedPercent = 0f
        private set
    @Volatile var refills = 0
        private set
    @Volatile var targetMs = minTargetMs
        private set

    fun start() {
        thread = Thread({ run() }, "pd-audio").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun close() {
        closed = true
        try { socket?.close() } catch (_: IOException) {}
    }

    private fun run() {
        var track: AudioTrack? = null
        try {
            val s = Socket()
            socket = s
            s.tcpNoDelay = true
            s.receiveBufferSize = 64 shl 10
            s.connect(InetSocketAddress(host, port), 3000)
            s.getOutputStream().apply {
                write(Protocol.message(Protocol.AUDIO_HELLO, 7) {
                    it.putShort(Protocol.VERSION.toShort())
                    it.putInt(pin)
                    it.put((if (mutePc) Protocol.AUDIO_MUTE_PC else 0).toByte())
                })
                flush()
            }
            val input = DataInputStream(BufferedInputStream(s.getInputStream(), 32 shl 10))
            val header = ByteArray(Protocol.HEADER_SIZE)
            val hb = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
            var payload = ByteArray(16 shl 10)
            var samples = ShortArray(8 shl 10)
            var out = ShortArray(8 shl 10)
            val resampler = DriftResampler()
            var rate = 48000
            var framesWritten = 0L
            var queueEma = -1.0
            var lastPacketNs = 0L
            var lastUnderrunNs = System.nanoTime()

            fun queuedFrames(t: AudioTrack): Long = framesWritten - (t.playbackHeadPosition.toLong() and 0xFFFFFFFFL)
            fun writeSilence(t: AudioTrack, ms: Double) {
                val frames = (rate * ms / 1000).toInt()
                val n = t.write(ShortArray(frames * 2), 0, frames * 2, AudioTrack.WRITE_NON_BLOCKING)
                if (n > 0) framesWritten += n / 2
            }

            while (!closed) {
                input.readFully(header)
                val type = header[0].toInt() and 0xFF
                val len = hb.getInt(1)
                if (len < 0 || len > 1 shl 20) throw IOException("bad audio message length $len")
                if (len > payload.size) payload = ByteArray(len)
                input.readFully(payload, 0, len)
                when (type) {
                    Protocol.AUDIO_FORMAT -> {
                        rate = ByteBuffer.wrap(payload, 0, 4).order(ByteOrder.LITTLE_ENDIAN).int
                        track?.release()
                        val t = createTrack(rate)
                        track = t
                        framesWritten = 0
                        targetMs = minTargetMs
                        queueEma = targetMs
                        resampler.reset()
                        // Start with the cushion already in place, then play.
                        if (Build.VERSION.SDK_INT >= 31) {
                            try { t.setStartThresholdInFrames(rate * startThresholdMs / 1000) } catch (e: Exception) { Log.w(TAG, "start threshold: $e") }
                        }
                        writeSilence(t, targetMs)
                        t.play()
                        Log.i(TAG, "audio $rate Hz, low-latency=${t.performanceMode == AudioTrack.PERFORMANCE_MODE_LOW_LATENCY}, " +
                            "track buffer ${t.bufferSizeInFrames * 1000 / rate} ms, target ${targetMs.toInt()} ms (adaptive)")
                    }
                    Protocol.AUDIO_DATA -> {
                        val t = track ?: continue
                        val frames = (len - 8) / 4
                        if (frames <= 0) continue
                        if (samples.size < frames * 2) samples = ShortArray(frames * 2)
                        ByteBuffer.wrap(payload, 8, frames * 4).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().get(samples, 0, frames * 2)

                        val now = System.nanoTime()
                        val continuous = lastPacketNs != 0L && now - lastPacketNs < 60_000_000L
                        lastPacketNs = now
                        var qMs = queuedFrames(t) * 1000.0 / rate
                        if (qMs < 2) { // starved (silence on the PC, or a hiccup): rebuild the cushion first
                            if (continuous) { // a real underrun mid-sound: the link needs more headroom
                                targetMs = minOf(maxTargetMs, targetMs + 10)
                                lastUnderrunNs = now
                                refills++
                            }
                            writeSilence(t, targetMs)
                            qMs += targetMs
                            queueEma = qMs
                        } else if (targetMs > minTargetMs && now - lastUnderrunNs > 20_000_000_000L) {
                            targetMs = maxOf(minTargetMs, targetMs - 5) // stable for a while: tighten again
                            lastUnderrunNs = now
                        }
                        queueEma = if (queueEma < 0) qMs else queueEma * 0.95 + qMs * 0.05
                        queuedMs = qMs.toInt()
                        if (queueEma > hardCapMs) { // the tablet's audio stalled: last resort
                            val ms = frames * 1000.0 / rate
                            droppedMs += ms.toLong()
                            queueEma -= ms
                            continue
                        }
                        if (queueEma > targetMs + deadbandMs && isSilent(samples, frames)) {
                            queueEma -= frames * 1000.0 / rate // skipping silence is inaudible: catch up fast
                            continue
                        }
                        val err = queueEma - targetMs
                        val adjust = if (abs(err) <= deadbandMs) 0.0
                            else ((err - sign(err) * deadbandMs) * gainPerMs).coerceIn(-maxSlower, maxFaster)
                        speedPercent = (adjust * 100).toFloat()

                        val need = (frames / (1 + adjust)).toInt() + 4
                        if (out.size < need * 2) out = ShortArray(need * 2)
                        val produced = resampler.process(samples, frames, 1 + adjust, out)
                        val n = t.write(out, 0, produced * 2, AudioTrack.WRITE_NON_BLOCKING)
                        if (n > 0) framesWritten += n / 2
                        if (n < produced * 2) droppedMs += (produced * 2 - maxOf(n, 0)) / 2 * 1000L / rate
                    }
                    Protocol.ERROR -> {
                        Log.w(TAG, "host refused audio: ${String(payload, 1, maxOf(0, len - 1))}")
                        return
                    }
                }
            }
        } catch (e: IOException) {
            if (!closed) Log.w(TAG, "audio connection ended: $e")
        } finally {
            track?.let {
                try { it.stop() } catch (_: IllegalStateException) {}
                it.release()
            }
            try { socket?.close() } catch (_: IOException) {}
        }
    }

    private fun isSilent(s: ShortArray, frames: Int): Boolean {
        for (i in 0 until frames * 2) if (s[i] > 64 || s[i] < -64) return false
        return true
    }

    private fun createTrack(rate: Int): AudioTrack =
        AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build())
            .setAudioFormat(AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(rate)
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                .build())
            .setBufferSizeInBytes(maxOf(
                AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT),
                rate / 1000 * 4 * trackBufferMs))
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()

    /**
     * Stereo linear-interpolation resampler for tiny speed changes. Continuous across packets: the
     * last frame of the previous packet is kept so there is no seam. ratio 1.0 = bit-exact copy.
     */
    private class DriftResampler {
        private var pos = 0.0 // read position in the current packet, in frames; -1 = previous packet's last frame
        private var prevL = 0f
        private var prevR = 0f

        fun reset() {
            pos = 0.0
            prevL = 0f
            prevR = 0f
        }

        fun process(inp: ShortArray, frames: Int, ratio: Double, out: ShortArray): Int {
            var n = 0
            val max = out.size / 2
            while (pos <= frames - 1 && n < max) {
                val i0 = floor(pos).toInt() // -1 .. frames-1
                val frac = (pos - i0).toFloat()
                val l0 = if (i0 < 0) prevL else inp[2 * i0].toFloat()
                val r0 = if (i0 < 0) prevR else inp[2 * i0 + 1].toFloat()
                val i1 = i0 + 1
                val l1 = if (i1 < frames) inp[2 * i1].toFloat() else l0
                val r1 = if (i1 < frames) inp[2 * i1 + 1].toFloat() else r0
                out[2 * n] = (l0 + (l1 - l0) * frac).toInt().toShort()
                out[2 * n + 1] = (r0 + (r1 - r0) * frac).toInt().toShort()
                n++
                pos += ratio
            }
            pos -= frames
            prevL = inp[2 * (frames - 1)].toFloat()
            prevR = inp[2 * (frames - 1) + 1].toFloat()
            return n
        }
    }
}
