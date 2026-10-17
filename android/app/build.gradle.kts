import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Release signing key, kept outside the repository. Point the Gradle property
// freedisplay.signing (e.g. in ~/.gradle/gradle.properties) at a properties file
// with storeFile, storePassword, keyAlias and keyPassword. Without it, release
// builds are signed with the local debug key.
val releaseSigning: Properties? = providers.gradleProperty("freedisplay.signing").orNull?.let { path ->
    Properties().apply { file(path).inputStream().use { load(it) } }
}

android {
    namespace = "com.freedisplay.client"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.freedisplay.client"
        minSdk = 26
        targetSdk = 35
        versionCode = 12 // keep in sync with kAppVersionCode in host/src/main.cpp
        versionName = "1.3.0"
    }

    signingConfigs {
        if (releaseSigning != null) {
            create("release") {
                storeFile = file(releaseSigning.getProperty("storeFile"))
                storePassword = releaseSigning.getProperty("storePassword")
                keyAlias = releaseSigning.getProperty("keyAlias")
                keyPassword = releaseSigning.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (releaseSigning != null) "release" else "debug")
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
