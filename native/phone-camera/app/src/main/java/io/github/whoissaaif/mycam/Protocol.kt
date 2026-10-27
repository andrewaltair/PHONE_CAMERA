package io.github.whoissaaif.mycam

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Wire protocol shared with the Windows companion (see pc/companion/protocol.h).
 *
 * Phone -> PC: 20-byte big-endian header followed by `length` payload bytes.
 *   u32 magic 'MCAM' | u8 type | u8 flags | u16 reserved | i64 ptsUs | u32 length
 *
 * PC -> Phone: fixed 8-byte commands.
 *   u32 magic 'MCMD' | u8 cmd | u8 arg | u16 reserved
 */
object Protocol {
    const val VERSION = 3

    // Wireless transport (1.4, PROTOCOL.md "Wireless transport"): UDP discovery, then the same packets over TCP.
    const val WIRELESS_DISCOVERY_PORT = 47801
    const val WIRELESS_TCP_PORT = 47800
    const val WIRELESS_ASK = "MYCAM?1"   // PC -> broadcast: "MYCAM?1 <pc name>"
    const val WIRELESS_HERE = "MYCAM!1"  // phone -> PC: "MYCAM!1 <tcp port> <phone name>"
    // phone -> PC, unicast to the probe's source port: "connect to me now" (either side may start a pairing).
    const val WIRELESS_CONNECT = "MYCAM+1"
    // QR pairing (1.5, PROTOCOL.md "QR pairing"). phone -> PC, unicast to the probe's source port:
    // "MYCAM#1 <tcp port> <proof> <phone name>". PC -> phone when it can't: "MYCAM-1 code" or "MYCAM-1 busy".
    const val WIRELESS_QR = "MYCAM#1"
    const val WIRELESS_REFUSE = "MYCAM-1"

    const val PACKET_MAGIC = 0x4D43414D // 'MCAM'
    const val HEADER_SIZE = 20

    // Phone -> PC packet types
    const val TYPE_HELLO = 0   // payload: u16 protocol version
    const val TYPE_CONFIG = 1  // payload: u16 width, u16 height, u16 sensorOrientation, u8 facing, then SPS/PPS (Annex-B)
    const val TYPE_FRAME = 2   // payload: H.264 Annex-B access unit
    const val TYPE_ORIENT = 3  // payload: u16 device rotation in degrees (0/90/180/270, clockwise)
    const val TYPE_STATE = 4   // payload: u8 state (STATE_*), u8 facing
    const val TYPE_LOG = 5     // payload: UTF-8 diagnostic text (shown in the PC log)
    const val TYPE_CAMERA = 6  // v3: payload: see CameraInfo (settings + capabilities)

    const val FLAG_KEYFRAME = 0x01

    const val STATE_IDLE = 0
    const val STATE_STREAMING = 1
    const val STATE_ERROR = 2
    const val STATE_PAUSED = 3 // v2: camera off because the user paused (on the phone or the PC)

    const val FACING_BACK = 0
    const val FACING_FRONT = 1

    const val COMMAND_MAGIC = 0x4D434D44 // 'MCMD'
    const val COMMAND_SIZE = 8

    // PC -> Phone commands
    const val CMD_HELLO = 1
    const val CMD_START = 2
    const val CMD_STOP = 3
    const val CMD_KEYFRAME = 4
    const val CMD_SET_FACING = 5 // arg: FACING_*
    const val CMD_PAUSE = 6 // v2
    const val CMD_RESUME = 7 // v2
    const val CMD_SET_QUALITY = 8  // v3: arg QUALITY_*
    const val CMD_SET_FPS = 9      // v3: arg 30 or 60 (120 since 1.3.2)
    const val CMD_SET_ZOOM = 10    // v3: arg zoom x10 (5 = 0.5x .. 255 = 25.5x)
    const val CMD_SET_EXPOSURE = 11 // v3: arg signed EV steps (int8)
    const val CMD_SET_TORCH = 12   // v3: arg 0 off, 1 on
    const val CMD_SET_FOCUS = 13   // v3: arg 0 continuous auto focus, 1 focus once and lock

