package com.headphonesafety.android

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.widget.Button
import android.widget.CompoundButton
import android.widget.RadioGroup
import android.widget.Switch
import android.widget.TextView
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

class MainActivity : Activity() {

    private lateinit var enableSwitch: Switch
    private lateinit var headroomGroup: RadioGroup
    private lateinit var statusText: TextView
    private lateinit var limiterSwitch: Switch
    private lateinit var limiterHeadroomGroup: RadioGroup
    private lateinit var limiterStatusText: TextView
    private lateinit var batteryStatusText: TextView
    private lateinit var batterySettingsButton: Button
    private lateinit var oemBatteryNoteText: TextView
    private lateinit var harnessButton: Button
    private lateinit var harnessResultText: TextView
    private lateinit var testTonePlayButton: Button
    private var testTonePlayer: MediaPlayer? = null

    private val uiHandler = Handler(Looper.getMainLooper())
    private val limiterStatusRunnable = object : Runnable {
        override fun run() {
            updateLimiterStatusText()
            uiHandler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        enableSwitch = findViewById(R.id.enableSwitch)
        headroomGroup = findViewById(R.id.headroomGroup)
        statusText = findViewById(R.id.statusText)
        limiterSwitch = findViewById(R.id.limiterSwitch)
        limiterHeadroomGroup = findViewById(R.id.limiterHeadroomGroup)
        limiterStatusText = findViewById(R.id.limiterStatusText)
        batteryStatusText = findViewById(R.id.batteryStatusText)
        batterySettingsButton = findViewById(R.id.batterySettingsButton)
        oemBatteryNoteText = findViewById(R.id.oemBatteryNoteText)
        oemBatteryNoteText.text = buildOemBatteryNote()
        harnessButton = findViewById(R.id.harnessButton)
        harnessResultText = findViewById(R.id.harnessResultText)
        testTonePlayButton = findViewById(R.id.testTonePlayButton)
        if (BuildConfig.DEBUG) {
            harnessButton.visibility = android.view.View.VISIBLE
            harnessResultText.visibility = android.view.View.VISIBLE
            harnessButton.setOnClickListener { startHarnessCapture() }
            testTonePlayButton.visibility = android.view.View.VISIBLE
            testTonePlayButton.setOnClickListener { toggleTestTone() }
        }
        batterySettingsButton.setOnClickListener {
            // Opens the OS's battery-optimization list screen, not the direct per-app request
            // dialog — the user does the actual whitelisting themselves from there.
            startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
        }

        val radioIds = intArrayOf(
            R.id.headroom0, R.id.headroom5, R.id.headroom10, R.id.headroom15, R.id.headroom20
        )
        val currentIndex = Prefs.headroomPresets.indexOf(Prefs.headroomPercent(this)).coerceAtLeast(0)
        headroomGroup.check(radioIds[currentIndex])
        headroomGroup.setOnCheckedChangeListener { _, checkedId ->
            val index = radioIds.indexOf(checkedId)
            if (index >= 0) {
                Prefs.setHeadroomPercent(this, Prefs.headroomPresets[index])
                if (Prefs.isEnabled(this)) startCapService()
            }
        }

        val limiterRadioIds = intArrayOf(
            R.id.limiterDb0, R.id.limiterDb5, R.id.limiterDb10, R.id.limiterDb15, R.id.limiterDb20
        )
        val currentLimiterIndex =
            Prefs.headroomPresets.indexOf(Prefs.limiterHeadroomDb(this)).coerceAtLeast(0)
        limiterHeadroomGroup.check(limiterRadioIds[currentLimiterIndex])
        limiterHeadroomGroup.setOnCheckedChangeListener { _, checkedId ->
            val index = limiterRadioIds.indexOf(checkedId)
            if (index >= 0) {
                Prefs.setLimiterHeadroomDb(this, Prefs.headroomPresets[index])
                if (Prefs.isLimiterEnabled(this)) startCapService()
            }
        }

        enableSwitch.isChecked = Prefs.isEnabled(this)
        enableSwitch.setOnCheckedChangeListener { _: CompoundButton, checked: Boolean ->
            Prefs.setEnabled(this, checked)
            applyServiceState()
            updateStatusText()
        }

        limiterSwitch.isChecked = Prefs.isLimiterEnabled(this)
        limiterSwitch.setOnCheckedChangeListener { _: CompoundButton, checked: Boolean ->
            Prefs.setLimiterEnabled(this, checked)
            applyServiceState()
            updateLimiterStatusText()
        }

        // Cold-launch auto-resume: isChecked above was set before these listeners attached, so
        // it doesn't start the service on its own. Without this, re-opening the app after the
        // system kills the background service (or after using it fresh post-install) leaves
        // Prefs saying "enabled" while nothing is actually running to back that up.
        if (Prefs.isEnabled(this) || Prefs.isLimiterEnabled(this)) {
            requestNotificationPermissionThenStart()
        }

        updateStatusText()
        updateLimiterStatusText()
        updateBatteryStatusText()
    }

    override fun onResume() {
        super.onResume()
        enableSwitch.isChecked = Prefs.isEnabled(this)
        limiterSwitch.isChecked = Prefs.isLimiterEnabled(this)
        updateStatusText()
        updateBatteryStatusText()
        uiHandler.post(limiterStatusRunnable)
    }

    override fun onPause() {
        super.onPause()
        uiHandler.removeCallbacks(limiterStatusRunnable)
    }

    override fun onDestroy() {
        super.onDestroy()
        testTonePlayer?.release()
        testTonePlayer = null
    }

    private fun updateStatusText() {
        statusText.text = if (Prefs.isEnabled(this)) {
            getString(R.string.status_running, Prefs.headroomPercent(this))
        } else {
            getString(R.string.status_disabled)
        }
    }

    private fun updateBatteryStatusText() {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        val exempt = pm.isIgnoringBatteryOptimizations(packageName)
        batteryStatusText.text = if (exempt) {
            getString(R.string.battery_exempt)
        } else {
            getString(R.string.battery_not_exempt)
        }
        batterySettingsButton.visibility = if (exempt) android.view.View.GONE else android.view.View.VISIBLE
    }

    /**
     * Only the Samsung path is confirmed live (this project's two real test devices, a Galaxy
     * S9+ and a Galaxy S21, are both Samsung) — every other maker's path below is sourced from
     * dontkillmyapp.com and labeled as unverified in the string itself, not claimed as tested.
     * Falls back to a generic "look for a similar setting" note for makers not covered here
     * (Pixel/AOSP-close phones, Motorola, etc., which generally don't need this at all).
     */
    private fun buildOemBatteryNote(): String {
        val makerNote = when (Build.MANUFACTURER.lowercase()) {
            "samsung" -> R.string.oem_battery_note_samsung
            "xiaomi" -> R.string.oem_battery_note_xiaomi
            "huawei", "honor" -> R.string.oem_battery_note_huawei
            "oneplus" -> R.string.oem_battery_note_oneplus
            "oppo", "realme" -> R.string.oem_battery_note_oppo
            "vivo" -> R.string.oem_battery_note_vivo
            else -> R.string.oem_battery_note_generic
        }
        return "${getString(R.string.oem_battery_note_intro)} ${getString(makerNote)}\n\n" +
            getString(R.string.oem_battery_note_source)
    }

    private fun updateLimiterStatusText() {
        limiterStatusText.text = when {
            !Prefs.isLimiterEnabled(this) -> getString(R.string.limiter_status_off)
            !LimiterStatus.dumpGranted -> getString(R.string.limiter_status_no_dump, packageName)
            LimiterStatus.deviceSupportsLimiter == false ->
                getString(R.string.limiter_status_unsupported_device)
            LimiterStatus.sessionFormatMismatch ->
                getString(R.string.limiter_status_active_uncertain, LimiterStatus.activeSessionCount)
            else -> getString(R.string.limiter_status_active, LimiterStatus.activeSessionCount)
        }
    }

    /** Service should run whenever either feature is on; stop only once both are off. */
    private fun applyServiceState() {
        if (Prefs.isEnabled(this) || Prefs.isLimiterEnabled(this)) {
            requestNotificationPermissionThenStart()
        } else {
            stopCapService()
        }
    }

    private fun requestNotificationPermissionThenStart() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED
        ) {
            ActivityCompat.requestPermissions(
                this, arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATIONS
            )
        }
        startCapService()
    }

    private fun startCapService() {
        val intent = Intent(this, VolumeCapService::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
    }

    private fun stopCapService() {
        startService(Intent(this, VolumeCapService::class.java).setAction(VolumeCapService.ACTION_STOP))
    }

    /**
     * Debug-only self-contained hot test tone, used for the measurement harness's control test
     * and clamp-workaround experiments — plays inside this app's own process so its audio session
     * goes through the exact same discovery/attach path as any third-party app's would, without
     * depending on an external media app or chooser dialog. Reads from this app's own external
     * files dir (`getExternalFilesDir`), not a shared path like /sdcard/Download — scoped storage
     * (Android 10+) blocks a normal app from opening arbitrary shared-storage paths without
     * MANAGE_EXTERNAL_STORAGE, confirmed live the hard way (crash-log EACCES on
     * `/sdcard/Download/hps_test_tone.wav`). Push the tone there with:
     * `adb push hps_test_tone.wav /sdcard/Android/data/com.headphonesafety.android/files/`
     */
    private fun toggleTestTone() {
        val existing = testTonePlayer
        if (existing != null) {
            existing.stop()
            existing.release()
            testTonePlayer = null
            testTonePlayButton.text = "Play test tone (debug)"
            return
        }
        val toneFile = java.io.File(getExternalFilesDir(null), "hps_test_tone.wav")
        if (!toneFile.exists()) {
            harnessResultText.text = "Missing $toneFile — adb push it first."
            return
        }
        val player = MediaPlayer().apply {
            // MediaPlayer defaults to USAGE_UNKNOWN, not USAGE_MEDIA — confirmed live via
            // `dumpsys audio`'s playback event log. PlaybackCaptureHarness's capture config
            // matches only USAGE_MEDIA (the same usage real media apps use), so without this the
            // harness would silently capture nothing from this test tone at all.
            setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            setDataSource(toneFile.absolutePath)
            isLooping = true
            prepare()
            start()
        }
        testTonePlayer = player
        testTonePlayButton.text = "Stop test tone (debug)"
    }

    /**
     * Debug-only entry point into [PlaybackCaptureHarness] — see that class's doc comment for
     * why this exists (a real level measurement, since a correct DynamicsProcessing parameter
     * read-back is not proof it's actually applied). RECORD_AUDIO is required by AudioRecord even
     * in playback-capture mode; MediaProjection's consent dialog is the standard system flow for
     * this API, not something this app can skip.
     */
    private fun startHarnessCapture() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            ActivityCompat.requestPermissions(
                this, arrayOf(Manifest.permission.RECORD_AUDIO), REQUEST_RECORD_AUDIO
            )
            return
        }
        harnessResultText.text = "Requesting capture permission..."
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        @Suppress("DEPRECATION")
        startActivityForResult(mpm.createScreenCaptureIntent(), REQUEST_MEDIA_PROJECTION)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_RECORD_AUDIO &&
            grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED
        ) {
            startHarnessCapture()
        }
    }

    /**
     * `MediaProjectionManager.getMediaProjection()` throws `SecurityException: Media projections
     * require a foreground service of type ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION`
     * unless called from within one — confirmed live, crashes even on this Android 10 device
     * despite that restriction being documented as an Android 14+ thing. So this hands the
     * result off to [HarnessCaptureService] instead of calling getMediaProjection() here.
     */
    @Suppress("DEPRECATION")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_MEDIA_PROJECTION) return
        if (resultCode != Activity.RESULT_OK || data == null) {
            harnessResultText.text = "Capture permission denied."
            return
        }
        harnessResultText.text =
            "Capturing via HarnessCaptureService (${HARNESS_DURATION_MS / 1000}s) — see logcat HPS-Harness"
        val serviceIntent = Intent(this, HarnessCaptureService::class.java).apply {
            putExtra(HarnessCaptureService.EXTRA_RESULT_CODE, resultCode)
            putExtra(HarnessCaptureService.EXTRA_RESULT_DATA, data)
            putExtra(HarnessCaptureService.EXTRA_DURATION_MS, HARNESS_DURATION_MS)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(serviceIntent)
        } else {
            startService(serviceIntent)
        }
    }

    companion object {
        private const val REQUEST_NOTIFICATIONS = 100
        private const val REQUEST_RECORD_AUDIO = 101
        private const val REQUEST_MEDIA_PROJECTION = 102
        private const val HARNESS_DURATION_MS = 8000L
    }
}
