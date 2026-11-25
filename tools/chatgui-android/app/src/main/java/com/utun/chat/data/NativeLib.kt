package com.utun.chat.data

import com.utun.chat.ChatApplication
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

interface ChatEventCallback {
    fun onEvent(type: Int, data: ByteArray?)
}

/** JNI API. start/stop/restart/resetDatabase сериализованы lifecycle mutex и вызываются из coroutine.
 * Stop сначала ждёт frontend audio/video owners, затем join worker на IO; isReady отражает native ready.
 * Очередь JNI подтверждает приём команды, результат операции приходит отдельным CHAT_EVT_*.
 * JSON snapshots читаются в worker синхронно: тяжёлые getters вызывать на IO. */
object NativeLib {
    external fun nativeClipboardSend(request: Long, type: Int, data: ByteArray, group: ByteArray?, peer: ByteArray?): Boolean
    external fun nativeClipboardApplied(header: ByteArray, success: Boolean): Boolean
    /** Preview без token; применение с token снимка и JSON-массивом decimal ID для переподписи. Результат — событие74. */
    external fun nativeGroupPolicy(id: Long, group: String, all: Boolean, token: String?, keep: String): Boolean
    /** Очередь команды; commit/отказ возвращает событие76 с тем же id/group. name=null — снимок. */
    external fun nativeGroupInfo(id: Long, group: String, name: String?, token: String?, clipboardMembers: Int): Boolean
    @Volatile private var coreStopping = false
    @Volatile private var eventCallback: ChatEventCallback? = null
    private val lifecycle = Mutex()
    private var initialized = false // Только под lifecycle; callbacks нельзя менять у ещё живого worker.
    fun isCoreStopping(): Boolean = coreStopping

    init {
        System.loadLibrary("utun")
    }

    /** Только IO под lifecycle, до первого worker либо после подтверждённого nativeStop. */
    private fun initCallbacks(dbPath: String, controlPort: Int): Boolean {
        val provider = ChatApplication.instance.configProvider
        val settings = provider.getVoiceSettings()
        val noiseSuppression = provider.getNoiseSuppressionSettings()
        val ok = nativeInit(
            dbPath, controlPort,
            provider,
            LogManager
        )
        if (!ok) {
            LogManager.addLog("ERROR", "AudioSettings", "Native initialization failed; voice settings not applied")
            return false
        }
        eventCallback?.let { nativeSetEventCallback(it) }
        nativeVoiceSetPreset(settings.preset)
        nativeVoiceSetCompressor(settings.compressorEnabled)
        nativeVoiceSetCompressorConfig(settings.maxGainDb, settings.lookbackMs, settings.lookaheadMs,
            settings.riseRate.toFloat(), settings.targetLevelDb.toFloat(), settings.gainOnlyOnSpeech, settings.vadMaxGainDb)
        if (!setNoiseSuppression(noiseSuppression)) {
            LogManager.addLog("ERROR", "AudioSettings", "Noise suppression initialization failed: $noiseSuppression")
            return false
        }
        LogManager.addLog("INFO", "AudioSettings", "Applied at initialization: $settings")
        initialized = true
        return true
    }

    /** Инициализация callbacks и создание worker — одна операция; существующий worker сохраняет callbacks до join. */
    suspend fun start(dbPath: String, controlPort: Int, configText: String): Boolean = lifecycle.withLock {
        if (coreStopping) { LogManager.addLog("ERROR", "Core", "Start rejected: previous frontend cleanup pending"); return@withLock false }
        if (isReady()) return@withLock true
        withContext(Dispatchers.IO) { (initialized || initCallbacks(dbPath, controlPort)) && nativeStart(configText) }
    }

    /** Снимает frontend media на main, дожидается worker на IO даже до ready; ошибка запрещает следующий init. */
    suspend fun stop() = lifecycle.withLock {
        prepareForCoreStop()
        check(withContext(Dispatchers.IO) { nativeStop() }) { "native worker stop failed" }
        initialized = false
        withContext(Dispatchers.Main.immediate) { ChatApplication.instance.memberFiles.coreStopped() }
        coreStopping = false
    }

    fun setEventCallback(cb: ChatEventCallback) {
        nativeSetEventCallback(cb)
        eventCallback = cb
    }

    /** Как stop, дополнительно освобождает JNI global refs только после подтверждённого join. */
    suspend fun destroy() = lifecycle.withLock {
        prepareForCoreStop()
        check(withContext(Dispatchers.IO) { nativeDestroy() }) { "native worker destroy failed" }
        initialized = false
        withContext(Dispatchers.Main.immediate) { ChatApplication.instance.memberFiles.coreStopped() }
        coreStopping = false
    }

    suspend fun restart(dbPath: String, controlPort: Int, configText: String): Boolean = lifecycle.withLock {
        var accepted = false
        try {
            prepareForCoreStop()
            withContext(Dispatchers.Main.immediate) { com.utun.chat.headless.HeadlessService.onCoreRestarting() }
            accepted = withContext(Dispatchers.IO) { (initialized || initCallbacks(dbPath, controlPort)) && nativeRestart(configText) }
            accepted
        } finally { resumeAfterRejectedChange(accepted) }
    }

    /** После HeadlessService.stopAndJoin. Под тем же mutex, что и start, исключаем гонку со стартом worker. */
    suspend fun replaceStoppedKeys(directory: String): Int = lifecycle.withLock {
        withContext(Dispatchers.IO) { nativeReplaceStoppedKeys(directory) }
    }

    /** Тот же mutex исключает start/stop между join, удалением БД и восстановлением worker.
     * afterStop выполняется на main после join; configText=null оставляет ядро выключенным. */
    suspend fun resetDatabase(dbPath: String, controlPort: Int, configText: String?, afterStop: () -> Unit) = lifecycle.withLock {
        prepareForCoreStop()
        if (configText != null) withContext(Dispatchers.Main.immediate) { com.utun.chat.headless.HeadlessService.onCoreRestarting() }
        check(withContext(Dispatchers.IO) { nativeStop() }) { "native stop before database reset failed" }
        initialized = false
        withContext(Dispatchers.Main.immediate) { ChatApplication.instance.memberFiles.coreStopped(); afterStop() }
        val error = withContext(Dispatchers.IO) { nativeClearDatabase(dbPath) }
        if (error != null) {
            LogManager.addLog("ERROR", "Core", "Database cleanup failed directory=$dbPath: $error")
            throw java.io.IOException("$dbPath: $error")
        }
        if (configText != null) {
            check(withContext(Dispatchers.Main.immediate) { com.utun.chat.headless.HeadlessService.isActive() }) {
                "service stopped before database reset recovery"
            }
            check(withContext(Dispatchers.IO) { initCallbacks(dbPath, controlPort) && nativeStart(configText) }) {
                "native start after database reset failed"
            }
        }
        coreStopping = false
    }

