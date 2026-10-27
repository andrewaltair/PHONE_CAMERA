package io.github.whoissaaif.mycam.ui.xp

import androidx.compose.foundation.Canvas
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import io.github.whoissaaif.mycam.ui.theme.Xp

/**
 * MyCam's sunny hills, painted in code (no image assets, nothing of Microsoft's): a sky gradient, a few soft
 * clouds and three Bezier hills. [night] paints the same place after dark for the dim screen: a navy sky, a
 * faint hill silhouette and three dim stars. Static: nothing animates.
 */
@Composable
fun HillsScene(modifier: Modifier = Modifier, night: Boolean = false) {
    Canvas(modifier) { if (night) drawNight() else drawDay() }
}

private fun DrawScope.drawDay() {
    val w = size.width
    val h = size.height
    drawRect(Brush.verticalGradient(0f to Xp.SkyTop, 0.62f to Xp.SkyHorizon, startY = 0f, endY = h))
    cloud(Offset(w * 0.22f, h * 0.14f), w * 0.16f)
    cloud(Offset(w * 0.74f, h * 0.24f), w * 0.12f)
    cloud(Offset(w * 0.5f, h * 0.06f), w * 0.08f)
    // Back hill: long and pale, right side.
    drawPath(hill(w, h, startY = 0.66f, c1 = Offset(0.35f, 0.5f), c2 = Offset(0.7f, 0.54f), endY = 0.6f), Xp.HillBack)
    // Middle hill: from the left.
    drawPath(hill(w, h, startY = 0.68f, c1 = Offset(0.25f, 0.58f), c2 = Offset(0.55f, 0.74f), endY = 0.8f), Xp.HillMid)
    // Front hill: the big soft curve, lit from the top.
    drawPath(
        hill(w, h, startY = 0.86f, c1 = Offset(0.4f, 0.68f), c2 = Offset(0.75f, 0.72f), endY = 0.84f),
        Brush.verticalGradient(listOf(Xp.HillFrontLight, Xp.HillFront), startY = h * 0.7f, endY = h),
    )
}

private fun DrawScope.drawNight() {
    val w = size.width
    val h = size.height
    drawRect(Brush.verticalGradient(listOf(Xp.NightTop, Xp.NightBottom)))
    listOf(Offset(0.18f, 0.12f) to 1.6f, Offset(0.71f, 0.08f) to 1.2f, Offset(0.86f, 0.3f) to 1.0f).forEach { (p, r) ->
        drawCircle(Xp.NightStar.copy(alpha = 0.45f), r * density, Offset(p.x * w, p.y * h))
    }
    drawPath(hill(w, h, startY = 0.9f, c1 = Offset(0.4f, 0.8f), c2 = Offset(0.75f, 0.84f), endY = 0.88f), Xp.NightHill)
}

/** A hill from the left edge to the right edge along a cubic Bezier, closed along the bottom. */
private fun hill(w: Float, h: Float, startY: Float, c1: Offset, c2: Offset, endY: Float) = Path().apply {
    moveTo(0f, startY * h)
    cubicTo(c1.x * w, c1.y * h, c2.x * w, c2.y * h, w, endY * h)
    lineTo(w, h)
    lineTo(0f, h)
    close()
}

/** A soft cloud: overlapping white puffs with feathered edges. */
private fun DrawScope.cloud(c: Offset, r: Float) {
    val puffs = listOf(Offset(-0.9f, 0.15f) to 0.55f, Offset(-0.3f, -0.2f) to 0.75f, Offset(0.4f, -0.05f) to 0.65f, Offset(1.0f, 0.2f) to 0.45f)
    for ((o, s) in puffs) {
        val center = Offset(c.x + o.x * r, c.y + o.y * r)
        val radius = r * s
        drawCircle(
            Brush.radialGradient(listOf(Color.White.copy(alpha = 0.95f), Color.White.copy(alpha = 0.85f), Color.White.copy(alpha = 0f)), center, radius),
            radius, center,
        )
    }
    // Flat-ish base.
    drawOval(Color.White.copy(alpha = 0.6f), Offset(c.x - r * 1.2f, c.y), Size(r * 2.4f, r * 0.45f))
}
