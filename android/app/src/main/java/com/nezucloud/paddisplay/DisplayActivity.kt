// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.wifi.WifiManager
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.util.DisplayMetrics
import android.view.Gravity
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.PointerIcon
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewConfiguration
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import kotlin.math.roundToInt

/** Full-screen view of the PC's virtual monitor. */
class DisplayActivity : Activity(), Connection.Listener, SurfaceHolder.Callback {

    companion object {
        const val EXTRA_HOST = "host"
        const val EXTRA_PORT = "port"
        const val EXTRA_PIN = "pin"
        const val EXTRA_ERROR = "error"
        const val RESULT_BAD_PIN = 2
        const val RESULT_FAILED = 3
        const val RESULT_FINGERPRINT_CHANGED = 4 // EXTRA_ERROR carries the fingerprint the PC showed this time
        private const val SCANCODE_F24 = 0x76
    }

    private lateinit var root: FrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var stats: TextView
    private lateinit var input: InputCapture
    private lateinit var gamepads: Gamepads
    private var deviceInput = true    // forward the keyboard, mouse and controllers connected to the tablet
    private var mouseCapture = false  // the mouse sends raw movement (games) instead of pointing at the picture
    private var grabShortcuts = true  // also take the keys Android reserves for itself (needs ShortcutService)
    private val pressed = BooleanArray(0x200) // PC scancodes sent as down and not yet up, lifted if focus is lost mid-chord
    private var repeatCode = 0
    private var hostReady = false     // the PC has sent its hello: the connection is live
    @Volatile private var decoder: VideoDecoder? = null // null while another app is in front: frames are acked and dropped
    private var connection: Connection? = null
    private var audio: AudioStream? = null
    private var mic: MicStream? = null
    private var cam: CameraStream? = null
    private var hostTrust: HostTrust? = null       // shared by the video, audio, microphone and camera connections
    private var mediaStarted = false
    private var micOn = false
    private var camOn = false
    private lateinit var mediaBadge: TextView
    private var showStats = false
    private var wifiLock: WifiManager.WifiLock? = null
    private var refreshHz = 60
    private var keepInBackground = true // the session outlives the picture while another app is in front (SessionService)
    private var serviceStarted = false
    // What the decoder was started with, to start a new one on the new surface when the picture comes back. The PC sends
    // its parameter sets once per stream, not with every keyframe. Taken only on hello, parameter sets and surface changes.
    private val streamLock = Any()
    private var streamCodec = 0
    private var streamFps = 0
    private var streamConfig: ByteArray? = null

