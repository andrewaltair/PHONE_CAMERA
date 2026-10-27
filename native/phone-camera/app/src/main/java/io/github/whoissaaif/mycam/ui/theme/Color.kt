package io.github.whoissaaif.mycam.ui.theme

import androidx.compose.ui.graphics.Color

// Windows XP "Luna Blue" design tokens (docs/redesign.md section 3.2), tuned for contrast. Shared in spirit with
// the PC window (pc/companion/settings_window.cpp), so both apps look like one product.
// Inspiration only: everything is drawn from scratch, no Microsoft artwork.
object Xp {
    // Title bar (app header): gloss highlight line, then the blue caption gradient, dark edge.
    val TitleBarTop = Color(0xFF3D8AF7)
    val TitleBarGloss = Color(0xFF0A5DEB)
    val TitleBarMid = Color(0xFF0053E1)
    val TitleBarBottom = Color(0xFF0047D0)
    val TitleBarHighlight = Color(0xFF7FB6FF)
    val TitleBarEdge = Color(0xFF0831D9)
    val TitleText = Color(0xFFFFFFFF)
    val TitleShadow = Color(0xFF0A1E78)

    // Surfaces.
    val Surface = Color(0xFFECE9D8)
    val Card = Color(0xFFFFFFFF)
    val TaskPaneTop = Color(0xFF7BA2E7)
    val TaskPaneBottom = Color(0xFF6375D6)
    val Bevel = Color(0xFFACA899)

    // Task groups.
    val GroupHeaderStart = Color(0xFFFFFFFF)
    val GroupHeaderEnd = Color(0xFFC6D3F7)
    val GroupTitle = Color(0xFF215DC6)
    val GroupHeroStart = Color(0xFF0055E5)
    val GroupHeroEnd = Color(0xFF2463D6)
    // Paused hero header. Darker than the spec's #E08A00 so white text keeps 4.5 : 1 (4.7 on the light stop).
    val GroupPausedStart = Color(0xFFB05F00)
    val GroupPausedEnd = Color(0xFF8F4C00)
    val GroupBody = Color(0xFFD6DFF7)
    val GroupBorder = Color(0xFFFFFFFF)

    // Text.
    val Selection = Color(0xFF316AC5)
    val Link = Color(0xFF215DC6)
    val Text = Color(0xFF000000)
    val Subtle = Color(0xFF4D4D4D)
    val DisabledText = Color(0xFF8A877A)
    val WarningText = Color(0xFF8F5200)

    // "Go" green (XP Start button). The spec's #3FAA3F top stop is only 2.9 : 1 with white text, so the
    // light colour is kept to a thin gloss band and the text sits on #2B842B or darker (4.8 : 1 and up).
    val GoGloss = Color(0xFF5DBE5D)
    val GoTop = Color(0xFF2B842B)
    val GoBottom = Color(0xFF226B22)
    val GoBorder = Color(0xFF1D5E1D)

    // The green progress bar (section 6.1).
    val ChunkTop = Color(0xFFE2F8E2)
    val ChunkLight = Color(0xFF6FD86F)
    val ChunkMid = Color(0xFF2DB52D)
    val ChunkBottom = Color(0xFF5ACD5A)
    val ProgressTrack = Color(0xFFFFFFFF)
    val ProgressBorder = Color(0xFFACA899)

    // Badges (graphics only).
    val AmberLight = Color(0xFFFFD86A)
    val AmberDark = Color(0xFFE08A00)
    val ErrorLight = Color(0xFFFF7B6B)
    val ErrorDark = Color(0xFFC81E0F)
    val GoLight = Color(0xFF7BD87B)
    val GoDark = Color(0xFF21A121)
    val NightBadgeLight = Color(0xFF7FB6EA)
    val NightBadgeDark = Color(0xFF123E7A)

    // Push buttons.
    val ButtonTop = Color(0xFFFFFFFF)
    val ButtonMid = Color(0xFFECEBE6)
    val ButtonBottom = Color(0xFFD6D0C5)
    val ButtonBorder = Color(0xFF003C74)
    val ButtonHotLight = Color(0xFFFFCF6B)
    val ButtonHotDark = Color(0xFFE5A01A)
    val ButtonDefaultLight = Color(0xFFCEE7FF)
    val ButtonDefaultDark = Color(0xFF6982EE)
    val ButtonDisabledFill = Color(0xFFF5F4EA)
    val ButtonDisabledBorder = Color(0xFFC9C7BA)

    // Checkbox and radio.
    val CheckBorder = Color(0xFF1C5180)
    val CheckFillTop = Color(0xFFDCDCD7)
    val CheckMark = Color(0xFF21A121)

    // Property-sheet tabs.
    val TabBorder = Color(0xFF919B9C)
    val TabHotLine = Color(0xFFE68B2C)
    val TabHotLight = Color(0xFFFFC73C)
    val TabPage = Color(0xFFFCFCFE)

    // Hills (first run) and the hills at night (dim screen).
    val SkyTop = Color(0xFF3F86E8)
    val SkyHorizon = Color(0xFFBFE0FF)
    val HillBack = Color(0xFF8ACB6E)
    val HillMid = Color(0xFF5DB443)
    val HillFront = Color(0xFF3E9B2F)
    val HillFrontLight = Color(0xFF6CC24F)
    val NightTop = Color(0xFF06142B)
    val NightBottom = Color(0xFF000000)
    val NightHill = Color(0xFF0B2414)
    val NightStar = Color(0xFF9FB4D8)
    val DimText = Color(0xFF8FA6C4)
    val DimSubtle = Color(0xFF51677F)
    val HeatWarm = Color(0xFFFFC94D)
    val HeatHot = Color(0xFFFF7A6B)
}

/**
 * The v2 flat interior (docs/redesign-v2.md section 4): XP chrome on the outside, a clean modern app inside.
 * White cards on a near-white page, one blue accent, flat line icons. Every pair here is WCAG AA:
 * white on [Blue] is 4.6 : 1, [Text] on [Page] is 15 : 1, [Subtle] on [Card] is 5.9 : 1,
 * [Subtle] on [Page] is 5.6 : 1 and [Disabled] on [Card] is 4.6 : 1.
 */
object V2 {
    val Page = Color(0xFFF5F7FA)
    val Card = Color(0xFFFFFFFF)
    val CardBorder = Color(0xFFE3E8EF)
    val Shadow = Color(0x14121A2B)

    val Blue = Color(0xFF1E6FE8)
    val BluePressed = Color(0xFF1760D0)
    val BlueSoft = Color(0xFFE8F0FE)

    val Text = Color(0xFF111827)
    val Subtle = Color(0xFF5B6472)
    val Disabled = Color(0xFF767F8C)

    // Secondary (white) button and other outlined controls.
    val Border = Color(0xFFD0D7E2)
    val BorderStrong = Color(0xFF9AA5B4)
    val Pressed = Color(0xFFEDF1F7)

    // Toggle switch: the off state is a grey track with a border, never colour alone.
    val TrackOff = Color(0xFFE3E8EF)
    val TrackOffBorder = Color(0xFF9AA5B4)
    val Thumb = Color(0xFFFFFFFF)

    // The dark pills over the live preview, at about 70 % black.
    val Chip = Color(0xB3000000)
    val ChipText = Color(0xFFFFFFFF)

    // The radar on the scan screen.
    val RadarRing = Color(0xFFC9D6EA)
    val RadarSweep = Color(0x4D1E6FE8)
    val RadarDot = Color(0xFF1E6FE8)
}
