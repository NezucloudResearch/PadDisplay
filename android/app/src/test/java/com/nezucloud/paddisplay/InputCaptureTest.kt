// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.os.Looper
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.MotionEvent.PointerCoords
import android.view.MotionEvent.PointerProperties
import android.view.View
import android.view.ViewConfiguration
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.time.Duration

@RunWith(RobolectricTestRunner::class)
class InputCaptureTest {

    private val sent = ArrayList<ByteArray>()
    private lateinit var capture: InputCapture

    @Before
    fun build() {
        val view = View(RuntimeEnvironment.getApplication())
        view.layout(0, 0, 1000, 1000)
        capture = InputCapture(view) { sent += it }
        idle(1000) // past the palm rejection: this clock starts near zero, a real uptime does not
    }

    private fun type(message: ByteArray) = message[0].toInt() and 0xFF
    private fun body(message: ByteArray) = ByteBuffer.wrap(message, Protocol.HEADER_SIZE, message.size - Protocol.HEADER_SIZE).slice().order(ByteOrder.LITTLE_ENDIAN)
    private fun mouseKinds() = sent.filter { type(it) == Protocol.MOUSE }.map { body(it).get().toInt() }

    private fun finger(id: Int, x: Float, y: Float) = id to (x to y)

    private fun event(action: Int, fingers: List<Pair<Int, Pair<Float, Float>>>, tool: Int = MotionEvent.TOOL_TYPE_FINGER): MotionEvent {
        val properties = Array(fingers.size) { i -> PointerProperties().apply { id = fingers[i].first; toolType = tool } }
        val coords = Array(fingers.size) { i -> PointerCoords().apply { x = fingers[i].second.first; y = fingers[i].second.second; pressure = 0.5f; size = 0.1f } }
        val now = SystemClock.uptimeMillis()
        return MotionEvent.obtain(now, now, action, fingers.size, properties, coords, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
    }

    private fun touch(action: Int, x: Float, y: Float) = capture.onTouch(event(action, listOf(finger(0, x, y))))

    private fun secondFinger(action: Int) = capture.onTouch(event(action or (1 shl MotionEvent.ACTION_POINTER_INDEX_SHIFT), listOf(finger(0, 100f, 100f), finger(1, 200f, 100f))))

    private fun idle(ms: Long) = shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms))

    private fun mouseMode() {
        capture.mouseMode = true
    }

    private val longPress get() = ViewConfiguration.getLongPressTimeout().toLong()

    // ---- touch as mouse

    @Test
    fun `a tap is a left click where the finger was`() {
        mouseMode()
        touch(MotionEvent.ACTION_DOWN, 500f, 500f)
        touch(MotionEvent.ACTION_UP, 500f, 500f)
        assertEquals(listOf(Protocol.MOUSE_MOVE, Protocol.MOUSE_LEFT_DOWN, Protocol.MOUSE_LEFT_UP), mouseKinds())
    }

    @Test
    fun `dragging holds the left button`() {
        mouseMode()
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        touch(MotionEvent.ACTION_MOVE, 300f, 100f)
        touch(MotionEvent.ACTION_MOVE, 400f, 100f)
        touch(MotionEvent.ACTION_UP, 400f, 100f)
        assertEquals(
            listOf(Protocol.MOUSE_MOVE, Protocol.MOUSE_LEFT_DOWN, Protocol.MOUSE_MOVE, Protocol.MOUSE_MOVE, Protocol.MOUSE_LEFT_UP),
            mouseKinds()
        )
    }

    @Test
    fun `a small slip is still a tap`() {
        mouseMode()
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        touch(MotionEvent.ACTION_MOVE, 101f, 100f)
        touch(MotionEvent.ACTION_UP, 101f, 100f)
        assertEquals(listOf(Protocol.MOUSE_MOVE, Protocol.MOUSE_LEFT_DOWN, Protocol.MOUSE_LEFT_UP), mouseKinds())
    }

    @Test
    fun `a long press is a right click`() {
        mouseMode()
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        idle(longPress + 1)
        touch(MotionEvent.ACTION_UP, 100f, 100f)
        assertEquals(listOf(Protocol.MOUSE_MOVE, Protocol.MOUSE_MOVE, Protocol.MOUSE_RIGHT_DOWN, Protocol.MOUSE_RIGHT_UP), mouseKinds())
    }

    @Test
    fun `a drag cancels the long press`() {
        mouseMode()
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        touch(MotionEvent.ACTION_MOVE, 300f, 100f)
        idle(longPress + 1)
        assertTrue(Protocol.MOUSE_RIGHT_DOWN !in mouseKinds())
    }

    @Test
    fun `a quick two-finger tap is a right click`() {
        mouseMode()
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        secondFinger(MotionEvent.ACTION_POINTER_DOWN)
        secondFinger(MotionEvent.ACTION_POINTER_UP)
        touch(MotionEvent.ACTION_UP, 100f, 100f)
        assertEquals(listOf(Protocol.MOUSE_MOVE, Protocol.MOUSE_MOVE, Protocol.MOUSE_RIGHT_DOWN, Protocol.MOUSE_RIGHT_UP), mouseKinds())
    }

    @Test
    fun `two fingers moving scroll`() {
        mouseMode()
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        secondFinger(MotionEvent.ACTION_POINTER_DOWN)
        capture.onTouch(event(MotionEvent.ACTION_MOVE, listOf(finger(0, 100f, 140f), finger(1, 200f, 140f))))
        val wheel = sent.first { type(it) == Protocol.MOUSE && body(it).get().toInt() == Protocol.MOUSE_WHEEL }
        assertEquals(120, body(wheel).getShort(5).toInt()) // 40 px of finger travel = one notch
    }

    @Test
    fun `scrolling is not a right click`() {
        mouseMode()
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        secondFinger(MotionEvent.ACTION_POINTER_DOWN)
        capture.onTouch(event(MotionEvent.ACTION_MOVE, listOf(finger(0, 100f, 400f), finger(1, 200f, 400f))))
        secondFinger(MotionEvent.ACTION_POINTER_UP)
        assertTrue(Protocol.MOUSE_RIGHT_DOWN !in mouseKinds())
    }

    @Test
    fun `a second finger during a drag lets the button go`() {
        mouseMode()
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        touch(MotionEvent.ACTION_MOVE, 300f, 100f)
        secondFinger(MotionEvent.ACTION_POINTER_DOWN)
        assertEquals(Protocol.MOUSE_LEFT_UP, mouseKinds().last())
    }

    @Test
    fun `a cancelled drag lets the button go`() {
        mouseMode()
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        touch(MotionEvent.ACTION_MOVE, 300f, 100f)
        touch(MotionEvent.ACTION_CANCEL, 300f, 100f)
        assertEquals(Protocol.MOUSE_LEFT_UP, mouseKinds().last())
    }

    @Test
    fun `a quick second tap lands on the first so Windows sees a double-click`() {
        mouseMode()
        touch(MotionEvent.ACTION_DOWN, 500f, 500f)
        touch(MotionEvent.ACTION_UP, 500f, 500f)
        sent.clear()
        touch(MotionEvent.ACTION_DOWN, 503f, 502f)
        val first = body(sent.first())
        assertEquals((0.5f * Protocol.COORD_MAX).toInt(), first.getShort(1).toInt() and 0xFFFF)
    }

    // ---- real multi-touch

    @Test
    fun `a finger down is a touch frame with its place`() {
        touch(MotionEvent.ACTION_DOWN, 500f, 250f)
        val frame = body(sent.single())
        assertEquals(Protocol.TOUCH, type(sent.single()))
        assertEquals(1, frame.get().toInt())
        assertEquals(0, frame.get().toInt())
        assertEquals(Protocol.TOUCH_DOWN, frame.get().toInt())
        assertEquals((0.5f * Protocol.COORD_MAX).toInt(), frame.short.toInt() and 0xFFFF)
        assertEquals((0.25f * Protocol.COORD_MAX).toInt(), frame.short.toInt() and 0xFFFF)
    }

    @Test
    fun `only the finger that came down is marked down`() {
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        sent.clear()
        secondFinger(MotionEvent.ACTION_POINTER_DOWN)
        val frame = body(sent.single())
        assertEquals(2, frame.get().toInt())
        assertEquals(Protocol.TOUCH_MOVE, frame.get(2).toInt())
        assertEquals(Protocol.TOUCH_DOWN, frame.get(12).toInt())
    }

    @Test
    fun `lifting and cancelling are told apart`() {
        touch(MotionEvent.ACTION_UP, 100f, 100f)
        touch(MotionEvent.ACTION_CANCEL, 100f, 100f)
        assertEquals(Protocol.TOUCH_UP, body(sent[0]).get(2).toInt())
        assertEquals(Protocol.TOUCH_CANCEL, body(sent[1]).get(2).toInt())
    }

    @Test
    fun `places outside the picture are held at its edge`() {
        touch(MotionEvent.ACTION_DOWN, 5000f, -50f)
        val frame = body(sent.single())
        assertEquals(Protocol.COORD_MAX, frame.getShort(3).toInt() and 0xFFFF)
        assertEquals(0, frame.getShort(5).toInt() and 0xFFFF)
    }

    // ---- the pen

    private fun pen(action: Int, x: Float = 100f, y: Float = 100f) =
        capture.onTouch(event(action, listOf(finger(0, x, y)), MotionEvent.TOOL_TYPE_STYLUS))

    @Test
    fun `a stylus is sent as a pen, also in mouse mode`() {
        mouseMode()
        pen(MotionEvent.ACTION_DOWN)
        assertEquals(Protocol.PEN, type(sent.single()))
        assertEquals(Protocol.PEN_DOWN, body(sent.single()).get().toInt())
    }

    @Test
    fun `the pen's pressure is never zero while it touches`() {
        capture.onTouch(event(MotionEvent.ACTION_DOWN, listOf(finger(0, 1f, 1f)), MotionEvent.TOOL_TYPE_STYLUS).also { it.setAction(MotionEvent.ACTION_DOWN) })
        assertTrue(body(sent.single()).getShort(6) > 0)
    }

    @Test
    fun `a palm right after the pen is ignored`() {
        pen(MotionEvent.ACTION_DOWN)
        pen(MotionEvent.ACTION_UP)
        sent.clear()
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        assertTrue(sent.isEmpty())
    }

    @Test
    fun `fingers work again once the pen has gone`() {
        pen(MotionEvent.ACTION_DOWN)
        pen(MotionEvent.ACTION_UP)
        idle(500)
        sent.clear()
        touch(MotionEvent.ACTION_DOWN, 100f, 100f)
        assertEquals(Protocol.TOUCH, type(sent.single()))
    }

    @Test
    fun `the pen leaves after it lifts`() {
        pen(MotionEvent.ACTION_DOWN)
        pen(MotionEvent.ACTION_UP)
        sent.clear()
        idle(200)
        assertEquals(Protocol.PEN_LEAVE, body(sent.single()).get().toInt())
    }
}
