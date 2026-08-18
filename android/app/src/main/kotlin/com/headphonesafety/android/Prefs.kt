package com.headphonesafety.android

import android.content.Context

/** SharedPreferences-backed settings — Android's equivalent of the other platforms' Settings.swift/persisted config. */
object Prefs {
    private const val FILE = "headphonesafety_prefs"
    private const val KEY_ENABLED = "volume_cap_enabled"
    private const val KEY_HEADROOM = "headroom_percent"
    private const val KEY_LIMITER_ENABLED = "limiter_enabled"
    // "limiter_headroom_db" is a third legacy key, deliberately left unread — see migrateIfNeeded's
    // doc comment for why it's excluded from the merge. Still present in installed prefs files;
    // no Kotlin constant needed since nothing in this file touches it again.
    private const val KEY_SCHEMA_VERSION = "schema_version"
    private const val KEY_UNIFIED_ENABLED = "unified_enabled"
    private const val KEY_UNIFIED_HEADROOM = "unified_headroom_percent"
    private const val CURRENT_SCHEMA_VERSION = 1

    val headroomPresets = intArrayOf(0, 5, 10, 15, 20)

    /**
     * One-time, idempotent migration to the unified "Headphone Safety" toggle (docs/android-port.md
     * step 4) — guarded by [KEY_SCHEMA_VERSION] so it's safe to call on every cold path (MainActivity,
     * VolumeCapService, BootReceiver all do) without redoing it or clobbering a user's post-migration
     * choice on a later run.
     *
     * `unified_enabled` is the OR of the two legacy toggles, so migrating never silently turns
     * protection off for someone who had either one on. `unified_headroom_percent` is seeded from
     * `headroom_percent` (Volume Cap's setting) **alone**, not `max(headroom_percent,
     * limiter_headroom_db)` as originally sketched — that math assumed both were live dB-ish knobs,
     * but `limiter_headroom_db` was confirmed inert on every Samsung device tested (the vendor HAL
     * silently clamps DynamicsProcessing's threshold to -2 dB regardless of what's requested; see
     * docs/android-port.md's "Clamp-workaround arms" note). Folding a dead value into `max()` would
     * mean a stale, never-effective limiter preset could silently overwrite a user's real, working
     * Volume Cap percentage. Legacy keys are left in place afterward, unused, for rollback safety.
     */
    fun migrateIfNeeded(context: Context) {
        val p = prefs(context)
        if (p.getInt(KEY_SCHEMA_VERSION, 0) >= CURRENT_SCHEMA_VERSION) return
        val wasCapEnabled = p.getBoolean(KEY_ENABLED, false)
        val wasLimiterEnabled = p.getBoolean(KEY_LIMITER_ENABLED, false)
        p.edit()
            .putBoolean(KEY_UNIFIED_ENABLED, wasCapEnabled || wasLimiterEnabled)
            .putInt(KEY_UNIFIED_HEADROOM, p.getInt(KEY_HEADROOM, 10))
            .putInt(KEY_SCHEMA_VERSION, CURRENT_SCHEMA_VERSION)
            .apply()
    }

    fun isUnifiedEnabled(context: Context): Boolean =
        prefs(context).getBoolean(KEY_UNIFIED_ENABLED, false)

    fun setUnifiedEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_UNIFIED_ENABLED, enabled).apply()
    }

    fun unifiedHeadroomPercent(context: Context): Int =
        prefs(context).getInt(KEY_UNIFIED_HEADROOM, 10)

    fun setUnifiedHeadroomPercent(context: Context, percent: Int) {
        prefs(context).edit().putInt(KEY_UNIFIED_HEADROOM, percent).apply()
    }

    private fun prefs(context: Context) =
        context.getSharedPreferences(FILE, Context.MODE_PRIVATE)
}
