package io.github.whoissaaif.mycam.ui.theme

import android.content.Context
import android.provider.Settings
import android.view.accessibility.AccessibilityManager
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.platform.LocalContext

// Windows XP had no dark mode, so MyCam uses one light Luna scheme everywhere (no dynamic colour).
private val LunaColorScheme = lightColorScheme(
    primary = Xp.Selection,
    onPrimary = Xp.Card,
    secondary = Xp.Link,
    background = Xp.Surface,
    onBackground = Xp.Text,
    surface = Xp.Card,
    onSurface = Xp.Text,
    onSurfaceVariant = Xp.Subtle,
    outline = Xp.ButtonBorder,
)

/**
 * True when transitions should be instant (docs/redesign.md section 6.3): the system animator scale is 0, or
 * TalkBack's touch exploration is on. The marquee is then replaced by a static "Working…" label.
 */
val LocalReducedMotion = staticCompositionLocalOf { false }

@Composable
private fun rememberReducedMotion(): Boolean {
    val context = LocalContext.current
    var touchExploration by remember { mutableStateOf(false) }
    DisposableEffect(context) {
        val am = context.getSystemService(Context.ACCESSIBILITY_SERVICE) as? AccessibilityManager
        val listener = AccessibilityManager.TouchExplorationStateChangeListener { touchExploration = it }
        if (am != null) {
            touchExploration = am.isTouchExplorationEnabled
            am.addTouchExplorationStateChangeListener(listener)
        }
        onDispose { am?.removeTouchExplorationStateChangeListener(listener) }
    }
    val animatorOff = remember(context) {
        try {
            Settings.Global.getFloat(context.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f
        } catch (_: Exception) {
            false // Previews and odd ROMs.
        }
    }
    return animatorOff || touchExploration
}

@Composable
fun MycamTheme(reducedMotion: Boolean? = null, content: @Composable () -> Unit) {
    val reduced = reducedMotion ?: rememberReducedMotion()
    CompositionLocalProvider(LocalReducedMotion provides reduced) {
        MaterialTheme(colorScheme = LunaColorScheme, typography = Typography, content = content)
    }
}
