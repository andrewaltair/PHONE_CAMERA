package io.github.whoissaaif.mycam.ui.v2

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.ui.theme.LocalReducedMotion
import io.github.whoissaaif.mycam.ui.theme.V2
import io.github.whoissaaif.mycam.ui.xp.rememberTick

// The v2 flat component kit (docs/redesign-v2.md section 4). The XP chrome (title bar, caption, green progress
// chunks) stays in ui/xp; everything inside a page is built from these.

val CardShape = RoundedCornerShape(10.dp)
private val ControlShape = RoundedCornerShape(6.dp)

/** A white card: radius 10, a 1 px #E3E8EF border and a soft shadow. */
@Composable
fun FlatCard(modifier: Modifier = Modifier, padding: Dp = 16.dp, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier
            .fillMaxWidth()
            .shadow(3.dp, CardShape, ambientColor = V2.Shadow, spotColor = V2.Shadow)
            .clip(CardShape)
            .background(V2.Card)
            .drawBehind { drawRoundRect(V2.CardBorder, cornerRadius = CornerRadius(10.dp.toPx()), style = Stroke(1.dp.toPx())) }
            .padding(padding),
        content = content,
    )
}

/** A page title and its one-line subtitle ("Connect to PC / Choose how you want to connect."). */
@Composable
fun PageHeading(title: String, subtitle: String?, modifier: Modifier = Modifier) {
    Column(modifier.fillMaxWidth()) {
        Text(
            title, style = MaterialTheme.typography.headlineSmall, color = V2.Text,
            modifier = Modifier.semantics { heading() },
        )
        if (subtitle != null) {
            Spacer(Modifier.height(4.dp))
            Text(subtitle, style = MaterialTheme.typography.bodyLarge, color = V2.Subtle)
        }
    }
}

/** The flat blue primary action. White on #1E6FE8 is 4.6 : 1. */
@Composable
fun PrimaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    contentDescription: String? = null,
) = FlatButton(text, onClick, modifier, enabled, primary = true, contentDescription = contentDescription)

/** The white secondary action: a #D0D7E2 border and dark text. */
@Composable
fun SecondaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    contentDescription: String? = null,
) = FlatButton(text, onClick, modifier, enabled, primary = false, contentDescription = contentDescription)

@Composable
private fun FlatButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier,
    enabled: Boolean,
    primary: Boolean,
    contentDescription: String?,
) {
    val interaction = remember { MutableInteractionSource() }
    // The pressed state is immediate; only the release animates back (section 12.5).
    val pressed by interaction.collectIsPressedAsState()
    val tick = rememberTick()
    val fill by animateFloatAsState(
        if (pressed) 1f else 0f,
        if (pressed) tween(0) else ease(Motion.SHORT), label = "buttonPress",
    )
    val bg = when {
        !enabled -> if (primary) V2.Border else V2.Page
        primary -> lerpColor(V2.Blue, V2.BluePressed, fill)
        else -> lerpColor(V2.Card, V2.Pressed, fill)
    }
    Box(
        modifier
            .heightIn(min = 48.dp)
            .widthIn(min = 96.dp)
            .clip(ControlShape)
            .background(bg)
            .then(
                if (primary) Modifier
                else Modifier.drawBehind {
                    drawRoundRect(
                        if (enabled) V2.Border else V2.CardBorder, cornerRadius = CornerRadius(6.dp.toPx()),
                        style = Stroke(1.dp.toPx()),
                    )
                }
            )
            .clickable(interaction, indication = null, enabled = enabled, role = Role.Button) { tick(); onClick() }
            .then(if (contentDescription != null) Modifier.semantics { this.contentDescription = contentDescription } else Modifier)
            .padding(horizontal = 20.dp, vertical = 12.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text, style = MaterialTheme.typography.labelLarge, textAlign = TextAlign.Center,
            color = when {
                !enabled -> V2.Disabled
                primary -> Color.White
                else -> V2.Text
            },
        )
    }
}

/**
 * A tappable row inside a card: a line icon, a title, a one-line description and a chevron. A row that is
 * not available yet stays visible but disabled, with [trailingNote] saying why ("Coming soon").
 */
