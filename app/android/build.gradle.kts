// AGP 9 compiles Kotlin with its built-in Kotlin support; declaring the Kotlin
// plugin here (not applied) pins the Kotlin Gradle Plugin version from the catalog.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.ksp) apply false
}