    private fun resumeAfterRejectedChange(accepted: Boolean) {
        coreStopping = false
        if (!accepted && nativeIsRunning()) com.utun.chat.AppEventHandler.onEvent(12, null)
    }

    /** Снять frontend-владельцев перед stop/restart/сменой ключей. Неподтверждённый CallService либо ещё
     * активный mediaPreparation запрещает native stop с ошибкой; Main остаётся доступен для retry. */
    private suspend fun prepareForCoreStop() {
        val radioStopped = withContext(Dispatchers.Main.immediate) {
            coreStopping = true
            val radio = RadioAudioEngine.instance().stop()
            AudioRecorderManager.stopAll()
            RadioCallController.reset()
            StreamController.reset(); CallController.reset(); ChatApplication.instance.avatars.stop(); ChatApplication.instance.memberFiles.cancel()
            radio
        }
        LogManager.addLog("INFO", "Core", "Waiting for radio audio cleanup")
        withContext(Dispatchers.IO) { radioStopped.get() }
        withContext(Dispatchers.Main.immediate) {
            CallService.requireReleased() // Не останавливать native при неподтверждённой очистке его media-владельца.
            StreamPlayerEngine.stopForCore()
            StreamController.requireReleased()
            AudioRecorderManager.requireReleasedForCore()
            check(!CallController.blocksMedia()) {
                LogManager.addLog("ERROR", "Call", "Core teardown blocked: call media preparation still running")
                "Call media preparation still running"
            }
        }
        LogManager.addLog("INFO", "Core", "Waiting for shared media audio cleanup")
        withContext(Dispatchers.IO) { MediaAudioController.awaitIdle() }
    }

    /** Лёгкое обновление сокетов (auto_sockets=android) без полного рестарта ядра */
    fun updateSockets(configText: String): Boolean {
        return nativeUpdateSockets(configText)
    }

    fun isReady(): Boolean = nativeIsRunning()

    fun getMySigningKey(): ByteArray? = nativeGetMySigningKey()
    fun getMySigningAddress(): String = getMySigningKey()?.let(MemberFilesProtocol::addressHex).orEmpty()

    fun requestInviteCandidates(channelId: String): Boolean = nativeRequestInviteCandidates(channelId)
    fun requestInviteLink(channelId: String, target: String, requestId: Long): Boolean =
        MemberFilesProtocol.validAddress(target) && nativeRequestInviteLink(channelId, MemberFilesProtocol.addressBytes(target), requestId)
    fun cancelInviteLink(requestId: Long): Boolean = nativeCancelInviteLink(requestId)
    fun requestLocalDevices(): Boolean = nativeRequestLocalDevices()
    fun sendLocalInvite(peer: ByteArray, request: Long, link: String): Boolean =
        nativeSendLocalInvite(peer, request, link.toByteArray(Charsets.UTF_8))

    fun ping() { nativePing() }
    fun requestCpuLoad(): Boolean = nativeRequestCpuLoad()
    fun isResponsive(): Boolean = nativeIsResponsive()

    /* ── App foreground/background (Android lifecycle) ── */
    /** true — запрос ACTIVE/STANDBY поставлен живому worker; применение асинхронно и учитывает stream hold ядра. */
    fun setActive(active: Boolean): Boolean = nativeSetActive(active)

    /* ── External network activity (ConnectivityManager) ── */
    fun notifyNetworkActive() { nativeNotifyNetworkActive() }

    fun sendMessage(channelId: String, text: String, replyTo: String = "", requestId: Long = 0, replyQuote: String = ""): Boolean =
        nativeSendMessage(channelId, text.toByteArray(Charsets.UTF_8), messageIdBytes(replyTo), requestId, replyQuote.toByteArray(Charsets.UTF_8))

    fun editMessage(channelId: String, id: String, version: Long, text: String, requestId: Long): Boolean =
        nativeEditMessage(channelId, messageIdBytes(id), version, text.toByteArray(Charsets.UTF_8), requestId)
    fun createChannel(name: String, channelId: String, groupType: String = "chat") { nativeCreateChannel(name, channelId, groupType) }
    fun kanbanRequest(group: String, request: Long, json: String): Boolean = nativeKanbanRequest(group, request, json)

    /** Подтверждение локального commit через MSG_SUBMITTED, как у общего composer. */
    fun kanbanMessageSend(group: String, card: String, text: String, request: Long): Boolean = nativeKanbanMessageSend(group, request,
        org.json.JSONObject().put("action", "message_create").put("entity", card).put("text", text).toString())

    /** Только собственный cache-файл; после true его очисткой владеет общий attachment worker. */
    fun kanbanAttachmentSend(group: String, card: String, path: String, type: String, width: Int = 0, height: Int = 0,
                             durationMs: Int = 0, request: Long): Boolean =
        nativeKanbanAttachmentSend(group, card, path, type, width, height, durationMs, request)
    fun connectNode(address: String, port: Int, pubkeyHex: String) { nativeConnectNode(address, port, pubkeyHex) }
    private val joinSequence = java.util.concurrent.atomic.AtomicLong(1)
    fun nextJoinRequest(): Long = joinSequence.getAndIncrement()
    fun joinChannel(link: String, request: Long): Int = nativeJoinChannel(link, request)
    fun addServer(channel: String, link: String, request: Long): Int = nativeAddServer(channel, link, request)
    fun decodeInvite(link: String): String? = nativeDecodeInvite(link)
    external fun decodeObjectLink(link: String): String?
    external fun buildObjectLink(group: String, card: String, message: String, groupLabel: String, cardLabel: String, messageLabel: String): String?
    fun deriveNodeId(pubkey: ByteArray): Long = nativeDeriveNodeId(pubkey)
    fun connectChannel(channelId: String) { nativeConnectChannel(channelId) }
    fun connectMemberNode(channelId: String, ed25519: String) { nativeConnectMemberNode(channelId, ed25519) }
    fun getChannels(): String = checkNotNull(nativeGetChannels()) { "Channel snapshot unavailable" }
    fun getMessages(channelId: String, limit: Int = 50, anchor: String = "", exact: Boolean = false): String =
        checkNotNull(nativeGetMessages(channelId, limit, messageIdBytes(anchor), exact)) { "Message snapshot unavailable channel=$channelId" }
    fun feedbackCommand(json: String, requestId: Long = 0): Boolean = nativeFeedbackCommand(json.toByteArray(Charsets.UTF_8), requestId)
    fun setDebugLevel(category: String, level: String) { nativeSetDebugLevel(category, level) }
    fun getDebugCategories(): List<String> = nativeGetDebugCategories()?.split(",")?.filter { it.isNotEmpty() } ?: emptyList()

