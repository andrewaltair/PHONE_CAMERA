package io.github.whoissaaif.mycam

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NearbyPcsTest {
    @Test fun newPcChangesTheList() {
        val n = NearbyPcs()
        assertTrue(n.seen("192.168.1.5", "DESKTOP-ABC", 0))
        assertEquals(listOf(NearbyPc("DESKTOP-ABC", "192.168.1.5")), n.list(0))
    }

    @Test fun repeatedProbeOnlyRefreshes() {
        val n = NearbyPcs()
        n.seen("192.168.1.5", "DESKTOP-ABC", 0)
        assertFalse(n.seen("192.168.1.5", "DESKTOP-ABC", 2_000))
        // Refreshed at 2 s, so still there at 7 s (5 s later).
        assertEquals(1, n.list(7_000).size)
    }

    @Test fun renameChangesTheList() {
        val n = NearbyPcs()
        n.seen("192.168.1.5", "DESKTOP-ABC", 0)
        assertTrue(n.seen("192.168.1.5", "STUDIO", 2_000))
        assertEquals(listOf(NearbyPc("STUDIO", "192.168.1.5")), n.list(2_000))
    }

    @Test fun quietPcsExpireAfterSixSeconds() {
        val n = NearbyPcs()
        n.seen("192.168.1.5", "A", 0)
        n.seen("192.168.1.6", "B", 3_000)
        assertEquals(2, n.list(6_000).size)          // Exactly 6 s old: kept.
        assertEquals(listOf("B"), n.list(6_001).map { it.name })
        assertTrue(n.list(9_001).isEmpty())
        assertTrue(n.isEmpty())
    }

    @Test fun expiredPcCountsAsNewAgain() {
        val n = NearbyPcs()
        n.seen("192.168.1.5", "A", 0)
        assertTrue(n.seen("192.168.1.5", "A", 10_000))
    }

    @Test fun theProbesPortIsKeptSoThePcCanBeAsked() {
        val n = NearbyPcs()
        n.seen("192.168.1.5", "DESKTOP-ABC", 0, 51234)
        assertEquals(51234, n.find("192.168.1.5", 0)?.port)
        // A PC that reopened its socket probes from a new port: the list changes, so the invite follows it.
        assertTrue(n.seen("192.168.1.5", "DESKTOP-ABC", 1_000, 51999))
        assertEquals(51999, n.find("192.168.1.5", 1_000)?.port)
        assertEquals(null, n.find("192.168.1.6", 1_000))
        assertEquals(null, n.find("192.168.1.5", 8_000)) // Expired.
    }

    @Test fun blankNameShowsTheAddress() {
        val n = NearbyPcs()
        n.seen("10.0.0.2", "  ", 0)
        assertEquals("10.0.0.2", n.list(0).single().name)
    }

    @Test fun orderedByNameThenNumericAddress() {
        val n = NearbyPcs()
        n.seen("192.168.1.10", "laptop", 0)
        n.seen("192.168.1.9", "laptop", 0)
        n.seen("192.168.1.2", "Desktop", 0)
        assertEquals(
            listOf(NearbyPc("Desktop", "192.168.1.2"), NearbyPc("laptop", "192.168.1.9"), NearbyPc("laptop", "192.168.1.10")),
            n.list(0),
        )
    }

    @Test fun statusMatchesPairedByNameAndConnectedByAddress() {
        val paired = listOf(PairedPc("aa", "Desktop-ABC"))
        val a = NearbyPc("DESKTOP-ABC", "192.168.1.5")
        val b = NearbyPc("LAPTOP", "192.168.1.6")
        assertEquals(NearbyStatus.Paired, NearbyPcs.status(a, paired, null, null))
        assertEquals(NearbyStatus.NotPaired, NearbyPcs.status(b, paired, null, null))
        assertEquals(NearbyStatus.Connected, NearbyPcs.status(a, paired, "192.168.1.5", "DESKTOP-ABC"))
        // The address wins over the name when it is known.
        assertEquals(NearbyStatus.Paired, NearbyPcs.status(a, paired, "192.168.1.7", "DESKTOP-ABC"))
        assertEquals(NearbyStatus.Connected, NearbyPcs.status(b, paired, null, "laptop"))
    }

    @Test fun orderedPutsConnectedThenPairedFirst() {
        val pcs = listOf(NearbyPc("A", "1.1.1.1"), NearbyPc("B", "1.1.1.2"), NearbyPc("C", "1.1.1.3"))
        val shown = NearbyPcs.ordered(pcs, listOf(PairedPc("x", "C")), "1.1.1.2", "B")
        assertEquals(listOf("B", "C", "A"), shown.map { it.first.name })
        assertEquals(listOf(NearbyStatus.Connected, NearbyStatus.Paired, NearbyStatus.NotPaired), shown.map { it.second })
    }
}
