package dev.rgnv.gefitplus.healthconnect

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import androidx.core.content.ContextCompat

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED && intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        if (!SyncConfig.hasConfig(context)) return
        try {
            ContextCompat.startForegroundService(context, Intent(context, SyncService::class.java))
        } catch (e: Exception) {
            Log.e("GeFitHealthConnect", "unable to start continuous sync after boot: ${e.message}")
        }
    }
}