    /* ── Chat settings (live update via C side) ── */
    fun setChatSetting(name: String, value: String) { nativeSetChatSetting(name, value) }
    fun getChatSettingInt(name: String, def: Int = 0): Int = nativeGetChatSettingInt(name, def)

    /* ── Per-channel «play sound on message» ── */
    fun getSoundOnMessage(chId: String): Boolean = nativeGetSoundOnMessage(chId) != 0
    fun setSoundOnMessage(chId: String, on: Boolean) { nativeSetSoundOnMessage(chId, on) }

    /** Переключает пересылку native-логов в LogManager, сохраняя файл/UDP/logcat C-ядра.
     * init включает callback; Activity включает подписку на экране логов и отключает при выходе.
     * Уже начатая доставка может завершиться после отключения; stop снимает пересылку до следующего init. */
    fun subscribeNativeLogs(enabled: Boolean) { nativeSubscribeLogs(enabled) }
    fun setUdpLogTarget(ip: String, port: Int) { nativeSetUdpLogTarget(ip, port) }
    /** Принимает полный INI для пересоздания ядра с новой сохранённой идентичностью; true подтверждает приём запроса. */
    suspend fun regenerateKeys(configText: String): Boolean = lifecycle.withLock {
        var accepted = false
        try {
            prepareForCoreStop()
            withContext(Dispatchers.Main.immediate) { com.utun.chat.headless.HeadlessService.onCoreRestarting() }
            accepted = withContext(Dispatchers.IO) { nativeRegenerateKeys(configText) }
            accepted
        } finally { resumeAfterRejectedChange(accepted) }
    }