@Composable
fun NavRow(
    icon: LineIcon,
    title: String,
    detail: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    trailingNote: String? = null,
) {
    val tick = rememberTick()
    val description = buildString {
        append(title); append(". "); append(detail)
        if (trailingNote != null) { append(". "); append(trailingNote) }
    }
    Row(
        modifier
            .fillMaxWidth()
            .heightIn(min = 64.dp)
            .clip(ControlShape)
            .clickable(enabled = enabled, role = Role.Button) { tick(); onClick() }
            .semantics { contentDescription = description }
            .padding(vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier.size(40.dp).clip(RoundedCornerShape(8.dp)).background(if (enabled) V2.BlueSoft else V2.Page),
            contentAlignment = Alignment.Center,
        ) {
            LineIconView(icon, 22.dp, if (enabled) V2.Blue else V2.Disabled)
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(
                title, style = MaterialTheme.typography.titleMedium,
                color = if (enabled) V2.Text else V2.Disabled,
            )
            Text(detail, style = MaterialTheme.typography.bodyMedium, color = if (enabled) V2.Subtle else V2.Disabled)
        }
        if (trailingNote != null) {
            Text(trailingNote, style = MaterialTheme.typography.bodyMedium, color = V2.Disabled)
            Spacer(Modifier.width(8.dp))
        }
        if (enabled) LineIconView(LineIcon.Chevron, 20.dp, V2.Border)
    }
}

/** The three-item bottom navigation bar (Connect / Camera / Settings); the active item is blue. */
@Composable
fun BottomNav(labels: List<String>, icons: List<LineIcon>, selected: Int, onSelect: (Int) -> Unit, modifier: Modifier = Modifier) {
    val tick = rememberTick()
    Row(
        modifier
            .fillMaxWidth()
            .background(V2.Card)
            .drawBehind { drawLine(V2.CardBorder, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
            .selectableGroup(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        labels.forEachIndexed { i, label ->
            val active = i == selected
            val color = if (active) V2.Blue else V2.Subtle
            Column(
                Modifier
                    .weight(1f)
                    .heightIn(min = 56.dp)
                    .selectable(active, role = Role.Tab) { if (!active) { tick(); onSelect(i) } }
                    .padding(vertical = 8.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center,
            ) {
                LineIconView(icons[i], 24.dp, color)
                Spacer(Modifier.height(2.dp))
                Text(
                    label, style = MaterialTheme.typography.labelLarge, color = color,
                    fontWeight = if (active) FontWeight.Bold else FontWeight.Normal,
                )
            }
        }
    }
}

/** Front / Back as a segmented control: one pill, the selected half filled blue. */
@Composable
fun SegmentedControl(options: List<String>, selectedIndex: Int, onSelect: (Int) -> Unit, modifier: Modifier = Modifier) {
    val tick = rememberTick()
    Row(
        modifier
            .fillMaxWidth()
            .clip(ControlShape)
            .background(V2.Page)
            .drawBehind { drawRoundRect(V2.Border, cornerRadius = CornerRadius(6.dp.toPx()), style = Stroke(1.dp.toPx())) }
            .padding(3.dp)
            .selectableGroup(),
    ) {
        options.forEachIndexed { i, label ->
            val sel = i == selectedIndex
            Box(
                Modifier
                    .weight(1f)
                    .heightIn(min = 44.dp)
                    .clip(RoundedCornerShape(4.dp))
                    .background(if (sel) V2.Blue else Color.Transparent)
                    .selectable(sel, role = Role.RadioButton) { if (!sel) { tick(); onSelect(i) } },
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    label, style = MaterialTheme.typography.labelLarge, maxLines = 1,
                    color = if (sel) Color.White else V2.Text,
                )
            }
        }
    }
}

/** One of the three stat tiles under the preview: a big value and its label. */
@Composable
fun StatTile(value: String, label: String, modifier: Modifier = Modifier) {
    Column(
        modifier
            .clip(CardShape)
            .background(V2.Card)
            .drawBehind { drawRoundRect(V2.CardBorder, cornerRadius = CornerRadius(10.dp.toPx()), style = Stroke(1.dp.toPx())) }
            .padding(vertical = 12.dp, horizontal = 8.dp)
            .semantics { contentDescription = "$label: $value" },
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(value, style = MaterialTheme.typography.titleLarge, color = V2.Text, maxLines = 1)
        Text(label, style = MaterialTheme.typography.bodyMedium, color = V2.Subtle, maxLines = 1)
    }
}

/** A flat dark pill over the preview (about 70 % black, white text). */
@Composable
fun DarkChip(text: String, modifier: Modifier = Modifier) {
    Text(
        text, style = MaterialTheme.typography.labelLarge, color = V2.ChipText,
        modifier = modifier
            .clip(RoundedCornerShape(50))
            .background(V2.Chip)
            .padding(horizontal = 10.dp, vertical = 5.dp),
    )
}

/** One entry of a dropdown. An entry this phone cannot do stays visible but disabled (section 10.3). */
data class Choice(val label: String, val enabled: Boolean = true)

/**
 * A dropdown (combo box) replacing the XP radio group: the label, the current value, a chevron, and a menu
 * that fades and scales open in 120 ms. Entries the phone cannot do are shown disabled, and [reasons] (the
 * lines FpsReason computes) are printed underneath, because a closed dropdown hides them.
 */
@Composable
fun FlatDropdown(
    label: String,
    choices: List<Choice>,
    selectedIndex: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    reasons: List<String> = emptyList(),
) {
    var open by remember { mutableStateOf(false) }
    val tick = rememberTick()
    val current = choices.getOrNull(selectedIndex)?.label ?: ""
    Column(modifier.fillMaxWidth()) {
        Text(label, style = MaterialTheme.typography.titleMedium, color = V2.Text)
        Spacer(Modifier.height(6.dp))
        Box {
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = 48.dp)
                    .clip(ControlShape)
                    .background(V2.Card)
                    .drawBehind { drawRoundRect(V2.Border, cornerRadius = CornerRadius(6.dp.toPx()), style = Stroke(1.dp.toPx())) }
                    .clickable(role = Role.DropdownList) { tick(); open = true }
                    .semantics { contentDescription = "$label: $current" }
                    .padding(horizontal = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(current, style = MaterialTheme.typography.bodyLarge, color = V2.Text, modifier = Modifier.weight(1f))
                Canvas(Modifier.size(18.dp)) {
                    // A chevron pointing down.
                    rotate(90f) { drawLineIcon(LineIcon.Chevron, V2.Subtle) }
                }
            }
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                choices.forEachIndexed { i, c ->
                    DropdownMenuItem(
                        text = {
                            Text(
                                c.label, style = MaterialTheme.typography.bodyLarge,
                                color = if (!c.enabled) V2.Disabled else if (i == selectedIndex) V2.Blue else V2.Text,
                            )
                        },
                        enabled = c.enabled,
                        onClick = { open = false; if (i != selectedIndex) { tick(); onSelect(i) } },
                    )
                }
            }
        }
        reasons.forEach {
            Spacer(Modifier.height(4.dp))
            Text(it, style = MaterialTheme.typography.bodyMedium, color = V2.Subtle)
        }
    }
}

/**
 * A toggle switch row: the label (and an optional detail), the switch on the right. Off is a grey track
 * with a visible border, so the state never depends on colour alone. The thumb moves and the track
 * crossfades together in 150 ms.
 */
@Composable
fun FlatSwitchRow(
    text: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    detail: String? = null,
    enabled: Boolean = true,
) {
    val tick = rememberTick()
    Row(
        modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .toggleable(value = checked, enabled = enabled, role = Role.Switch) { tick(); onCheckedChange(it) }
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f).padding(end = 12.dp)) {
            Text(text, style = MaterialTheme.typography.bodyLarge, color = if (enabled) V2.Text else V2.Disabled)
            if (detail != null) Text(detail, style = MaterialTheme.typography.bodyMedium, color = V2.Subtle)
        }
        FlatSwitch(checked, enabled)
    }
}

