// Отдельный Java/AAudio APK для измерений arm64 (API 28+); не линкует основное ядро libutun.
plugins { id("com.android.application") }

android {
    namespace = "com.utun.audioprobe"
    compileSdk = 36
    ndkVersion = "29.0.14206865"
    defaultConfig {
        applicationId = "com.utun.audioprobe"
        minSdk = 28
        targetSdk = 36
        versionCode = 1
        versionName = "1"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild { cmake { arguments("-DANDROID_STL=c++_static") } }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt") } }
}
