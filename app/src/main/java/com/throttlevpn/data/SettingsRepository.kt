package com.throttlevpn.data

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.doublePreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map

private val Context.dataStore: DataStore<Preferences> by preferencesDataStore(
    name = "throttle_vpn_settings"
)

data class AppSettings(
    val capPercentage: Int = 90,
    val manualDownloadCapMbps: Double? = null,
    val manualUploadCapMbps: Double? = null,
    val autoStartOnBoot: Boolean = false,
    val autoReconnect: Boolean = true,
    val backgroundMode: Boolean = true,
    val showNotification: Boolean = true,
    val measuredDownloadMbps: Double = 0.0,
    val measuredUploadMbps: Double = 0.0
)

class SettingsRepository(private val context: Context) {

    /* ---- preference keys ------------------------------------------ */
    private object K {
        val CAP_PCT        = intPreferencesKey("cap_percentage")
        val MANUAL_DL      = doublePreferencesKey("manual_download_cap")
        val MANUAL_UL      = doublePreferencesKey("manual_upload_cap")
        val MANUAL_MODE    = booleanPreferencesKey("manual_mode")
        val AUTO_BOOT      = booleanPreferencesKey("auto_start_on_boot")
        val AUTO_RECONNECT = booleanPreferencesKey("auto_reconnect")
        val BG_MODE        = booleanPreferencesKey("background_mode")
        val SHOW_NOTIF     = booleanPreferencesKey("show_notification")
        val MEAS_DL        = doublePreferencesKey("measured_download_mbps")
        val MEAS_UL        = doublePreferencesKey("measured_upload_mbps")
    }

    /* ---- reactive flows ------------------------------------------- */

    val capPercentage: Flow<Int> = context.dataStore.data.map { it[K.CAP_PCT] ?: 90 }

    val manualDownloadCap: Flow<Double?> = context.dataStore.data.map { prefs ->
        if (prefs[K.MANUAL_MODE] == true) prefs[K.MANUAL_DL] else null
    }

    val manualUploadCap: Flow<Double?> = context.dataStore.data.map { prefs ->
        if (prefs[K.MANUAL_MODE] == true) prefs[K.MANUAL_UL] else null
    }

    val autoStartOnBoot: Flow<Boolean> = context.dataStore.data.map { it[K.AUTO_BOOT] ?: false }
    val autoReconnect: Flow<Boolean>   = context.dataStore.data.map { it[K.AUTO_RECONNECT] ?: true }
    val backgroundMode: Flow<Boolean>  = context.dataStore.data.map { it[K.BG_MODE] ?: true }
    val showNotification: Flow<Boolean> = context.dataStore.data.map { it[K.SHOW_NOTIF] ?: true }

    val measuredDownloadMbps: Flow<Double> = context.dataStore.data.map { it[K.MEAS_DL] ?: 0.0 }
    val measuredUploadMbps: Flow<Double>   = context.dataStore.data.map { it[K.MEAS_UL] ?: 0.0 }

    /* ---- suspend setters ------------------------------------------ */

    suspend fun setCapPercentage(value: Int) {
        context.dataStore.edit { it[K.CAP_PCT] = value.coerceIn(80, 95) }
    }

    suspend fun setManualDownloadCap(mbps: Double?) {
        context.dataStore.edit { prefs ->
            if (mbps != null) {
                prefs[K.MANUAL_DL] = mbps
                prefs[K.MANUAL_MODE] = true
            } else {
                prefs.remove(K.MANUAL_DL)
                if (prefs[K.MANUAL_UL] == null) prefs[K.MANUAL_MODE] = false
            }
        }
    }

    suspend fun setManualUploadCap(mbps: Double?) {
        context.dataStore.edit { prefs ->
            if (mbps != null) {
                prefs[K.MANUAL_UL] = mbps
                prefs[K.MANUAL_MODE] = true
            } else {
                prefs.remove(K.MANUAL_UL)
                if (prefs[K.MANUAL_DL] == null) prefs[K.MANUAL_MODE] = false
            }
        }
    }

    suspend fun setAutoStartOnBoot(on: Boolean) { context.dataStore.edit { it[K.AUTO_BOOT] = on } }
    suspend fun setAutoReconnect(on: Boolean)   { context.dataStore.edit { it[K.AUTO_RECONNECT] = on } }
    suspend fun setBackgroundMode(on: Boolean)  { context.dataStore.edit { it[K.BG_MODE] = on } }
    suspend fun setShowNotification(on: Boolean){ context.dataStore.edit { it[K.SHOW_NOTIF] = on } }

    suspend fun setMeasuredSpeeds(dl: Double, ul: Double) {
        context.dataStore.edit {
            it[K.MEAS_DL] = dl
            it[K.MEAS_UL] = ul
        }
    }

    /** One-shot snapshot of all settings. */
    suspend fun getAllSettings(): AppSettings {
        val p = context.dataStore.data.first()
        val manual = p[K.MANUAL_MODE] ?: false
        return AppSettings(
            capPercentage = p[K.CAP_PCT] ?: 90,
            manualDownloadCapMbps = if (manual) p[K.MANUAL_DL] else null,
            manualUploadCapMbps   = if (manual) p[K.MANUAL_UL] else null,
            autoStartOnBoot = p[K.AUTO_BOOT] ?: false,
            autoReconnect   = p[K.AUTO_RECONNECT] ?: true,
            backgroundMode  = p[K.BG_MODE] ?: true,
            showNotification = p[K.SHOW_NOTIF] ?: true,
            measuredDownloadMbps = p[K.MEAS_DL] ?: 0.0,
            measuredUploadMbps   = p[K.MEAS_UL] ?: 0.0
        )
    }
}
