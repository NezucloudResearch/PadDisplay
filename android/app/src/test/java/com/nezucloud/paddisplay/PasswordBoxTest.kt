// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.app.Activity
import android.os.Looper
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.TextView
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.shadows.ShadowDialog
import java.time.Duration

@RunWith(RobolectricTestRunner::class)
class PasswordBoxTest {

    private val sent = ArrayList<String>()
    private lateinit var box: PasswordBox

    @Before
    fun build() {
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        activity.setTheme(android.R.style.Theme_DeviceDefault)
        val anchor = View(activity)
        activity.setContentView(anchor)
        box = PasswordBox(activity, anchor) { sent += it }
    }

    private fun views(root: View): List<View> = if (root is ViewGroup) listOf(root) + (0 until root.childCount).flatMap { views(root.getChildAt(it)) } else listOf(root)
    private fun shown() = views(ShadowDialog.getLatestDialog().window!!.decorView)
    private fun field() = shown().filterIsInstance<EditText>().single()
    private fun press(label: String) = shown().filterIsInstance<TextView>().first { it.text.toString() == label }.performClick()
    private fun texts() = shown().filterIsInstance<TextView>().filter { it.visibility == View.VISIBLE && it !is EditText }.map { it.text.toString() }
    private fun idle(ms: Long) = shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms))

    @Test
    fun `it starts closed`() {
        assertFalse(box.isOpen)
    }

    @Test
    fun `showing it takes the keys from the PC`() {
        box.show()
        assertTrue(box.isOpen)
    }

    @Test
    fun `showing it twice keeps one box`() {
        box.show()
        val first = ShadowDialog.getLatestDialog()
        box.show()
        assertTrue(first === ShadowDialog.getLatestDialog())
    }

    @Test
    fun `an empty password is not sent`() {
        box.show()
        press("Send")
        assertTrue(sent.isEmpty())
    }

    @Test
    fun `the typed password goes to the PC once`() {
        box.show()
        field().setText("secret")
        press("Send")
        assertEquals(listOf("secret"), sent)
    }

    @Test
    fun `no second send while the PC verifies`() {
        box.show()
        field().setText("secret")
        press("Send")
        assertFalse(field().isEnabled)
    }

    @Test
    fun `no unlock in time asks again and wipes the field`() {
        box.show()
        field().setText("wrong")
        press("Send")
        idle(8001)
        assertTrue(field().isEnabled)
        assertEquals("", field().text.toString())
        assertTrue(texts().any { it.startsWith("That didn't work") })
    }

    @Test
    fun `an unlock closes it and hands the keys back`() {
        box.show()
        field().setText("secret")
        press("Send")
        box.dismiss()
        idle(1)
        assertFalse(box.isOpen)
    }

    @Test
    fun `closing wipes what was typed`() {
        box.show()
        val typed = field()
        typed.setText("secret")
        box.dismiss()
        idle(1)
        assertEquals("", typed.text.toString())
    }

    @Test
    fun `cancel closes it without sending`() {
        box.show()
        field().setText("secret")
        press("Cancel")
        idle(1)
        assertFalse(box.isOpen)
        assertTrue(sent.isEmpty())
    }

    @Test
    fun `dismissing a closed box does nothing`() {
        box.dismiss()
        assertFalse(box.isOpen)
    }
}
