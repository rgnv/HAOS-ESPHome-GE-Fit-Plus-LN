package dev.rgnv.gefitplus.healthconnect

import android.content.Context
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import org.json.JSONObject
import java.io.File

object SyncConfig {
    private const val PREFS = "sync_config"
    private const val URL = "ha_url"
    private const val TOKEN = "ha_token"
    private const val LAST_MEASUREMENT = "last_measurement_id"
    private const val LAST_GLUCOSE = "last_glucose_key"

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
    fun getLastGlucose(context: Context) = prefs(context).getString(LAST_GLUCOSE, "") ?: ""
    fun setLastGlucose(context: Context, value: String) = prefs(context).edit().putString(LAST_GLUCOSE, value).apply()

    /** Imports a one-shot config staged through a trusted ADB session, then deletes it. */
    fun importAdbConfig(context: Context): Boolean {
        val file = File(context.filesDir, "adb-config.json")
        if (!file.isFile) return false
        return try {
            val json = JSONObject(file.readText())
            val url = json.getString(URL).trim()
            val token = json.getString(TOKEN).trim()
            check(url.isNotEmpty() && token.isNotEmpty())
            save(context, url, token)
            file.delete()
            true
        } finally {
            if (file.exists()) file.delete()
        }
    }
}
