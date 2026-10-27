package io.github.whoissaaif.mycam.ui

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Log
import android.util.Size
import android.view.Surface
import android.view.TextureView
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.requiredSize
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import com.google.zxing.BarcodeFormat
import com.google.zxing.BinaryBitmap
import com.google.zxing.DecodeHintType
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.ReaderException
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader
import io.github.whoissaaif.mycam.QrPairing
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.ui.v2.SecondaryButton

/**
 * "Scan QR Code" (IMPROVEMENTS.md 12): a full-screen camera that looks for the PC's MyCam QR code, the same
 * code iPhones scan. The first MyCam code found goes to [onFound] (once) and the scanner closes; a code from
 * an older companion (no fingerprint) says to update MyCam on the PC; anything else is ignored.
 *
 * Portrait only while it shows (the preview relies on the camera's own rotation for a portrait screen).
 * The camera is this screen's own, so the scanner can't run while the phone is streaming.
 */
@Composable
// Portrait lock is deliberate and only while the scanner shows: see the KDoc.
@SuppressLint("SourceLockedOrientationActivity")
fun QrScanner(onFound: (String) -> Unit, onClose: () -> Unit) {
    val context = LocalContext.current
    var permitted by remember {
        mutableStateOf(ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED)
    }
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { permitted = it }
    LaunchedEffect(Unit) { if (!permitted) ask.launch(Manifest.permission.CAMERA) }

    // Portrait while scanning; whatever it was before afterwards.
    val activity = context.findActivity()
    DisposableEffect(activity) {
        val before = activity?.requestedOrientation ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        onDispose { activity?.requestedOrientation = before }
    }

    var oldPc by remember { mutableStateOf(false) }
    var found by remember { mutableStateOf(false) }
    val haptics = LocalHapticFeedback.current
    val latestFound by rememberUpdatedState(onFound)
    val onText: (String) -> Unit = { text ->
        if (!found) {
            when (QrPairing.parse(text)) {
                is QrPairing.Scan.Ok -> {
                    found = true
                    haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                    latestFound(text)
                }
                QrPairing.Scan.OldPc -> oldPc = true
                QrPairing.Scan.NotMyCam -> {}
            }
        }
    }

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        val camera = remember(permitted) { if (permitted) QrCamera.create(context) else null }
        if (camera != null) {
            CameraView(camera, onText)
        }
        Column(
            Modifier.fillMaxSize().windowInsetsPadding(WindowInsets.safeDrawing).padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                stringResource(R.string.qr_title), style = MaterialTheme.typography.titleLarge, color = Color.White,
                textAlign = TextAlign.Center, modifier = Modifier.semantics { heading() },
            )
            Spacer(Modifier.height(8.dp))
            Text(
                stringResource(
                    when {
                        !permitted -> R.string.qr_need_camera
                        camera == null -> R.string.qr_no_camera
                        oldPc -> R.string.qr_old_pc
                        else -> R.string.qr_hint
                    }
                ),
                style = MaterialTheme.typography.bodyMedium, color = Color.White, textAlign = TextAlign.Center,
                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
            )
            Spacer(Modifier.weight(1f))
            // A plain frame to aim with: no moving scan line (reduced motion has nothing to turn off).
            Box(Modifier.size(240.dp).border(3.dp, Color.White.copy(alpha = 0.85f), RoundedCornerShape(18.dp)))
            Spacer(Modifier.weight(1f))
            SecondaryButton(stringResource(R.string.qr_close), onClick = onClose, modifier = Modifier.fillMaxWidth())
        }
    }
}

/** The preview, scaled to cover the screen (centre-cropped), with the decoder fed from the same session. */
@Composable
private fun CameraView(camera: QrCamera, onText: (String) -> Unit) {
    val latest by rememberUpdatedState(onText)
    DisposableEffect(camera) {
        camera.onText = { latest(it) }
        onDispose { camera.close() }
    }
    BoxWithConstraints(Modifier.fillMaxSize().clipToBounds(), contentAlignment = Alignment.Center) {
        // The buffer is landscape (as the sensor gives it); on a portrait screen it shows turned upright.
        val turned = camera.sensorOrientation % 180 != 0
        val w = (if (turned) camera.size.height else camera.size.width).toFloat()
        val h = (if (turned) camera.size.width else camera.size.height).toFloat()
        val density = LocalDensity.current
        val boxW = with(density) { maxWidth.toPx() }
        val boxH = with(density) { maxHeight.toPx() }
        val scale = maxOf(boxW / w, boxH / h)
        val size = with(density) { androidx.compose.ui.unit.DpSize((w * scale).toDp(), (h * scale).toDp()) }
        AndroidView(
            modifier = Modifier.requiredSize(size),
            factory = { ctx ->
                TextureView(ctx).apply {
                    surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                        override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                            texture.setDefaultBufferSize(camera.size.width, camera.size.height)
                            camera.start(texture)
                        }
                        override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {}
                        override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                            camera.close()
                            return true
                        }
                        override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {}
                    }
                }
            },
        )
    }
}

private fun Context.findActivity(): Activity? {
    var c: Context? = this
    while (c is ContextWrapper) {
        if (c is Activity) return c
        c = c.baseContext
    }
    return null
}

