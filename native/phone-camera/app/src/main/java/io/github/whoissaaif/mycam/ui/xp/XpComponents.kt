package io.github.whoissaaif.mycam.ui.xp

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.keyframes
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.snap
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.progressSemantics
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathOperation
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.translate
import androidx.compose.ui.graphics.drawOutline
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.layout.onPlaced
import androidx.compose.ui.layout.positionInParent
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.ui.theme.LocalReducedMotion
import io.github.whoissaaif.mycam.ui.theme.V2
import io.github.whoissaaif.mycam.ui.theme.Xp

// Windows XP "Luna" building blocks for Compose (docs/redesign.md sections 3 and 6). Drawn from scratch: XP is the
// inspiration, nothing of Microsoft's is shipped. The PC window draws the same parts with Direct2D.

/** Duration helper: [ms] normally, 0 when reduced motion is on. */
@Composable
fun motionMs(ms: Int): Int = if (LocalReducedMotion.current) 0 else ms

/** Light haptic for option changes and button presses (section 6.3). */
@Composable
fun rememberTick(): () -> Unit {
    val haptics = LocalHapticFeedback.current
    return remember(haptics) { { haptics.performHapticFeedback(HapticFeedbackType.SegmentTick) } }
}

// --- Title bar ---------------------------------------------------------------------------------------

/** XP caption as the app header: blue gloss, a light highlight line, white bold title with a dark shadow. */
@Composable
fun TitleBarHeader(
    title: String,
    iconRes: Int,
    modifier: Modifier = Modifier,
    compact: Boolean = false,
    trailing: (@Composable RowScope.() -> Unit)? = null,
) {
    // The caption gloss starts below the status bar; the status bar itself gets the caption's deep blue.
    val statusTop = WindowInsets.statusBars.getTop(LocalDensity.current).toFloat()
    Row(
        modifier
            .fillMaxWidth()
            .drawBehind { drawTitleBar(statusTop) }
            .windowInsetsPadding(WindowInsets.statusBars)
            .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal))
            .heightIn(min = if (compact) 48.dp else 56.dp)
            .padding(start = 12.dp, end = if (trailing != null) 4.dp else 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Image(painterResource(iconRes), contentDescription = null, modifier = Modifier.size(if (compact) 24.dp else 32.dp))
        Spacer(Modifier.width(8.dp))
        Text(
            title,
            style = (if (compact) MaterialTheme.typography.titleMedium else MaterialTheme.typography.titleLarge).copy(shadow = TitleTextShadow),
            color = Xp.TitleText,
            modifier = Modifier.weight(1f).semantics { heading() },
        )
        trailing?.invoke(this)
    }
}

/** XP caption text shadow: 1 dp down and right, dark navy. */
val TitleTextShadow = Shadow(Xp.TitleShadow, Offset(2f, 2f), 1f)

/** White text over the sky picture (first run): a soft navy halo so it reads on clouds too. */
val SkyTextShadow = Shadow(Xp.TitleShadow.copy(alpha = 0.85f), Offset(0f, 2f), 10f)

/**
 * The XP Luna caption (docs/redesign.md 3.2): a light gloss band at the top with a 1 dp highlight line, deepening
 * to the main blue, then a darker bottom edge. [captionTop] is where the caption starts (below the status bar).
 */
private fun DrawScope.drawTitleBar(captionTop: Float = 0f) {
    if (captionTop > 0f) drawRect(Xp.TitleBarGloss, size = Size(size.width, captionTop))
    val h = size.height - captionTop
    drawRect(
        Brush.verticalGradient(
            0f to Xp.TitleBarTop, 0.14f to Xp.TitleBarTop, 0.32f to Xp.TitleBarGloss, 0.62f to Xp.TitleBarMid,
            0.9f to Xp.TitleBarBottom, 1f to Xp.TitleBarBottom,
            startY = captionTop, endY = size.height,
        ),
        topLeft = Offset(0f, captionTop), size = Size(size.width, h),
    )
    val px = 1.dp.toPx()
    drawLine(Xp.TitleBarHighlight, Offset(0f, captionTop + px * 1.5f), Offset(size.width, captionTop + px * 1.5f), strokeWidth = px)
    drawLine(Xp.TitleBarEdge, Offset(0f, size.height - px), Offset(size.width, size.height - px), strokeWidth = px * 2)
}

// --- Property-sheet tabs ----------------------------------------------------------------------------

