package io.github.whoissaaif.mycam.ui

import androidx.compose.animation.core.Animatable
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.foundation.layout.offset
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.ui.theme.MycamTheme
import io.github.whoissaaif.mycam.ui.v2.Motion
import io.github.whoissaaif.mycam.ui.v2.PrimaryButton
import io.github.whoissaaif.mycam.ui.v2.ms
import io.github.whoissaaif.mycam.ui.xp.SkyTextShadow
import androidx.compose.animation.core.tween
import kotlinx.coroutines.delay

/**
 * The splash (docs/redesign-v2.md 3.2): the hills photo, the app mark, the name and one line, with a blue
 * Get Started. It shows on every launch for about a second, never blocks, and a tap anywhere skips it.
 *
 * [SplashGate] is also the splash → Connect transition (section 12): the splash content fades and lifts
 * 12 dp while the photo stays where it is, so the two screens read as one scene. 300 ms, instant under
 * reduced motion.
 */
@Composable
fun SplashGate(show: Boolean, onDone: () -> Unit, content: @Composable () -> Unit) {
    val duration = ms(Motion.SPLASH)
    // 0 = splash, 1 = the app. Interruptible: a tap during the hold retargets from where it is.
    val t = remember { Animatable(if (show) 0f else 1f) }
    LaunchedEffect(show) {
        if (show) {
            delay(SPLASH_HOLD_MS)
            onDone()
        } else {
            t.animateTo(1f, tween(duration, easing = Motion.EaseOut))
        }
    }
    Box(Modifier.fillMaxSize()) {
        if (t.value < 1f) {
            // The shared background: it does not move or fade during the transition.
            Image(
                painterResource(R.drawable.bg_hills), contentDescription = null,
                modifier = Modifier.fillMaxSize(), contentScale = ContentScale.Crop,
            )
            SplashContent(
                modifier = Modifier
                    .fillMaxSize()
                    .alpha(1f - t.value)
                    .offset(y = (-Motion.LIFT_DP * t.value).dp),
                onStart = onDone,
            )
        }
        if (t.value > 0f) {
            Box(Modifier.fillMaxSize().alpha(t.value)) { content() }
        }
    }
}

/** How long the splash stays before it hands over. Short enough never to feel like a wait. */
const val SPLASH_HOLD_MS = 1_000L

@Composable
private fun SplashContent(modifier: Modifier = Modifier, onStart: () -> Unit) {
    val skip = stringResource(R.string.splash_skip_description)
    Column(
        modifier
            // A tap anywhere skips the splash.
            .clickable(remember { MutableInteractionSource() }, indication = null, onClick = onStart)
            .semantics { contentDescription = skip }
            .windowInsetsPadding(WindowInsets.safeDrawing)
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Spacer(Modifier.weight(1f))
        Image(
            painterResource(R.drawable.app_logo), contentDescription = null,
            modifier = Modifier.size(128.dp),
        )
        Spacer(Modifier.height(20.dp))
        Text(
            stringResource(R.string.app_name),
            style = MaterialTheme.typography.headlineSmall.copy(shadow = SkyTextShadow),
            color = Color.White,
        )
        Spacer(Modifier.height(6.dp))
        Text(
            stringResource(R.string.splash_tagline),
            style = MaterialTheme.typography.bodyLarge.copy(shadow = SkyTextShadow),
            color = Color.White, textAlign = TextAlign.Center,
        )
        Spacer(Modifier.weight(1f))
        PrimaryButton(
            stringResource(R.string.splash_start), onClick = onStart,
            modifier = Modifier.widthIn(min = 220.dp),
        )
        Spacer(Modifier.height(24.dp))
    }
}

@Preview(widthDp = 360, heightDp = 780)
@Composable
private fun SplashPreview() {
    MycamTheme(reducedMotion = true) {
        Box(Modifier.fillMaxSize().background(Color.White)) {
            SplashGate(show = true, onDone = {}) { }
        }
    }
}
