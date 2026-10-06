// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast

/** Connect screen: USB, discovered PCs on Wi-Fi, manual address, and input settings. */
class MainActivity : Activity() {

    private lateinit var hostList: LinearLayout
    private lateinit var status: TextView
    private lateinit var discovery: Discovery
    private val prefs by lazy { getSharedPreferences("settings", MODE_PRIVATE) }
    private var pendingHost: String? = null
    private var pendingPort = Protocol.TCP_PORT

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = dp(20)
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }
        col.addView(TextView(this).apply {
            text = "PadDisplay"
            textSize = 28f
            setTypeface(typeface, Typeface.BOLD)
        })
        val version = packageManager.getPackageInfo(packageName, 0).versionName
        col.addView(TextView(this).apply {
            text = "by Nezucloud · v$version"
            alpha = 0.7f
        })
        col.addView(TextView(this).apply {
            text = "Use this tablet as an extra monitor for your PC. Start PadDisplay on the PC first."
            setPadding(0, dp(4), 0, dp(16))
        })

        status = TextView(this).apply { setPadding(0, 0, 0, dp(8)) }
        col.addView(status)

        col.addView(section("USB"))
        col.addView(button("Connect over USB") { connect("127.0.0.1", Protocol.TCP_PORT) })
        col.addView(hint("Needs USB debugging enabled. The PC app sets up the USB tunnel automatically; the PIN is asked once."))

        col.addView(section("Wi-Fi"))
        hostList = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        col.addView(hostList)
        val manual = EditText(this).apply {
            hint = "PC address, e.g. 192.168.1.20"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            setText(prefs.getString("lastHost", ""))
            isSingleLine = true
        }
        val manualRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(manual, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(button("Connect") {
                val addr = manual.text.toString().trim()
                if (addr.isNotEmpty()) {
                    prefs.edit().putString("lastHost", addr).apply()
                    connect(addr, Protocol.TCP_PORT)
                }
            })
        }
        col.addView(manualRow)

        col.addView(section("Input"))
        col.addView(switch("Touch acts as mouse (tap = click, two fingers = scroll / right-click)", "mouseMode", false))
        col.addView(hint("Off = real Windows multi-touch. The stylus always works as a pen with pressure and tilt."))
        col.addView(switch("Send keyboard, mouse and game controllers connected to this tablet to the PC", "deviceInput", true))
        col.addView(switch("Capture the mouse for games (relative movement)", "mouseCapture", false))
        col.addView(hint("Ctrl+Alt+Shift+M switches mouse capture while connected. Game controllers need the ViGEmBus driver on the PC."))
        col.addView(switch("Show performance overlay", "stats", false))

        col.addView(section("Audio"))
        col.addView(switch("Play PC audio on this tablet", "audio", false))
        col.addView(switch("Mute the PC's speakers while playing here", "audioMutePc", true))
        col.addView(hint("Off = no audio is captured or sent at all. On uses about 1.5 Mbps and does not slow the picture."))

        col.addView(section("Display"))
        col.addView(switch("Use this tablet as the PC's only screen", "onlyScreen", false))
        col.addView(hint("Off = an extra screen next to the PC's own. On = the PC's own screens are switched off while this tablet is connected, and come back when it disconnects."))
        col.addView(switch("Gaming mode (the stream gets GPU time ahead of the game)", "gamingMode", false))
        col.addView(hint("Turn it on if the picture stutters while a game uses the whole GPU. With little free video memory it can freeze the NVIDIA encoder."))
        val rates = DisplayModes.rates(this)
        col.addView(hint("Refresh rate (the PC's virtual monitor switches to match)"))
        col.addView(radioGroup("refreshRate", 0,
            listOf(0 to "Highest (${rates.last()} Hz)") + rates.map { it to "$it Hz" }))
        col.addView(hint("Wi-Fi power mode while streaming"))
        col.addView(radioGroup("wifiMode", 1, listOf(
            1 to "High performance (recommended)",
            2 to "Low latency (may stall a few seconds when connecting on some devices)",
            0 to "System default (power saving, choppier)")))

        setContentView(ScrollView(this).apply { addView(col) })
        discovery = Discovery(this) { showHosts(it) }
        showHosts(emptyList())
    }

    override fun onResume() {
        super.onResume()
        discovery.start()
    }

    override fun onPause() {
        discovery.stop()
        super.onPause()
    }

    private fun showHosts(hosts: List<Discovery.Host>) {
        hostList.removeAllViews()
        if (hosts.isEmpty()) hostList.addView(hint("Searching for PCs on this network…"))
        for (h in hosts) hostList.addView(button("${h.name}  (${h.address})") { connect(h.address, h.port) })
    }

    private fun isLoopback(host: String) = host == "127.0.0.1" || host == "localhost" || host == "::1"

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
        status.text = ""
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
            DisplayActivity.RESULT_FAILED -> {
                status.text = "Last connection to $host: ${error ?: "failed"}" +
                    if (isLoopback(host)) "\nIs the tablet plugged in with USB debugging on, and PadDisplay running on the PC?" else ""
            }
        }
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun section(title: String) = TextView(this).apply {
        text = title
        textSize = 18f
        setTypeface(typeface, Typeface.BOLD)
        setPadding(0, dp(20), 0, dp(6))
    }

    private fun hint(text: String) = TextView(this).apply {
        this.text = text
        alpha = 0.7f
        setPadding(0, dp(4), 0, dp(4))
    }

    private fun button(text: String, onClick: () -> Unit) = Button(this).apply {
        this.text = text
        isAllCaps = false
        setOnClickListener { onClick() }
    }

    private fun radioGroup(key: String, default: Int, options: List<Pair<Int, String>>) = RadioGroup(this).apply {
        val current = prefs.getInt(key, default)
        for ((value, label) in options) {
            addView(RadioButton(this@MainActivity).apply {
                id = View.generateViewId()
                text = label
                isChecked = value == current
                setOnClickListener { prefs.edit().putInt(key, value).apply() }
            })
        }
    }

    private fun switch(label: String, key: String, default: Boolean) = Switch(this).apply {
        text = label
        isChecked = prefs.getBoolean(key, default)
        setPadding(0, dp(8), 0, dp(8))
        setOnCheckedChangeListener { _, checked -> prefs.edit().putBoolean(key, checked).apply() }
        textAlignment = View.TEXT_ALIGNMENT_VIEW_START
    }
}