/** XP property-sheet tabs. The selected tab is lifted, page coloured, with the orange top line that slides. */
@Composable
fun XpTabs(titles: List<String>, selected: Int, onSelect: (Int) -> Unit, modifier: Modifier = Modifier) {
    val density = LocalDensity.current
    val lefts = remember { mutableStateMapOf<Int, Dp>() }
    val widths = remember { mutableStateMapOf<Int, Dp>() }
    val lineX by animateDpAsState(lefts[selected] ?: 0.dp, tween(motionMs(150)), label = "tabLineX")
    val lineW by animateDpAsState(widths[selected] ?: 0.dp, tween(motionMs(150)), label = "tabLineW")
    val tick = rememberTick()
    Box(
        modifier
            .fillMaxWidth()
            .background(Xp.Surface)
            .drawBehind {
                drawLine(Xp.TabBorder, Offset(0f, size.height - 0.5f), Offset(size.width, size.height - 0.5f), 1.dp.toPx())
            }
            .padding(start = 8.dp, top = 8.dp),
    ) {
        Row(Modifier.selectableGroup(), verticalAlignment = Alignment.Bottom) {
            titles.forEachIndexed { i, title ->
                val isSel = i == selected
                Box(
                    Modifier
                        .onPlaced {
                            with(density) {
                                lefts[i] = it.positionInParent().x.toDp()
                                widths[i] = it.size.width.toDp()
                            }
                        }
                        .padding(top = if (isSel) 0.dp else 3.dp)
                        .widthIn(min = 104.dp)
                        .heightIn(min = if (isSel) 48.dp else 45.dp)
                        .drawBehind { drawTab(isSel) }
                        .selectable(
                            selected = isSel, role = Role.Tab,
                            interactionSource = remember { MutableInteractionSource() }, indication = null,
                        ) { if (!isSel) { tick(); onSelect(i) } }
                        .padding(horizontal = 16.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        title, style = MaterialTheme.typography.titleMedium,
                        fontWeight = if (isSel) FontWeight.Bold else FontWeight.Normal, color = Xp.Text,
                    )
                }
            }
        }
        // The orange hot line on top of the selected tab.
        if (lineW > 0.dp) {
            Box(
                Modifier
                    .offset { IntOffset(lineX.roundToPx(), 0) }
                    .width(lineW)
                    .height(3.dp)
                    .clip(RoundedCornerShape(topStart = 3.dp, topEnd = 3.dp))
                    .background(Brush.verticalGradient(listOf(Xp.TabHotLine, Xp.TabHotLight))),
            )
        }
    }
}

private fun DrawScope.drawTab(selected: Boolean) {
    val r = 3.dp.toPx()
    val path = Path().apply {
        moveTo(0f, size.height)
        lineTo(0f, r)
        quadraticTo(0f, 0f, r, 0f)
        lineTo(size.width - r, 0f)
        quadraticTo(size.width, 0f, size.width, r)
        lineTo(size.width, size.height)
    }
    drawPath(
        path,
        if (selected) Brush.verticalGradient(listOf(Xp.TabPage, Xp.TabPage))
        else Brush.verticalGradient(listOf(Color.White, Xp.ButtonMid, Xp.ButtonBottom)),
    )
    drawPath(path, Xp.TabBorder, style = Stroke(1.dp.toPx()))
}

// --- Task pane and task groups ----------------------------------------------------------------------

/** XP Explorer task-pane background. */
fun Modifier.taskPane(): Modifier = background(Brush.verticalGradient(listOf(Xp.TaskPaneTop, Xp.TaskPaneBottom)))

enum class GroupKind { Normal, Hero, Paused }

/**
 * XP task group: a gradient header with rounded top corners and a body underneath. Normal groups have a
 * white-to-lavender header and a round chevron button; the hero group a dark blue (or, paused, amber)
 * header with white text. [expanded] null means it can't collapse.
 */
