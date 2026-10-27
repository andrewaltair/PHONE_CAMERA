package io.github.whoissaaif.mycam.ui

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.ui.theme.MycamTheme
import io.github.whoissaaif.mycam.ui.theme.Xp
import io.github.whoissaaif.mycam.ui.v2.PrimaryButton
import io.github.whoissaaif.mycam.ui.xp.SkyTextShadow
import android.content.SharedPreferences
import androidx.core.content.edit
import io.github.whoissaaif.mycam.PairedPcs
import io.github.whoissaaif.mycam.WebcamService
import io.github.whoissaaif.mycam.ui.xp.XpLink
import io.github.whoissaaif.mycam.ui.xp.XpProgressBar
import io.github.whoissaaif.mycam.ui.xp.motionMs

private const val STEPS = 3

/**
 * The first-run cards are shown once, to a new user. Skip, Got it on the last card and the first connection
 * all end them for good ([done]). A phone that has paired a PC has connected before (an upgrade from a
 * version without the flag), so it never sees them either.
 */
object FirstRun {
    fun shouldShow(prefs: SharedPreferences, pairedPcs: PairedPcs): Boolean =
        !prefs.getBoolean(WebcamService.PREF_FIRST_RUN_DONE, false) && pairedPcs.list().isEmpty()

    fun done(prefs: SharedPreferences) {
        if (!prefs.getBoolean(WebcamService.PREF_FIRST_RUN_DONE, false)) {
            prefs.edit { putBoolean(WebcamService.PREF_FIRST_RUN_DONE, true) }
        }
    }
}

/**
 * First run (A9, docs/redesign.md 5.7): three cards over the sunny hills, shown once (see [FirstRun]).
 * [onDone] hides them (Skip, or Got it on the last card).
 */
@Composable
fun FirstRunScreen(onDone: () -> Unit, modifier: Modifier = Modifier) {
    var step by rememberSaveable { mutableIntStateOf(0) }
    BackHandler(enabled = step > 0) { step-- }
    val uri = LocalUriHandler.current
    Box(modifier.fillMaxSize()) {
        Image(painterResource(R.drawable.bg_hills), contentDescription = null, modifier = Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
        Column(
            Modifier
                .fillMaxSize()
                .windowInsetsPadding(WindowInsets.safeDrawing)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Image(painterResource(R.drawable.app_logo), contentDescription = null, modifier = Modifier.size(40.dp))
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.app_name), style = MaterialTheme.typography.titleLarge.copy(shadow = SkyTextShadow), color = Color.White, modifier = Modifier.weight(1f))
                XpLink(stringResource(R.string.first_skip), onClick = onDone, color = Color.White)
            }
            Spacer(Modifier.height(32.dp))
            val shape = RoundedCornerShape(8.dp)
            Column(
                Modifier
                    .widthIn(max = 480.dp)
                    .fillMaxWidth()
                    .clip(shape)
                    .background(Xp.Card.copy(alpha = 0.94f))
                    .drawBehind { drawRoundRect(Xp.Bevel, cornerRadius = CornerRadius(8.dp.toPx()), style = Stroke(1.dp.toPx())) }
                    .padding(20.dp),
            ) {
                // Step indicator: three XP progress chunks, filled as you go.
                XpProgressBar((step + 1f) / STEPS, Modifier.width(120.dp), segments = STEPS)
                Spacer(Modifier.height(4.dp))
                Text(stringResource(R.string.first_step, step + 1, STEPS), style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle)
                Spacer(Modifier.height(12.dp))
                val fade = motionMs(150)
                AnimatedContent(
                    step, transitionSpec = { fadeIn(tween(fade)) togetherWith fadeOut(tween(fade)) },
                    label = "firstRunStep",
                ) { s ->
                    Column {
                        val (title, text) = when (s) {
                            0 -> R.string.first_1_title to R.string.first_1_text
                            1 -> R.string.first_2_title to R.string.first_2_text
                            else -> R.string.first_3_title to R.string.first_3_text
                        }
                        Text(stringResource(title), style = MaterialTheme.typography.headlineSmall, color = Xp.Text, modifier = Modifier.semantics { heading() })
                        Spacer(Modifier.height(8.dp))
                        Text(stringResource(text), style = MaterialTheme.typography.bodyLarge, color = Xp.Text)
                        if (s == 0) {
                            XpLink(stringResource(R.string.about_get_windows), onClick = { try { uri.openUri(RELEASES_URL) } catch (_: Exception) {} })
                        }
                    }
                }
                Spacer(Modifier.height(16.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    PrimaryButton(
                        stringResource(if (step < STEPS - 1) R.string.first_next else R.string.first_done),
                        onClick = { if (step < STEPS - 1) step++ else onDone() },
                    )
                }
            }
        }
    }
}

@Preview(widthDp = 360, heightDp = 720)
@Composable
private fun FirstRunPreview() {
    MycamTheme(reducedMotion = true) { FirstRunScreen(onDone = {}) }
}
