package io.github.whoissaaif.mycam

import android.content.Context
import androidx.core.content.edit
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.xmlpull.v1.XmlPullParser

/** Pairing keys live in their own preferences file, which backups and phone-to-phone transfers leave out. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class PairedPcsTest {
    private val app = RuntimeEnvironment.getApplication()
    private val main = app.getSharedPreferences(WebcamService.PREFS, Context.MODE_PRIVATE)
    private val pcId = ByteArray(16) { 7 }
    private val key = ByteArray(32) { 9 }

    @Test fun upgradeMovesKeysOutOfTheBackedUpFile() {
        // As 1.5.0 stored them: in the main file, next to the settings.
        PairedPcs(main).apply { put(pcId, key, "DESKTOP-ABC"); allowCamera(pcId) }
        val phoneId = PairedPcs(main).phoneId()
        main.edit(commit = true) { putBoolean(WebcamService.PREF_PREVIEW, false) }

        val pcs = PairedPcs.open(app)

        assertArrayEquals(key, pcs.key(pcId))
        assertTrue(pcs.cameraAllowed(pcId))
        assertEquals(listOf("DESKTOP-ABC"), pcs.names())
        assertArrayEquals(phoneId, pcs.phoneId())
        assertEquals(setOf(WebcamService.PREF_PREVIEW), main.all.keys) // Settings stay; no secret is left.
    }

    @Test fun newPairingsNeverTouchTheMainFile() {
        PairedPcs.open(app).apply { put(pcId, key, "DESKTOP-ABC"); allowCamera(pcId); phoneId() }
        assertTrue(main.all.isEmpty())
    }

    @Test fun backupRulesExcludeThePairingFile() {
        for (rules in listOf(R.xml.backup_rules, R.xml.data_extraction_rules)) {
            val excluded = mutableListOf<String>()
            val xml = app.resources.getXml(rules)
            while (xml.next() != XmlPullParser.END_DOCUMENT) {
                if (xml.eventType == XmlPullParser.START_TAG && xml.name == "exclude") {
                    assertEquals("sharedpref", xml.getAttributeValue(null, "domain"))
                    excluded += xml.getAttributeValue(null, "path")
                }
            }
            assertFalse(excluded.isEmpty())
            assertTrue(excluded.all { it == PairedPcs.PREFS + ".xml" })
        }
    }
}
