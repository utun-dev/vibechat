package com.utun.chat.data

import java.util.Collections
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.ConcurrentLinkedQueue
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.flow.MutableStateFlow

/** Минимальная поверхность Telecom; Bluetooth lifecycle не создаёт настоящую Connection. */
class UtunConnection {
    var onRouteChanged: ((AudioRoute) -> Unit)? = null
    var onEndpointsChanged: (() -> Unit)? = null
    var confirmedRoute: AudioRoute? = null
    var headsetAvailable: Boolean? = null
    val routeRequests = mutableListOf<AudioRoute>()
    var failRoute = false
    fun routeTo(route: AudioRoute) {
        check(!failRoute) { "Telecom request unavailable" }
        routeRequests.add(route)
    }
    fun confirm(route: AudioRoute) { confirmedRoute = route; onRouteChanged?.invoke(route) }
}

/** Собирает настоящий LogManager API router; вывод можно отключить для конкурентной серии. */
object LogManager {
    data class Entry(val level: String, val category: String, val message: String)
    val entries: MutableList<Entry> = Collections.synchronizedList(mutableListOf<Entry>())
    var console = true
    fun addLog(level: String, category: String, message: String) {
        entries.add(Entry(level, category, message))
        if (console || level == "ERROR") System.err.println("[$level] [$category] $message")
    }
}

