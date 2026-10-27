package io.github.whoissaaif.mycam.ui

import android.os.Build
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.SizeTransform
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.blur
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.PairedPc
import io.github.whoissaaif.mycam.PhonePreview
import io.github.whoissaaif.mycam.Protocol
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.WebcamService
import io.github.whoissaaif.mycam.ui.theme.MycamTheme
import io.github.whoissaaif.mycam.ui.theme.V2
import io.github.whoissaaif.mycam.ui.v2.BottomNav
import io.github.whoissaaif.mycam.ui.v2.LineIcon
import io.github.whoissaaif.mycam.ui.v2.Motion
import io.github.whoissaaif.mycam.ui.v2.ms
import io.github.whoissaaif.mycam.ui.xp.BadgeKind
import io.github.whoissaaif.mycam.ui.xp.BottomBarItem
import io.github.whoissaaif.mycam.ui.xp.GlossyBadge
import io.github.whoissaaif.mycam.ui.xp.TitleBarHeader
import io.github.whoissaaif.mycam.ui.xp.motionMs
import kotlinx.coroutines.delay

/** The PHONE_CAMERA repository used by the Windows link and the About section. */
const val RELEASES_URL = "https://github.com/andrewaltair/PHONE_CAMERA"

/** The original MIT-licensed MyCam source credited in About. */
const val SOURCE_URL = "https://github.com/whoissaaif/mycam"

/** What the main screen can ask the activity to do. Defaults are no-ops (previews). */
data class ScreenActions(
    val onFacing: (Int) -> Unit = {},
    val onPause: (Boolean) -> Unit = {},
    val onCommand: (Int, Int) -> Unit = { _, _ -> },
    val onDim: () -> Unit = {},
    val onWireless: (Boolean) -> Unit = {},
    val onAnswerPc: (Boolean) -> Unit = {},
    val onForgetPc: (String) -> Unit = {},
    val onForgetAllPcs: () -> Unit = {},
    val onAutoDim: (Boolean) -> Unit = {},
    val onDismissTimedOut: () -> Unit = {},
    val onScan: () -> Unit = {},
    /** Asks a PC from the nearby list to connect to this phone (the pairing then starts on both). */
    val onConnectPc: (String) -> Unit = {},
    val onPreview: (Boolean) -> Unit = {},
    /** The scanner found a MyCam QR code (its text): pair with that PC and connect. */
    val onQrScanned: (String) -> Unit = {},
    val onDismissQr: () -> Unit = {},
    val onDismissPaired: () -> Unit = {},
    /** "Let this PC use the camera?": true allows it, false forgets the PC and drops the link. */
    val onAnswerCameraGate: (Boolean) -> Unit = {},
)

internal data class StatusView(val icon: Int, val headline: String, val detail: String, val live: Boolean = false)

@Composable
internal fun describe(state: WebcamService.UiState): StatusView = when {
    state.paused -> StatusView(R.drawable.status_paused, stringResource(R.string.status_paused), stringResource(R.string.status_paused_detail))
    state.error != null -> StatusView(R.drawable.status_error, stringResource(R.string.status_error), state.error)
    state.streaming -> StatusView(
        R.drawable.status_streaming, stringResource(R.string.status_streaming),
        stringResource(if (state.facing == Protocol.FACING_FRONT) R.string.camera_front else R.string.camera_back) +
            " · " + state.resolution + if (state.wireless) " · Wi-Fi" else " · USB",
        live = true,
    )
    state.connected && state.wireless -> StatusView(
        R.drawable.status_ready, stringResource(R.string.status_idle),
        stringResource(R.string.wireless_connected, state.wirelessPc ?: ""),
    )
    state.connected -> StatusView(R.drawable.status_ready, stringResource(R.string.status_idle), stringResource(R.string.status_idle_detail))
    state.wirelessOn -> StatusView(
        R.drawable.status_disconnected, stringResource(R.string.status_wireless_waiting),
        stringResource(R.string.status_wireless_waiting_detail),
    )
    else -> StatusView(R.drawable.status_disconnected, stringResource(R.string.status_disconnected), stringResource(R.string.status_disconnected_detail))
}

