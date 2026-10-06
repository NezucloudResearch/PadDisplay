// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import kotlin.math.abs
import kotlin.math.atan
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.tan

/**
 * Turns MotionEvents on the video view into protocol messages.
 * - Stylus: always sent as pen (pressure, tilt, hover, barrel button, eraser).
 * - Fingers: native multi-touch, or (mouseMode) mouse gestures: tap = click, drag = left-drag,
 *   long-press / two-finger tap = right click, two-finger drag = scroll.
 * Fingers are ignored while the pen is near the screen (palm rejection).
 * - A mouse connected to the tablet (deviceInput): the PC pointer follows it over the picture, or,
 *   while the activity holds pointer capture, its raw movement is sent for games.
 */
class InputCapture(private val view: View, private val send: (ByteArray) -> Unit) {

    var mouseMode = false
    var deviceInput = true

    private val handler = Handler(Looper.getMainLooper())
    private val slop = ViewConfiguration.get(view.context).scaledTouchSlop.toFloat()
    private var lastPenMs = 0L
    private var penInRange = false
    private val penLeave = Runnable {
        if (penInRange) sendPen(Protocol.PEN_LEAVE, lastPenX, lastPenY, 0f, 0f, 0f, 0)
        penInRange = false
    }
    private var lastPenX = 0f
    private var lastPenY = 0f

    fun onTouch(ev: MotionEvent): Boolean {
        if (ev.actionMasked == MotionEvent.ACTION_DOWN) view.requestUnbufferedDispatch(ev)
        if (isPen(ev)) return handlePen(ev)
        if (isMouse(ev)) return handlePointer(ev)
        if (SystemClock.uptimeMillis() - lastPenMs < 400 || penInRange) return true // palm rejection
        if (mouseMode) handleMouse(ev) else handleTouch(ev)
        return true
    }

    fun onHover(ev: MotionEvent): Boolean = if (isPen(ev)) handlePen(ev) else isMouse(ev) && handlePointer(ev)

    /** Mouse wheel and button changes. */
    fun onGenericMotion(ev: MotionEvent): Boolean = !isPen(ev) && isMouse(ev) && handlePointer(ev)

    private fun isPen(ev: MotionEvent): Boolean {
        val t = ev.getToolType(0)
        return t == MotionEvent.TOOL_TYPE_STYLUS || t == MotionEvent.TOOL_TYPE_ERASER
    }

    private fun nx(x: Float) = (x / view.width * Protocol.COORD_MAX).toInt().coerceIn(0, Protocol.COORD_MAX)
    private fun ny(y: Float) = (y / view.height * Protocol.COORD_MAX).toInt().coerceIn(0, Protocol.COORD_MAX)

    // ---- pen ------------------------------------------------------------------------------

    private fun handlePen(ev: MotionEvent): Boolean {
        lastPenMs = SystemClock.uptimeMillis()
        var buttons = 0
        if (ev.buttonState and (MotionEvent.BUTTON_STYLUS_PRIMARY or MotionEvent.BUTTON_SECONDARY) != 0) buttons = buttons or Protocol.PEN_BARREL
        if (ev.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER) buttons = buttons or Protocol.PEN_ERASER

        fun emit(kind: Int, history: Boolean) {
            if (history) for (h in 0 until ev.historySize) {
                sendPen(kind, ev.getHistoricalX(0, h), ev.getHistoricalY(0, h), ev.getHistoricalPressure(0, h),
                    ev.getHistoricalAxisValue(MotionEvent.AXIS_TILT, 0, h),
                    ev.getHistoricalAxisValue(MotionEvent.AXIS_ORIENTATION, 0, h), buttons)
            }
            sendPen(kind, ev.getX(0), ev.getY(0), ev.getPressure(0), ev.getAxisValue(MotionEvent.AXIS_TILT, 0),
                ev.getAxisValue(MotionEvent.AXIS_ORIENTATION, 0), buttons)
        }

        when (ev.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE -> {
                handler.removeCallbacks(penLeave)
                penInRange = true
                emit(Protocol.PEN_HOVER, true)
            }
            // Android ends hover right before the tip touches; delay the leave so it is skipped then.
            MotionEvent.ACTION_HOVER_EXIT -> handler.postDelayed(penLeave, 80)
            MotionEvent.ACTION_DOWN -> {
                handler.removeCallbacks(penLeave)
                penInRange = true
                emit(Protocol.PEN_DOWN, false)
            }
            MotionEvent.ACTION_MOVE -> emit(Protocol.PEN_MOVE, true)
            MotionEvent.ACTION_UP -> {
                emit(Protocol.PEN_UP, false)
                handler.postDelayed(penLeave, 150) // cancelled if hover continues
            }
            MotionEvent.ACTION_CANCEL -> {
                emit(Protocol.PEN_UP, false)
                handler.post(penLeave)
            }
        }
        return true
    }

