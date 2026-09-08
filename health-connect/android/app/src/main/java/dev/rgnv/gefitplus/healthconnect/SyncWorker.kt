package dev.rgnv.gefitplus.healthconnect

import android.content.Context
import androidx.health.connect.client.HealthConnectClient
import androidx.health.connect.client.permission.HealthPermission
import androidx.health.connect.client.records.WeightRecord
import androidx.health.connect.client.records.metadata.Metadata
import androidx.health.connect.client.units.Mass
import androidx.work.CoroutineWorker
import androidx.work.WorkerParameters
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.time.Instant
import java.time.ZoneOffset

class SyncWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result = try {
        sync(applicationContext)
        Result.success()
    } catch (_: MissingPermissionException) {
        Result.failure()
    } catch (_: Exception) {
        Result.retry()
    }

    private suspend fun sync(context: Context) = withContext(Dispatchers.IO) {
        val base = SyncConfig.getUrl(context)
        val token = SyncConfig.getToken(context)
        check(base.isNotEmpty() && token.isNotEmpty()) { "sync is not configured" }
        check(HealthConnectClient.getSdkStatus(context, PROVIDER) == HealthConnectClient.SDK_AVAILABLE) {
            "Health Connect is unavailable"
        }
        val client = HealthConnectClient.getOrCreate(context)
        val writePermission = HealthPermission.getWritePermission(WeightRecord::class)
        check(client.permissionController.getGrantedPermissions().contains(writePermission)) {
            throw MissingPermissionException()
        }

        val measurement = getJson(base, token, "/api/states/sensor.ge_fit_plus_ln_measurement_id")
        val measurementId = measurement.optString("state", "")
        if (measurementId.isEmpty() || measurementId == SyncConfig.getLastMeasurement(context)) return@withContext

        val weight = getJson(base, token, "/api/states/sensor.ge_fit_plus_ln_weight")
        val kilograms = weight.getString("state").toDouble()
        val attrs = weight.optJSONObject("attributes")
        val time = attrs?.optString("measurement_time", "")?.takeIf { it.isNotEmpty() }?.let(Instant::parse)
            ?: Instant.now()
        val record = WeightRecord(
            time = time,
            zoneOffset = ZoneOffset.UTC,
            weight = Mass.kilograms(kilograms),
            metadata = Metadata.manualEntry()
        )
        client.insertRecords(listOf(record))
        SyncConfig.setLastMeasurement(context, measurementId)
    }

    private fun getJson(base: String, token: String, path: String): JSONObject {
        val connection = URL(base.trimEnd('/') + path).openConnection() as HttpURLConnection
        connection.connectTimeout = 15_000
        connection.readTimeout = 15_000
        connection.setRequestProperty("Authorization", "Bearer $token")
        connection.setRequestProperty("Accept", "application/json")
        return try {
            check(connection.responseCode < 400) { "HA HTTP ${connection.responseCode}" }
            JSONObject(connection.inputStream.bufferedReader().use { it.readText() })
        } finally {
            connection.disconnect()
        }
    }

    private class MissingPermissionException : Exception()

    companion object {
        private const val PROVIDER = "com.google.android.healthconnect.controller"
    }
}
