package com.headphonesafety.android

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.audiofx.AudioEffect
import android.media.audiofx.DynamicsProcessing
import android.util.Log
import androidx.core.content.ContextCompat
import java.util.UUID

private const val TAG = "HPS-Limiter"

// AudioEffect's public EFFECT_TYPE_* UUID constants don't include DynamicsProcessing (it's not
// exposed on the base class) — this is DynamicsProcessing's own effect-type UUID, taken straight
// from the framework's internal engine-init error this project hit while debugging the
// channelCount crash (see the "DynamicsProcessing gotcha" note in memory). It's a fixed AOSP
// constant, not something that varies per device, so hardcoding it here is safe.
private val DYNAMICS_PROCESSING_TYPE = UUID.fromString("7261676f-6d75-7369-6364-28e2fd3ac39e")

/**
 * The Real-Time Limiter, Android edition — see docs/android-port.md's "RESOLVED" note on
 * Feature 2 for the full architecture writeup and why this mechanism (per-session
 * DynamicsProcessing.Limiter, sessions discovered via `dumpsys media.audio_flinger`) was chosen
 * over the two dead-end alternatives (MediaProjection capture-and-replay duplicates audio instead
 * of replacing it; global session-0 attachment is deprecated/non-functional).
 *
 * Coverage is real but partial: only sessions this process can discover via `dumpsys` are
 * protected, which requires the `android.permission.DUMP` permission — a signature permission
 * that cannot be granted through a normal runtime dialog. See android/README.md for the one-time
 * `adb shell pm grant` setup step this needs.
 */
class SessionLimiterManager(private val context: Context) {

    private val attached = mutableMapOf<Int, DynamicsProcessing>()

    val activeSessionCount: Int get() = attached.size

    /** Whether this device's audio HAL exposes a DynamicsProcessing implementation at all — not
     * every OEM's effect bundle includes it, and unlike the -2dB threshold clamp found on the
     * Samsung test device (a value the effect accepts but silently overrides), a device with no
     * implementation fails at DynamicsProcessing() construction for every session, every time.
     * Checked once via AudioEffect.queryEffects() (a device-wide capability query, not per-session)
     * and cached — it can't change while the process is alive. */
    val deviceSupportsEffect: Boolean by lazy {
        runCatching {
            AudioEffect.queryEffects()?.any { it.type == DYNAMICS_PROCESSING_TYPE } ?: false
        }.onFailure { Log.e(TAG, "queryEffects failed", it) }.getOrDefault(false)
    }

