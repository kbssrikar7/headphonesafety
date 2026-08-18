package com.headphonesafety.android

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.AudioPlaybackConfiguration
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.core.app.NotificationCompat

/**
 * Foreground service that keeps Volume Cap running while the app isn't in the foreground —
 * Android's equivalent of the menu-bar/tray daemon on macOS and Linux.
 *
 * Headphone/Bluetooth presence check is a simplification, documented in docs/android-port.md:
 * Android has no direct "is this the current default output route" query the way macOS's
 * kAudioDevicePropertyTransportType on the default device does — presence of a headphone-classed
 * device in AudioManager.getDevices(GET_DEVICES_OUTPUTS) is used as a proxy for "headphones are
 * the active output," matching the doc's stated approach.
 *
 * Clamp enforcement runs on two triggers: the `android.media.VOLUME_CHANGED_ACTION` broadcast
 * (undocumented but widely relied on) for fast reaction, plus a 400ms poll loop — the same
 * cadence the macOS build uses — as a fallback in case that broadcast doesn't fire on a given
 * OEM skin. Belt and suspenders, not a bet on the undocumented path alone.
 */
class VolumeCapService : Service() {

    companion object {
        const val ACTION_STOP = "com.headphonesafety.android.action.STOP"
        private const val NOTIFICATION_ID = 1
        private const val CHANNEL_ID = "volume_cap_status"
        private const val POLL_INTERVAL_MS = 400L
        private const val LIMITER_POLL_INTERVAL_MS = 2000L
        private const val SET_VOLUME_RETRIES = 3
        // AudioPlaybackConfiguration changes arrive in bursts (multiple callbacks within a few
        // hundred ms of one new session starting) — coalesce into a single out-of-cycle scan
        // rather than forking a dumpsys process per burst event.
        private const val PLAYBACK_CALLBACK_DEBOUNCE_MS = 300L
        private const val TAG = "HPS-VolumeCapService"
        // The Limiter's threshold parameter is confirmed inert on every Samsung device tested —
        // silently clamped to -2.0 dB regardless of what's requested, and every other candidate
        // gain parameter (postGain, MBC, PreEq) tested the same way (docs/android-port.md's
        // "Clamp-workaround arms" note). No UI control exposes this value anymore since it does
        // nothing; the constant exists only because SessionLimiterManager.attach() still needs
        // *a* number to pass to setThreshold(), and 2 keeps that call's behavior legible/consistent
        // with the documented ceiling in case a future non-Samsung device does honor it.
        private const val LIMITER_FIXED_HEADROOM_DB = 2

        private val HEADPHONE_TYPES = buildSet {
            add(AudioDeviceInfo.TYPE_WIRED_HEADPHONES)
            add(AudioDeviceInfo.TYPE_WIRED_HEADSET)
            add(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                add(AudioDeviceInfo.TYPE_USB_HEADSET)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                add(AudioDeviceInfo.TYPE_BLE_HEADSET)
            }
        }
    }

    private lateinit var audioManager: AudioManager
    private lateinit var limiterManager: SessionLimiterManager
    @Volatile private var headphoneActive = false

