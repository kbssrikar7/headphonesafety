package com.headphonesafety.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Regression coverage for the dumpsys parsing that discovers audio sessions to protect. Fixtures
 * below are trimmed, real `dumpsys media.audio_flinger` output captured live from two different
 * physical phones during this project's cross-device testing — a Galaxy S9+/Android 10 (3-column
 * table, session first) and a Galaxy S21/Android 15 (5-column table, session still first but with
 * an extra "cnt" column and a trailing non-numeric "name" column). The two devices' headers differ
 * enough that a fixed-column-count parser silently returns nothing on one of them; that's the bug
 * this class's dynamic header-driven parsing exists to avoid.
 */
class SessionLimiterManagerTest {

    @Test
    fun `parses Android 10 Galaxy S9+ style table (session, pid, count)`() {
        val dump = """
            Global session refs:
              session   pid count
                 9953 25793     1
                 9961 25793     1
                 9969 25793     1
                15801  1388     1
            mSomeUnrelatedNextSection=1
        """.trimIndent()

        val sessions = SessionLimiterManager.parseSessionTable(dump)

        assertEquals(setOf(9953, 9961, 9969, 15801), sessions)
    }

    @Test
    fun `parses Android 15 Galaxy S21 style table (session, cnt, pid, uid, name) with populated rows`() {
        val dump = """
            Global session refs:
              session  cnt     pid    uid  name
               363319    1    7542   1000  com.example.player
               363327    2    8067  10055  com.example.other
            mSystemReady=1
        """.trimIndent()

        val sessions = SessionLimiterManager.parseSessionTable(dump)

        assertEquals(setOf(363319, 363327), sessions)
    }

    @Test
    fun `Android 15 style table with zero active sessions returns empty set, not null`() {
        // Captured live: the section and header both exist, but the very next line is already
        // the following dumpsys section because nothing is playing right now.
        val dump = """
            Global session refs:
              session  cnt     pid    uid  name
            mSystemReady=1
            CoreFx Setting Values:
        """.trimIndent()

        val sessions = SessionLimiterManager.parseSessionTable(dump)

        assertEquals(emptySet<Int>(), sessions)
    }

    @Test
    fun `negative session id (reserved platform sentinel) is excluded but doesn't end the scan`() {
        // Captured live on a Galaxy S21/Android 15: Samsung's SoundAlive service holds session
        // -3, a reserved sentinel, not a real per-app session. DynamicsProcessing correctly
        // refuses to attach to it ("Cannot initialize effect engine ... Error: -3"), so it must
        // never reach attach() — but it's still a well-formed table row, and real sessions listed
        // after it must still be picked up.
        val dump = """
            Global session refs:
              session  cnt     pid    uid  name
               364297    2   27424  10343  com.sec.android.app.music
                   -3    3   27804  10139  com.sec.android.app.soundalive
               364298    1    7326   1041  audioserver
            mSystemReady=1
        """.trimIndent()

        val sessions = SessionLimiterManager.parseSessionTable(dump)

        assertEquals(setOf(364297, 364298), sessions)
    }

    @Test
    fun `missing 'Global session refs' section entirely returns null, not empty`() {
        val dump = """
            Some totally different dumpsys section
            with no session table in it at all
        """.trimIndent()

        assertNull(SessionLimiterManager.parseSessionTable(dump))
    }

    @Test
    fun `fallback scrape finds session mentions when the table format is unrecognizable`() {
        val dump = """
            AudioTrack::add session 4242 pid 555
            some other line
            removeTrack_l session=9000
        """.trimIndent()

        val sessions = SessionLimiterManager.parseSessionMentions(dump)

        assertEquals(setOf(4242, 9000), sessions)
    }

    @Test
    fun `fallback scrape self-limits to empty when match count is implausibly high (noise, not real sessions)`() {
        val dump = (1..100).joinToString("\n") { "session $it" }

        val sessions = SessionLimiterManager.parseSessionMentions(dump)

        assertEquals(emptySet<Int>(), sessions)
    }
}
