package io.github.whoissaaif.mycam.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.Crossfade
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.scale
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.QrPairing
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.WebcamService
import io.github.whoissaaif.mycam.ui.theme.LocalReducedMotion
import io.github.whoissaaif.mycam.ui.theme.V2
import io.github.whoissaaif.mycam.ui.v2.FlatCard
import io.github.whoissaaif.mycam.ui.v2.LineIcon
import io.github.whoissaaif.mycam.ui.v2.Motion
import io.github.whoissaaif.mycam.ui.v2.NavRow
import io.github.whoissaaif.mycam.ui.v2.PageHeading
import io.github.whoissaaif.mycam.ui.v2.PrimaryButton
import io.github.whoissaaif.mycam.ui.v2.SecondaryButton
import io.github.whoissaaif.mycam.ui.v2.ease
import io.github.whoissaaif.mycam.ui.xp.LivePill
import io.github.whoissaaif.mycam.ui.xp.XpLink
import io.github.whoissaaif.mycam.ui.xp.XpProgressBar
import io.github.whoissaaif.mycam.ui.xp.popSpring

/**
 * Connect (docs/redesign-v2.md 3.3): the status surface, then the three ways to connect.
 *
 * The mockup shows only the three rows. The status headline, its detail and Pause stay here (section 10.1
 * and 10.2): the status is the one thing people read at a glance, and Pause is this product's privacy
 * promise, so neither may be buried.
 */
@Composable
fun ConnectPage(
    state: WebcamService.UiState,
    actions: ScreenActions,
    applying: Boolean,
    onFindDevices: () -> Unit,
    sweepLive: Boolean,
    onLiveSwept: () -> Unit,
    modifier: Modifier = Modifier,
    onScanQr: () -> Unit = {},
) {
    var usbHelp by rememberSaveable { mutableStateOf(false) }
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        StatusCard(state, actions, applying, sweepLive, onLiveSwept)

        state.qr?.let { QrStatusCard(it, onScanAgain = onScanQr, onDismiss = actions.onDismissQr) }

        PageHeading(stringResource(R.string.connect_title), stringResource(R.string.connect_subtitle))

        FlatCard(padding = 8.dp) {
            // The quickest way (IMPROVEMENTS.md 12): scan the PC's code, paired and connected. The scanner needs
            // the camera, so not while it streams.
            NavRow(
                LineIcon.Qr, stringResource(R.string.connect_qr), stringResource(R.string.connect_qr_detail),
                onClick = onScanQr, enabled = !state.streaming,
                trailingNote = if (state.streaming) stringResource(R.string.connect_qr_busy) else null,
            )
            CardDivider()
            NavRow(
                LineIcon.Search, stringResource(R.string.connect_find), stringResource(R.string.connect_find_detail),
                onClick = onFindDevices,
            )
            CardDivider()
            NavRow(
                LineIcon.Usb, stringResource(R.string.connect_usb), stringResource(R.string.connect_usb_detail),
                onClick = { usbHelp = !usbHelp },
            )
            AnimatedVisibility(usbHelp) {
                Column(Modifier.padding(start = 52.dp, end = 8.dp, bottom = 8.dp)) {
                    Text(
                        stringResource(R.string.connect_usb_help),
                        style = MaterialTheme.typography.bodyMedium, color = V2.Subtle,
                    )
                    if (!state.connected) {
                        val uri = LocalUriHandler.current
                        XpLink(stringResource(R.string.about_get_windows), onClick = {
                            try { uri.openUri(RELEASES_URL) } catch (_: Exception) {}
                        })
                    }
                }
            }
        }
    }
}

/**
 * After a QR scan: "Connecting to <PC>…" with the XP progress bar, or why it didn't work with Scan again.
 * A polite live region, so TalkBack reads the outcome.
 */