@Composable
fun TaskGroup(
    title: String,
    modifier: Modifier = Modifier,
    kind: GroupKind = GroupKind.Normal,
    expanded: Boolean? = null,
    onToggle: () -> Unit = {},
    bodyColor: Color = Xp.GroupBody,
    trailing: (@Composable () -> Unit)? = null,
    belowHeader: (@Composable () -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    val headerShape = RoundedCornerShape(topStart = 4.dp, topEnd = 4.dp)
    val headerBrush = when (kind) {
        GroupKind.Normal -> Brush.horizontalGradient(listOf(Xp.GroupHeaderStart, Xp.GroupHeaderEnd))
        GroupKind.Hero -> Brush.horizontalGradient(listOf(Xp.GroupHeroStart, Xp.GroupHeroEnd))
        GroupKind.Paused -> Brush.horizontalGradient(listOf(Xp.GroupPausedStart, Xp.GroupPausedEnd))
    }
    val titleColor = if (kind == GroupKind.Normal) Xp.GroupTitle else Color.White
    val rotation by animateFloatAsState(if (expanded == false) 180f else 0f, tween(motionMs(180)), label = "chevron")
    val expandLabel = stringResource(R.string.group_expand)
    val collapseLabel = stringResource(R.string.group_collapse)
    val expandedText = stringResource(R.string.group_expanded)
    val collapsedText = stringResource(R.string.group_collapsed)
    Column(modifier.fillMaxWidth()) {
        Row(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 48.dp)
                .clip(headerShape)
                .background(headerBrush)
                .then(
                    if (expanded == null) Modifier
                    else Modifier
                        .clickable(
                            interactionSource = remember { MutableInteractionSource() }, indication = null,
                            role = Role.Button, onClickLabel = if (expanded) collapseLabel else expandLabel,
                            onClick = onToggle,
                        )
                        .semantics { stateDescription = if (expanded) expandedText else collapsedText }
                )
                .padding(start = 12.dp, end = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                title,
                style = if (kind == GroupKind.Normal) MaterialTheme.typography.titleMedium else MaterialTheme.typography.titleLarge,
                color = titleColor,
                modifier = Modifier.weight(1f).padding(vertical = 8.dp).semantics { heading() },
            )
            trailing?.invoke()
            if (expanded != null) {
                Spacer(Modifier.width(8.dp))
                Chevron(Modifier.rotate(rotation))
            }
        }
        val body: @Composable () -> Unit = {
            Column(
                Modifier
                    .fillMaxWidth()
                    .background(bodyColor)
                    .drawBehind {
                        val w = 1.dp.toPx()
                        drawLine(Xp.GroupBorder, Offset(w / 2, 0f), Offset(w / 2, size.height), w)
                        drawLine(Xp.GroupBorder, Offset(size.width - w / 2, 0f), Offset(size.width - w / 2, size.height), w)
                        drawLine(Xp.GroupBorder, Offset(0f, size.height - w / 2), Offset(size.width, size.height - w / 2), w)
                    },
            ) {
                belowHeader?.invoke()
                Column(Modifier.padding(horizontal = 12.dp, vertical = 12.dp), content = content)
            }
        }
        if (expanded == null) {
            body()
        } else {
            AnimatedVisibility(
                visible = expanded,
                enter = expandVertically(tween(motionMs(180), easing = FastOutSlowInEasing)) + fadeIn(tween(motionMs(180))),
                exit = shrinkVertically(tween(motionMs(180), easing = FastOutSlowInEasing)) + fadeOut(tween(motionMs(180))),
            ) { body() }
        }
    }
}

/** The round XP chevron button (double arrow pointing up = collapse). */
@Composable
private fun Chevron(modifier: Modifier = Modifier) {
    Canvas(modifier.size(22.dp)) {
        val r = size.minDimension / 2
        drawCircle(Brush.verticalGradient(listOf(Color.White, Xp.GroupHeaderEnd)), r)
        drawCircle(Color(0xFF9DB2E8), r - 0.5f, style = Stroke(1.dp.toPx()))
        val s = size.minDimension
        val stroke = Stroke(1.6.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round)
        for (dy in listOf(-0.1f, 0.12f)) {
            val p = Path().apply {
                moveTo(s * 0.32f, s * (0.55f + dy))
                lineTo(s * 0.5f, s * (0.37f + dy))
                lineTo(s * 0.68f, s * (0.55f + dy))
            }
            drawPath(p, Xp.GroupTitle, style = stroke)
        }
    }
}

// --- Buttons ----------------------------------------------------------------------------------------

/**
 * XP push button frame: white-to-beige gradient, dark blue border, radius 3. Pressed: inverted gradient,
 * the orange hot-track ring and contents shifted 1 dp down and right. [isDefault] adds the blue inner ring.
 */
@Composable
fun XpButtonFrame(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    isDefault: Boolean = false,
    selected: Boolean = false,
    role: Role = Role.Button,
    contentDescription: String? = null,
    contentPadding: Dp = 16.dp,
    content: @Composable RowScope.() -> Unit,
) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val tick = rememberTick()
    val shift = if (pressed && enabled) 1.dp else 0.dp
    Row(
        modifier
            .heightIn(min = 48.dp)
            .drawBehind { drawXpButton(enabled, pressed, isDefault, selected) }
            .then(
                if (role == Role.RadioButton) {
                    Modifier.selectable(selected, enabled = enabled, role = role, interactionSource = interaction, indication = null) {
                        tick(); onClick()
                    }
                } else {
                    Modifier.clickable(interaction, indication = null, enabled = enabled, role = role) { tick(); onClick() }
                }
            )
            .then(if (contentDescription != null) Modifier.semantics { this.contentDescription = contentDescription } else Modifier)
            .padding(horizontal = contentPadding)
            .offset(shift, shift),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.Center,
        content = content,
    )
}

