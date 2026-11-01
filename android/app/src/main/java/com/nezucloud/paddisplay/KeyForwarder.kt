// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.view.InputDevice
import android.view.KeyEvent
import android.view.View
import android.view.ViewConfiguration

/**
 * Sends the keys of a keyboard connected to the tablet to the PC, and remembers which are down, so that none stays held
 * when the window loses focus. UI thread.
 */
class KeyForwarder(
    private val view: View, // only to post the repeats on
    private val gamepads: Gamepads,
    private val send: (ByteArray) -> Unit,
    private val keysStayOnTablet: () -> Boolean,
    private val onMouseCaptureChord: () -> Unit,
) {

    private companion object {
        const val SCANCODE_F24 = 0x76
    }

    private val pressed = BooleanArray(0x200) // PC scancodes sent as down and not yet up, lifted if focus is lost mid-chord
    private var repeatCode = 0

    // Held modifiers and lock keys are not repeated, as on a real keyboard.
    private val noRepeat = intArrayOf(
        0x1D, 0x2A, 0x36, 0x38, 0x3A, 0x45, 0x46, Protocol.KEY_EXTENDED or 0x1D,
        Protocol.KEY_EXTENDED or 0x38, Protocol.KEY_EXTENDED or 0x5B, Protocol.KEY_EXTENDED or 0x5C, Protocol.KEY_PAUSE
    )

    private val repeater = object : Runnable {
        override fun run() {
            if (repeatCode == 0) return
            sendKey(repeatCode, true)
            view.postDelayed(this, ViewConfiguration.getKeyRepeatDelay().toLong())
        }
    }

    /**
     * true = the key went to the PC (or was used here), so Android must not act on it.
     * [fromService]: the key came through ShortcutService, before Android's own handling. Android then
     * generates no auto-repeat for it, so it is made here (the PC does not repeat injected keys itself).
     */
    fun forward(ev: KeyEvent, fromService: Boolean = false): Boolean {
        if (keysStayOnTablet()) return false
        if (gamepads.onKey(ev)) return true
        // A mouse's side buttons also arrive as Back/Forward keys. They are sent as mouse buttons.
        if (ev.isFromSource(InputDevice.SOURCE_MOUSE) || ev.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE))
            return ev.keyCode == KeyEvent.KEYCODE_BACK || ev.keyCode == KeyEvent.KEYCODE_FORWARD
        val down = ev.action == KeyEvent.ACTION_DOWN
        if (!down && ev.action != KeyEvent.ACTION_UP) return false
        if (isTabletsOwnVolumeKey(ev)) return false
        val code = Keyboard.scancode(ev)
        if (code == 0) return false
        if (isMouseCaptureChord(ev)) {
            if (down && ev.repeatCount == 0) {
                onMouseCaptureChord()
                // The PC got Ctrl, Alt and Shift but not the M. Released with nothing in between, such
                // a chord is how Windows switches the input language: put an unused key in between.
                sendKey(SCANCODE_F24, true)
                sendKey(SCANCODE_F24, false)
            }
            return true
        }
        sendKey(code, down) // repeats too: Windows does not repeat an injected key by itself
        if (fromService) repeatLikeAndroid(code, down)
        return true
    }

    // The tablet's own volume buttons stay with the tablet; a keyboard's go to the PC.
    private fun isTabletsOwnVolumeKey(ev: KeyEvent): Boolean {
        val volume = ev.keyCode == KeyEvent.KEYCODE_VOLUME_UP || ev.keyCode == KeyEvent.KEYCODE_VOLUME_DOWN ||
                ev.keyCode == KeyEvent.KEYCODE_VOLUME_MUTE
        return volume && ev.device?.isExternal != true
    }

    private fun isMouseCaptureChord(ev: KeyEvent) =
        ev.keyCode == KeyEvent.KEYCODE_M && ev.isCtrlPressed && ev.isAltPressed && ev.isShiftPressed

    private fun repeatLikeAndroid(code: Int, down: Boolean) {
        if (down && code !in noRepeat) {
            repeatCode = code
            view.removeCallbacks(repeater)
            view.postDelayed(repeater, ViewConfiguration.getKeyRepeatTimeout().toLong())
        } else if (!down && code == repeatCode) {
            stopRepeat()
        }
    }

    private fun stopRepeat() {
        repeatCode = 0
        view.removeCallbacks(repeater)
    }

    fun releaseAll() {
        stopRepeat()
        for (c in pressed.indices) if (pressed[c]) sendKey(c, false)
    }

    private fun sendKey(code: Int, down: Boolean) {
        if (code in pressed.indices) pressed[code] = down
        send(Protocol.message(Protocol.KEY, 3) {
            it.put((if (down) Protocol.KEY_DOWN else 0).toByte())
            it.putShort(code.toShort())
        })
    }
}
