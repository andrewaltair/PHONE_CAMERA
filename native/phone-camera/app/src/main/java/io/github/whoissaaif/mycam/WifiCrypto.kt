package io.github.whoissaaif.mycam

import java.io.DataInputStream
import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.math.BigInteger
import java.nio.ByteBuffer
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.PublicKey
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPrivateKeySpec
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * Crypto for the wireless link (PROTOCOL.md "Wireless security"): P-256 ECDH, HKDF-SHA256 and AES-256-GCM
 * records. Kept byte-compatible with pc/companion/wifi_crypto.cpp; protocol/golden.txt ("wifi.*", made with
 * the JDK) is checked by both test suites.
 */
object WifiCrypto {
    const val DIR_PC_TO_PHONE = 0
    const val DIR_PHONE_TO_PC = 1
    const val MAX_RECORD = 16 * 1024 * 1024

    private val curve by lazy {
        KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1")) }
            .generateKeyPair().public.let { (it as ECPublicKey).params }
    }

    fun newKeyPair(): KeyPair =
        KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1")) }.generateKeyPair()

    /** Uncompressed point: 0x04 | X (32) | Y (32). */
    fun rawPublic(key: PublicKey): ByteArray {
        val w = (key as ECPublicKey).w
        return byteArrayOf(4) + fixed32(w.affineX) + fixed32(w.affineY)
    }

    fun publicFromRaw(raw: ByteArray): PublicKey {
        require(raw.size == 65 && raw[0] == 4.toByte()) { "bad public key" }
        val point = ECPoint(BigInteger(1, raw.copyOfRange(1, 33)), BigInteger(1, raw.copyOfRange(33, 65)))
        return KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(point, curve))
    }

    /** For tests: a private key from its 32-byte scalar. */
    fun privateFromScalar(d: ByteArray): PrivateKey =
        KeyFactory.getInstance("EC").generatePrivate(ECPrivateKeySpec(BigInteger(1, d), curve))

    /** The shared secret: the X coordinate of the shared point (32 bytes, big-endian). */
    fun ecdh(own: PrivateKey, peer: PublicKey): ByteArray =
        KeyAgreement.getInstance("ECDH").run { init(own); doPhase(peer, true); generateSecret() }

    fun sha256(vararg parts: ByteArray): ByteArray =
        MessageDigest.getInstance("SHA-256").run { parts.forEach { update(it) }; digest() }

    fun hmac(key: ByteArray, data: ByteArray): ByteArray =
        Mac.getInstance("HmacSHA256").run { init(SecretKeySpec(key, "HmacSHA256")); doFinal(data) }

    /** RFC 5869 HKDF-SHA256. */
    fun hkdf(ikm: ByteArray, salt: ByteArray, info: ByteArray, length: Int): ByteArray {
        val prk = hmac(if (salt.isEmpty()) ByteArray(32) else salt, ikm)
        val out = ByteArray(length)
        var t = ByteArray(0)
        var pos = 0
        var i = 1
        while (pos < length) {
            t = hmac(prk, t + info + byteArrayOf(i++.toByte()))
            val n = minOf(t.size, length - pos)
            System.arraycopy(t, 0, out, pos, n)
            pos += n
        }
        return out
    }

    // --- The handshake's derived values (names match golden.txt) -------------------------------------

    fun commit(nb: ByteArray, phonePub: ByteArray, pcPub: ByteArray) =
        sha256("MyCam commit v1".toByteArray(), nb, phonePub, pcPub)

    /** The 6-digit code both screens show while pairing. */
    fun sas(pcPub: ByteArray, phonePub: ByteArray, na: ByteArray, nb: ByteArray): String {
        val h = sha256(pcPub, phonePub, na, nb)
        val v = ByteBuffer.wrap(h, 0, 4).int.toLong() and 0xFFFFFFFFL
        return "%06d".format(v % 1_000_000L)
    }

    fun pairKey(ecdh: ByteArray, na: ByteArray, nb: ByteArray) =
        hkdf(ecdh, na + nb, "MyCam pair v1".toByteArray(), 32)

    /**
     * QR pairing (PROTOCOL.md "QR pairing"): the scanned one-time code goes into the pairing key, so only a
     * phone that saw the PC's screen can finish the handshake. Golden "qr.pair_key".
     */
    fun qrPairKey(ecdh: ByteArray, code: ByteArray, npc: ByteArray, nph: ByteArray) =
        hkdf(ecdh + code, npc + nph, "MyCam qr pair v1".toByteArray(), 32)

    /** 96 bytes: PC-to-phone key, phone-to-PC key, finished key. */
    fun sessionKeys(ecdh: ByteArray, pairKey: ByteArray, npc: ByteArray, nph: ByteArray) =
        hkdf(ecdh + pairKey, npc + nph, "MyCam session v1".toByteArray(), 96)

    fun finished(kFin: ByteArray, label: String, transcript: ByteArray) = hmac(kFin, label.toByteArray() + transcript)

    fun nonce(dir: Int, counter: Long): ByteArray = ByteBuffer.allocate(12).putInt(dir).putLong(counter).array()

    /** One record: u32 BE ciphertext length, then AES-256-GCM ciphertext with its 16-byte tag. */
    fun seal(key: ByteArray, dir: Int, counter: Long, plain: ByteArray, off: Int = 0, len: Int = plain.size): ByteArray {
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, nonce(dir, counter)))
        val ct = c.doFinal(plain, off, len)
        return ByteBuffer.allocate(4 + ct.size).putInt(ct.size).put(ct).array()
    }

    fun open(key: ByteArray, dir: Int, counter: Long, ciphertext: ByteArray): ByteArray {
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, nonce(dir, counter)))
        return c.doFinal(ciphertext) // Throws AEADBadTagException if tampered with.
    }

    private fun fixed32(v: BigInteger): ByteArray {
        val b = v.toByteArray()
        val out = ByteArray(32)
        val n = minOf(32, b.size)
        System.arraycopy(b, b.size - n, out, 32 - n, n)
        return out
    }
}

