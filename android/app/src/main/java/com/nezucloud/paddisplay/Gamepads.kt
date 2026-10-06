// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.content.Context
import android.hardware.input.InputManager
import android.os.Build
import android.os.CombinedVibration
import android.os.Handler
import android.os.Looper
import android.os.VibrationEffect
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Game controllers connected to the tablet. Each one gets a slot (0..3) and is sent to the host
 * as a whole state in the XInput layout whenever it changes; the host shows it to Windows as an
 * Xbox 360 controller. Rumble from the PC is played on the controller.
 * A controller is announced as soon as it is connected, not at its first button: Windows needs
 * a moment to start the virtual controller, and a first press sent during that time is lost.
 * Call everything from the main thread.
 */
class Gamepads(context: Context, private val send: (ByteArray) -> Unit) : InputManager.InputDeviceListener {

    private class Pad(val index: Int) {
        var keys = 0 // buttons that arrive as key events
        var hat = 0  // d-pad that arrives as a hat axis
        var leftTriggerKey = false
        var rightTriggerKey = false
        var leftTrigger = 0
        var rightTrigger = 0
        var lx = 0
        var ly = 0
        var rx = 0
        var ry = 0
        var sent: ByteArray? = null
    }

    private val inputManager = context.getSystemService(Context.INPUT_SERVICE) as InputManager
    private val pads = HashMap<Int, Pad>() // by Android device id

    init {
        inputManager.registerInputDeviceListener(this, Handler(Looper.getMainLooper()))
    }

    private fun padFor(deviceId: Int): Pad? {
        pads[deviceId]?.let { return it }
        val index = (0 until Protocol.MAX_GAMEPADS).firstOrNull { i -> pads.values.none { it.index == i } } ?: return null
        return Pad(index).also { pads[deviceId] = it }
    }

    private fun button(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> Protocol.PAD_A
        KeyEvent.KEYCODE_BUTTON_B -> Protocol.PAD_B
        KeyEvent.KEYCODE_BUTTON_X -> Protocol.PAD_X
        KeyEvent.KEYCODE_BUTTON_Y -> Protocol.PAD_Y
        KeyEvent.KEYCODE_BUTTON_L1 -> Protocol.PAD_LEFT_SHOULDER
        KeyEvent.KEYCODE_BUTTON_R1 -> Protocol.PAD_RIGHT_SHOULDER
        KeyEvent.KEYCODE_BUTTON_THUMBL -> Protocol.PAD_LEFT_THUMB
        KeyEvent.KEYCODE_BUTTON_THUMBR -> Protocol.PAD_RIGHT_THUMB
        KeyEvent.KEYCODE_BUTTON_START -> Protocol.PAD_START
        KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BACK -> Protocol.PAD_BACK
        KeyEvent.KEYCODE_BUTTON_MODE -> Protocol.PAD_GUIDE
        KeyEvent.KEYCODE_DPAD_UP -> Protocol.PAD_DPAD_UP
        KeyEvent.KEYCODE_DPAD_DOWN -> Protocol.PAD_DPAD_DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> Protocol.PAD_DPAD_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> Protocol.PAD_DPAD_RIGHT
        else -> 0
    }

    /** Sticks and the usual buttons: not a remote control or a sensor that only claims to be a joystick. */
    private fun isController(dev: InputDevice) = !dev.isVirtual &&
        dev.supportsSource(InputDevice.SOURCE_GAMEPAD) && dev.supportsSource(InputDevice.SOURCE_JOYSTICK) &&
        dev.getMotionRange(MotionEvent.AXIS_X) != null && dev.hasKeys(KeyEvent.KEYCODE_BUTTON_A)[0]

    /**
     * Tells the host about every connected controller, with its current state. Call when the host
     * is ready (also after a reconnect: it has forgotten the controllers by then).
     */
    fun announce() {
        for (id in InputDevice.getDeviceIds()) {
            val dev = InputDevice.getDevice(id) ?: continue
            if (isController(dev)) padFor(id)
        }
        for (pad in pads.values) {
            pad.sent = null
            sendState(pad)
        }
    }

    /** true = the key belongs to a game controller and was used. */
    fun onKey(ev: KeyEvent): Boolean {
        if (!ev.isFromSource(InputDevice.SOURCE_GAMEPAD)) return false
        val down = ev.action == KeyEvent.ACTION_DOWN
        if (!down && ev.action != KeyEvent.ACTION_UP) return false
        val bit = button(ev.keyCode)
        val trigger = ev.keyCode == KeyEvent.KEYCODE_BUTTON_L2 || ev.keyCode == KeyEvent.KEYCODE_BUTTON_R2
        if (bit == 0 && !trigger) return false
        val pad = padFor(ev.deviceId) ?: return true
        when (ev.keyCode) {
            KeyEvent.KEYCODE_BUTTON_L2 -> pad.leftTriggerKey = down
            KeyEvent.KEYCODE_BUTTON_R2 -> pad.rightTriggerKey = down
            else -> pad.keys = if (down) pad.keys or bit else pad.keys and bit.inv()
        }
        sendState(pad)
        return true
    }

