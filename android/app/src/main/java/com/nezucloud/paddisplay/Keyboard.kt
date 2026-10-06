// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.util.SparseIntArray
import android.view.KeyEvent

/**
 * Turns a key of a keyboard connected to the tablet into the PC scancode (set 1) the host injects.
 * The key's position is sent, not its character: the PC applies its own keyboard layout, as it
 * would for a keyboard plugged into it.
 */
object Keyboard {
    private const val E = Protocol.KEY_EXTENDED

    /** Linux key codes (KeyEvent.scanCode) above 88. Up to there they equal the PC scancode. */
    private val fromLinux = SparseIntArray().apply {
        put(89, 0x73) // Ro
        put(92, 0x79) // Henkan
        put(93, 0x70) // Katakana/Hiragana
        put(94, 0x7B) // Muhenkan
        put(124, 0x7D) // Yen
        put(96, E or 0x1C) // numpad Enter
        put(97, E or 0x1D) // right Ctrl
        put(98, E or 0x35) // numpad /
        put(99, E or 0x37) // Print Screen
        put(100, E or 0x38) // right Alt
        put(102, E or 0x47) // Home
        put(103, E or 0x48) // Up
        put(104, E or 0x49) // Page Up
        put(105, E or 0x4B) // Left
        put(106, E or 0x4D) // Right
        put(107, E or 0x4F) // End
        put(108, E or 0x50) // Down
        put(109, E or 0x51) // Page Down
        put(110, E or 0x52) // Insert
        put(111, E or 0x53) // Delete
        put(113, E or 0x20) // Mute
        put(114, E or 0x2E) // Volume down
        put(115, E or 0x30) // Volume up
        put(117, 0x59) // numpad =
        put(119, Protocol.KEY_PAUSE)
        put(125, E or 0x5B) // left Windows
        put(126, E or 0x5C) // right Windows
        put(127, E or 0x5D) // Menu
        put(163, E or 0x19) // Next track
        put(164, E or 0x22) // Play/pause
        put(165, E or 0x10) // Previous track
        put(166, E or 0x24) // Stop
        for (i in 0..10) put(183 + i, 0x64 + i) // F13..F23
        put(194, 0x76) // F24
    }

    /** For key events without a scan code (injected, e.g. by adb or a remote-control tool). */
    private val fromAndroid = SparseIntArray().apply {
        val letters = intArrayOf( // A..Z on a QWERTY keyboard
            0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
            0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C)
        for (i in letters.indices) put(KeyEvent.KEYCODE_A + i, letters[i])
        put(KeyEvent.KEYCODE_0, 0x0B)
        for (i in 1..9) put(KeyEvent.KEYCODE_0 + i, 0x01 + i)
        for (i in 0..9) put(KeyEvent.KEYCODE_F1 + i, 0x3B + i)
        put(KeyEvent.KEYCODE_F11, 0x57)
        put(KeyEvent.KEYCODE_F12, 0x58)
        put(KeyEvent.KEYCODE_ESCAPE, 0x01)
        put(KeyEvent.KEYCODE_MINUS, 0x0C)
        put(KeyEvent.KEYCODE_EQUALS, 0x0D)
        put(KeyEvent.KEYCODE_DEL, 0x0E) // Backspace
        put(KeyEvent.KEYCODE_TAB, 0x0F)
        put(KeyEvent.KEYCODE_LEFT_BRACKET, 0x1A)
        put(KeyEvent.KEYCODE_RIGHT_BRACKET, 0x1B)
        put(KeyEvent.KEYCODE_ENTER, 0x1C)
        put(KeyEvent.KEYCODE_CTRL_LEFT, 0x1D)
        put(KeyEvent.KEYCODE_SEMICOLON, 0x27)
        put(KeyEvent.KEYCODE_APOSTROPHE, 0x28)
        put(KeyEvent.KEYCODE_GRAVE, 0x29)
        put(KeyEvent.KEYCODE_SHIFT_LEFT, 0x2A)
        put(KeyEvent.KEYCODE_BACKSLASH, 0x2B)
        put(KeyEvent.KEYCODE_COMMA, 0x33)
        put(KeyEvent.KEYCODE_PERIOD, 0x34)
        put(KeyEvent.KEYCODE_SLASH, 0x35)
        put(KeyEvent.KEYCODE_SHIFT_RIGHT, 0x36)
        put(KeyEvent.KEYCODE_ALT_LEFT, 0x38)
        put(KeyEvent.KEYCODE_SPACE, 0x39)
        put(KeyEvent.KEYCODE_CAPS_LOCK, 0x3A)
        put(KeyEvent.KEYCODE_NUM_LOCK, 0x45)
        put(KeyEvent.KEYCODE_SCROLL_LOCK, 0x46)
        put(KeyEvent.KEYCODE_BREAK, Protocol.KEY_PAUSE)
        put(KeyEvent.KEYCODE_SYSRQ, E or 0x37)
        put(KeyEvent.KEYCODE_CTRL_RIGHT, E or 0x1D)
        put(KeyEvent.KEYCODE_ALT_RIGHT, E or 0x38)
        put(KeyEvent.KEYCODE_META_LEFT, E or 0x5B)
        put(KeyEvent.KEYCODE_META_RIGHT, E or 0x5C)
        put(KeyEvent.KEYCODE_MENU, E or 0x5D)
        put(KeyEvent.KEYCODE_MOVE_HOME, E or 0x47)
        put(KeyEvent.KEYCODE_DPAD_UP, E or 0x48)
        put(KeyEvent.KEYCODE_PAGE_UP, E or 0x49)
        put(KeyEvent.KEYCODE_DPAD_LEFT, E or 0x4B)
        put(KeyEvent.KEYCODE_DPAD_RIGHT, E or 0x4D)
        put(KeyEvent.KEYCODE_MOVE_END, E or 0x4F)
        put(KeyEvent.KEYCODE_DPAD_DOWN, E or 0x50)
        put(KeyEvent.KEYCODE_PAGE_DOWN, E or 0x51)
        put(KeyEvent.KEYCODE_INSERT, E or 0x52)
        put(KeyEvent.KEYCODE_FORWARD_DEL, E or 0x53)
        val numpad = intArrayOf(0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49) // 0..9
        for (i in numpad.indices) put(KeyEvent.KEYCODE_NUMPAD_0 + i, numpad[i])
        put(KeyEvent.KEYCODE_NUMPAD_DIVIDE, E or 0x35)
        put(KeyEvent.KEYCODE_NUMPAD_MULTIPLY, 0x37)
        put(KeyEvent.KEYCODE_NUMPAD_SUBTRACT, 0x4A)
        put(KeyEvent.KEYCODE_NUMPAD_ADD, 0x4E)
        put(KeyEvent.KEYCODE_NUMPAD_DOT, 0x53)
        put(KeyEvent.KEYCODE_NUMPAD_ENTER, E or 0x1C)
    }

    /** 0 = not a key the PC knows; leave it to Android. */
    fun scancode(ev: KeyEvent): Int {
        val linux = ev.scanCode
        if (linux in 1..83 || linux in 86..88) return linux
        val mapped = fromLinux.get(linux)
        return if (mapped != 0) mapped else fromAndroid.get(ev.keyCode)
    }
}
