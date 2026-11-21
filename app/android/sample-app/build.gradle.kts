plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "dev.capturekit.sample"
    compileSdk = 37

    defaultConfig {
        applicationId = "dev.capturekit.sample"
        minSdk = 26
        targetSdk = 37
        versionCode = 1
        versionName = "0.0.1"
    }

    buildFeatures {
        buildConfig = true
    }

    flavorDimensions += "mode"
    productFlavors {
        create("standard") {
            dimension = "mode"
        }
        create("field") {
            dimension = "mode"
            applicationIdSuffix = ".field"
            // Upload code that uses these BuildConfig fields must live in src/field (field flavour only).
            fun esc(s: String) = s.replace("\\", "\\\\").replace("\"", "\\\"")
            val tusUrl = esc((project.findProperty("TUS_URL") as String?) ?: "")
            val verifierUrl = esc((project.findProperty("VERIFIER_URL") as String?) ?: "")
            buildConfigField("String", "TUS_URL", "\"$tusUrl\"")
            buildConfigField("String", "VERIFIER_URL", "\"$verifierUrl\"")
            // Empty DSN = Sentry off. Telemetry.kt additionally requires telemetry consent.
            val sentryDsn = esc((project.findProperty("SENTRY_DSN") as String?) ?: "")
            buildConfigField("String", "SENTRY_DSN", "\"$sentryDsn\"")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation(project(":capturekit"))
    // Crash reports exist only in the field flavour: the standard build has no Sentry classes and no INTERNET.
    "fieldImplementation"(libs.sentry.android)
}
