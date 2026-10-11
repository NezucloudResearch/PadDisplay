// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Switch
import android.widget.TextView
import kotlin.math.roundToInt

/**
 * The look of the connect screen and the overlays, built from code with the framework widgets (no UI library).
 * Colours come from res/values and res/values-night, so the app follows the tablet's light or dark setting.
 */
class Ui(private val ctx: Context) {
    fun dp(v: Int) = (v * ctx.resources.displayMetrics.density).roundToInt()
    fun color(id: Int) = ctx.getColor(id)

    fun shape(fill: Int, radiusDp: Int, stroke: Int = 0, strokeDp: Int = 0): GradientDrawable = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = dp(radiusDp).toFloat()
        if (strokeDp > 0) setStroke(dp(strokeDp), stroke)
    }

    /** [content] with a touch ripple clipped to the same rounded corners. */
    fun ripple(content: Drawable?, radiusDp: Int): Drawable =
        RippleDrawable(ColorStateList.valueOf(color(R.color.pd_ripple)), content, shape(Color.WHITE, radiusDp))

    fun text(text: CharSequence, sizeSp: Float, colorId: Int, bold: Boolean = false) = TextView(ctx).apply {
        this.text = text
        textSize = sizeSp
        setTextColor(color(colorId))
        if (bold) setTypeface(typeface, Typeface.BOLD)
    }

    fun title(text: String) = text(text, 16f, R.color.pd_text)

    fun caption(text: String) = text(text, 13f, R.color.pd_text2).apply { setLineSpacing(0f, 1.15f) }

    fun sectionLabel(text: String) = text(text.uppercase(), 12f, R.color.pd_accent, bold = true).apply {
        letterSpacing = 0.08f
        setPadding(0, 0, 0, dp(4))
    }

    /** A section: a rounded surface with a small heading; rows are added with [addRow]. */
    fun card(title: String) = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        background = shape(color(R.color.pd_surface), 16, color(R.color.pd_outline), 1)
        setPadding(dp(18), dp(16), dp(18), dp(8))
        addView(sectionLabel(title))
    }

    fun divider() = View(ctx).apply {
        setBackgroundColor(color(R.color.pd_outline))
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, maxOf(1, dp(1) / 2))
    }

    /** Adds [row] to [card] under the heading or the previous row, with a hairline between rows. */
    fun addRow(card: LinearLayout, row: View) {
        if (card.childCount > 1) card.addView(divider())
        card.addView(row, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
    }

    fun switchView() = Switch(ctx).apply {
        val on = color(R.color.pd_accent)
        val off = color(R.color.pd_text2)
        thumbTintList = ColorStateList(arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()), intArrayOf(on, off))
        trackTintList = ColorStateList(arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
            intArrayOf(color(R.color.pd_accent_soft), color(R.color.pd_outline)))
    }

    /** Title and optional explanation on the left, the switch on the right; a tap anywhere on the row flips it. */
    fun toggleRow(title: String, description: String?, sw: Switch) = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setPadding(0, dp(12), 0, dp(12))
        minimumHeight = dp(56)
        val texts = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            addView(title(title))
            if (description != null) addView(caption(description).apply { setPadding(0, dp(3), 0, 0) })
        }
        addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        addView(sw, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { marginStart = dp(16) })
        background = ripple(null, 8)
        setOnClickListener { sw.toggle() }
    }

    fun primaryButton(label: String, onClick: () -> Unit) = Button(ctx).apply {
        text = label
        isAllCaps = false
        textSize = 16f
        setTypeface(typeface, Typeface.BOLD)
        setTextColor(color(R.color.pd_on_accent))
        background = ripple(shape(color(R.color.pd_accent), 12), 12)
        stateListAnimator = null
        minHeight = dp(52)
        setOnClickListener { onClick() }
    }

    fun tonalButton(label: String, onClick: () -> Unit) = Button(ctx).apply {
        text = label
        isAllCaps = false
        textSize = 15f
        setTextColor(color(R.color.pd_accent))
        background = ripple(shape(color(R.color.pd_accent_soft), 12), 12)
        stateListAnimator = null
        minHeight = dp(48)
        setOnClickListener { onClick() }
    }

    fun field(hintText: String) = EditText(ctx).apply {
        hint = hintText
        setHintTextColor(color(R.color.pd_text2))
        setTextColor(color(R.color.pd_text))
        textSize = 16f
        isSingleLine = true
        background = shape(color(R.color.pd_bg), 12, color(R.color.pd_outline), 1)
        setPadding(dp(14), dp(12), dp(14), dp(12))
    }

    /** One chosen option out of a few short ones, side by side. [onPick] gets the option's value. */
    fun segmented(options: List<Pair<Int, String>>, current: Int, onPick: (Int) -> Unit) = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        background = shape(color(R.color.pd_bg), 12, color(R.color.pd_outline), 1)
        setPadding(dp(3), dp(3), dp(3), dp(3))
        val cells = ArrayList<TextView>()
        var selected = current
        fun paint() {
            for ((i, cell) in cells.withIndex()) {
                val on = options[i].first == selected
                cell.background = if (on) shape(color(R.color.pd_accent), 10) else null
                cell.setTextColor(color(if (on) R.color.pd_on_accent else R.color.pd_text))
                cell.setTypeface(null, if (on) Typeface.BOLD else Typeface.NORMAL)
            }
        }
        for ((value, label) in options) {
            val cell = TextView(ctx).apply {
                text = label
                textSize = 14f
                gravity = Gravity.CENTER
                setPadding(dp(8), dp(11), dp(8), dp(11))
                setOnClickListener { selected = value; paint(); onPick(value) }
            }
            cells.add(cell)
            addView(cell, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        }
        paint()
    }

    /** One chosen option out of several, one per line (longer labels). */
    fun choiceList(options: List<Pair<Int, String>>, current: Int, onPick: (Int) -> Unit) = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        val radios = ArrayList<android.widget.RadioButton>()
        val tint = ColorStateList(arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
            intArrayOf(color(R.color.pd_accent), color(R.color.pd_text2)))
        for ((value, label) in options) {
            val radio = android.widget.RadioButton(ctx).apply {
                buttonTintList = tint
                isChecked = value == current
                isClickable = false
            }
            radios.add(radio)
            addView(LinearLayout(ctx).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(0, dp(6), 0, dp(6))
                addView(radio)
                addView(text(label, 15f, R.color.pd_text).apply { setPadding(dp(6), 0, 0, 0) },
                    LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                background = ripple(null, 8)
                setOnClickListener {
                    for ((i, r) in radios.withIndex()) r.isChecked = options[i].first == value
                    onPick(value)
                }
            })
        }
    }

    /** A pill for the overlays on top of the picture. */
    fun pill(fill: Int) = shape(fill, 14)
}