/** Только host-тест: считает уведомления standby, не загружает JNI и не запускает ядро. */
object NativeLib {
    @Volatile var coreStopping = false
    fun isCoreStopping() = coreStopping
    @Volatile var callAudioFailures = emptySet<String>()
    @Volatile var onCallFeed: () -> Unit = {}
    val callAudioCommands: MutableList<Pair<String, Long>> = Collections.synchronizedList(mutableListOf())
    fun callAudioStart(id: Long): Boolean {
        callAudioCommands.add("start" to id)
        return "start" !in callAudioFailures
    }
    fun callAudioStop() {
        callAudioCommands.add("stop" to 0L)
        check("stop" !in callAudioFailures) { "native call audio stop failed" }
    }
    fun callAudioFeed(id: Long, samples: ShortArray, muted: Boolean, ptsUs: Long): Boolean { onCallFeed(); return true }
    fun callAudioPull(id: Long, samples: ShortArray) = 0
    fun callAudioClock(id: Long, nowUs: Long, snapshot: LongArray): Boolean { snapshot[0] = 1; return true }
    fun callAudioDiagnostics(id: Long, snapshot: LongArray) = false
    fun callAudioProgress(id: Long, epoch: Long, position: Long, atUs: Long, nowUs: Long, measured: Boolean) {}
    fun callAudioSetAec(enabled: Boolean) {}
    fun callAudioSetAecDelay(frames: Int) {}
    fun callAudioResetCapture(id: Long) { callAudioCommands.add("resetCapture" to id) }
    val callCommands = mutableListOf<Pair<String, Long>>()
    var rejectedCallCommands = emptySet<String>()
    fun callStart(channel: String, peer: String?, id: Long, devices: Boolean = false): Boolean {
        callCommands.add("start" to id); return "start" !in rejectedCallCommands
    }
    fun callAccept(id: Long): Boolean { callCommands.add("accept" to id); return "accept" !in rejectedCallCommands }
    fun callDecline(id: Long): Boolean { callCommands.add("decline" to id); return "decline" !in rejectedCallCommands }
    fun callHangup(id: Long): Boolean { callCommands.add("hangup" to id); return "hangup" !in rejectedCallCommands }
    fun callContinue(id: Long): Boolean { callCommands.add("continue" to id); return "continue" !in rejectedCallCommands }
    data class RadioCallCommand(val id: Long, val action: Int, val group: String?, val peer: String?)
    val radioCalls = mutableListOf<RadioCallCommand>()
    var onRadioCall: (RadioCallCommand) -> Boolean = { true }
    fun radioCall(id: Long, action: Int, group: String? = null, peer: String? = null, devices: Boolean = false): Boolean {
        val command = RadioCallCommand(id, action, group, peer)
        radioCalls.add(command)
        return onRadioCall(command)
    }
    data class RadioCommand(val kind: String, val group: String)
    val radioCommands: MutableList<RadioCommand> = Collections.synchronizedList(mutableListOf())
    @Volatile var radioRequest = 0L
    @Volatile private var radioGroup = ""
    @Volatile private var radioMode = 0
    @Volatile private var radioTx = false
    @Volatile var radioFailures = emptySet<String>()
    @Volatile var radioCaptureAlive = false
    @Volatile var radioPullResult = 0
    val radioPullResults = ConcurrentLinkedQueue<Int>()
    @Volatile var onRadioFeed: (String) -> Unit = {}
    @Volatile var onRadioCaptureStart: (String) -> Unit = {}
    val radioFeeds: MutableList<String> = Collections.synchronizedList(mutableListOf())
    fun radioSetActive(group: String, active: Boolean) { radioCommands.add(RadioCommand("active=$active", group)) }
    fun radioConfigure(request: Long, settings: RadioAutoSettings): Boolean { radioRequest = request; return settings.valid() }
    fun radioAudioStart(group: String, channels: Int): Boolean {
        if ("start" in radioFailures) return false
        radioGroup = group; radioCommands.add(RadioCommand("audio start", group)); return true
    }
    fun radioAudioStop() {
        radioCommands.add(RadioCommand("audio stop", radioGroup))
        check("stop" !in radioFailures) { "native audio stop failed" }
        radioTx = false; radioCaptureAlive = false
    }
    fun radioAudioPull(group: String, out: ShortArray, playedFrames: Long): Int {
        check("pull" !in radioFailures) { "native pull failed" }
        val result = radioPullResults.poll() ?: radioPullResult
        if (result in 1..out.size) out.fill(7, 0, result)
        return result
    }
    fun radioAudioFeed(group: String, samples: ShortArray): Boolean {
        onRadioFeed(group)
        radioFeeds.add(group)
        check("feed" !in radioFailures) { "native feed failed" }
        return "feed reject" !in radioFailures && group == radioGroup && radioCaptureAlive && samples.isNotEmpty()
    }
    fun radioCaptureStart(group: String, channels: Int, mode: Int): Boolean {
        radioCommands.add(RadioCommand("capture start", group))
        if ("capture reject" in radioFailures) return false
        radioMode = if ("detector off" in radioFailures) 0 else mode
        radioCaptureAlive = true
        onRadioCaptureStart(group)
        check("capture start" !in radioFailures) { "native capture start failed after effect" }
        return true
    }
    fun radioCaptureStop() {
        radioCommands.add(RadioCommand("capture stop", radioGroup))
        check("capture stop" !in radioFailures) { "native capture stop failed" }
        radioTx = false; radioCaptureAlive = false
    }
    fun radioCaptureDiscontinuity(group: String) {
        radioCommands.add(RadioCommand("boundary", group))
        check("boundary" !in radioFailures) { "native boundary failed" }
    }
    fun radioAutoMode(): Int {
        check("mode" !in radioFailures) { "native mode query failed" }
        return if ("bad mode" in radioFailures) 99 else radioMode
    }
    fun radioTransmitting() = radioTx
    fun radioTalkBegin(group: String) {
        radioCommands.add(RadioCommand("begin", group)); radioTx = true
        check("begin" !in radioFailures) { "native begin failed after effect" }
    }
    fun radioTalkEnd(group: String) {
        radioCommands.add(RadioCommand("end", group))
        check("end" !in radioFailures) { "native end failed" }
        radioTx = false
    }
    data class LocalInvite(val peer: String, val request: Long, val link: String)
    val localInvites = mutableListOf<LocalInvite>()
    var sendLocal: (LocalInvite) -> Boolean = { true }
    fun sendLocalInvite(peer: ByteArray, request: Long, link: String): Boolean {
        val call = LocalInvite(peer.toHexString(), request, link)
        localInvites.add(call)
        return sendLocal(call)
    }
    data class FileTransfer(val id: Long, val group: String, val server: String, val owner: String,
                            val remote: String, val local: String, val upload: Boolean, val version: ByteArray)
    var fileRequest: (String, String, ByteArray) -> Boolean = { _, _, _ -> true }
    var acceptFileTransfer = true
    val fileTransfers = mutableListOf<FileTransfer>()
    val fileCancels = mutableListOf<Long>()
    fun getMySigningAddress() = "01".repeat(32)
    fun fileRequest(group: String, server: String, wire: ByteArray) = fileRequest.invoke(group, server, wire)
    fun fileTransfer(id: Long, group: String, server: String, owner: String, remote: String, local: String,
                     upload: Boolean, version: ByteArray): Boolean {
        fileTransfers.add(FileTransfer(id, group, server, owner, remote, local, upload, version.copyOf()))
        return acceptFileTransfer
    }
    fun fileCancel(id: Long): Boolean { fileCancels.add(id); return true }
    val networkNotifications = AtomicInteger()
    var ready = true
    var acceptSocketUpdate = true
    val socketUpdates = mutableListOf<String>()
    val activityRequests = mutableListOf<Boolean>()
    var activityAttempts = 0
    var failActivityRequest = false
    var acceptActivityRequest = true
    val phoneRequests = mutableListOf<Pair<Int, Int>>()
    var acceptPhoneRequest = true
    fun notifyNetworkActive() { networkNotifications.incrementAndGet() }
    fun isReady(): Boolean = ready
    var streamFailures = emptySet<String>()
    val streamStops = mutableListOf<Long>()
    val streamEvents = ConcurrentLinkedQueue<ByteArray>()
    var streamStats: LongArray? = null
    var voiceActive = false
    fun voiceIsActive() = voiceActive
    fun streamTxStats(id: Long): LongArray? { check("stats" !in streamFailures); return streamStats }
    fun streamPull(id: Long): ByteArray? { check("pull" !in streamFailures); return streamEvents.poll() }
    fun streamStop(id: Long) { check("stop" !in streamFailures); streamStops.add(id) }
    fun setPhoneState(state: Int, policy: Int): Boolean {
        phoneRequests.add(state to policy)
        return acceptPhoneRequest
    }
    fun updateSockets(config: String): Boolean { socketUpdates.add(config); return acceptSocketUpdate }
    fun setActive(active: Boolean): Boolean {
        activityAttempts++
        check(!failActivityRequest) { "activity request unavailable" }
        if (!acceptActivityRequest) return false
        activityRequests.add(active)
        return true
    }
}

