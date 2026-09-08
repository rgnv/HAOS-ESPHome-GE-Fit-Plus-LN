package dev.rgnv.gefitplus.healthconnect

import android.content.Context
import android.util.Log
import androidx.health.connect.client.HealthConnectClient
import androidx.health.connect.client.permission.HealthPermission
import androidx.health.connect.client.records.BloodGlucoseRecord
import androidx.health.connect.client.records.WeightRecord
import androidx.health.connect.client.records.metadata.Metadata
import androidx.health.connect.client.request.ReadRecordsRequest
import androidx.health.connect.client.time.TimeRangeFilter
import androidx.health.connect.client.units.BloodGlucose
import androidx.health.connect.client.units.Mass
import androidx.work.CoroutineWorker
import androidx.work.WorkerParameters
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.time.Instant
import java.time.ZoneId
import java.time.ZoneOffset

class SyncWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result = try {
        SyncEngine.syncOnce(applicationContext)
        Log.i(TAG, "sync completed")
        Result.success()
    } catch (_: SyncEngine.MissingPermissionException) {
        Log.e(TAG, "sync blocked: Health Connect write permission is not granted")
        Result.failure()
    } catch (e: Exception) {
        Log.e(TAG, "sync failed: ${e.javaClass.simpleName}: ${e.message}")
        Result.retry()
    }

    companion object {
        private const val TAG = "GeFitHealthConnect"
    }
}

internal object SyncEngine {
    suspend fun syncOnce(context: Context) = withContext(Dispatchers.IO) {
        val base = SyncConfig.getUrl(context)
        val token = SyncConfig.getToken(context)
        check(base.isNotEmpty() && token.isNotEmpty()) { "sync is not configured" }
        check(HealthConnectClient.getSdkStatus(context, PROVIDER) == HealthConnectClient.SDK_AVAILABLE) {
            "Health Connect is unavailable"
        }
        val client = HealthConnectClient.getOrCreate(context)
        val writeWeight = HealthPermission.getWritePermission(WeightRecord::class)
        val writeGlucose = HealthPermission.getWritePermission(BloodGlucoseRecord::class)
        val granted = client.permissionController.getGrantedPermissions()
        check(granted.contains(writeWeight) && granted.contains(writeGlucose)) {
            throw MissingPermissionException()
        }

        syncWeight(context, base, token, client, granted)
        syncGlucose(context, base, token, client, granted)
    }

    private suspend fun syncWeight(
        context: Context,
        base: String,
        token: String,
        client: HealthConnectClient,
        granted: Set<String>
    ) {
        val measurement = getJson(base, token, "/api/states/sensor.ge_fit_plus_ln_measurement_id")
        val measurementId = measurement.optString("state", "")
        if (measurementId.isEmpty() || measurementId == SyncConfig.getLastMeasurement(context)) {
            verifyRecentWeight(client, granted)
            Log.i(TAG, "no new measurement")
            return
        }

        val weight = getJson(base, token, "/api/states/sensor.ge_fit_plus_ln_weight")
        val kilograms = weight.getString("state").toDouble()
        val attrs = weight.optJSONObject("attributes")
        val time = attrs?.optString("measurement_time", "")?.takeIf { it.isNotEmpty() }?.let(Instant::parse)
            ?: Instant.now()
        val record = WeightRecord(
            time = time,
            zoneOffset = healthOffset(time),
            weight = Mass.kilograms(kilograms),
            metadata = Metadata.manualEntry()
        )
        client.insertRecords(listOf(record))
        SyncConfig.setLastMeasurement(context, measurementId)
        Log.i(TAG, "wrote WeightRecord measurement_id=$measurementId offset=${healthOffset(time)}")
        verifyRecentWeight(client, granted)
    }

    private suspend fun syncGlucose(
        context: Context,
        base: String,
        token: String,
        client: HealthConnectClient,
        granted: Set<String>
    ) {
        val glucose = getJson(base, token, "/api/states/sensor.gluroo_blood_glucose")
        val value = glucose.optString("state", "").toDoubleOrNull()
        if (value == null) {
            Log.i(TAG, "glucose unavailable")
            return
        }
        val attrs = glucose.optJSONObject("attributes")
        val measuredAt = attrs?.optString("measured_at", "")?.takeIf { it.isNotEmpty() }
            ?: glucose.optString("last_updated", "")
        val time = measuredAt.takeIf { it.isNotEmpty() }?.let(Instant::parse) ?: Instant.now()
        val key = "${time}|$value"
        if (key == SyncConfig.getLastGlucose(context)) {
            verifyRecentGlucose(client, granted)
            Log.i(TAG, "no new glucose measurement")
            return
        }
        val record = BloodGlucoseRecord(
            time = time,
            zoneOffset = healthOffset(time),
            metadata = Metadata.manualEntry(),
            level = BloodGlucose.milligramsPerDeciliter(value),
            specimenSource = BloodGlucoseRecord.SPECIMEN_SOURCE_INTERSTITIAL_FLUID,
            mealType = 0,
            relationToMeal = BloodGlucoseRecord.RELATION_TO_MEAL_UNKNOWN
        )
        client.insertRecords(listOf(record))
        SyncConfig.setLastGlucose(context, key)
        Log.i(TAG, "wrote BloodGlucoseRecord measured_at=$time offset=${healthOffset(time)} value_mgdl=$value")
        verifyRecentGlucose(client, granted)
    }

    private suspend fun verifyRecentWeight(client: HealthConnectClient, granted: Set<String>) {
        val readPermission = HealthPermission.getReadPermission(WeightRecord::class)
        if (!granted.contains(readPermission)) {
            Log.i(TAG, "readback skipped: READ_WEIGHT permission is not granted")
            return
        }
        val response = client.readRecords(
            ReadRecordsRequest(WeightRecord::class, TimeRangeFilter.after(Instant.now().minusSeconds(86_400)))
        )
        val latest = response.records.maxByOrNull { it.time }
        Log.i(TAG, "verified Health Connect WeightRecord count=${response.records.size} latest_offset=${latest?.zoneOffset}")
    }

    private suspend fun verifyRecentGlucose(client: HealthConnectClient, granted: Set<String>) {
        val readPermission = HealthPermission.getReadPermission(BloodGlucoseRecord::class)
        if (!granted.contains(readPermission)) {
            Log.i(TAG, "glucose readback skipped: READ_BLOOD_GLUCOSE permission is not granted")
            return
        }
        val response = client.readRecords(
            ReadRecordsRequest(BloodGlucoseRecord::class, TimeRangeFilter.after(Instant.now().minusSeconds(86_400)))
        )
        val latest = response.records.maxByOrNull { it.time }
        Log.i(TAG, "verified Health Connect BloodGlucoseRecord count=${response.records.size} latest_offset=${latest?.zoneOffset}")
    }

    private fun healthOffset(time: Instant): ZoneOffset = HEALTH_ZONE.rules.getOffset(time)

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

    class MissingPermissionException : Exception()

    private const val TAG = "GeFitHealthConnect"
    private const val PROVIDER = "com.google.android.healthconnect.controller"
    private val HEALTH_ZONE: ZoneId = ZoneId.of("America/Los_Angeles")
}
