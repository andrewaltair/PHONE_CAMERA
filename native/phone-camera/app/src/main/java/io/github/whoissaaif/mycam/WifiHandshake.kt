package io.github.whoissaaif.mycam

import android.content.Context
import android.content.SharedPreferences
import android.util.Base64
import androidx.core.content.edit
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.Socket
import java.net.SocketTimeoutException
import java.security.MessageDigest
import java.security.SecureRandom

/**
 * Phone side of the wireless handshake (PROTOCOL.md "Wireless security"): pairs a new PC with a 6-digit
 * code the user compares, or proves a paired PC knows the pairing key; then hands back encrypted streams.
 * Runs on its own thread; the only blocking user step is [askUser] while pairing.
 */
class WifiHandshake(
    private val socket: Socket,
    private val store: PairedPcs,
    private val phoneId: ByteArray,
    private val phoneName: String,
    /**
     * Pairing: show the PC name and code, return true if the user allowed it (blocks up to ~60 s). Stop
     * asking (and return false) once [pcGone] says the PC hung up, e.g. "Cancel pairing" on the PC.
     */
    private val askUser: (pcName: String, code: String, pcGone: () -> Boolean) -> Boolean,
    /** QR pairing: the one-time code scanned from this PC's screen (16 bytes), or null if none is pending. */
    private val qrCodeFor: (pcId: ByteArray) -> ByteArray? = { null },
) {
    class Result(
        val input: InputStream,
        val output: OutputStream,
        val pcName: String,
        val newlyPaired: Boolean,
        /** Which PC this is, for [PairedPcs.cameraAllowed]. */
        val pcId: ByteArray,
    )

    private val random = SecureRandom()
    private val transcript = MessageDigest.getInstance("SHA-256")
    private lateinit var input: DataInputStream
    private lateinit var output: DataOutputStream

    /** Returns the secure streams, or null (after closing the socket) if the PC was refused or failed. */
    fun run(): Result? = try {
        socket.soTimeout = STEP_TIMEOUT_MS
        input = DataInputStream(socket.getInputStream())
        output = DataOutputStream(socket.getOutputStream())
        handshake()
    } catch (e: Exception) {
        android.util.Log.i(TAG, "handshake failed: ${e.message}")
        null
    }.also { if (it == null) try { socket.close() } catch (_: IOException) {} }

    private fun handshake(): Result? {
        // ClientHello: "MCHS" | version | flags | pcId | ePK | Npc | name
        val hello = DataInputStream(read(TYPE_CLIENT_HELLO).inputStream())
        val magic = ByteArray(4).also { hello.readFully(it) }
        if (!magic.contentEquals(MAGIC) || hello.readUnsignedByte() != VERSION) throw IOException("not a MyCam PC")
        val flags = hello.readUnsignedByte()
        val forcePair = flags and FLAG_FORCE_PAIR != 0
        val pcId = ByteArray(16).also { hello.readFully(it) }
        val pcPub = ByteArray(65).also { hello.readFully(it) }
        val npc = ByteArray(16).also { hello.readFully(it) }
        val pcName = readName(hello)

        val key = WifiCrypto.newKeyPair()
        val phonePub = WifiCrypto.rawPublic(key.public)
        val z = WifiCrypto.ecdh(key.private, WifiCrypto.publicFromRaw(pcPub))
        val nph = randomBytes(16)

        // QR pairing (PROTOCOL.md "QR pairing"): the PC proved, by connecting after our invite, that it is the
        // one whose code we scanned; the code goes into the key, so only it can finish. Nothing to ask.
        if (flags and FLAG_QR != 0) {
            val code = qrCodeFor(pcId) ?: run { reject(REJECT_AUTH); return null }
            send(TYPE_SERVER_HELLO, phoneId + phonePub + nph + byteArrayOf(MODE_QR) + name(phoneName))
            val pairKey = WifiCrypto.qrPairKey(z, code, npc, nph)
            val keys = WifiCrypto.sessionKeys(z, pairKey, npc, nph)
            if (!finishedFromPc(keys)) { reject(REJECT_AUTH); return null }
            store.put(pcId, pairKey, pcName)
            return finish(keys, pcId, pcName, newlyPaired = true)
        }

        val known = if (forcePair) null else store.key(pcId)

        if (known != null) {
            send(TYPE_SERVER_HELLO, phoneId + phonePub + nph + byteArrayOf(MODE_PAIRED) + name(phoneName))
            val keys = WifiCrypto.sessionKeys(z, known, npc, nph)
            if (!finishedFromPc(keys)) { reject(REJECT_AUTH); return null }
            return finish(keys, pcId, pcName, newlyPaired = false)
        }

        // Pairing: commit to Nb before seeing Na, so nobody in between can steer the code.
        val nb = randomBytes(16)
        send(TYPE_SERVER_HELLO, phoneId + phonePub + nph + byteArrayOf(MODE_PAIRING) + WifiCrypto.commit(nb, phonePub, pcPub) + name(phoneName))
        val na = read(TYPE_NONCE_A).also { if (it.size != 16) throw IOException("bad nonce") }
        send(TYPE_NONCE_B, nb)
        val pairKey = WifiCrypto.pairKey(z, na, nb)
        val keys = WifiCrypto.sessionKeys(z, pairKey, npc, nph)
        if (!finishedFromPc(keys)) { reject(REJECT_AUTH); return null }

        val code = WifiCrypto.sas(pcPub, phonePub, na, nb)
        // The PC sends nothing while the user decides. If it hangs up meanwhile (Cancel pairing on the PC, or
        // its timeout), close the question, and never store a key for a pairing the PC abandoned.
        val watch = HangUpWatch().also { it.start() }
        val allowed = try {
            askUser(pcName, code) { watch.gone }
        } finally {
            watch.finish()
        }
        if (watch.gone) throw IOException("the PC hung up while pairing")
        if (!allowed) { reject(REJECT_REFUSED); return null }
        store.put(pcId, pairKey, pcName)
        // The user compared the code and tapped Allow, which is consent for this PC to use the camera. A QR
        // pairing has no such moment, so it does not set this and the phone asks once, after connecting.
        store.allowCamera(pcId)
        return finish(keys, pcId, pcName, newlyPaired = true)
    }

    private fun finishedFromPc(keys: ByteArray): Boolean {
        val expected = WifiCrypto.finished(keys.copyOfRange(64, 96), "PC", transcriptSoFar())
        val got = read(TYPE_PC_FINISHED, addToTranscript = false)
        return MessageDigest.isEqual(expected, got)
    }

    private fun finish(keys: ByteArray, pcId: ByteArray, pcName: String, newlyPaired: Boolean): Result {
        val mac = WifiCrypto.finished(keys.copyOfRange(64, 96), "PH", transcriptSoFar())
        send(TYPE_PHONE_FINISHED, mac, addToTranscript = false)
        socket.soTimeout = 0
        return Result(
            SecureInputStream(socket.getInputStream(), keys.copyOfRange(0, 32), WifiCrypto.DIR_PC_TO_PHONE),
            SecureOutputStream(socket.getOutputStream(), keys.copyOfRange(32, 64), WifiCrypto.DIR_PHONE_TO_PC),
            pcName, newlyPaired, pcId,
        )
    }

    private fun reject(reason: Int) {
        try { send(TYPE_REJECT, byteArrayOf(reason.toByte()), addToTranscript = false) } catch (_: IOException) {}
    }

    // --- Framing: u16 length (type + body) | u8 type | body -----------------------------------------

    private fun send(type: Int, body: ByteArray, addToTranscript: Boolean = true) {
        val frame = ByteArrayOutputStream().apply {
            DataOutputStream(this).apply { writeShort(body.size + 1); writeByte(type); write(body) }
        }.toByteArray()
        if (addToTranscript) transcript.update(frame)
        output.write(frame)
        output.flush()
    }

    private fun read(expected: Int, addToTranscript: Boolean = true): ByteArray {
        val len = input.readUnsignedShort()
        if (len < 1) throw IOException("empty message")
        val type = input.readUnsignedByte()
        val body = ByteArray(len - 1).also { input.readFully(it) }
        if (type == TYPE_REJECT) throw IOException("PC gave up")
        if (type != expected) throw IOException("expected message $expected, got $type")
        if (addToTranscript) transcript.update(byteArrayOf((len shr 8).toByte(), len.toByte(), type.toByte()) + body)
        return body
    }

    /**
     * Reads the socket while the user decides on a pairing: the PC must send nothing then, so end of stream,
     * an error or any byte means it hung up. Polls with a short timeout so [finish] can stop it.
     */
    private inner class HangUpWatch : Thread("wifi-pair-watch") {
        @Volatile var gone = false
            private set
        @Volatile private var done = false

        override fun run() {
            try {
                while (!done) {
                    try {
                        input.read()
                        gone = true
                        return
                    } catch (_: SocketTimeoutException) {
                    }
                }
            } catch (_: IOException) {
                gone = true
            }
        }

        fun finish() {
            done = true
            join()
            socket.soTimeout = 0
        }

        override fun start() {
            socket.soTimeout = WATCH_POLL_MS
            super.start()
        }
    }

    private fun transcriptSoFar(): ByteArray = (transcript.clone() as MessageDigest).digest()

    private fun randomBytes(n: Int) = ByteArray(n).also { random.nextBytes(it) }

    private fun name(s: String): ByteArray {
        val b = s.toByteArray(Charsets.UTF_8).let { if (it.size > 64) it.copyOf(64) else it }
        return byteArrayOf(b.size.toByte()) + b
    }

    private fun readName(d: DataInputStream): String {
        val n = d.readUnsignedByte()
        return String(ByteArray(n).also { d.readFully(it) }, Charsets.UTF_8)
    }

    companion object {
        private const val TAG = "WifiHandshake"
        private val MAGIC = "MCHS".toByteArray()
        private const val VERSION = 1
        private const val FLAG_FORCE_PAIR = 0x01
        private const val FLAG_QR = 0x02
        private const val STEP_TIMEOUT_MS = 10_000
        private const val WATCH_POLL_MS = 250
        const val TYPE_CLIENT_HELLO = 1
        const val TYPE_SERVER_HELLO = 2
        const val TYPE_NONCE_A = 3
        const val TYPE_NONCE_B = 4
        const val TYPE_PC_FINISHED = 5
        const val TYPE_PHONE_FINISHED = 6
        const val TYPE_REJECT = 7
        private const val MODE_PAIRED: Byte = 0
        private const val MODE_PAIRING: Byte = 1
        private const val MODE_QR: Byte = 2
        const val REJECT_REFUSED = 1
        const val REJECT_AUTH = 2
        const val REJECT_BUSY = 3
    }
}