private fun DrawScope.drawXpButton(enabled: Boolean, pressed: Boolean, isDefault: Boolean, selected: Boolean) {
    val r = CornerRadius(3.dp.toPx())
    val px = 1.dp.toPx()
    // Touch target is 48 dp; the drawn button sits 2 dp in from the top and bottom, like XP's 23 px button.
    val inset = 2.dp.toPx()
    val top = Offset(0f, inset)
    val sz = Size(size.width, size.height - 2 * inset)
    val fill = when {
        !enabled -> Brush.verticalGradient(listOf(Xp.ButtonDisabledFill, Xp.ButtonDisabledFill))
        selected -> Brush.verticalGradient(listOf(Color(0xFF4F86D9), Xp.Selection), startY = inset, endY = size.height - inset)
        pressed -> Brush.verticalGradient(0f to Xp.ButtonBottom, 0.15f to Xp.ButtonMid, 1f to Color.White, startY = inset, endY = size.height - inset)
        else -> Brush.verticalGradient(0f to Xp.ButtonTop, 0.85f to Xp.ButtonMid, 1f to Xp.ButtonBottom, startY = inset, endY = size.height - inset)
    }
    drawRoundRect(fill, top, sz, r)
    val ring = when {
        !enabled || selected -> null
        pressed -> listOf(Xp.ButtonHotLight, Xp.ButtonHotDark)
        isDefault -> listOf(Xp.ButtonDefaultLight, Xp.ButtonDefaultDark)
        else -> null
    }
    if (ring != null) {
        drawRoundRect(
            Brush.verticalGradient(ring, startY = inset, endY = size.height - inset),
            Offset(px * 2, inset + px * 2), Size(sz.width - px * 4, sz.height - px * 4), CornerRadius(2.dp.toPx()),
            style = Stroke(2.dp.toPx()),
        )
    }
    drawRoundRect(
        if (enabled) Xp.ButtonBorder else Xp.ButtonDisabledBorder,
        Offset(px / 2, inset + px / 2), Size(sz.width - px, sz.height - px), r, style = Stroke(px),
    )
}

/** XP push button with a text label. */
@Composable
fun XpButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    isDefault: Boolean = false,
    contentDescription: String? = null,
) {
    XpButtonFrame(onClick, modifier.widthIn(min = 88.dp), enabled = enabled, isDefault = isDefault, contentDescription = contentDescription) {
        Text(
            text, style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.Normal,
            color = if (enabled) Xp.Text else Xp.DisabledText, textAlign = TextAlign.Center,
        )
    }
}

/** A row of XP toggle buttons acting as one choice (zoom presets). The selected one fills blue. */
@Composable
fun XpToggleRow(
    options: List<String>,
    selectedIndex: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(modifier.selectableGroup(), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        options.forEachIndexed { i, label ->
            val sel = i == selectedIndex
            XpButtonFrame(
                onClick = { onSelect(i) }, selected = sel, role = Role.RadioButton, contentPadding = 4.dp,
                modifier = Modifier.weight(1f),
            ) {
                Text(label, style = MaterialTheme.typography.labelLarge, color = if (sel) Color.White else Xp.Text, maxLines = 1)
            }
        }
    }
}

enum class BadgeKind { Pause, Resume, Dim }

