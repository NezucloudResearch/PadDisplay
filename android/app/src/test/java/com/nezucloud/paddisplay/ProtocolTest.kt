// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

class ProtocolTest {

    private fun payload(message: ByteArray): ByteBuffer =
        ByteBuffer.wrap(message, Protocol.HEADER_SIZE, message.size - Protocol.HEADER_SIZE).slice().order(ByteOrder.LITTLE_ENDIAN)

    @Test
    fun `a message starts with its type and a little-endian length`() {
        val message = Protocol.message(Protocol.KEY, 3) { it.put(1); it.putShort(0x0102) }
        assertArrayEquals(byteArrayOf(0x8A.toByte(), 3, 0, 0, 0, 1, 2, 1), message)
    }

    @Test
    fun `an empty message is only its header`() {
        assertEquals(Protocol.HEADER_SIZE, Protocol.message(Protocol.BYE, 0).size)
    }

    @Test
    fun `the client hello has the layout the host reads`() {
        val message = Protocol.clientHello(2560, 1600, 328, Protocol.MASK_H264, 123456, 120, Protocol.HELLO_RECONNECTS, "tab")
        assertEquals(Protocol.CLIENT_HELLO, message[0].toInt() and 0xFF)
        val p = payload(message)
        assertEquals(Protocol.VERSION, p.short.toInt())
        assertEquals(2560, p.short.toInt())
        assertEquals(1600, p.short.toInt())
        assertEquals(328, p.short.toInt())
        assertEquals(Protocol.MASK_H264, p.get().toInt())
        assertEquals(Protocol.MAX_TOUCH_POINTS, p.get().toInt())
        assertEquals(123456, p.int)
        assertEquals(120, p.short.toInt())
        assertEquals(Protocol.HELLO_RECONNECTS, p.get().toInt())
        assertEquals("tab", ByteArray(p.remaining()).also { p.get(it) }.toString(Charsets.UTF_8))
    }

    @Test
    fun `a long device name is cut to what the host keeps`() {
        val message = Protocol.clientHello(1, 1, 1, 1, 1, 1, 0, "x".repeat(200))
        assertEquals(Protocol.HEADER_SIZE + 17 + 63, message.size)
    }

    @Test
    fun `secure text carries the submit flag and the text as utf-8`() {
        val message = Protocol.secureText("pä", true)
        val p = payload(message)
        assertEquals(Protocol.SECURE_TEXT_SUBMIT, p.get().toInt())
        assertEquals("pä", ByteArray(p.remaining()).also { p.get(it) }.toString(Charsets.UTF_8))
    }

    // CLAUDE.md: protocol.h and Protocol.kt change together. Tests run in android/app.
    private val header = File("../../host/src/protocol.h").readText()

    private fun toConstantName(camelCase: String) = camelCase.replace(Regex("([a-z0-9])([A-Z])"), "$1_$2").uppercase()

    private fun constant(name: String) = Protocol::class.java.getField(name).get(null) as Int

    @Test
    fun `every message type has the value the host gives it`() {
        val body = Regex("""enum class Msg : uint8_t\s*\{(.*?)\};""", RegexOption.DOT_MATCHES_ALL).find(header)!!.groupValues[1]
        val types = Regex("""^\s*(\w+)\s*=\s*0x([0-9A-Fa-f]+)""", RegexOption.MULTILINE).findAll(body).toList()
        assertTrue("found only ${types.size} message types in protocol.h", types.size >= 30)
        for (type in types) {
            val (name, value) = type.destructured
            assertEquals(name, value.toInt(16), constant(toConstantName(name)))
        }
    }

    @Test
    fun `the protocol version is the host's`() {
        val version = Regex("""kProtocolVersion = (\d+)""").find(header)!!.groupValues[1].toInt()
        assertEquals(version, Protocol.VERSION)
    }

    @Test
    fun `the ports and the header size are the host's`() {
        assertEquals(Regex("""kTcpPort = (\d+)""").find(header)!!.groupValues[1].toInt(), Protocol.TCP_PORT)
        assertEquals(Regex("""kDiscoveryPort = (\d+)""").find(header)!!.groupValues[1].toInt(), Protocol.DISCOVERY_PORT)
        assertEquals(Regex("""kHeaderSize = (\d+)""").find(header)!!.groupValues[1].toInt(), Protocol.HEADER_SIZE)
    }
}
