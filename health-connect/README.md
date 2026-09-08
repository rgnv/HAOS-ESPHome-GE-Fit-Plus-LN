# Google Health Connect bridge (phase 2)

Health Connect is an on-device Android data store. Home Assistant cannot write to it directly from HAOS; the bridge must run on the Xperia as an Android companion app.

## Current scope

The first bridge iteration will write only:

- `WeightRecord` from `sensor.ge_fit_plus_ln_weight`
- `BodyFatRecord` from `sensor.ge_fit_plus_ln_body_fat`

The bridge will keep BMI, raw impedance, and the remaining percentage metrics in Home Assistant initially. Body-water and lean-mass records require unit conversion and a validation pass before enabling them.

## Sync design

1. The ESPHome device publishes a new `measurement_id` for each completed weigh-in.
2. The Xperia bridge polls the HA REST API over HTTPS for the measurement ID and related sensor states.
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

The Android bridge will require a user-entered HA HTTPS URL and a narrowly scoped HA token. Those values must be stored in Android app-private encrypted storage and must never be placed in this repository, an APK release, or CI logs.

The public event contract is [measurement.schema.json](measurement.schema.json).

## Official references

- [Health Connect overview](https://developer.android.com/health-and-fitness/guides/health-connect/overview)
- [Get started with Health Connect](https://developer.android.com/health-and-fitness/guides/health-connect/develop/get-started)
- [Health Connect data types](https://developer.android.com/health-and-fitness/guides/health-connect/plan/data-types)
- [Health Connect availability](https://developer.android.com/health-and-fitness/guides/health-connect/plan/availability)
