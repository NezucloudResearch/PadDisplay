// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.AccessibilityServiceInfo
import android.content.ComponentName
import android.content.Context
import android.provider.Settings
import android.view.KeyEvent
import android.view.accessibility.AccessibilityEvent

/**
 * Where the activity asks to see keyboard keys before Android does. Without this, Android takes
 * Alt+Tab, the Windows key, Ctrl+Esc and similar shortcuts for itself and the PC never hears of them.
 */
object KeyRouter {
    /** Set while the picture has focus and the keyboard goes to the PC; returns true when it took the key. */
    @Volatile var handler: ((KeyEvent) -> Boolean)? = null
        private set

    fun attach(h: (KeyEvent) -> Boolean) {
        handler = h
        ShortcutService.instance?.setFiltering(true)
    }

    fun detach() {
        handler = null
        ShortcutService.instance?.setFiltering(false)
    }
}

/**
 * Optional accessibility service, switched on by the user in Android's settings. It is the only way for an
 * app to receive the keys Android reserves for itself. It reads no screen content and asks to see keys
 * only while PadDisplay is in front and sending the keyboard to the PC (see [KeyRouter]); at any other
 * time Android handles keys exactly as if the service did not exist, and does not even ask it.
 */
class ShortcutService : AccessibilityService() {

    companion object {
        @Volatile var instance: ShortcutService? = null

        /** Whether the user has switched the service on in Android's accessibility settings. */
        fun isEnabled(context: Context): Boolean {
            val me = ComponentName(context, ShortcutService::class.java)
            val list = Settings.Secure.getString(context.contentResolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES) ?: return false
            return list.split(':').any { ComponentName.unflattenFromString(it) == me }
        }
    }

    override fun onServiceConnected() {
        instance = this
        setFiltering(KeyRouter.handler != null)
    }

    /** The flag is added and removed at run time so Android only calls this app for keys during a session. */
    fun setFiltering(on: Boolean) {
        val info = serviceInfo ?: return
        val flags = if (on) info.flags or AccessibilityServiceInfo.FLAG_REQUEST_FILTER_KEY_EVENTS
                    else info.flags and AccessibilityServiceInfo.FLAG_REQUEST_FILTER_KEY_EVENTS.inv()
        if (flags == info.flags) return
        info.flags = flags
        serviceInfo = info
    }

    override fun onKeyEvent(event: KeyEvent): Boolean = KeyRouter.handler?.invoke(event) ?: false

    override fun onAccessibilityEvent(event: AccessibilityEvent) {}

    override fun onInterrupt() {}

    override fun onUnbind(intent: android.content.Intent?): Boolean {
        instance = null
        return super.onUnbind(intent)
    }
}
