package io.github.whoissaaif.mycam.ui.theme

import androidx.compose.material3.Typography
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import io.github.whoissaaif.mycam.R

// DejaVu Sans (Bitstream Vera licence + public-domain changes; assets/licenses/DejaVu-LICENSE.txt).
// Condensed has Tahoma-like proportions for the XP look (docs/redesign.md section 3.3).
val DejaVuCondensed = FontFamily(
    Font(R.font.dejavu_sans_condensed, FontWeight.Normal),
    Font(R.font.dejavu_sans_condensed_bold, FontWeight.Bold),
)
val DejaVuSans = FontFamily(Font(R.font.dejavu_sans_bold, FontWeight.Bold))
val DejaVuMono = FontFamily(Font(R.font.dejavu_sans_mono_bold, FontWeight.Bold))

// XP type ramp for a phone. Body text is never below 14 sp.
val Typography = Typography(
    // Main instruction (status headline, dialog questions).
    headlineSmall = TextStyle(fontFamily = DejaVuSans, fontWeight = FontWeight.Bold, fontSize = 22.sp, lineHeight = 28.sp),
    // Window title, group titles in the hero.
    titleLarge = TextStyle(fontFamily = DejaVuSans, fontWeight = FontWeight.Bold, fontSize = 18.sp, lineHeight = 24.sp),
    // Group titles, buttons.
    titleMedium = TextStyle(fontFamily = DejaVuCondensed, fontWeight = FontWeight.Bold, fontSize = 14.sp, lineHeight = 20.sp),
    labelLarge = TextStyle(fontFamily = DejaVuCondensed, fontWeight = FontWeight.Bold, fontSize = 14.sp, lineHeight = 20.sp),
    // Body.
    bodyLarge = TextStyle(fontFamily = DejaVuCondensed, fontWeight = FontWeight.Normal, fontSize = 15.sp, lineHeight = 21.sp),
    bodyMedium = TextStyle(fontFamily = DejaVuCondensed, fontWeight = FontWeight.Normal, fontSize = 14.sp, lineHeight = 20.sp),
    bodySmall = TextStyle(fontFamily = DejaVuCondensed, fontWeight = FontWeight.Normal, fontSize = 14.sp, lineHeight = 20.sp),
    // The LIVE pill.
    labelSmall = TextStyle(fontFamily = DejaVuCondensed, fontWeight = FontWeight.Bold, fontSize = 14.sp, lineHeight = 18.sp),
)

/** Pairing code: big tabular digits. */
val PairingCodeStyle = TextStyle(fontFamily = DejaVuMono, fontWeight = FontWeight.Bold, fontSize = 40.sp, lineHeight = 48.sp)