/** Glossy round XP badge: amber with pause bars, green with a play arrow, or night blue with a crescent. */
@Composable
fun GlossyBadge(kind: BadgeKind, size: Dp, modifier: Modifier = Modifier) {
    Canvas(modifier.size(size)) {
        val r = this.size.minDimension / 2
        val c = center
        val (light, dark) = when (kind) {
            BadgeKind.Pause -> Xp.AmberLight to Xp.AmberDark
            BadgeKind.Resume -> Xp.GoLight to Xp.GoDark
            BadgeKind.Dim -> Xp.NightBadgeLight to Xp.NightBadgeDark
        }
        drawCircle(Color.White, r, c)
        val inner = r * 0.86f
        drawCircle(
            Brush.radialGradient(listOf(light, dark), center = Offset(c.x - inner * 0.3f, c.y - inner * 0.45f), radius = inner * 1.6f),
            inner, c,
        )
        when (kind) {
            BadgeKind.Pause -> {
                val w = inner * 0.26f
                val h = inner * 0.95f
                drawRect(Color.White, Offset(c.x - inner * 0.42f, c.y - h / 2), Size(w, h))
                drawRect(Color.White, Offset(c.x + inner * 0.16f, c.y - h / 2), Size(w, h))
            }
            BadgeKind.Dim -> {
                val moon = inner * 0.5f
                val mc = Offset(c.x - inner * 0.06f, c.y + inner * 0.04f)
                val moonPath = Path().apply { addOval(Rect(mc, moon)) }
                val bite = Path().apply { addOval(Rect(Offset(mc.x + moon * 0.45f, mc.y - moon * 0.35f), moon * 0.85f)) }
                drawPath(Path.combine(PathOperation.Difference, moonPath, bite), Color.White)
            }
            BadgeKind.Resume -> {
                val p = Path().apply {
                    moveTo(c.x - inner * 0.28f, c.y - inner * 0.48f)
                    lineTo(c.x + inner * 0.52f, c.y)
                    lineTo(c.x - inner * 0.28f, c.y + inner * 0.48f)
                    close()
                }
                drawPath(p, Color.White)
            }
        }
        // XP gloss over the top half.
        drawOval(
            Brush.verticalGradient(listOf(Color.White.copy(alpha = 0.55f), Color.Transparent), startY = c.y - inner * 0.92f, endY = c.y + inner * 0.08f),
            topLeft = Offset(c.x - inner * 0.78f, c.y - inner * 0.92f), size = Size(inner * 1.56f, inner * 1.0f),
        )
    }
}

/**
 * The hero command: a 56 dp+ button with a badge, a bold title and a description. [go] makes it the GoGreen
 * "Resume" button with white text.
 */
@Composable
fun HeroCommandButton(
    title: String,
    description: String,
    badge: BadgeKind,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    go: Boolean = false,
) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val tick = rememberTick()
    val shift = if (pressed) 1.dp else 0.dp
    Row(
        modifier
            .fillMaxWidth()
            .heightIn(min = 64.dp)
            .drawBehind {
                if (go) drawGoButton(pressed) else drawXpButton(enabled = true, pressed = pressed, isDefault = true, selected = false)
            }
            .clickable(interaction, indication = null, role = Role.Button) { tick(); onClick() }
            .padding(horizontal = 16.dp, vertical = 8.dp)
            .offset(shift, shift),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        GlossyBadge(badge, 36.dp)
        Spacer(Modifier.width(12.dp))
        Column {
            Text(title, style = MaterialTheme.typography.titleMedium.copy(fontSize = MaterialTheme.typography.bodyLarge.fontSize),
                color = if (go) Color.White else Xp.Text)
            Text(description, style = MaterialTheme.typography.bodyMedium, color = if (go) Color.White else Xp.Subtle)
        }
    }
}

/** GoGreen fill: a thin light gloss band on top, text-safe green (4.8 : 1 and darker) below it. */
private fun DrawScope.drawGoButton(pressed: Boolean, radius: Dp = 3.dp) {
    val r = CornerRadius(radius.toPx())
    val brush = if (pressed) Brush.verticalGradient(listOf(Xp.GoBottom, Xp.GoTop))
    else Brush.verticalGradient(0f to Xp.GoGloss, 0.12f to Xp.GoTop, 1f to Xp.GoBottom)
    drawRoundRect(brush, cornerRadius = r)
    drawRoundRect(Xp.GoBorder, cornerRadius = r, style = Stroke(1.dp.toPx()))
}

/** A GoGreen push button with white text (first-run Next). */
@Composable
fun GoButton(text: String, onClick: () -> Unit, modifier: Modifier = Modifier) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val tick = rememberTick()
    val shift = if (pressed) 1.dp else 0.dp
    Box(
        modifier
            .heightIn(min = 48.dp)
            .widthIn(min = 112.dp)
            .drawBehind { drawGoButton(pressed) }
            .clickable(interaction, indication = null, role = Role.Button) { tick(); onClick() }
            .padding(horizontal = 16.dp)
            .offset(shift, shift),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, style = MaterialTheme.typography.labelLarge, color = Color.White)
    }
}

