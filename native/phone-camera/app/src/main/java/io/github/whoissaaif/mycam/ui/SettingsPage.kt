package io.github.whoissaaif.mycam.ui

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.PairedPc
import io.github.whoissaaif.mycam.PhonePreview
import io.github.whoissaaif.mycam.Protocol
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.WebcamService
import io.github.whoissaaif.mycam.ui.theme.V2
import io.github.whoissaaif.mycam.ui.v2.Choice
import io.github.whoissaaif.mycam.ui.v2.FlatCard
import io.github.whoissaaif.mycam.ui.v2.FlatDropdown
import io.github.whoissaaif.mycam.ui.v2.FlatSliderRow
import io.github.whoissaaif.mycam.ui.v2.FlatSwitchRow
import io.github.whoissaaif.mycam.ui.v2.SecondaryButton
import io.github.whoissaaif.mycam.ui.xp.XpLink
import io.github.whoissaaif.mycam.ui.xp.XpProgressBar
import java.util.Locale
import kotlin.math.roundToInt

private val RATES = listOf(30, 60, 120)

/**
 * Settings (docs/redesign-v2.md 3.7): the same controls as the PC's camera page, in the same order —
 * Resolution and Frame Rate as dropdowns, Zoom and Brightness as sliders, Auto Focus and Torch as
 * switches — plus the wireless, paired-PCs, phone and about groups.
 *
 * Capability rule (section 10.3): a dropdown entry this phone cannot do stays visible but disabled, and
 * the reason [FpsReason] computes is printed under the control, because a closed dropdown hides it.
 */
@Composable
fun SettingsPage(
    state: WebcamService.UiState,
    applying: Boolean,
    actions: ScreenActions,
    onCommand: (Int, Int) -> Unit,
    autoDim: Boolean,
    previewOn: Boolean,
    onPreview: (Boolean) -> Unit,
    onFindDevices: () -> Unit,
    onLicences: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        CardSection(stringResource(R.string.section_video)) {
            if (applying) {
                XpProgressBar(null)
                Spacer(Modifier.height(12.dp))
            }
            VideoDropdowns(state, onCommand)
        }

        CardSection(stringResource(R.string.section_camera)) {
            CameraSliders(state, onCommand)
        }

        CardSection(stringResource(R.string.section_preview)) {
            FlatSwitchRow(
                stringResource(R.string.preview_toggle), previewOn, onPreview,
                detail = stringResource(R.string.preview_toggle_detail),
                enabled = PhonePreview.possible,
            )
            if (!PhonePreview.possible) {
                Text(stringResource(R.string.preview_needs_android9), style = MaterialTheme.typography.bodyMedium, color = V2.Subtle)
            }
        }

        CardSection(stringResource(R.string.section_wireless)) {
            FlatSwitchRow(stringResource(R.string.wireless_toggle), state.wirelessOn, actions.onWireless)
            Text(
                when {
                    state.connected && state.wireless -> stringResource(R.string.wireless_connected, state.wirelessPc ?: "")
                    !state.wirelessOn -> stringResource(R.string.wireless_hint_off)
                    state.wirelessAddress == null -> stringResource(R.string.wireless_hint_no_wifi)
                    else -> stringResource(R.string.wireless_hint_on, state.wirelessAddress)
                },
                style = MaterialTheme.typography.bodyMedium, color = V2.Subtle,
            )
            Spacer(Modifier.height(12.dp))
            SecondaryButton(
                stringResource(if (state.wirelessOn) R.string.scan_button else R.string.scan_button_turn_on),
                onClick = onFindDevices,
                contentDescription = if (state.wirelessOn) null else stringResource(R.string.scan_button_turn_on_description),
            )
        }

        CardSection(stringResource(R.string.section_paired)) {
            PairedList(state.pairedPcs, actions.onForgetPc, actions.onForgetAllPcs)
        }

        CardSection(stringResource(R.string.section_phone)) {
            FlatSwitchRow(
                stringResource(R.string.auto_dim_toggle), autoDim, actions.onAutoDim,
                detail = stringResource(R.string.auto_dim_hint),
            )
        }

        CardSection(stringResource(R.string.section_about)) {
            val uri = LocalUriHandler.current
            XpLink(stringResource(R.string.about_get_windows), onClick = {
                try { uri.openUri(RELEASES_URL) } catch (_: Exception) {}
            })
            Text(
                stringResource(R.string.about_version, appVersionName(LocalContext.current)),
                style = MaterialTheme.typography.bodyMedium, color = V2.Subtle,
            )
            XpLink(stringResource(R.string.about_project), onClick = {
                try { uri.openUri(RELEASES_URL) } catch (_: Exception) {}
            })
            Text(
                stringResource(R.string.about_source),
                style = MaterialTheme.typography.bodyMedium, color = V2.Subtle,
            )
            XpLink(stringResource(R.string.about_source_link), onClick = {
                try { uri.openUri(SOURCE_URL) } catch (_: Exception) {}
            })
            Text(
                stringResource(R.string.about_video_only),
                style = MaterialTheme.typography.bodyMedium, color = V2.Subtle,
            )
            XpLink(stringResource(R.string.about_licences), onClick = onLicences)
        }
    }
}

