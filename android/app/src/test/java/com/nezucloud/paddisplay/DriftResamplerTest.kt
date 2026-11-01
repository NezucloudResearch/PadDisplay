// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

class DriftResamplerTest {

    private val resampler = DriftResampler()

    /** A rising ramp, the same on both channels: any seam or lost frame shows as a jump. */
    private fun ramp(frames: Int, start: Int = 0) = ShortArray(frames * 2) { ((start + it / 2) * 10).toShort() }

    private fun left(out: ShortArray, frames: Int) = (0 until frames).map { out[it * 2].toInt() }

    @Test
    fun `at ratio 1 the output is a bit-exact copy`() {
        val input = ramp(100)
        val out = ShortArray(400)
        assertEquals(100, resampler.process(input, 100, 1.0, out))
        assertArrayEquals(input, out.copyOf(200))
    }

    @Test
    fun `running faster gives fewer frames`() {
        val produced = resampler.process(ramp(1000), 1000, 1.02, ShortArray(4000))
        assertEquals(1000 / 1.02, produced.toDouble(), 2.0)
    }

    @Test
    fun `running slower gives more frames`() {
        val produced = resampler.process(ramp(1000), 1000, 0.99, ShortArray(4000))
        assertEquals(1000 / 0.99, produced.toDouble(), 2.0)
    }

    @Test
    fun `left and right stay apart`() {
        val input = ShortArray(20) { if (it % 2 == 0) 1000 else -1000 }
        val out = ShortArray(40)
        val produced = resampler.process(input, 10, 0.97, out)
        for (i in 0 until produced) {
            assertEquals(1000, out[i * 2].toInt())
            assertEquals(-1000, out[i * 2 + 1].toInt())
        }
    }

    @Test
    fun `the first frame of a packet follows the last frame of the one before`() {
        val a = ShortArray(400)
        val b = ShortArray(400)
        val firstCount = resampler.process(ramp(100), 100, 1.01, a)
        val secondCount = resampler.process(ramp(100, 100), 100, 1.01, b)
        val joined = left(a, firstCount) + left(b, secondCount)
        for (i in 1 until joined.size) assertTrue("jump at $i: ${joined[i - 1]} -> ${joined[i]}", abs(joined[i] - joined[i - 1]) <= 12)
    }

    // The clock drift is corrected by a fraction of a frame per packet, so that fraction has to carry over or the speed change is lost.
    @Test
    fun `a speed change holds over many packets`() {
        var produced = 0
        repeat(500) { produced += resampler.process(ramp(100), 100, 1.01, ShortArray(400)) }
        assertEquals(50_000 / 1.01, produced.toDouble(), 2.0)
    }

    @Test
    fun `a slow-down holds over many packets`() {
        var produced = 0
        repeat(500) { produced += resampler.process(ramp(100), 100, 0.99, ShortArray(400)) }
        assertEquals(50_000 / 0.99, produced.toDouble(), 2.0)
    }

    @Test
    fun `a full output buffer stops it safely`() {
        assertEquals(10, resampler.process(ramp(100), 100, 1.0, ShortArray(20)))
    }

    @Test
    fun `reset forgets the previous packet`() {
        resampler.process(ramp(100), 100, 1.01, ShortArray(400))
        resampler.reset()
        val out = ShortArray(400)
        resampler.process(ramp(10), 10, 1.0, out)
        assertEquals(0, out[0].toInt())
    }
}