/** The switch graphic on its own (graphics only; the row carries the semantics). */
@Composable
fun FlatSwitch(checked: Boolean, enabled: Boolean = true) {
    val t by animateFloatAsState(if (checked) 1f else 0f, ease(Motion.TOGGLE), label = "switch")
    Canvas(Modifier.size(48.dp, 28.dp)) {
        val h = 28.dp.toPx()
        val w = 48.dp.toPx()
        val r = CornerRadius(h / 2)
        val track = if (!enabled) V2.CardBorder else lerpColor(V2.TrackOff, V2.Blue, t)
        drawRoundRect(track, size = Size(w, h), cornerRadius = r)
        // The off state keeps a border, so it reads without colour.
        if (t < 1f) {
            drawRoundRect(
                if (enabled) V2.TrackOffBorder.copy(alpha = 1f - t) else V2.Border,
                size = Size(w, h), cornerRadius = r, style = Stroke(1.5.dp.toPx()),
            )
        }
        val pad = 3.dp.toPx()
        val thumbR = (h - 2 * pad) / 2
        val cx = pad + thumbR + t * (w - 2 * pad - 2 * thumbR)
        drawCircle(V2.Shadow, thumbR, Offset(cx, h / 2 + 1.dp.toPx()))
        drawCircle(if (enabled) V2.Thumb else V2.Page, thumbR, Offset(cx, h / 2))
        if (t < 1f) drawCircle(V2.Border.copy(alpha = 1f - t), thumbR, Offset(cx, h / 2), style = Stroke(1.dp.toPx()))
    }
}

/** A slider row: the label, the value on the right, the track underneath. */
@Composable
fun FlatSliderRow(
    label: String,
    value: Float,
    valueText: String,
    range: ClosedFloatingPointRange<Float>,
    onValueChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
    steps: Int = 0,
    enabled: Boolean = true,
) {
    Column(modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(label, style = MaterialTheme.typography.titleMedium, color = V2.Text, modifier = Modifier.weight(1f))
            Text(valueText, style = MaterialTheme.typography.bodyLarge, color = V2.Subtle)
        }
        Slider(
            value = value.coerceIn(range.start, range.endInclusive),
            onValueChange = onValueChange,
            valueRange = range,
            steps = steps,
            enabled = enabled,
            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).semantics { contentDescription = label },
            colors = SliderDefaults.colors(
                thumbColor = V2.Blue, activeTrackColor = V2.Blue, inactiveTrackColor = V2.TrackOff,
                disabledThumbColor = V2.Disabled, disabledActiveTrackColor = V2.Border, disabledInactiveTrackColor = V2.CardBorder,
            ),
        )
    }
}

