// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong

/**
 * Hardware decoder tuned for latency: vendor low-latency mode, frames rendered as soon as they
 * are decoded, and if several are ready only the newest is shown.
 */
class VideoDecoder(private val surface: Surface) {

    val framesRendered = AtomicInteger()
    val framesDropped = AtomicInteger()
    val decodeUsTotal = AtomicLong()
    val decodeCount = AtomicInteger()
    var decoderName = ""
        private set

    /** The codec failed for good (a fatal CodecException, or its output thread died): only a new one shows pictures again. */
    @Volatile var broken = false
        private set

    private var codec: MediaCodec? = null
    private var outputThread: Thread? = null
    @Volatile private var running = false
    private var waitingForKeyframe = true
    private val queuedAt = ConcurrentHashMap<Long, Long>()

    companion object {
        private const val TAG = "PadDecoder"

        fun mimeFor(codec: Int) = if (codec == Protocol.CODEC_HEVC) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC

        // Enumerating codecs can take hundreds of ms on some ROMs; do it once.
        private val codecInfos: Array<MediaCodecInfo> by lazy { MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos }

        private fun isHardwareDecoder(info: MediaCodecInfo, mime: String) =
            !info.isEncoder && info.isHardwareAccelerated && info.supportedTypes.any { it.equals(mime, true) }

        fun hasHardwareDecoder(mime: String) = codecInfos.any { isHardwareDecoder(it, mime) }

        private fun pickDecoder(mime: String) = codecInfos.firstOrNull { isHardwareDecoder(it, mime) }
    }

    @Synchronized
    fun configure(codecType: Int, width: Int, height: Int, fps: Int) {
        release()
        val mime = mimeFor(codecType)
        val name = pickDecoder(mime)?.name
        val c = if (name != null) MediaCodec.createByCodecName(name) else MediaCodec.createDecoderByType(mime)
        decoderName = c.name

        fun format(lowLatency: Boolean) = MediaFormat.createVideoFormat(mime, width, height).apply {
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, width * height)
            if (lowLatency) {
                setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
                setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE.toInt())
                if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                // Qualcomm (Snapdragon) vendor extensions: no output reordering / buffering.
                setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
                setInteger("vendor.qti-ext-dec-picture-order.enable", 1)
            }
        }
        try {
            c.configure(format(true), surface, null, 0)
        } catch (e: Exception) {
            Log.w(TAG, "low-latency configure failed, retrying plain: $e")
            c.reset()
            c.configure(format(false), surface, null, 0)
        }
        c.start()
        codec = c
        waitingForKeyframe = true
        broken = false
        running = true
        outputThread = Thread({ outputLoop(c) }, "pd-decoder-out").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
        Log.i(TAG, "decoder $decoderName ${width}x$height@$fps")
    }

    /** Queues codec parameter sets (VPS/SPS/PPS). */
    @Synchronized
    fun queueConfig(data: ByteArray, length: Int) {
        val c = codec ?: return
        try {
            val idx = c.dequeueInputBuffer(100_000)
            if (idx < 0) return
            c.getInputBuffer(idx)!!.apply { clear(); put(data, 0, length) }
            c.queueInputBuffer(idx, 0, length, 0, MediaCodec.BUFFER_FLAG_CODEC_CONFIG)
        } catch (e: IllegalStateException) {
            noteFailure(e)
        }
    }

    /** Returns false when the frame was dropped and a keyframe is needed to resync. */
    @Synchronized
    fun queueFrame(data: ByteArray, offset: Int, length: Int, ptsUs: Long, keyframe: Boolean): Boolean {
        val c = codec ?: return true
        if (waitingForKeyframe && !keyframe) return false
        try {
            val idx = c.dequeueInputBuffer(50_000)
            if (idx < 0) {
                waitingForKeyframe = true
                return false
            }
            val buf = c.getInputBuffer(idx)!!
            buf.clear()
            if (length > buf.capacity()) {
                c.queueInputBuffer(idx, 0, 0, ptsUs, 0)
                waitingForKeyframe = true
                return false
            }
            buf.put(data, offset, length)
            queuedAt[ptsUs] = System.nanoTime()
            c.queueInputBuffer(idx, 0, length, ptsUs, if (keyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
        } catch (e: IllegalStateException) {
            noteFailure(e)
            waitingForKeyframe = true
            return false
        }
        waitingForKeyframe = false
        return true
    }

    // A transient CodecException passes by itself; anything else means this codec will not decode again.
    private fun noteFailure(e: IllegalStateException) {
        if (!running || broken || (e is MediaCodec.CodecException && e.isTransient)) return
        Log.w(TAG, "decoder $decoderName failed: $e")
        broken = true
    }

    private fun outputLoop(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        try {
            while (running) {
                val idx = c.dequeueOutputBuffer(info, 100_000)
                if (idx < 0) continue
                var current = idx
                var pts = info.presentationTimeUs
                // Skip straight to the newest decoded frame.
                while (true) {
                    val next = c.dequeueOutputBuffer(info, 0)
                    if (next < 0) break
                    c.releaseOutputBuffer(current, false)
                    queuedAt.remove(pts)
                    framesDropped.incrementAndGet()
                    current = next
                    pts = info.presentationTimeUs
                }
                c.releaseOutputBuffer(current, true)
                framesRendered.incrementAndGet()
                queuedAt.remove(pts)?.let {
                    decodeUsTotal.addAndGet((System.nanoTime() - it) / 1000)
                    decodeCount.incrementAndGet()
                }
            }
        } catch (e: IllegalStateException) {
            // Also the normal end when release() stops the codec under this thread: running is false then. Otherwise
            // nothing shows decoded pictures any more, transient error or not.
            if (running) {
                Log.w(TAG, "decoder $decoderName output failed: $e")
                broken = true
            }
        }
    }

    @Synchronized
    fun release() {
        running = false
        outputThread?.join(500)
        outputThread = null
        codec?.let {
            try { it.stop() } catch (_: Exception) {}
            it.release()
        }
        codec = null
        queuedAt.clear()
    }
}
