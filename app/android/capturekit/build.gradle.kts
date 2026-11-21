plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.ksp)
}

val skipNative = providers.gradleProperty("capturekit.skipNative").map(String::toBoolean).getOrElse(false)

android {
    namespace = "dev.capturekit"
    compileSdk = 37
    ndkVersion = "30.0.16248370"

    defaultConfig {
        minSdk = 26
        consumerProguardFiles("consumer-rules.pro")
        if (!skipNative) {
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DCAPTUREKIT_BUILD_TESTS=OFF", "-DANDROID_STL=c++_shared")
                }
            }
            ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        }
    }

    if (!skipNative) {
        externalNativeBuild {
            cmake {
                path = file("../../core/CMakeLists.txt")
                version = "4.1.2"
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    api(libs.arcore)
    implementation(libs.tus.android.client)
    implementation(libs.tus.java.client)
    implementation(libs.room.runtime)
    implementation(libs.room.ktx)
    ksp(libs.room.compiler)
    implementation(libs.work.runtime.ktx)
    implementation(libs.litert)

    testImplementation(libs.junit)
}
