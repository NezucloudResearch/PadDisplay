// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.app.Activity
import android.os.Looper
import android.view.KeyEvent
import android.view.View
import android.view.ViewConfiguration
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.time.Duration

@RunWith(RobolectricTestRunner::class)
class KeyForwarderTest {

    private data class Sent(val down: Boolean, val code: Int)

    private val sent = ArrayList<Sent>()
    private var typingPassword = false
    private var chords = 0
    private lateinit var keys: KeyForwarder

    @Before
    fun build() {
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        val view = View(activity)
        activity.setContentView(view)
        val pads = Gamepads(activity) {}
        keys = KeyForwarder(view, pads, { record(it) }, { typingPassword }, { chords++ })
    }

    private fun record(message: ByteArray) {
        val p = ByteBuffer.wrap(message).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(Protocol.KEY, p.get().toInt() and 0xFF)
        p.int
        sent += Sent(p.get().toInt() == Protocol.KEY_DOWN, p.short.toInt() and 0xFFFF)
    }

    private fun key(action: Int, keyCode: Int, linuxCode: Int, meta: Int = 0, repeat: Int = 0) =
        KeyEvent(0, 0, action, keyCode, repeat, meta, -1, linuxCode)

    private fun down(linuxCode: Int, keyCode: Int = KeyEvent.KEYCODE_A) = key(KeyEvent.ACTION_DOWN, keyCode, linuxCode)
    private fun up(linuxCode: Int, keyCode: Int = KeyEvent.KEYCODE_A) = key(KeyEvent.ACTION_UP, keyCode, linuxCode)

    private fun idle(ms: Long) = shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms))

    private val repeatStart get() = ViewConfiguration.getKeyRepeatTimeout().toLong()
    private val repeatStep get() = ViewConfiguration.getKeyRepeatDelay().toLong()

    @Test
    fun `a key goes to the PC and Android does not act on it`() {
        assertTrue(keys.forward(down(30)))
        assertTrue(keys.forward(up(30)))
        assertEquals(listOf(Sent(true, 0x1E), Sent(false, 0x1E)), sent)
    }

    @Test
    fun `while the password box is open keys stay on the tablet`() {
        typingPassword = true
        assertFalse(keys.forward(down(30)))
        assertTrue(sent.isEmpty())
    }

    @Test
    fun `a key the PC does not know stays with Android`() {
        assertFalse(keys.forward(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_POWER)))
        assertTrue(sent.isEmpty())
    }

    @Test
    fun `other key actions stay with Android`() {
        assertFalse(keys.forward(key(KeyEvent.ACTION_MULTIPLE, KeyEvent.KEYCODE_A, 30)))
    }

    @Test
    fun `the tablet's own volume keys stay with the tablet`() {
        assertFalse(keys.forward(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_VOLUME_UP, 115)))
        assertTrue(sent.isEmpty())
    }

    @Test
    fun `losing focus lifts what is held`() {
        keys.forward(down(30))
        keys.forward(down(29, KeyEvent.KEYCODE_CTRL_LEFT))
        sent.clear()
        keys.releaseAll()
        assertEquals(setOf(Sent(false, 0x1D), Sent(false, 0x1E)), sent.toSet())
    }

    @Test
    fun `a key already released is not lifted again`() {
        keys.forward(down(30))
        keys.forward(up(30))
        sent.clear()
        keys.releaseAll()
        assertTrue(sent.isEmpty())
    }

    @Test
    fun `the mouse capture chord switches capture and does not reach the PC`() {
        val meta = KeyEvent.META_CTRL_ON or KeyEvent.META_ALT_ON or KeyEvent.META_SHIFT_ON
        assertTrue(keys.forward(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_M, 50, meta)))
        assertEquals(1, chords)
        assertFalse(sent.any { it.code == 50 })
    }

    @Test
    fun `an unused key follows the chord so Windows does not switch language`() {
        val meta = KeyEvent.META_CTRL_ON or KeyEvent.META_ALT_ON or KeyEvent.META_SHIFT_ON
        keys.forward(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_M, 50, meta))
        assertEquals(listOf(Sent(true, 0x76), Sent(false, 0x76)), sent)
    }

    @Test
    fun `a held chord switches capture once`() {
        val meta = KeyEvent.META_CTRL_ON or KeyEvent.META_ALT_ON or KeyEvent.META_SHIFT_ON
        keys.forward(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_M, 50, meta))
        keys.forward(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_M, 50, meta, repeat = 1))
        assertEquals(1, chords)
    }

    @Test
    fun `keys from the shortcut service repeat by themselves`() {
        keys.forward(down(30), fromService = true)
        sent.clear()
        idle(repeatStart + repeatStep * 2 + 1)
        assertTrue("repeats: ${sent.size}", sent.size >= 2)
        assertTrue(sent.all { it == Sent(true, 0x1E) })
    }

    @Test
    fun `releasing the key ends the repeat`() {
        keys.forward(down(30), fromService = true)
        keys.forward(up(30), fromService = true)
        sent.clear()
        idle(repeatStart * 3)
        assertTrue(sent.isEmpty())
    }

    @Test
    fun `modifiers held through the service do not repeat`() {
        keys.forward(down(29, KeyEvent.KEYCODE_CTRL_LEFT), fromService = true)
        sent.clear()
        idle(repeatStart * 3)
        assertTrue(sent.isEmpty())
    }

    @Test
    fun `keys that Android repeats itself are not repeated here`() {
        keys.forward(down(30))
        sent.clear()
        idle(repeatStart * 3)
        assertTrue(sent.isEmpty())
    }

    @Test
    fun `losing focus stops the repeat`() {
        keys.forward(down(30), fromService = true)
        keys.releaseAll()
        sent.clear()
        idle(repeatStart * 3)
        assertTrue(sent.isEmpty())
    }
}
