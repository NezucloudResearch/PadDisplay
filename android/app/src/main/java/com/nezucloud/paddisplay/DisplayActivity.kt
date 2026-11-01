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
        const val RESULT_FINGERPRINT_CHANGED = 4 // EXTRA_ERROR carries the fingerprint the PC showed this time
    }

    private lateinit var root: FrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var stats: TextView
    private lateinit var input: InputCapture
    private lateinit var gamepads: Gamepads
    private lateinit var keys: KeyForwarder
    private lateinit var passwordBox: PasswordBox
    private var deviceInput = true    // forward the keyboard, mouse and controllers connected to the tablet
    private var mouseCapture = false  // the mouse sends raw movement (games) instead of pointing at the picture
    private var grabShortcuts = true  // also take the keys Android reserves for itself (needs ShortcutService)
    private var hostReady = false     // the PC has sent its hello: the connection is live
    private var pcFrozen = false      // the PC is locked and we can't show it: offer (and auto-open) the password box
    private lateinit var passwordButton: TextView // shown while frozen: reopens the box to type the PC's sign-in password
    @Volatile
    private var decoder: VideoDecoder? = null // null while another app is in front: frames are acked and dropped
    @Volatile
    private var upscaler: Upscaler? = null    // between the decoder and the screen while the PC renders below the panel size
    private var renderScale = 100             // percent of the panel's resolution the PC is asked to render
    private var sharpUpscale = true           // upscale with GSR; off = the display hardware's plain stretch
    private var connection: Connection? = null
    private var media: SessionMedia? = null
    private var hostTrust: HostTrust? = null       // shared by the video, audio, microphone and camera connections
    // The session outlives one connection: after a lost link it connects again with the same hello, for a while.
    private var sessionActive = false              // connected or reconnecting, with or without a picture
    private var hello: ByteArray? = null
    @Volatile
    private var everConnected = false              // the PC accepted this tablet in this session, so a lost link is worth retrying
    private var reconnectStartedMs = 0L            // 0 = not reconnecting
    private var reconnectAttempts = 0
    private val reconnect = Runnable { connect() }
    @Volatile
    private var mediaStarted = false
    private lateinit var mediaBadge: TextView
    private var showStats = false
    private var wifiLock: WifiManager.WifiLock? = null
    private var refreshHz = 60
    private var keepInBackground =
        true // the session outlives the picture while another app is in front (SessionService)
    private var serviceStarted = false

    // What the decoder was started with, to start a new one on the new surface when the picture comes back. The PC sends
    // its parameter sets once per stream, not with every keyframe. Taken only on hello, parameter sets and surface changes.
    private val streamLock = Any()
    private var streamCodec = 0
    private var streamFps = 0
    private var streamConfig: ByteArray? = null

    @Volatile
    private var videoW = 0
    @Volatile
    private var videoH = 0
    @Volatile
    private var codecName = ""
    @Volatile
    private var rttUs = 0
    @Volatile
    private var hostKbps = 0
    @Volatile
    private var bytesReceived = 0L
    @Volatile
    private var lastKeyframeReq = 0L
    private var lastDecoderRebuildMs = 0L         // reader thread only
    private val decoderRebuilds = ArrayDeque<Long>() // reader thread only: when the last few rebuilds happened
    private var lastStatsMs = 0L
    private var receivedAtLastStats = 0L
    @Volatile
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
        renderScale = prefs.getInt("renderScale", 100)
        sharpUpscale = prefs.getBoolean("sharpUpscale", true)

        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        surfaceView = SurfaceView(this)
        root.addView(
            surfaceView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
                Gravity.CENTER
            )
        )
        addOverlays()
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
        keys = KeyForwarder(root, gamepads, { msg -> connection?.send(msg) }, { passwordBox.isOpen }, { toggleMouseCapture() })
        passwordBox = PasswordBox(this, root) { text -> connection?.send(Protocol.secureText(text, true)) }
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

    private fun addOverlays() {
        val ui = Ui(this)
        stats = TextView(this).apply {
            setTextColor(Color.WHITE)
            background = ui.pill(0xB0101418.toInt())
            textSize = 12f
            typeface = Typeface.MONOSPACE
            setPadding(ui.dp(12), ui.dp(6), ui.dp(12), ui.dp(6))
            text = "Connecting…"
        }
        addOverlay(stats, Gravity.TOP or Gravity.START) { setMargins(ui.dp(12), ui.dp(12), ui.dp(12), ui.dp(12)) }
        // Shown whenever this tablet's microphone or camera is actually capturing, so it is never silent.
        mediaBadge = TextView(this).apply {
            setTextColor(Color.WHITE)
            background = ui.pill(0xE0B3261E.toInt())
            textSize = 13f
            setTypeface(typeface, Typeface.BOLD)
            setPadding(ui.dp(14), ui.dp(6), ui.dp(14), ui.dp(6))
            visibility = View.GONE
        }
        addOverlay(mediaBadge, Gravity.TOP or Gravity.END) { setMargins(ui.dp(12), ui.dp(12), ui.dp(12), ui.dp(12)) }
        // Shown only while the picture is frozen because the PC is locked and the tablet cannot show that
        // screen (the host sends LockState); tapping it opens the box to type the PC's sign-in password.
        passwordButton = ui.text("🔑  Type PC password", 15f, R.color.pd_on_accent, bold = true).apply {
            background = ui.ripple(ui.pill(ui.color(R.color.pd_accent)), 14)
            setPadding(ui.dp(22), ui.dp(13), ui.dp(22), ui.dp(13))
            visibility = View.GONE
            setOnClickListener { passwordBox.show() }
        }
        addOverlay(passwordButton, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL) { setMargins(0, 0, 0, ui.dp(28)) }
    }

    private fun addOverlay(view: View, gravity: Int, margins: FrameLayout.LayoutParams.() -> Unit) {
        val wrap = FrameLayout.LayoutParams.WRAP_CONTENT
        root.addView(view, FrameLayout.LayoutParams(wrap, wrap, gravity).apply(margins))
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
            openPasswordDialogIfFrozen() // came back to the app while the PC is still locked: offer the box again
        } else {
            // Another window has the keyboard now: stop asking for keys, and let go of the ones the PC thinks are held.
            KeyRouter.detach()
            keys.releaseAll()
        }
    }

    // ---- keyboard, mouse and game controllers connected to the tablet ---------------------

    /** Ask for the keys Android reserves for itself, only while the picture has focus and the PC has answered. */
    private fun attachKeys() {
        if (hostReady && deviceInput && grabShortcuts && hasWindowFocus()) KeyRouter.attach { ev -> keys.forward(ev, true) }
    }

    private fun applyMouseCapture() {
        if (mouseCapture) surfaceView.requestPointerCapture() else surfaceView.releasePointerCapture()
    }

    private fun toggleMouseCapture() {
        mouseCapture = !mouseCapture
        input.releaseMouseButtons()
        applyMouseCapture()
        Toast.makeText(
            this, if (mouseCapture) "Mouse captured for games. Ctrl+Alt+Shift+M releases it." else "Mouse released.",
            Toast.LENGTH_SHORT
        ).show()
    }

    override fun dispatchKeyEvent(ev: KeyEvent): Boolean = (deviceInput && keys.forward(ev)) || super.dispatchKeyEvent(ev)

    override fun dispatchGenericMotionEvent(ev: MotionEvent): Boolean =
        (deviceInput && gamepads.onMotion(ev)) || super.dispatchGenericMotionEvent(ev)

    // ---- surface lifecycle drives the connection ------------------------------------------

    override fun surfaceCreated(holder: SurfaceHolder) {
        setFrameRate(holder)
        root.removeCallbacks(statsTick)
        root.post(statsTick)
        if (sessionActive) {
            // Back from the background: the session kept running without a picture, so only the decoder is new.
            attachDecoder(decoderSurface(holder.surface))
            return
        }
        decoder = VideoDecoder(decoderSurface(holder.surface))
        startSession()
    }

    private fun startSession() {
        val host = intent.getStringExtra(EXTRA_HOST) ?: "127.0.0.1"
        val port = intent.getIntExtra(EXTRA_PORT, Protocol.TCP_PORT)
        val pin = intent.getIntExtra(EXTRA_PIN, 0)
        val prefs = getSharedPreferences("settings", MODE_PRIVATE)
        hello = makeHello(pin)
        gotFirstFrame = false
        if (!Tls.isLoopback(host)) acquireWifiLock()
        // Everything but USB (the loopback tunnel never touches the network) is encrypted and pinned to the PC's certificate.
        val trust = if (Tls.isLoopback(host)) null else HostTrust(prefs, host)
        hostTrust = trust
        mediaStarted = false
        media = SessionMedia(this, host, port, pin, trust, prefs) { updateBadge() }
        sessionActive = true
        connect()
        media?.startAudio()
        if (keepInBackground) {
            val granted = { permission: String -> checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED }
            SessionService.onDisconnect = { finish() }
            serviceStarted = true
            SessionService.start(
                this, host, prefs.getBoolean("micToPc", false) && granted(Manifest.permission.RECORD_AUDIO),
                prefs.getBoolean("camToPc", false) && granted(Manifest.permission.CAMERA)
            )
        }
    }

    private fun makeHello(pin: Int): ByteArray {
        val metrics = DisplayMetrics()
        @Suppress("DEPRECATION")
        windowManager.defaultDisplay.getRealMetrics(metrics)
        // The PC renders, encodes and sends this size; below the panel's, this tablet upscales it.
        val w = scaledToRender(maxOf(metrics.widthPixels, metrics.heightPixels))
        val h = scaledToRender(minOf(metrics.widthPixels, metrics.heightPixels))
        val prefs = getSharedPreferences("settings", MODE_PRIVATE)
        var flags = Protocol.HELLO_RECONNECTS
        if (prefs.getBoolean("gamingMode", false)) flags = flags or Protocol.HELLO_GAMING_MODE
        if (prefs.getBoolean("onlyScreen", false)) flags = flags or Protocol.HELLO_ONLY_SCREEN
        return Protocol.clientHello(w, h, metrics.densityDpi, supportedCodecs(), pin, refreshHz, flags, "${Build.MANUFACTURER} ${Build.MODEL}")
    }

    private fun supportedCodecs(): Int {
        var mask = 0
        if (VideoDecoder.hasHardwareDecoder(VideoDecoder.mimeFor(Protocol.CODEC_H264))) mask = mask or Protocol.MASK_H264
        if (VideoDecoder.hasHardwareDecoder(VideoDecoder.mimeFor(Protocol.CODEC_HEVC))) mask = mask or Protocol.MASK_HEVC
        return if (mask == 0) Protocol.MASK_H264 else mask
    }

    private fun connect() {
        val h = hello ?: return
        val host = intent.getStringExtra(EXTRA_HOST) ?: "127.0.0.1"
        val port = intent.getIntExtra(EXTRA_PORT, Protocol.TCP_PORT)
        connection = Connection(host, port, hostTrust, this).also { it.start(h) }
    }

    /** [size] at [renderScale], rounded to a multiple of 8, which the PC's encoders and the NV12 format handle best. */
    private fun scaledToRender(size: Int) = if (renderScale >= 100) size else (size * renderScale / 100 + 4) / 8 * 8

    /**
     * The surface the decoder draws into: the upscaler's while the PC renders below the panel's size and sharp
     * upscaling is on, else the screen's own (the display hardware then stretches the picture, with one buffer hop
     * less). If the GPU cannot run the upscaler, the picture is shown directly rather than not at all.
     */
    private fun decoderSurface(screen: Surface): Surface {
        if (renderScale >= 100 || !sharpUpscale) return screen
        val u = try {
            Upscaler(this, screen)
        } catch (e: Exception) {
            Log.w("PadDisplay", "upscaler unavailable, plain stretch instead: $e")
            return screen
        }
        if (videoW > 0) u.setVideoSize(videoW, videoH)
        upscaler = u
        return u.inputSurface
    }

    /** After the decoder: it must stop drawing into the upscaler's surface first. */
    private fun releaseUpscaler() {
        upscaler?.release()
        upscaler = null
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
            releaseUpscaler()
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        upscaler?.setViewSize(width, height)
    }

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
        if (keepInBackground && !isFinishing && sessionActive) {
            // Another app is in front, or the screen went off: only the picture stops. Audio, microphone, camera
            // and the PC's virtual monitor stay as they are.
            input.releaseMouseButtons()
            detachDecoder()
            return
        }
        closeSession()
    }

    private fun closeSession() {
        sessionActive = false
        root.removeCallbacks(reconnect)
        reconnectStartedMs = 0L // a later session starts with its own minute of retries
        everConnected = false
        hostReady = false
        KeyRouter.detach()
        keys.releaseAll()
        wifiLock?.release()
        wifiLock = null
        if (serviceStarted) {
            serviceStarted = false
            SessionService.onDisconnect = null
            SessionService.stop(this)
        }
        // The picture's connection first: the PC puts its own screens back when that one ends.
        closeEach({ connection?.close() })
        connection = null
        stopMediaStreams()
        closeEach({ decoder?.release() }, { releaseUpscaler() })
        decoder = null
    }

    private fun stopMediaStreams() {
        media?.stop()
        mediaStarted = false
        updateBadge()
    }

    /**
     * The link to the PC broke after the PC had accepted this tablet (Wi-Fi dropped, the cable was pulled, the PC restarted
     * PadDisplay): connect again for a while instead of ending. The last picture stays up meanwhile. The PC keeps the virtual
     * monitor, and the windows on it, waiting for this tablet. UI thread.
     */
    private fun reconnectLater(reason: String) {
        connection = null
        val now = SystemClock.uptimeMillis()
        if (reconnectStartedMs == 0L) {
            Log.i("PadDisplay", "connection lost ($reason): reconnecting")
            reconnectStartedMs = now
            reconnectAttempts = 0
            hostReady = false
            KeyRouter.detach()
            keys.releaseAll() // the PC let go of them itself when the session ended
            input.releaseMouseButtons()
            stopMediaStreams() // their connections went down with the link; they start again with the new session
            gotFirstFrame = false
        }
        if (now - reconnectStartedMs > 60_000) {
            fail("Lost the connection to the PC: $reason")
            return
        }
        stats.visibility = View.VISIBLE
        stats.text = "Connection lost. Reconnecting…"
        root.postDelayed(reconnect, longArrayOf(500, 1000, 2000)[minOf(reconnectAttempts++, 2)])
    }

    override fun onDestroy() {
        passwordBox.dismiss() // don't leak the window
        closeSession() // ended while in the background, where the surface was already gone
        KeyRouter.detach()
        input.release()
        gamepads.release()
        super.onDestroy()
    }

    // ---- Connection.Listener (reader thread) -----------------------------------------------

    private fun startMediaStreams() {
        if (mediaStarted) return
        mediaStarted = true
        runOnUiThread {
            if (!isFinishing && sessionActive) media?.startCapture()
        }
    }

    private fun updateBadge() {
        val micOn = media?.micOn == true
        val camOn = media?.camOn == true
        val parts = listOfNotNull(if (micOn) "Microphone on" else null, if (camOn) "Camera on" else null)
        mediaBadge.text = "● " + parts.joinToString("  ·  ")
        mediaBadge.visibility = if (parts.isEmpty()) View.GONE else View.VISIBLE
        // The label can't be seen from another app; the notification says it there.
        if (keepInBackground && sessionActive) SessionService.update(
            this,
            intent.getStringExtra(EXTRA_HOST) ?: "",
            micOn,
            camOn
        )
    }

    override fun onHostHello(version: Int, codec: Int, width: Int, height: Int, fps: Int, bitrateKbps: Int) {
        everConnected = true
        startMediaStreams()
        videoW = width
        videoH = height
        hostKbps = bitrateKbps
        codecName = if (codec == Protocol.CODEC_HEVC) "HEVC" else "H.264"
        runOnUiThread {
            reconnectStartedMs = 0L // connected (again)
            hostReady = true
            attachKeys()
            fitSurface()
            if (deviceInput) gamepads.announce()
        }
        try {
            synchronized(streamLock) {
                streamCodec = codec
                streamFps = fps
                upscaler?.setVideoSize(width, height)
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
        decoder?.let { if (it.broken) rebuildDecoder(it) }
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

    /**
     * A codec that failed for good never shows another picture, so it is replaced by a new one set up as the last
     * (reader thread, only after a failure). A decoder that keeps failing is a dead end: the session ends then.
     */
    private fun rebuildDecoder(broken: VideoDecoder) {
        val now = SystemClock.uptimeMillis()
        if (now - lastDecoderRebuildMs < 1000) return
        lastDecoderRebuildMs = now
        while (decoderRebuilds.isNotEmpty() && now - decoderRebuilds.first() > 10_000) decoderRebuilds.removeFirst()
        if (decoderRebuilds.size >= 5) {
            fail("Decoder error: this tablet's video decoder keeps failing.")
            return
        }
        decoderRebuilds.addLast(now)
        try {
            synchronized(streamLock) {
                if (decoder !== broken || streamCodec == 0) return
                broken.configure(streamCodec, videoW, videoH, streamFps)
                streamConfig?.let { broken.queueConfig(it, it.size) }
            }
        } catch (e: Exception) {
            fail("Decoder error: ${e.message}")
            return
        }
        Log.w("PadDisplay", "decoder failed and was started again")
        lastKeyframeReq = 0L
        requestKeyframe()
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
            setResult(
                if (code == Protocol.ERR_BAD_PIN) RESULT_BAD_PIN else RESULT_FAILED,
                Intent().putExtra(EXTRA_ERROR, message)
            )
            finish()
        }
    }

    override fun onRumble(index: Int, large: Int, small: Int) {
        runOnUiThread { gamepads.onRumble(index, large, small) }
    }

    override fun onNotice(message: String) {
        runOnUiThread { Toast.makeText(this, "PadDisplay: $message", Toast.LENGTH_LONG).show() }
    }

    // The PC is locked and the tablet can't show that screen (frozen) → offer the password box; the unlock that
    // follows a correct password arrives here as frozen=false and dismisses it. Only a v5 host sends this, so
    // receiving it is itself proof the host can type the password. It cannot be told from the host's version: when
    // the tablet connects while already locked the host never sent HostHello (capture is refused on the secure
    // desktop). When frozen, auto-open the box so the user need not hunt for a button; the button stays as a way
    // back if they dismiss it.
    override fun onLockState(frozen: Boolean) {
        runOnUiThread {
            pcFrozen = frozen
            passwordButton.visibility = if (frozen) View.VISIBLE else View.GONE
            if (frozen) {
                if (!gotFirstFrame) stats.text = "PC locked — type your password" // not "Connecting…": we are connected
                openPasswordDialogIfFrozen()
            } else {
                passwordBox.dismiss() // signed in / streaming again
            }
        }
    }

    // Open the box by itself when the PC is frozen on the sign-in screen - but only while the window is actually
    // in front (showing a dialog on a backgrounded activity throws). onWindowFocusChanged reopens it on return.
    private fun openPasswordDialogIfFrozen() {
        if (pcFrozen && !isFinishing && hasWindowFocus()) passwordBox.show()
    }

    override fun onClosed(connection: Connection, reason: String?) {
        if (reason == null) return // closed on purpose: by this app, or by the PC with an error it shows
        runOnUiThread {
            if (connection !== this.connection || isFinishing) return@runOnUiThread
            if (everConnected && !connection.endedByBug) reconnectLater(reason) else fail(reason)
        }
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
                val u = upscaler
                val upscaled = if (u == null) "" else {
                    val draws = u.drawCount.getAndSet(0)
                    String.format(" · GSR upscale %.1f ms", if (draws > 0) u.drawUsTotal.getAndSet(0) / 1000f / draws else 0f)
                }

                @Suppress("DEPRECATION")
                val panelHz = windowManager.defaultDisplay.refreshRate.roundToInt()
                val a = media?.audio
                val line = String.format(
                    "%s %dx%d · %.0f fps (panel %d Hz) · %.1f Mbps (cap %.0f) · RTT %.1f ms · decode %.1f ms%s%s%s · %s",
                    codecName, videoW, videoH, fps, panelHz, mbps, hostKbps / 1000f, rttUs / 1000f, decMs, upscaled,
                    if (dropped > 0) " · skipped $dropped" else "",
                    if (a != null) String.format(
                        " · audio buffer %d/%d ms, speed %+.2f%%, underruns %d, dropped %d ms",
                        a.queuedMs,
                        a.targetMs.toInt(),
                        a.speedPercent,
                        a.refills,
                        a.droppedMs
                    ) else "", d.decoderName
                )
                if (gotFirstFrame) Log.i("PadStats", line)
                if (showStats || !gotFirstFrame) {
                    stats.visibility = View.VISIBLE
                    stats.text = if (gotFirstFrame) line else if (reconnectStartedMs != 0L) "Connection lost. Reconnecting…" else "Connecting…"
                }
            }
            receivedAtLastStats = bytesReceived
            lastStatsMs = now
            root.postDelayed(this, 1000)
        }
    }
}
