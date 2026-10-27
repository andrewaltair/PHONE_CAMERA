package io.github.whoissaaif.mycam.snapshots

import android.graphics.Bitmap
import android.os.SystemClock
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.activity.ComponentActivity
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.CameraStreamer
import io.github.whoissaaif.mycam.NearbyPc
import io.github.whoissaaif.mycam.PairedPc
import io.github.whoissaaif.mycam.Protocol
import io.github.whoissaaif.mycam.WebcamService.UiState
import io.github.whoissaaif.mycam.ui.DimScreen
import io.github.whoissaaif.mycam.ui.FirstRunScreen
import io.github.whoissaaif.mycam.ui.WebcamScreen
import io.github.whoissaaif.mycam.ui.theme.MycamTheme
import io.github.whoissaaif.mycam.ui.theme.Xp
import io.github.whoissaaif.mycam.ui.xp.XpProgressBar
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File

/**
 * Renders the phone screens to PNGs for visual review (no device needed). Opt-in and record-only: run with
 * `gradlew testDebugUnitTest -Psnapshots --tests "*PhoneSnapshots*"`; the files land in app/build/snapshots.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [35], qualifiers = "w360dp-h800dp-xhdpi")
class PhoneSnapshots {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    private lateinit var outDir: File

    @Before
    fun setUp() {
        val dir = System.getProperty("mycam.snapshots")
        assumeTrue("snapshots are opt-in (-Psnapshots)", dir != null)
        outDir = File(dir!!).apply { mkdirs() }
    }

    private val info = Protocol.CameraInfo(
        quality = Protocol.QUALITY_1080P, fps = 30, zoomX100 = 100, zoomMinX100 = 60, zoomMaxX100 = 1000,
        ev = 1, evMin = -12, evMax = 12, evStepX100 = 33,
        flags = Protocol.CAM_TORCH_AVAILABLE or Protocol.CAM_HAS_AUTOFOCUS or Protocol.CAM_HAS_4K,
        width = 1920, height = 1080, actualFps = 30,
        fpsModes = listOf(Protocol.FPS_30 or Protocol.FPS_60 or Protocol.FPS_120, Protocol.FPS_30 or Protocol.FPS_60, Protocol.FPS_30),
    )
    private val streaming = UiState(connected = true, streaming = true, resolution = "1920×1080", cameraInfo = info)

    private fun shot(name: String, fontScale: Float = 1f, reduced: Boolean = true, content: @Composable () -> Unit) {
        compose.mainClock.autoAdvance = false
        compose.setContent {
            val d = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(d.density, fontScale)) {
                MycamTheme(reducedMotion = reduced, content = content)
            }
        }
        compose.mainClock.advanceTimeBy(1_000)
        val view = compose.activity.window.decorView
        val bmp = Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888)
        view.draw(android.graphics.Canvas(bmp))
        File(outDir, "$name.png").outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }


    @Test fun disconnected() = shot("01-disconnected") { WebcamScreen(UiState()) }
    @Test fun disconnectedLarge() = shot("01-disconnected-fs13", 1.3f) { WebcamScreen(UiState()) }
    @Test fun firstRun() = shot("02-first-run") { FirstRunScreen(onDone = {}) }
    @Test fun firstRunLarge() = shot("02-first-run-fs13", 1.3f) { FirstRunScreen(onDone = {}) }
    @Test fun readyUsb() = shot("03-ready-usb") { WebcamScreen(UiState(connected = true, cameraInfo = info)) }
    @Test fun streamingNow() = shot("04-streaming") { WebcamScreen(streaming) }

    @Config(qualifiers = "w360dp-h1000dp-xhdpi")
    @Test fun streamingNowLarge() = shot("04-streaming-fs13", 1.3f) { WebcamScreen(streaming) }

    @Config(qualifiers = "w800dp-h360dp-land-xhdpi")
    @Test fun streamingLandscape() = shot("05-streaming-landscape") { WebcamScreen(streaming) }

    @Config(qualifiers = "w840dp-h600dp-land-xhdpi")
    @Test fun streamingTablet() = shot("05-streaming-tablet") { WebcamScreen(streaming) }

    @Test fun paused() = shot("06-paused") { WebcamScreen(UiState(connected = true, streaming = true, paused = true, cameraInfo = info)) }
    @Test fun pausedLarge() = shot("06-paused-fs13", 1.3f) { WebcamScreen(UiState(connected = true, streaming = true, paused = true, cameraInfo = info)) }

    private val settingsState = streaming.copy(
        camera = CameraStreamer.Settings(quality = Protocol.QUALITY_4K, fps = 60),
        wirelessOn = true, wirelessAddress = "192.168.1.23",
        pairedPcs = listOf(PairedPc("a1", "DESKTOP-ABC"), PairedPc("b2", "LAPTOP-XYZ")),
    )

    @Config(qualifiers = "w360dp-h1400dp-xhdpi")
    @Test fun settings() = shot("07-settings") { WebcamScreen(settingsState, initialTab = 1) }

    @Config(qualifiers = "w360dp-h1700dp-xhdpi")
    @Test fun settingsLarge() = shot("07-settings-fs13", 1.3f) { WebcamScreen(settingsState, initialTab = 1) }

    @Config(qualifiers = "w800dp-h600dp-land-xhdpi")
    @Test fun settingsWide() = shot("07-settings-wide") { WebcamScreen(settingsState, initialTab = 1) }

    private fun pairingState() = UiState(
        wirelessOn = true, wirelessAddress = "192.168.1.23", pendingPc = "DESKTOP-ABC", pendingCode = "554294",
        pendingDeadline = SystemClock.elapsedRealtime() + 42_000,
    )

    @Test fun pairing() = shot("08-pairing") { WebcamScreen(pairingState()) }
    @Test fun pairingLarge() = shot("08-pairing-fs13", 1.3f) { WebcamScreen(pairingState()) }
    @Config(qualifiers = "w800dp-h360dp-land-xhdpi")
    @Test fun pairingLandscapeLarge() = shot("08-pairing-landscape-fs13", 1.3f) { WebcamScreen(pairingState()) }

    @Test fun pairingTimedOut() = shot("09-pairing-timed-out") {
        WebcamScreen(UiState(wirelessOn = true, wirelessAddress = "192.168.1.23", pairingTimedOut = "DESKTOP-ABC"))
    }

    @Test fun dim() = shot("10-dim") { DimScreen(streaming) }
    @Test fun dimHot() = shot("11-dim-heat") { DimScreen(streaming.copy(thermal = 4)) }
    @Test fun heatWarning() = shot("12-streaming-heat") { WebcamScreen(streaming.copy(thermal = 2)) }
    @Test fun error() = shot("13-error") { WebcamScreen(UiState(connected = true, error = "The camera is in use by another app.")) }

    // Scan for PCs (docs/redesign.md 5.4): Now while listening (with the marquee), Settings with results, Now with none.
    private val scanBase = UiState(wirelessOn = true, wirelessAddress = "192.168.1.23", pairedPcs = listOf(PairedPc("a1", "DESKTOP-ABC")))

    @Test fun scanning() = shot("15-scan-scanning", reduced = false) {
        WebcamScreen(scanBase.copy(scanning = true, nearbyPcs = listOf(NearbyPc("DESKTOP-ABC", "192.168.1.20"))))
    }
    @Test fun scanningReduced() = shot("15-scan-scanning-reduced") { WebcamScreen(scanBase.copy(scanning = true)) }

    @Config(qualifiers = "w360dp-h1400dp-xhdpi")
    @Test fun scanResults() = shot("15-scan-results") {
        WebcamScreen(
            scanBase.copy(
                connected = true, wireless = true, wirelessPc = "STUDIO", wirelessPcIp = "192.168.1.31", scanned = true,
                pairedPcs = listOf(PairedPc("a1", "DESKTOP-ABC"), PairedPc("c3", "STUDIO")),
                nearbyPcs = listOf(
                    NearbyPc("DESKTOP-ABC", "192.168.1.20"), NearbyPc("LIVING-ROOM-PC", "192.168.1.44"), NearbyPc("STUDIO", "192.168.1.31"),
                ),
            ),
            initialTab = 1,
        )
    }

    @Config(qualifiers = "w360dp-h1000dp-xhdpi")
    @Test fun scanResultsLarge() = shot("15-scan-results-fs13", 1.3f) {
        WebcamScreen(
            scanBase.copy(scanned = true, nearbyPcs = listOf(NearbyPc("DESKTOP-ABC", "192.168.1.20"), NearbyPc("LIVING-ROOM-PC", "192.168.1.44"))),
        )
    }

    @Test fun scanNone() = shot("15-scan-none") { WebcamScreen(scanBase.copy(scanned = true)) }

    @Config(qualifiers = "w360dp-h1400dp-xhdpi")
    @Test fun scanWifiOff() = shot("15-scan-wifi-off") { WebcamScreen(UiState(), initialTab = 1) }

    @Test fun progressBars() = shot("14-progress") {
        Column(Modifier.fillMaxSize().background(Xp.Surface).padding(16.dp)) {
            XpProgressBar(0.3f)
            XpProgressBar(0.7f, Modifier.padding(top = 12.dp))
            XpProgressBar(1f, Modifier.padding(top = 12.dp))
        }
    }
}

