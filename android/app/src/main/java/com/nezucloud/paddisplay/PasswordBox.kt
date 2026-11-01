// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView

/**
 * A box to type the PC's Windows password into when the tablet cannot show its locked sign-in screen.
 * The text is sent over the (encrypted) link and the PC types it into the sign-in field; nothing is kept
 * here. While the box is up, key events belong to it ([isOpen]), so they are not forwarded. UI thread.
 */
class PasswordBox(
    private val activity: Activity,
    private val anchor: View, // only to post the timeout on
    private val send: (String) -> Unit,
) {

    private companion object {
        const val VERIFY_TIMEOUT_MS = 8000L
    }

    private var dialog: AlertDialog? = null
    private var field: EditText? = null
    private var status: TextView? = null  // a hint / error line under the field
    private var spinner: View? = null     // the "Signing in…" row, shown while waiting for the PC to verify
    private var buttons: View? = null     // the Cancel / Send row
    private var submitting = false        // a password was sent and we are waiting to see if it unlocks the PC
    private val verifyTimeout = Runnable { onVerifyTimedOut() } // fires if no unlock arrives after sending

    val isOpen get() = dialog != null

    fun show() {
        if (dialog != null) return // already up
        val ui = Ui(activity)
        val field = ui.field("Windows password").apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        // Hidden until there is something to say (e.g. "wrong password").
        val status = ui.caption("").apply { setTextColor(ui.color(R.color.pd_danger)); visibility = View.GONE }
        val spinner = signingInRow(ui)
        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            background = ui.shape(ui.color(R.color.pd_surface), 20, ui.color(R.color.pd_outline), 1)
            val side = ui.dp(22)
            setPadding(side, ui.dp(20), side, ui.dp(18))
            addView(ui.text("Sign in to the PC", 19f, R.color.pd_text, bold = true))
            addView(
                ui.caption("Type the Windows password and send it to the PC's sign-in screen.")
                    .apply { setPadding(0, ui.dp(6), 0, ui.dp(16)) })
            addView(
                field,
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            )
            addView(status.apply { setPadding(0, ui.dp(10), 0, 0) })
            addView(spinner)
        }
        val box = AlertDialog.Builder(activity).setView(content).create()
        dialog = box
        this.field = field
        this.status = status
        this.spinner = spinner
        box.window?.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT)) // show our own rounded surface, not the platform frame
        box.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE)
        box.setOnDismissListener {
            anchor.removeCallbacks(verifyTimeout)
            submitting = false
            dialog = null; this.field = null; this.status = null; this.spinner = null; buttons = null
            field.text.clear()
        }
        val row = buttonRow(ui, box)
        buttons = row
        content.addView(row)
        try {
            box.show()
            field.requestFocus()
        } catch (_: Exception) { // window not attached (racing a background transition): drop it, reopen on focus
            dialog = null
        }
    }

    private fun signingInRow(ui: Ui) = LinearLayout(activity).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        visibility = View.GONE
        setPadding(0, ui.dp(18), 0, ui.dp(2))
        addView(ProgressBar(activity).apply {
            isIndeterminate = true
            indeterminateTintList = android.content.res.ColorStateList.valueOf(ui.color(R.color.pd_accent))
        }, LinearLayout.LayoutParams(ui.dp(26), ui.dp(26)))
        addView(ui.text("Signing in…", 15f, R.color.pd_text2).apply { setPadding(ui.dp(14), 0, 0, 0) })
    }

    // Styled like the connect screen: a tonal Cancel and an accent Send.
    private fun buttonRow(ui: Ui, box: AlertDialog) = LinearLayout(activity).apply {
        orientation = LinearLayout.HORIZONTAL
        setPadding(0, ui.dp(18), 0, 0)
        addView(
            ui.tonalButton("Cancel") { box.dismiss() },
            LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        )
        addView(ui.primaryButton("Send") { submit() },
            LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { marginStart = ui.dp(10) })
    }

    // Send the typed password and switch the box to a "Signing in…" state: the PC types it into the sign-in
    // screen and, if it is right, unlocks - which arrives back as LockState(false) and dismisses the box. If no
    // unlock comes within the timeout, the password was wrong (or did not take), so we show an error and let the
    // user try again. There is no explicit "wrong password" message from Windows we can read, so the unlock (or
    // its absence) is the signal.
    private fun submit() {
        if (submitting) return
        val text = field?.text?.toString() ?: return
        if (text.isEmpty()) {
            showError("Type the password first.")
            return
        }
        send(text)
        submitting = true
        status?.visibility = View.GONE
        buttons?.visibility = View.GONE
        field?.isEnabled = false
        spinner?.visibility = View.VISIBLE
        dialog?.setCancelable(false) // don't let a stray back/tap drop it mid-verify; the timeout restores control
        anchor.removeCallbacks(verifyTimeout)
        anchor.postDelayed(verifyTimeout, VERIFY_TIMEOUT_MS)
    }

    private fun onVerifyTimedOut() {
        if (!submitting) return
        submitting = false
        spinner?.visibility = View.GONE
        buttons?.visibility = View.VISIBLE
        field?.apply { isEnabled = true; text.clear(); requestFocus() }
        dialog?.setCancelable(true)
        showError("That didn't work. Check the password and try again.")
    }

    private fun showError(message: String) {
        status?.apply { text = message; visibility = View.VISIBLE }
    }

    /** The password worked, the PC unlocked on its own, or the window is going away. */
    fun dismiss() {
        anchor.removeCallbacks(verifyTimeout)
        dialog?.dismiss()
    }
}
