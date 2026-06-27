package com.throttlevpn.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationCompat
import com.throttlevpn.R
import com.throttlevpn.ui.MainActivity

class VpnNotificationManager(private val ctx: Context) {

    companion object {
        const val CHANNEL_ID      = "throttle_vpn_channel"
        const val NOTIFICATION_ID = 1
    }

    private val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    init { createChannel() }

    private fun createChannel() {
        val ch = NotificationChannel(
            CHANNEL_ID,
            "ThrottleVPN Service",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Persistent notification while the VPN is active"
            setShowBadge(false)
        }
        nm.createNotificationChannel(ch)
    }

    fun buildNotification(
        status: String,
        dlCap: Double,
        ulCap: Double
    ): Notification {
        val openIntent = PendingIntent.getActivity(
            ctx, 0,
            Intent(ctx, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_SINGLE_TOP
            },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val stopIntent = PendingIntent.getService(
            ctx, 1,
            Intent(ctx, SpeedLimiterVpnService::class.java).apply {
                action = SpeedLimiterVpnService.ACTION_STOP
            },
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val capText = if (dlCap > 0 || ulCap > 0)
            "↓ %.1f Mbps  ↑ %.1f Mbps".format(dlCap, ulCap)
        else "No cap applied"

        return NotificationCompat.Builder(ctx, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_vpn_shield)
            .setContentTitle("ThrottleVPN — $status")
            .setContentText(capText)
            .setContentIntent(openIntent)
            .addAction(R.drawable.ic_vpn_shield, "Stop", stopIntent)
            .setOngoing(true)
            .setSilent(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .build()
    }

    fun update(status: String, dlCap: Double, ulCap: Double) {
        nm.notify(NOTIFICATION_ID, buildNotification(status, dlCap, ulCap))
    }
}
