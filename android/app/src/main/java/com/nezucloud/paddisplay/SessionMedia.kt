// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.Manifest
import android.app.Activity
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.util.Log
import android.widget.Toast

/** Each on its own, because one that throws must not leave the others open: the microphone and the camera would go on sending. */
fun closeEach(vararg closers: () -> Unit) {
    for (close in closers) {
        try {
            close()
        } catch (e: Exception) {
            Log.e("PadDisplay", "closing the session", e)
        }
    }
}

/**
 * The connections of a session beside the picture: the PC's audio, this tablet's microphone and its camera. UI thread,
 * except [micOn] and [camOn], which the streams set from their own threads.
 */
class SessionMedia(
    private val activity: Activity,
    private val host: String,
    private val port: Int,
    private val pin: Int,
    private val trust: HostTrust?,
    private val prefs: SharedPreferences,
    private val onCaptureChanged: () -> Unit, // UI thread
) {
    @Volatile var micOn = false
        private set
    @Volatile var camOn = false
        private set
    var audio: AudioStream? = null
        private set
    private var mic: MicStream? = null
    private var cam: CameraStream? = null

    fun startAudio() {
        if (audio != null || !prefs.getBoolean("audio", false)) return // already running from the start of the session
        audio = AudioStream(host, port, pin, prefs.getBoolean("audioMutePc", true), trust).also { it.start() }
    }

    // These open only once the PC has accepted the PIN on the video connection, so a wrong PIN is counted once and not
    // once per connection, and only if the user switched them on and Android allows it.
    fun startCapture() {
        startAudio()
        if (prefs.getBoolean("micToPc", false)) startMicrophone()
        if (prefs.getBoolean("camToPc", false)) startCamera()
    }

    private fun granted(permission: String) = activity.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    private fun toast(message: String) = Toast.makeText(activity, message, Toast.LENGTH_LONG).show()

    private fun startMicrophone() {
        if (!granted(Manifest.permission.RECORD_AUDIO)) {
            toast("Microphone: permission was taken away in Android's settings")
            return
        }
        mic = MicStream(host, port, pin, trust, object : MicStream.Listener {
            override fun onMicState(active: Boolean) {
                micOn = active
                activity.runOnUiThread(onCaptureChanged)
            }

            override fun onMicNotice(message: String) {
                activity.runOnUiThread { toast("Microphone: $message") }
            }
        }).also { it.start() }
    }

    private fun startCamera() {
        if (!granted(Manifest.permission.CAMERA)) {
            toast("Camera: permission was taken away in Android's settings")
            return
        }
        val frontCamera = prefs.getInt("camFacing", 1) == 1
        val height720 = prefs.getInt("camQuality", 720) != 1080
        cam = CameraStream(activity.applicationContext, host, port, pin, trust, frontCamera, height720, object : CameraStream.Listener {
            override fun onCameraState(streaming: Boolean) {
                camOn = streaming
                activity.runOnUiThread(onCaptureChanged)
            }

            override fun onCameraNotice(message: String) {
                activity.runOnUiThread { toast("Camera: $message") }
            }
        }).also { it.start() }
    }

    fun stop() {
        closeEach({ audio?.close() }, { mic?.close() }, { cam?.close() })
        audio = null
        mic = null
        cam = null
        micOn = false
        camOn = false
    }
}
