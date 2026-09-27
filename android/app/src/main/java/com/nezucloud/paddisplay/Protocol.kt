// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Wire protocol shared with the Windows host (see docs/PROTOCOL.md). Little-endian. */
object Protocol {
    const val VERSION = 2 // v2: client acks every frame (host flow control)
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
    const val AUDIO_MUTE_PC = 1

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

    fun clientHello(screenW: Int, screenH: Int, dpi: Int, codecMask: Int, pin: Int, maxFps: Int, name: String): ByteArray {
        val nameBytes = name.toByteArray(Charsets.UTF_8).take(63).toByteArray()
        return message(CLIENT_HELLO, 16 + nameBytes.size) {
            it.putShort(VERSION.toShort())
            it.putShort(screenW.toShort())
            it.putShort(screenH.toShort())
            it.putShort(dpi.toShort())
            it.put(codecMask.toByte())
            it.put(10)
            it.putInt(pin)
            it.putShort(maxFps.toShort())
            it.put(nameBytes)
        }
    }
}
