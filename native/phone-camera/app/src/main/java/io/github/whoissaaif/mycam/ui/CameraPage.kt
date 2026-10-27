package io.github.whoissaaif.mycam.ui

import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.TextureView
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import io.github.whoissaaif.mycam.PhonePreview
import io.github.whoissaaif.mycam.PreviewSupport
import io.github.whoissaaif.mycam.Protocol
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.WebcamService
import io.github.whoissaaif.mycam.ui.theme.V2
import io.github.whoissaaif.mycam.ui.v2.DarkChip
import io.github.whoissaaif.mycam.ui.v2.FlatCard
import io.github.whoissaaif.mycam.ui.v2.LineIcon
import io.github.whoissaaif.mycam.ui.v2.LineIconView
import io.github.whoissaaif.mycam.ui.v2.Motion
import io.github.whoissaaif.mycam.ui.v2.SegmentedControl
import io.github.whoissaaif.mycam.ui.v2.StatTile
import io.github.whoissaaif.mycam.ui.v2.FlatSwitchRow
import io.github.whoissaaif.mycam.ui.v2.ease
import java.util.Locale
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Camera (docs/redesign-v2.md 3.6): the live preview, Front / Back, and the three stat tiles.
 *
 * The preview is a second output of the running capture session (6.1), so it never changes what the PC
 * receives. It is off while the screen is dimmed, off in the background, and it turns itself off when the
 * phone gets too hot — each of those says so here instead of leaving a black rectangle.
 */
@Composable
fun CameraPage(
    state: WebcamService.UiState,
    previewOn: Boolean,
    onPreview: (Boolean) -> Unit,
    onFacing: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        PreviewArea(state, previewOn)

        SegmentedControl(
            options = listOf(stringResource(R.string.camera_front), stringResource(R.string.camera_back)),
            selectedIndex = if (state.facing == Protocol.FACING_FRONT) 0 else 1,
            onSelect = { onFacing(if (it == 0) Protocol.FACING_FRONT else Protocol.FACING_BACK) },
        )

        val info = state.cameraInfo
        val live = state.streaming && !state.paused
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            StatTile(
                if (live && info != null && info.width > 0) "${info.height}p" else FpsReason.qualityName(state.camera.quality),
                stringResource(R.string.stat_resolution), Modifier.weight(1f),
            )
            StatTile(
                (if (live && info != null && info.actualFps > 0) info.actualFps else state.camera.fps).toString(),
                stringResource(R.string.stat_fps), Modifier.weight(1f),
            )
            StatTile(
                zoomLabel(info?.zoomX100?.takeIf { it > 0 }?.div(100f) ?: state.camera.zoom),
                stringResource(R.string.stat_zoom), Modifier.weight(1f),
            )
        }

        FlatCard {
            FlatSwitchRow(
                stringResource(R.string.preview_toggle), previewOn, onPreview,
                detail = stringResource(R.string.preview_toggle_detail),
                enabled = PhonePreview.possible,
            )
            previewNote(previewOn, state)?.let {
                Text(it, style = MaterialTheme.typography.bodyMedium, color = V2.Subtle)
            }
        }
    }
}

/** The preview, or a plain reason why there isn't one. Never a black rectangle (section 12.9). */
@Composable
private fun PreviewArea(state: WebcamService.UiState, previewOn: Boolean) {
    val running by PhonePreview.running.collectAsState()
    val alpha by animateFloatAsState(if (running) 1f else 0f, ease(Motion.STANDARD), label = "previewFade")
    val aspect = aspectOf(state.resolution)
    Box(
        Modifier
            .fillMaxWidth()
            .aspectRatio(aspect)
            .clip(RoundedCornerShape(10.dp))
            .background(V2.Text),
    ) {
        if (previewOn && PhonePreview.possible) PreviewTexture(Modifier.fillMaxSize().alpha(alpha))
        // The placeholder sits under the picture and fades out as it arrives.
        Column(
            Modifier.fillMaxSize().alpha(1f - alpha).padding(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            LineIconView(LineIcon.Preview, 40.dp, Color.White.copy(alpha = 0.7f))
            Spacer(Modifier.height(10.dp))
            Text(
                previewNote(previewOn, state) ?: stringResource(R.string.preview_waiting),
                style = MaterialTheme.typography.bodyMedium, color = Color.White, textAlign = TextAlign.Center,
            )
        }
        val info = state.cameraInfo
        if (state.streaming && info != null && info.width > 0) {
            Row(
                Modifier.align(Alignment.BottomStart).padding(10.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                DarkChip("${info.width} × ${info.height}")
                DarkChip("${if (info.actualFps > 0) info.actualFps else state.camera.fps} FPS")
                DarkChip(stringResource(if (state.facing == Protocol.FACING_FRONT) R.string.camera_front else R.string.camera_back))
            }
        }
    }
}

/** The TextureView whose surface becomes the camera's second output. */
@Composable
private fun PreviewTexture(modifier: Modifier = Modifier) {
    AndroidView(
        modifier = modifier,
        factory = { ctx ->
            TextureView(ctx).apply {
                isOpaque = false
                surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                    override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                        PhonePreview.setSurface(Surface(texture))
                    }

                    override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {}

                    override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                        PhonePreview.setSurface(null)
                        return true
                    }

                    override fun onSurfaceTextureUpdated(texture: SurfaceTexture) {}
                }
            }
        },
    )
    DisposableEffect(Unit) { onDispose { PhonePreview.setSurface(null) } }
}

/** One line saying why the preview isn't showing, or null when it is (or is about to be). */
@Composable
private fun previewNote(previewOn: Boolean, state: WebcamService.UiState): String? {
    val support by PhonePreview.support.collectAsState()
    val running by PhonePreview.running.collectAsState()
    return when {
        !PhonePreview.possible -> stringResource(R.string.preview_needs_android9)
        !previewOn -> stringResource(R.string.preview_off)
        support == PreviewSupport.TooHot -> stringResource(R.string.preview_too_hot)
        support == PreviewSupport.NotAtThisFrameRate -> stringResource(R.string.preview_not_at_fps)
        support == PreviewSupport.Refused -> stringResource(R.string.preview_refused)
        running -> null
        !state.streaming || state.paused -> stringResource(R.string.preview_waiting)
        else -> null
    }
}

/** "1920×1080" to 1.778. Falls back to 16 : 9 before the camera has reported. */
internal fun aspectOf(resolution: String): Float {
    val parts = resolution.split('×', 'x')
    if (parts.size == 2) {
        val w = parts[0].trim().toFloatOrNull()
        val h = parts[1].trim().toFloatOrNull()
        if (w != null && h != null && h > 0f && w > 0f) return w / h
    }
    return 16f / 9f
}

/** "1.0x" / "2x": the zoom as the stat tile and the slider show it. */
internal fun zoomLabel(zoom: Float): String =
    if (abs(zoom - zoom.roundToInt()) < 0.05f) "${zoom.roundToInt()}x" else String.format(Locale.US, "%.1fx", zoom)
