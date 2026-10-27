package io.github.whoissaaif.mycam

import java.net.URI
import java.net.URLDecoder
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * Pairing by scanning the PC's QR code (PROTOCOL.md "QR pairing"): the same code iPhones scan, read here
 * natively. Nothing connects into the PC: after the scan the phone answers the PC's discovery probe with
 * "MYCAM#1" and a proof that it holds the code, and the PC connects to this phone with the code mixed into
 * the handshake's keys ([WifiHandshake], [WifiCrypto.qrPairKey]). No code to compare, nothing to allow:
 * scanning the PC's screen is the consent. Works on any network, Windows "Public" ones included.
 *
 * Plain Kotlin, checked against protocol/golden.txt "qr.*".
 */
object QrPairing {
    const val PC_PORT = 47900

    /** What a MyCam QR code holds: the PC's address, the one-time code (16 bytes) and the PC id (16 bytes). */
    class Code(val host: String, val code: ByteArray, val pcId: ByteArray)

    sealed class Scan {
        /** Not a MyCam code at all: the scanner keeps looking and says nothing. */
        object NotMyCam : Scan()
        /** A MyCam code from a companion before 1.5 (no PC id): MyCam on the PC needs updating. */
        object OldPc : Scan()
        class Ok(val code: Code) : Scan()
    }

    /**
     * Why a pairing didn't work, in terms the screen can explain. NoPc: no probe from that PC was heard (not on
     * this network, or it closed the QR code). NoSession: it was heard but never connected.
     */
    enum class Failure { NoPc, CodeUsed, Busy, NoSession }

    /** Reads `https://<ipv4>:47900/?t=<code>&pc=<PC id>`. */
    fun parse(text: String): Scan {
        val uri = try { URI(text.trim()) } catch (_: Exception) { return Scan.NotMyCam }
        if (!"https".equals(uri.scheme, ignoreCase = true) || uri.port != PC_PORT) return Scan.NotMyCam
        if (!(uri.rawPath.isNullOrEmpty() || uri.rawPath == "/")) return Scan.NotMyCam
        // A MyCam code always points at an IPv4 address, because the PC puts one of its own addresses in it.
        // Anything else (a host name, so something that would have to be resolved) is not ours: saying so now
        // beats pairing that waits 20 s for a probe from a name that will never match one.
        val host = uri.host?.takeIf(::isIpv4) ?: return Scan.NotMyCam
        val query = queryOf(uri.rawQuery ?: "")
        val code = query["t"]?.let { decodeBase64Url(it) }?.takeIf { it.size == 16 } ?: return Scan.NotMyCam
        val pcId = query["pc"]?.let { unhex(it) }?.takeIf { it.size == 16 } ?: return Scan.OldPc
        return Scan.Ok(Code(host, code, pcId))
    }

    /** The invite's proof: hex of the first 16 bytes of HMAC(code, "MyCam qr invite v1" | tcp port, u16 BE). */
    fun inviteProof(code: ByteArray, tcpPort: Int): String {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(code, "HmacSHA256"))
        mac.update("MyCam qr invite v1".toByteArray(Charsets.US_ASCII))
        mac.update(byteArrayOf((tcpPort shr 8).toByte(), tcpPort.toByte()))
        return hex(mac.doFinal().copyOf(16))
    }

    /** "MYCAM#1 <tcp port> <proof> <phone name>", sent to the probe's source address and port. */
    fun invite(code: ByteArray, tcpPort: Int, phoneName: String) =
        "${Protocol.WIRELESS_QR} $tcpPort ${inviteProof(code, tcpPort)} $phoneName"

    // --- Encodings (android.util.Base64 isn't there in unit tests, java.util.Base64 needs API 26) --------

    private const val ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"

    /** Base64url without padding; null if it isn't exactly that. */
    fun decodeBase64Url(s: String): ByteArray? {
        if (s.length % 4 == 1) return null
        val out = java.io.ByteArrayOutputStream(s.length * 3 / 4)
        var acc = 0
        var bits = 0
        for (c in s) {
            val v = ALPHABET.indexOf(c)
            if (v < 0) return null
            acc = (acc shl 6) or v
            bits += 6
            if (bits >= 8) {
                bits -= 8
                out.write(acc shr bits and 0xFF)
            }
        }
        if (acc and ((1 shl bits) - 1) != 0) return null
        return out.toByteArray()
    }

    fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }

    /** "192.168.1.20": four decimal octets, nothing else (no name, no IPv6, no shorthand). */
    private fun isIpv4(host: String): Boolean {
        val parts = host.split('.')
        return parts.size == 4 && parts.all { p ->
            p.isNotEmpty() && p.length <= 3 && p.all { it in '0'..'9' } && p.toInt() <= 255
        }
    }

    fun unhex(s: String): ByteArray? {
        // Plain ASCII hex only. Character.digit() would also accept other Unicode digit forms, so a `pc=`
        // written in, say, Arabic-Indic digits would pass and then never match the PC's own id.
        if (s.length % 2 != 0 || s.any { it !in '0'..'9' && it !in 'a'..'f' && it !in 'A'..'F' }) return null
        return ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
    }

    private fun queryOf(raw: String): Map<String, String> = raw.split('&').filter { it.isNotEmpty() }.associate { pair ->
        val eq = pair.indexOf('=')
        val name = if (eq < 0) pair else pair.substring(0, eq)
        val value = if (eq < 0) "" else pair.substring(eq + 1)
        URLDecoder.decode(name, "UTF-8") to URLDecoder.decode(value, "UTF-8")
    }
}
