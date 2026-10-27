package io.github.whoissaaif.mycam

import io.github.whoissaaif.mycam.ui.FpsReason
import org.junit.Assert.assertEquals
import org.junit.Test

/** The per-state frame-rate reasons (A10) come only from the reported masks. */
class FpsReasonTest {

    private fun info(vararg masks: Int) = Protocol.CameraInfo(
        quality = 0, fps = 30, zoomX100 = 100, zoomMinX100 = 100, zoomMaxX100 = 800,
        ev = 0, evMin = -4, evMax = 4, evStepX100 = 50, flags = 0, width = 1920, height = 1080, actualFps = 30,
        fpsModes = masks.toList(),
    )

    private val f30 = Protocol.FPS_30
    private val f60 = Protocol.FPS_30 or Protocol.FPS_60
    private val f120 = Protocol.FPS_30 or Protocol.FPS_60 or Protocol.FPS_120

    @Test fun unknownBeforeTheCameraReports() {
        assertEquals(listOf(FpsReason.Unknown), FpsReason.compute(null, Protocol.QUALITY_1080P))
    }

    @Test fun only30Everywhere() {
        assertEquals(listOf(FpsReason.Only30), FpsReason.compute(info(f30, f30, f30), Protocol.QUALITY_720P))
    }

    @Test fun sixtyNeeds1080pOrLowerAt4K() {
        assertEquals(
            listOf(FpsReason.NeedsOrLower(60, Protocol.QUALITY_1080P), FpsReason.NeedsOrLower(120, Protocol.QUALITY_1080P)),
            FpsReason.compute(info(f120, f120, f30), Protocol.QUALITY_4K),
        )
    }

    @Test fun oneTwentyNeeds720p() {
        assertEquals(
            listOf(FpsReason.NeedsOrLower(120, Protocol.QUALITY_720P)),
            FpsReason.compute(info(f120, f60, f30), Protocol.QUALITY_1080P),
        )
    }

    @Test fun oddMasksNameTheQualities() {
        assertEquals(
            listOf(FpsReason.Needs(60, listOf(Protocol.QUALITY_1080P))),
            FpsReason.compute(info(f30, f60, f30), Protocol.QUALITY_720P),
        )
    }

    @Test fun nothingGreyedSaysUpTo() {
        assertEquals(listOf(FpsReason.UpTo(120)), FpsReason.compute(info(f120, f120, f30), Protocol.QUALITY_720P))
        assertEquals(listOf(FpsReason.UpTo(60)), FpsReason.compute(info(f60, f60, f60), Protocol.QUALITY_4K))
    }

    @Test fun missing4KIsIgnored() {
        assertEquals(
            listOf(FpsReason.UpTo(60)),
            FpsReason.compute(info(f60, f60, f120), Protocol.QUALITY_1080P) { it != Protocol.QUALITY_4K },
        )
    }
}
