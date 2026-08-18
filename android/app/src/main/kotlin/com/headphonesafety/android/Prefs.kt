package com.headphonesafety.android

import android.content.Context

/** SharedPreferences-backed settings — Android's equivalent of the other platforms' Settings.swift/persisted config. */
object Prefs {
    private const val FILE = "headphonesafety_prefs"
    private const val KEY_ENABLED = "volume_cap_enabled"
    private const val KEY_HEADROOM = "headroom_percent"
    private const val KEY_LIMITER_ENABLED = "limiter_enabled"
    private const val KEY_LIMITER_HEADROOM_DB = "limiter_headroom_db"

    val headroomPresets = intArrayOf(0, 5, 10, 15, 20)

    fun isEnabled(context: Context): Boolean =
        prefs(context).getBoolean(KEY_ENABLED, false)

    fun setEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_ENABLED, enabled).apply()
    }

    fun headroomPercent(context: Context): Int =
        prefs(context).getInt(KEY_HEADROOM, 10)

    fun setHeadroomPercent(context: Context, percent: Int) {
        prefs(context).edit().putInt(KEY_HEADROOM, percent).apply()
    }

    fun isLimiterEnabled(context: Context): Boolean =
        prefs(context).getBoolean(KEY_LIMITER_ENABLED, false)

    fun setLimiterEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_LIMITER_ENABLED, enabled).apply()
    }

    /** True dB, unlike Volume Cap's percentage approximation — DynamicsProcessing.Limiter's
     * threshold operates on real PCM signal level, so the same dB-preset concept the other
     * platforms use applies directly here. */
    fun limiterHeadroomDb(context: Context): Int =
        prefs(context).getInt(KEY_LIMITER_HEADROOM_DB, 10)

    fun setLimiterHeadroomDb(context: Context, db: Int) {
        prefs(context).edit().putInt(KEY_LIMITER_HEADROOM_DB, db).apply()
    }

    private fun prefs(context: Context) =
        context.getSharedPreferences(FILE, Context.MODE_PRIVATE)
}