/** A paired PC as the UI shows it: [id] is the hex PC id (the storage key), [name] its name. */
data class PairedPc(val id: String, val name: String)

/** PCs this phone has paired with: their id, pairing key and name (app-private preferences, [PREFS]). */
class PairedPcs(private val prefs: SharedPreferences) {
    fun key(pcId: ByteArray): ByteArray? =
        prefs.getString(PREFIX + hex(pcId), null)?.substringBefore('|')?.let { Base64.decode(it, Base64.NO_WRAP) }

    fun put(pcId: ByteArray, key: ByteArray, name: String) {
        prefs.edit().putString(PREFIX + hex(pcId), Base64.encodeToString(key, Base64.NO_WRAP) + "|" + name).apply()
    }

    fun names(): List<String> = list().map { it.name }

    /** Every paired PC, sorted by name. */
    fun list(): List<PairedPc> = prefs.all.filterKeys { it.startsWith(PREFIX) }
        .map { (k, v) -> PairedPc(k.removePrefix(PREFIX), (v as String).substringAfter('|')) }
        .sortedBy { it.name.lowercase() }

    /**
     * Whether this PC may turn the camera on. Pairing settles that the two sides share a key; it does not
     * settle that the person holding the phone wanted *this* PC to be able to watch. A PC that hasn't been
     * allowed yet still gets a session, with the camera off, and the phone asks once. A key stored before
     * this flag existed reads as not allowed, so an already-paired PC is asked for the first time too.
     */
    fun cameraAllowed(pcId: ByteArray): Boolean = prefs.getBoolean(ALLOW_PREFIX + hex(pcId), false)

