// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Wire protocol shared with the Windows host (see docs/PROTOCOL.md). Little-endian. */
object Protocol {
    const val VERSION = 3 // v2: client acks every frame (host flow control); v3: keyboard, relative mouse, game controllers
    const val TCP_PORT = 27183
    const val DISCOVERY_PORT = 27184
    const val HEADER_SIZE = 5

    // host -> client
    const val HOST_HELLO = 0x01
    const val CONFIG = 0x02
    const val FRAME = 0x03
    const val PING = 0x04
    const val ERROR = 0x05
    const val AUDIO_FORMAT = 0x06
    const val AUDIO_DATA = 0x07
    const val RUMBLE = 0x08
    const val NOTICE = 0x09

    // client -> host
    const val CLIENT_HELLO = 0x81
    const val TOUCH = 0x82
    const val PEN = 0x83
    const val MOUSE = 0x84
    const val KEYFRAME_REQ = 0x85
    const val PONG = 0x86
    const val FRAME_ACK = 0x87
    const val KEEPALIVE = 0x88
    const val AUDIO_HELLO = 0x89
    const val KEY = 0x8A
    const val MOUSE_REL = 0x8B
    const val GAMEPAD = 0x8C
    const val AUDIO_MUTE_PC = 1
    const val HELLO_GAMING_MODE = 1
    const val HELLO_ONLY_SCREEN = 2

    const val CODEC_H264 = 1
    const val CODEC_HEVC = 2
    const val MASK_H264 = 1
    const val MASK_HEVC = 2

    const val ERR_BAD_PIN = 1

    const val TOUCH_DOWN = 0
    const val TOUCH_MOVE = 1
    const val TOUCH_UP = 2
    const val TOUCH_CANCEL = 3

    const val PEN_HOVER = 0
    const val PEN_DOWN = 1
    const val PEN_MOVE = 2
    const val PEN_UP = 3
    const val PEN_LEAVE = 4
    const val PEN_BARREL = 1
    const val PEN_ERASER = 2

    const val MOUSE_MOVE = 0
    const val MOUSE_LEFT_DOWN = 1
    const val MOUSE_LEFT_UP = 2
    const val MOUSE_RIGHT_DOWN = 3
    const val MOUSE_RIGHT_UP = 4
    const val MOUSE_WHEEL = 5
    const val MOUSE_MIDDLE_DOWN = 6
    const val MOUSE_MIDDLE_UP = 7
    const val MOUSE_X1_DOWN = 8
    const val MOUSE_X1_UP = 9
    const val MOUSE_X2_DOWN = 10
    const val MOUSE_X2_UP = 11

    const val KEY_DOWN = 1
    const val KEY_EXTENDED = 0x100 // the scancode has the E0 prefix
    const val KEY_PAUSE = 0x145

    const val GAMEPAD_CONNECTED = 1
    const val MAX_GAMEPADS = 4

    // Game controller buttons: the XInput bit layout.
    const val PAD_DPAD_UP = 0x0001
    const val PAD_DPAD_DOWN = 0x0002
    const val PAD_DPAD_LEFT = 0x0004
    const val PAD_DPAD_RIGHT = 0x0008
    const val PAD_START = 0x0010
    const val PAD_BACK = 0x0020
    const val PAD_LEFT_THUMB = 0x0040
    const val PAD_RIGHT_THUMB = 0x0080
    const val PAD_LEFT_SHOULDER = 0x0100
    const val PAD_RIGHT_SHOULDER = 0x0200
    const val PAD_GUIDE = 0x0400
    const val PAD_A = 0x1000
    const val PAD_B = 0x2000
    const val PAD_X = 0x4000
    const val PAD_Y = 0x8000

    const val COORD_MAX = 65535
    const val PRESSURE_MAX = 1024

    /** Builds a complete message: header + payload written by [fill]. */
    inline fun message(type: Int, payloadSize: Int, fill: (ByteBuffer) -> Unit = {}): ByteArray {
        val bytes = ByteArray(HEADER_SIZE + payloadSize)
        val buf = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        buf.put(type.toByte())
        buf.putInt(payloadSize)
        fill(buf)
        return bytes
    }

    fun clientHello(screenW: Int, screenH: Int, dpi: Int, codecMask: Int, pin: Int, maxFps: Int, flags: Int, name: String): ByteArray {
        val nameBytes = name.toByteArray(Charsets.UTF_8).take(63).toByteArray()
        return message(CLIENT_HELLO, 17 + nameBytes.size) {
            it.putShort(VERSION.toShort())
            it.putShort(screenW.toShort())
            it.putShort(screenH.toShort())
            it.putShort(dpi.toShort())
            it.put(codecMask.toByte())
            it.put(10)
            it.putInt(pin)
            it.putShort(maxFps.toShort())
            it.put(flags.toByte())
            it.put(nameBytes)
        }
    }
}