/** A white card with a heading. */
@Composable
fun CardSection(title: String, modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    FlatCard(modifier) {
        Text(
            title, style = MaterialTheme.typography.titleLarge, color = V2.Text,
            modifier = Modifier.semantics { heading() },
        )
        Spacer(Modifier.height(12.dp))
        content()
    }
}

/** Resolution and Frame Rate as dropdowns, with the unavailable entries disabled and the reason underneath. */
@Composable
private fun VideoDropdowns(state: WebcamService.UiState, onCommand: (Int, Int) -> Unit) {
    val info = state.cameraInfo
    val settings = state.camera
    val reported = info != null && info.width > 0
    // Only once the camera has reported it (#11): no info yet must not offer 4K on a phone that can't do it.
    val has4K = info != null && info.flags and Protocol.CAM_HAS_4K != 0
    val qualityAvailable = { q: Int -> q != Protocol.QUALITY_4K || has4K }
    val mask = if (reported) info.fpsMask(settings.quality) else Protocol.FPS_30
    val rateAvailable = { i: Int ->
        i == 0 || (i == 1 && mask and Protocol.FPS_60 != 0) || (i == 2 && mask and Protocol.FPS_120 != 0)
    }

    FlatDropdown(
        label = stringResource(R.string.video_quality),
        choices = listOf(
            Choice("720p", qualityAvailable(Protocol.QUALITY_720P)),
            Choice("1080p", qualityAvailable(Protocol.QUALITY_1080P)),
            Choice("4K", qualityAvailable(Protocol.QUALITY_4K)),
        ),
        selectedIndex = settings.quality,
        onSelect = { onCommand(Protocol.CMD_SET_QUALITY, it) },
    )
    Spacer(Modifier.height(16.dp))
    FlatDropdown(
        label = stringResource(R.string.video_fps),
        choices = RATES.mapIndexed { i, r -> Choice("$r fps", rateAvailable(i)) },
        // Show what this quality will actually run at: the chosen rate, or the best one below it that works
        // here (the choice is kept, so going back to a quality that supports it restores it).
        selectedIndex = RATES.indices.last { it == 0 || (RATES[it] <= settings.fps && rateAvailable(it)) },
        onSelect = { onCommand(Protocol.CMD_SET_FPS, RATES[it]) },
        reasons = FpsReason.compute(info, settings.quality, qualityAvailable).map { fpsReasonLine(it) },
    )
    if (reported && state.streaming) {
        Spacer(Modifier.height(8.dp))
        Text(
            stringResource(R.string.video_actual, "${info.width}×${info.height}", info.actualFps),
            style = MaterialTheme.typography.bodyMedium, color = V2.Subtle,
        )
    }
}