    fun hasDumpPermission(): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.DUMP) ==
            PackageManager.PERMISSION_GRANTED

    /**
     * Re-scans active audio sessions and brings the attached-effects set in line with them.
     *
     * `headphoneActive` gates whether attached sessions are actually enabled — a real bug fixed
     * here: this used to always enable every attached session regardless of output route, meaning
     * the limiter would apply to speaker playback too, contradicting the whole "headphone safety"
     * framing. Deliberately does NOT route "no headphones" through `releaseAll()` — that would
     * mean re-discovering and re-attaching every session from scratch on every headphone
     * connect/disconnect, which is wasteful churn. Sessions stay attached; only their enabled
     * state toggles with the route, via `DynamicsProcessing.setEnabled`, which is cheap.
     */
    fun refresh(headroomDb: Int, headphoneActive: Boolean) {
        LimiterStatus.deviceSupportsLimiter = deviceSupportsEffect
        if (!hasDumpPermission() || !deviceSupportsEffect) {
            releaseAll()
            return
        }

        val sessions = discoverSessions()

        val stale = attached.keys - sessions
        for (id in stale) {
            detach(id)
        }

        for (id in sessions) {
            val existing = attached[id]
            if (existing == null) {
                attach(id, headroomDb, headphoneActive)
            } else {
                updateThreshold(existing, headroomDb)
            }
        }

        for ((id, dp) in attached) {
            runCatching {
                dp.setEnabled(headphoneActive)
                Log.d(TAG, "session $id setEnabled($headphoneActive) readback=${dp.getEnabled()}")
            }.onFailure { Log.e(TAG, "setEnabled sync failed for session $id", it) }
        }
    }

    fun releaseAll() {
        for (id in attached.keys.toList()) {
            detach(id)
        }
    }

    /**
     * Constructing with an explicit Config that hardcodes channelCount=2 crashes with
     * "AudioEffect: bad parameter value" on sessions whose actual channel count doesn't match
     * (confirmed live: 7 of 9 discovered sessions on the test device, likely mono system-sound
     * sessions) — the DynamicsProcessing constructor internally calls its own getChannelCount()
     * and reconciles the supplied Config against it, and a mismatch is rejected outright.
     * Constructing with cfg=null instead lets the effect auto-detect the session's real channel
     * count and builds a default config (all stages in-use) around it, which is always valid —
     * the pre/mbc/postEq stages are then explicitly disabled per-channel afterward so only the
     * limiter actually processes anything, and the limiter is configured with our headroom.
     */
    private fun attach(sessionId: Int, headroomDb: Int, headphoneActive: Boolean) {
        runCatching {
            Log.d(TAG, "attaching limiter to session $sessionId at ${-headroomDb}dB")
            val dp = DynamicsProcessing(0, sessionId, null)
            val channelCount = dp.channelCount
            for (ch in 0 until channelCount) {
                dp.getPreEqByChannelIndex(ch)?.setEnabled(false)
                dp.getMbcByChannelIndex(ch)?.setEnabled(false)
                dp.getPostEqByChannelIndex(ch)?.setEnabled(false)
                val limiter = dp.getLimiterByChannelIndex(ch)
                limiter.setAttackTime(1.0f)
                limiter.setReleaseTime(60.0f)
                limiter.setRatio(20.0f) // near brick-wall, matching the Linux/macOS hard-limit precedent
                limiter.setThreshold(-headroomDb.toFloat())
                limiter.setPostGain(0.0f)
                limiter.setEnabled(true)
            }
            // Starts disabled unless headphones are already active — otherwise a session
            // discovered while on speaker would briefly limit before refresh()'s sync loop
            // (which runs after this returns) catches up.
            dp.setEnabled(headphoneActive)
            attached[sessionId] = dp
            val readBack = dp.getLimiterByChannelIndex(0)?.threshold
            Log.d(TAG, "attached OK to session $sessionId ($channelCount ch), enabled=${dp.getEnabled()}, threshold readback=$readBack")
        }.onFailure { Log.e(TAG, "attach failed for session $sessionId", it) }
    }

    private fun updateThreshold(dp: DynamicsProcessing, headroomDb: Int) {
        runCatching {
            for (ch in 0 until dp.channelCount) {
                dp.getLimiterByChannelIndex(ch)?.setThreshold(-headroomDb.toFloat())
            }
            val readBack = dp.getLimiterByChannelIndex(0)?.threshold
            Log.d(TAG, "updateThreshold requested=${-headroomDb} readback=$readBack")
        }.onFailure { Log.e(TAG, "updateThreshold failed", it) }
    }

    private fun detach(sessionId: Int) {
        attached.remove(sessionId)?.let { dp ->
            runCatching { dp.setEnabled(false) }
            runCatching { dp.release() }
        }
    }

    /** Parses the "Global session refs:" table out of `dumpsys media.audio_flinger` — the only
     * public(ish) source of other apps' audio session IDs available without root. Requires DUMP.
     * This table's format is undocumented debug output, not a stable API — it's reverse-engineered
     * from one Android 10/One UI device, and AOSP or OEM forks on other versions could shape it
     * differently. If the expected header is missing outright, that's surfaced as a format
     * mismatch (LimiterStatus.sessionFormatMismatch) rather than silently reported as "0 sessions,"
     * and a looser fallback scrape is used so coverage degrades gracefully instead of going dark. */
    /** Visible internally so the debug-only positive-control test (MainActivity) can reuse the
     * same session discovery the real limiter uses, instead of duplicating the dumpsys parsing. */
    internal fun discoverSessions(): Set<Int> {
        return runCatching {
            val process = ProcessBuilder("dumpsys", "media.audio_flinger")
                .redirectErrorStream(true)
                .start()
            val output = process.inputStream.bufferedReader().readText()
            val exit = process.waitFor()
            val tableResult = parseSessionTable(output)
            val sessions: Set<Int>
            if (tableResult != null) {
                LimiterStatus.sessionFormatMismatch = false
                sessions = tableResult
            } else {
                LimiterStatus.sessionFormatMismatch = output.length > 500
                sessions = parseSessionMentions(output)
                Log.w(TAG, "dumpsys format mismatch (no 'Global session refs:' header found on " +
                    "this device/Android version) — using fallback scrape, ${sessions.size} found")
            }
            Log.d(TAG, "dumpsys exit=$exit outputLen=${output.length} sessions=$sessions")
            sessions
        }.onFailure { Log.e(TAG, "discoverSessions failed", it) }.getOrDefault(emptySet())
    }

    companion object {
        private val WHITESPACE = Regex("""\s+""")
        private val FALLBACK_SESSION_MENTION = Regex("""(?i)session[\s:=]+(\d{1,7})\b""")

        /**
         * Primary parse strategy for the "Global session refs:" table. Reads the *column position*
         * of "session" from the header row instead of assuming a fixed column count/order — this
         * was confirmed necessary by comparing two real devices live: a Galaxy S9+/Android 10 whose
         * rows are 3 plain numbers, versus a Galaxy S21/Android 15 whose header is
         * `session  cnt  pid  uid  name` (5 columns, the last one non-numeric text). A fixed
         * 3-number-only regex matches the former and silently matches nothing on the latter.
         * Reading the header's column order handles both, and any future reordering/insertion,
         * without needing per-version special-casing.
         *
         * Returns null (not an empty set) if the "Global session refs:" section itself is never
         * found, so callers can tell "section absent" (a bigger format change, needs the fallback
         * scrape) apart from "section present but its table is genuinely empty right now."
         */
        internal fun parseSessionTable(dumpText: String): Set<Int>? {
            val lines = dumpText.lineSequence().map { it.trim() }.iterator()
            var foundSection = false
            for (line in lines) {
                if (line.startsWith("Global session refs:")) {
                    foundSection = true
                    break
                }
            }
            if (!foundSection) return null

            // The next non-blank line is expected to be the column header, naming where the
            // "session" column sits — read it rather than assuming its index.
            var sessionColumnIndex = -1
            while (lines.hasNext()) {
                val line = lines.next()
                if (line.isEmpty()) continue
                sessionColumnIndex = line.split(WHITESPACE).indexOf("session")
                break
            }
            if (sessionColumnIndex < 0) return emptySet() // section present, no header/rows at all

            val result = mutableSetOf<Int>()
            while (lines.hasNext()) {
                val line = lines.next()
                if (line.isEmpty()) break
                val sessionId = line.split(WHITESPACE).getOrNull(sessionColumnIndex)?.toIntOrNull()
                    ?: break // table ended (ran into the next dumpsys section)
                // Negative/zero session ids are reserved platform sentinels, not real per-app
                // sessions — confirmed live on a Galaxy S21: Samsung's own SoundAlive service
                // holds session -3, which DynamicsProcessing correctly refuses to attach to
                // ("Cannot initialize effect engine ... Error: -3"). Still a valid table row
                // though (matches the header shape), so skip it without ending the scan.
                if (sessionId > 0) result.add(sessionId)
            }
            return result
        }

        // A real phone is never going to have anywhere close to this many concurrent audio
        // sessions — live testing found this scrape correctly picks up exactly the real session
        // ids on two real devices (it keys off "N effects for session <id>" lines elsewhere in
        // the dump), but it's still a loose text match with no structural guarantee. If some other
        // device's dump text makes it match far more than could plausibly be real, that's a sign
        // it's matching noise, not sessions — better to report "couldn't determine sessions" than
        // to feed dozens of bogus ids into attach() every 2 seconds forever.
        private const val MAX_PLAUSIBLE_FALLBACK_SESSIONS = 40

        /** Fallback for when the primary table's header format has drifted on some Android
         * version/OEM fork — scrapes any "session <number>" mention from the raw dump text
         * (matches things like the "N effects for session <id>" lines AudioFlinger's dump prints
         * per active session, elsewhere from the summary table). Noisier and less precise than the
         * table parse (can't rule out an unrelated number that happens to sit near the word
         * "session"), so it's only used when the primary parse fails outright, and self-limits if
         * it looks like it's matching noise rather than real sessions. */
        internal fun parseSessionMentions(dumpText: String): Set<Int> {
            val matches = FALLBACK_SESSION_MENTION.findAll(dumpText)
                .mapNotNull { it.groupValues[1].toIntOrNull() }
                .filter { it > 0 }
                .toSet()
            if (matches.size > MAX_PLAUSIBLE_FALLBACK_SESSIONS) {
                Log.w(TAG, "fallback scrape found ${matches.size} 'session' mentions — " +
                    "implausibly high, treating as noise rather than real sessions")
                return emptySet()
            }
            return matches
        }
    }
}
