# Google Health Connect bridge (phase 2)

Health Connect is an on-device Android data store. Home Assistant cannot write to it directly from HAOS; the bridge must run in a separate Android app.

## Current scope

The first bridge iteration will write only:

- `WeightRecord` from `sensor.ge_fit_plus_ln_weight`
- `BodyFatRecord` from `sensor.ge_fit_plus_ln_body_fat`

The bridge will keep BMI, raw impedance, and the remaining percentage metrics in Home Assistant initially. Body-water and lean-mass records require unit conversion and a validation pass before enabling them.

## Sync design

1. The ESPHome device publishes a new `measurement_id` for each completed weigh-in.
2. The Android bridge polls the HA REST API over HTTPS for the measurement ID and related sensor states.
3. It uses the measurement ID plus HA timestamp and source as the local deduplication key.
4. It writes the confirmed values to Health Connect with metadata identifying the GE Fit Plus LN source.
5. The last successfully synced ID is stored in Android app-private storage.

A future push-triggered path can reduce polling latency, but polling is the initial reliable implementation.

## Minimum Health Connect permissions

The Android app should request only the permissions needed for the initial scope:

```xml
<uses-permission android:name="android.permission.health.WRITE_WEIGHT" />
<uses-permission android:name="android.permission.health.WRITE_BODY_FAT" />
```

If the bridge later reads existing Health Connect records for cross-application deduplication, it will separately request `READ_WEIGHT` and `READ_BODY_FAT`. The user must grant or revoke these permissions through the Health Connect UI.

The official Jetpack client is `androidx.health.connect:connect-client`; the exact version will be pinned when the Android module is built. Health Connect requires Android 9/API 28 or newer with Google Play services. Android 14 includes Health Connect in the system; older supported Android releases use the Health Connect app.

## Local configuration

The repository now contains a minimal Android bridge under `android/`. The debug APK
was built and installed on the Xperia through its paired wireless ADB connection. It
currently writes the scale's `WeightRecord`; body-fat and glucose records remain
separate follow-up scopes.

On first launch:

1. Enter the HA URL and a narrowly scoped HA long-lived token in the app UI.
2. Tap **Save and enable periodic sync**.
3. Tap **Grant Health Connect weight permission** and approve it in Health Connect.
4. Tap **Sync now** to verify the first record.

The app stores the URL/token with Android encrypted app-private preferences and keeps a
measurement-ID dedupe value so a weigh-in is written once. The HA token is never part
of this repository or APK build configuration.

Build locally with the Gradle wrapper/toolchain used by CI, then install the generated
`app/build/outputs/apk/debug/app-debug.apk` only through a trusted ADB session.

The public event contract remains [measurement.schema.json](measurement.schema.json).


## Official references

- [Health Connect overview](https://developer.android.com/health-and-fitness/guides/health-connect/overview)
- [Get started with Health Connect](https://developer.android.com/health-and-fitness/guides/health-connect/develop/get-started)
- [Health Connect data types](https://developer.android.com/health-and-fitness/guides/health-connect/plan/data-types)
- [Health Connect availability](https://developer.android.com/health-and-fitness/guides/health-connect/plan/availability)