@Composable
private fun QrStatusCard(qr: WebcamService.QrStatus, onScanAgain: () -> Unit, onDismiss: () -> Unit) {
    FlatCard {
        val failure = qr.failure
        Text(
            when {
                failure != null && !qr.trying -> stringResource(R.string.qr_failed)
                qr.pcName != null -> stringResource(R.string.qr_connecting, qr.pcName)
                else -> stringResource(R.string.qr_pairing)
            },
            style = MaterialTheme.typography.titleMedium, color = V2.Text,
            modifier = Modifier.semantics { heading(); liveRegion = LiveRegionMode.Polite },
        )
        Spacer(Modifier.height(8.dp))
        if (failure == null) {
            XpProgressBar(null)
            return@FlatCard
        }
        Text(
            when (failure) {
                QrPairing.Failure.NoPc -> stringResource(R.string.qr_fail_no_pc)
                QrPairing.Failure.CodeUsed -> stringResource(R.string.qr_fail_code_used)
                QrPairing.Failure.Busy -> stringResource(R.string.qr_fail_busy)
                QrPairing.Failure.NoSession -> stringResource(R.string.qr_fail_no_session, qr.pcName ?: "the PC")
            },
            style = MaterialTheme.typography.bodyMedium, color = V2.Subtle,
        )
        // Still trying: the PC said no, but nothing proves that datagram came from it, so the reason is
        // shown while the phone keeps asking. Only when the attempt runs out does it become the answer.
        if (qr.trying) {
            Spacer(Modifier.height(12.dp))
            XpProgressBar(null)
            return@FlatCard
        }
        Spacer(Modifier.height(12.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            PrimaryButton(stringResource(R.string.qr_scan_again), onClick = { onDismiss(); onScanAgain() }, modifier = Modifier.weight(1f))
            SecondaryButton(stringResource(R.string.qr_close), onClick = onDismiss, modifier = Modifier.weight(1f))
        }
    }
}

/** A 1 px hairline between the rows of a card. */
@Composable
fun CardDivider() {
    Box(Modifier.fillMaxWidth().padding(start = 52.dp).height(1.dp).background(V2.CardBorder))
}

/**
 * The status surface: the picture with its state badge, the headline, the detail and Pause / Resume.
 * The headline is a polite live region, so TalkBack announces a change of state.
 */
@Composable
private fun StatusCard(
    state: WebcamService.UiState,
    actions: ScreenActions,
    applying: Boolean,
    sweepLive: Boolean,
    onLiveSwept: () -> Unit,
) {
    val view = describe(state)
    val uri = LocalUriHandler.current
    FlatCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            StatusPicture(view.icon)
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(
                    view.headline, style = MaterialTheme.typography.titleLarge, color = V2.Text,
                    modifier = Modifier.semantics { heading(); liveRegion = LiveRegionMode.Polite },
                )
                Spacer(Modifier.height(2.dp))
                Text(view.detail, style = MaterialTheme.typography.bodyMedium, color = V2.Subtle)
            }
            if (view.live) {
                Spacer(Modifier.width(8.dp))
                LivePill(sweep = sweepLive, onSwept = onLiveSwept)
            }
        }
        if (applying) {
            Spacer(Modifier.height(12.dp))
            XpProgressBar(null)
        }
        heatWarning(state.thermal)?.let { (text, _) ->
            Spacer(Modifier.height(10.dp))
            Text(text, style = MaterialTheme.typography.bodyMedium, color = io.github.whoissaaif.mycam.ui.theme.Xp.WarningText)
        }
        if (!state.connected && !state.wirelessOn) {
            XpLink(stringResource(R.string.get_it), onClick = {
                try { uri.openUri(RELEASES_URL) } catch (_: Exception) {}
            })
        }
        Spacer(Modifier.height(12.dp))
        // Pause is never buried (section 10.1). Resume is the primary action, pausing the quieter one.
        // The explanation comes first, so the button is the last thing in the card and reads as the action.
        Text(
            stringResource(if (state.paused) R.string.action_resume_detail else R.string.action_pause_detail),
            style = MaterialTheme.typography.bodyMedium, color = V2.Subtle,
        )
        Spacer(Modifier.height(8.dp))
        if (state.paused) {
            PrimaryButton(
                stringResource(R.string.action_resume_title), onClick = { actions.onPause(false) },
                modifier = Modifier.fillMaxWidth(),
            )
        } else {
            SecondaryButton(
                stringResource(R.string.action_pause_title), onClick = { actions.onPause(true) },
                modifier = Modifier.fillMaxWidth(),
            )
        }
    }
}

/** The status picture, crossfading between states with a small pop on the badge (section 12). */
@Composable
private fun StatusPicture(icon: Int) {
    val reduced = LocalReducedMotion.current
    Crossfade(icon, animationSpec = ease(Motion.STANDARD), label = "status") { res ->
        val pop = remember { Animatable(if (reduced) 1f else 0.6f) }
        LaunchedEffect(Unit) { if (!reduced) pop.animateTo(1f, popSpring()) }
        Image(painterResource(res), contentDescription = null, modifier = Modifier.size(64.dp).scale(pop.value))
    }
}
