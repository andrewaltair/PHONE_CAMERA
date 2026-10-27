package io.github.whoissaaif.mycam

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * Checks QR pairing (PROTOCOL.md "QR pairing") against protocol/golden.txt "qr.*" (made with the JDK); the PC's
 * tests check the same vectors. qr.code_text is text, the rest hex.
 */
class QrPairingTest {
    private val golden: Map<String, String> by lazy {
        File("../protocol/golden.txt").readLines()
            .map { it.substringBefore('#') }
            .filter { '=' in it }
            .associate { it.substringBefore('=').trim() to it.substringAfter('=').trim() }
    }

    private fun g(name: String) = hex(golden.getValue(name))
    private fun hex(s: String) = s.replace(" ", "").chunked(2).map { it.toInt(16).toByte() }.toByteArray()

    @Test
    fun keyAndInviteMatchGolden() {
        val code = g("qr.code")
        assertArrayEquals(g("qr.pair_key"), WifiCrypto.qrPairKey(g("wifi.ecdh"), code, g("wifi.npc"), g("wifi.nph")))
        val port = g("qr.port").let { (it[0].toInt() and 0xFF shl 8) or (it[1].toInt() and 0xFF) }
        assertEquals(47800, port)
        assertEquals(golden.getValue("qr.invite_proof").lowercase(), QrPairing.inviteProof(code, port))
        assertTrue(QrPairing.inviteProof(code, port + 1) != QrPairing.inviteProof(code, port)) // The port counts.
        assertEquals(
            "MYCAM#1 47800 ${golden.getValue("qr.invite_proof").lowercase()} CMF by Nothing Phone 1",
            QrPairing.invite(code, port, "CMF by Nothing Phone 1"),
        )
        assertArrayEquals(code, QrPairing.decodeBase64Url(golden.getValue("qr.code_text")))
    }

    @Test
    fun base64UrlRefusesOtherSpellings() {
        assertArrayEquals(byteArrayOf(0xFF.toByte(), 0xEF.toByte()), QrPairing.decodeBase64Url("_-8"))
        assertArrayEquals(ByteArray(0), QrPairing.decodeBase64Url(""))
        assertNull(QrPairing.decodeBase64Url("QUJD="))
        assertNull(QrPairing.decodeBase64Url("QUJD+"))
        assertNull(QrPairing.decodeBase64Url("Q"))
        assertNull(QrPairing.decodeBase64Url("QR")) // Leftover bits set.
    }

    @Test
    fun readsTheSameQrCodeIphonesScan() {
        val t = golden.getValue("qr.code_text")
        val pc = golden.getValue("qr.pc_id").lowercase()
        val ok = QrPairing.parse("https://192.168.1.20:47900/?t=$t&pc=$pc")
        assertTrue(ok is QrPairing.Scan.Ok)
        val code = (ok as QrPairing.Scan.Ok).code
        assertEquals("192.168.1.20", code.host)
        assertArrayEquals(g("qr.code"), code.code)
        assertArrayEquals(g("qr.pc_id"), code.pcId)
        // Field order and case don't matter, and no path is the same as "/".
        assertTrue(QrPairing.parse("https://10.0.0.5:47900?pc=${pc.uppercase()}&t=$t") is QrPairing.Scan.Ok)

        // A companion before 1.5: a code but no PC id, so MyCam on the PC needs updating.
        assertEquals(QrPairing.Scan.OldPc, QrPairing.parse("https://192.168.1.20:47900/?t=$t"))
        assertEquals(QrPairing.Scan.OldPc, QrPairing.parse("https://192.168.1.20:47900/?t=$t&pc=abcd"))
        // Anything else isn't ours.
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("https://example.com/?t=$t&pc=$pc"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("http://192.168.1.20:47900/?t=$t&pc=$pc"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("https://192.168.1.20:47900/other?t=$t&pc=$pc"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("https://192.168.1.20:47900/?pc=$pc"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("https://192.168.1.20:47900/?t=short&pc=$pc"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("WIFI:S:home;T:WPA;P:secret;;"))
        // The PC always puts one of its own IPv4 addresses in the code. A host name on the right port is not
        // ours: it could never match the address a probe comes from, so it would only waste the 20 s wait.
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("https://example.com:47900/?t=$t&pc=$pc"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("https://192.168.1.999:47900/?t=$t&pc=$pc"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("https://192.168.1:47900/?t=$t&pc=$pc"))
        // A pc= in other Unicode digit forms is not hex: it would pass Character.digit and then never match.
        assertEquals(QrPairing.Scan.OldPc, QrPairing.parse("https://192.168.1.20:47900/?t=$t&pc=" + "٠".repeat(32)))
        assertNull(QrPairing.unhex("٠١"))
        assertArrayEquals(byteArrayOf(0x0A, 0xBC.toByte()), QrPairing.unhex("0aBc"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("not a url at all %%%"))
    }

    @Test
    fun hexBothWays() {
        val id = g("qr.pc_id")
        assertEquals("707172737475767778797a7b7c7d7e7f", QrPairing.hex(id))
        assertArrayEquals(id, QrPairing.unhex("707172737475767778797a7b7c7d7e7f"))
        assertNull(QrPairing.unhex("707"))
        assertNull(QrPairing.unhex("zz"))
        assertNull(QrPairing.unhex("+1")) // toInt(16) would take it; a hex digit check doesn't.
    }
}