/** A quality / fps / camera change on its way: the marquee runs until the phone reports [done] (or 4 s). */
private class Applying(val startInfo: Protocol.CameraInfo?, val done: (WebcamService.UiState) -> Boolean)

const val TAB_CONNECT = 0
const val TAB_CAMERA = 1
const val TAB_SETTINGS = 2

/**
 * PHONE_CAMERA's main screen: the XP Luna title bar around the v2 flat interior (docs/redesign-v2.md sections 3 and 4),
 * with a three-item bottom navigation — Connect, Camera, Settings.
 *
 * Changing tab is a shared-axis horizontal slide following the tab order (section 12): the outgoing page
 * leaves 16 dp and fades, the incoming one arrives from 16 dp, 250 ms, instant under reduced motion. Find
 * Devices opens the scan page to the right of Connect, so Back comes straight back.
 */
@Composable
fun WebcamScreen(
    state: WebcamService.UiState,
    modifier: Modifier = Modifier,
    actions: ScreenActions = ScreenActions(),
    autoDim: Boolean = true,
    previewOn: Boolean = true,
    initialTab: Int = TAB_CONNECT,
    initialScan: Boolean = false,
) {
    var tab by rememberSaveable { mutableStateOf(initialTab) }
    var scanOpen by rememberSaveable { mutableStateOf(initialScan) }
    var qrOpen by rememberSaveable { mutableStateOf(false) }
    // The scanner needs the camera: it closes if a stream starts meanwhile.
    LaunchedEffect(state.streaming) { if (state.streaming) qrOpen = false }
    // Just paired: the Camera tab, behind "Paired with ..." (the PC is connected, so that's where to be).
    LaunchedEffect(state.justPaired, state.cameraGate) {
        if (state.justPaired != null || state.cameraGate != null) {
            qrOpen = false
            scanOpen = false
            tab = TAB_CAMERA
        }
    }
    var showLicences by remember { mutableStateOf(false) }

    // Applying feedback: only while the camera runs, since that's when a change restarts it.
    var applying by remember { mutableStateOf<Applying?>(null) }
    LaunchedEffect(applying) {
        if (applying != null) {
            delay(4_000)
            applying = null
        }
    }
    LaunchedEffect(state, applying) {
        val a = applying ?: return@LaunchedEffect
        if (state.cameraInfo !== a.startInfo && a.done(state)) applying = null
    }
    val live = state.streaming && !state.paused
    // The LIVE pill's gloss sweep plays once per stream, not again on every tab switch.
    var liveSwept by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(state.streaming) { if (!state.streaming) liveSwept = false }
    val onCommand: (Int, Int) -> Unit = { cmd, arg ->
        if (live && cmd == Protocol.CMD_SET_QUALITY && arg != state.camera.quality) {
            applying = Applying(state.cameraInfo) { it.cameraInfo?.quality == arg }
        } else if (live && cmd == Protocol.CMD_SET_FPS && arg != state.camera.fps) {
            applying = Applying(state.cameraInfo) { it.cameraInfo?.fps == arg }
        }
        actions.onCommand(cmd, arg)
    }
    val onFacing: (Int) -> Unit = { f ->
        if (live && f != state.facing) applying = Applying(state.cameraInfo) { it.facing == f }
        actions.onFacing(f)
    }

    val pairing = state.pendingPc != null || state.pairingTimedOut != null
    val modal = pairing || showLicences || state.justPaired != null || state.cameraGate != null
    val blur by animateDpAsState(if (modal) 16.dp else 0.dp, tween(motionMs(Motion.DIALOG)), label = "backdropBlur")

    BackHandler(enabled = qrOpen) { qrOpen = false }
    BackHandler(enabled = (scanOpen || tab != TAB_CONNECT) && !modal && !qrOpen) {
        if (scanOpen) scanOpen = false else tab = TAB_CONNECT
    }

    // The preview only runs on the Camera tab (and only while the app is showing; the activity owns that).
    DisposableEffect(tab, scanOpen) {
        PhonePreview.setPreviewPage(tab == TAB_CAMERA && !scanOpen)
        onDispose { PhonePreview.setPreviewPage(false) }
    }

    val onFindDevices = {
        scanOpen = true
        actions.onScan()
    }

    val wifiLabel = stringResource(
        when {
            state.connected && state.wireless -> R.string.wifi_status_connected
            state.wirelessOn -> R.string.wifi_status_on
            else -> R.string.wifi_status_off
        }
    )
    // The title-bar actions: Dim (while live) and the Wi-Fi state, which opens Settings.
    val barItems: @Composable RowScope.() -> Unit = {
        if (live) {
            BottomBarItem(
                stringResource(R.string.action_dim), onClick = actions.onDim, onTitleBar = true,
                icon = { GlossyBadge(BadgeKind.Dim, 24.dp) },
            )
        }
        BottomBarItem(wifiLabel, onClick = { scanOpen = false; tab = TAB_SETTINGS }, onTitleBar = true)
    }

    val sideInsets = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
    val slide = with(LocalDensity.current) { Motion.SLIDE_DP.dp.roundToPx() }
    val pageMs = ms(Motion.PAGE)
    BoxWithConstraints(modifier.fillMaxSize()) {
        // A phone in landscape: a compact caption, so the content gets the height.
        val compactCaption = maxHeight < 480.dp
        Column(
            Modifier
                .fillMaxSize()
                .background(V2.Page)
                .then(if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && blur > 0.dp) Modifier.blur(blur) else Modifier)
                .then(if (modal) Modifier.clearAndSetSemantics { } else Modifier),
        ) {
            TitleBarHeader(
                stringResource(R.string.app_name), R.drawable.app_logo,
                compact = compactCaption, trailing = { barItems() },
            )
            // Pages are keyed in tab order, with the scan page just to the right of Connect, so the slide
            // always runs the way the user moved.
            val page = if (scanOpen) 1 else if (tab == TAB_CONNECT) 0 else tab + 1
            AnimatedContent(
                targetState = page,
                modifier = Modifier.weight(1f).fillMaxWidth(),
                transitionSpec = {
                    val forward = targetState > initialState
                    val fade = tween<Float>(pageMs, easing = Motion.EaseOut)
                    val move = tween<IntOffset>(pageMs, easing = Motion.EaseOut)
                    (slideInHorizontally(move) { if (forward) slide else -slide } + fadeIn(fade)) togetherWith
                        (slideOutHorizontally(move) { if (forward) -slide else slide } + fadeOut(fade)) using
                        SizeTransform(clip = false)
                },
                label = "page",
            ) { p ->
                Column(
                    Modifier
                        .fillMaxSize()
                        .windowInsetsPadding(sideInsets)
                        .verticalScroll(rememberScrollState())
                        .padding(16.dp),
                ) {
                    when (p) {
                        0 -> ConnectPage(
                            state, actions, applying != null, onFindDevices,
                            sweepLive = !liveSwept, onLiveSwept = { liveSwept = true },
                            onScanQr = { qrOpen = true },
                        )
                        1 -> ScanPage(state, actions.onScan, actions.onConnectPc, onBack = { scanOpen = false })
                        2 -> CameraPage(state, previewOn, actions.onPreview, onFacing)
                        else -> SettingsPage(
                            state, applying != null, actions, onCommand, autoDim, previewOn, actions.onPreview,
                            onFindDevices = onFindDevices, onLicences = { showLicences = true },
                        )
                    }
                    Spacer(Modifier.padding(bottom = 8.dp))
                }
            }
            BottomNav(
                labels = listOf(
                    stringResource(R.string.nav_connect), stringResource(R.string.nav_camera), stringResource(R.string.nav_settings),
                ),
                icons = listOf(LineIcon.Plug, LineIcon.Camera, LineIcon.Settings),
                selected = tab,
                onSelect = { scanOpen = false; tab = it },
                modifier = Modifier.windowInsetsPadding(WindowInsets.navigationBars.only(WindowInsetsSides.Bottom)),
            )
        }

        PairingDialog(
            pcName = state.pendingPc ?: state.pairingTimedOut,
            code = state.pendingCode,
            deadline = state.pendingDeadline,
            timedOut = state.pendingPc == null && state.pairingTimedOut != null,
            onAnswer = actions.onAnswerPc,
            onDismissTimedOut = actions.onDismissTimedOut,
        )
        LicenceDialog(visible = showLicences && !pairing, onClose = { showLicences = false })
        PairedDialog(pcName = state.justPaired.takeIf { !pairing }, onClose = actions.onDismissPaired)
        CameraGateDialog(
            pcName = state.cameraGate.takeIf { !pairing },
            onAllow = { actions.onAnswerCameraGate(true) },
            onDeny = { actions.onAnswerCameraGate(false) },
        )
        // Over everything: the scanner is a full-screen camera with its own Close.
        if (qrOpen) {
            QrScanner(
                onFound = { text ->
                    qrOpen = false
                    tab = TAB_CONNECT
                    scanOpen = false
                    actions.onQrScanned(text)
                },
                onClose = { qrOpen = false },
            )
        }
    }
}

