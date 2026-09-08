package dev.rgnv.gefitplus.healthconnect

import android.os.Bundle
import android.text.InputType
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.health.connect.client.PermissionController
import androidx.health.connect.client.permission.HealthPermission
import androidx.health.connect.client.records.WeightRecord
import androidx.health.connect.client.records.BloodGlucoseRecord
import androidx.work.Constraints
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequest
import androidx.work.PeriodicWorkRequest
import androidx.work.WorkManager
import java.util.concurrent.TimeUnit

class MainActivity : AppCompatActivity() {
    private lateinit var urlInput: EditText
    private lateinit var tokenInput: EditText
    private lateinit var status: TextView
    private val permissionLauncher = registerForActivityResult(
        PermissionController.createRequestPermissionResultContract(
            "com.google.android.healthconnect.controller"
        )
    ) { granted: Set<String> ->
        if (granted.contains(HealthPermission.getWritePermission(WeightRecord::class))) {
            showStatus("Weight write permission granted. Tap Sync now to test.")
        } else {
            showStatus("Health Connect weight permission was not granted.")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val p = dp(20)
            setPadding(p, p, p, p)
        }
        content.addView(TextView(this).apply {
            text = "GE Fit Plus → Google Health Connect"
            textSize = 22f
        }, matchWrap())
        content.addView(TextView(this).apply {
            text = "This Android bridge polls Home Assistant for new scale and Gluroo glucose measurements and writes them to Health Connect. The HA token stays in encrypted app-private storage."
            setPadding(0, dp(12), 0, dp(12))
        }, matchWrap())
        urlInput = EditText(this).apply {
            hint = "HA URL, e.g. https://haos.rgnv.dev"
            setSingleLine(true)
        }
        content.addView(urlInput, matchWrap())
        tokenInput = EditText(this).apply {
            hint = "HA long-lived token"
            setSingleLine(true)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        content.addView(tokenInput, marginParams(0, 10, 0, 10))
        content.addView(Button(this).apply {
            text = "Save and enable periodic sync"
            setOnClickListener { saveAndSchedule() }
        }, matchWrap())
        content.addView(Button(this).apply {
            text = "Grant Health Connect weight permission"
            setOnClickListener { requestPermissions() }
        }, marginParams(0, 10, 0, 0))
        content.addView(Button(this).apply {
            text = "Sync now"
            setOnClickListener { syncNow() }
        }, matchWrap())
        status = TextView(this).apply { setPadding(0, dp(14), 0, 0) }
        content.addView(status, matchWrap())
        setContentView(ScrollView(this).apply { addView(content) })
        val imported = SyncConfig.importAdbConfig(this)
        urlInput.setText(SyncConfig.getUrl(this))
        tokenInput.setText(SyncConfig.getToken(this))
        showStatus(if (imported) "HA settings imported securely. Save to enable periodic sync." else "Ready. Save settings, grant Health Connect permission, then sync.")
    }

    private fun saveAndSchedule() {
        val url = urlInput.text.toString().trim()
        val token = tokenInput.text.toString().trim()
        if (url.isEmpty() || token.isEmpty()) {
            showStatus("Enter both the HA URL and long-lived token.")
            return
        }
        SyncConfig.save(this, url, token)
        val constraints = Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build()
        val request = PeriodicWorkRequest.Builder(SyncWorker::class.java, 15, TimeUnit.MINUTES)
            .setConstraints(constraints).build()
        WorkManager.getInstance(this).enqueueUniquePeriodicWork(
            "ge_fit_plus_health_connect_periodic", ExistingPeriodicWorkPolicy.UPDATE, request
        )
        showStatus("Saved. Periodic sync is enabled; Android may batch the 15-minute schedule.")
    }

    private fun requestPermissions() {
        if (!SyncConfig.hasConfig(this)) {
            showStatus("Save the HA URL and token first.")
            return
        }
        permissionLauncher.launch(setOf(
            HealthPermission.getWritePermission(WeightRecord::class),
            HealthPermission.getReadPermission(WeightRecord::class),
            HealthPermission.getWritePermission(BloodGlucoseRecord::class),
            HealthPermission.getReadPermission(BloodGlucoseRecord::class)
        ))
    }

    private fun syncNow() {
        if (!SyncConfig.hasConfig(this)) {
            showStatus("Save the HA URL and token first.")
            return
        }
        WorkManager.getInstance(this).enqueue(OneTimeWorkRequest.Builder(SyncWorker::class.java).build())
        showStatus("Sync queued. Check the Health Connect app after it completes.")
    }

    private fun showStatus(text: String) { status.text = text }
    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()
    private fun matchWrap() = ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
    private fun marginParams(l: Int, t: Int, r: Int, b: Int): ViewGroup.LayoutParams =
        LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            setMargins(dp(l), dp(t), dp(r), dp(b))
        }
}
