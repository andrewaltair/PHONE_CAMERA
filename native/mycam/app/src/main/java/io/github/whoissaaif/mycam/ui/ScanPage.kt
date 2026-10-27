package io.github.whoissaaif.mycam.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.NearbyPcs
import io.github.whoissaaif.mycam.NearbyStatus
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.WebcamService
import io.github.whoissaaif.mycam.ui.theme.LocalReducedMotion
import io.github.whoissaaif.mycam.ui.theme.V2
import io.github.whoissaaif.mycam.ui.v2.FlatCard
import io.github.whoissaaif.mycam.ui.v2.LineIcon
import io.github.whoissaaif.mycam.ui.v2.LineIconView
import io.github.whoissaaif.mycam.ui.v2.Radar
import io.github.whoissaaif.mycam.ui.v2.SecondaryButton
import io.github.whoissaaif.mycam.ui.xp.XpProgressBar

/**
 * Find Devices (docs/redesign-v2.md 3.4): a radar with a constant-speed sweep and a dot per PC found, the
 * heading, the hint, then the devices with their name and address.
 *
 * Reduced motion keeps the radar but stops the sweep, and shows the green XP marquee instead (section
 * 10.5): the sweep is the only thing that said "still looking".
 */
@Composable
fun ScanPage(
    state: WebcamService.UiState,
    onScan: () -> Unit,
    onConnectPc: (String) -> Unit,
    onBack: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val reduced = LocalReducedMotion.current
    val pcs = state.nearbyPcs
    Column(modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            SecondaryButton(stringResource(R.string.back), onClick = onBack)
        }
        Spacer(Modifier.height(16.dp))
        Radar(
            dots = pcs.size,
            modifier = Modifier.size(200.dp),
            animate = state.scanning && !reduced,
        )
        Spacer(Modifier.height(20.dp))
        // The heading follows what the scan actually found: a finished scan with nothing on this Wi-Fi must
        // not say "Devices found". Idle covers the scan never starting (wireless off, or camera permission
        // still to be granted), where "Scanning…" would be a lie.
        val title = when {
            state.scanning -> R.string.scan_title
            pcs.isNotEmpty() -> R.string.scan_title_done
            state.scanned -> R.string.scan_title_none
            else -> R.string.scan_title_idle
        }
        Text(
            stringResource(title),
            style = MaterialTheme.typography.titleLarge, color = V2.Text, textAlign = TextAlign.Center,
            modifier = Modifier.semantics { heading() },
        )
        Spacer(Modifier.height(6.dp))
        Text(
            stringResource(R.string.scan_hint), style = MaterialTheme.typography.bodyMedium,
            color = V2.Subtle, textAlign = TextAlign.Center,
        )
        if (state.scanning && reduced) {
            Spacer(Modifier.height(12.dp))
            XpProgressBar(null, Modifier.padding(horizontal = 32.dp))
        }
        val status: String? = when {
            !state.wirelessOn -> stringResource(R.string.wireless_hint_off)
            state.scanning -> stringResource(R.string.scan_looking)
            pcs.isNotEmpty() -> pluralStringResource(R.plurals.scan_found, pcs.size, pcs.size)
            state.scanned -> stringResource(R.string.scan_none)
            else -> null
        }
        if (status != null) {
            Spacer(Modifier.height(12.dp))
            Text(
                status, style = MaterialTheme.typography.bodyMedium, color = V2.Subtle, textAlign = TextAlign.Center,
                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
            )
        }
        if (state.wirelessAddress != null) {
            Spacer(Modifier.height(4.dp))
            Text(
                stringResource(R.string.scan_phone_address, state.wirelessAddress),
                style = MaterialTheme.typography.bodyMedium, color = V2.Subtle, textAlign = TextAlign.Center,
            )
        }
        if (state.wirelessOn && pcs.isNotEmpty()) {
            Spacer(Modifier.height(16.dp))
            // Tapping a PC asks it to connect here (PROTOCOL.md "The phone asks for a session"), so the
            // pairing code can be brought up from the phone and not only from the PC's own list.
            Text(
                stringResource(R.string.scan_tap_hint), style = MaterialTheme.typography.bodyMedium,
                color = V2.Subtle, textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(8.dp))
            FlatCard(padding = 8.dp) {
                NearbyPcs.ordered(pcs, state.pairedPcs, state.wirelessPcIp, state.wirelessPc)
                    .forEachIndexed { i, (pc, st) ->
                        if (i > 0) CardDivider()
                        DeviceRow(
                            pc.name, pc.ip, st,
                            asking = state.invitingPc == pc.ip,
                            // A connected PC has nowhere to go, and one whose probe port wasn't recorded
                            // can't be asked at all.
                            onClick = if (st == NearbyStatus.Connected || pc.port == 0) null
                                      else ({ onConnectPc(pc.ip) }),
                        )
                    }
            }
        }
        Spacer(Modifier.height(16.dp))
        SecondaryButton(
            stringResource(if (state.wirelessOn) R.string.scan_again else R.string.scan_button_turn_on),
            onClick = onScan, enabled = !state.scanning,
            contentDescription = if (state.wirelessOn) null else stringResource(R.string.scan_button_turn_on_description),
        )
    }
}

/**
 * One device found: a monitor glyph, the name in bold, its address, and what pairing state it is in.
 * [onClick] (null: not actionable) asks that PC to connect; [asking] is while it is being asked.
 */
@Composable
private fun DeviceRow(name: String, ip: String, status: NearbyStatus, asking: Boolean, onClick: (() -> Unit)?) {
    val statusText = stringResource(
        when {
            asking -> R.string.scan_status_connecting
            status == NearbyStatus.Connected -> R.string.scan_status_connected
            status == NearbyStatus.Paired -> R.string.scan_status_paired
            else -> R.string.scan_status_new
        }
    )
    val description = stringResource(R.string.scan_row_description, name, ip, statusText)
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .then(if (onClick != null && !asking) Modifier.clickable(onClick = onClick) else Modifier)
            .padding(vertical = 8.dp)
            .clearAndSetSemantics {
                contentDescription = description
                if (onClick != null && !asking) role = Role.Button
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(36.dp), contentAlignment = Alignment.Center) {
            LineIconView(LineIcon.Monitor, 24.dp, V2.Blue)
        }
        Spacer(Modifier.width(8.dp))
        Column(Modifier.weight(1f)) {
            Text(name, style = MaterialTheme.typography.bodyLarge, fontWeight = FontWeight.Bold, color = V2.Text)
            Text(ip, style = MaterialTheme.typography.bodyMedium, color = V2.Subtle)
        }
        Text(statusText, style = MaterialTheme.typography.bodyMedium, color = V2.Subtle, modifier = Modifier.weight(1f))
    }
}
