package com.headphonesafety.android

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat

private const val TAG = "HPS-Harness"

/**
 * Debug-only foreground service — NOT part of the shipped Headphone Safety feature. Exists solely
 * because `MediaProjectionManager.getMediaProjection()` throws
 * `SecurityException: Media projections require a foreground service of type
 * ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION` unless called from within an active
 * foreground service of that type — confirmed live on this project's Android 10/API 29 test
 * device, contradicting the assumption (based on reading the API docs, not testing) that this
 * restriction only applies starting Android 14. Calling it directly from an Activity, as
 * `MainActivity` originally tried, crashes every time regardless of device Android version.
 */
class HarnessCaptureService : Service() {

    companion object {
        const val EXTRA_RESULT_CODE = "resultCode"
        const val EXTRA_RESULT_DATA = "resultData"
        const val EXTRA_DURATION_MS = "durationMs"
        private const val NOTIFICATION_ID = 2
        private const val CHANNEL_ID = "harness_capture"
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        ensureChannel()
        val notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Headphone Safety (debug)")
            .setContentText("Capturing audio for the measurement harness...")
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        val resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0)
        val resultData = intent.getParcelableExtra<Intent>(EXTRA_RESULT_DATA)
        val durationMs = intent.getLongExtra(EXTRA_DURATION_MS, 8000L)

        if (resultData == null) {
            Log.e(TAG, "HarnessCaptureService started with no result data")
            stopSelf()
            return START_NOT_STICKY
        }

        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val projection = runCatching { mpm.getMediaProjection(resultCode, resultData) }
            .onFailure { Log.e(TAG, "getMediaProjection failed", it) }
            .getOrNull()

        if (projection == null) {
            stopSelf()
            return START_NOT_STICKY
        }

        PlaybackCaptureHarness.capture(projection, durationMs) { _ ->
            // Result itself is logged inside PlaybackCaptureHarness (tag HPS-Harness) — this is a
            // debug tool driven via logcat, not surfaced back through app UI.
            runCatching { projection.stop() }
            stopSelf()
        }
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (nm.getNotificationChannel(CHANNEL_ID) != null) return
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Debug capture harness", NotificationManager.IMPORTANCE_LOW)
        )
    }
}
