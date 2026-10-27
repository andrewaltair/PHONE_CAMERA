package io.github.whoissaaif.mycam

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.File

/** Checks Protocol.kt against the shared byte-exact vectors in protocol/golden.txt. */
class ProtocolTest {

    private val golden: Map<String, ByteArray> by lazy {
        // Gradle runs unit tests from the app module directory.
        File("../protocol/golden.txt").readLines()
            .map { it.substringBefore('#').trim() }
            .filter { it.contains('=') }
            // Only the wire protocol's entries: others (wifi.sas, qr.fingerprint...) are text, not hex.
            .filter { it.startsWith("packet.") || it.startsWith("command.") }
            .associate { line ->
                val (name, value) = line.split('=', limit = 2).map { it.trim() }
                name to hex(value)
            }
    }

    private fun hex(s: String) = s.replace(" ", "").chunked(2).map { it.toInt(16).toByte() }.toByteArray()

    @Test
    fun packetsMatchGolden() {
        assertArrayEquals(golden.getValue("packet.hello"), Protocol.packet(Protocol.TYPE_HELLO, 0, 0, hex("0001")))
        assertArrayEquals(
            golden.getValue("packet.config"),
            Protocol.packet(Protocol.TYPE_CONFIG, 0, 0, hex("0780 0438 005A 01 0000000167")),
        )
        assertArrayEquals(
            golden.getValue("packet.frame"),
            Protocol.packet(Protocol.TYPE_FRAME, Protocol.FLAG_KEYFRAME, 0x0102030405060708L, hex("DEADBEEF")),
        )
        assertArrayEquals(golden.getValue("packet.orient"), Protocol.packet(Protocol.TYPE_ORIENT, 0, 0, hex("010E")))
        assertArrayEquals(
            golden.getValue("packet.state"),
            Protocol.packet(
                Protocol.TYPE_STATE, 0, 0,
                byteArrayOf(Protocol.STATE_STREAMING.toByte(), Protocol.FACING_FRONT.toByte()),
            ),
        )
        assertArrayEquals(golden.getValue("packet.log"), Protocol.packet(Protocol.TYPE_LOG, 0, 0, "hi".toByteArray()))
        assertArrayEquals(
            golden.getValue("packet.state_paused"),
            Protocol.packet(Protocol.TYPE_STATE, 0, 0, byteArrayOf(Protocol.STATE_PAUSED.toByte(), Protocol.FACING_BACK.toByte())),
        )
        assertArrayEquals(
            golden.getValue("packet.camera"),
            Protocol.packet(
                Protocol.TYPE_CAMERA, 0, 0,
                Protocol.CameraInfo(
                    Protocol.QUALITY_1080P, 30, 100, 60, 1000, 0, -12, 12, 33,
                    Protocol.CAM_TORCH_AVAILABLE or Protocol.CAM_HAS_60FPS or Protocol.CAM_HAS_4K, 1920, 1080, 30,
                ).encode(),
            ),
        )
        val all = Protocol.FPS_30 or Protocol.FPS_60 or Protocol.FPS_120
        val withModes = Protocol.CameraInfo(
            Protocol.QUALITY_1080P, 120, 100, 100, 1000, 0, -12, 12, 33,
            Protocol.CAM_TORCH_AVAILABLE or Protocol.CAM_HAS_60FPS or Protocol.CAM_HAS_4K or Protocol.CAM_HAS_120FPS,
            1920, 1080, 120, fpsModes = listOf(all, all, Protocol.FPS_30),
        )
        assertArrayEquals(golden.getValue("packet.camera_modes"), Protocol.packet(Protocol.TYPE_CAMERA, 0, 0, withModes.encode()))
        assertEquals(Protocol.FPS_30, withModes.fpsMask(Protocol.QUALITY_4K)) // 4K: 60/120 must not be offered.
        assertEquals(all, withModes.fpsMask(Protocol.QUALITY_1080P))
    }

    @Test
    fun commandsMatchGolden() {
        val expected = mapOf(
            "command.hello" to Protocol.Command(Protocol.CMD_HELLO, 0),
            "command.start" to Protocol.Command(Protocol.CMD_START, 0),
            "command.stop" to Protocol.Command(Protocol.CMD_STOP, 0),
            "command.keyframe" to Protocol.Command(Protocol.CMD_KEYFRAME, 0),
            "command.facing_front" to Protocol.Command(Protocol.CMD_SET_FACING, Protocol.FACING_FRONT),
            "command.pause" to Protocol.Command(Protocol.CMD_PAUSE, 0),
            "command.resume" to Protocol.Command(Protocol.CMD_RESUME, 0),
            "command.quality_4k" to Protocol.Command(Protocol.CMD_SET_QUALITY, Protocol.QUALITY_4K),
            "command.fps_60" to Protocol.Command(Protocol.CMD_SET_FPS, 60),
            "command.fps_120" to Protocol.Command(Protocol.CMD_SET_FPS, 120),
            "command.zoom_2x" to Protocol.Command(Protocol.CMD_SET_ZOOM, 20),
            "command.exposure_m2" to Protocol.Command(Protocol.CMD_SET_EXPOSURE, (-2).toByte().toInt() and 0xFF),
            "command.torch_on" to Protocol.Command(Protocol.CMD_SET_TORCH, 1),
            "command.focus_lock" to Protocol.Command(Protocol.CMD_SET_FOCUS, 1),
        )
        for ((name, command) in expected) {
            assertEquals(name, listOf(command), Protocol.CommandParser().feed(golden.getValue(name)))
        }
    }

    @Test
    fun commandParserHandlesSplitsBatchesAndGarbage() {
        val start = golden.getValue("command.start")
        val stop = golden.getValue("command.stop")
        val parser = Protocol.CommandParser()

        // Split across reads.
        assertEquals(emptyList<Protocol.Command>(), parser.feed(start.copyOfRange(0, 3)))
        assertEquals(listOf(Protocol.Command(Protocol.CMD_START, 0)), parser.feed(start.copyOfRange(3, start.size)))

        // Garbage first, then two commands in one read.
        assertEquals(
            listOf(Protocol.Command(Protocol.CMD_START, 0), Protocol.Command(Protocol.CMD_STOP, 0)),
            parser.feed(hex("00 FF 4D 43") + start + stop),
        )

        // A stray magic followed by an invalid command must not hide the real command after it.
        assertEquals(
            listOf(Protocol.Command(Protocol.CMD_STOP, 0)),
            Protocol.CommandParser().feed(hex("4D434D44 FF 00 1234") + stop),
        )

        // A read far larger than the old fixed 4 KiB buffer.
        assertEquals(
            listOf(Protocol.Command(Protocol.CMD_START, 0)),
            Protocol.CommandParser().feed(ByteArray(20000) + start),
        )
    }
}
