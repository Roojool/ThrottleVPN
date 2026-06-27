package com.throttlevpn.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.VpnService
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import com.google.android.material.slider.Slider
import com.google.android.material.switchmaterial.SwitchMaterial
import com.throttlevpn.R
import com.throttlevpn.service.VpnStatus
import com.throttlevpn.speed.SpeedTestState
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    private lateinit var vm: MainViewModel

    /* ---- UI handles ------------------------------------------------ */
    private lateinit var statusDot: View
    private lateinit var tvStatus: TextView
    private lateinit var btnStart: Button
    private lateinit var btnStop: Button
    private lateinit var btnSpeedTest: Button
    private lateinit var tvMeasDl: TextView
    private lateinit var tvMeasUl: TextView
    private lateinit var tvCapDl: TextView
    private lateinit var tvCapUl: TextView
    private lateinit var tvLiveDl: TextView
    private lateinit var tvLiveUl: TextView
    private lateinit var slider: Slider
    private lateinit var tvPct: TextView
    private lateinit var swManual: SwitchMaterial
    private lateinit var etDl: EditText
    private lateinit var etUl: EditText
    private lateinit var btnApply: Button
    private lateinit var manualBox: LinearLayout
    private lateinit var progress: ProgressBar
    private lateinit var tvTestStatus: TextView
    private lateinit var btnSettings: ImageButton
    private lateinit var btnReset: Button

    /* ---- permission launchers ------------------------------------- */
    private val vpnLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { if (it.resultCode == RESULT_OK) vm.startVpn() }

    private val notifLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { /* proceed regardless */ }

    /* ================================================================ */

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        vm = ViewModelProvider(this)[MainViewModel::class.java]

        bind()
        wire()
        observe()
        askNotifPermission()
    }

    private fun bind() {
        statusDot    = findViewById(R.id.statusIndicator)
        tvStatus     = findViewById(R.id.tvStatus)
        btnStart     = findViewById(R.id.btnStart)
        btnStop      = findViewById(R.id.btnStop)
        btnSpeedTest = findViewById(R.id.btnSpeedTest)
        tvMeasDl     = findViewById(R.id.tvMeasuredDown)
        tvMeasUl     = findViewById(R.id.tvMeasuredUp)
        tvCapDl      = findViewById(R.id.tvCapDown)
        tvCapUl      = findViewById(R.id.tvCapUp)
        tvLiveDl     = findViewById(R.id.tvLiveDown)
        tvLiveUl     = findViewById(R.id.tvLiveUp)
        slider       = findViewById(R.id.sliderPercentage)
        tvPct        = findViewById(R.id.tvPercentage)
        swManual     = findViewById(R.id.switchManual)
        etDl         = findViewById(R.id.etManualDown)
        etUl         = findViewById(R.id.etManualUp)
        btnApply     = findViewById(R.id.btnApplyManual)
        manualBox    = findViewById(R.id.manualContainer)
        progress     = findViewById(R.id.progressSpeedTest)
        tvTestStatus = findViewById(R.id.tvSpeedTestStatus)
        btnSettings  = findViewById(R.id.btnSettings)
        btnReset     = findViewById(R.id.btnResetSpeeds)
    }

    private fun wire() {
        btnStart.setOnClickListener {
            val prep = VpnService.prepare(this)
            if (prep != null) vpnLauncher.launch(prep) else vm.startVpn()
        }
        btnStop.setOnClickListener { vm.stopVpn() }
        btnSpeedTest.setOnClickListener { vm.runSpeedTest() }
        btnReset.setOnClickListener {
            vm.resetSpeeds()
            Toast.makeText(this, "Speeds reset", Toast.LENGTH_SHORT).show()
        }

        slider.addOnChangeListener { _, v, fromUser ->
            if (fromUser) {
                val p = v.toInt()
                tvPct.text = getString(R.string.pct_fmt, p)
                vm.setCapPercentage(p)
            }
        }

        swManual.setOnCheckedChangeListener { _, on ->
            manualBox.visibility = if (on) View.VISIBLE else View.GONE
            if (!on) { vm.setManualDownloadCap(null); vm.setManualUploadCap(null) }
        }

        btnApply.setOnClickListener {
            etDl.text.toString().toDoubleOrNull()?.takeIf { it > 0 }
                ?.let { vm.setManualDownloadCap(it) }
            etUl.text.toString().toDoubleOrNull()?.takeIf { it > 0 }
                ?.let { vm.setManualUploadCap(it) }
            vm.applyCaps()
            Toast.makeText(this, "Caps applied", Toast.LENGTH_SHORT).show()
        }

        btnSettings.setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }
    }

    private fun observe() {
        lifecycleScope.launch {
            vm.uiState.collect { s ->
                /* ---- status ---------------------------------------- */
                when (s.vpnStatus) {
                    VpnStatus.CONNECTED -> {
                        statusDot.setBackgroundResource(R.drawable.bg_status_connected)
                        tvStatus.text = getString(R.string.status_connected)
                        btnStart.isEnabled = false; btnStop.isEnabled = true
                    }
                    VpnStatus.CONNECTING, VpnStatus.RECONNECTING -> {
                        statusDot.setBackgroundResource(R.drawable.bg_status_connecting)
                        tvStatus.text = if (s.vpnStatus == VpnStatus.RECONNECTING)
                            getString(R.string.status_reconnecting) else getString(R.string.status_connecting)
                        btnStart.isEnabled = false; btnStop.isEnabled = true
                    }
                    VpnStatus.DISCONNECTED, VpnStatus.ERROR -> {
                        statusDot.setBackgroundResource(R.drawable.bg_status_disconnected)
                        tvStatus.text = if (s.vpnStatus == VpnStatus.ERROR)
                            getString(R.string.status_error) else getString(R.string.status_disconnected)
                        btnStart.isEnabled = true; btnStop.isEnabled = false
                    }
                }

                /* ---- bandwidth ------------------------------------- */
                tvMeasDl.text = getString(R.string.mbps_fmt, s.measuredDownloadMbps)
                tvMeasUl.text = getString(R.string.mbps_fmt, s.measuredUploadMbps)
                tvCapDl.text  = getString(R.string.mbps_fmt, s.downloadCapMbps)
                tvCapUl.text  = getString(R.string.mbps_fmt, s.uploadCapMbps)

                val liveDl = s.trafficStats.downloadBytesPerSec * 8.0 / 1_000_000.0
                val liveUl = s.trafficStats.uploadBytesPerSec * 8.0 / 1_000_000.0
                tvLiveDl.text = getString(R.string.mbps_fmt2, liveDl)
                tvLiveUl.text = getString(R.string.mbps_fmt2, liveUl)

                slider.value = s.capPercentage.toFloat()
                tvPct.text = getString(R.string.pct_fmt, s.capPercentage)

                if (s.manualDownloadCap != null && !swManual.isChecked) {
                    swManual.isChecked = true
                    manualBox.visibility = View.VISIBLE
                }

                /* ---- speed test ------------------------------------ */
                when (val t = s.speedTestState) {
                    is SpeedTestState.Idle -> {
                        progress.visibility = View.GONE
                        tvTestStatus.visibility = View.GONE
                        btnSpeedTest.isEnabled = true
                    }
                    is SpeedTestState.Testing -> {
                        progress.visibility = View.VISIBLE
                        tvTestStatus.visibility = View.VISIBLE
                        tvTestStatus.text = t.phase
                        progress.progress = (t.progress * 100).toInt()
                        btnSpeedTest.isEnabled = false
                    }
                    is SpeedTestState.Complete -> {
                        progress.visibility = View.GONE
                        tvTestStatus.visibility = View.VISIBLE
                        tvTestStatus.text = getString(R.string.speed_result,
                            t.result.downloadMbps, t.result.uploadMbps)
                        btnSpeedTest.isEnabled = true
                    }
                    is SpeedTestState.Error -> {
                        progress.visibility = View.GONE
                        tvTestStatus.visibility = View.VISIBLE
                        tvTestStatus.text = getString(R.string.speed_error, t.message)
                        btnSpeedTest.isEnabled = true
                    }
                }
            }
        }
    }

    private fun askNotifPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ActivityCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
            != PackageManager.PERMISSION_GRANTED
        ) {
            notifLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }
}
