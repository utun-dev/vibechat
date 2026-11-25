package com.utun.chat.data

import android.content.Context

/** Camera/DSP — управляемая граница, настоящие StreamController/StreamService компилируются из приложения. */
data class StreamCaptureMode(val screen: Boolean = false, val width: Int = 1280, val height: Int = 720, val videoCodec: Int = 1)
object StreamCameraModes {
    fun screenModes(portrait: Boolean) = listOf(StreamCaptureMode(true, if (portrait) 720 else 1280, if (portrait) 1280 else 720))
}
data class StreamCaptureConfig(val value: Int = 1, val mode: StreamCaptureMode = StreamCaptureMode()) { fun withMode(next: StreamCaptureMode) = copy(mode = next) }
class ScreenProjection(context: Context, grant: android.content.Intent, val stopped: () -> Unit) {
    var portrait = false
    var onOrientationChanged: (() -> Unit)? = null
    fun configurationChanged() {}
    var closed = false
    fun close() { closed = true }
}
typealias RadioCompressorSettings = Any
class StreamPreview { var closed = false; fun close() { closed = true } }
class StreamCaptureEngine(context: Context, val id: Long, group: String, val config: StreamCaptureConfig, microphone: Boolean,
    preview: StreamPreview, initialAudioCompressor: RadioCompressorSettings,
    private val ready: (StreamCaptureConfig, Boolean) -> Unit, failure: (String) -> Unit) {
    companion object { var nextFailures = emptySet<String>(); val created = mutableListOf<StreamCaptureEngine>() }
    init { created.add(this) }
    var failures = nextFailures
    var projection: ScreenProjection? = null
    var started = false
    var stopped = false
    var stops = 0
    var audioCompressorSettings = initialAudioCompressor
    fun start() { started = true; check("start" !in failures); ready(config, false) }
    fun stop(): Boolean {
        stops++; check("stop" !in failures)
        if ("pending" in failures) return false
        stopped = true; check("stop after" !in failures); return true
    }
    fun change(config: StreamCaptureConfig) { check("change" !in failures); ready(config, false) }
    fun setAudioEnabled(enabled: Boolean): Boolean { check("audio" !in failures); return "audio reject" !in failures }
}