    fun allowCamera(pcId: ByteArray) {
        prefs.edit { putBoolean(ALLOW_PREFIX + hex(pcId), true) }
    }

    /** Forgets one PC (by [PairedPc.id]): it has to pair again, with a code, next time. */
    fun forget(id: String) {
        prefs.edit { remove(PREFIX + id); remove(ALLOW_PREFIX + id) }
    }

    fun forgetAll() {
        prefs.edit().apply {
            prefs.all.keys.filter { it.startsWith(PREFIX) || it.startsWith(ALLOW_PREFIX) }.forEach { remove(it) }
        }.apply()
    }

    /** This phone's random id, made once. */
    fun phoneId(): ByteArray {
        prefs.getString(PHONE_ID, null)?.let { s -> return ByteArray(16) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() } }
        val id = ByteArray(16).also { SecureRandom().nextBytes(it) }
        prefs.edit().putString(PHONE_ID, hex(id)).apply()
        return id
    }

    companion object {
        /**
         * Its own preferences file, left out of cloud backup and device transfer (res/xml/backup_rules.xml,
         * data_extraction_rules.xml): a restored or new phone pairs again instead of carrying the keys along.
         */
        const val PREFS = "mycam_pairing"
        private const val PREFIX = "paired_pc_"
        private const val ALLOW_PREFIX = "cam_ok_"
        private const val PHONE_ID = "phone_id"
        private fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }

        fun open(context: Context): PairedPcs {
            val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            migrate(context.getSharedPreferences(WebcamService.PREFS, Context.MODE_PRIVATE), prefs)
            return PairedPcs(prefs)
        }

        /** Up to 1.5.0 these lived in the main preferences file, which is backed up: move them out, once. */
        fun migrate(from: SharedPreferences, to: SharedPreferences) {
            val old = from.all.filterKeys { it.startsWith(PREFIX) || it.startsWith(ALLOW_PREFIX) || it == PHONE_ID }
            if (old.isEmpty()) return
            to.edit(commit = true) { old.forEach { (k, v) -> if (v is Boolean) putBoolean(k, v) else putString(k, v as String) } }
            from.edit(commit = true) { old.keys.forEach { remove(it) } }
        }
    }
}
