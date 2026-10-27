package io.github.whoissaaif.mycam.ui

import android.os.SystemClock
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.paneTitle
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.WebcamService
import io.github.whoissaaif.mycam.ui.theme.PairingCodeStyle
import io.github.whoissaaif.mycam.ui.theme.V2
import io.github.whoissaaif.mycam.ui.v2.MonitorIllustration
import io.github.whoissaaif.mycam.ui.v2.PrimaryButton
import io.github.whoissaaif.mycam.ui.v2.SecondaryButton
import io.github.whoissaaif.mycam.ui.xp.XpButton
import io.github.whoissaaif.mycam.ui.xp.XpDialogFrame
import io.github.whoissaaif.mycam.ui.xp.XpProgressBar
import io.github.whoissaaif.mycam.ui.xp.motionMs
import kotlinx.coroutines.delay
import java.util.Locale

/**
 * A modal layer: a 40 % dim over the (blurred, see WebcamScreen) screen and a dialog that fades in and rises
 * 16 dp (docs/redesign.md 6.2 / 6.3). Touches never reach the screen behind.
 */
@Composable
fun ModalLayer(visible: Boolean, content: @Composable () -> Unit) {
    val rise = with(LocalDensity.current) { 16.dp.roundToPx() }
    AnimatedVisibility(
        visible = visible,
        enter = fadeIn(tween(motionMs(220))),
        exit = fadeOut(tween(motionMs(150))),
    ) {
        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = 0.4f))
                .clickable(remember { MutableInteractionSource() }, indication = null, onClick = {})
                .windowInsetsPadding(WindowInsets.safeDrawing)
                .padding(16.dp),
            contentAlignment = Alignment.Center,
        ) {
            Box(Modifier.animateEnterExit(enter = slideInVertically(tween(motionMs(220))) { rise })) { content() }
        }
    }
}

/**
 * Pairing a new PC over Wi-Fi (A3, docs/redesign.md 5.5): the code in big digits, a green bar counting down the
 * handshake timeout, Don't allow and Allow (the default button). When the time runs out it says so.
 */
@Composable
fun PairingDialog(
    pcName: String?,
    code: String?,
    deadline: Long,
    timedOut: Boolean,
    onAnswer: (Boolean) -> Unit,
    onDismissTimedOut: () -> Unit,
) {
    // Keep the last values while the dialog fades out.
    var lastName by remember { mutableStateOf("") }
    var lastCode by remember { mutableStateOf("") }
    if (pcName != null) lastName = pcName
    if (code != null) lastCode = code
    val visible = pcName != null

    BackHandler(enabled = visible) { if (timedOut) onDismissTimedOut() else onAnswer(false) }

    ModalLayer(visible) {
        val title = stringResource(R.string.pair_dialog_title)
        XpDialogFrame(
            title = title,
            onClose = { if (timedOut) onDismissTimedOut() else onAnswer(false) },
            modifier = Modifier.semantics { paneTitle = title },
        ) {
            if (timedOut) {
                Text(
                    stringResource(R.string.pair_timed_out), style = MaterialTheme.typography.titleLarge, color = V2.Text,
                    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                )
                Spacer(Modifier.height(8.dp))
                Text(stringResource(R.string.pair_timed_out_detail, lastName), style = MaterialTheme.typography.bodyMedium, color = V2.Subtle)
                Spacer(Modifier.height(16.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    XpButton(stringResource(R.string.close), onClick = onDismissTimedOut, isDefault = true)
                }
            } else {
                PairingBody(lastName, lastCode, deadline, onAnswer)
            }
        }
    }
}

@Composable
private fun PairingBody(pcName: String, code: String, deadline: Long, onAnswer: (Boolean) -> Unit) {
    var now by remember { mutableLongStateOf(SystemClock.elapsedRealtime()) }
    LaunchedEffect(deadline) {
        while (true) {
            now = SystemClock.elapsedRealtime()
            delay(250)
        }
    }
    val total = WebcamService.PAIRING_TIMEOUT_MS
    val remaining = if (deadline == 0L) total else (deadline - now).coerceIn(0L, total)
    val seconds = ((remaining + 999) / 1000).toInt()

    // The mockup's monitor illustration (docs/redesign-v2.md 3.5); the wording stays "Don't allow / Allow",
    // which is Android's own language for this question (section 10.4).
    MonitorIllustration(Modifier.fillMaxWidth().height(96.dp))
    Spacer(Modifier.height(12.dp))
    Text(
        stringResource(R.string.pair_question, pcName), style = MaterialTheme.typography.titleLarge,
        color = V2.Text, textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth(),
    )
    Spacer(Modifier.height(12.dp))
    Text(stringResource(R.string.pair_check), style = MaterialTheme.typography.bodyMedium, color = V2.Text)
    val shown = WebcamService.formatCode(code)
    Text(
        shown, style = PairingCodeStyle, color = V2.Text, textAlign = TextAlign.Center,
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 8.dp)
            // TalkBack reads the code digit by digit.
            .clearAndSetSemantics { contentDescription = code.toCharArray().joinToString(" ") },
    )
    Text(stringResource(R.string.pair_differ), style = MaterialTheme.typography.bodyMedium, color = V2.Subtle)
    Spacer(Modifier.height(12.dp))
    Row(verticalAlignment = Alignment.CenterVertically) {
        XpProgressBar(remaining.toFloat() / total, Modifier.weight(1f))
        Spacer(Modifier.width(12.dp))
        Text(
            stringResource(R.string.pair_waiting, String.format(Locale.US, "%d:%02d", seconds / 60, seconds % 60)),
            style = MaterialTheme.typography.bodyMedium, color = V2.Subtle,
        )
    }
    Spacer(Modifier.height(16.dp))
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End)) {
        SecondaryButton(stringResource(R.string.wireless_deny), onClick = { onAnswer(false) })
        PrimaryButton(stringResource(R.string.wireless_allow), onClick = { onAnswer(true) })
    }
}