/**
 * The scanner's camera: the first back camera (else any), a preview output and a YUV output at about 720p.
 * ZXing reads the luminance plane of the latest frame on the camera thread; a frame that arrives while one
 * is being decoded is skipped. Found text goes to [onText] on the main thread.
 */
internal class QrCamera private constructor(
    private val manager: CameraManager,
    private val cameraId: String,
    val size: Size,
    val sensorOrientation: Int,
) {
    @Volatile var onText: (String) -> Unit = {}
    private val thread = HandlerThread("qr-camera").apply { start() }
    private val handler = Handler(thread.looper)
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var closed = false
    private var started = false
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null
    private var surface: Surface? = null
    private val decoder = QRCodeReader()
    private val hints = mapOf(DecodeHintType.POSSIBLE_FORMATS to listOf(BarcodeFormat.QR_CODE))

    @SuppressLint("MissingPermission") // The screen only creates this once the permission is granted.
    fun start(texture: SurfaceTexture) {
        handler.post {
            if (closed || started) return@post
            started = true
            val preview = Surface(texture).also { surface = it }
            val yuv = ImageReader.newInstance(size.width, size.height, ImageFormat.YUV_420_888, 2).also { reader = it }
            yuv.setOnImageAvailableListener({ decode(it) }, handler)
            try {
                manager.openCamera(cameraId, object : CameraDevice.StateCallback() {
                    override fun onOpened(d: CameraDevice) {
                        if (closed) { d.close(); return }
                        device = d
                        @Suppress("DEPRECATION") // The List<Surface> overload works on every supported API level.
                        d.createCaptureSession(listOf(preview, yuv.surface), object : CameraCaptureSession.StateCallback() {
                            override fun onConfigured(s: CameraCaptureSession) {
                                if (closed) { s.close(); return }
                                session = s
                                try {
                                    val req = d.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                                        addTarget(preview)
                                        addTarget(yuv.surface)
                                        set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                                    }
                                    s.setRepeatingRequest(req.build(), null, handler)
                                } catch (e: Exception) {
                                    Log.w(TAG, "scanner: request refused", e)
                                }
                            }
                            override fun onConfigureFailed(s: CameraCaptureSession) {
                                Log.w(TAG, "scanner: session refused")
                            }
                        }, handler)
                    }
                    override fun onDisconnected(d: CameraDevice) = d.close()
                    override fun onError(d: CameraDevice, error: Int) {
                        Log.w(TAG, "scanner: camera error $error")
                        d.close()
                    }
                }, handler)
            } catch (e: Exception) {
                Log.w(TAG, "scanner: can't open the camera", e)
            }
        }
    }

    private fun decode(r: ImageReader) {
        val image = try { r.acquireLatestImage() } catch (_: Exception) { null } ?: return
        try {
            if (closed) return
            val plane = image.planes[0] // Luminance, one byte per pixel (pixel stride 1 for YUV_420_888's Y).
            val data = ByteArray(plane.buffer.remaining()).also { plane.buffer.get(it) }
            val source = PlanarYUVLuminanceSource(data, plane.rowStride, image.height, 0, 0, image.width, image.height, false)
            val text = try {
                decoder.decode(BinaryBitmap(HybridBinarizer(source)), hints).text
            } catch (_: ReaderException) {
                null
            } finally {
                decoder.reset()
            }
            if (text != null && !closed) main.post { if (!closed) onText(text) }
        } catch (e: Exception) {
            Log.w(TAG, "scanner: frame skipped", e)
        } finally {
            image.close()
        }
    }

    fun close() {
        if (closed) return
        closed = true
        handler.post {
            try { session?.close() } catch (_: Exception) {}
            try { device?.close() } catch (_: Exception) {}
            try { reader?.close() } catch (_: Exception) {}
            surface?.release()
            session = null; device = null; reader = null; surface = null
        }
        thread.quitSafely()
    }

    companion object {
        private const val TAG = "QrScanner"

        /** The scanner's camera, or null if this phone has none it can use. */
        fun create(context: Context): QrCamera? = try {
            val manager = context.getSystemService(CameraManager::class.java)
            val ids = manager.cameraIdList.toList()
            val id = ids.firstOrNull {
                manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
            } ?: ids.firstOrNull()
            id?.let {
                val chars = manager.getCameraCharacteristics(it)
                val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                val yuv = map?.getOutputSizes(ImageFormat.YUV_420_888)?.toList().orEmpty()
                val previews = map?.getOutputSizes(SurfaceTexture::class.java)?.toSet().orEmpty()
                val size = chooseSize(yuv.filter { s -> s in previews }.ifEmpty { yuv })
                size?.let { s -> QrCamera(manager, it, s, chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90) }
            }
        } catch (e: Exception) {
            Log.w(TAG, "scanner: no camera", e)
            null
        }

        /** About 720p is plenty for a code on a screen and quick to decode: 1280×720 if offered, else the
         * largest 16:9 up to 1920 wide, else the largest up to 1920×1080, else the smallest there is. */
        internal fun chooseSize(sizes: List<Size>): Size? {
            sizes.firstOrNull { it.width == 1280 && it.height == 720 }?.let { return it }
            val capped = sizes.filter { it.width <= 1920 && it.height <= 1080 }
            capped.filter { it.width * 9 == it.height * 16 }.maxByOrNull { it.width * it.height }?.let { return it }
            return capped.maxByOrNull { it.width * it.height } ?: sizes.minByOrNull { it.width * it.height }
        }
    }
}