    private fun sendPen(kind: Int, x: Float, y: Float, pressure: Float, tilt: Float, orientation: Float, buttons: Int) {
        lastPenX = x
        lastPenY = y
        // Android: tilt = angle from perpendicular, orientation = direction of the lean (0 = up, +pi/2 = right).
        // Windows: tiltX positive leans right, tiltY positive leans toward the user (down).
        val t = tan(tilt.toDouble().coerceIn(0.0, 1.5))
        val tiltX = Math.toDegrees(atan(t * sin(orientation.toDouble()))).toInt().coerceIn(-90, 90)
        val tiltY = Math.toDegrees(atan(-t * cos(orientation.toDouble()))).toInt().coerceIn(-90, 90)
        val p = if (kind == Protocol.PEN_DOWN || kind == Protocol.PEN_MOVE)
            (pressure.coerceIn(0f, 1f) * Protocol.PRESSURE_MAX).toInt().coerceAtLeast(1) else 0
        send(Protocol.message(Protocol.PEN, 10) {
            it.put(kind.toByte())
            it.put(buttons.toByte())
            it.putShort(nx(x).toShort())
            it.putShort(ny(y).toShort())
            it.putShort(p.toShort())
            it.put(tiltX.toByte())
            it.put(tiltY.toByte())
        })
    }

    // ---- native multi-touch ---------------------------------------------------------------

    private fun handleTouch(ev: MotionEvent) {
        val action = ev.actionMasked
        if (action == MotionEvent.ACTION_MOVE) {
            for (h in 0 until ev.historySize) sendTouchFrame(ev, h)
        }
        sendTouchFrame(ev, -1)
    }

    private fun touchKind(ev: MotionEvent, i: Int): Int = when (ev.actionMasked) {
        MotionEvent.ACTION_DOWN -> Protocol.TOUCH_DOWN
        MotionEvent.ACTION_POINTER_DOWN -> if (i == ev.actionIndex) Protocol.TOUCH_DOWN else Protocol.TOUCH_MOVE
        MotionEvent.ACTION_UP -> Protocol.TOUCH_UP
        MotionEvent.ACTION_POINTER_UP -> if (i == ev.actionIndex) Protocol.TOUCH_UP else Protocol.TOUCH_MOVE
        MotionEvent.ACTION_CANCEL -> Protocol.TOUCH_CANCEL
        else -> Protocol.TOUCH_MOVE
    }

    /** history < 0 = current sample. */
    private fun sendTouchFrame(ev: MotionEvent, history: Int) {
        val n = minOf(ev.pointerCount, 10)
        val size = maxOf(view.width, 1).toFloat()
        send(Protocol.message(Protocol.TOUCH, 1 + n * 10) {
            it.put(n.toByte())
            for (i in 0 until n) {
                val x = if (history < 0) ev.getX(i) else ev.getHistoricalX(i, history)
                val y = if (history < 0) ev.getY(i) else ev.getHistoricalY(i, history)
                val pr = if (history < 0) ev.getPressure(i) else ev.getHistoricalPressure(i, history)
                val major = if (history < 0) ev.getTouchMajor(i) else ev.getHistoricalTouchMajor(i, history)
                it.put(ev.getPointerId(i).toByte())
                it.put((if (history < 0) touchKind(ev, i) else Protocol.TOUCH_MOVE).toByte())
                it.putShort(nx(x).toShort())
                it.putShort(ny(y).toShort())
                it.putShort((pr.coerceIn(0f, 1f) * Protocol.PRESSURE_MAX).toInt().toShort())
                it.putShort((major / size * Protocol.COORD_MAX).toInt().coerceIn(0, Protocol.COORD_MAX).toShort())
            }
        })
    }

    // ---- touch as mouse -------------------------------------------------------------------

    private enum class State { IDLE, PENDING, DRAGGING, TWO_FINGER, DONE }

    private var state = State.IDLE
    private var downX = 0f
    private var downY = 0f
    private var twoStartMs = 0L
    private var twoMoved = false
    private var lastCx = 0f
    private var lastCy = 0f
    private var scrollAccX = 0f
    private var scrollAccY = 0f
    private var lastTapMs = 0L
    private var lastTapX = 0f
    private var lastTapY = 0f

    private val longPress = Runnable {
        if (state == State.PENDING) {
            rightClick(downX, downY)
            state = State.DONE
        }
    }

