// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log

/**
 * Keeps the session alive while another app is in front. It holds no connection itself: DisplayActivity keeps
 * the session and this foreground service only stops Android from freezing or killing the process meanwhile
 * (HarmonyOS and most ROMs do that within seconds to a background app with an open socket). Its notification
 * also says what is still running and offers Disconnect.
 */
class SessionService : Service() {

    companion object {
        private const val TAG = "PadDisplay"
        private const val CHANNEL = "session"
        private const val NOTIFICATION_ID = 1
        private const val ACTION_DISCONNECT = "com.nezucloud.paddisplay.DISCONNECT"
        private const val ACTION_STOP = "com.nezucloud.paddisplay.STOP"
        private const val EXTRA_HOST = "host"
        private const val EXTRA_MIC = "mic"
        private const val EXTRA_CAM = "cam"

        /** Set by DisplayActivity while its session runs; the notification's Disconnect calls it (main thread). */
        var onDisconnect: (() -> Unit)? = null

        // The types are decided when the service starts: Android allows background microphone and camera use only
        // to a service that declared them while the app was in front. Declaring them does not turn either on.
        fun start(context: Context, host: String, mic: Boolean, cam: Boolean) {
            val intent = Intent(context, SessionService::class.java)
                .putExtra(EXTRA_HOST, host).putExtra(EXTRA_MIC, mic).putExtra(EXTRA_CAM, cam)
            try {
                context.startForegroundService(intent)
            } catch (e: Exception) {
                // The session still works while in front; only the background part is lost.
                Log.w(TAG, "cannot start the background service: $e")
            }
        }

        // Through the service's own queue, behind the start: stopping a service started this way before it called
        // startForeground crashes the app, and a wrong PIN ends the session within milliseconds.
        fun stop(context: Context) {
            try {
                context.startService(Intent(context, SessionService::class.java).setAction(ACTION_STOP))
            } catch (e: Exception) {
                // Refused only to an app in the background without a running service: there is nothing to stop then.
                context.stopService(Intent(context, SessionService::class.java))
            }
        }

        /** Refreshes the notification so it says whether the microphone or camera is capturing right now. */
        fun update(context: Context, host: String, micOn: Boolean, camOn: Boolean) {
            val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            try {
                manager.notify(NOTIFICATION_ID, build(context, host, micOn, camOn))
            } catch (e: Exception) {
                Log.w(TAG, "notification update: $e")
            }
        }

        private fun build(context: Context, host: String, micOn: Boolean, camOn: Boolean): Notification {
            val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            if (manager.getNotificationChannel(CHANNEL) == null) {
                manager.createNotificationChannel(NotificationChannel(CHANNEL, "Connection to the PC", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "Shown while PadDisplay stays connected in the background"
                    setShowBadge(false)
                })
            }
            // The same intent as the launcher's, so the existing task comes back with the picture on top.
            val open = Intent.makeMainActivity(ComponentName(context, MainActivity::class.java))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED)
            val disconnect = Intent(context, SessionService::class.java).setAction(ACTION_DISCONNECT)
            val immutable = PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            val capturing = listOfNotNull(if (micOn) "microphone on" else null, if (camOn) "camera on" else null)
            val where = if (Tls.isLoopback(host)) "over USB" else "to $host"
            return Notification.Builder(context, CHANNEL)
                .setSmallIcon(R.drawable.ic_stat_display)
                .setContentTitle("Connected $where")
                .setContentText(if (capturing.isEmpty()) "PadDisplay stays connected while you use other apps."
                    else "This tablet's " + capturing.joinToString(" and ") + ".")
                .setOngoing(true)
                .setCategory(Notification.CATEGORY_SERVICE)
                .setContentIntent(PendingIntent.getActivity(context, 0, open, immutable))
                .addAction(Notification.Action.Builder(null, "Disconnect", PendingIntent.getService(context, 1, disconnect, immutable)).build())
                .apply { if (Build.VERSION.SDK_INT >= 31) setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE) }
                .build()
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_DISCONNECT -> onDisconnect?.invoke() ?: stopSelf() // DisplayActivity finishes, and its stop follows
            ACTION_STOP -> stopSelf()
        }
        if (intent?.action != null) return START_NOT_STICKY
        val host = intent?.getStringExtra(EXTRA_HOST) ?: ""
        val notification = build(this, host, false, false)
        var types = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        if (Build.VERSION.SDK_INT >= 30) {
            if (intent?.getBooleanExtra(EXTRA_MIC, false) == true) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            if (intent?.getBooleanExtra(EXTRA_CAM, false) == true) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
        }
        try {
            startForeground(NOTIFICATION_ID, notification, types)
        } catch (e: Exception) {
            // Android refuses the microphone or camera type when it no longer counts the app as in front
            // (or the permission was taken away): keep the connection, without them in the background.
            Log.w(TAG, "foreground service with types $types refused, retrying without microphone and camera: $e")
            try {
                startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
            } catch (e2: Exception) {
                Log.w(TAG, "foreground service refused: $e2")
                stopSelf()
            }
        }
        return START_NOT_STICKY // a restarted service would have no session to keep
    }
}
