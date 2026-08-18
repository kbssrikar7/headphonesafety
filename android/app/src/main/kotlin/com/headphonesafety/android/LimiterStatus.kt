package com.headphonesafety.android

/** Shared, process-local status the running service publishes and MainActivity reads —
 * avoids a bind/broadcast round trip for what's just a couple of ints the UI polls. */
object LimiterStatus {
    @Volatile var dumpGranted: Boolean = false
    @Volatile var activeSessionCount: Int = 0

    /** null = not checked yet; false = this device's audio HAL has no DynamicsProcessing effect
     * at all, so the limiter can never attach regardless of DUMP/sessions — a real, device-
     * dependent gap (not every OEM ships the effect), distinct from "0 sessions right now." */
    @Volatile var deviceSupportsLimiter: Boolean? = null

    /** True when `dumpsys media.audio_flinger`'s expected "Global session refs:" table wasn't
     * found at all on this Android version/OEM build — a dumpsys format drift, not "0 sessions."
     * The fallback text-scrape parser was used instead, which is best-effort. */
    @Volatile var sessionFormatMismatch: Boolean = false
}
