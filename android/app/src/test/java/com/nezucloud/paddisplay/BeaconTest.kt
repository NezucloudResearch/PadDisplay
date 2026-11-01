// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class BeaconTest {

    private fun parse(message: String) = Discovery.parseBeacon(message, "192.168.1.9", 1234)

    @Test
    fun `a beacon names the PC, its address and port`() {
        assertEquals(Discovery.Host("LING", "192.168.1.9", 27183, 1234), parse("PADDISPLAY 5 27183 LING"))
    }

    @Test
    fun `a PC name may hold spaces`() {
        assertEquals("My Gaming PC", parse("PADDISPLAY 5 27183 My Gaming PC")?.name)
    }

    @Test
    fun `surrounding white space does not matter`() {
        assertEquals("LING", parse("  PADDISPLAY 5 27183 LING\n")?.name)
    }

    @Test
    fun `other broadcasts are ignored`() {
        assertNull(parse("SOMETHING 5 27183 LING"))
    }

    @Test
    fun `a beacon without a name is ignored`() {
        assertNull(parse("PADDISPLAY 5 27183"))
    }

    @Test
    fun `a beacon with a bad port is ignored`() {
        assertNull(parse("PADDISPLAY 5 port LING"))
    }
}
