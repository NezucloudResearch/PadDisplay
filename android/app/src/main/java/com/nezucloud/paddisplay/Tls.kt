// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.content.SharedPreferences
import java.io.IOException
import java.net.Socket
import java.security.MessageDigest
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.X509TrustManager

/** The PC presented a different certificate than the one this tablet pinned the first time. */
class FingerprintChangedException(val seen: String) : CertificateException("the PC's security fingerprint changed")

/**
 * Which PC is this? The PC makes a self-signed certificate once; the first time this tablet connects it
 * remembers the certificate's fingerprint (like SSH) and refuses a PC that later shows a different one.
 * One per session: the video and audio connections share it, so they must see the same certificate.
 *
 * The fingerprint is stored only after the PC has accepted the PIN ([confirm]), so a wrong-PIN attempt,
 * or someone else's PC, never becomes the remembered identity.
 */
class HostTrust(private val prefs: SharedPreferences, host: String) {
    private val prefKey = "tlsfp_$host"
    private val lock = Any()
    private var pinned: String? = prefs.getString(prefKey, null)
    private var firstSeen: String? = null

    /** One context per session: creating it is the slow part of a TLS connect on a phone. */
    val sslContext: SSLContext by lazy {
        SSLContext.getInstance("TLSv1.2").apply { init(null, arrayOf(PinningTrustManager()), null) }
    }

    /** Called during the handshake with the fingerprint of the certificate the PC presented. */
    private fun check(fingerprint: String) = synchronized(lock) {
        val expected = pinned ?: firstSeen
        if (expected == null) firstSeen = fingerprint
        else if (expected != fingerprint) throw FingerprintChangedException(fingerprint)
    }

    /** The PC accepted our PIN: remember its certificate. */
    fun confirm() = synchronized(lock) {
        val seen = firstSeen
        if (pinned == null && seen != null) {
            pinned = seen
            prefs.edit().putString(prefKey, seen).apply()
        }
    }

    fun forget() = synchronized(lock) {
        pinned = null
        firstSeen = null
        prefs.edit().remove(prefKey).apply()
    }

    private inner class PinningTrustManager : X509TrustManager {
        override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) = throw CertificateException("not a server")

        override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) {
            if (chain.isEmpty()) throw CertificateException("no certificate")
            check(fingerprintOf(chain[0]))
        }

        override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
    }

    companion object {
        /** "AB:CD:..." (SHA-256 of the certificate), the same text the PC shows in its tray menu. */
        fun fingerprintOf(cert: X509Certificate): String =
            MessageDigest.getInstance("SHA-256").digest(cert.encoded).joinToString(":") { "%02X".format(it) }

        /** The first 8 bytes: what the tray menu and the dialogs show. */
        fun short(fingerprint: String) = fingerprint.take(23)
    }
}

object Tls {
    // AES-128-GCM: hardware-accelerated on the tablet and the PC, and cheaper per byte than the AES-256 suite
    // the PC would pick if both were offered. The PC's certificate is ECDSA, so this is the matching suite.
    private val SUITES = arrayOf("TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256")

    /** The adb tunnel over USB: it never touches the network, so it stays plain and keeps its lowest latency. */
    fun isLoopback(host: String) = host == "127.0.0.1" || host == "localhost" || host == "::1"

    /**
     * Runs the TLS handshake over an already connected socket, which keeps its low-latency options (no delay,
     * DSCP, buffer sizes, timeout). Blocking; call it on the connection's own thread.
     * @throws java.io.IOException the handshake failed, or the PC's certificate is not the pinned one
     */
    fun wrap(raw: Socket, host: String, port: Int, trust: HostTrust): SSLSocket {
        val ssl = trust.sslContext.socketFactory.createSocket(raw, host, port, true) as SSLSocket
        ssl.enabledProtocols = arrayOf("TLSv1.2")
        ssl.enabledCipherSuites = SUITES
        ssl.startHandshake()
        return ssl
    }

    /**
     * Ends a connection from any thread, the UI thread included, by closing its TCP socket: that ends it for both
     * sides, wakes the thread reading from it, and cannot block. The TLS socket on top is left to that thread:
     * closing it writes a goodbye record, which Android refuses on the UI thread (NetworkOnMainThreadException).
     */
    fun hangUp(raw: Socket?) {
        try { raw?.close() } catch (_: IOException) {}
    }

    /** The pinning failure, if that is why [e] happened. The trust manager's exception arrives wrapped. */
    fun fingerprintChange(e: Throwable): FingerprintChangedException? =
        generateSequence(e) { it.cause }.filterIsInstance<FingerprintChangedException>().firstOrNull()
}
