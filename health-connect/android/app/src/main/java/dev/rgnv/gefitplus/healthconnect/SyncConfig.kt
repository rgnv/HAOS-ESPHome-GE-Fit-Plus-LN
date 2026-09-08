package dev.rgnv.gefitplus.healthconnect

import android.content.Context
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey

object SyncConfig {
    private const val PREFS = "sync_config"
    private const val URL = "ha_url"
    private const val TOKEN = "ha_token"
    private const val LAST_MEASUREMENT = "last_measurement_id"

    private fun prefs(context: Context): android.content.SharedPreferences {
        val key = MasterKey.Builder(context).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build()
        return EncryptedSharedPreferences.create(
            context,
            PREFS,
            key,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
        )
    }

    fun save(context: Context, url: String, token: String) =
        prefs(context).edit().putString(URL, url).putString(TOKEN, token).apply()

    fun getUrl(context: Context) = prefs(context).getString(URL, "") ?: ""
    fun getToken(context: Context) = prefs(context).getString(TOKEN, "") ?: ""
    fun hasConfig(context: Context) = getUrl(context).isNotEmpty() && getToken(context).isNotEmpty()
    fun getLastMeasurement(context: Context) = prefs(context).getString(LAST_MEASUREMENT, "") ?: ""
    fun setLastMeasurement(context: Context, value: String) = prefs(context).edit().putString(LAST_MEASUREMENT, value).apply()
}
