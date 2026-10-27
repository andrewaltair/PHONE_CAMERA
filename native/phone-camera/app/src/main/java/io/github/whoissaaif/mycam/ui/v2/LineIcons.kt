package io.github.whoissaaif.mycam.ui.v2

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.ui.theme.V2

/** The flat line icons of the v2 interior (docs/redesign-v2.md section 4): one 1.5 dp stroke, no fills, no gloss. */
enum class LineIcon { Qr, Search, Usb, Camera, Settings, Plug, Chevron, Monitor, Wifi, Preview, Torch, Focus }

@Composable
fun LineIconView(icon: LineIcon, size: Dp = 24.dp, color: Color = V2.Blue, modifier: Modifier = Modifier) {
    Canvas(modifier.size(size)) { drawLineIcon(icon, color) }
}

/** Draws [icon] inside the current size on a 24-unit grid. */
fun DrawScope.drawLineIcon(icon: LineIcon, color: Color, strokeDp: Float = 1.5f) {
    val u = size.minDimension / 24f
    val w = strokeDp.dp.toPx().coerceAtLeast(1f)
    val stroke = Stroke(w, cap = StrokeCap.Round, join = StrokeJoin.Round)
    fun line(x1: Float, y1: Float, x2: Float, y2: Float) =
        drawLine(color, Offset(x1 * u, y1 * u), Offset(x2 * u, y2 * u), w, StrokeCap.Round)
    fun box(x: Float, y: Float, bw: Float, bh: Float, r: Float = 2f) =
        drawRoundRect(color, Offset(x * u, y * u), Size(bw * u, bh * u), androidx.compose.ui.geometry.CornerRadius(r * u), style = stroke)
    fun arc(cx: Float, cy: Float, radius: Float, start: Float, sweep: Float) = drawArc(
        color, start, sweep, useCenter = false,
        topLeft = Offset((cx - radius) * u, (cy - radius) * u),
        size = Size(radius * 2 * u, radius * 2 * u), style = stroke,
    )
    when (icon) {
        LineIcon.Qr -> {
            box(3f, 3f, 7f, 7f, 1.5f); box(14f, 3f, 7f, 7f, 1.5f); box(3f, 14f, 7f, 7f, 1.5f)
            line(14f, 14f, 17f, 14f); line(14f, 14f, 14f, 17f); line(20f, 17f, 20f, 20f); line(17f, 20f, 20f, 20f)
            line(17f, 17f, 17f, 17.01f)
        }
        LineIcon.Search -> {
            drawCircle(color, 6.5f * u, Offset(10.5f * u, 10.5f * u), style = stroke)
            line(15.2f, 15.2f, 20f, 20f)
        }
        LineIcon.Usb -> {
            line(12f, 21f, 12f, 7f)
            drawCircle(color, 1.6f * u, Offset(12f * u, 4.6f * u), style = stroke)
            line(12f, 13f, 7.5f, 10.5f); line(7.5f, 10.5f, 7.5f, 7.5f)
            line(12f, 16.5f, 16.5f, 14f); line(16.5f, 14f, 16.5f, 11f)
            box(6.3f, 6.2f, 2.4f, 2.4f, 0.4f); box(15.3f, 9.7f, 2.4f, 2.4f, 1.2f)
        }
        LineIcon.Camera -> {
            box(2.5f, 6.5f, 19f, 13f, 2.5f)
            line(8f, 6.5f, 9.8f, 4f); line(9.8f, 4f, 14.2f, 4f); line(14.2f, 4f, 16f, 6.5f)
            drawCircle(color, 4f * u, Offset(12f * u, 13f * u), style = stroke)
        }
        LineIcon.Settings -> {
            drawCircle(color, 3f * u, Offset(12f * u, 12f * u), style = stroke)
            for (i in 0 until 8) {
                val a = Math.toRadians(i * 45.0)
                val sx = 12 + 5.2f * Math.cos(a).toFloat()
                val sy = 12 + 5.2f * Math.sin(a).toFloat()
                val ex = 12 + 8.4f * Math.cos(a).toFloat()
                val ey = 12 + 8.4f * Math.sin(a).toFloat()
                line(sx, sy, ex, ey)
            }
        }
        LineIcon.Plug -> {
            // Two chain links meeting in the middle: the "connect" mark. Each is a "C" whose gap faces
            // the other, with a short diagonal joining them.
            arc(9.5f, 14.5f, 4.5f, 315f, 270f) // Lower-left link, open towards the upper right.
            arc(14.5f, 9.5f, 4.5f, 135f, 270f) // Upper-right link, open towards the lower left.
            line(10.8f, 13.2f, 13.2f, 10.8f)
        }
        LineIcon.Chevron -> {
            line(9.5f, 7f, 15f, 12f); line(15f, 12f, 9.5f, 17f)
        }
        LineIcon.Monitor -> {
            box(2.5f, 4f, 19f, 13f, 2f); line(8f, 20.5f, 16f, 20.5f); line(12f, 17f, 12f, 20.5f)
        }
        LineIcon.Wifi -> {
            arc(12f, 15f, 9.5f, 215f, 110f); arc(12f, 15f, 6f, 210f, 120f); arc(12f, 15f, 2.8f, 205f, 130f)
            drawCircle(color, 1.1f * u, Offset(12f * u, 16.5f * u))
        }
        LineIcon.Preview -> {
            // An eye-less "view" mark: a framed picture with hills, matching the Snap logo's lens.
            box(2.5f, 4.5f, 19f, 15f, 2.5f)
            val p = Path().apply {
                moveTo(4.5f * u, 16f * u)
                cubicTo(8f * u, 10.5f * u, 10f * u, 14f * u, 12.5f * u, 12f * u)
                cubicTo(15f * u, 10f * u, 17f * u, 15f * u, 19.5f * u, 13f * u)
            }
            drawPath(p, color, style = stroke)
        }
        LineIcon.Torch -> {
            line(12f, 3f, 12f, 7f); line(12f, 17f, 12f, 21f)
            box(9f, 7f, 6f, 10f, 1.5f); line(9f, 11f, 15f, 11f)
        }
        LineIcon.Focus -> {
            drawCircle(color, 3.2f * u, Offset(12f * u, 12f * u), style = stroke)
            line(3f, 7f, 3f, 4f); line(3f, 4f, 6f, 4f); line(21f, 7f, 21f, 4f); line(21f, 4f, 18f, 4f)
            line(3f, 17f, 3f, 20f); line(3f, 20f, 6f, 20f); line(21f, 17f, 21f, 20f); line(21f, 20f, 18f, 20f)
        }
    }
}

/** The bounds helper the radar and the monitor illustration share. */
internal fun DrawScope.squareIn(): Rect {
    val s = size.minDimension
    return Rect(Offset((size.width - s) / 2f, (size.height - s) / 2f), Size(s, s))
}
