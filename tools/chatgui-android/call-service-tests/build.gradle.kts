plugins { id("org.jetbrains.kotlin.jvm") }

kotlin {
    jvmToolchain(17)
    sourceSets.named("main") {
        kotlin.srcDirs("../router-tests/src/main/kotlin", "../app/src/main/java")
        // Настоящие service/controller; DSP/camera/Telecom заменены явными границами в CallServiceHost.
        kotlin.include("android/**", "androidx/**", "com/utun/chat/RouterHostApp.kt",
            "com/utun/chat/data/RouterHostEnvironment.kt", "com/utun/chat/data/RadioHostEnvironment.kt",
            "com/utun/chat/data/CallServiceHost.kt", "com/utun/chat/data/CallService.kt", "com/utun/chat/data/CallController.kt",
            "com/utun/chat/data/CallState.kt", "com/utun/chat/data/CallAudioRouter.kt", "com/utun/chat/data/CoreActivity.kt",
            "com/utun/chat/data/PhoneMediaDispatcher.kt", "com/utun/chat/data/MemberFilesProtocol.kt",
            "com/utun/chat/data/SettingsOperations.kt",
            "com/utun/chat/data/RadioAutoSettings.kt", "com/utun/chat/data/StreamServiceHost.kt",
            "com/utun/chat/data/StreamService.kt", "com/utun/chat/data/StreamController.kt", "com/utun/chat/data/StreamTxStats.kt",
            "com/utun/chat/data/StreamFecParameters.kt")
    }
}

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.7.3")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.7.3")
}
tasks.test { testLogging { showStandardStreams = true } }