    const val QUALITY_720P = 0
    const val QUALITY_1080P = 1
    const val QUALITY_4K = 2

    // CAMERA flags
    const val CAM_TORCH_AVAILABLE = 0x01
    const val CAM_TORCH_ON = 0x02
    const val CAM_FOCUS_LOCKED = 0x04
    const val CAM_HAS_60FPS = 0x08
    const val CAM_HAS_4K = 0x10
    const val CAM_HAS_AUTOFOCUS = 0x20
    const val CAM_HAS_120FPS = 0x40 // 1.3.2

    // CAMERA frame-rate masks (one byte per quality, 1.3.2)
    const val FPS_30 = 0x01
    const val FPS_60 = 0x02
    const val FPS_120 = 0x04

    fun packet(type: Int, flags: Int, ptsUs: Long, payload: ByteArray, offset: Int = 0, length: Int = payload.size): ByteArray {
        val buf = ByteBuffer.allocate(HEADER_SIZE + length).order(ByteOrder.BIG_ENDIAN)
        buf.putInt(PACKET_MAGIC)
        buf.put(type.toByte())
        buf.put(flags.toByte())
        buf.putShort(0)
        buf.putLong(ptsUs)
        buf.putInt(length)
        buf.put(payload, offset, length)
        return buf.array()
    }

    data class Command(val cmd: Int, val arg: Int)

    /**
     * Phone camera settings and what the camera supports (TYPE_CAMERA payload: 18 bytes, plus since 1.3.2 three
     * FPS_* masks saying which frame rates work at 720p, 1080p and 4K; older PCs ignore the extra bytes).
     */
    data class CameraInfo(
        val quality: Int, val fps: Int,
        val zoomX100: Int, val zoomMinX100: Int, val zoomMaxX100: Int,
        val ev: Int, val evMin: Int, val evMax: Int, val evStepX100: Int,
        val flags: Int, val width: Int, val height: Int, val actualFps: Int,
        val fpsModes: List<Int> = emptyList(),
    ) {
        fun encode(): ByteArray = ByteBuffer.allocate(18 + fpsModes.size).order(ByteOrder.BIG_ENDIAN)
            .put(quality.toByte()).put(fps.toByte())
            .putShort(zoomX100.toShort()).putShort(zoomMinX100.toShort()).putShort(zoomMaxX100.toShort())
            .put(ev.toByte()).put(evMin.toByte()).put(evMax.toByte()).put(evStepX100.toByte())
            .put(flags.toByte()).putShort(width.toShort()).putShort(height.toShort()).put(actualFps.toByte())
            .apply { fpsModes.forEach { put(it.toByte()) } }
            .array()

        /** FPS_* bits that work at [quality]; derived from the older flags if the masks are missing. */
        fun fpsMask(quality: Int): Int = fpsModes.getOrNull(quality)
            ?: (FPS_30 or (if (flags and CAM_HAS_60FPS != 0) FPS_60 else 0) or (if (flags and CAM_HAS_120FPS != 0) FPS_120 else 0))
    }

    /** Splits the PC's byte stream into commands, skipping garbage until the next valid magic. */
    class CommandParser {
        private var pending = ByteArray(0)

        fun feed(bytes: ByteArray, length: Int = bytes.size): List<Command> {
            val data = pending + bytes.copyOfRange(0, length)
            val buf = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN)
            val commands = mutableListOf<Command>()
            var pos = 0
            while (data.size - pos >= COMMAND_SIZE) {
                // Only accept plausible commands so stray magic bytes in garbage can't desync the stream.
                val cmd = data[pos + 4].toInt() and 0xFF
                val valid = buf.getInt(pos) == COMMAND_MAGIC && cmd in CMD_HELLO..CMD_SET_FOCUS &&
                    data[pos + 6].toInt() == 0 && data[pos + 7].toInt() == 0
                if (!valid) {
                    pos++ // Resync byte by byte.
                    continue
                }
                commands += Command(cmd, data[pos + 5].toInt() and 0xFF)
                pos += COMMAND_SIZE
            }
            pending = data.copyOfRange(pos, data.size)
            return commands
        }
    }
}
