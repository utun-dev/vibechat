import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// Ключ и пароли создаёт create_signing_key.sh; пути считаются от каталога настроек.
val releaseSigningFile = rootProject.file("signing/keystore.properties")
val releaseSigning = Properties().apply {
    if (releaseSigningFile.isFile) releaseSigningFile.inputStream().use { load(it) }
}
val validateReleaseSigning by tasks.registering {
    doLast {
        check(releaseSigningFile.isFile) {
            "Release signing settings missing: $releaseSigningFile. Run ./create_signing_key.sh first."
        }
        for (key in listOf("storeFile", "storePassword", "keyAlias", "keyPassword")) {
            check(!releaseSigning.getProperty(key).isNullOrBlank()) { "Release signing: missing $key in $releaseSigningFile" }
        }
        val keystore = releaseSigningFile.parentFile.resolve(releaseSigning.getProperty("storeFile"))
        check(keystore.isFile) { "Release signing keystore not found: $keystore" }
        logger.lifecycle("Release signing: keystore={}, alias={}", keystore, releaseSigning.getProperty("keyAlias"))
    }
}

// Общие desktop/Android картинки; Sync не оставляет старые версии в генерируемых ресурсах.
val prepareSharedImages by tasks.registering(Sync::class) {
    from("../../chatgui/resources/bg.webp") {
        into("drawable-nodpi")
        rename { "chat_background.webp" }
    }
    from("../../chatgui/resources/icons/app_icon.webp") {
        into("mipmap-xxhdpi")
        rename { "ic_launcher.webp" }
    }
    into(layout.buildDirectory.dir("generated/sharedImages/res"))
    doLast { logger.lifecycle("Shared chat images prepared: bg.webp and 256x256 app_icon.webp") }
}
tasks.named("preBuild") { dependsOn(prepareSharedImages) }

// Общий комплект лицензий ядра и встроенных библиотек включается в каждый APK основного приложения.
val prepareLicenses by tasks.registering(Sync::class) {
    from("../../..") { include("LICENSE", "LICENSING", "THIRD_PARTY_NOTICES") }
    into(layout.buildDirectory.dir("generated/licenses/assets/licenses"))
    doLast { logger.lifecycle("uTun license documents prepared for APK assets/licenses") }
}
tasks.named("preBuild") { dependsOn(prepareLicenses) }

android {
    namespace = "com.utun.chat"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    defaultConfig {
        applicationId = "com.utun.chat"
        testInstrumentationRunner = "com.utun.chat.BlePeerProbe"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"

        externalNativeBuild {
            cmake {
                arguments("-DANDROID_STL=c++_static")
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    // Общие TGS-анимации с desktop-чатгуи, без копирования ресурсов.
    sourceSets["main"].assets.srcDir("../../chatgui/resources/animations")
    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("generated/licenses/assets"))
    sourceSets["main"].res.srcDir(layout.buildDirectory.dir("generated/sharedImages/res"))

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    signingConfigs {
        create("release") {
            if (releaseSigningFile.isFile) {
                storeFile = releaseSigning.getProperty("storeFile")?.let { releaseSigningFile.parentFile.resolve(it) }
                storePassword = releaseSigning.getProperty("storePassword")
                keyAlias = releaseSigning.getProperty("keyAlias")
                keyPassword = releaseSigning.getProperty("keyPassword")
                storeType = "PKCS12"
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
        }
    }
}
tasks.matching { it.name == "preReleaseBuild" }.configureEach { dependsOn(validateReleaseSigning) }

// Настоящий JNI callback с отказами JVM; Linux host нужен для ELF --gc-sections, NDK ABI отдельно проверяет сборка APK.
val testJniLog by tasks.registering(Exec::class) {
    onlyIf("JNI host test requires Linux") { System.getProperty("os.name").startsWith("Linux") }
    workingDir(rootProject.projectDir)
    commandLine("python3", rootProject.file("tests/jni_log/run.py"), "--java-home", System.getProperty("java.home"))
}
val testJniUtf8 by tasks.registering(Exec::class) {
    onlyIf("Checked JNI JVM test requires Linux") { System.getProperty("os.name").startsWith("Linux") }
    workingDir(rootProject.projectDir)
    commandLine("python3", rootProject.file("tests/jni_utf8/run.py"), "--java-home", System.getProperty("java.home"))
}

// Настоящий Kotlin dispatcher исполняется на JVM; небольшой JNI-приёмник сохраняет его события в тесте.
val bleCommandTestJni = layout.buildDirectory.file("ble-command-tests/libble_commands.so")
val buildBleCommandTestJni by tasks.registering(Exec::class) {
    onlyIf("BLE command JNI test requires Linux") { System.getProperty("os.name").startsWith("Linux") }
    val source = rootProject.file("tests/ble_commands/events.c")
    val jdk = file(System.getProperty("java.home"))
    inputs.file(source); outputs.file(bleCommandTestJni)
    doFirst { bleCommandTestJni.get().asFile.parentFile.mkdirs() }
    commandLine("cc", "-std=c99", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
        "-I${jdk.resolve("include")}", "-I${jdk.resolve("include/linux")}", source, "-o", bleCommandTestJni.get().asFile)
}

// Зависимость только между задачами: host-модели не входят в classpath приложения/APK.
tasks.withType<org.gradle.api.tasks.testing.Test>().configureEach {
    dependsOn(":router-tests:test", ":capture-tests:test", ":language-tests:test", ":call-service-tests:test", ":video-tests:test",
        testJniLog, testJniUtf8)
    if (name == "testDebugUnitTest") dependsOn(":ble-command-tests:test")
    val messageLinkCases = rootProject.file("../chatgui/tests/message_links_cases.tsv")
    inputs.file(messageLinkCases)
    systemProperty("utun.messageLinkCases", messageLinkCases.absolutePath)
    val eventHeader = rootProject.file("../../src/chat/chat_event.h")
    val bridgeSource = rootProject.file("jni_bridge/android_jni_bridge.c")
    inputs.files(eventHeader, bridgeSource)
    systemProperty("utun.chatEventHeader", eventHeader.absolutePath)
    systemProperty("utun.androidBridgeSource", bridgeSource.absolutePath)
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303") // Реальный JSON для проверки границы JNI, без Android mock stubs.
    implementation(platform("androidx.compose:compose-bom:2024.02.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.compose.foundation:foundation")
    implementation("com.airbnb.android:lottie-compose:6.3.0")
    implementation("androidx.activity:activity-compose:1.8.2")
    implementation("androidx.appcompat:appcompat:1.7.1")
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.7.0")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.7.0")
    implementation("androidx.datastore:datastore-preferences:1.0.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")
    implementation("com.google.code.gson:gson:2.10.1")
    implementation("com.google.mlkit:barcode-scanning:17.3.0")
    implementation("com.google.zxing:core:3.5.3")
    val cameraXVersion = "1.7.0-rc01"
    implementation("androidx.camera:camera-camera2:$cameraXVersion")
    implementation("androidx.camera:camera-lifecycle:$cameraXVersion")
    implementation("androidx.camera:camera-view:$cameraXVersion")
    implementation("androidx.camera:camera-video:$cameraXVersion")
}
