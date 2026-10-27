package io.github.whoissaaif.mycam.ui

import io.github.whoissaaif.mycam.Protocol

/**
 * Why some frame rates are greyed out at the selected quality (A10, docs/redesign.md 5.4). Computed from the fps
 * masks the camera reported for every quality, so it stays capability-driven: nothing here knows any phone.
 */
sealed class FpsReason {
    /** The camera hasn't reported yet. */
    data object Unknown : FpsReason()

    /** No quality does more than 30 fps. */
    data object Only30 : FpsReason()

    /** [fps] works at [quality] and every quality below it, but not at the selected one. */
    data class NeedsOrLower(val fps: Int, val quality: Int) : FpsReason()

    /** [fps] works only at [qualities] (not a simple "or lower" rule). */
    data class Needs(val fps: Int, val qualities: List<Int>) : FpsReason()

    /** Nothing is greyed out because of the quality; [maxFps] is the fastest rate at this quality. */
    data class UpTo(val maxFps: Int) : FpsReason()

    companion object {
        private val RATES = listOf(60 to Protocol.FPS_60, 120 to Protocol.FPS_120)
        private val QUALITIES = listOf(Protocol.QUALITY_720P, Protocol.QUALITY_1080P, Protocol.QUALITY_4K)

        /**
         * Reasons for the frame rates the camera can do at some quality but not at [quality]. [qualityAvailable]
         * says which qualities this camera offers at all (4K may be missing).
         */
        fun compute(info: Protocol.CameraInfo?, quality: Int, qualityAvailable: (Int) -> Boolean = { true }): List<FpsReason> {
            if (info == null || info.width == 0) return listOf(Unknown)
            val offered = QUALITIES.filter(qualityAvailable)
            val union = offered.fold(0) { acc, q -> acc or info.fpsMask(q) }
            if (union and (Protocol.FPS_60 or Protocol.FPS_120) == 0) return listOf(Only30)
            val here = info.fpsMask(quality)
            val reasons = RATES.filter { (_, bit) -> union and bit != 0 && here and bit == 0 }.map { (fps, bit) ->
                val where = offered.filter { info.fpsMask(it) and bit != 0 }
                val top = where.max()
                if (top < quality && where == offered.filter { it <= top }) NeedsOrLower(fps, top) else Needs(fps, where)
            }
            if (reasons.isNotEmpty()) return reasons
            val max = RATES.lastOrNull { (_, bit) -> here and bit != 0 }?.first ?: 30
            return listOf(UpTo(max))
        }

        fun qualityName(q: Int) = when (q) {
            Protocol.QUALITY_720P -> "720p"
            Protocol.QUALITY_1080P -> "1080p"
            else -> "4K"
        }
    }
}
