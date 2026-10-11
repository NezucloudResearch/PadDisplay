// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.provider.Settings
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast

/** Connect screen: USB, discovered PCs on Wi-Fi, manual address, and input settings. */
class MainActivity : Activity() {

    private lateinit var hostList: LinearLayout
    private lateinit var status: TextView
    private lateinit var shortcutState: TextView
    private lateinit var discovery: Discovery
    private val ui by lazy { Ui(this) }
    private val prefs by lazy { getSharedPreferences("settings", MODE_PRIVATE) }
    private var pendingHost: String? = null
    private var pendingPort = Protocol.TCP_PORT

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = ui.dp(20)
        val col = MaxWidthColumn(this, ui.dp(720)).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, ui.dp(28), pad, ui.dp(32))
        }

        // Header
        col.addView(ui.text("PadDisplay", 32f, R.color.pd_text, bold = true))
        val version = packageManager.getPackageInfo(packageName, 0).versionName
        col.addView(ui.caption("by Nezucloud · v$version").apply { setPadding(0, ui.dp(2), 0, 0) })
        col.addView(ui.text("Use this tablet as an extra monitor for your PC. Start PadDisplay on the PC first.", 15f, R.color.pd_text2).apply {
            setPadding(0, ui.dp(12), 0, ui.dp(18))
        })

        status = ui.text("", 14f, R.color.pd_danger).apply {
            background = ui.shape(ui.color(R.color.pd_danger_soft), 12)
            setPadding(ui.dp(16), ui.dp(12), ui.dp(16), ui.dp(12))
            visibility = View.GONE
        }
        col.addView(status, spaced(bottom = ui.dp(14)))

        // Connect
        val connect = ui.card("Connect")
        connect.addView(ui.primaryButton("Connect over USB") { connect("127.0.0.1", Protocol.TCP_PORT) }, spaced(top = ui.dp(4), bottom = ui.dp(6)))
        connect.addView(ui.caption("Needs USB debugging enabled. The PC app sets up the USB tunnel automatically; the PIN is asked once."))
        connect.addView(ui.divider(), spaced(top = ui.dp(16), bottom = ui.dp(14)))
        connect.addView(ui.text("On Wi-Fi", 15f, R.color.pd_text, bold = true))
        hostList = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        connect.addView(hostList)
        val manual = ui.field("PC address, e.g. 192.168.1.20").apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            setText(prefs.getString("lastHost", ""))
        }
        val manualRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(manual, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(ui.tonalButton("Connect") {
                val addr = manual.text.toString().trim()
                if (addr.isNotEmpty()) {
                    prefs.edit().putString("lastHost", addr).apply()
                    connect(addr, Protocol.TCP_PORT)
                }
            }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { marginStart = ui.dp(10) })
        }
        connect.addView(manualRow, spaced(top = ui.dp(8), bottom = ui.dp(10)))
        col.addView(connect, spaced(bottom = ui.dp(14)))

        // Input
        val input = ui.card("Input")
        ui.addRow(input, toggle("Touch acts as mouse",
            "Tap = click, two fingers = scroll / right-click. Off = real Windows multi-touch. The stylus always works as a pen with pressure and tilt.",
            "mouseMode", false))
        ui.addRow(input, toggle("Send keyboard, mouse and game controllers to the PC",
            "Those connected to this tablet. Game controllers need the ViGEmBus driver on the PC.", "deviceInput", true))
        ui.addRow(input, toggle("Send the keyboard's system shortcuts too",
            "Alt+Tab, the Windows key, Ctrl+Esc and similar go to the PC instead of this tablet. Needs the PadDisplay keyboard service switched on in Android's accessibility settings; it only sees keys while PadDisplay is open and connected.",
            "grabShortcuts", true))
        shortcutState = ui.text("", 13f, R.color.pd_ok, bold = true)
        ui.addRow(input, LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, ui.dp(10), 0, ui.dp(12))
            addView(shortcutState)
            addView(ui.tonalButton("Open accessibility settings") {
                startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
            }, spaced(top = ui.dp(8)))
        })
        ui.addRow(input, toggle("Capture the mouse for games",
            "Relative movement. Ctrl+Alt+Shift+M switches mouse capture while connected.", "mouseCapture", false))
        ui.addRow(input, toggle("Show performance overlay", null, "stats", false))
        col.addView(input, spaced(bottom = ui.dp(14)))

        // Audio
        val audio = ui.card("Audio")
        ui.addRow(audio, toggle("Play PC audio on this tablet",
            "Off = no audio is captured or sent at all. On uses about 1.5 Mbps and does not slow the picture.", "audio", false))
        ui.addRow(audio, toggle("Mute the PC's speakers while playing here", null, "audioMutePc", true))
        col.addView(audio, spaced(bottom = ui.dp(14)))

        // Microphone and camera
        val media = ui.card("Microphone and camera")
        ui.addRow(media, permissionToggle("Use this tablet's microphone as a PC microphone",
            "Needs a virtual microphone on the PC: PadDisplay there offers to set one up the first time. Apps then pick \"CABLE Output\" (VB-CABLE) or \"PadDisplay Virtual Microphone\". Off = the microphone is never used.",
            "micToPc", Manifest.permission.RECORD_AUDIO))
        ui.addRow(media, permissionToggle("Use this tablet's camera as a PC webcam",
            "Needs Windows 11 on the PC and its one-time setup. The camera only turns on while an app on the PC has the webcam open, and a \"camera on\" label shows here. Off = the camera is never used.",
            "camToPc", Manifest.permission.CAMERA))
        ui.addRow(media, labelled("Camera", segmented("camFacing", 1, listOf(1 to "Front", 0 to "Back"))))
        ui.addRow(media, labelled("Webcam quality", segmented("camQuality", 720, listOf(720 to "720p (recommended)", 1080 to "1080p (uses more Wi-Fi)"))))
        ui.addRow(media, ui.caption("Only this tablet decides: the PC cannot turn the microphone or camera on.").apply { setPadding(0, ui.dp(12), 0, ui.dp(12)) })
        col.addView(media, spaced(bottom = ui.dp(14)))

        // Display
        val display = ui.card("Display")
        ui.addRow(display, toggle("Use this tablet as the PC's only screen",
            "Off = an extra screen next to the PC's own. On = the PC's own screens are switched off while this tablet is connected, and come back when it disconnects.",
            "onlyScreen", false))
        ui.addRow(display, toggle("Gaming mode",
            "The stream gets GPU time ahead of the game. Turn it on if the picture stutters while a game uses the whole GPU. With little free video memory it can freeze the NVIDIA encoder.",
            "gamingMode", false))
        val rates = DisplayModes.rates(this)
        ui.addRow(display, labelled("Refresh rate", choiceList("refreshRate", 0,
            listOf(0 to "Highest (${rates.last()} Hz)") + rates.map { it to "$it Hz" }),
            "The PC's virtual monitor switches to match."))
        ui.addRow(display, labelled("Wi-Fi power mode while streaming", choiceList("wifiMode", 1, listOf(
            1 to "High performance (recommended)",
            2 to "Low latency (may stall a few seconds when connecting on some devices)",
            0 to "System default (power saving, choppier)"))))
        col.addView(display, spaced(bottom = ui.dp(6)))

        setContentView(ScrollView(this).apply {
            isFillViewport = true
            addView(col, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER_HORIZONTAL))
        })
        discovery = Discovery(this) { showHosts(it) }
        showHosts(emptyList())
    }

    private fun spaced(top: Int = 0, bottom: Int = 0) =
        LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            topMargin = top
            bottomMargin = bottom
        }

    /** The title of a setting above its control, and an optional explanation below. */
    private fun labelled(title: String, control: View, note: String? = null) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, ui.dp(12), 0, ui.dp(12))
        addView(ui.title(title))
        if (note != null) addView(ui.caption(note).apply { setPadding(0, ui.dp(2), 0, 0) })
        addView(control, spaced(top = ui.dp(10)))
    }

    private fun segmented(key: String, default: Int, options: List<Pair<Int, String>>) =
        ui.segmented(options, prefs.getInt(key, default)) { prefs.edit().putInt(key, it).apply() }

    private fun choiceList(key: String, default: Int, options: List<Pair<Int, String>>) =
        ui.choiceList(options, prefs.getInt(key, default)) { prefs.edit().putInt(key, it).apply() }

    override fun onResume() {
        super.onResume()
        discovery.start()
        val on = ShortcutService.isEnabled(this)
        shortcutState.text = if (on) "Keyboard service is on" else "Keyboard service is off: system shortcuts stay with the tablet"
        shortcutState.setTextColor(ui.color(if (on) R.color.pd_ok else R.color.pd_text2))
    }

    override fun onPause() {
        discovery.stop()
        super.onPause()
    }

    private fun showHosts(hosts: List<Discovery.Host>) {
        hostList.removeAllViews()
        if (hosts.isEmpty()) hostList.addView(ui.caption("Searching for PCs on this network…").apply { setPadding(0, ui.dp(6), 0, ui.dp(4)) })
        for (h in hosts) hostList.addView(hostRow(h), spaced(top = ui.dp(8)))
    }

    private fun isLoopback(host: String) = Tls.isLoopback(host)

    // The PIN is required over USB too: the adb tunnel is reachable by any app on this tablet.
    private fun connect(host: String, port: Int) {
        val pin = prefs.getInt("pin_$host", 0)
        if (pin != 0) launch(host, port, pin) else askPin(host, port, null)
    }

    private fun askPin(host: String, port: Int, error: String?) {
        val field = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER
            hint = "6-digit PIN"
            gravity = Gravity.CENTER
            textSize = 22f
        }
        AlertDialog.Builder(this)
            .setTitle(if (isLoopback(host)) "PIN for the PC (USB)" else "PIN for $host")
            .setMessage((error?.let { "$it\n\n" } ?: "") + "Right-click the PadDisplay icon in the PC's tray to see the PIN. It is asked once and remembered.")
            .setView(field)
            .setPositiveButton("Connect") { _, _ ->
                val pin = field.text.toString().toIntOrNull() ?: 0
                prefs.edit().putInt("pin_$host", pin).apply()
                launch(host, port, pin)
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun launch(host: String, port: Int, pin: Int) {
        pendingHost = host
        pendingPort = port
        showStatus("")
        startActivityForResult(Intent(this, DisplayActivity::class.java)
            .putExtra(DisplayActivity.EXTRA_HOST, host)
            .putExtra(DisplayActivity.EXTRA_PORT, port)
            .putExtra(DisplayActivity.EXTRA_PIN, pin), 1)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val error = data?.getStringExtra(DisplayActivity.EXTRA_ERROR)
        val host = pendingHost ?: return
        when (resultCode) {
            DisplayActivity.RESULT_BAD_PIN -> {
                prefs.edit().remove("pin_$host").apply()
                askPin(host, pendingPort, error)
            }
            DisplayActivity.RESULT_FINGERPRINT_CHANGED -> {
                val fingerprint = error?.let { HostTrust.short(it) } ?: "?"
                AlertDialog.Builder(this)
                    .setTitle("This PC's security fingerprint changed")
                    .setMessage("The PC at $host shows a different encryption identity than the one this tablet remembered.\n\n" +
                        "New fingerprint: $fingerprint\n\n" +
                        "If you reinstalled PadDisplay on the PC, or use another Windows account there, compare it with the fingerprint in the PC's tray menu and choose Trust. " +
                        "If you changed nothing, someone on this network may be pretending to be your PC: don't connect.")
                    .setPositiveButton("Trust the new identity") { _, _ ->
                        prefs.edit().remove("tlsfp_$host").apply()
                        connect(host, pendingPort)
                    }
                    .setNegativeButton("Cancel", null)
                    .show()
            }
            DisplayActivity.RESULT_FAILED -> {
                showStatus("Last connection to $host: ${error ?: "failed"}" +
                    if (isLoopback(host)) "\nIs the tablet plugged in with USB debugging on, and PadDisplay running on the PC?" else "")
            }
        }
    }

    private fun showStatus(text: String) {
        status.text = text
        status.visibility = if (text.isEmpty()) View.GONE else View.VISIBLE
    }

    private fun hostRow(h: Discovery.Host) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(ui.dp(16), ui.dp(12), ui.dp(16), ui.dp(12))
        background = ui.ripple(ui.shape(ui.color(R.color.pd_accent_soft), 12), 12)
        addView(ui.text(h.name, 16f, R.color.pd_accent, bold = true))
        addView(ui.caption(h.address))
        setOnClickListener { connect(h.address, h.port) }
    }

    private fun toggle(title: String, description: String?, key: String, default: Boolean) =
        ui.toggleRow(title, description, ui.switchView().apply {
            isChecked = prefs.getBoolean(key, default)
            setOnCheckedChangeListener { _, checked -> prefs.edit().putBoolean(key, checked).apply() }
        })

    // A switch for the microphone or camera: turning it on asks Android for the permission first, and it only stays on if the
    // user allows it. The permission request is the system's own dialog, so the user always sees what is being allowed.
    private val permissionSwitches = HashMap<Int, Pair<Switch, String>>()

    private fun permissionToggle(title: String, description: String, key: String, permission: String): View {
        val code = permissionSwitches.size + 100
        val sw = ui.switchView().apply {
            isChecked = prefs.getBoolean(key, false) && checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED
            if (!isChecked) prefs.edit().putBoolean(key, false).apply()
            permissionSwitches[code] = this to key
            setOnCheckedChangeListener { _, checked ->
                if (!checked) {
                    prefs.edit().putBoolean(key, false).apply()
                } else if (checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED) {
                    prefs.edit().putBoolean(key, true).apply()
                } else {
                    requestPermissions(arrayOf(permission), code)
                }
            }
        }
        return ui.toggleRow(title, description, sw)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        val (switch, key) = permissionSwitches[requestCode] ?: return
        val granted = grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED
        prefs.edit().putBoolean(key, granted).apply()
        if (!granted) {
            switch.isChecked = false
            Toast.makeText(this, "PadDisplay needs that permission to use it", Toast.LENGTH_LONG).show()
        }
    }
}

/** A column no wider than [maxWidth], so the cards stay readable on a wide tablet. */
private class MaxWidthColumn(context: Context, private val maxWidth: Int) : LinearLayout(context) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        super.onMeasure(if (w > maxWidth) MeasureSpec.makeMeasureSpec(maxWidth, MeasureSpec.EXACTLY) else widthMeasureSpec, heightMeasureSpec)
    }
}