/**
 * "Paired with <PC>" after a new pairing (QR code or 6-digit code): the PC is connected, so the screen
 * behind it has already moved to the Camera tab. OK closes it.
 */
@Composable
fun PairedDialog(pcName: String?, onClose: () -> Unit) {
    var lastName by remember { mutableStateOf("") }
    if (pcName != null) lastName = pcName
    val visible = pcName != null
    BackHandler(enabled = visible, onBack = onClose)
    ModalLayer(visible) {
        val title = stringResource(R.string.paired_title)
        XpDialogFrame(title = title, onClose = onClose, modifier = Modifier.semantics { paneTitle = title }) {
            MonitorIllustration(Modifier.fillMaxWidth().height(96.dp))
            Spacer(Modifier.height(12.dp))
            Text(
                stringResource(R.string.paired_with, lastName), style = MaterialTheme.typography.titleLarge,
                color = V2.Text, textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth().semantics { liveRegion = LiveRegionMode.Polite },
            )
            Spacer(Modifier.height(8.dp))
            Text(
                stringResource(R.string.paired_detail), style = MaterialTheme.typography.bodyMedium, color = V2.Subtle,
                textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(16.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                PrimaryButton(stringResource(R.string.paired_ok), onClick = onClose)
            }
        }
    }
}

/**
 * "Let this PC use the camera?" — the one place the user agrees to a PC watching through this phone.
 *
 * It appears once per PC: after a QR pairing, and the first time a PC paired before this existed connects.
 * The session is already up and the camera is off until this is answered, so there is nothing to lose by
 * taking a moment over it. There is deliberately no way to dismiss it without answering: a stray tap or a
 * Back press must not leave a PC half-allowed, so Back and the close button both mean "don't allow".
 */
@Composable
fun CameraGateDialog(pcName: String?, onAllow: () -> Unit, onDeny: () -> Unit) {
    var lastName by remember { mutableStateOf("") }
    if (pcName != null) lastName = pcName
    val visible = pcName != null
    BackHandler(enabled = visible, onBack = onDeny)
    ModalLayer(visible) {
        val title = stringResource(R.string.gate_title)
        XpDialogFrame(title = title, onClose = onDeny, modifier = Modifier.semantics { paneTitle = title }) {
            MonitorIllustration(Modifier.fillMaxWidth().height(96.dp))
            Spacer(Modifier.height(12.dp))
            Text(
                stringResource(R.string.paired_with, lastName), style = MaterialTheme.typography.titleLarge,
                color = V2.Text, textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth().semantics { liveRegion = LiveRegionMode.Polite },
            )
            Spacer(Modifier.height(8.dp))
            Text(
                stringResource(R.string.gate_question), style = MaterialTheme.typography.bodyMedium,
                color = V2.Text, textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(6.dp))
            Text(
                stringResource(R.string.gate_detail), style = MaterialTheme.typography.bodyMedium,
                color = V2.Subtle, textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(16.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                SecondaryButton(stringResource(R.string.wireless_deny), onClick = onDeny, modifier = Modifier.weight(1f))
                PrimaryButton(stringResource(R.string.wireless_allow), onClick = onAllow, modifier = Modifier.weight(1f))
            }
        }
    }
}

/** The licence text of the shipped fonts (About > Licences). */
@Composable
fun LicenceDialog(visible: Boolean, onClose: () -> Unit) {
    val context = LocalContext.current
    val text = remember(visible) {
        if (!visible) "" else try {
            context.assets.open("licenses/DejaVu-LICENSE.txt").bufferedReader().use { it.readText() }
        } catch (_: Exception) {
            ""
        }
    }
    BackHandler(enabled = visible, onBack = onClose)
    ModalLayer(visible) {
        XpDialogFrame(stringResource(R.string.about_licences_title), onClose = onClose) {
            Text(stringResource(R.string.about_font), style = MaterialTheme.typography.titleMedium, color = V2.Text)
            Spacer(Modifier.height(8.dp))
            Column(
                Modifier
                    .fillMaxWidth()
                    .heightIn(max = 360.dp)
                    .background(V2.Page)
                    .verticalScroll(rememberScrollState())
                    .padding(8.dp),
            ) {
                Text(text, style = MaterialTheme.typography.bodyMedium, color = V2.Text)
            }
            Spacer(Modifier.height(12.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                XpButton(stringResource(R.string.close), onClick = onClose, isDefault = true)
            }
        }
    }
}
