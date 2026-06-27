package com.throttlevpn.engine

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Live throughput monitor that wraps [Tun2SocksEngine] and periodically
 * samples the cumulative byte counters to derive bytes-per-second rates.
 */
data class LiveTrafficStats(
    val uploadBytesPerSec: Long = 0L,
    val downloadBytesPerSec: Long = 0L,
    val totalUploadBytes: Long = 0L,
    val totalDownloadBytes: Long = 0L
)

class TrafficShaper(private val engine: Tun2SocksEngine) {

    private val _stats = MutableStateFlow(LiveTrafficStats())
    val stats: StateFlow<LiveTrafficStats> = _stats.asStateFlow()

    private var monitorJob: Job? = null
    private var prevUl = 0L
    private var prevDl = 0L

    var downloadCapMbps: Double = 0.0
        private set
    var uploadCapMbps: Double = 0.0
        private set

    /** Push new caps to the native engine. */
    fun updateCaps(downloadMbps: Double, uploadMbps: Double) {
        downloadCapMbps = downloadMbps
        uploadCapMbps = uploadMbps
        engine.setRateLimit(downloadMbps, uploadMbps)
    }

    /** Start sampling traffic counters every 500 ms. */
    fun startMonitoring(scope: CoroutineScope) {
        val snap = engine.getStats()
        prevUl = snap.uploadBytes
        prevDl = snap.downloadBytes

        monitorJob = scope.launch(Dispatchers.IO) {
            while (isActive) {
                delay(500)
                val cur = engine.getStats()
                val ulDelta = cur.uploadBytes - prevUl
                val dlDelta = cur.downloadBytes - prevDl

                _stats.value = LiveTrafficStats(
                    uploadBytesPerSec = ulDelta * 2,        /* 500 ms → 1 s */
                    downloadBytesPerSec = dlDelta * 2,
                    totalUploadBytes = cur.uploadBytes,
                    totalDownloadBytes = cur.downloadBytes
                )
                prevUl = cur.uploadBytes
                prevDl = cur.downloadBytes
            }
        }
    }

    fun stopMonitoring() {
        monitorJob?.cancel()
        monitorJob = null
    }
}
