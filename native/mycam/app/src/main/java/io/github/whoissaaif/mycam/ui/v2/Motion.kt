package io.github.whoissaaif.mycam.ui.v2

import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.Easing
import androidx.compose.animation.core.FiniteAnimationSpec
import androidx.compose.animation.core.tween
import androidx.compose.runtime.Composable
import androidx.compose.runtime.ReadOnlyComposable
import io.github.whoissaaif.mycam.ui.theme.LocalReducedMotion

/**
 * One timing system for the whole phone app (docs/redesign-v2.md section 12): three durations on one ease-out
 * curve, nothing longer than 300 ms, and everything instant under reduced motion. Only opacity and
 * transform are ever animated, so a transition can be interrupted and retarget from where it is.
 */
object Motion {
    /** Short: toggles' thumb, dropdown open, pressed-state release. */
    const val SHORT = 120

    /** Standard: crossfades, preview fade, status change. */
    const val STANDARD = 200

    /** A page or tab change. */
    const val PAGE = 250

    /** A dialog rising with its backdrop. */
    const val DIALOG = 220

    /** A toggle switch. */
    const val TOGGLE = 150

    /** Splash to Connect: the longest motion in the app. */
    const val SPLASH = 300

    /** The shared ease-out curve. Fast out of the gate, settling with no overshoot. */
    val EaseOut: Easing = CubicBezierEasing(0.2f, 0f, 0f, 1f)

    /** How far content slides on a tab change. */
    const val SLIDE_DP = 16

    /** How far the splash content lifts as it leaves. */
    const val LIFT_DP = 12
}

/** [ms] on the shared curve, or instant when reduced motion is on. */
@Composable
@ReadOnlyComposable
fun <T> ease(ms: Int): FiniteAnimationSpec<T> =
    tween(if (LocalReducedMotion.current) 0 else ms, easing = Motion.EaseOut)

/** [ms], or 0 when reduced motion is on. Same helper as the XP kit's, for the v2 components. */
@Composable
@ReadOnlyComposable
fun ms(ms: Int): Int = if (LocalReducedMotion.current) 0 else ms
