package io.github.whoissaaif.mycam

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Rect
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.graphics.SurfaceTexture
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.SystemClock
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import kotlin.math.roundToInt

/**
 * Streams one camera into a hardware H.264 encoder via its input Surface, at the requested quality and
 * frame rate, with live camera controls (zoom, exposure, torch, focus).
 * All callbacks run on [handler]'s thread; call every method from that thread too.
 */
class CameraStreamer(
    context: Context,
    private val handler: Handler,
    private val facing: Int,
    private var settings: Settings,
    private val listener: Listener,
) {
    /** What the user chose; the phone stores it and the PC can change it (protocol v3). */
    data class Settings(
        val quality: Int = Protocol.QUALITY_1080P,
        val fps: Int = 30,
        val zoom: Float = 1f,
        val ev: Int = 0,
        val torch: Boolean = false,
        val focusLocked: Boolean = false,
    )

    interface Listener {
        /** Whether this mode carries the phone's own preview, and why not when it doesn't. */
        fun onPreviewSupport(support: PreviewSupport) {}

        /** Codec config (SPS/PPS) is ready. Called before the first frame. */
        fun onConfig(width: Int, height: Int, sensorOrientation: Int, facing: Int, csd: ByteArray)
        fun onFrame(data: ByteArray, ptsUs: Long, keyFrame: Boolean)
        fun onError(message: String)
        /** Settings actually in effect and what this camera supports. Sent on start and after each change. */
        fun onCameraInfo(info: Protocol.CameraInfo) {}
        /** Diagnostic text, forwarded to the PC log. */
        fun onLog(message: String) {}
    }

    /** What the opened camera can do. */
    private class Caps(
        val zoomMin: Float, val zoomMax: Float, val useZoomRatio: Boolean, val activeArray: Rect?,
        val evMin: Int, val evMax: Int, val evStep: Float,
        val torch: Boolean, val autofocus: Boolean, val has60: Boolean, val has4K: Boolean,
        val has120: Boolean = false,
    )

    private val cameraManager = context.getSystemService(CameraManager::class.java)
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var encoder: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var request: CaptureRequest.Builder? = null
    private var caps: Caps? = null
    private var stopped = false
    private var outputCount = 0
    private var reported = false
    private var configSent = false

    private var width = 0
    private var height = 0
    private var fps = 30
    private var sensorOrientation = 0
    private var realtimeTimestamps = false // Sensor timestamps use elapsedRealtime (else the monotonic clock).
    private var latencySumUs = 0L
    private var latencyCount = 0
    private var cameraLatencySumUs = 0L
    private var cameraLatencyCount = 0
    private var lastLatencyLog = 0L
    private var latencyModes: (CaptureRequest.Builder) -> Unit = {}
    private var captureFps = 30                       // Camera rate; above fps in high-speed mode (encoder drops the rest).
    private var fallbackRange: Range<Int> = Range(30, 30) // Normal session if the high-speed one is refused.
    private var exposureNs = 0L // Latest exposure time and ISO, logged with the latency (explains noise).
    private var iso = 0
    private var fpsModes = IntArray(3) // Per quality (720p, 1080p, 4K): Protocol.FPS_* bits that work.

    // The phone's own preview (docs/redesign-v2.md 6.1): a second, deferred output of the same session, so the
    // encoder stream never changes and the camera is never restarted. Owned by the camera thread.
    private var previewSize: Size? = null
    private var previewConfig: OutputConfiguration? = null  // API 28+, created with the session.
    private var previewSurface: Surface? = null             // The surface currently in the repeating request.
    private var previewFinalized = false
    private var previewWanted: Surface? = null              // What the UI last asked for.
    private var previewRefused = false                      // This camera refused the second output; don't retry.

    /** How a camera delivers a quality at a frame rate: output size, capture range, normal or high-speed. */
    private class Mode(val size: Size, val fps: Int, val range: Range<Int>, val highSpeed: Boolean)

    // Measures how long the camera pipeline takes (sensor timestamp to capture result) separately from the
    // encoder, so the log shows where the phone-side delay comes from.
    private val captureCallback = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureCompleted(s: CameraCaptureSession, r: CaptureRequest, result: TotalCaptureResult) {
            exposureNs = result.get(CaptureResult.SENSOR_EXPOSURE_TIME) ?: exposureNs
            iso = result.get(CaptureResult.SENSOR_SENSITIVITY) ?: iso
            val ts = result.get(CaptureResult.SENSOR_TIMESTAMP) ?: return
            val us = sinceSensorUs(ts / 1000)
            if (us in 0..2_000_000) {
                cameraLatencySumUs += us
                cameraLatencyCount++
            }
        }
    }

    @SuppressLint("MissingPermission") // Caller checks CAMERA permission before starting.
    fun start() {
        try {
            logCameraReport()
            val ids = sameFacingIds()
            if (ids.isEmpty()) {
                listener.onError(if (facing == Protocol.FACING_FRONT) "No front camera" else "No back camera")
                return
            }
            // The highest rate up to the one asked for that a camera facing this way can do at this quality
            // (some phones only offer high frame rates on a secondary camera id). 30 always works.
            val (cameraId, mode) = FPS_STEPS.filter { it <= settings.fps }.firstNotNullOfOrNull { f ->
                ids.firstNotNullOfOrNull { id -> modeFor(id, settings.quality, f)?.let { id to it } }
            } ?: (ids.first() to modeFor(ids.first(), settings.quality, 30)!!)
            val chars = cameraManager.getCameraCharacteristics(cameraId)
            sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
            realtimeTimestamps = chars.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) ==
                CameraMetadata.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME
            val sizes = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                ?.getOutputSizes(MediaCodec::class.java) ?: emptyArray()

            width = mode.size.width
            height = mode.size.height
            fps = mode.fps
            captureFps = if (mode.highSpeed) mode.range.upper else fps
            fallbackRange = chooseFpsRange(chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES), 30)
            // The preview's own output: a smaller size with the stream's aspect ratio, so it costs little.
            previewSize = if (PhonePreview.possible) previewSizeFor(chars, mode.size) else null

            // Which frame rates each quality can have on this phone (any camera facing this way), so the phone
            // and PC only offer combinations that work.
            fpsModes = IntArray(3) { q ->
                FPS_STEPS.filter { f -> ids.any { id -> modeFor(id, q, f) != null } }
                    .fold(0) { mask, f -> mask or fpsBit(f) }
            }
            val q = settings.quality
            caps = readCaps(chars).let {
                Caps(
                    it.zoomMin, it.zoomMax, it.useZoomRatio, it.activeArray, it.evMin, it.evMax, it.evStep,
                    it.torch, it.autofocus,
                    has60 = fpsModes[q] and fpsBit(60) != 0,
                    has4K = sizes.any { s -> s.width == 3840 && s.height == 2160 } && encoderSupports(3840, 2160, 30),
                    has120 = fpsModes[q] and fpsBit(120) != 0,
                )
            }
            latencyModes = lowLatencyModes(chars, mode.highSpeed)
            listener.onLog(
                "camera $cameraId: ${width}x$height @ $fps fps (asked ${settings.fps}), AE ${mode.range}, sensor $sensorOrientation" +
                    (if (mode.highSpeed) ", high-speed capture at ${mode.range.upper} fps" else "") +
                    "; fps per quality 720p/1080p/4K: " + fpsModes.joinToString(" / ") { m -> describeFps(m) }
            )
            val fpsRange = mode.range
            val highSpeed = mode.highSpeed

            startEncoder()
            cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(device: CameraDevice) {
                    if (stopped) { device.close(); return }
                    camera = device
                    listener.onLog("camera opened")
                    createSession(device, fpsRange, highSpeed)
                }

                override fun onDisconnected(device: CameraDevice) {
                    device.close()
                    if (!stopped) listener.onError("Camera disconnected (in use by another app?)")
                }

                override fun onError(device: CameraDevice, error: Int) {
                    device.close()
                    if (!stopped) listener.onError("Camera error $error")
                }
            }, handler)
        } catch (e: Exception) {
            Log.e(TAG, "start failed", e)
            listener.onError("Camera start failed: ${e.message}")
        }
    }

    /** Changes zoom / exposure / torch / focus on the running camera (quality and fps need a restart). */
    fun updateControls(newSettings: Settings) {
        val old = settings
        settings = newSettings
        val s = session ?: return
        val b = request ?: return
        try {
            if (newSettings.focusLocked != old.focusLocked && caps?.autofocus == true) {
                if (newSettings.focusLocked) {
                    // Focus once on the current scene, then hold it.
                    b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO)
                    b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_START)
                    captureOnce(s, b.build())
                    b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
                } else {
                    b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_CANCEL)
                    captureOnce(s, b.build())
                    b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
                }
            }
            applyControls(b)
            repeat(s, b.build())
        } catch (e: Exception) {
            listener.onLog("controls not applied: ${e.message}")
        }
        reportInfo()
    }

    fun requestKeyFrame() {
        encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
    }

    /**
     * Shows (or stops showing) the camera on the phone's own screen. [surface] comes from the Camera
     * screen's TextureView; null turns the preview off.
     *
     * The preview is a **second output of the running session**: its output configuration is created
     * deferred with the session and finalised when the surface arrives, so neither the encoder stream nor
     * the camera is touched. Turning it off removes it from the repeating request only. If the camera
     * refuses it (or this mode can't take it), the preview is dropped and the reason is reported; the
     * stream is never sacrificed for it.
     */
    fun setPreviewSurface(surface: Surface?) {
        previewWanted = surface
        if (surface == null) {
            detachPreview()
            return
        }
        if (session != null) attachPreview(surface)
    }

    /** The size the preview surface should hold (the Camera screen sets the buffer size to it). */
    fun previewSize(): Size? = previewSize

    private fun attachPreview(s: Surface) {
        val sess = session ?: return
        val b = request ?: return
        val cfg = previewConfig
        if (cfg == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            listener.onPreviewSupport(if (previewRefused) PreviewSupport.Refused else PreviewSupport.NotAtThisFrameRate)
            return
        }
        if (previewSurface === s) return
        try {
            if (!previewFinalized) {
                cfg.addSurface(s)
                sess.finalizeOutputConfigurations(listOf(cfg))
                previewFinalized = true
            } else {
                previewSurface?.let { old ->
                    b.removeTarget(old)
                    cfg.removeSurface(old)
                }
                cfg.addSurface(s)
                sess.updateOutputConfiguration(cfg)
            }
            previewSurface = s
            b.addTarget(s)
            repeat(sess, b.build())
            listener.onLog("preview on (${previewSize?.width}x${previewSize?.height})")
            listener.onPreviewSupport(PreviewSupport.Ok)
        } catch (e: Exception) {
            previewRefused = true
            previewSurface?.let { old -> try { b.removeTarget(old) } catch (_: Exception) {} }
            previewSurface = null
            try { repeat(sess, b.build()) } catch (_: Exception) {}
            listener.onLog("preview refused: ${e.message}")
            listener.onPreviewSupport(PreviewSupport.Refused)
        }
    }

    private fun detachPreview() {
        val s = previewSurface ?: return
        previewSurface = null
        val sess = session ?: return
        val b = request ?: return
        try {
            b.removeTarget(s)
            repeat(sess, b.build())
            listener.onLog("preview off")
        } catch (e: Exception) {
            listener.onLog("preview could not be removed: ${e.message}")
        }
    }

    fun stop() {
        stopped = true
        try { session?.close() } catch (_: Exception) {}
        try { camera?.close() } catch (_: Exception) {}
        try { encoder?.stop() } catch (_: Exception) {}
        try { encoder?.release() } catch (_: Exception) {}
        inputSurface?.release()
        session = null; camera = null; encoder = null; inputSurface = null; request = null
        previewConfig = null; previewSurface = null; previewFinalized = false
    }

    /** Cameras facing the requested way, in the order the phone lists them (the main one first). */
    private fun sameFacingIds(): List<String> {
        val wanted = if (facing == Protocol.FACING_FRONT) CameraCharacteristics.LENS_FACING_FRONT
        else CameraCharacteristics.LENS_FACING_BACK
        return cameraManager.cameraIdList.filter {
            cameraManager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == wanted
        }
    }

    /**
     * How camera [id] would deliver [quality] at [fps], or null if this phone can't. The one place that decides
     * what works: start() uses it to open the camera and the capability report uses it for the UI, so
     * nothing is offered that wouldn't start. Checks, for the exact size the quality maps to:
     *  - the encoder accepts that size at that rate;
     *  - a normal mode reaches the rate (an AE range up to it, and a short enough minimum frame duration), or
     *  - a high-speed mode captures at the rate or a multiple of it (the encoder then drops the extra frames).
     */
    private fun modeFor(id: String, quality: Int, fps: Int): Mode? {
        val chars = cameraManager.getCameraCharacteristics(id)
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return null
        val sizes = map.getOutputSizes(MediaCodec::class.java) ?: return null
        if (sizes.isEmpty()) return null
        val size = chooseSize(sizes, quality) { s -> encoderSupports(s.width, s.height, 30) }
        val aeRanges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
        if (fps <= 30) return Mode(size, 30, chooseFpsRange(aeRanges, 30), false)
        if (!encoderSupports(size.width, size.height, fps)) return null
        val minFrameNs = map.getOutputMinFrameDuration(MediaCodec::class.java, size)
        val normalMax = if (minFrameNs > 0) 1_000_000_000.0 / minFrameNs else 30.0
        if (aeRanges?.any { it.upper >= fps } == true && normalMax >= fps - 1) {
            return Mode(size, fps, chooseFpsRange(aeRanges, fps), false)
        }
        return highSpeedRange(chars, size, fps)?.let { Mode(size, fps, it, true) }
    }

    /**
     * The high-speed (constrained) capture range to use for [target] fps output at [size]: the lowest fixed
     * range that is a multiple of [target], so the encoder can keep an even every-Nth frame. Needs Android 10
     * for the encoder-side frame dropping (KEY_MAX_FPS_TO_ENCODER).
     */
    private fun highSpeedRange(chars: CameraCharacteristics, size: Size, target: Int): Range<Int>? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return null
        val caps = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: return null
        if (CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO !in caps) return null
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return null
        if (map.highSpeedVideoSizes.none { it == size }) return null
        return map.getHighSpeedVideoFpsRangesFor(size)
            .filter { it.lower == it.upper && it.upper >= target && it.upper % target == 0 }
            .minByOrNull { it.upper }
    }

    /** Repeating request; a high-speed session needs it as a burst (one request per captured frame batch). */
    private fun repeat(s: CameraCaptureSession, req: CaptureRequest) {
        if (s is CameraConstrainedHighSpeedCaptureSession) s.setRepeatingBurst(s.createHighSpeedRequestList(req), captureCallback, handler)
        else s.setRepeatingRequest(req, captureCallback, handler)
    }

    private fun captureOnce(s: CameraCaptureSession, req: CaptureRequest) {
        if (s is CameraConstrainedHighSpeedCaptureSession) s.captureBurst(s.createHighSpeedRequestList(req), null, handler)
        else s.capture(req, null, handler)
    }

    /** One-time report of what every camera offers, so frame-rate limits can be explained from the PC log. */
    private fun logCameraReport() {
        if (reported) return
        reported = true
        listener.onLog("encoder 1080p: " + listOf(30, 60, 120).joinToString { "$it fps ${if (encoderSupports(1920, 1080, it)) "yes" else "no"}" })
        for (id in cameraManager.cameraIdList) {
            try {
                val c = cameraManager.getCameraCharacteristics(id)
                val facingName = when (c.get(CameraCharacteristics.LENS_FACING)) {
                    CameraCharacteristics.LENS_FACING_FRONT -> "front"
                    CameraCharacteristics.LENS_FACING_BACK -> "back"
                    else -> "external"
                }
                val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                val ns = map?.getOutputMinFrameDuration(MediaCodec::class.java, Size(1920, 1080)) ?: 0L
                val max1080 = if (ns > 0) (1_000_000_000.0 / ns).roundToInt() else 0
                val ranges = c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)?.joinToString(" ") ?: "-"
                val caps = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: IntArray(0)
                val highSpeed = CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO in caps
                val hsSizes = if (highSpeed) map?.highSpeedVideoSizes?.joinToString(" ") { s ->
                    "${s.width}x${s.height}:" + map.getHighSpeedVideoFpsRangesFor(s).joinToString(",")
                } ?: "" else ""
                val physical = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) c.physicalCameraIds.joinToString(",") else ""
                listener.onLog(
                    "camera report $id ($facingName): AE ranges $ranges; 1080p max ${max1080} fps; " +
                        "high-speed ${if (highSpeed) hsSizes else "no"}" + (if (physical.isNotEmpty()) "; physical $physical" else "")
                )
            } catch (e: Exception) {
                listener.onLog("camera report $id: ${e.message}")
            }
        }
    }

    private fun readCaps(chars: CameraCharacteristics): Caps {
        val zoomRatioRange = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) else null
        val maxDigital = chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1f
        val ev = chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE) ?: Range(0, 0)
        val evStep = chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toFloat() ?: 0f
        val afModes = chars.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: IntArray(0)
        return Caps(
            zoomMin = zoomRatioRange?.lower ?: 1f,
            zoomMax = zoomRatioRange?.upper ?: maxDigital,
            useZoomRatio = zoomRatioRange != null,
            activeArray = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE),
            evMin = ev.lower, evMax = ev.upper, evStep = evStep,
            torch = chars.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true,
            autofocus = CaptureRequest.CONTROL_AF_MODE_AUTO in afModes,
            has60 = false, has4K = false,
        )
    }

    /** Zoom, exposure, torch and focus mode from [settings], clamped to what the camera supports. */
    private fun applyControls(b: CaptureRequest.Builder) {
        val c = caps ?: return
        val zoom = settings.zoom.coerceIn(c.zoomMin, c.zoomMax)
        if (c.useZoomRatio && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            b.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom)
        } else if (c.activeArray != null) {
            val a = c.activeArray
            val cw = (a.width() / zoom).toInt()
            val ch = (a.height() / zoom).toInt()
            val l = a.left + (a.width() - cw) / 2
            val t = a.top + (a.height() - ch) / 2
            b.set(CaptureRequest.SCALER_CROP_REGION, Rect(l, t, l + cw, t + ch))
        }
        b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, settings.ev.coerceIn(c.evMin, c.evMax))
        b.set(
            CaptureRequest.FLASH_MODE,
            if (settings.torch && c.torch) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF,
        )
        if (c.autofocus) {
            b.set(
                CaptureRequest.CONTROL_AF_MODE,
                if (settings.focusLocked) CaptureRequest.CONTROL_AF_MODE_AUTO else CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO,
            )
        }
    }

    private fun reportInfo() {
        val c = caps ?: return
        var flags = 0
        if (c.torch) flags = flags or Protocol.CAM_TORCH_AVAILABLE
        if (settings.torch && c.torch) flags = flags or Protocol.CAM_TORCH_ON
        if (settings.focusLocked && c.autofocus) flags = flags or Protocol.CAM_FOCUS_LOCKED
        if (c.has60) flags = flags or Protocol.CAM_HAS_60FPS
        if (c.has4K) flags = flags or Protocol.CAM_HAS_4K
        if (c.has120) flags = flags or Protocol.CAM_HAS_120FPS
        if (c.autofocus) flags = flags or Protocol.CAM_HAS_AUTOFOCUS
        listener.onCameraInfo(
            Protocol.CameraInfo(
                quality = settings.quality, fps = settings.fps,
                zoomX100 = (settings.zoom.coerceIn(c.zoomMin, c.zoomMax) * 100).roundToInt(),
                zoomMinX100 = (c.zoomMin * 100).roundToInt(), zoomMaxX100 = (c.zoomMax * 100).roundToInt().coerceAtMost(65535),
                ev = settings.ev.coerceIn(c.evMin, c.evMax), evMin = c.evMin, evMax = c.evMax,
                evStepX100 = (c.evStep * 100).roundToInt(),
                flags = flags, width = width, height = height, actualFps = fps, fpsModes = fpsModes.toList(),
            )
        )
    }

    /**
     * Microseconds from a sensor timestamp (in us) until now. Which clock the sensor uses varies by phone (the
     * reported source isn't always right), so try the reported one first, then the other.
     */
    private fun sinceSensorUs(sensorUs: Long): Long {
        val realtime = SystemClock.elapsedRealtimeNanos() / 1000 - sensorUs
        val monotonic = System.nanoTime() / 1000 - sensorUs
        val preferred = if (realtimeTimestamps) realtime else monotonic
        return if (preferred in 0..2_000_000) preferred else if (realtimeTimestamps) monotonic else realtime
    }

    /**
     * Phone-side latency, logged every 5 s: sensor to capture result (camera pipeline) and sensor to encoded
     * frame (total). The encoder's share is roughly the difference.
     */
    private fun measureLatency(ptsUs: Long) {
        val latency = sinceSensorUs(ptsUs)
        if (latency in 0..2_000_000) {
            latencySumUs += latency
            latencyCount++
        }
        val nowMs = SystemClock.elapsedRealtime()
        if (nowMs - lastLatencyLog >= 5000) {
            if (latencyCount > 0) {
                val total = latencySumUs / latencyCount / 1000
                val camera = if (cameraLatencyCount > 0) cameraLatencySumUs / cameraLatencyCount / 1000 else -1
                listener.onLog(
                    "latency: capture to encoded avg $total ms over $latencyCount frames" +
                        (if (camera >= 0) " (camera $camera ms, encoder ~${total - camera} ms)" else "") +
                        "; exposure ${"%.1f".format(exposureNs / 1e6)} ms, ISO $iso"
                )
            } else {
                listener.onLog("latency: timestamps not comparable (diff ${latency / 1000} ms)")
            }
            latencySumUs = 0
            latencyCount = 0
            cameraLatencySumUs = 0
            cameraLatencyCount = 0
            lastLatencyLog = nowMs
        }
    }

    /**
     * Camera settings that trade a little quality for delay, where the camera offers them: no electronic
     * stabilisation (it holds frames back to look ahead) and the fast noise-reduction and edge paths.
     * High-speed capture is the exception: its short exposures are noisy, so it gets full noise reduction
     * and no extra sharpening (which would sharpen the noise too).
     */
    private fun lowLatencyModes(chars: CameraCharacteristics, highSpeed: Boolean): (CaptureRequest.Builder) -> Unit {
        val eis = chars.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES) ?: IntArray(0)
        val nr = chars.get(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES) ?: IntArray(0)
        val edge = chars.get(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES) ?: IntArray(0)
        listener.onLog("camera modes: stabilization ${eis.joinToString()}, noise ${nr.joinToString()}, edge ${edge.joinToString()}")
        val nrMode = if (highSpeed) CameraMetadata.NOISE_REDUCTION_MODE_HIGH_QUALITY else CameraMetadata.NOISE_REDUCTION_MODE_FAST
        val edgeMode = if (highSpeed) CameraMetadata.EDGE_MODE_OFF else CameraMetadata.EDGE_MODE_FAST
        return { b ->
            if (CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF in eis)
                b.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF)
            if (nrMode in nr) b.set(CaptureRequest.NOISE_REDUCTION_MODE, nrMode)
            if (edgeMode in edge) b.set(CaptureRequest.EDGE_MODE, edgeMode)
        }
    }

    /** Changes the encoder bitrate live (used to adapt to what the USB link can carry). */
    fun setBitrate(bitsPerSecond: Int) {
        encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitsPerSecond) })
    }

    /** The bitrate chosen for the current mode, before any adaptation. */
    val targetBitrate: Int get() = bitrateFor(width, height, fps)
    val frameIntervalMs: Int get() = 1000 / fps

    private fun encoderFormat(lowLatencyExtras: Boolean): MediaFormat =
        MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateFor(width, height, fps))
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
            if (captureFps > fps && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                // High-speed capture: the input surface drops frames so only `fps` of them are encoded.
                setFloat(MediaFormat.KEY_MAX_FPS_TO_ENCODER, fps.toFloat())
            }
            if (lowLatencyExtras) {
                // Resend the last frame only if the camera stalls (e.g. while reconfiguring). Exactly one frame
                // interval made every slightly late frame a duplicate: 30 fps streams came out at ~42 fps.
                setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 200_000L)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) setInteger(MediaFormat.KEY_LATENCY, 1)
                setInteger(MediaFormat.KEY_PRIORITY, 0) // Real-time priority.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
                    setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0) // B-frames make the encoder hold frames back.
                }
            }
        }

    private fun startEncoder() {
        val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        codec.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {}

            override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                if (stopped) return
                try {
                    if (outputCount++ == 0) listener.onLog("first encoder output: flags=${info.flags} size=${info.size}")
                    val buf = codec.getOutputBuffer(index)
                    if (buf != null && info.size > 0) {
                        val bytes = ByteArray(info.size)
                        buf.position(info.offset)
                        buf.get(bytes, 0, info.size)
                        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                            listener.onConfig(width, height, sensorOrientation, facing, bytes)
                            configSent = true
                        } else {
                            val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                            if (!configSent) {
                                // No config seen at all: announce the stream anyway; SPS/PPS travel inside key frames.
                                configSent = true
                                listener.onConfig(width, height, sensorOrientation, facing, ByteArray(0))
                            }
                            listener.onFrame(bytes, info.presentationTimeUs, key)
                            measureLatency(info.presentationTimeUs)
                        }
                    }
                    codec.releaseOutputBuffer(index, false)
                } catch (e: IllegalStateException) {
                    // Codec was stopped concurrently; ignore.
                }
            }

            override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                if (!stopped) listener.onError("Encoder error: ${e.diagnosticInfo}")
            }

            override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                // Some encoders (e.g. MediaTek with PREPEND_HEADER_TO_SYNC_FRAMES) never emit a separate
                // CODEC_CONFIG buffer; the output format always carries SPS (csd-0) and PPS (csd-1).
                if (stopped) return
                val csd = listOfNotNull(format.getByteBuffer("csd-0"), format.getByteBuffer("csd-1"))
                    .map { b -> ByteArray(b.remaining()).also { b.duplicate().get(it) } }
                    .fold(ByteArray(0)) { acc, part -> acc + part }
                listener.onLog("encoder format: $format")
                configSent = true
                listener.onConfig(width, height, sensorOrientation, facing, csd)
            }
        }, handler)
        try {
            codec.configure(encoderFormat(lowLatencyExtras = true), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        } catch (e: Exception) {
            // Some vendor encoders reject the optional low-latency keys; retry without them.
            Log.w(TAG, "Encoder rejected format, retrying with basic settings", e)
            codec.reset()
            codec.configure(encoderFormat(lowLatencyExtras = false), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }
        inputSurface = codec.createInputSurface()
        codec.start()
        enableVendorLowLatency(codec)
        listener.onLog("encoder ${codec.name} started, ${bitrateFor(width, height, fps) / 1_000_000} Mbps")
        encoder = codec
    }

    /**
     * Many hardware encoders keep a few frames queued unless a vendor-specific low-latency switch is on
     * (Qualcomm: "vendor.qti-ext-enc-low-latency.enable"; MediaTek and others use their own names). Android 12+
     * lists them, so turn on anything that looks like one and log the full list for later tuning.
     */
    private fun enableVendorLowLatency(codec: MediaCodec) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return
        try {
            val params = codec.supportedVendorParameters
            listener.onLog("encoder vendor parameters: ${params.joinToString().ifEmpty { "none" }}")
            val lowLatency = params.filter { p ->
                val n = p.lowercase().replace('_', '-')
                "low-latency" in n || "lowlatency" in n
            }
            if (lowLatency.isEmpty()) return
            codec.setParameters(Bundle().apply { lowLatency.forEach { putInt(it, 1) } })
            listener.onLog("encoder low-latency on: ${lowLatency.joinToString()}")
        } catch (e: Exception) {
            listener.onLog("encoder vendor parameters failed: ${e.message}")
        }
    }

    @Suppress("DEPRECATION") // List<Surface> overloads work on all supported API levels.
    private fun createSession(device: CameraDevice, fpsRange: Range<Int>, highSpeed: Boolean) {
        val surface = inputSurface ?: return
        // A refused high-speed session falls back to a normal one at 30 fps instead of failing the stream.
        fun fallBack(reason: String) {
            listener.onLog("high-speed capture unavailable ($reason), using $fallbackRange")
            fps = 30
            captureFps = 30
            createSession(device, fallbackRange, false)
        }
        // The phone's own preview rides along as a second, deferred output. A constrained high-speed session
        // takes no extra output, and Android 8 can't finalise one later, so those modes have no preview.
        val size = previewSize
        val previewOut = if (
            !highSpeed && !previewRefused && size != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
        ) {
            try {
                OutputConfiguration(size, SurfaceTexture::class.java).apply { enableSurfaceSharing() }
            } catch (e: Exception) {
                listener.onLog("preview output not available: ${e.message}")
                null
            }
        } else {
            null
        }
        previewConfig = previewOut
        previewFinalized = false
        previewSurface = null
        val callback = object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(s: CameraCaptureSession) {
                if (stopped) { s.close(); return }
                session = s
                val b = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                    addTarget(surface)
                    set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange)
                    set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                    latencyModes(this)
                }
                applyControls(b)
                request = b
                try {
                    repeat(s, b.build())
                    listener.onLog("capture started" + if (highSpeed) " (high-speed $fpsRange)" else "")
                    reportInfo()
                } catch (e: Exception) {
                    if (highSpeed) { s.close(); fallBack(e.message ?: "request refused") }
                    else listener.onError("Capture failed: ${e.message}")
                    return
                }
                // Only now is it known whether this mode can show a preview.
                if (previewOut == null) {
                    listener.onPreviewSupport(
                        when {
                            previewRefused -> PreviewSupport.Refused
                            Build.VERSION.SDK_INT < Build.VERSION_CODES.P -> PreviewSupport.NeedsAndroid9
                            else -> PreviewSupport.NotAtThisFrameRate
                        }
                    )
                } else {
                    previewWanted?.let { attachPreview(it) }
                }
            }

            override fun onConfigureFailed(s: CameraCaptureSession) {
                when {
                    // A camera that won't take the preview output still has to stream: try again without it.
                    previewOut != null -> {
                        previewRefused = true
                        listener.onLog("session refused with the preview output; retrying without it")
                        listener.onPreviewSupport(PreviewSupport.Refused)
                        createSession(device, fpsRange, highSpeed)
                    }
                    highSpeed -> fallBack("session refused")
                    else -> listener.onError("Camera session configuration failed")
                }
            }
        }
        try {
            when {
                highSpeed -> device.createConstrainedHighSpeedCaptureSession(listOf(surface), callback, handler)
                previewOut != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.P -> device.createCaptureSession(
                    SessionConfiguration(
                        SessionConfiguration.SESSION_REGULAR,
                        listOf(OutputConfiguration(surface), previewOut),
                        { r: Runnable -> handler.post(r) },
                        callback,
                    )
                )
                else -> device.createCaptureSession(listOf(surface), callback, handler)
            }
        } catch (e: Exception) {
            when {
                previewOut != null -> {
                    previewRefused = true
                    listener.onLog("session with a preview output failed (${e.message}); retrying without it")
                    listener.onPreviewSupport(PreviewSupport.Refused)
                    createSession(device, fpsRange, highSpeed)
                }
                highSpeed -> fallBack(e.message ?: "session refused")
                else -> throw e
            }
        }
    }

    /**
     * The size of the preview output: the largest size up to 1280 wide with the stream's aspect ratio (so
     * the picture on the phone is never stretched), falling back to the closest aspect ratio available.
     */
    private fun previewSizeFor(chars: CameraCharacteristics, stream: Size): Size? {
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return null
        val sizes = map.getOutputSizes(SurfaceTexture::class.java)?.toList().orEmpty()
        if (sizes.isEmpty()) return null
        val aspect = stream.width.toFloat() / stream.height
        val capped = sizes.filter { it.width <= 1280 && it.height <= 1280 }.ifEmpty { sizes }
        val same = capped.filter { kotlin.math.abs(it.width.toFloat() / it.height - aspect) < 0.02f }
        return (same.ifEmpty { capped }).maxByOrNull { it.width.toLong() * it.height }
    }

    companion object {
        private const val TAG = "CameraStreamer"

        /** Frame rates offered, highest first (start() falls back down this list). */
        val FPS_STEPS = listOf(120, 60, 30)

        fun fpsBit(fps: Int): Int = when {
            fps >= 120 -> Protocol.FPS_120
            fps >= 60 -> Protocol.FPS_60
            else -> Protocol.FPS_30
        }

        private fun describeFps(mask: Int) =
            FPS_STEPS.reversed().filter { mask and fpsBit(it) != 0 }.joinToString(",").ifEmpty { "-" }

        fun targetSize(quality: Int): Size = when (quality) {
            Protocol.QUALITY_720P -> Size(1280, 720)
            Protocol.QUALITY_4K -> Size(3840, 2160)
            else -> Size(1920, 1080)
        }

        /**
         * About 0.16 bits per pixel per frame: 4 Mbps at 720p30, 10 at 1080p30, 20 at 1080p60, 40 at 4K30.
         * Frames above 60 fps count half (they differ very little), so 1080p120 gets 30 Mbps.
         */
        fun bitrateFor(width: Int, height: Int, fps: Int): Int {
            val effectiveFps = if (fps > 60) 60 + (fps - 60) / 2 else fps
            return (width.toLong() * height * effectiveFps * 16 / 100).coerceIn(4_000_000L, 40_000_000L).toInt()
        }

        /**
         * The requested quality if the camera and encoder can do it exactly; otherwise the largest 16:9 size
         * up to it, then the largest size up to it, then the smallest available.
         */
        fun chooseSize(sizes: Array<Size>, quality: Int, encoderOk: (Size) -> Boolean = { true }): Size {
            val target = targetSize(quality)
            val usable = sizes.filter { encoderOk(it) }.ifEmpty { sizes.toList() }
            val capped = usable.filter { it.width <= target.width && it.height <= target.height }
            capped.firstOrNull { it.width == target.width && it.height == target.height }?.let { return it }
            capped.filter { it.width * 9 == it.height * 16 }.maxByOrNull { it.width * it.height }?.let { return it }
            capped.maxByOrNull { it.width * it.height }?.let { return it }
            return usable.minByOrNull { it.width * it.height } ?: Size(1280, 720)
        }

        /** A fixed [fps] range if available, else the range reaching [fps] with the highest minimum, else <= [fps]. */
        fun chooseFpsRange(ranges: Array<Range<Int>>?, fps: Int): Range<Int> {
            if (ranges.isNullOrEmpty()) return Range(fps, fps)
            ranges.firstOrNull { it.lower == fps && it.upper == fps }?.let { return it }
            ranges.filter { it.upper == fps }.maxByOrNull { it.lower }?.let { return it }
            return ranges.filter { it.upper <= fps }.maxWithOrNull(compareBy({ it.upper }, { it.lower }))
                ?: ranges.first()
        }

        private val encoderCache = HashMap<Triple<Int, Int, Int>, Boolean>()

        /** Whether a hardware/software AVC encoder on this phone accepts this size and frame rate. */
        fun encoderSupports(width: Int, height: Int, fps: Int): Boolean = encoderCache.getOrPut(Triple(width, height, fps)) {
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            }
            try { MediaCodecList(MediaCodecList.REGULAR_CODECS).findEncoderForFormat(format) != null } catch (_: Exception) { false }
        }
    }
}
