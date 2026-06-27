package com.throttlevpn.ui

import android.os.Bundle
import android.widget.ImageButton
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.switchmaterial.SwitchMaterial
import com.throttlevpn.R
import com.throttlevpn.data.SettingsRepository
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

class SettingsActivity : AppCompatActivity() {

    private lateinit var repo: SettingsRepository

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)
        repo = SettingsRepository(this)

        val swBoot   = findViewById<SwitchMaterial>(R.id.switchAutoStart)
        val swRecon  = findViewById<SwitchMaterial>(R.id.switchAutoReconnect)
        val swBg     = findViewById<SwitchMaterial>(R.id.switchBackground)
        val swNotif  = findViewById<SwitchMaterial>(R.id.switchNotification)
        val btnBack  = findViewById<ImageButton>(R.id.btnBack)

        lifecycleScope.launch {
            swBoot.isChecked  = repo.autoStartOnBoot.first()
            swRecon.isChecked = repo.autoReconnect.first()
            swBg.isChecked    = repo.backgroundMode.first()
            swNotif.isChecked = repo.showNotification.first()
        }

        swBoot.setOnCheckedChangeListener  { _, c -> lifecycleScope.launch { repo.setAutoStartOnBoot(c) } }
        swRecon.setOnCheckedChangeListener { _, c -> lifecycleScope.launch { repo.setAutoReconnect(c) } }
        swBg.setOnCheckedChangeListener    { _, c -> lifecycleScope.launch { repo.setBackgroundMode(c) } }
        swNotif.setOnCheckedChangeListener { _, c -> lifecycleScope.launch { repo.setShowNotification(c) } }

        btnBack.setOnClickListener { finish() }
    }
}
