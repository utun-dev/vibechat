plugins {
    id("org.jetbrains.kotlin.jvm")
}

kotlin {
    jvmToolchain(17)
    sourceSets.named("main") {
        // Компилируем настоящие компоненты приложения; Android fixtures существуют только в этом JVM-модуле.
        kotlin.srcDir(providers.gradleProperty("routerSourceDir").getOrElse("../app/src/main/java"))
        kotlin.include("android/**", "androidx/**", "com/utun/chat/RouterHostApp.kt", "com/utun/chat/data/RouterHostEnvironment.kt",
            "com/utun/chat/data/IncomingHostEnvironment.kt", "com/utun/chat/data/IncomingCallManager.kt",
            "com/utun/chat/data/IncomingCallActionReceiver.kt",
            "com/utun/chat/data/MediaAudioController.kt", "com/utun/chat/data/MicrophoneCapture.kt",
            "com/utun/chat/data/MediaCaptureTimeline.kt", "com/utun/chat/data/MediaHostCapture.kt",
            "com/utun/chat/data/CallAudioRouter.kt", "com/utun/chat/data/CoreActivity.kt",
            "com/utun/chat/data/ProximityManager.kt", "com/utun/chat/data/PhoneStateMonitor.kt",
            "com/utun/chat/data/NetworkActivityMonitor.kt", "com/utun/chat/data/NetworkChangeMonitor.kt",
            "com/utun/chat/data/RingtonePlayer.kt", "com/utun/chat/data/CallRingback.kt", "com/utun/chat/data/NotificationSound.kt")
        kotlin.include("com/utun/chat/data/MemberFilesRepository.kt", "com/utun/chat/data/MemberFilesProtocol.kt",
            "com/utun/chat/headless/HeadlessHostEnvironment.kt", "com/utun/chat/data/LocalDiscovery.kt",
            "com/utun/chat/data/BatteryOptimizationPermission.kt")
        kotlin.include("com/utun/chat/data/RadioAudioEngine.kt", "com/utun/chat/data/RadioPttHolds.kt",
            "com/utun/chat/data/RadioAutoSettings.kt", "com/utun/chat/data/RadioVadWindow.kt",
            "com/utun/chat/data/RadioHostEnvironment.kt", "com/utun/chat/data/MicrophonePermission.kt",
            "com/utun/chat/data/RadioCallController.kt", "com/utun/chat/data/RadioOverlayService.kt")
        kotlin.include("com/utun/chat/data/CallAudioEngine.kt", "com/utun/chat/data/CallAudioOutput.kt",
            "com/utun/chat/data/CallAudioTiming.kt", "com/utun/chat/data/CallAudioTrace.kt",
            "com/utun/chat/data/CallPlaybackBuffer.kt", "com/utun/chat/data/CallOutputRecovery.kt",
            "com/utun/chat/data/CallState.kt", "com/utun/chat/data/CallAudioHost.kt")
        kotlin.include("com/utun/chat/data/ConversationSubmissions.kt", "com/utun/chat/data/ConversationTarget.kt")
        kotlin.include("com/utun/chat/data/PhoneMediaDispatcher.kt")
        kotlin.include("com/utun/chat/data/SettingsOperations.kt")
    }
}

// Отдельный каталог результатов позволяет проверить регрессию на старом исходнике без замены кода приложения.
providers.gradleProperty("routerTestsBuildDir").orNull?.let { layout.buildDirectory.set(file(it)) }

dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.7.3")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.7.3")
}

tasks.test {
    testLogging { showStandardStreams = true }
}