/** Encrypts each write as one record (the service writes whole packets, so one packet = one record). */
class SecureOutputStream(private val out: OutputStream, private val key: ByteArray, private val dir: Int) : OutputStream() {
    private var counter = 0L

    override fun write(b: Int) = write(byteArrayOf(b.toByte()), 0, 1)

    override fun write(b: ByteArray, off: Int, len: Int) {
        if (len == 0) return
        out.write(WifiCrypto.seal(key, dir, counter++, b, off, len))
        out.flush()
    }

    override fun flush() = out.flush()
    override fun close() = out.close()
}

/** Reads and decrypts records; any tampering or reordering fails the tag check and ends the link. */
class SecureInputStream(input: InputStream, private val key: ByteArray, private val dir: Int) : InputStream() {
    private val src = DataInputStream(input)
    private var counter = 0L
    private var buf = ByteArray(0)
    private var pos = 0

    override fun read(): Int {
        val one = ByteArray(1)
        return if (read(one, 0, 1) < 0) -1 else one[0].toInt() and 0xFF
    }

    override fun read(b: ByteArray, off: Int, len: Int): Int {
        while (pos >= buf.size) {
            val n = try { src.readInt() } catch (_: EOFException) { return -1 }
            if (n < 16 || n > WifiCrypto.MAX_RECORD) throw IOException("bad record length $n")
            val ct = ByteArray(n)
            src.readFully(ct)
            buf = try { WifiCrypto.open(key, dir, counter++, ct) } catch (e: Exception) { throw IOException("record rejected", e) }
            pos = 0
        }
        val n = minOf(len, buf.size - pos)
        System.arraycopy(buf, pos, b, off, n)
        pos += n
        return n
    }

    override fun close() = src.close()
}
