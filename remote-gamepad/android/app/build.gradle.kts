plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.linkalls.cemuremotegamepad"
    compileSdk = 36

    defaultConfig {
        applicationId = "dev.linkalls.cemuremotegamepad"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0-usb-preview"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}