    /* ── Channel members ── */
    fun getChannelMembers(channelId: String): String =
        checkNotNull(nativeGetChannelMembers(channelId)) { "Member snapshot unavailable channel=$channelId" }
    /** Асинхронный live-снимок; ответ — событие MEMBER_LIST. */
    fun requestMemberList(channelId: String): Boolean = nativeRequestMemberList(channelId)
    fun requestGroupAvatar(id: Long, group: String): Boolean = nativeAvatarOperation(0, id, MemberFilesProtocol.addressBytes(group), null, null, null, null, null)
    fun transferAvatar(id: Long, group: String, server: String, owner: String, remote: String, local: String,
                       upload: Boolean = false, maxBytes: Int = AvatarProtocol.MAX_ORIGINAL): Boolean =
        nativeAvatarOperation(if (upload) 2 else 1, id, MemberFilesProtocol.addressBytes(group), remote, local,
            java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN).putLong(maxBytes.toLong()).array(), MemberFilesProtocol.addressBytes(server), MemberFilesProtocol.addressBytes(owner))
    fun avatarList(id: Long, group: String, server: String, owner: String, cursor: Long, version: ByteArray): Boolean =
        nativeAvatarOperation(8, id, MemberFilesProtocol.addressBytes(group), "avatars", null,
            java.nio.ByteBuffer.allocate(24).order(java.nio.ByteOrder.LITTLE_ENDIAN).putLong(cursor).put(version).array(), MemberFilesProtocol.addressBytes(server), MemberFilesProtocol.addressBytes(owner))
    fun avatarMkdir(id: Long, group: String, server: String, owner: String): Boolean =
        nativeAvatarOperation(3, id, MemberFilesProtocol.addressBytes(group), "avatars", null, null, MemberFilesProtocol.addressBytes(server), MemberFilesProtocol.addressBytes(owner))
    fun avatarStat(id: Long, group: String, server: String, owner: String, remote: String): Boolean =
        nativeAvatarOperation(4, id, MemberFilesProtocol.addressBytes(group), remote, null, null, MemberFilesProtocol.addressBytes(server), MemberFilesProtocol.addressBytes(owner))
    fun setMemberAvatar(id: Long, group: String, server: String, file: String): Boolean =
        nativeAvatarOperation(5, id, MemberFilesProtocol.addressBytes(group), file, null, null, server.takeIf { it.isNotEmpty() }?.let(MemberFilesProtocol::addressBytes), null)
    fun setGroupAvatar(id: Long, group: String, data: ByteArray): Boolean =
        nativeAvatarOperation(6, id, MemberFilesProtocol.addressBytes(group), null, null, data, null, null)
    fun cancelAvatar(id: Long): Boolean = nativeAvatarOperation(7, id, null, null, null, null, null, null)
    fun fileRequest(group: String, server: String, wire: ByteArray): Boolean = nativeFileRequest(MemberFilesProtocol.addressBytes(group), MemberFilesProtocol.addressBytes(server), wire)
    fun fileTransfer(id: Long, group: String, server: String, owner: String, remote: String, local: String,
                     upload: Boolean, version: ByteArray): Boolean =
        nativeFileTransfer(id, MemberFilesProtocol.addressBytes(group), MemberFilesProtocol.addressBytes(server), MemberFilesProtocol.addressBytes(owner), remote.toByteArray(), local.toByteArray(), upload, version)
    fun fileCancel(id: Long): Boolean = nativeFileCancel(id)
    private external fun nativeFileRequest(group: ByteArray, server: ByteArray, wire: ByteArray): Boolean
    private external fun nativeFileTransfer(id: Long, group: ByteArray, server: ByteArray, owner: ByteArray, remote: ByteArray,
                                           local: ByteArray, upload: Boolean, version: ByteArray): Boolean
    private external fun nativeFileCancel(id: Long): Boolean
    fun getMemberDetail(channelId: String, peer: String): String =
        checkNotNull(nativeGetMemberDetail(channelId, peer)) { "Member detail unavailable channel=$channelId peer=$peer" }
    fun setMemberNick(channelId: String, ed: String, nick: String) { nativeSetMemberNick(channelId, ed, nick) }
    fun toggleMemberRadioMute(channelId: String, address: String): Boolean =
        MemberFilesProtocol.validAddress(address) && nativeToggleMemberRadioMute(channelId, address)
    fun isChannelOwner(channelId: String): Boolean = nativeIsChannelOwner(channelId)
    fun setMemberFlags(channelId: String, ed: String, admin: Boolean, supernode: Boolean, moder: Boolean, storage: Boolean,
                       verified: Boolean, deleted: Boolean) {
        nativeSetMemberFlags(channelId, ed, admin, supernode, moder, storage, verified, deleted)
    }
    fun transferAdmin(channelId: String, ed25519: String) { nativeTransferAdmin(channelId, ed25519) }
    fun deleteChannel(channelId: String) { nativeDeleteChannel(channelId) }
    fun getMemberLinks(peer: String): String = checkNotNull(nativeGetMemberLinks(peer)) { "Member links unavailable peer=$peer" }
    /** Свежие BLE-соседи; синхронный запрос worker, вызывать на IO. Адрес обновляется каждым объявлением. */
    fun getBluetoothPeers(): String = checkNotNull(nativeGetBluetoothPeers()) { "Bluetooth devices snapshot unavailable" }

    /* ── Local sockets (for auto-mode display) ── */
    fun requestLocalSockets() = nativeRequestLocalSockets()

    /* ── Voice recording ── */
    fun voiceStart(channelId: String, replyTo: String = "", requestId: Long = 0, generation: Long = 0, replyQuote: String = ""): Boolean {
        if (coreStopping || (channelId != "__test__" && !isReady()) || StreamController.blocksCapture() || CallController.blocksMedia()) {
            LogManager.addLog("WARN", "Voice", "Capture rejected target=$channelId coreStopping=$coreStopping; core/media unavailable")
            return false
        }
        return nativeVoiceStart(channelId, messageIdBytes(replyTo), requestId, generation, replyQuote.toByteArray(Charsets.UTF_8))
    }
    fun voiceFeed(samples: ShortArray): Boolean = nativeVoiceFeed(samples)
    fun voiceStop(): Int = nativeVoiceStop()
    fun voiceCancel() { nativeVoiceCancel() }
    fun voiceIsActive(): Boolean = nativeVoiceIsActive()
    fun voiceSetPreset(preset: Int) { nativeVoiceSetPreset(preset) }
    fun voiceGetPreset(): Int = nativeVoiceGetPreset()
    fun voiceSetCompressor(enabled: Boolean) { nativeVoiceSetCompressor(enabled) }
    fun voiceGetCompressor(): Boolean = nativeVoiceGetCompressor()
    fun voiceGetPeakLevel(): Float = nativeVoiceGetPeakLevel()
    fun voiceSetCompressorConfig(maxGainDb: Int, lookbackMs: Int, lookaheadMs: Int,
                                  riseRate: Float, targetLevelDb: Float, gainOnlyOnSpeech: Boolean, vadMaxGainDb: Int) {
        nativeVoiceSetCompressorConfig(maxGainDb, lookbackMs, lookaheadMs, riseRate, targetLevelDb, gainOnlyOnSpeech, vadMaxGainDb)
    }

    /* ── Attachment ── */
    fun attachmentSend(channelId: String, filePath: String, replyTo: String = "", requestId: Long = 0, replyQuote: String = ""): Boolean =
        nativeAttachmentSend(channelId, filePath, messageIdBytes(replyTo), requestId, replyQuote.toByteArray(Charsets.UTF_8))
    fun attachmentDownload(channelId: String, msgId: Long): Boolean = nativeAttachmentDownload(channelId, msgId)

    /* ── Video message ── */
    fun videoSend(channelId: String, filePath: String, durationMs: Int, width: Int, height: Int,
                  replyTo: String = "", requestId: Long = 0, generation: Long = 0, replyQuote: String = ""): Boolean =
        nativeVideoSend(channelId, filePath, durationMs, width, height, messageIdBytes(replyTo), requestId, generation,
            replyQuote.toByteArray(Charsets.UTF_8))

    /* Live media используют отдельную ограниченную очередь, минуя чатовый event bus. */
    fun streamOpen(id: Long, group: String, source: String = "", width: Int = 0, height: Int = 0,
                   rotation: Int = 0, audio: Boolean = true, epoch: Long = 0, videoCodec: Int = 1): Boolean {
        if (coreStopping) { LogManager.addLog("WARN", "stream", "Open rejected during core teardown id=$id group=$group"); return false }
        return nativeStreamOpen(id, MemberFilesProtocol.addressBytes(group), source.takeIf { it.isNotEmpty() }?.let(MemberFilesProtocol::addressBytes), width, height, rotation, audio, epoch, videoCodec)
    }
    fun streamWatch(group: String?): Boolean {
        if (coreStopping) { LogManager.addLog("WARN", "stream", "Watch rejected during core teardown group=$group"); return false }
        return nativeStreamWatch(group?.let(MemberFilesProtocol::addressBytes))
    }
    fun streamFeed(id: Long, video: Boolean, pts: Long, key: Boolean, data: ByteArray, revision: Long = 0,
                   width: Int = 0, height: Int = 0, rotation: Int = 0, audio: Boolean = true, videoCodec: Int = 1): Int =
        nativeStreamFeed(id, video, pts, key, data, revision, width, height, rotation, audio, videoCodec)
    fun streamTxStats(id: Long): LongArray? = nativeStreamTxStats(id)
    fun streamSetAudioEnabled(id: Long, enabled: Boolean): Boolean = nativeStreamSetAudioEnabled(id, enabled)
    fun streamVideoRate(id: Long, automatic: Boolean, nominal: Int, minimum: Int): Int =
        nativeStreamVideoRate(id, automatic, nominal, minimum)
    fun streamStop(id: Long) { nativeStreamStop(id) }
    fun streamPull(id: Long, positionUs: Long = -1, video: Int = 1, audioAheadUs: Long = 80000, records: Int = 1): ByteArray? =
        nativeStreamPull(id, positionUs, video, audioAheadUs, records)
    fun streamStatus(id: Long): LongArray? = nativeStreamStatus(id)
    fun streamTelemetry(id: Long): ByteArray? = nativeStreamTelemetry(id)
    external fun videoLog(trace: Boolean, message: String)
    fun streamResync(id: Long): Boolean = nativeStreamResync(id)
    external fun streamSignalCreate(id: Long): Long
    external fun streamSignalNotify(handle: Long)
    external fun streamSignalWait(handle: Long, timeoutMs: Int): Int
    external fun streamSignalClose(handle: Long)
    fun streamPlaybackCreate(id: Long, pts: Long, audio: Boolean): Long {
        if (coreStopping) { LogManager.addLog("WARN", "stream", "Playback rejected during core teardown id=$id"); return 0 }
        return nativeStreamPlaybackCreate(id, pts, audio, StreamPlayerEngine.lowLatency)
    }
    external fun streamPlaybackBuffering(handle: Long): Boolean
    fun streamPlaybackFeed(handle: Long, pts: Long, packet: ByteArray): Int = nativeStreamPlaybackFeed(handle, pts, packet)
    fun streamPlaybackPrepare(handle: Long): Int = nativeStreamPlaybackPrepare(handle)
    fun streamPlaybackBuffered(handle: Long): Int = nativeStreamPlaybackBuffered(handle)
    fun streamPlaybackPull(handle: Long, pcm: ShortArray, frames: Int, limit: Long): Int =
        nativeStreamPlaybackPull(handle, pcm, frames, limit)
    fun streamPlaybackSilence(handle: Long, frames: Int) { nativeStreamPlaybackSilence(handle, frames) }
    fun streamPlaybackUpdate(handle: Long, now: Long, played: Long, ready: Long, guard: Long, starved: Boolean, sourceFinished: Boolean = false): LongArray? =
        nativeStreamPlaybackUpdate(handle, now, played, ready, guard, starved, StreamPlayerEngine.lowLatency, sourceFinished)
    fun streamPlaybackFinish(handle: Long) { nativeStreamPlaybackFinish(handle) }
    fun streamPlaybackEmpty(handle: Long): Boolean = nativeStreamPlaybackEmpty(handle)
    fun streamPlaybackNeedsInput(handle: Long): Boolean = nativeStreamPlaybackNeedsInput(handle)
    fun streamPlaybackClose(handle: Long) { nativeStreamPlaybackClose(handle) }
    fun streamOpusCreate(encode: Boolean): Long = nativeStreamOpusCreate(encode)
    fun streamOpusEncode(handle: Long, pcm: ShortArray, compressor: RadioCompressorSettings): ByteArray? =
        nativeStreamOpusEncode(handle, pcm, compressor.enabled, compressor.maxGainDb, compressor.riseRate)
    fun streamOpusDecode(handle: Long, packet: ByteArray): ShortArray? = nativeStreamOpusDecode(handle, packet)
    fun streamOpusClose(handle: Long) { nativeStreamOpusClose(handle) }
    private external fun nativeStreamOpen(id: Long, group: ByteArray, source: ByteArray?, width: Int, height: Int, rotation: Int,
                                         audio: Boolean, epoch: Long, videoCodec: Int): Boolean
    private external fun nativeStreamWatch(group: ByteArray?): Boolean
    private external fun nativeStreamFeed(id: Long, video: Boolean, pts: Long, key: Boolean, data: ByteArray,
                                         revision: Long, width: Int, height: Int, rotation: Int, audio: Boolean, videoCodec: Int): Int
    private external fun nativeStreamTxStats(id: Long): LongArray?
    private external fun nativeStreamSetAudioEnabled(id: Long, enabled: Boolean): Boolean
    private external fun nativeStreamVideoRate(id: Long, automatic: Boolean, nominal: Int, minimum: Int): Int
    private external fun nativeStreamStop(id: Long)
    private external fun nativeStreamPull(id: Long, position: Long, video: Int, audioAhead: Long, records: Int): ByteArray?
    private external fun nativeStreamStatus(id: Long): LongArray?
    private external fun nativeStreamTelemetry(id: Long): ByteArray?
    private external fun nativeStreamResync(id: Long): Boolean
    private external fun nativeStreamPlaybackCreate(id: Long, pts: Long, audio: Boolean, lowLatency: Boolean): Long
    private external fun nativeStreamPlaybackFeed(handle: Long, pts: Long, packet: ByteArray): Int
    private external fun nativeStreamPlaybackPrepare(handle: Long): Int
    private external fun nativeStreamPlaybackBuffered(handle: Long): Int
    private external fun nativeStreamPlaybackPull(handle: Long, pcm: ShortArray, frames: Int, limit: Long): Int
    private external fun nativeStreamPlaybackSilence(handle: Long, frames: Int)
    private external fun nativeStreamPlaybackUpdate(handle: Long, now: Long, played: Long, ready: Long,
                                                   guard: Long, starved: Boolean, lowLatency: Boolean, sourceFinished: Boolean): LongArray?
    private external fun nativeStreamPlaybackFinish(handle: Long)
    private external fun nativeStreamPlaybackEmpty(handle: Long): Boolean
    private external fun nativeStreamPlaybackNeedsInput(handle: Long): Boolean
    private external fun nativeStreamPlaybackClose(handle: Long)
    private external fun nativeStreamOpusCreate(encode: Boolean): Long
    private external fun nativeStreamOpusEncode(handle: Long, pcm: ShortArray, compressor: Boolean, maxGainDb: Int, riseRate: Int): ByteArray?
    private external fun nativeStreamOpusDecode(handle: Long, packet: ByteArray): ShortArray?
    private external fun nativeStreamOpusClose(handle: Long)

    /* ── Photo message ── */
    fun photoSend(channelId: String, filePath: String, width: Int, height: Int, contentType: String,
                  replyTo: String = "", requestId: Long = 0, generation: Long = 0, replyQuote: String = ""): Boolean =
        nativePhotoSend(channelId, filePath, width, height, messageIdBytes(replyTo), requestId, contentType, generation,
            replyQuote.toByteArray(Charsets.UTF_8))

    /* ── Voice playback ── */
    fun voiceDecodeOpen(filePath: String, outInfo: IntArray): Long = nativeVoiceDecodeOpen(filePath, outInfo)
    fun voiceDecodeRead(handle: Long, buf: ShortArray): Int = nativeVoiceDecodeRead(handle, buf)
    fun voiceDecodeClose(handle: Long) { nativeVoiceDecodeClose(handle) }

    /* ── P2P аудио-звонок (сигналинг) ── */
    /** true — копия команды принята живым worker; результат согласования приходит CALL_* событием. */
    fun callStart(channelId: String, peerAddress: String?, callId: Long, devices: Boolean = false): Boolean {
        if (coreStopping) { LogManager.addLog("WARN", "Call", "Start rejected during core teardown id=$callId"); return false }
        return nativeCallStart(channelId, peerAddress, callId, devices)
    }
    fun callAccept(callId: Long): Boolean {
        if (coreStopping) { LogManager.addLog("WARN", "Call", "Accept rejected during core teardown id=$callId"); return false }
        return nativeCallAccept(callId)
    }
    fun callDecline(callId: Long): Boolean = nativeCallDecline(callId)
    fun callHangup(callId: Long): Boolean = nativeCallHangup(callId)
    fun callContinue(callId: Long): Boolean = nativeCallContinue(callId)
    /** SIM: state=0 UNKNOWN/1 IDLE/2 BUSY, policy=маска call_policy (0..31).
     * true — копия принята очередью живого worker; применение асинхронно, stop/restart может отменить её.
     * После SERVICE_STARTED монитор повторяет актуальное состояние. false — отказ с native-диагностикой. */
    fun setPhoneState(state: Int, policy: Int): Boolean = nativeSetPhoneState(state, policy)
    /** Адресное приглашение в рацию: action — radio_call_action (1..8), id — ненулевые uint64-биты Long.
     * Для START (1) group/peer также ненулевые; прочие действия используют id.
     * true — копия команды принята очередью живого uasync; результат сигналинга приходит событием 65
     * (id/group/peer:8LE + phase/reason/incoming:1). false — отказ с native-логом; restart отменяет старые запросы. */
    fun radioCall(id: Long, action: Int, group: String? = null, peer: String? = null, devices: Boolean = false): Boolean =
        nativeRadioCall(id, group, peer, action, devices)
    private external fun nativeRadioCall(id: Long, group: String?, peer: String?, action: Int, devices: Boolean): Boolean

    /* ── P2P аудио-звонок (медиа) ── */
    fun callAudioStart(callId: Long): Boolean {
        if (coreStopping) { LogManager.addLog("WARN", "Call", "Audio start rejected during core teardown id=$callId"); return false }
        return nativeCallAudioStart(callId)
    }
    /** Настоящий raw PCM и mute кадра: зануление выполняется в core после AEC/NS/AGC. */
    fun callAudioFeed(callId: Long, samples: ShortArray, muted: Boolean, capturePtsUs: Long): Boolean =
        nativeCallAudioFeed(callId, samples, muted, capturePtsUs)
    fun callAudioPull(callId: Long, out: ShortArray): Int = nativeCallAudioPull(callId, out)
    fun callAudioProgress(callId: Long, epoch: Long, position: Long, atUs: Long, nowUs: Long, measured: Boolean) =
        nativeCallAudioProgress(callId, epoch, position, atUs, nowUs, measured)
    /** out: epoch, source_epoch, played, written, pts_us, valid, measured, held_back_us. */
    fun callAudioClock(callId: Long, nowUs: Long, out: LongArray): Boolean = nativeCallAudioClock(callId, nowUs, out)
    /** call=debug: encoded_ms, processing_ms, reserve_ms, spread_ms, pcm_samples, tempo_x100, underruns, waiting. */
    fun callAudioDiagnostics(callId: Long, out: LongArray): Boolean = nativeCallAudioDiagnostics(callId, out)
    /** Независимый снимок последних 30с для графиков; null после завершения аудиосессии. */
    fun callAudioHistory(callId: Long): LongArray? = if (coreStopping) null else nativeCallAudioHistory(callId)
    fun callAudioStop() { nativeCallAudioStop() }

    /* AEC (эхоподавление): включается по спикерфону; delay — измеренная задержка в кадрах. */
    fun callAudioSetAec(enabled: Boolean) { nativeCallAudioSetAec(enabled) }
    fun callAudioSetAecDelay(frames: Int) { nativeCallAudioSetAecDelay(frames) }
    /** Сохранённый DataStore-снимок: call/radio — между кадрами, voice — со следующей записи. */
    fun setNoiseSuppression(settings: NoiseSuppressionSettings): Boolean = nativeSetNoiseSuppression(settings.nativeLevel)
    fun callAudioResetCapture(callId: Long) { nativeCallAudioResetCapture(callId) }

    /* ── Рация (PTT walkie-talkie) ── */
    @Volatile private var radioRoute: Pair<String, Long>? = null
    private fun radioRouteFor(groupId: String): Long = radioRoute?.takeIf { it.first == groupId }?.second ?: 0L
    fun radioSetActive(groupId: String, on: Boolean) { nativeRadioSetActive(groupId, on) }
    fun radioSubscribers(groupId: String): Int = nativeRadioSubscribers(groupId)
    fun radioAudioStart(groupId: String, channels: Int): Boolean {
        if (coreStopping) { LogManager.addLog("WARN", "Radio", "Audio start rejected during core teardown group=$groupId"); return false }
        val route = nativeRadioAudioStart(groupId, channels)
        radioRoute = if (route != 0L) groupId to route else null
        return route != 0L
    }
    fun radioAudioFeed(groupId: String, samples: ShortArray): Boolean = nativeRadioAudioFeed(radioRouteFor(groupId), samples)
    fun radioAudioPull(groupId: String, out: ShortArray, playedFrames: Long): Int = nativeRadioAudioPull(radioRouteFor(groupId), out, playedFrames)
    fun radioAudioStop() { radioRoute = null; nativeRadioAudioStop() }
    fun radioCaptureStart(groupId: String, channels: Int, mode: Int): Boolean {
        if (coreStopping) { LogManager.addLog("WARN", "Radio", "Capture rejected during core teardown group=$groupId mode=$mode"); return false }
        return nativeRadioCaptureStart(radioRouteFor(groupId), channels, mode)
    }
    /** true — копия снимка поставлена живому worker; проверка и применение ещё не подтверждены.
     * ConfigProvider.onRadioSettingsApplied вернёт тот же requestId и результат атомарной проверки/применения.
     * false — запрос отклонён (включая отсутствие callback). Stop/restart может отменить ожидающий снимок без ACK;
     * новый запуск аудио отправляет настройки повторно. До ACK текущего запроса capture не возобновляется. */
    fun radioConfigure(requestId: Long, settings: RadioAutoSettings): Boolean = nativeRadioConfigure(requestId, settings.nativeValues())
    // dBFS, valid, RX-блокировка, вероятность речи 0..1, vadValid, усиление VAD в dB, компрессор VAD включён.
    fun radioInputStatus(): FloatArray = nativeRadioInputStatus()
    fun radioCaptureStop() { nativeRadioCaptureStop() }
    fun radioCaptureDiscontinuity(groupId: String) { nativeRadioCaptureDiscontinuity(radioRouteFor(groupId)) }
    fun radioTransmitting(): Boolean = nativeRadioTransmitting()
    fun radioAutoMode(): Int = nativeRadioAutoMode()
    fun radioTalkBegin(groupId: String) { nativeRadioTalkBegin(radioRouteFor(groupId)) }
    fun radioTalkEnd(groupId: String) { nativeRadioTalkEnd(radioRouteFor(groupId)) }

    /* ── DM (личные сообщения) ── */
    /** ID уже зафиксированной ядром беседы выбранного режима; null — отказ. */
    fun dmStart(peer: String, sourceChId: String, peerName: String, groups: Boolean = false): String? =
        nativeDmStart(peer, sourceChId, peerName, groups).takeIf { it != 0L }?.toULong()?.toString()
    fun dmMediaAction(convId: String, author: String, seq: Long, generation: Long, opened: Boolean): Boolean =
        nativeDmMediaAction(convId, author, seq, generation, opened)
    /** UTF-8 без Modified UTF-8: true подтверждает очередь, CHAT_EVT_MSG_SUBMITTED — запись истории/outbox. */
    fun dmSend(convId: String, text: String, generation: Long, requestId: Long): Boolean =
        nativeDmSend(convId, text.toByteArray(Charsets.UTF_8), generation, requestId)
    fun dmInvite(requestId: Long, peer: String, source: String, channelId: String, name: String, cancel: Boolean = false): Boolean =
        nativeDmInvite(requestId, peer, source, channelId, name, cancel)
    fun isLocalChannelMember(channelId: String): Boolean = nativeIsLocalChannelMember(channelId)
    fun dmDelete(convId: String, generation: Long, request: Long): Boolean = nativeDmDelete(convId, generation, request)
    fun dmRequestConversations() { nativeDmRequestConversations() }
    fun dmRequestCallRoute(convId: String) { nativeDmRequestCallRoute(convId) }
    fun dmRequestMessages(convId: String, generation: Long) { nativeDmRequestMessages(convId, generation) }
    /** Блокирующий снимок ядра: вызывать на Dispatchers.IO, результат привязан к conv/generation. */
    fun dmMembers(convId: String, generation: Long): ByteArray? = nativeDmMembers(convId, generation)
    fun dmSendFile(convId: String, path: String, name: String, generation: Long, requestId: Long): Boolean =
        nativeDmSendFile(convId, path, name, generation, requestId)

    private external fun nativeInit(dbPath: String, controlPort: Int, config: Any, logCb: Any): Boolean
    private external fun nativeStart(configText: String): Boolean
    private external fun nativeStop(): Boolean
    private external fun nativeRequestCpuLoad(): Boolean
    private external fun nativeSetEventCallback(cb: Any)
    private external fun nativeDestroy(): Boolean
    private external fun nativeSendMessage(channelId: String, text: ByteArray, replyTo: ByteArray, requestId: Long, replyQuote: ByteArray): Boolean
    private external fun nativeEditMessage(channelId: String, messageId: ByteArray, version: Long, text: ByteArray, requestId: Long): Boolean
    private external fun nativeCreateChannel(name: String, channelId: String, groupType: String)
    private external fun nativeKanbanRequest(group: String, request: Long, json: String): Boolean
    private external fun nativeKanbanMessageSend(group: String, request: Long, json: String): Boolean
    private external fun nativeKanbanAttachmentSend(group: String, card: String, path: String, type: String,
                                                   width: Int, height: Int, durationMs: Int, request: Long): Boolean
    private external fun nativeConnectNode(address: String, port: Int, pubkeyHex: String)
    private external fun nativeJoinChannel(link: String, request: Long): Int
    private external fun nativeDecodeInvite(link: String): String?
    private external fun nativeAddServer(channel: String, link: String, request: Long): Int
    private external fun nativeDeriveNodeId(pubkey: ByteArray): Long
    private external fun nativeConnectChannel(channelId: String)
    private external fun nativeConnectMemberNode(channelId: String, ed25519: String)
    private external fun nativeGetChannels(): String?
    private external fun nativeGetMessages(channelId: String, limit: Int, anchor: ByteArray, exact: Boolean): String?
    fun devgroupCommand(action: Int, group: String, peer: String, text: String, pin: String, requestId: Long): Boolean =
        nativeDevgroupCommand(action, group, peer, text.toByteArray(Charsets.UTF_8), pin.toByteArray(Charsets.UTF_8), requestId)
    private external fun nativeDevgroupCommand(action: Int, group: String, peer: String, text: ByteArray, pin: ByteArray, requestId: Long): Boolean
    private external fun nativeFeedbackCommand(json: ByteArray, requestId: Long): Boolean
    private external fun nativeSetDebugLevel(category: String, level: String)
    private external fun nativeGetDebugCategories(): String?
    private external fun nativeSetChatSetting(name: String, value: String)
    private external fun nativeGetChatSettingInt(name: String, def: Int): Int
    private external fun nativeGetSoundOnMessage(chId: String): Int
    private external fun nativeSetSoundOnMessage(chId: String, on: Boolean)
    private external fun nativeSubscribeLogs(enabled: Boolean)
    private external fun nativeSetUdpLogTarget(ip: String, port: Int)
    private external fun nativeReplaceStoppedKeys(directory: String): Int
    private external fun nativeRegenerateKeys(configText: String): Boolean
    private external fun nativeClearDatabase(dbPath: String): String?
    private external fun nativeGetMySigningKey(): ByteArray?
    private external fun nativeGetBluetoothPeers(): String?
    private external fun nativeRequestInviteCandidates(channelId: String): Boolean
    private external fun nativeRequestInviteLink(channelId: String, target: ByteArray, requestId: Long): Boolean
    private external fun nativeCancelInviteLink(requestId: Long): Boolean
    private external fun nativeRequestLocalDevices(): Boolean
    private external fun nativeSendLocalInvite(peer: ByteArray, request: Long, link: ByteArray): Boolean
    private external fun nativeRestart(configText: String): Boolean
    private external fun nativeUpdateSockets(configText: String): Boolean
    private external fun nativePing()
    private external fun nativeIsResponsive(): Boolean
    private external fun nativeIsRunning(): Boolean
    private external fun nativeSetActive(active: Boolean): Boolean
    private external fun nativeNotifyNetworkActive()

    /* ── Channel members JNI ── */
    private external fun nativeGetChannelMembers(channelId: String): String?
    private external fun nativeRequestMemberList(channelId: String): Boolean
    private external fun nativeAvatarOperation(op: Int, id: Long, group: ByteArray?,
        remote: String?, local: String?, data: ByteArray?, serverEd: ByteArray?, ownerEd: ByteArray?): Boolean
    private external fun nativeGetMemberDetail(channelId: String, peer: String): String?
    private external fun nativeSetMemberNick(channelId: String, ed: String, nick: String)
    private external fun nativeToggleMemberRadioMute(channelId: String, address: String): Boolean
    private external fun nativeIsChannelOwner(channelId: String): Boolean
    private external fun nativeSetMemberFlags(channelId: String, ed: String, admin: Boolean, supernode: Boolean, moder: Boolean, storage: Boolean,
                                              verified: Boolean, deleted: Boolean)
    private external fun nativeTransferAdmin(channelId: String, ed25519: String)
    private external fun nativeDeleteChannel(channelId: String)
    private external fun nativeGetMemberLinks(peer: String): String?

    /* ── Local sockets JNI ── */
    private external fun nativeRequestLocalSockets()

    /* ── Voice recording JNI ── */
    private external fun nativeVoiceStart(channelId: String, replyTo: ByteArray, requestId: Long, generation: Long, replyQuote: ByteArray): Boolean
    private external fun nativeVoiceFeed(samples: ShortArray): Boolean
    private external fun nativeVoiceStop(): Int
    private external fun nativeVoiceCancel()
    private external fun nativeVoiceIsActive(): Boolean
    private external fun nativeVoiceSetPreset(preset: Int)
    private external fun nativeVoiceGetPreset(): Int
    private external fun nativeVoiceSetCompressor(enabled: Boolean)
    private external fun nativeVoiceGetCompressor(): Boolean
    private external fun nativeVoiceGetPeakLevel(): Float
    private external fun nativeVoiceSetCompressorConfig(
        maxGainDb: Int, lookbackMs: Int, lookaheadMs: Int,
        riseRate: Float, targetLevelDb: Float, gainOnlyOnSpeech: Boolean, vadMaxGainDb: Int
    )

    /* ── Attachment JNI ── */
    private external fun nativeAttachmentSend(channelId: String, filePath: String, replyTo: ByteArray, requestId: Long, replyQuote: ByteArray): Boolean
    private external fun nativeAttachmentDownload(channelId: String, msgId: Long): Boolean

    /* ── Video message JNI ── */
    private external fun nativeVideoSend(channelId: String, filePath: String, durationMs: Int, width: Int, height: Int,
                                        replyTo: ByteArray, requestId: Long, generation: Long, replyQuote: ByteArray): Boolean

    /* ── Photo message JNI ── */
    private external fun nativePhotoSend(channelId: String, filePath: String, width: Int, height: Int,
                                          replyTo: ByteArray, requestId: Long, contentType: String, generation: Long, replyQuote: ByteArray): Boolean

    /* ── Voice playback JNI ── */
    private external fun nativeVoiceDecodeOpen(filePath: String, outInfo: IntArray): Long
    private external fun nativeVoiceDecodeRead(handle: Long, buf: ShortArray): Int
    private external fun nativeVoiceDecodeClose(handle: Long)

    /* ── P2P аудио-звонок JNI ── */
    private external fun nativeCallStart(channelId: String, peerAddress: String?, callId: Long, devices: Boolean): Boolean
    private external fun nativeCallAccept(callId: Long): Boolean
    private external fun nativeCallDecline(callId: Long): Boolean
    private external fun nativeCallHangup(callId: Long): Boolean
    private external fun nativeCallContinue(callId: Long): Boolean
    private external fun nativeSetPhoneState(state: Int, policy: Int): Boolean
    private external fun nativeCallAudioStart(callId: Long): Boolean
    private external fun nativeCallAudioFeed(callId: Long, samples: ShortArray, muted: Boolean, capturePtsUs: Long): Boolean
    private external fun nativeCallAudioPull(callId: Long, out: ShortArray): Int
    private external fun nativeCallAudioProgress(callId: Long, epoch: Long, position: Long, atUs: Long, nowUs: Long, measured: Boolean)
    private external fun nativeCallAudioClock(callId: Long, nowUs: Long, out: LongArray): Boolean
    private external fun nativeCallAudioDiagnostics(callId: Long, out: LongArray): Boolean
    private external fun nativeCallAudioHistory(callId: Long): LongArray?
    private external fun nativeCallAudioStop()
    private external fun nativeCallAudioSetAec(enabled: Boolean)
    private external fun nativeCallAudioSetAecDelay(frames: Int)
    private external fun nativeSetNoiseSuppression(suppressDb: Int): Boolean
    private external fun nativeCallAudioResetCapture(callId: Long)

    /* ── Рация JNI ── */
    private external fun nativeRadioSetActive(groupId: String, on: Boolean)
    private external fun nativeRadioSubscribers(groupId: String): Int
    private external fun nativeRadioAudioStart(groupId: String, channels: Int): Long
    private external fun nativeRadioAudioFeed(groupId: Long, samples: ShortArray): Boolean
    private external fun nativeRadioAudioPull(groupId: Long, out: ShortArray, playedFrames: Long): Int
    private external fun nativeRadioAudioStop()
    private external fun nativeRadioCaptureStart(groupId: Long, channels: Int, mode: Int): Boolean
    private external fun nativeRadioConfigure(requestId: Long, settings: IntArray): Boolean
    private external fun nativeRadioInputStatus(): FloatArray
    private external fun nativeRadioCaptureStop()
    private external fun nativeRadioCaptureDiscontinuity(groupId: Long)
    private external fun nativeRadioTransmitting(): Boolean
    private external fun nativeRadioAutoMode(): Int
    private external fun nativeRadioTalkBegin(groupId: Long)
    private external fun nativeRadioTalkEnd(groupId: Long)

    /* ── DM JNI ── */
    private external fun nativeDmDelete(convId: String, generation: Long, request: Long): Boolean
    private external fun nativeDmStart(peer: String, sourceChId: String, peerName: String, groups: Boolean): Long
    private external fun nativeDmMediaAction(convId: String, author: String, seq: Long, generation: Long, opened: Boolean): Boolean
    private external fun nativeDmSend(convId: String, text: ByteArray, generation: Long, requestId: Long): Boolean
    private external fun nativeDmInvite(requestId: Long, peer: String, source: String, channelId: String, name: String, cancel: Boolean): Boolean
    private external fun nativeIsLocalChannelMember(channelId: String): Boolean
    private external fun nativeDmRequestConversations()
    private external fun nativeDmRequestCallRoute(convId: String)
    private external fun nativeDmRequestMessages(convId: String, generation: Long)
    private external fun nativeDmMembers(convId: String, generation: Long): ByteArray?
    private external fun nativeDmSendFile(convId: String, path: String, name: String, generation: Long, requestId: Long): Boolean
}
