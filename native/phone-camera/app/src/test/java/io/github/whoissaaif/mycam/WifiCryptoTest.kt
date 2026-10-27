package io.github.whoissaaif.mycam

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.File

/**
 * Checks WifiCrypto against protocol/golden.txt "wifi.*" (made with the JDK, independently of both apps);
 * pc/tests/tests.cpp checks the Windows side against the same vectors.
 */
class WifiCryptoTest {

    private val golden: Map<String, String> by lazy {
        File("../protocol/golden.txt").readLines()
            .map { it.substringBefore('#').trim() }
            .filter { it.startsWith("wifi.") && it.contains('=') }
            .associate { line -> line.split('=', limit = 2).map { it.trim() }.let { it[0] to it[1] } }
    }

    private fun g(name: String) = hex(golden.getValue(name))
    private fun hex(s: String) = s.replace(" ", "").chunked(2).map { it.toInt(16).toByte() }.toByteArray()

    @Test
    fun hkdfMatchesRfc5869() {
        val okm = WifiCrypto.hkdf(hex("0B".repeat(22)), hex("000102030405060708090A0B0C"), hex("F0F1F2F3F4F5F6F7F8F9"), 42)
        assertArrayEquals(g("wifi.hkdf_rfc5869_1"), okm)
    }

    @Test
    fun ecdhAndKeyFormatMatch() {
        val pcPub = g("wifi.pc_public")
        val phonePub = g("wifi.phone_public")
        // Round trip of the raw point format.
        assertArrayEquals(pcPub, WifiCrypto.rawPublic(WifiCrypto.publicFromRaw(pcPub)))
        val z1 = WifiCrypto.ecdh(WifiCrypto.privateFromScalar(g("wifi.pc_private")), WifiCrypto.publicFromRaw(phonePub))
        val z2 = WifiCrypto.ecdh(WifiCrypto.privateFromScalar(g("wifi.phone_private")), WifiCrypto.publicFromRaw(pcPub))
        assertArrayEquals(g("wifi.ecdh"), z1)
        assertArrayEquals(g("wifi.ecdh"), z2)
    }

    @Test
    fun handshakeValuesMatch() {
        val pcPub = g("wifi.pc_public")
        val phonePub = g("wifi.phone_public")
        val z = g("wifi.ecdh")
        val na = g("wifi.na")
        val nb = g("wifi.nb")
        assertArrayEquals(g("wifi.commit"), WifiCrypto.commit(nb, phonePub, pcPub))
        assertEquals(golden.getValue("wifi.sas"), WifiCrypto.sas(pcPub, phonePub, na, nb))
        val pairKey = WifiCrypto.pairKey(z, na, nb)
        assertArrayEquals(g("wifi.pair_key"), pairKey)
        val okm = WifiCrypto.sessionKeys(z, pairKey, g("wifi.npc"), g("wifi.nph"))
        assertArrayEquals(g("wifi.session_okm"), okm)
        val kFin = okm.copyOfRange(64, 96)
        assertArrayEquals(g("wifi.finished_pc"), WifiCrypto.finished(kFin, "PC", g("wifi.transcript")))
        assertArrayEquals(g("wifi.finished_phone"), WifiCrypto.finished(kFin, "PH", g("wifi.transcript")))
    }

    @Test
    fun recordsMatchAndRejectTampering() {
        val key = g("wifi.session_okm").copyOfRange(0, 32)
        assertArrayEquals(g("wifi.record_pc_0"), WifiCrypto.seal(key, WifiCrypto.DIR_PC_TO_PHONE, 0, "MCMD hello".toByteArray()))
        assertArrayEquals(g("wifi.record_phone_7"), WifiCrypto.seal(key, WifiCrypto.DIR_PHONE_TO_PC, 7, ByteArray(0)))

        // Streams: what one side writes, the other reads back, in order.
        val wire = ByteArrayOutputStream()
        val out = SecureOutputStream(wire, key, WifiCrypto.DIR_PHONE_TO_PC)
        out.write("first ".toByteArray())
        out.write("second".toByteArray())
        val read = SecureInputStream(ByteArrayInputStream(wire.toByteArray()), key, WifiCrypto.DIR_PHONE_TO_PC).readBytes()
        assertEquals("first second", String(read))

        // A flipped bit, or the wrong direction, must fail.
        val tampered = wire.toByteArray().also { it[10] = (it[10].toInt() xor 1).toByte() }
        assertTrue(fails { SecureInputStream(ByteArrayInputStream(tampered), key, WifiCrypto.DIR_PHONE_TO_PC).readBytes() })
        assertTrue(fails { SecureInputStream(ByteArrayInputStream(wire.toByteArray()), key, WifiCrypto.DIR_PC_TO_PHONE).readBytes() })
    }

    private fun fails(block: () -> Unit) = try { block(); false } catch (_: IOException) { true }
}
