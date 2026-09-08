# Google Health Connect bridge (phase 2)

Health Connect is an on-device Android data store. Home Assistant cannot write to it directly from HAOS; the bridge must run in a separate Android app.

## Current scope

The first bridge iteration writes only:

- `WeightRecord` from `sensor.ge_fit_plus_ln_weight`
- `BloodGlucoseRecord` from `sensor.gluroo_blood_glucose`

The bridge keeps body fat, BMI, raw impedance, and the remaining scale metrics in
Home Assistant initially. Body-fat records require a separate validation pass before
enabling them. Gluroo CGM values are written as interstitial-fluid glucose records.

## Sync design

1. The ESPHome device publishes a new `measurement_id` for each completed weigh-in.
2. A visible Android foreground service polls the HA REST API every 2 minutes for the measurement ID and related sensor states. WorkManager remains a 15-minute fallback for periods when Android stops the foreground service.
3. It uses the measurement ID for scale records and timestamp plus value for glucose deduplication.
4. It writes confirmed values to Health Connect with a `ZoneOffset` for `America/Los_Angeles`, selected from the original UTC sample time so daylight-saving changes are handled correctly.
5. The last successfully synced keys are stored in Android encrypted app-private storage.
6. The foreground notification restarts after app update or device boot when configuration is present.

The service keeps a partial wake lock while active so the two-minute polling loop is not suspended during ordinary screen-off operation. Android force-stop, revoked permissions, or loss of network can still stop logging and must be visible in the notification/logs.

## Minimum Health Connect permissions

The Android app should request only the permissions needed for the initial scope:

```xml
<uses-permission android:name="android.permission.health.WRITE_WEIGHT" />
<uses-permission android:name="android.permission.health.READ_WEIGHT" />
<uses-permission android:name="android.permission.health.WRITE_BLOOD_GLUCOSE" />
<uses-permission android:name="android.permission.health.READ_BLOOD_GLUCOSE" />
```

The bridge requests `WRITE_WEIGHT`, `READ_WEIGHT`, `WRITE_BLOOD_GLUCOSE`, and
`READ_BLOOD_GLUCOSE`. It uses the read permissions for post-write verification and
measurement deduplication. Any future body-fat or other record types will require
separately requested permissions through the Health Connect UI.

The official Jetpack client is `androidx.health.connect:connect-client`; the exact version will be pinned when the Android module is built. Health Connect requires Android 9/API 28 or newer with Google Play services. Android 14 includes Health Connect in the system; older supported Android releases use the Health Connect app.

## Local configuration

The repository contains an Android bridge under `android/`. It writes scale `WeightRecord` and Gluroo `BloodGlucoseRecord` data. Body-fat and the remaining composition metrics remain HA-only.

On first launch:

1. Enter the HA URL and a narrowly scoped HA long-lived token in the app UI.
2. Tap **Save and start continuous sync**.
3. Tap **Grant Health Connect permissions** and approve weight and blood-glucose access in Health Connect.
4. Confirm the ongoing **Health Connect sync active** notification.
5. Tap **Sync now** to verify the first record if needed.

The app stores the URL/token with Android encrypted app-private preferences and keeps
dedupe keys so each weigh-in or glucose sample is written once. The HA token is never
part of this repository or APK build configuration. New glucose and weight records use
the America/Los_Angeles Health Connect offset; previously written records are not
rewritten automatically.

Build locally with the Gradle wrapper/toolchain used by CI, then install the generated
`app/build/outputs/apk/debug/app-debug.apk` only through a trusted ADB session.

The public event contract remains [measurement.schema.json](measurement.schema.json).


## Official references

- [Health Connect overview](https://developer.android.com/health-and-fitness/guides/health-connect/overview)
- [Get started with Health Connect](https://developer.android.com/health-and-fitness/guides/health-connect/develop/get-started)
- [Health Connect data types](https://developer.android.com/health-and-fitness/guides/health-connect/plan/data-types)
- [Health Connect availability](https://developer.android.com/health-and-fitness/guides/health-connect/plan/availability)