/** A task link (blue, underlined), 48 dp tall. */
@Composable
fun XpLink(text: String, onClick: () -> Unit, modifier: Modifier = Modifier, color: Color = Xp.Link) {
    Box(
        modifier
            .heightIn(min = 48.dp)
            .widthIn(min = 48.dp)
            .clickable(role = Role.Button, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        // Over a picture (first run) the white link gets the caption shadow so it reads on the sky.
        val style = MaterialTheme.typography.bodyMedium.let { if (color == Color.White) it.copy(shadow = SkyTextShadow) else it }
        Text(text, style = style, color = color, textDecoration = TextDecoration.Underline)
    }
}

// --- Checkbox and radio -----------------------------------------------------------------------------

/** XP checkbox: 20 dp white box with a dark blue border and a green check. The whole row toggles. */
@Composable
fun XpCheckbox(
    text: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    val tick = rememberTick()
    Row(
        modifier
            .heightIn(min = 48.dp)
            .toggleable(value = checked, enabled = enabled, role = Role.Checkbox) { tick(); onCheckedChange(it) },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Canvas(Modifier.size(20.dp)) {
            val s = size.minDimension
            drawRect(
                if (enabled) Brush.linearGradient(listOf(Xp.CheckFillTop, Color.White), Offset.Zero, Offset(s, s))
                else Brush.verticalGradient(listOf(Xp.ButtonDisabledFill, Xp.ButtonDisabledFill)),
            )
            drawRect(if (enabled) Xp.CheckBorder else Xp.ButtonDisabledBorder, style = Stroke(1.dp.toPx()))
            if (checked) {
                val p = Path().apply {
                    moveTo(s * 0.22f, s * 0.48f)
                    lineTo(s * 0.42f, s * 0.7f)
                    lineTo(s * 0.8f, s * 0.26f)
                }
                drawPath(
                    p, if (enabled) Xp.CheckMark else Xp.DisabledText,
                    style = Stroke(2.6.dp.toPx(), cap = StrokeCap.Square, join = StrokeJoin.Miter),
                )
            }
        }
        Spacer(Modifier.width(12.dp))
        Text(text, style = MaterialTheme.typography.bodyMedium, color = if (enabled) Xp.Text else Xp.DisabledText)
    }
}

/** XP radio: a 20 dp circle with a dark blue border and a green dot that scales in. */
@Composable
fun XpRadioButton(text: String, selected: Boolean, onClick: () -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true) {
    val tick = rememberTick()
    val dot by animateFloatAsState(
        if (selected) 1f else 0f,
        if (LocalReducedMotion.current) snap() else tween(120),
        label = "radioDot",
    )
    Row(
        modifier
            .heightIn(min = 48.dp)
            .selectable(selected = selected, enabled = enabled, role = Role.RadioButton) { if (!selected) { tick(); onClick() } }
            .padding(end = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Canvas(Modifier.size(20.dp)) {
            val r = size.minDimension / 2
            drawCircle(
                if (enabled) Brush.linearGradient(listOf(Xp.CheckFillTop, Color.White), Offset.Zero, Offset(size.width, size.height))
                else Brush.verticalGradient(listOf(Xp.ButtonDisabledFill, Xp.ButtonDisabledFill)),
                r,
            )
            drawCircle(if (enabled) Xp.CheckBorder else Xp.ButtonDisabledBorder, r - 0.5.dp.toPx(), style = Stroke(1.dp.toPx()))
            if (dot > 0f) {
                val dr = r * 0.45f * dot
                drawCircle(
                    if (enabled) Brush.radialGradient(listOf(Xp.GoLight, Xp.GoDark), center = Offset(center.x - dr * 0.3f, center.y - dr * 0.3f), radius = dr * 1.4f)
                    else Brush.verticalGradient(listOf(Xp.DisabledText, Xp.DisabledText)),
                    dr,
                )
            }
        }
        Spacer(Modifier.width(8.dp))
        Text(text, style = MaterialTheme.typography.bodyMedium, color = if (enabled) Xp.Text else Xp.DisabledText)
    }
}

/** A labelled group of XP radios that wraps onto more lines when narrow. */
@Composable
fun XpRadioGroup(
    options: List<String>,
    selectedIndex: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    enabled: (Int) -> Boolean = { true },
) {
    FlowRow(modifier.fillMaxWidth().selectableGroup()) {
        options.forEachIndexed { i, label ->
            XpRadioButton(label, selected = i == selectedIndex, onClick = { onSelect(i) }, enabled = enabled(i))
        }
    }
}

// --- Progress bar -----------------------------------------------------------------------------------

/**
 * XP's green segmented progress bar (section 6.1). [progress] null = marquee (3 chunks sliding, 2.0 s per
 * pass, 0.4 s rest); otherwise determinate, filled in whole chunks. With reduced motion the marquee becomes
 * a static "Working…" label.
 */
@Composable
fun XpProgressBar(progress: Float?, modifier: Modifier = Modifier, segments: Int = 0) {
    val reduced = LocalReducedMotion.current
    if (progress == null && reduced) {
        Text(stringResource(R.string.working), style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle, modifier = modifier)
        return
    }
    val pass = if (progress == null) {
        val t = rememberInfiniteTransition(label = "marquee")
        t.animateFloat(
            0f, 1f,
            infiniteRepeatable(keyframes {
                durationMillis = 2400
                0f at 0 using LinearEasing
                1f at 2000
                1f at 2400
            }),
            label = "marqueePass",
        )
    } else null
    Canvas(
        modifier
            .fillMaxWidth()
            .height(20.dp)
            .then(if (progress == null) Modifier.progressSemantics() else Modifier.progressSemantics(progress.coerceIn(0f, 1f))),
    ) {
        val r = CornerRadius(3.dp.toPx())
        drawRoundRect(Xp.ProgressTrack, cornerRadius = r)
        drawRoundRect(Xp.ProgressBorder, cornerRadius = r, style = Stroke(1.dp.toPx()))
        val pad = 4.dp.toPx()
        val chunkW = 8.dp.toPx()
        val gap = 2.dp.toPx()
        val chunkH = size.height - 2 * pad
        val innerW = size.width - 2 * pad
        val slots = ((innerW + gap) / (chunkW + gap)).toInt().coerceAtLeast(1)
        val chunkBrush = Brush.verticalGradient(
            0f to Xp.ChunkTop, 0.45f to Xp.ChunkLight, 0.55f to Xp.ChunkMid, 1f to Xp.ChunkBottom,
            startY = pad, endY = pad + chunkH,
        )
        fun chunk(x: Float) = drawRect(chunkBrush, Offset(x, pad), Size(chunkW, chunkH))
        if (progress != null && segments > 0) {
            // A step indicator: [segments] equal chunks, filled in whole steps.
            val segW = (innerW - (segments - 1) * gap) / segments
            val filled = Math.round(progress.coerceIn(0f, 1f) * segments)
            for (i in 0 until filled) drawRect(chunkBrush, Offset(pad + i * (segW + gap), pad), Size(segW, chunkH))
        } else if (progress != null) {
            val filled = (progress.coerceIn(0f, 1f) * slots).toInt()
            for (i in 0 until filled) chunk(pad + i * (chunkW + gap))
        } else {
            val p = pass?.value ?: 0f
            val step = chunkW + gap
            val start = -3 * step + p * (innerW + 3 * step)
            clipRect(pad, pad, size.width - pad, size.height - pad) {
                for (i in 0 until 3) chunk(pad + start + i * step)
            }
        }
    }
}

// --- LIVE pill, bottom bar, dialog frame ------------------------------------------------------------

/**
 * "LIVE" pill in GoGreen with white text (4.8 : 1). With [sweep], a one-shot gloss sweep plays when it
 * appears (then [onSwept]); never a looping pulse.
 */
@Composable
fun LivePill(modifier: Modifier = Modifier, sweep: Boolean = false, onSwept: () -> Unit = {}) {
    val reduced = LocalReducedMotion.current
    val play = sweep && !reduced
    val anim = remember { Animatable(if (play) 0f else 1f) }
    LaunchedEffect(Unit) {
        if (play) anim.animateTo(1f, tween(600, easing = LinearEasing))
        onSwept()
    }
    val shape = RoundedCornerShape(50)
    val spoken = stringResource(R.string.live_description)
    Box(
        modifier
            .clip(shape)
            .drawBehind {
                drawGoButton(pressed = false, radius = (size.height / 2).toDp())
                val v = anim.value
                if (v in 0.001f..0.999f) {
                    val band = size.width * 0.45f
                    val x = -band + v * (size.width + band)
                    drawRect(
                        Brush.horizontalGradient(
                            listOf(Color.Transparent, Color.White.copy(alpha = 0.55f), Color.Transparent), startX = x, endX = x + band,
                        )
                    )
                }
            }
            .padding(horizontal = 12.dp, vertical = 2.dp),
    ) {
        Text(
            stringResource(R.string.live), style = MaterialTheme.typography.labelSmall, color = Color.White,
            modifier = Modifier.semantics { contentDescription = spoken },
        )
    }
}

/** XP status-bar strip at the bottom of the screen with real actions (A8). */
@Composable
fun BottomBar(modifier: Modifier = Modifier, content: @Composable RowScope.() -> Unit) {
    Row(
        modifier
            .fillMaxWidth()
            .background(Xp.Surface)
            .drawBehind {
                drawLine(Xp.Bevel, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx())
                drawLine(Color.White, Offset(0f, 1.dp.toPx() * 1.5f), Offset(size.width, 1.dp.toPx() * 1.5f), 1.dp.toPx())
            }
            .windowInsetsPadding(WindowInsets.navigationBars)
            .heightIn(min = 56.dp)
            .padding(horizontal = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        content = content,
    )
}

/** A flat tappable item in the bottom bar: an optional leading icon and a label, 48 dp tall. */
@Composable
fun BottomBarItem(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    onTitleBar: Boolean = false,
    icon: (@Composable () -> Unit)? = null,
) {
    val tick = rememberTick()
    Row(
        modifier
            .heightIn(min = 48.dp)
            .clip(RoundedCornerShape(3.dp))
            .clickable(role = Role.Button) { tick(); onClick() }
            .padding(horizontal = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) {
            icon()
            Spacer(Modifier.width(8.dp))
        }
        Text(
            text,
            style = if (onTitleBar) MaterialTheme.typography.bodyMedium.copy(shadow = TitleTextShadow) else MaterialTheme.typography.bodyMedium,
            color = if (onTitleBar) Xp.TitleText else Xp.Link, textDecoration = TextDecoration.Underline,
        )
    }
}

/**
 * An XP dialog window: a blue title bar with a close box (rounded top corners), a beige body. Used for the
 * pairing request and the licence text.
 */
@Composable
fun XpDialogFrame(
    title: String,
    onClose: (() -> Unit)?,
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    val shape = RoundedCornerShape(8.dp, 8.dp, 2.dp, 2.dp)
    Column(
        modifier
            .widthIn(max = 480.dp)
            .fillMaxWidth()
            .clip(shape)
            // v2 flat kit: a white sheet, not the XP beige face. The Luna title bar stays, because the
            // app's own header uses the same one (docs/redesign-v2.md §4).
            .background(V2.Card)
            .drawWithContent {
                drawContent()
                // The window frame on top of the content, following the shape (rounded top, square-ish bottom).
                val w = 2.dp.toPx()
                val outline = shape.createOutline(Size(size.width - w, size.height - w), layoutDirection, this)
                translate(w / 2, w / 2) { drawOutline(outline, Xp.TitleBarEdge, style = Stroke(w)) }
            },
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 48.dp)
                .drawBehind { drawTitleBar() }
                .padding(start = 12.dp, end = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                title, style = MaterialTheme.typography.titleMedium.copy(shadow = TitleTextShadow),
                color = Xp.TitleText, modifier = Modifier.weight(1f).semantics { heading() },
            )
            if (onClose != null) CloseBox(onClose)
        }
        // Scrolls when the window is short (landscape, large text), so the buttons keep their full size.
        Column(Modifier.verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 16.dp), content = content)
    }
}

