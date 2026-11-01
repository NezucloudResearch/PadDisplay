// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.view.KeyEvent
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class KeyboardTest {

    private val extended = Protocol.KEY_EXTENDED

    private fun fromKeyboard(linuxCode: Int, keyCode: Int = KeyEvent.KEYCODE_UNKNOWN) =
        Keyboard.scancode(KeyEvent(0, 0, KeyEvent.ACTION_DOWN, keyCode, 0, 0, -1, linuxCode))

    private fun injected(keyCode: Int) = Keyboard.scancode(KeyEvent(KeyEvent.ACTION_DOWN, keyCode))

    @Test
    fun `below 84 a Linux code is the PC scancode`() {
        assertEquals(0x1E, fromKeyboard(30)) // A
        assertEquals(0x01, fromKeyboard(1)) // Esc
    }

    @Test
    fun `the keys past F10 are mapped`() {
        assertEquals(0x57, fromKeyboard(87)) // F11
        assertEquals(0x58, fromKeyboard(88)) // F12
    }

    @Test
    fun `navigation keys are extended`() {
        assertEquals(extended or 0x48, fromKeyboard(103)) // Up
        assertEquals(extended or 0x53, fromKeyboard(111)) // Delete
    }

    @Test
    fun `right-hand modifiers differ from the left ones`() {
        assertEquals(extended or 0x1D, fromKeyboard(97))
        assertEquals(0x1D, fromKeyboard(29))
    }

    @Test
    fun `the Windows keys are mapped`() {
        assertEquals(extended or 0x5B, fromKeyboard(125))
        assertEquals(extended or 0x5C, fromKeyboard(126))
    }

    @Test
    fun `F13 to F24 are mapped`() {
        assertEquals(0x64, fromKeyboard(183))
        assertEquals(0x6E, fromKeyboard(193))
        assertEquals(0x76, fromKeyboard(194))
    }

    @Test
    fun `Pause has its own code`() {
        assertEquals(Protocol.KEY_PAUSE, fromKeyboard(119))
    }

    @Test
    fun `a key without a scan code goes by its key code`() {
        assertEquals(0x1E, injected(KeyEvent.KEYCODE_A))
        assertEquals(0x0B, injected(KeyEvent.KEYCODE_0))
        assertEquals(0x02, injected(KeyEvent.KEYCODE_1))
        assertEquals(0x1C, injected(KeyEvent.KEYCODE_ENTER))
    }

    @Test
    fun `the keys on a tablet's edge are not for the PC`() {
        assertEquals(0, injected(KeyEvent.KEYCODE_POWER))
        assertEquals(0, fromKeyboard(0))
    }
}