/** Zoom and Brightness as sliders, Auto focus and Torch as switches. Only what the camera reported. */
@Composable
private fun CameraSliders(state: WebcamService.UiState, onCommand: (Int, Int) -> Unit) {
    val info = state.cameraInfo
    if (info == null || info.width == 0) {
        Text(stringResource(R.string.controls_unknown), style = MaterialTheme.typography.bodyMedium, color = V2.Subtle)
        return
    }
    val zoomMin = info.zoomMinX100 / 100f
    val zoomMax = info.zoomMaxX100 / 100f
    if (zoomMax > zoomMin) {
        FlatSliderRow(
            label = stringResource(R.string.control_zoom),
            value = info.zoomX100 / 100f,
            valueText = zoomLabel(info.zoomX100 / 100f),
            range = zoomMin..zoomMax,
            onValueChange = { z ->
                onCommand(Protocol.CMD_SET_ZOOM, (z.coerceIn(zoomMin, zoomMax) * 10).roundToInt().coerceIn(1, 255))
            },
        )
        Spacer(Modifier.height(8.dp))
    }
    if (info.evMax > info.evMin) {
        val ev = info.ev * info.evStepX100 / 100f
        FlatSliderRow(
            label = stringResource(R.string.control_brightness),
            value = info.ev.toFloat(),
            valueText = (if (ev > 0) "+" else "") + String.format(Locale.US, "%.1f", ev) + " EV",
            range = info.evMin.toFloat()..info.evMax.toFloat(),
            steps = (info.evMax - info.evMin - 1).coerceAtLeast(0),
            onValueChange = { onCommand(Protocol.CMD_SET_EXPOSURE, it.roundToInt() and 0xFF) },
        )
        Spacer(Modifier.height(8.dp))
    }
    if (info.flags and Protocol.CAM_HAS_AUTOFOCUS != 0) {
        FlatSwitchRow(
            stringResource(R.string.focus_auto_toggle),
            checked = info.flags and Protocol.CAM_FOCUS_LOCKED == 0,
            onCheckedChange = { auto -> onCommand(Protocol.CMD_SET_FOCUS, if (auto) 0 else 1) },
        )
    }
    FlatSwitchRow(
        stringResource(R.string.control_torch),
        checked = info.flags and Protocol.CAM_TORCH_ON != 0,
        onCheckedChange = { onCommand(Protocol.CMD_SET_TORCH, if (it) 1 else 0) },
        enabled = info.flags and Protocol.CAM_TORCH_AVAILABLE != 0,
    )
}

@Composable
private fun fpsReasonLine(r: FpsReason): String = when (r) {
    FpsReason.Unknown -> stringResource(R.string.video_fps_unknown)
    FpsReason.Only30 -> stringResource(R.string.video_fps_only_30)
    is FpsReason.NeedsOrLower ->
        if (r.quality == Protocol.QUALITY_720P) stringResource(R.string.video_fps_needs, r.fps, FpsReason.qualityName(r.quality))
        else stringResource(R.string.video_fps_needs_or_lower, r.fps, FpsReason.qualityName(r.quality))
    is FpsReason.Needs -> stringResource(R.string.video_fps_needs, r.fps, r.qualities.joinToString(" / ") { FpsReason.qualityName(it) })
    is FpsReason.UpTo -> stringResource(R.string.video_fps_up_to, r.maxFps)
}

/** One row per paired PC with its own Forget, plus Forget all when there are several. */
@Composable
private fun PairedList(pcs: List<PairedPc>, onForget: (String) -> Unit, onForgetAll: () -> Unit) {
    if (pcs.isEmpty()) {
        Text(stringResource(R.string.paired_none), style = MaterialTheme.typography.bodyMedium, color = V2.Subtle)
        return
    }
    pcs.forEach { pc ->
        Row(Modifier.fillMaxWidth().heightIn(min = 56.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(pc.name, style = MaterialTheme.typography.bodyLarge, color = V2.Text, modifier = Modifier.weight(1f))
            SecondaryButton(
                stringResource(R.string.paired_forget), onClick = { onForget(pc.id) },
                contentDescription = stringResource(R.string.paired_forget_description, pc.name),
            )
        }
    }
    Text(stringResource(R.string.paired_hint), style = MaterialTheme.typography.bodyMedium, color = V2.Subtle)
    if (pcs.size > 1) {
        Spacer(Modifier.height(8.dp))
        SecondaryButton(stringResource(R.string.paired_forget_all), onClick = onForgetAll)
    }
}

internal fun appVersionName(context: Context): String = try {
    val info = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        context.packageManager.getPackageInfo(context.packageName, PackageManager.PackageInfoFlags.of(0))
    } else {
        @Suppress("DEPRECATION")
        context.packageManager.getPackageInfo(context.packageName, 0)
    }
    info.versionName ?: "?"
} catch (_: Exception) {
    "?"
}