    private fun mouse(kind: Int, x: Float, y: Float, wheelV: Int = 0, wheelH: Int = 0) {
        send(Protocol.message(Protocol.MOUSE, 9) {
            it.put(kind.toByte())
            it.putShort(nx(x).toShort())
            it.putShort(ny(y).toShort())
            it.putShort(wheelV.coerceIn(-32768, 32767).toShort())
            it.putShort(wheelH.coerceIn(-32768, 32767).toShort())
        })
    }

    private fun rightClick(x: Float, y: Float) {
        mouse(Protocol.MOUSE_MOVE, x, y)
        mouse(Protocol.MOUSE_RIGHT_DOWN, x, y)
        mouse(Protocol.MOUSE_RIGHT_UP, x, y)
    }

    private fun centroid(ev: MotionEvent): Pair<Float, Float> {
        var sx = 0f
        var sy = 0f
        for (i in 0 until ev.pointerCount) { sx += ev.getX(i); sy += ev.getY(i) }
        return sx / ev.pointerCount to sy / ev.pointerCount
    }

    private fun handleMouse(ev: MotionEvent) {
        val x = ev.x
        val y = ev.y
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                state = State.PENDING
                downX = x
                downY = y
                // Snap a quick second tap onto the first so Windows sees a double-click.
                if (SystemClock.uptimeMillis() - lastTapMs < 400 && hypot(x - lastTapX, y - lastTapY) < slop * 4) {
                    downX = lastTapX
                    downY = lastTapY
                }
                mouse(Protocol.MOUSE_MOVE, downX, downY)
                handler.postDelayed(longPress, ViewConfiguration.getLongPressTimeout().toLong())
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                handler.removeCallbacks(longPress)
                if (state == State.DRAGGING) mouse(Protocol.MOUSE_LEFT_UP, x, y)
                if (state != State.DONE) {
                    state = State.TWO_FINGER
                    twoStartMs = SystemClock.uptimeMillis()
                    twoMoved = false
                    val (cx, cy) = centroid(ev)
                    lastCx = cx
                    lastCy = cy
                    scrollAccX = 0f
                    scrollAccY = 0f
                }
            }
            MotionEvent.ACTION_MOVE -> when (state) {
                State.PENDING -> if (hypot(x - downX, y - downY) > slop) {
                    handler.removeCallbacks(longPress)
                    state = State.DRAGGING
                    mouse(Protocol.MOUSE_LEFT_DOWN, downX, downY)
                    mouse(Protocol.MOUSE_MOVE, x, y)
                }
                State.DRAGGING -> mouse(Protocol.MOUSE_MOVE, x, y)
                State.TWO_FINGER -> {
                    val (cx, cy) = centroid(ev)
                    // 40 px of finger travel = one wheel notch (120); natural (content follows finger) direction.
                    scrollAccY += (cy - lastCy) * 3f
                    scrollAccX += -(cx - lastCx) * 3f
                    lastCx = cx
                    lastCy = cy
                    if (abs(scrollAccY) + abs(scrollAccX) > slop * 3) twoMoved = true
                    val wv = scrollAccY.toInt()
                    val wh = scrollAccX.toInt()
                    if (wv != 0 || wh != 0) {
                        mouse(Protocol.MOUSE_WHEEL, cx, cy, wv, wh)
                        scrollAccY -= wv
                        scrollAccX -= wh
                    }
                }
                else -> {}
            }
            MotionEvent.ACTION_POINTER_UP -> if (state == State.TWO_FINGER) {
                if (!twoMoved && SystemClock.uptimeMillis() - twoStartMs < 300) {
                    val (cx, cy) = centroid(ev)
                    rightClick(cx, cy)
                }
                state = State.DONE
            }
            MotionEvent.ACTION_UP -> {
                handler.removeCallbacks(longPress)
                when (state) {
                    State.PENDING -> {
                        mouse(Protocol.MOUSE_LEFT_DOWN, downX, downY)
                        mouse(Protocol.MOUSE_LEFT_UP, downX, downY)
                        lastTapMs = SystemClock.uptimeMillis()
                        lastTapX = downX
                        lastTapY = downY
                    }
                    State.DRAGGING -> mouse(Protocol.MOUSE_LEFT_UP, x, y)
                    else -> {}
                }
                state = State.IDLE
            }
            MotionEvent.ACTION_CANCEL -> {
                handler.removeCallbacks(longPress)
                if (state == State.DRAGGING) mouse(Protocol.MOUSE_LEFT_UP, x, y)
                state = State.IDLE
            }
        }
    }

    // ---- mouse connected to the tablet ----------------------------------------------------

    // Android button -> the protocol's "down" kind; the "up" kind is the next value.
    private val mouseButtons = arrayOf(
        MotionEvent.BUTTON_PRIMARY to Protocol.MOUSE_LEFT_DOWN,
        MotionEvent.BUTTON_SECONDARY to Protocol.MOUSE_RIGHT_DOWN,
        MotionEvent.BUTTON_TERTIARY to Protocol.MOUSE_MIDDLE_DOWN,
        MotionEvent.BUTTON_BACK to Protocol.MOUSE_X1_DOWN,
        MotionEvent.BUTTON_FORWARD to Protocol.MOUSE_X2_DOWN,
    )
    private var buttonsDown = 0
    private var relAccX = 0f
    private var relAccY = 0f
    private var padX = 0f
    private var padY = 0f

    private fun isMouse(ev: MotionEvent) = deviceInput && ev.isFromSource(InputDevice.SOURCE_MOUSE)

    private fun mouseRel(kind: Int, dx: Int = 0, dy: Int = 0, wheelV: Int = 0, wheelH: Int = 0) {
        send(Protocol.message(Protocol.MOUSE_REL, 9) {
            it.put(kind.toByte())
            it.putShort(dx.coerceIn(-32768, 32767).toShort())
            it.putShort(dy.coerceIn(-32768, 32767).toShort())
            it.putShort(wheelV.coerceIn(-32768, 32767).toShort())
            it.putShort(wheelH.coerceIn(-32768, 32767).toShort())
        })
    }

    /** Sends the buttons that changed since the last event. Every mouse event carries all of them. */
    private fun syncButtons(state: Int, sendKind: (Int) -> Unit) {
        val changed = state xor buttonsDown
        if (changed == 0) return
        for ((button, downKind) in mouseButtons) {
            if (changed and button != 0) sendKind(if (state and button != 0) downKind else downKind + 1)
        }
        buttonsDown = state
    }

    // One wheel notch is 1.0 on Android and 120 on Windows.
    private fun wheel(ev: MotionEvent, axis: Int) = (ev.getAxisValue(axis) * 120).roundToInt()

    /** The mouse points at the picture: the PC pointer goes to the same place. */
    private fun handlePointer(ev: MotionEvent): Boolean {
        val x = ev.x
        val y = ev.y
        if (ev.actionMasked == MotionEvent.ACTION_SCROLL) {
            mouse(Protocol.MOUSE_WHEEL, x, y, wheel(ev, MotionEvent.AXIS_VSCROLL), wheel(ev, MotionEvent.AXIS_HSCROLL))
            return true
        }
        mouse(Protocol.MOUSE_MOVE, x, y)
        syncButtons(ev.buttonState) { mouse(it, x, y) }
        return true
    }

    /** With pointer capture (for games): raw movement, so the pointer is not bound to the picture. */
    fun onCapturedPointer(ev: MotionEvent): Boolean {
        if (ev.actionMasked == MotionEvent.ACTION_SCROLL) {
            mouseRel(Protocol.MOUSE_WHEEL, 0, 0, wheel(ev, MotionEvent.AXIS_VSCROLL), wheel(ev, MotionEvent.AXIS_HSCROLL))
            return true
        }
        if (ev.isFromSource(InputDevice.SOURCE_MOUSE_RELATIVE)) {
            if (ev.actionMasked == MotionEvent.ACTION_MOVE) {
                for (h in 0 until ev.historySize) {
                    relAccX += ev.getHistoricalX(h)
                    relAccY += ev.getHistoricalY(h)
                }
                relAccX += ev.x
                relAccY += ev.y
            }
        } else { // a captured touchpad reports where the finger is, not how far it moved
            if (ev.actionMasked == MotionEvent.ACTION_MOVE) {
                relAccX += ev.x - padX
                relAccY += ev.y - padY
            }
            padX = ev.x
            padY = ev.y
        }
        val dx = relAccX.toInt()
        val dy = relAccY.toInt()
        if (dx != 0 || dy != 0) {
            mouseRel(Protocol.MOUSE_MOVE, dx, dy)
            relAccX -= dx
            relAccY -= dy
        }
        syncButtons(ev.buttonState) { mouseRel(it) }
        return true
    }

    /** Capture was switched on or off: no button may stay held on the PC. */
    fun releaseMouseButtons() {
        syncButtons(0) { mouseRel(it) }
        relAccX = 0f
        relAccY = 0f
    }

    fun release() {
        handler.removeCallbacksAndMessages(null)
    }
}
