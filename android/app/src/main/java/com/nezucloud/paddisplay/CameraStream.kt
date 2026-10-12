// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import java.io.DataInputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.Executor

/**
 * Sends this tablet's camera to the PC on its own connection, where it shows up as the PC's webcam.
 *
 * The camera only runs while an app on the PC has the webcam open: the PC says so with CAM_STATE, and the tablet
 * starts the camera and the encoder on 1 and shuts both (camera light off) on 0. Nothing is captured otherwise.
 * Camera2 feeds a hardware H.264 encoder directly through its input surface: no copy through the CPU.
 */
class CameraStream(
    private val context: Context,
    private val host: String,
    private val port: Int,
    private val pin: Int,
    private val trust: HostTrust?, // null over USB; otherwise the same pinned TLS as the video connection
    private val front: Boolean,
    private val height720: Boolean, // true = 1280x720, false = 1920x1080
    private val listener: Listener,
) {
    interface Listener {
        fun onCameraState(streaming: Boolean)
        fun onCameraNotice(message: String)
    }

    companion object {
        private const val TAG = "PadCam"
        private const val FPS = 30
    }

    @Volatile private var closed = false
    @Volatile private var noticed = false
    @Volatile private var socket: Socket? = null
    @Volatile private var rawSocket: Socket? = null
    private var thread: Thread? = null
    private val sendLock = Any()
    private var out: java.io.OutputStream? = null

    private val cameraThread = HandlerThread("pd-camera").apply { start() }
    private val cameraHandler = Handler(cameraThread.looper)
    private val executor = Executor { cameraHandler.post(it) }

    // Only touched on cameraHandler.
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var encoder: MediaCodec? = null
    private var encoderThread: Thread? = null
    private var inputSurface: Surface? = null
    private var size = Size(1280, 720)

    fun start() {
        thread = Thread({ run() }, "pd-cam").apply { start() }
    }

    fun close() {
        closed = true
        Tls.hangUp(rawSocket)
        cameraHandler.post {
            stopCapture()
            cameraThread.quitSafely()
        }
    }

    private fun run() {
        try {
            size = chooseSize()
            val raw = Socket()
            rawSocket = raw
            socket = raw
            raw.tcpNoDelay = true
            raw.trafficClass = 0xA0 // DSCP CS5, like the video
            raw.connect(InetSocketAddress(host, port), 3000)
            val s: Socket = if (trust != null) Tls.wrap(raw, host, port, trust).also { socket = it } else raw
            out = s.getOutputStream()
            send(Protocol.message(Protocol.CAM_HELLO, 12) {
                it.putShort(Protocol.VERSION.toShort())
                it.putInt(pin)
                it.put(Protocol.CODEC_H264.toByte())
                it.putShort(size.width.toShort())
                it.putShort(size.height.toShort())
                it.put(FPS.toByte())
            })
            val input = DataInputStream(s.getInputStream())
            val header = ByteArray(Protocol.HEADER_SIZE)
            val hb = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
            while (!closed) {
                input.readFully(header)
                val type = header[0].toInt() and 0xFF
                val len = hb.getInt(1)
                if (len < 0 || len > 64 shl 10) throw IOException("bad message length $len")
                val payload = ByteArray(len)
                input.readFully(payload)
                when (type) {
                    Protocol.CAM_STATE -> {
                        val active = len >= 1 && payload[0].toInt() != 0
                        cameraHandler.post { if (active) startCapture() else stopCapture() }
                    }
                    Protocol.CAM_KEYFRAME_REQ -> cameraHandler.post { requestKeyframe() }
                    Protocol.NOTICE -> { noticed = true; listener.onCameraNotice(String(payload, Charsets.UTF_8)) }
                    Protocol.ERROR -> if (len > 1) { noticed = true; listener.onCameraNotice(String(payload, 1, len - 1, Charsets.UTF_8)) }
                }
            }
        } catch (e: IOException) {
            if (!closed && !noticed) listener.onCameraNotice(e.message ?: "The camera connection ended")
            Log.w(TAG, "camera ended: $e")
        } catch (e: RuntimeException) {
            if (!closed) listener.onCameraNotice("The camera could not be used: ${e.message}")
            Log.w(TAG, "camera failed: $e")
        } finally {
            closed = true
            try { rawSocket?.close() } catch (_: IOException) {}
            try { socket?.close() } catch (_: IOException) {}
            cameraHandler.post {
                stopCapture()
                cameraThread.quitSafely()
            }
        }
    }

    private fun send(msg: ByteArray) {
        synchronized(sendLock) {
            val o = out ?: throw IOException("not connected")
            o.write(msg)
            o.flush()
        }
    }

    /** The camera's output size closest to the chosen quality (16:9 first), which is what the PC's webcam shows. */
    private fun chooseSize(): Size {
        val wantW = if (height720) 1280 else 1920
        val wantH = if (height720) 720 else 1080
        val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val id = cameraId(manager) ?: throw IOException("This tablet has no ${if (front) "front" else "back"} camera")
        val map = manager.getCameraCharacteristics(id).get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val sizes = map?.getOutputSizes(MediaCodec::class.java) ?: return Size(wantW, wantH)
        return sizes.filter { it.width * 9 == it.height * 16 && it.width <= 1920 }
            .minByOrNull { Math.abs(it.width - wantW) } ?: sizes.minByOrNull { Math.abs(it.width - wantW) + Math.abs(it.height - wantH) } ?: Size(wantW, wantH)
    }

    private fun cameraId(manager: CameraManager): String? {
        val facing = if (front) CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK
        return manager.cameraIdList.firstOrNull { manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == facing }
    }

    // ---- everything below runs on cameraHandler --------------------------------------------------------------

    @SuppressLint("MissingPermission") // the switch asks for CAMERA before it can be turned on
    private fun startCapture() {
        if (closed || camera != null || encoder != null) return // already starting or running
        try {
            val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
            val id = cameraId(manager) ?: throw IOException("no camera")
            startEncoder()
            manager.openCamera(id, object : CameraDevice.StateCallback() {
                override fun onOpened(device: CameraDevice) {
                    camera = device
                    createSession(device)
                }
                override fun onDisconnected(device: CameraDevice) {
                    device.close()
                    stopCapture()
                }
                override fun onError(device: CameraDevice, error: Int) {
                    device.close()
                    stopCapture()
                    listener.onCameraNotice("The camera reported error $error")
                }
            }, cameraHandler)
        } catch (e: Exception) {
            stopCapture()
            listener.onCameraNotice("The camera could not be started: ${e.message}")
        }
    }

    private fun createSession(device: CameraDevice) {
        val surface = inputSurface ?: return
        try {
            val request = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(surface)
                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range(FPS, FPS))
            }
            val config = SessionConfiguration(SessionConfiguration.SESSION_REGULAR, listOf(OutputConfiguration(surface)), executor,
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(s: CameraCaptureSession) {
                        if (camera == null) { s.close(); return }
                        session = s
                        try {
                            s.setRepeatingRequest(request.build(), null, cameraHandler)
                            listener.onCameraState(true)
                            Log.i(TAG, "camera streaming ${size.width}x${size.height}")
                        } catch (e: CameraAccessException) {
                            stopCapture()
                            listener.onCameraNotice("The camera could not be started: ${e.message}")
                        }
                    }
                    override fun onConfigureFailed(s: CameraCaptureSession) {
                        stopCapture()
                        listener.onCameraNotice("The camera could not be configured")
                    }
                })
            device.createCaptureSession(config)
        } catch (e: Exception) {
            stopCapture()
            listener.onCameraNotice("The camera could not be started: ${e.message}")
        }
    }

    /**
     * Encoders differ in which optional keys they accept, and a rejected key fails the whole configure. So try the full
     * low-latency set first, then fewer options, down to the keys every encoder takes.
     * [level] 0 = everything, 1 = no profile / B-frame / latency / priority hints, 2 = also no constant-bitrate request.
     */
    private fun encoderFormat(level: Int) = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, size.width, size.height).apply {
        setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        setInteger(MediaFormat.KEY_BIT_RATE, if (height720) 4_000_000 else 8_000_000)
        setInteger(MediaFormat.KEY_FRAME_RATE, FPS)
        // No periodic keyframes: the PC asks for one when it needs one (a new app opened the camera, or a decode problem),
        // exactly like the screen stream.
        setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 3600)
        if (level < 2) setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
        if (level < 1) {
            // No B-frames, so a late frame never waits behind a reordering delay.
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileMain)
            setInteger(MediaFormat.KEY_PRIORITY, 0) // real time
            if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LATENCY, 1)
        }
    }

    private fun startEncoder() {
        var codec: MediaCodec? = null
        var level = 0
        while (codec == null) {
            val c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            try {
                c.configure(encoderFormat(level), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                codec = c
            } catch (e: RuntimeException) {
                Log.w(TAG, "encoder configuration level $level rejected: $e")
                try { c.release() } catch (_: Exception) {}
                if (++level > 2) throw IOException("The tablet's video encoder rejected every configuration: ${e.message}")
            }
        }
        Log.i(TAG, "encoder ${codec.name} configured at level $level")
        inputSurface = codec.createInputSurface()
        codec.start()
        encoder = codec
        encoderThread = Thread({ drainEncoder(codec) }, "pd-cam-enc").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun drainEncoder(codec: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        try {
            while (!closed) {
                val index = codec.dequeueOutputBuffer(info, 100_000)
                if (index < 0) {
                    if (index == MediaCodec.INFO_TRY_AGAIN_LATER || index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) continue
                    continue
                }
                val buf = codec.getOutputBuffer(index)
                if (buf != null && info.size > 0) {
                    buf.position(info.offset)
                    buf.limit(info.offset + info.size)
                    val isConfig = info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0
                    val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                    // One message per access unit; this thread is the camera's alone, so the allocation does not touch the screen stream.
                    val msg = if (isConfig) Protocol.message(Protocol.CAM_CONFIG, info.size) { it.put(buf) }
                    else Protocol.message(Protocol.CAM_FRAME, 9 + info.size) {
                        it.putLong(info.presentationTimeUs)
                        it.put((if (key) 1 else 0).toByte())
                        it.put(buf)
                    }
                    send(msg)
                }
                codec.releaseOutputBuffer(index, false)
            }
        } catch (e: IOException) {
            Log.w(TAG, "camera send ended: $e")
        } catch (e: IllegalStateException) {
            // the encoder was stopped under us: that is the normal way this loop ends
        }
    }

    private fun requestKeyframe() {
        try {
            encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        } catch (_: IllegalStateException) {
        }
    }

    private fun stopCapture() {
        val wasOn = camera != null || encoder != null
        try { session?.close() } catch (_: Exception) {}
        session = null
        try { camera?.close() } catch (_: Exception) {}
        camera = null
        val codec = encoder
        encoder = null
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        try { inputSurface?.release() } catch (_: Exception) {}
        inputSurface = null
        encoderThread = null
        if (wasOn) listener.onCameraState(false)
    }
}