    @Volatile private var videoW = 0
    @Volatile private var videoH = 0
    @Volatile private var codecName = ""
    @Volatile private var rttUs = 0
    @Volatile private var hostKbps = 0
    @Volatile private var bytesReceived = 0L
    @Volatile private var lastKeyframeReq = 0L
    private var lastStatsMs = 0L
    private var receivedAtLastStats = 0L
    private var gotFirstFrame = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val prefs = getSharedPreferences("settings", MODE_PRIVATE)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // Ask the panel for the chosen refresh rate (0 = highest it supports, e.g. 120 Hz).
        val mode = DisplayModes.pick(this, prefs.getInt("refreshRate", 0))
        refreshHz = mode?.refreshRate?.roundToInt() ?: 60
        window.attributes = window.attributes.apply {
            layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            if (mode != null) preferredDisplayModeId = mode.modeId
        }
        showStats = prefs.getBoolean("stats", false)
        keepInBackground = prefs.getBoolean("background", true)

        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        surfaceView = SurfaceView(this)
        root.addView(surfaceView, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER))
        val ui = Ui(this)
        stats = TextView(this).apply {
            setTextColor(Color.WHITE)
            background = ui.pill(0xB0101418.toInt())
            textSize = 12f
            typeface = Typeface.MONOSPACE
            setPadding(ui.dp(12), ui.dp(6), ui.dp(12), ui.dp(6))
            text = "Connecting…"
        }
        root.addView(stats, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START)
            .apply { setMargins(ui.dp(12), ui.dp(12), ui.dp(12), ui.dp(12)) })
        // Shown whenever this tablet's microphone or camera is actually capturing, so it is never silent.
        mediaBadge = TextView(this).apply {
            setTextColor(Color.WHITE)
            background = ui.pill(0xE0B3261E.toInt())
            textSize = 13f
            setTypeface(typeface, Typeface.BOLD)
            setPadding(ui.dp(14), ui.dp(6), ui.dp(14), ui.dp(6))
            visibility = View.GONE
        }
        root.addView(mediaBadge, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.END)
            .apply { setMargins(ui.dp(12), ui.dp(12), ui.dp(12), ui.dp(12)) })
        setContentView(root)
        hideSystemUi()

        input = InputCapture(surfaceView) { msg -> connection?.send(msg) }
        input.mouseMode = prefs.getBoolean("mouseMode", false)
        surfaceView.setOnTouchListener { _, ev -> input.onTouch(ev) }
        surfaceView.setOnHoverListener { _, ev -> input.onHover(ev) }
        surfaceView.holder.addCallback(this)

        deviceInput = prefs.getBoolean("deviceInput", true)
        grabShortcuts = prefs.getBoolean("grabShortcuts", true)
        mouseCapture = deviceInput && prefs.getBoolean("mouseCapture", false)
        input.deviceInput = deviceInput
        gamepads = Gamepads(this) { msg -> connection?.send(msg) }
        if (deviceInput) {
            surfaceView.setOnGenericMotionListener { _, ev -> input.onGenericMotion(ev) }
            surfaceView.setOnCapturedPointerListener { _, ev -> input.onCapturedPointer(ev) }
            // The picture already shows the PC's pointer.
            surfaceView.pointerIcon = PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL)
            // Pointer capture is given to the focused view.
            surfaceView.isFocusable = true
            surfaceView.isFocusableInTouchMode = true
            surfaceView.requestFocus()
        }
    }

    private fun hideSystemUi() {
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideSystemUi()
            applyMouseCapture() // Android drops the capture whenever the window loses focus
            attachKeys()
        } else {
            // Another window has the keyboard now: stop asking for keys, and let go of the ones the PC thinks are held.
            KeyRouter.detach()
            releaseAllKeys()
        }
    }

    // ---- keyboard, mouse and game controllers connected to the tablet ---------------------

    /** Ask for the keys Android reserves for itself, only while the picture has focus and the PC has answered. */
    private fun attachKeys() {
        if (hostReady && deviceInput && grabShortcuts && hasWindowFocus()) KeyRouter.attach { ev -> forwardKey(ev, true) }
    }

    private fun applyMouseCapture() {
        if (mouseCapture) surfaceView.requestPointerCapture() else surfaceView.releasePointerCapture()
    }

    private fun toggleMouseCapture() {
        mouseCapture = !mouseCapture
        input.releaseMouseButtons()
        applyMouseCapture()
        Toast.makeText(this, if (mouseCapture) "Mouse captured for games. Ctrl+Alt+Shift+M releases it." else "Mouse released.",
            Toast.LENGTH_SHORT).show()
    }

    override fun dispatchKeyEvent(ev: KeyEvent): Boolean = (deviceInput && forwardKey(ev)) || super.dispatchKeyEvent(ev)

    override fun dispatchGenericMotionEvent(ev: MotionEvent): Boolean =
        (deviceInput && gamepads.onMotion(ev)) || super.dispatchGenericMotionEvent(ev)

    /**
     * true = the key went to the PC (or was used here), so Android must not act on it.
     * [fromService]: the key came through ShortcutService, before Android's own handling. Android then
     * generates no auto-repeat for it, so it is made here (the PC does not repeat injected keys itself).
     */
    private fun forwardKey(ev: KeyEvent, fromService: Boolean = false): Boolean {
        if (gamepads.onKey(ev)) return true
        // A mouse's side buttons also arrive as Back/Forward keys. They are sent as mouse buttons.
        if (ev.isFromSource(InputDevice.SOURCE_MOUSE) || ev.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE))
            return ev.keyCode == KeyEvent.KEYCODE_BACK || ev.keyCode == KeyEvent.KEYCODE_FORWARD
        val down = ev.action == KeyEvent.ACTION_DOWN
        if (!down && ev.action != KeyEvent.ACTION_UP) return false
        // The tablet's own volume buttons stay with the tablet; a keyboard's go to the PC.
        val volume = ev.keyCode == KeyEvent.KEYCODE_VOLUME_UP || ev.keyCode == KeyEvent.KEYCODE_VOLUME_DOWN ||
            ev.keyCode == KeyEvent.KEYCODE_VOLUME_MUTE
        if (volume && ev.device?.isExternal != true) return false
        val code = Keyboard.scancode(ev)
        if (code == 0) return false
        if (ev.keyCode == KeyEvent.KEYCODE_M && ev.isCtrlPressed && ev.isAltPressed && ev.isShiftPressed) {
            if (down && ev.repeatCount == 0) {
                toggleMouseCapture()
                // The PC got Ctrl, Alt and Shift but not the M. Released with nothing in between, such
                // a chord is how Windows switches the input language: put an unused key in between.
                sendKey(SCANCODE_F24, true)
                sendKey(SCANCODE_F24, false)
            }
            return true
        }
        sendKey(code, down) // repeats too: Windows does not repeat an injected key by itself
        if (fromService) {
            if (down && code !in noRepeat) {
                repeatCode = code
                root.removeCallbacks(repeater)
                root.postDelayed(repeater, ViewConfiguration.getKeyRepeatTimeout().toLong())
            } else if (!down && code == repeatCode) {
                stopRepeat()
            }
        }
        return true
    }

    // Held modifiers and lock keys are not repeated, as on a real keyboard.
    private val noRepeat = intArrayOf(0x1D, 0x2A, 0x36, 0x38, 0x3A, 0x45, 0x46, Protocol.KEY_EXTENDED or 0x1D,
        Protocol.KEY_EXTENDED or 0x38, Protocol.KEY_EXTENDED or 0x5B, Protocol.KEY_EXTENDED or 0x5C, Protocol.KEY_PAUSE)

    private val repeater = object : Runnable {
        override fun run() {
            if (repeatCode == 0) return
            sendKey(repeatCode, true)
            root.postDelayed(this, ViewConfiguration.getKeyRepeatDelay().toLong())
        }
    }

    private fun stopRepeat() {
        repeatCode = 0
        root.removeCallbacks(repeater)
    }

    private fun releaseAllKeys() {
        stopRepeat()
        for (c in pressed.indices) if (pressed[c]) sendKey(c, false)
    }

    private fun sendKey(code: Int, down: Boolean) {
        if (code in pressed.indices) pressed[code] = down
        connection?.send(Protocol.message(Protocol.KEY, 3) {
            it.put((if (down) Protocol.KEY_DOWN else 0).toByte())
            it.putShort(code.toShort())
        })
    }

    // ---- surface lifecycle drives the connection ------------------------------------------

    override fun surfaceCreated(holder: SurfaceHolder) {
        setFrameRate(holder)
        root.removeCallbacks(statsTick)
        root.post(statsTick)
        if (connection != null) {
            // Back from the background: the session kept running without a picture, so only the decoder is new.
            attachDecoder(holder.surface)
            return
        }
        decoder = VideoDecoder(holder.surface)
        val host = intent.getStringExtra(EXTRA_HOST) ?: "127.0.0.1"
        val port = intent.getIntExtra(EXTRA_PORT, Protocol.TCP_PORT)
        val pin = intent.getIntExtra(EXTRA_PIN, 0)
        val metrics = DisplayMetrics()
        @Suppress("DEPRECATION")
        windowManager.defaultDisplay.getRealMetrics(metrics)
        val w = maxOf(metrics.widthPixels, metrics.heightPixels)
        val h = minOf(metrics.widthPixels, metrics.heightPixels)
        var mask = 0
        if (VideoDecoder.hasHardwareDecoder(VideoDecoder.mimeFor(Protocol.CODEC_H264))) mask = mask or Protocol.MASK_H264
        if (VideoDecoder.hasHardwareDecoder(VideoDecoder.mimeFor(Protocol.CODEC_HEVC))) mask = mask or Protocol.MASK_HEVC
        if (mask == 0) mask = Protocol.MASK_H264
        val prefs = getSharedPreferences("settings", MODE_PRIVATE)
        var flags = 0
        if (prefs.getBoolean("gamingMode", false)) flags = flags or Protocol.HELLO_GAMING_MODE
        if (prefs.getBoolean("onlyScreen", false)) flags = flags or Protocol.HELLO_ONLY_SCREEN
        val hello = Protocol.clientHello(w, h, metrics.densityDpi, mask, pin, refreshHz, flags, "${Build.MANUFACTURER} ${Build.MODEL}")
        gotFirstFrame = false
        if (!Tls.isLoopback(host)) acquireWifiLock()
        // Everything but USB (the loopback tunnel never touches the network) is encrypted and pinned to the PC's certificate.
        val trust = if (Tls.isLoopback(host)) null else HostTrust(prefs, host)
        hostTrust = trust
        mediaStarted = false
        connection = Connection(host, port, trust, this).also { it.start(hello) }
        if (prefs.getBoolean("audio", false)) {
            audio = AudioStream(host, port, pin, prefs.getBoolean("audioMutePc", true), trust).also { it.start() }
        }
        if (keepInBackground) {
            val granted = { permission: String -> checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED }
            SessionService.onDisconnect = { finish() }
            serviceStarted = true
            SessionService.start(this, host, prefs.getBoolean("micToPc", false) && granted(Manifest.permission.RECORD_AUDIO),
                prefs.getBoolean("camToPc", false) && granted(Manifest.permission.CAMERA))
        }
    }

    private fun setFrameRate(holder: SurfaceHolder) {
        if (Build.VERSION.SDK_INT < 30) return
        try {
            holder.surface.setFrameRate(refreshHz.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
        } catch (e: Exception) {
            Log.w("PadDisplay", "setFrameRate: $e")
        }
    }

    /**
     * A new decoder on the new surface, set up as the last one was. Frames up to the next keyframe cannot be decoded
     * (the PC never sends one by itself), so one is asked for right away. Runs on the UI thread once per return
     * from the background; the reader thread keeps acking frames meanwhile and only waits if a hello comes in now.
     */
    private fun attachDecoder(surface: Surface) {
        val d = VideoDecoder(surface)
        try {
            synchronized(streamLock) {
                if (streamCodec != 0) {
                    d.configure(streamCodec, videoW, videoH, streamFps)
                    streamConfig?.let { d.queueConfig(it, it.size) }
                }
                decoder = d
            }
        } catch (e: Exception) {
            d.release()
            fail("Decoder error: ${e.message}")
            return
        }
        lastKeyframeReq = 0L
        requestKeyframe()
    }

    /** The surface is going away: the codec must let go of it before surfaceDestroyed returns. */
    private fun detachDecoder() {
        synchronized(streamLock) {
            val d = decoder
            decoder = null // the reader thread now acks and drops frames
            d?.release()
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

    /**
     * Wi-Fi power save batches packets (~100 ms beacons). Modes: 0 = system default,
     * 1 = high performance (power save off), 2 = low latency (can stall traffic for seconds while
     * the driver switches on some Huawei ROMs).
     */
    private fun acquireWifiLock() {
        if (wifiLock != null) return
        val mode = when (getSharedPreferences("settings", MODE_PRIVATE).getInt("wifiMode", 1)) {
            1 -> @Suppress("DEPRECATION") WifiManager.WIFI_MODE_FULL_HIGH_PERF
            2 -> WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            else -> return
        }
        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock = wifi.createWifiLock(mode, "paddisplay").apply {
            setReferenceCounted(false)
            acquire()
        }
        Log.i("PadDisplay", "wifi lock mode $mode acquired")
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        root.removeCallbacks(statsTick)
        if (keepInBackground && !isFinishing && connection != null) {
            // Another app is in front, or the screen went off: only the picture stops. Audio, microphone, camera
            // and the PC's virtual monitor stay as they are.
            input.releaseMouseButtons()
            detachDecoder()
            return
        }
        closeSession()
    }

    private fun closeSession() {
        hostReady = false
        KeyRouter.detach()
        releaseAllKeys()
        wifiLock?.release()
        wifiLock = null
        if (serviceStarted) {
            serviceStarted = false
            SessionService.onDisconnect = null
            SessionService.stop(this)
        }
        // The picture's connection first: the PC puts its own screens back when that one ends. Each on its own, because
        // one that throws must not leave the others open: the microphone and the camera would go on sending.
        val closing = listOf<() -> Unit>({ connection?.close() }, { audio?.close() }, { mic?.close() }, { cam?.close() }, { decoder?.release() })
        for (close in closing) {
            try { close() } catch (e: Exception) { Log.e("PadDisplay", "closing the session", e) }
        }
        connection = null
        audio = null
        mic = null
        cam = null
        decoder = null
    }

    override fun onDestroy() {
        closeSession() // ended while in the background, where the surface was already gone
        KeyRouter.detach()
        input.release()
        gamepads.release()
        super.onDestroy()
    }

    // ---- Connection.Listener (reader thread) -----------------------------------------------

    // The microphone and camera connections open only once the PC has accepted the PIN on the video connection, so a wrong
    // PIN is counted once and not once per connection, and only if the user switched them on and Android allows it.
    private fun startMediaStreams() {
        if (mediaStarted) return
        mediaStarted = true
        val host = intent.getStringExtra(EXTRA_HOST) ?: return
        val port = intent.getIntExtra(EXTRA_PORT, Protocol.TCP_PORT)
        val pin = intent.getIntExtra(EXTRA_PIN, 0)
        val prefs = getSharedPreferences("settings", MODE_PRIVATE)
        val granted = { permission: String -> checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED }
        runOnUiThread {
            if (isFinishing || connection == null) return@runOnUiThread
            if (prefs.getBoolean("micToPc", false)) {
                if (granted(Manifest.permission.RECORD_AUDIO)) {
                    mic = MicStream(host, port, pin, hostTrust, object : MicStream.Listener {
                        override fun onMicState(active: Boolean) { micOn = active; runOnUiThread { updateBadge() } }
                        override fun onMicNotice(message: String) { runOnUiThread { Toast.makeText(this@DisplayActivity, "Microphone: $message", Toast.LENGTH_LONG).show() } }
                    }).also { it.start() }
                } else {
                    Toast.makeText(this, "Microphone: permission was taken away in Android's settings", Toast.LENGTH_LONG).show()
                }
            }
            if (prefs.getBoolean("camToPc", false)) {
                if (granted(Manifest.permission.CAMERA)) {
                    cam = CameraStream(applicationContext, host, port, pin, hostTrust, prefs.getInt("camFacing", 1) == 1, prefs.getInt("camQuality", 720) != 1080,
                        object : CameraStream.Listener {
                            override fun onCameraState(streaming: Boolean) { camOn = streaming; runOnUiThread { updateBadge() } }
                            override fun onCameraNotice(message: String) { runOnUiThread { Toast.makeText(this@DisplayActivity, "Camera: $message", Toast.LENGTH_LONG).show() } }
                        }).also { it.start() }
                } else {
                    Toast.makeText(this, "Camera: permission was taken away in Android's settings", Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    private fun updateBadge() {
        val parts = listOfNotNull(if (micOn) "Microphone on" else null, if (camOn) "Camera on" else null)
        mediaBadge.text = "● " + parts.joinToString("  ·  ")
        mediaBadge.visibility = if (parts.isEmpty()) View.GONE else View.VISIBLE
        // The label can't be seen from another app; the notification says it there.
        if (keepInBackground && connection != null) SessionService.update(this, intent.getStringExtra(EXTRA_HOST) ?: "", micOn, camOn)
    }

    override fun onHostHello(codec: Int, width: Int, height: Int, fps: Int, bitrateKbps: Int) {
        startMediaStreams()
        videoW = width
        videoH = height
        hostKbps = bitrateKbps
        codecName = if (codec == Protocol.CODEC_HEVC) "HEVC" else "H.264"
        runOnUiThread {
            hostReady = true
            attachKeys()
            fitSurface()
            if (deviceInput) gamepads.announce()
        }
        try {
            synchronized(streamLock) {
                streamCodec = codec
                streamFps = fps
                decoder?.configure(codec, width, height, fps)
            }
        } catch (e: Exception) {
            fail("Decoder error: ${e.message}")
        }
    }

    override fun onConfig(data: ByteArray, length: Int) {
        synchronized(streamLock) {
            streamConfig = data.copyOf(length) // once per stream, not per frame
            decoder?.queueConfig(data, length)
        }
    }

    override fun onFrame(ptsUs: Long, keyframe: Boolean, data: ByteArray, offset: Int, length: Int) {
        bytesReceived += length
        val ok = try {
            decoder?.queueFrame(data, offset, length, ptsUs, keyframe) ?: true
        } catch (e: IllegalStateException) {
            false
        }
        if (!ok) requestKeyframe()
        if (!gotFirstFrame && ok) {
            gotFirstFrame = true
            runOnUiThread { if (!showStats) stats.visibility = View.GONE }
        }
    }

    override fun onPing(rttUs: Int, bitrateKbps: Int) {
        this.rttUs = rttUs
        hostKbps = bitrateKbps
    }

    override fun onFingerprintChanged(fingerprint: String) {
        runOnUiThread {
            setResult(RESULT_FINGERPRINT_CHANGED, Intent().putExtra(EXTRA_ERROR, fingerprint))
            finish()
        }
    }

    override fun onError(code: Int, message: String) {
        runOnUiThread {
            setResult(if (code == Protocol.ERR_BAD_PIN) RESULT_BAD_PIN else RESULT_FAILED, Intent().putExtra(EXTRA_ERROR, message))
            finish()
        }
    }

    override fun onRumble(index: Int, large: Int, small: Int) {
        runOnUiThread { gamepads.onRumble(index, large, small) }
    }

    override fun onNotice(message: String) {
        runOnUiThread { Toast.makeText(this, "PadDisplay: $message", Toast.LENGTH_LONG).show() }
    }

    override fun onClosed(reason: String?) {
        if (reason != null) fail(reason)
    }

    private fun fail(reason: String) {
        runOnUiThread {
            if (isFinishing) return@runOnUiThread
            setResult(RESULT_FAILED, Intent().putExtra(EXTRA_ERROR, reason))
            Toast.makeText(this, "PadDisplay: $reason", Toast.LENGTH_LONG).show()
            finish()
        }
    }

    private fun requestKeyframe() {
        val now = SystemClock.uptimeMillis()
        if (now - lastKeyframeReq < 300) return
        lastKeyframeReq = now
        connection?.send(Protocol.message(Protocol.KEYFRAME_REQ, 0))
    }

    /** Letterbox the video into the screen so touch coordinates map 1:1 onto the picture. */
    private fun fitSurface() {
        if (videoW == 0 || videoH == 0) return
        val sw = root.width
        val sh = root.height
        if (sw == 0 || sh == 0) {
            root.post { fitSurface() }
            return
        }
        val scale = minOf(sw.toFloat() / videoW, sh.toFloat() / videoH)
        val lp = surfaceView.layoutParams as FrameLayout.LayoutParams
        lp.width = (videoW * scale).roundToInt()
        lp.height = (videoH * scale).roundToInt()
        lp.gravity = Gravity.CENTER
        surfaceView.layoutParams = lp
    }

    private val statsTick = object : Runnable {
        override fun run() {
            val now = SystemClock.uptimeMillis()
            val d = decoder
            if (d != null && lastStatsMs != 0L) {
                val secs = (now - lastStatsMs) / 1000f
                val fps = d.framesRendered.getAndSet(0) / secs
                val mbps = (bytesReceived - receivedAtLastStats) * 8 / 1e6f / secs
                val n = d.decodeCount.getAndSet(0)
                val decMs = if (n > 0) d.decodeUsTotal.getAndSet(0) / 1000f / n else 0f
                val dropped = d.framesDropped.getAndSet(0)
                @Suppress("DEPRECATION")
                val panelHz = windowManager.defaultDisplay.refreshRate.roundToInt()
                val a = audio
                val line = String.format(
                    "%s %dx%d · %.0f fps (panel %d Hz) · %.1f Mbps (cap %.0f) · RTT %.1f ms · decode %.1f ms%s%s · %s",
                    codecName, videoW, videoH, fps, panelHz, mbps, hostKbps / 1000f, rttUs / 1000f, decMs,
                    if (dropped > 0) " · skipped $dropped" else "",
                    if (a != null) String.format(" · audio buffer %d/%d ms, speed %+.2f%%, underruns %d, dropped %d ms", a.queuedMs, a.targetMs.toInt(), a.speedPercent, a.refills, a.droppedMs) else "", d.decoderName)
                if (gotFirstFrame) Log.i("PadStats", line)
                if (showStats || !gotFirstFrame) {
                    stats.visibility = View.VISIBLE
                    stats.text = if (!gotFirstFrame) "Connecting…" else line
                }
            }
            receivedAtLastStats = bytesReceived
            lastStatsMs = now
            root.postDelayed(this, 1000)
        }
    }
}
