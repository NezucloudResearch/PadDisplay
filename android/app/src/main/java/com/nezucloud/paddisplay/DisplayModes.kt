// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.app.Activity
import android.view.Display
import kotlin.math.roundToInt

/** Refresh rates the panel supports at its native resolution. */
object DisplayModes {
    @Suppress("DEPRECATION")
    private fun nativeModes(activity: Activity): List<Display.Mode> {
        val modes = activity.windowManager.defaultDisplay.supportedModes.toList()
        val maxPixels = modes.maxOfOrNull { it.physicalWidth * it.physicalHeight } ?: return emptyList()
        return modes.filter { it.physicalWidth * it.physicalHeight == maxPixels }
    }

    /** Distinct rates in Hz, ascending (e.g. [60, 120]). */
    fun rates(activity: Activity): List<Int> =
        nativeModes(activity).map { it.refreshRate.roundToInt() }.distinct().sorted().ifEmpty { listOf(60) }

    /** The native-resolution mode for [hz] (0 = highest). */
    fun pick(activity: Activity, hz: Int): Display.Mode? {
        val modes = nativeModes(activity)
        return modes.firstOrNull { hz > 0 && it.refreshRate.roundToInt() == hz } ?: modes.maxByOrNull { it.refreshRate }
    }
}
