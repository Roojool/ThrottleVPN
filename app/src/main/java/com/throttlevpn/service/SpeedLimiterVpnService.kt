package com.throttlevpn.service

import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.ParcelFileDescriptor
import android.util.Log
import com.throttlevpn.data.SettingsRepository
import com.throttlevpn.engine.LiveTrafficStats
import com.throttlevpn.engine.TrafficShaper
import com.throttlevpn.engine.Tun2SocksEngine
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

enum class VpnStatus { DISCONNECTED, CONNECTING, CONNECTED, RECONNECTING, ERROR }

class SpeedLimiterVpnService : android.net.VpnService() {

    /* ================================================================ */
    /*  Companion — observable state accessible from any component      */
    /* ================================================================ */
    companion object {
        private val _status       = MutableStateFlow(VpnStatus.DISCONNECTED)
        private val _trafficStats = MutableStateFlow(LiveTrafficStats())
        private val _dlCap        = MutableStateFlow(0.0)
        private val _ulCap        = MutableStateFlow(0.0)

        val status: StateFlow<VpnStatus>          = _status.asStateFlow()
        val trafficStats: StateFlow<LiveTrafficStats> = _trafficStats.asStateFlow()
        val downloadCapMbps: StateFlow<Double>    = _dlCap.asStateFlow()
        val uploadCapMbps: StateFlow<Double>      = _ulCap.asStateFlow()

        /** Convenience: start VPN from any context. */
        fun start(context: Context) {
            context.startForegroundService(
                Intent(context, SpeedLimiterVpnService::class.java)
                    .apply { action = ACTION_START }
            )
        }

        /** Convenience: stop VPN from any context. */
        fun stop(context: Context) {
            context.startService(
                Intent(context, SpeedLimiterVpnService::class.java)
                    .apply { action = ACTION_STOP }
            )
        }

        /** Directly update caps on the running instance (if any). */
        fun applyCaps(dl: Double, ul: Double) {
            instance?.updateCaps(dl, ul)
        }

        const val ACTION_START = "com.throttlevpn.START"
        const val ACTION_STOP  = "com.throttlevpn.STOP"

        @Volatile private var instance: SpeedLimiterVpnService? = null
    }

    /* ================================================================ */
    /*  Instance state                                                  */
    /* ================================================================ */
    private var vpnFd: ParcelFileDescriptor? = null
    private var engine: Tun2SocksEngine? = null
    private var shaper: TrafficShaper? = null
    private var notif: VpnNotificationManager? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var netCb: ConnectivityManager.NetworkCallback? = null
    private lateinit var settings: SettingsRepository

    /* ================================================================ */
    /*  Lifecycle                                                       */
    /* ================================================================ */
    override fun onCreate() {
        super.onCreate()
        instance = this
        settings = SettingsRepository(this)
        notif = VpnNotificationManager(this)
        startForeground(
            VpnNotificationManager.NOTIFICATION_ID,
            notif!!.buildNotification("Initialising…", 0.0, 0.0)
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> doStart()
            ACTION_STOP  -> doStop()
        }
        return START_STICKY
    }

    override fun onRevoke() { doStop(); super.onRevoke() }

    override fun onDestroy() {
        scope.cancel()
        doStop()
        instance = null
        super.onDestroy()
    }

    /* ================================================================ */
    /*  Start / stop                                                    */
    /* ================================================================ */
    private fun doStart() {
        if (_status.value == VpnStatus.CONNECTED) return
        _status.value = VpnStatus.CONNECTING

        scope.launch(Dispatchers.IO) {
            try {
                establish()
                registerNetCb()
                _status.value = VpnStatus.CONNECTED
                applySavedCaps()
                shaper?.startMonitoring(scope)
                collectStats()
            } catch (ex: Exception) {
                Log.e("VpnService", "start failed", ex)
                _status.value = VpnStatus.ERROR
            }
        }
    }

    private fun doStop() {
        _status.value = VpnStatus.DISCONNECTED
        shaper?.stopMonitoring()
        engine?.stop()
        vpnFd?.close()
        vpnFd = null; engine = null; shaper = null
        _dlCap.value = 0.0; _ulCap.value = 0.0

        netCb?.let {
            (getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager)
                .unregisterNetworkCallback(it)
        }
        netCb = null

        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    /* ================================================================ */
    /*  VPN tunnel setup                                                */
    /* ================================================================ */
    private fun establish() {
        vpnFd?.close(); engine?.stop()

        vpnFd = Builder()
            .setSession("ThrottleVPN")
            .setMtu(1500)
            .addAddress("10.0.0.2", 32)
            .addRoute("0.0.0.0", 0)
            .addAddress("fd00::2", 128)
            .addRoute("::", 0)
            .addDnsServer("8.8.8.8")
            .addDnsServer("8.8.4.4")
            .addDnsServer("2001:4860:4860::8888")
            .addDisallowedApplication(packageName)
            .establish() ?: throw IllegalStateException("VPN interface refused")

        engine = Tun2SocksEngine(this)
        shaper = TrafficShaper(engine!!)

        if (!engine!!.start(vpnFd!!.fd)) {
            throw IllegalStateException("Native engine failed to start")
        }
    }

    /* ================================================================ */
    /*  Cap management                                                  */
    /* ================================================================ */
    fun updateCaps(dl: Double, ul: Double) {
        shaper?.updateCaps(dl, ul)
        _dlCap.value = dl; _ulCap.value = ul
        notif?.update("Connected", dl, ul)
    }

    private suspend fun applySavedCaps() {
        val s = settings.getAllSettings()
        val dl = s.manualDownloadCapMbps
            ?: (s.measuredDownloadMbps * s.capPercentage / 100.0)
        val ul = s.manualUploadCapMbps
            ?: (s.measuredUploadMbps * s.capPercentage / 100.0)
        if (dl > 0 && ul > 0) updateCaps(dl, ul)
    }

    /* ================================================================ */
    /*  Network change handling                                         */
    /* ================================================================ */
    private fun registerNetCb() {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val req = NetworkRequest.Builder()
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()

        netCb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                if (_status.value == VpnStatus.RECONNECTING) {
                    scope.launch(Dispatchers.IO) {
                        try {
                            establish()
                            applySavedCaps()
                            shaper?.startMonitoring(scope)
                            _status.value = VpnStatus.CONNECTED
                        } catch (_: Exception) {
                            _status.value = VpnStatus.ERROR
                        }
                    }
                }
            }

            override fun onLost(network: Network) {
                if (_status.value == VpnStatus.CONNECTED) {
                    _status.value = VpnStatus.RECONNECTING
                    shaper?.stopMonitoring()
                }
            }
        }
        cm.registerNetworkCallback(req, netCb!!)
    }

    /* ================================================================ */
    /*  Stats → companion flows & notification                          */
    /* ================================================================ */
    private fun collectStats() {
        scope.launch {
            shaper?.stats?.collect { stats ->
                _trafficStats.value = stats
                withContext(Dispatchers.Main) {
                    notif?.update(
                        "Connected",
                        shaper?.downloadCapMbps ?: 0.0,
                        shaper?.uploadCapMbps ?: 0.0
                    )
                }
            }
        }
    }
}
