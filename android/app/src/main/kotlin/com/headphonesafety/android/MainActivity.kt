package com.headphonesafety.android

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
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

    companion object {
        private const val REQUEST_NOTIFICATIONS = 100
    }
}
