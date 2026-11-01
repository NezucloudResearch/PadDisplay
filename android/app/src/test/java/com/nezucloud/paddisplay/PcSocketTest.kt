// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.net.ConnectException
import java.net.InetAddress
import java.net.ServerSocket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

class PcSocketTest {

    private val server = ServerSocket(0, 1, InetAddress.getLoopbackAddress())
    private val pc = PcSocket("127.0.0.1", server.localPort, null)

    @After
    fun closeServer() = server.close()

    @Test
    fun `over USB the connection is plain and has no send delay`() {
        val socket = pc.connect()
        assertTrue(socket.isConnected)
        assertTrue(socket.tcpNoDelay)
    }

    @Test
    fun `options are set before the connect`() {
        var connectedWhenTuned = true
        pc.connect { connectedWhenTuned = it.isConnected }
        assertFalse(connectedWhenTuned)
    }

    @Test
    fun `bytes reach the PC`() {
        val socket = pc.connect()
        val accepted = server.accept()
        socket.getOutputStream().write(42)
        socket.getOutputStream().flush()
        assertEquals(42, accepted.getInputStream().read())
    }

    @Test
    fun `hanging up wakes a thread blocked reading`() {
        val socket = pc.connect()
        val ended = CountDownLatch(1)
        thread {
            try {
                socket.getInputStream().read()
            } catch (_: java.io.IOException) {
            }
            ended.countDown()
        }
        Thread.sleep(100) // the reader is inside read() now
        pc.hangUp()
        assertTrue("the reader is still blocked", ended.await(2, TimeUnit.SECONDS))
    }

    @Test
    fun `closing an unused socket does nothing harmful`() {
        pc.closeQuietly()
        pc.hangUp()
    }

    @Test
    fun `closing twice is safe`() {
        pc.connect()
        pc.closeQuietly()
        pc.closeQuietly()
    }

    @Test(expected = ConnectException::class)
    fun `no PC listening is reported to the caller`() {
        val port = server.localPort
        server.close()
        PcSocket("127.0.0.1", port, null).connect()
    }
}