/** XP's red close box, with a 48 dp touch target. */
@Composable
private fun CloseBox(onClick: () -> Unit) {
    val label = stringResource(R.string.close)
    Box(
        Modifier
            .size(48.dp)
            .clickable(role = Role.Button, onClickLabel = label, onClick = onClick)
            .semantics { contentDescription = label },
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.size(26.dp)) {
            val r = CornerRadius(3.dp.toPx())
            drawRoundRect(Brush.verticalGradient(listOf(Xp.ErrorLight, Xp.ErrorDark)), cornerRadius = r)
            drawRoundRect(Color.White, cornerRadius = r, style = Stroke(1.dp.toPx()))
            val s = size.minDimension
            val stroke = 2.4.dp.toPx()
            drawLine(Color.White, Offset(s * 0.3f, s * 0.3f), Offset(s * 0.7f, s * 0.7f), stroke, StrokeCap.Round)
            drawLine(Color.White, Offset(s * 0.7f, s * 0.3f), Offset(s * 0.3f, s * 0.7f), stroke, StrokeCap.Round)
        }
    }
}

/** True when the available width is medium or larger (two panes). */
@Composable
fun WidthAware(content: @Composable (wide: Boolean) -> Unit) {
    BoxWithConstraints { content(maxWidth >= 600.dp) }
}

/** Spring with a slight overshoot, for the status badge pop. */
fun <T> popSpring() = spring<T>(dampingRatio = 0.55f, stiffness = 900f)
