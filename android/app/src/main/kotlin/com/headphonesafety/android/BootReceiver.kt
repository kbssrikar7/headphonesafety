package com.headphonesafety.android

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build

/**
 * Without this, a device reboot silently drops both features: the foreground service does not
 * survive a reboot, and nothing else in this app runs until the user manually reopens it and
 * re-toggles the switches. `Prefs` already persists the "should be on" state — this just acts on
 * it at boot, the same way the (already-existing) auto-resume-on-cold-launch fix in MainActivity
 * acts on it when the service was killed by the system without a reboot.
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        if (!Prefs.isEnabled(context) && !Prefs.isLimiterEnabled(context)) return

        val serviceIntent = Intent(context, VolumeCapService::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(serviceIntent)
        } else {
            context.startService(serviceIntent)
        }
    }
}
