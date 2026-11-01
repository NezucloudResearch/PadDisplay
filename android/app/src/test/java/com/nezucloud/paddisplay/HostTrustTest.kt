// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
package com.nezucloud.paddisplay

import android.content.Context
import android.content.SharedPreferences
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import java.io.IOException
import java.security.cert.CertificateException

@RunWith(RobolectricTestRunner::class)
class HostTrustTest {

    private lateinit var prefs: SharedPreferences
    private val host = "192.168.1.9"
    private val pc = "AA:BB"
    private val otherPc = "CC:DD"

    @Before
    fun openPrefs() {
        prefs = RuntimeEnvironment.getApplication().getSharedPreferences("test", Context.MODE_PRIVATE)
        prefs.edit().clear().commit()
    }

    private fun remembered() = prefs.getString("tlsfp_$host", null)

    @Test
    fun `a first connection is allowed`() {
        HostTrust(prefs, host).check(pc)
    }

    @Test
    fun `nothing is remembered before the PIN is accepted`() {
        HostTrust(prefs, host).check(pc)
        assertNull(remembered())
    }

    @Test
    fun `a wrong PIN never makes a PC the remembered one`() {
        HostTrust(prefs, host).check(pc) // the session ends here: no confirm
        HostTrust(prefs, host).check(otherPc)
    }

    @Test
    fun `the PC is remembered once it accepted the PIN`() {
        val trust = HostTrust(prefs, host)
        trust.check(pc)
        trust.confirm()
        assertEquals(pc, remembered())
    }

    @Test
    fun `a remembered PC is allowed again`() {
        prefs.edit().putString("tlsfp_$host", pc).commit()
        HostTrust(prefs, host).check(pc)
    }

    @Test
    fun `a PC with another fingerprint is refused and reported`() {
        prefs.edit().putString("tlsfp_$host", pc).commit()
        val refused = assertThrows(FingerprintChangedException::class.java) { HostTrust(prefs, host).check(otherPc) }
        assertEquals(otherPc, refused.seen)
    }

    @Test
    fun `all connections of a session must show the same certificate`() {
        val trust = HostTrust(prefs, host)
        trust.check(pc)
        assertThrows(FingerprintChangedException::class.java) { trust.check(otherPc) }
    }

    @Test
    fun `a refused PC does not replace the remembered one`() {
        prefs.edit().putString("tlsfp_$host", pc).commit()
        val trust = HostTrust(prefs, host)
        assertThrows(FingerprintChangedException::class.java) { trust.check(otherPc) }
        trust.confirm()
        assertEquals(pc, remembered())
    }

    @Test
    fun `each address has its own fingerprint`() {
        prefs.edit().putString("tlsfp_$host", pc).commit()
        HostTrust(prefs, "192.168.1.10").check(otherPc)
    }

    @Test
    fun `forgetting lets the next PC be pinned`() {
        prefs.edit().putString("tlsfp_$host", pc).commit()
        HostTrust.forget(prefs, host)
        assertNull(remembered())
        HostTrust(prefs, host).check(otherPc)
    }

    @Test
    fun `the short fingerprint is what the tray menu shows`() {
        assertEquals("AA:BB:CC:DD:EE:FF:00:11", HostTrust.short("AA:BB:CC:DD:EE:FF:00:11:22:33"))
    }

    @Test
    fun `USB addresses are the loopback ones`() {
        assertTrue(Tls.isLoopback("127.0.0.1"))
        assertTrue(Tls.isLoopback("localhost"))
        assertTrue(Tls.isLoopback("::1"))
        assertFalse(Tls.isLoopback("192.168.1.9"))
    }

    @Test
    fun `a pinning failure is found inside the exceptions around it`() {
        val pinning = FingerprintChangedException(otherPc)
        assertSame(pinning, Tls.fingerprintChange(IOException("handshake", CertificateException("wrapped", pinning))))
    }

    @Test
    fun `any other failure is not a pinning failure`() {
        assertNull(Tls.fingerprintChange(IOException("reset", CertificateException("expired"))))
    }
}
