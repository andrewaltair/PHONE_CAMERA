package io.github.whoissaaif.mycam.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.NearbyPcs
import io.github.whoissaaif.mycam.NearbyStatus
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.WebcamService
import io.github.whoissaaif.mycam.ui.theme.LocalReducedMotion
import io.github.whoissaaif.mycam.ui.theme.Xp
import io.github.whoissaaif.mycam.ui.xp.XpButton
import io.github.whoissaaif.mycam.ui.xp.XpProgressBar

/**
 * "Scan for PCs" (docs/redesign.md 5.4): a push button, the green marquee while the phone listens for PC probes
 * (6 s), then the PCs it heard with their status. When wireless mode is off the button says "Turn on and
 * scan", because pressing it turns wireless mode on. The status line is a polite live region, so TalkBack
 * announces "Looking…" and then the result. [showAddress] adds this phone's IP to "No PC found" where no
 * other hint shows it (the Now tab).
 */
@Composable
fun ScanForPcs(state: WebcamService.UiState, onScan: () -> Unit, showAddress: Boolean, modifier: Modifier = Modifier) {
    Column(modifier.fillMaxWidth()) {
        XpButton(
            stringResource(if (state.wirelessOn) R.string.scan_button else R.string.scan_button_turn_on),
            onClick = onScan,
            enabled = !state.scanning,
            modifier = Modifier.widthIn(min = 140.dp),
            contentDescription = if (state.wirelessOn) null else stringResource(R.string.scan_button_turn_on_description),
        )
        // With reduced motion the bar would only say "Working…"; the static "Looking for PCs…" line says it better.
        if (state.scanning && !LocalReducedMotion.current) XpProgressBar(null, Modifier.padding(top = 8.dp))

        val pcs = state.nearbyPcs
        val status: String? = when {
            !state.wirelessOn -> null
            state.scanning -> stringResource(R.string.scan_looking)
            pcs.isNotEmpty() -> pluralStringResource(R.plurals.scan_found, pcs.size, pcs.size)
            state.scanned -> buildString {
                append(stringResource(R.string.scan_none))
                if (showAddress) {
                    append(' ')
                    append(
                        state.wirelessAddress?.let { stringResource(R.string.scan_phone_address, it) }
                            ?: stringResource(R.string.wireless_hint_no_wifi)
                    )
                }
            }
            else -> null
        }
        if (status != null) {
            Text(
                status, style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle,
                modifier = Modifier.padding(top = 8.dp).semantics { liveRegion = LiveRegionMode.Polite },
            )
        }
        if (state.wirelessOn) {
            NearbyPcs.ordered(pcs, state.pairedPcs, state.wirelessPcIp, state.wirelessPc).forEach { (pc, st) ->
                NearbyRow(pc.name, pc.ip, st)
            }
        }
    }
}

@Composable
private fun NearbyRow(name: String, ip: String, status: NearbyStatus) {
    val statusText = stringResource(
        when (status) {
            NearbyStatus.Connected -> R.string.scan_status_connected
            NearbyStatus.Paired -> R.string.scan_status_paired
            NearbyStatus.NotPaired -> R.string.scan_status_new
        }
    )
    val description = stringResource(R.string.scan_row_description, name, ip, statusText)
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .padding(vertical = 4.dp)
            .clearAndSetSemantics { contentDescription = description },
        verticalAlignment = Alignment.Top,
    ) {
        StatusDot(status, Modifier.padding(top = 5.dp))
        Spacer(Modifier.width(8.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    name, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Bold, color = Xp.Text,
                    modifier = Modifier.weight(1f, fill = false),
                )
                Text(ip, style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle)
            }
            Spacer(Modifier.height(2.dp))
            Text(statusText, style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle)
        }
    }
}

/** A small glossy dot (graphics only): green connected, blue paired, amber not paired yet. */
@Composable
private fun StatusDot(status: NearbyStatus, modifier: Modifier = Modifier) {
    val (light, dark) = when (status) {
        NearbyStatus.Connected -> Xp.GoLight to Xp.GoDark
        NearbyStatus.Paired -> Color(0xFF7FB6FF) to Xp.Selection
        NearbyStatus.NotPaired -> Xp.AmberLight to Xp.AmberDark
    }
    Canvas(modifier.size(12.dp)) {
        val r = size.minDimension / 2
        drawCircle(Brush.radialGradient(listOf(light, dark), center = Offset(r * 0.7f, r * 0.6f), radius = r * 1.4f), r)
        drawCircle(Color.White, r - 0.5.dp.toPx(), style = Stroke(1.dp.toPx()))
        drawCircle(dark.copy(alpha = 0.6f), r, style = Stroke(0.5.dp.toPx()))
    }
}
