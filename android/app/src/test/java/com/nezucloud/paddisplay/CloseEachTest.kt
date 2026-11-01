// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class CloseEachTest {

    @Test
    fun `all of them are closed`() {
        val closed = ArrayList<String>()
        closeEach({ closed += "audio" }, { closed += "mic" }, { closed += "camera" })
        assertEquals(listOf("audio", "mic", "camera"), closed)
    }

    @Test
    fun `one that fails does not leave the others open`() {
        val closed = ArrayList<String>()
        closeEach({ closed += "audio" }, { throw IllegalStateException("mic") }, { closed += "camera" })
        assertEquals(listOf("audio", "camera"), closed)
    }
}
