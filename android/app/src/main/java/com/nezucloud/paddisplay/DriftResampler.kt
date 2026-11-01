// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import kotlin.math.floor

/**
 * Stereo linear-interpolation resampler for tiny speed changes. Continuous across packets: the
 * last frame of the previous packet is kept so there is no seam. ratio 1.0 = bit-exact copy.
 */
internal class DriftResampler {
    private var pos = 0.0 // read position in the current packet, in frames; -1 = previous packet's last frame
    private var prevL = 0f
    private var prevR = 0f

    fun reset() {
        pos = 0.0
        prevL = 0f
        prevR = 0f
    }

    fun process(inp: ShortArray, frames: Int, ratio: Double, out: ShortArray): Int {
        var n = 0
        val max = out.size / 2
        while (pos <= frames - 1 && n < max) {
            val i0 = floor(pos).toInt() // -1 .. frames-1
            val frac = (pos - i0).toFloat()
            val l0 = if (i0 < 0) prevL else inp[2 * i0].toFloat()
            val r0 = if (i0 < 0) prevR else inp[2 * i0 + 1].toFloat()
            val i1 = i0 + 1
            val l1 = if (i1 < frames) inp[2 * i1].toFloat() else l0
            val r1 = if (i1 < frames) inp[2 * i1 + 1].toFloat() else r0
            out[2 * n] = (l0 + (l1 - l0) * frac).toInt().toShort()
            out[2 * n + 1] = (r0 + (r1 - r0) * frac).toInt().toShort()
            n++
            pos += ratio
        }
        pos -= frames
        prevL = inp[2 * (frames - 1)].toFloat()
        prevR = inp[2 * (frames - 1) + 1].toFloat()
        return n
    }
}