    /** true = the event is a controller's sticks, triggers or d-pad and was used. */
    fun onMotion(ev: MotionEvent): Boolean {
        if (!ev.isFromSource(InputDevice.SOURCE_JOYSTICK) || ev.actionMasked != MotionEvent.ACTION_MOVE) return false
        val pad = padFor(ev.deviceId) ?: return true
        val dev = ev.device
        // The right stick is Z/RZ on most controllers and RX/RY on some.
        val rightOnZ = dev?.getMotionRange(MotionEvent.AXIS_Z, ev.source) != null
        pad.lx = stick(ev, dev, MotionEvent.AXIS_X)
        pad.ly = -stick(ev, dev, MotionEvent.AXIS_Y) // Android: down is positive; XInput: up
        pad.rx = stick(ev, dev, if (rightOnZ) MotionEvent.AXIS_Z else MotionEvent.AXIS_RX)
        pad.ry = -stick(ev, dev, if (rightOnZ) MotionEvent.AXIS_RZ else MotionEvent.AXIS_RY)
        pad.leftTrigger = trigger(maxOf(ev.getAxisValue(MotionEvent.AXIS_LTRIGGER), ev.getAxisValue(MotionEvent.AXIS_BRAKE)))
        pad.rightTrigger = trigger(maxOf(ev.getAxisValue(MotionEvent.AXIS_RTRIGGER), ev.getAxisValue(MotionEvent.AXIS_GAS)))
        val hx = ev.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hy = ev.getAxisValue(MotionEvent.AXIS_HAT_Y)
        pad.hat = (if (hx < -0.5f) Protocol.PAD_DPAD_LEFT else if (hx > 0.5f) Protocol.PAD_DPAD_RIGHT else 0) or
            (if (hy < -0.5f) Protocol.PAD_DPAD_UP else if (hy > 0.5f) Protocol.PAD_DPAD_DOWN else 0)
        sendState(pad)
        return true
    }

    private fun stick(ev: MotionEvent, dev: InputDevice?, axis: Int): Int {
        val v = ev.getAxisValue(axis)
        val flat = dev?.getMotionRange(axis, ev.source)?.flat ?: 0f
        return if (abs(v) <= flat) 0 else (v.coerceIn(-1f, 1f) * 32767).roundToInt()
    }

    private fun trigger(v: Float) = (v.coerceIn(0f, 1f) * 255).roundToInt()

    private fun sendState(pad: Pad, connected: Boolean = true) {
        val msg = Protocol.message(Protocol.GAMEPAD, 14) {
            it.put(pad.index.toByte())
            it.put((if (connected) Protocol.GAMEPAD_CONNECTED else 0).toByte())
            it.putShort((pad.keys or pad.hat).toShort())
            it.put((if (pad.leftTriggerKey) 255 else pad.leftTrigger).toByte())
            it.put((if (pad.rightTriggerKey) 255 else pad.rightTrigger).toByte())
            it.putShort(pad.lx.toShort())
            it.putShort(pad.ly.toShort())
            it.putShort(pad.rx.toShort())
            it.putShort(pad.ry.toShort())
        }
        if (msg.contentEquals(pad.sent)) return
        pad.sent = msg
        send(msg)
    }

    /** Rumble asked for by a game on the PC: 0..255 for the large and the small motor, 0 = stop. */
    fun onRumble(index: Int, large: Int, small: Int) {
        val deviceId = pads.entries.firstOrNull { it.value.index == index }?.key ?: return
        val dev = InputDevice.getDevice(deviceId) ?: return
        // The level holds until the PC changes it, so the effect is long and gets cancelled.
        fun effect(level: Int) = VibrationEffect.createOneShot(60_000, level.coerceIn(1, 255))
        if (Build.VERSION.SDK_INT >= 31) {
            val manager = dev.vibratorManager
            val ids = manager.vibratorIds
            when {
                ids.isEmpty() -> {}
                large == 0 && small == 0 -> manager.cancel()
                ids.size == 1 -> manager.vibrate(CombinedVibration.createParallel(effect(maxOf(large, small))))
                else -> {
                    // Controllers list the strong (left) motor first.
                    manager.cancel()
                    val both = CombinedVibration.startParallel()
                    if (large > 0) both.addVibrator(ids[0], effect(large))
                    if (small > 0) both.addVibrator(ids[1], effect(small))
                    manager.vibrate(both.combine())
                }
            }
        } else {
            @Suppress("DEPRECATION")
            val vibrator = dev.vibrator
            if (!vibrator.hasVibrator()) return
            if (large == 0 && small == 0) vibrator.cancel() else vibrator.vibrate(effect(maxOf(large, small)))
        }
    }

    override fun onInputDeviceAdded(deviceId: Int) {
        val dev = InputDevice.getDevice(deviceId) ?: return
        if (isController(dev)) padFor(deviceId)?.let { sendState(it) }
    }

    override fun onInputDeviceChanged(deviceId: Int) {}

    override fun onInputDeviceRemoved(deviceId: Int) {
        val pad = pads.remove(deviceId) ?: return
        sendState(pad, connected = false)
    }

    fun release() {
        inputManager.unregisterInputDeviceListener(this)
        for (index in pads.values.map { it.index }) onRumble(index, 0, 0)
        pads.clear()
    }
}