/**
 * The scan screen's radar (section 3.4): concentric rings with a sweep at a constant angular speed, and a
 * dot per device found. With reduced motion the sweep stands still ([animate] false) and the caller shows
 * the green XP marquee instead.
 */
@Composable
fun Radar(dots: Int, modifier: Modifier = Modifier, animate: Boolean = true) {
    val sweep = if (animate) {
        rememberInfiniteTransition(label = "radar").animateFloat(
            0f, 360f, infiniteRepeatable(tween(3200, easing = LinearEasing)), label = "radarSweep",
        ).value
    } else {
        45f
    }
    val label = stringResource(R.string.scan_radar_description)
    Canvas(modifier.semantics { contentDescription = label }) {
        val c = Offset(size.width / 2, size.height / 2)
        val rMax = size.minDimension / 2 - 2.dp.toPx()
        val px = 1.5.dp.toPx()
        for (i in 3 downTo 1) drawCircle(V2.RadarRing, rMax * i / 3f, c, style = Stroke(px))
        drawLine(V2.RadarRing, Offset(c.x - rMax, c.y), Offset(c.x + rMax, c.y), px / 1.5f)
        drawLine(V2.RadarRing, Offset(c.x, c.y - rMax), Offset(c.x, c.y + rMax), px / 1.5f)
        // The sweep: a soft wedge trailing the leading edge.
        rotate(sweep, c) {
            drawArc(
                Brush.sweepGradient(0f to V2.RadarSweep, 0.17f to Color.Transparent, 1f to Color.Transparent, center = c),
                startAngleOffset, 60f, useCenter = true,
                topLeft = Offset(c.x - rMax, c.y - rMax), size = Size(rMax * 2, rMax * 2),
            )
            drawLine(V2.RadarDot, c, Offset(c.x + rMax, c.y), px, StrokeCap.Round)
        }
        // Found devices: a dot each, spread around the middle ring.
        for (i in 0 until dots) {
            val a = Math.toRadians(-60.0 + i * 55.0)
            val rr = rMax * (0.45f + 0.18f * (i % 3))
            val p = Offset(c.x + rr * Math.cos(a).toFloat(), c.y + rr * Math.sin(a).toFloat())
            drawCircle(V2.RadarDot.copy(alpha = 0.25f), 9.dp.toPx(), p)
            drawCircle(V2.RadarDot, 4.5.dp.toPx(), p)
        }
        drawCircle(V2.RadarDot, 5.dp.toPx(), c)
    }
}

private const val startAngleOffset = 0f

/** The monitor illustration the pairing dialog and the Connect screen use (a PC, drawn flat). */
@Composable
fun MonitorIllustration(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val s = size.minDimension
        val w = s * 0.92f
        val h = w * 0.62f
        val left = (size.width - w) / 2
        val top = (size.height - (h + s * 0.18f)) / 2
        val px = 2.dp.toPx()
        drawRoundRect(V2.BlueSoft, Offset(left, top), Size(w, h), CornerRadius(s * 0.06f))
        drawRoundRect(V2.Blue, Offset(left, top), Size(w, h), CornerRadius(s * 0.06f), style = Stroke(px))
        // The screen's content: three bars, like a list of devices.
        for (i in 0 until 3) {
            val y = top + h * (0.28f + i * 0.2f)
            drawRoundRect(
                V2.Blue.copy(alpha = 0.55f - i * 0.12f), Offset(left + w * 0.16f, y),
                Size(w * (0.62f - i * 0.12f), h * 0.085f), CornerRadius(h * 0.05f),
            )
        }
        // Stand and foot.
        drawRect(V2.Blue, Offset(size.width / 2 - w * 0.05f, top + h), Size(w * 0.1f, s * 0.11f))
        drawRoundRect(
            V2.Blue, Offset(size.width / 2 - w * 0.22f, top + h + s * 0.11f),
            Size(w * 0.44f, s * 0.05f), CornerRadius(s * 0.025f),
        )
    }
}

/** Colour mix without pulling in the graphics lerp import everywhere. */
internal fun lerpColor(a: Color, b: Color, t: Float): Color = androidx.compose.ui.graphics.lerp(a, b, t.coerceIn(0f, 1f))

/** True when transitions should be instant. Re-exported so v2 code needn't import the theme package. */
@Composable
fun reducedMotion(): Boolean = LocalReducedMotion.current
