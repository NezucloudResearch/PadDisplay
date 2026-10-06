// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.wifi.WifiManager
import android.graphics.Color
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
        private const val SCANCODE_F24 = 0x76
    }

    private lateinit var root: FrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var stats: TextView
    private lateinit var input: InputCapture
    private lateinit var gamepads: Gamepads
    private var deviceInput = true    // forward the keyboard, mouse and controllers connected to the tablet
    private var mouseCapture = false  // the mouse sends raw movement (games) instead of pointing at the picture
    private var decoder: VideoDecoder? = null
    private var connection: Connection? = null
    private var audio: AudioStream? = null
    private var showStats = false
    private var wifiLock: WifiManager.WifiLock? = null
    private var refreshHz = 60

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

        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        surfaceView = SurfaceView(this)
        root.addView(surfaceView, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER))
        stats = TextView(this).apply {
            setTextColor(Color.WHITE)
            setBackgroundColor(0x88000000.toInt())
            textSize = 12f
            setPadding(12, 6, 12, 6)
            text = "Connecting…"
        }
        root.addView(stats, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START))
        setContentView(root)
        hideSystemUi()

        input = InputCapture(surfaceView) { msg -> connection?.send(msg) }
        input.mouseMode = prefs.getBoolean("mouseMode", false)
        surfaceView.setOnTouchListener { _, ev -> input.onTouch(ev) }
        surfaceView.setOnHoverListener { _, ev -> input.onHover(ev) }
        surfaceView.holder.addCallback(this)

        deviceInput = prefs.getBoolean("deviceInput", true)
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
        }
    }

    // ---- keyboard, mouse and game controllers connected to the tablet ---------------------

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

    /** true = the key went to the PC (or was used here), so Android must not act on it. */
    private fun forwardKey(ev: KeyEvent): Boolean {
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
        return true
    }

    private fun sendKey(code: Int, down: Boolean) {
        connection?.send(Protocol.message(Protocol.KEY, 3) {
            it.put((if (down) Protocol.KEY_DOWN else 0).toByte())
            it.putShort(code.toShort())
        })
    }

    // ---- surface lifecycle drives the connection ------------------------------------------

    override fun surfaceCreated(holder: SurfaceHolder) {
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
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                holder.surface.setFrameRate(refreshHz.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
            } catch (e: Exception) {
                Log.w("PadDisplay", "setFrameRate: $e")
            }
        }
        val prefs = getSharedPreferences("settings", MODE_PRIVATE)
        var flags = 0
        if (prefs.getBoolean("gamingMode", false)) flags = flags or Protocol.HELLO_GAMING_MODE
        if (prefs.getBoolean("onlyScreen", false)) flags = flags or Protocol.HELLO_ONLY_SCREEN
        val hello = Protocol.clientHello(w, h, metrics.densityDpi, mask, pin, refreshHz, flags, "${Build.MANUFACTURER} ${Build.MODEL}")
        gotFirstFrame = false
        if (host != "127.0.0.1" && host != "localhost") acquireWifiLock()
        connection = Connection(host, port, this).also { it.start(hello) }
        if (prefs.getBoolean("audio", false)) {
            audio = AudioStream(host, port, pin, prefs.getBoolean("audioMutePc", true)).also { it.start() }
        }
        root.post(statsTick)
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
        wifiLock?.release()
        wifiLock = null
        root.removeCallbacks(statsTick)
        audio?.close()
        audio = null
        connection?.close()
        connection = null
        decoder?.release()
        decoder = null
    }

    override fun onDestroy() {
        input.release()
        gamepads.release()
        super.onDestroy()
    }

    // ---- Connection.Listener (reader thread) -----------------------------------------------

    override fun onHostHello(codec: Int, width: Int, height: Int, fps: Int, bitrateKbps: Int) {
        videoW = width
        videoH = height
        hostKbps = bitrateKbps
        codecName = if (codec == Protocol.CODEC_HEVC) "HEVC" else "H.264"
        runOnUiThread {
            fitSurface()
            if (deviceInput) gamepads.announce()
        }
        try {
            decoder?.configure(codec, width, height, fps)
        } catch (e: Exception) {
            fail("Decoder error: ${e.message}")
        }
    }

    override fun onConfig(data: ByteArray, length: Int) {
        decoder?.queueConfig(data, length)
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