// --- Previews ----------------------------------------------------------------------------------------

private val previewInfo = Protocol.CameraInfo(
    quality = Protocol.QUALITY_1080P, fps = 30, zoomX100 = 100, zoomMinX100 = 60, zoomMaxX100 = 1000,
    ev = 1, evMin = -12, evMax = 12, evStepX100 = 33,
    flags = Protocol.CAM_TORCH_AVAILABLE or Protocol.CAM_HAS_AUTOFOCUS or Protocol.CAM_HAS_4K,
    width = 1920, height = 1080, actualFps = 30,
    fpsModes = listOf(Protocol.FPS_30 or Protocol.FPS_60 or Protocol.FPS_120, Protocol.FPS_30 or Protocol.FPS_60, Protocol.FPS_30),
)

@Preview(showBackground = true, widthDp = 360, heightDp = 780)
@Composable
private fun StreamingPreview() {
    MycamTheme(reducedMotion = true) {
        WebcamScreen(WebcamService.UiState(connected = true, streaming = true, resolution = "1920×1080", cameraInfo = previewInfo))
    }
}

@Preview(showBackground = true, widthDp = 360, heightDp = 780)
@Composable
private fun CameraPreviewTab() {
    MycamTheme(reducedMotion = true) {
        WebcamScreen(
            WebcamService.UiState(connected = true, streaming = true, resolution = "1920×1080", cameraInfo = previewInfo),
            initialTab = TAB_CAMERA,
        )
    }
}

@Preview(showBackground = true, widthDp = 360, heightDp = 780)
@Composable
private fun DisconnectedPreview() {
    MycamTheme(reducedMotion = true) { WebcamScreen(WebcamService.UiState()) }
}

@Preview(showBackground = true, widthDp = 360, heightDp = 1200)
@Composable
private fun SettingsPreview() {
    MycamTheme(reducedMotion = true) {
        WebcamScreen(
            WebcamService.UiState(
                connected = true, streaming = true, resolution = "1920×1080", cameraInfo = previewInfo,
                camera = io.github.whoissaaif.mycam.CameraStreamer.Settings(quality = Protocol.QUALITY_4K),
                wirelessOn = true, wirelessAddress = "192.168.1.23",
                pairedPcs = listOf(PairedPc("a1", "DESKTOP-ABC"), PairedPc("b2", "LAPTOP-XYZ")),
            ),
            initialTab = TAB_SETTINGS,
        )
    }
}

@Preview(showBackground = true, widthDp = 360, heightDp = 780)
@Composable
private fun PairingPreview() {
    MycamTheme(reducedMotion = true) {
        WebcamScreen(WebcamService.UiState(wirelessOn = true, pendingPc = "DESKTOP-ABC", pendingCode = "554294"))
    }
}
