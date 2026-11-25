plugins { id("org.jetbrains.kotlin.jvm") }

kotlin {
    jvmToolchain(17)
    sourceSets.named("main") {
        // Настоящие reader/очереди, независимый от router-tests Android API без аппаратных ресурсов.
        kotlin.srcDir("../app/src/main/java")
        kotlin.include("android/**", "androidx/**", "com/utun/chat/data/AndroidMicrophoneCapture.kt",
            "com/utun/chat/data/MicrophoneCapture.kt", "com/utun/chat/data/CaptureHostLog.kt",
            "com/utun/chat/RecorderHostApp.kt", "com/utun/chat/data/RecorderHostEnvironment.kt",
            "com/utun/chat/data/AudioRecorderManager.kt")
    }
}

dependencies { testImplementation("junit:junit:4.13.2") }
tasks.test { testLogging { showStandardStreams = true } }
