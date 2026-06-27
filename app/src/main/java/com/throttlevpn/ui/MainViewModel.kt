package com.throttlevpn.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.throttlevpn.data.SettingsRepository
import com.throttlevpn.engine.LiveTrafficStats
import com.throttlevpn.service.SpeedLimiterVpnService
import com.throttlevpn.service.VpnStatus
import com.throttlevpn.speed.SpeedTestManager
import com.throttlevpn.speed.SpeedTestState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

data class VpnUiState(
    val vpnStatus: VpnStatus = VpnStatus.DISCONNECTED,
    val trafficStats: LiveTrafficStats = LiveTrafficStats(),
    val downloadCapMbps: Double = 0.0,
    val uploadCapMbps: Double = 0.0,
    val capPercentage: Int = 90,
    val measuredDownloadMbps: Double = 0.0,
    val measuredUploadMbps: Double = 0.0,
    val speedTestState: SpeedTestState = SpeedTestState.Idle,
    val manualDownloadCap: Double? = null,
    val manualUploadCap: Double? = null
)

class MainViewModel(app: Application) : AndroidViewModel(app) {

    val settings = SettingsRepository(app)
    val speedTest = SpeedTestManager()

    private val _ui = MutableStateFlow(VpnUiState())
    val uiState: StateFlow<VpnUiState> = _ui.asStateFlow()

    init {
        observe(SpeedLimiterVpnService.status)          { s -> _ui.update { it.copy(vpnStatus = s) } }
        observe(SpeedLimiterVpnService.trafficStats)    { s -> _ui.update { it.copy(trafficStats = s) } }
        observe(SpeedLimiterVpnService.downloadCapMbps) { v -> _ui.update { it.copy(downloadCapMbps = v) } }
        observe(SpeedLimiterVpnService.uploadCapMbps)   { v -> _ui.update { it.copy(uploadCapMbps = v) } }
        observe(settings.capPercentage)                 { v -> _ui.update { it.copy(capPercentage = v) } }
        observe(settings.measuredDownloadMbps)          { v -> _ui.update { it.copy(measuredDownloadMbps = v) } }
        observe(settings.measuredUploadMbps)            { v -> _ui.update { it.copy(measuredUploadMbps = v) } }
        observe(settings.manualDownloadCap)             { v -> _ui.update { it.copy(manualDownloadCap = v) } }
        observe(settings.manualUploadCap)               { v -> _ui.update { it.copy(manualUploadCap = v) } }
        observe(speedTest.state)                        { v -> _ui.update { it.copy(speedTestState = v) } }
    }

    private fun <T> observe(flow: kotlinx.coroutines.flow.Flow<T>, action: (T) -> Unit) {
        viewModelScope.launch { flow.collect(action) }
    }

    /* ---- actions -------------------------------------------------- */

    fun startVpn() = SpeedLimiterVpnService.start(getApplication())
    fun stopVpn()  = SpeedLimiterVpnService.stop(getApplication())

    fun runSpeedTest() {
        viewModelScope.launch {
            val res = speedTest.runSpeedTest()
            if (res.downloadMbps > 0) {
                settings.setMeasuredSpeeds(res.downloadMbps, res.uploadMbps)
                val pct = _ui.value.capPercentage
                recalcCaps(res.downloadMbps, res.uploadMbps, pct)
            }
        }
    }

    fun setCapPercentage(pct: Int) {
        viewModelScope.launch {
            settings.setCapPercentage(pct)
            val s = _ui.value
            if (s.manualDownloadCap == null && s.measuredDownloadMbps > 0) {
                recalcCaps(s.measuredDownloadMbps, s.measuredUploadMbps, pct)
            }
        }
    }

    fun setManualDownloadCap(mbps: Double?) {
        viewModelScope.launch {
            settings.setManualDownloadCap(mbps)
            val s = _ui.value
            val dl = mbps ?: (s.measuredDownloadMbps * s.capPercentage / 100.0)
            _ui.update { it.copy(downloadCapMbps = dl, manualDownloadCap = mbps) }
        }
    }

    fun setManualUploadCap(mbps: Double?) {
        viewModelScope.launch {
            settings.setManualUploadCap(mbps)
            val s = _ui.value
            val ul = mbps ?: (s.measuredUploadMbps * s.capPercentage / 100.0)
            _ui.update { it.copy(uploadCapMbps = ul, manualUploadCap = mbps) }
        }
    }

    /** Push current caps to the running VPN service. */
    fun applyCaps() {
        val s = _ui.value
        SpeedLimiterVpnService.applyCaps(s.downloadCapMbps, s.uploadCapMbps)
    }

    /** Reset all measured and manual speed values to zero. */
    fun resetSpeeds() {
        viewModelScope.launch {
            settings.setMeasuredSpeeds(0.0, 0.0)
            settings.setManualDownloadCap(null)
            settings.setManualUploadCap(null)
            speedTest.reset()
            _ui.update {
                it.copy(
                    measuredDownloadMbps = 0.0,
                    measuredUploadMbps = 0.0,
                    downloadCapMbps = 0.0,
                    uploadCapMbps = 0.0,
                    manualDownloadCap = null,
                    manualUploadCap = null,
                    speedTestState = SpeedTestState.Idle
                )
            }
        }
    }

    private fun recalcCaps(dlMax: Double, ulMax: Double, pct: Int) {
        val dl = dlMax * pct / 100.0
        val ul = ulMax * pct / 100.0
        _ui.update { it.copy(downloadCapMbps = dl, uploadCapMbps = ul,
                              measuredDownloadMbps = dlMax, measuredUploadMbps = ulMax) }
    }
}
