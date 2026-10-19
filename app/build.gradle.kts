plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Official releases are signed only from a maintainer-owned keystore injected
// by CI. Nothing private is kept in git or silently replaced with debug signing.
val officialKeystorePath = System.getenv("ANDROID_KEYSTORE_PATH")
val officialKeystorePassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")
val officialKeyAlias = System.getenv("ANDROID_KEY_ALIAS")
val officialKeyPassword = System.getenv("ANDROID_KEY_PASSWORD")
val hasOfficialReleaseSigning = listOf(
    officialKeystorePath,
    officialKeystorePassword,
    officialKeyAlias,
    officialKeyPassword
).all { !it.isNullOrBlank() }
val requestedReleaseTask = gradle.startParameter.taskNames.any { task ->
    task.contains("release", ignoreCase = true)
}
if (requestedReleaseTask && !hasOfficialReleaseSigning) {
    error("Release APK ditolak: keystore resmi Android belum disediakan oleh CI.")
}

android {
    namespace = "com.xykal.sampdroid"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "com.xykal.sampdroid"
        minSdk = 24
        targetSdk = 34
        versionCode = 18
        versionName = "0.18.0"

        // WAJIB: dua arsitektur sesuai permintaan
        ndk {
            abiFilters += listOf("armeabi-v7a", "arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-O2", "-fexceptions", "-frtti")
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    signingConfigs {
        create("officialRelease") {
            if (hasOfficialReleaseSigning) {
                storeFile = file(officialKeystorePath!!)
                storePassword = officialKeystorePassword!!
                keyAlias = officialKeyAlias!!
                keyPassword = officialKeyPassword!!
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // `assembleRelease` already fails above without every credential.
            // Debug signing is never used as a fallback for a publishable APK.
            if (hasOfficialReleaseSigning) {
                signingConfig = signingConfigs.getByName("officialRelease")
            }
        }
        debug {
            isDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        viewBinding = false
    }
    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.4")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
