package com.throttlevpn.receiver

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.throttlevpn.data.SettingsRepository
import com.throttlevpn.service.SpeedLimiterVpnService
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        val autoStart = runBlocking {
            SettingsRepository(context).autoStartOnBoot.first()
        }
        if (autoStart) {
            SpeedLimiterVpnService.start(context)
        }
    }
}