/** Управляемая подготовка конфига: тест удерживает реальный worker, считает перекрытия и затем дожидается его завершения. */
class HostConfigProvider {
    val changes = SettingsOperations()
    val proximitySettings = MutableStateFlow(true to 5f)
    val radioCompressorSettings = MutableStateFlow(Any())
    var failStreamSave = false
    fun saveStreamCaptureSettings(config: Any?, audio: Boolean?): Deferred<Unit> = CompletableDeferred<Unit>().also {
        if (failStreamSave) it.completeExceptionally(IllegalStateException("stream settings save failed")) else it.complete(Unit)
    }
    var callRoute = AudioRoute.EARPIECE
    var failCallRouteSave = false
    fun getCallAudioRoute() = callRoute
    suspend fun setCallAudioRoute(route: AudioRoute) { check(!failCallRouteSave) { "call route settings unavailable" }; callRoute = route }
    @JvmField var radioOverlay = false
    var failOverlayWrite = false
    fun getRadioOverlay() = radioOverlay
    suspend fun setRadioOverlay(enabled: Boolean) {
        check(!failOverlayWrite) { "overlay settings write unavailable" }
        radioOverlay = enabled
    }
    var streamSubblocks = 8
    var streamRedundant = 0
    var streamRegularLimit = 192
    fun getInt(key: String): Int = when (key) {
        "radio.auto_enabled" -> if (radioSettings.enabled) 1 else 0
        "chat.stream_subblocks" -> streamSubblocks
        "chat.stream_rs_redundant" -> streamRedundant
        "chat.stream_regular_limit" -> streamRegularLimit
        else -> error("Unexpected setting: $key")
    }
    var radioSettings = RadioAutoSettings()
    var failRadioSettings = false
    fun getRadioAutoSettings(): RadioAutoSettings {
        check(!failRadioSettings) { "radio settings read failed" }
        return radioSettings
    }
    var keepMicrophoneReady = true
    fun getRadioKeepMicrophoneReady() = keepMicrophoneReady
    var mediaProfile = MediaAudioProfile.AUTO
    var failMediaRead = false
    var failMediaWrite = false
    var mediaLoads = 0
    val mediaSaves = mutableListOf<MediaAudioProfile>()
    var stereo = false
    var micSource = 0
    fun getMediaAudioProfile(): MediaAudioProfile {
        mediaLoads++
        check(!failMediaRead) { "media storage read failed" }
        return mediaProfile
    }
    fun setMediaAudioProfile(value: MediaAudioProfile) {
        check(!failMediaWrite) { "media storage write failed" }
        mediaProfile = value; mediaSaves.add(value)
    }
    fun getRadioCaptureStereo() = stereo
    fun getRadioMicSource() = micSource
    val incomingCallOptions = MutableStateFlow(List(5) { false })
    val builds = AtomicInteger()
    val inBuild = AtomicInteger()
    val maximumConcurrentBuilds = AtomicInteger()
    private val workers: MutableSet<Thread> = Collections.synchronizedSet(mutableSetOf<Thread>())
    @Volatile var interfaces = listOf("wlan0" to "192.0.2.1")
    @Volatile var onBuild: (Int) -> String = { "config-$it" }

    fun getAvailableInterfaces(): List<Pair<String, String>> { workers.add(Thread.currentThread()); return interfaces }
    fun buildConfigText(): Deferred<String> {
        val result = CompletableDeferred<String>()
        maximumConcurrentBuilds.accumulateAndGet(inBuild.incrementAndGet(), ::maxOf)
        try { result.complete(onBuild(builds.incrementAndGet()))
        } catch (e: Throwable) { result.completeExceptionally(e)
        } finally { inBuild.decrementAndGet() }
        return result
    }
    fun joinWorkers() {
        val snapshot = synchronized(workers) { workers.toList() }
        snapshot.forEach { it.join(2_000); check(!it.isAlive) { "config worker did not finish" } }
    }
}
