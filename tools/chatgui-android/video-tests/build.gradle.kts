plugins { id("org.jetbrains.kotlin.jvm") }

kotlin {
    jvmToolchain(17)
    sourceSets.named("main") {
        // Настоящий RX/протокольный формат; Android codecs, probe и JNI управляются на аппаратных границах.
        kotlin.srcDir("../app/src/main/java")
        kotlin.include("android/**", "androidx/**", "com/utun/chat/data/VideoHost.kt", "com/utun/chat/data/CallVideoPlayer.kt",
            "com/utun/chat/data/CallVideoFormat.kt", "com/utun/chat/data/StreamVideoCodec.kt", "com/utun/chat/data/H264AnnexB.kt",
            "com/utun/chat/data/StreamPreview.kt", "com/utun/chat/data/VideoCodecResources.kt",
            "com/utun/chat/data/StreamVideoPlayback.kt", "com/utun/chat/data/StreamDecodePump.kt")
        kotlin.include("com/utun/chat/VideoHostApp.kt", "com/utun/chat/data/VideoEngineHost.kt",
            "com/utun/chat/data/CallVideoEngine.kt", "com/utun/chat/data/CallVideoBitrateRange.kt",
            "com/utun/chat/data/CallVideoCadence.kt", "com/utun/chat/data/CallVideoOrientation.kt")
        kotlin.include("com/utun/chat/data/StreamHost.kt", "com/utun/chat/data/StreamPlayerEngine.kt",
            "com/utun/chat/data/StreamAudioMixer.kt", "com/utun/chat/data/StreamAudioOutput.kt",
            "com/utun/chat/data/StreamAudioMix.kt", "com/utun/chat/data/StreamReadQueue.kt", "com/utun/chat/data/StreamPlaybackTelemetry.kt",
            "com/utun/chat/data/StreamCaptureEngine.kt", "com/utun/chat/data/StreamCaptureConfig.kt",
            "com/utun/chat/data/TelecomCallManager.kt", "com/utun/chat/data/UtunConnection.kt", "com/utun/chat/data/UtunConnectionService.kt", "com/utun/chat/data/MemberFilesProtocol.kt",
            "com/utun/chat/data/QrScannerSession.kt", "com/utun/chat/data/VideoFilePlayer.kt",
            "com/utun/chat/data/VideoFileMetadata.kt", "com/google/**")
        kotlin.include("com/utun/chat/data/CameraCaptureResources.kt", "com/utun/chat/data/VideoFileTracks.kt")
    }
}
dependencies {
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.7.3")
    implementation("org.json:json:20240303")
    testImplementation("junit:junit:4.13.2")
}
tasks.test { testLogging { showStandardStreams = true } }