    private val deviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>) {
            refreshHeadphoneState(enforceIfActive = true)
        }

        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>) {
            // React unconditionally rather than re-querying mid-event — same discipline as the
            // Linux/macOS removal watchers (docs/android-port.md hard-won lesson #4).
            headphoneActive = false
            refreshHeadphoneState(enforceIfActive = false)
        }
    }

    /**
     * Trigger only, not a session-ID source — `AudioPlaybackConfiguration.getSessionId()` is
     * `@SystemApi`-hidden and scrubbed to a placeholder for normal apps (confirmed via AOSP's
     * `PlaybackActivityMonitor.anonymizeForPublicConsumption()`), so this callback can't hand us
     * real session ids directly the way `dumpsys media.audio_flinger` can. What it *can* do is
     * tell us "something's playback state changed somewhere on the system, right now" — used here
     * only to run an out-of-cycle limiter scan instead of waiting up to
     * [LIMITER_POLL_INTERVAL_MS] for the next poll tick. The base poll interval is deliberately
     * left unchanged; the cost this trigger saves is *latency to first scan*, not scan frequency.
     */
    private val playbackCallback = object : AudioManager.AudioPlaybackCallback() {
        override fun onPlaybackConfigChanged(configs: MutableList<AudioPlaybackConfiguration>?) {
            pollHandler.removeCallbacks(debouncedLimiterScan)
            pollHandler.postDelayed(debouncedLimiterScan, PLAYBACK_CALLBACK_DEBOUNCE_MS)
        }
    }

    private val debouncedLimiterScan = Runnable {
        if (Prefs.isUnifiedEnabled(this)) {
            Log.d(TAG, "out-of-cycle limiter scan triggered by AudioPlaybackCallback")
        }
        // Cancel any already-scheduled regular tick and let this run take its place — running
        // limiterPollRunnable directly (not just its body) means it also re-schedules the next
        // regular tick LIMITER_POLL_INTERVAL_MS from *now*, so the trigger and the base poll never
        // double up.
        pollHandler.removeCallbacks(limiterPollRunnable)
        limiterPollRunnable.run()
    }

    private val volumeChangeReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (headphoneActive) enforceCap()
        }
    }

    private val pollHandler = Handler(Looper.getMainLooper())
    private val pollRunnable = object : Runnable {
        override fun run() {
            if (headphoneActive) enforceCap()
            pollHandler.postDelayed(this, POLL_INTERVAL_MS)
        }
    }

    private val limiterPollRunnable = object : Runnable {
        override fun run() {
            LimiterStatus.dumpGranted = limiterManager.hasDumpPermission()
            when {
                !Prefs.isUnifiedEnabled(this@VolumeCapService) -> limiterManager.releaseAll()
                // No reason to fork `dumpsys` and re-discover sessions while nothing would be
                // enabled anyway — real battery win on top of the per-session setEnabled sync,
                // since the poll would otherwise keep running (and finding sessions to silently
                // leave disabled) the whole time the phone plays through the speaker.
                !headphoneActive -> { /* leave already-attached sessions as-is, just disabled */ }
                else -> refreshLimiter()
            }
            LimiterStatus.activeSessionCount = limiterManager.activeSessionCount
            updateNotification()
            pollHandler.postDelayed(this, LIMITER_POLL_INTERVAL_MS)
        }
    }

    private fun refreshLimiter() {
        limiterManager.refresh(LIMITER_FIXED_HEADROOM_DB, headphoneActive)
    }

    override fun onCreate() {
        super.onCreate()
        Prefs.migrateIfNeeded(this)
        audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        limiterManager = SessionLimiterManager(this)
        audioManager.registerAudioDeviceCallback(deviceCallback, null)
        audioManager.registerAudioPlaybackCallback(playbackCallback, pollHandler)
        registerReceiver(volumeChangeReceiver, IntentFilter("android.media.VOLUME_CHANGED_ACTION"))
        pollHandler.postDelayed(pollRunnable, POLL_INTERVAL_MS)
        pollHandler.postDelayed(limiterPollRunnable, LIMITER_POLL_INTERVAL_MS)
        refreshHeadphoneState(enforceIfActive = true)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            Prefs.setUnifiedEnabled(this, false)
            stopSelf()
            return START_NOT_STICKY
        }
        startForeground(NOTIFICATION_ID, buildNotification())
        return START_STICKY
    }

    override fun onDestroy() {
        audioManager.unregisterAudioDeviceCallback(deviceCallback)
        audioManager.unregisterAudioPlaybackCallback(playbackCallback)
        unregisterReceiver(volumeChangeReceiver)
        pollHandler.removeCallbacks(pollRunnable)
        pollHandler.removeCallbacks(limiterPollRunnable)
        pollHandler.removeCallbacks(debouncedLimiterScan)
        limiterManager.releaseAll()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun refreshHeadphoneState(enforceIfActive: Boolean) {
        val outputs = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        headphoneActive = outputs.any { it.type in HEADPHONE_TYPES }
        updateNotification()
        if (headphoneActive && enforceIfActive) enforceCap()
        // Sync the limiter's enabled state immediately on any route change rather than waiting
        // up to LIMITER_POLL_INTERVAL_MS for the next poll tick — disconnect in particular must
        // disable promptly, not lag behind, or "headphone safety" would keep limiting speaker
        // audio for up to 2 seconds after headphones come off.
        if (Prefs.isUnifiedEnabled(this)) {
            refreshLimiter()
            LimiterStatus.activeSessionCount = limiterManager.activeSessionCount
        }
    }

    /**
     * A successful setStreamVolume call is not proof the change took effect (hard-won lesson #1
     * in docs/android-port.md) — read the volume back and retry a bounded number of times rather
     * than trusting the call.
     */
    private fun enforceCap() {
        val headroomPercent = Prefs.unifiedHeadroomPercent(this)
        val max = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
        val min = audioManager.getStreamMinVolume(AudioManager.STREAM_MUSIC)
        val cap = (max * (100 - headroomPercent) / 100).coerceIn(min, max)

        repeat(SET_VOLUME_RETRIES) {
            val current = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)
            if (current <= cap) return
            audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, cap, 0)
        }
    }

    private fun buildNotification() = NotificationCompat.Builder(this, CHANNEL_ID)
        .setContentTitle(getString(R.string.app_name))
        .setContentText(statusText())
        .setSmallIcon(android.R.drawable.ic_lock_silent_mode_off)
        .setOngoing(true)
        .setContentIntent(
            PendingIntent.getActivity(
                this, 0, Intent(this, MainActivity::class.java),
                PendingIntent.FLAG_IMMUTABLE
            )
        )
        .addAction(
            0, getString(R.string.stop_action),
            PendingIntent.getService(
                this, 0, Intent(this, VolumeCapService::class.java).setAction(ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE
            )
        )
        .build()

    private fun updateNotification() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        ensureChannel(nm)
        nm.notify(NOTIFICATION_ID, buildNotification())
    }

    private fun ensureChannel(nm: NotificationManager) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        if (nm.getNotificationChannel(CHANNEL_ID) != null) return
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, getString(R.string.app_name), NotificationManager.IMPORTANCE_LOW)
        )
    }

    private fun statusText(): String {
        val capPart = if (headphoneActive) getString(R.string.status_capping) else getString(R.string.status_no_headphones)
        if (!Prefs.isUnifiedEnabled(this)) return capPart
        val limiterPart = when {
            !limiterManager.hasDumpPermission() -> getString(R.string.status_limiter_no_dump)
            !limiterManager.deviceSupportsEffect -> getString(R.string.status_limiter_unsupported)
            LimiterStatus.sessionFormatMismatch ->
                getString(R.string.status_limiter_sessions_uncertain, limiterManager.activeSessionCount)
            else -> getString(R.string.status_limiter_sessions, limiterManager.activeSessionCount)
        }
        return "$capPart $limiterPart"
    }
}
