plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.freedisplay.client"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.freedisplay.client"
        minSdk = 26
        targetSdk = 35
        versionCode = 8 // keep in sync with kAppVersionCode in host/src/main.cpp
        versionName = "1.0.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Debug-signed so the host can install it without a release keystore.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}
