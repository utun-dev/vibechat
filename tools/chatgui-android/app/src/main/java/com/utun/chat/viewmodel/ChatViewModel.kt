package com.utun.chat.viewmodel

import com.utun.chat.R
import com.utun.chat.data.AppLanguage
import android.content.Context
import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import com.utun.chat.ChatApplication
import com.utun.chat.AppEventHandler
import com.utun.chat.headless.HeadlessService
import com.utun.chat.data.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.async
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream
import org.json.JSONObject
import org.json.JSONArray

private val IMAGE_EXTENSIONS = setOf("jpg", "jpeg", "png", "gif", "bmp", "webp")

/** UI-снимки каналов/PM, выбор беседы и подтверждения локальных отправок. События и команды UI обрабатываются на main.
 * Тяжёлые чтения идут через repository на IO; revision/instance guards отбрасывают устаревшие ответы.
 * Команда JNI подтверждает лишь очередь: commit отправки сверяется по уникальному request ID события.
 * Настройки/сброс БД принадлежат Application; media/controllers также переживают UI и освобождаются по ACK.
 * clearState вызывается после stop/join и убирает состояние удалённой БД. Повреждённый event логируется,
 * его ошибка не завершает подписку; новая ViewModel читает снимки вместо повтора старых команд. */
class ChatViewModel : ViewModel() {
    private companion object {
        val submissionIds = java.util.concurrent.atomic.AtomicLong(1)
        val inviteRequestIds = java.util.concurrent.atomic.AtomicLong(1)
    }
    val composerSubmissions = ConversationSubmissions(viewModelScope)

    /** После приёма ViewModel владеет cache и обеими частями отправки, даже если Activity уничтожена. */
    fun sendConversation(target: ConversationTarget, token: String, text: String, filePath: String?, name: String,
                         video: Boolean, reply: ChannelReply?): Boolean {
        val attachmentTask: (suspend () -> Boolean)? = filePath?.let { path ->
            { if (video) sendVideoMessage(path, target, reply, clearReply = false).await()
                else sendAttachment(path, target, name, reply).await() }
        }
        val textTask: (suspend () -> Boolean)? = if (text.isBlank()) null else {
            { when (target) {
                is ConversationTarget.Channel -> sendChannelText(target, text, reply).await()
                is ConversationTarget.Pm -> sendDmText(target, text).await()
                is ConversationTarget.Card -> submitMessage(target.id, null) {
                    NativeLib.kanbanMessageSend(target.id, target.cardId, text, it)
                }
            } }
        }
        return composerSubmissions.submit(target, token, attachmentTask, textTask, { finishReply(reply) }, { _messageSendFailed.value = true })
    }

    data class ChannelEdit(val channelId: String, val message: Message, val revision: Long)
    private val _editing = MutableStateFlow<ChannelEdit?>(null)
    val editing: StateFlow<ChannelEdit?> = _editing
    private var editRevision = 0L

    fun selectEdit(message: Message) {
        val channel = _currentChannel.value?.id ?: return
        if (!message.isOutgoing || message.isEncrypted || message.contentType !in setOf("text", "text/plain") ||
            !validMessageId(message.messageId) || channel in _submittingChannels.value || _isRecording.value) {
            LogManager.addLog("WARN", "Edit", "cannot open editor ch=$channel id=${message.messageId}"); return
        }
        _editing.value = ChannelEdit(channel, message, ++editRevision)
        LogManager.addLog("INFO", "Edit", "opened ch=$channel id=${message.messageId} ver=${message.version}")
    }

    fun cancelEdit(edit: ChannelEdit) { if (_editing.value == edit) _editing.value = null }

    fun saveEdit(edit: ChannelEdit, text: String): Deferred<Boolean> = viewModelScope.async {
        submitMessage(edit.channelId, null, reportError = false) {
            NativeLib.editMessage(edit.channelId, edit.message.messageId, edit.message.version, text, it)
        }.also { if (it) cancelEdit(edit) }
    }

    private val _replies = MutableStateFlow<Map<String, ChannelReply>>(emptyMap())
    val replies: StateFlow<Map<String, ChannelReply>> = _replies
    private var replyRevision = 0L
    private val submissions = mutableMapOf<Long, CompletableDeferred<Boolean>>()
    private val submissionChannels = mutableMapOf<Long, String>()
    private val _submittingChannels = MutableStateFlow<Set<String>>(emptySet())
    val submittingChannels: StateFlow<Set<String>> = _submittingChannels
    private val _messageSendFailed = MutableStateFlow(false)
    val messageSendFailed: StateFlow<Boolean> = _messageSendFailed
    private val _messageSendTooLarge = MutableStateFlow(false)
    val messageSendTooLarge: StateFlow<Boolean> = _messageSendTooLarge
    private val _replyOriginals = MutableStateFlow<Map<String, Message>>(emptyMap())
    val replyOriginals: StateFlow<Map<String, Message>> = _replyOriginals
    private val _historyAnchor = MutableStateFlow("")
    val historyAnchor: StateFlow<String> = _historyAnchor
    data class MessageJump(val messageId: String, val serial: Long)
    private val _messageJump = MutableStateFlow<MessageJump?>(null)
    val messageJump: StateFlow<MessageJump?> = _messageJump
    data class ObjectNavigation(val target: ObjectLinkTarget, val url: String, val revision: Long)
    data class ObjectLinkError(val url: String, val reason: Int, val retry: Boolean)
    private val _objectNavigation = MutableStateFlow<ObjectNavigation?>(null)
    val objectNavigation: StateFlow<ObjectNavigation?> = _objectNavigation
    private val _objectLinkError = MutableStateFlow<ObjectLinkError?>(null)
    val objectLinkError: StateFlow<ObjectLinkError?> = _objectLinkError
    private var objectLinkRevision = 0L

    fun cancelObjectNavigation() {
        ++objectLinkRevision; _objectNavigation.value = null; _objectLinkError.value = null
    }

    fun dismissObjectLinkError() { _objectLinkError.value = null }

    fun completeObjectLink(revision: Long, available: Boolean) {
        val pending = _objectNavigation.value ?: return
        if (pending.revision != revision) return
        if (!available) {
            LogManager.addLog("WARN", "LINK", "Object unavailable revision=$revision group=${pending.target.group} card=${pending.target.card} message=${pending.target.message}")
            _objectLinkError.value = ObjectLinkError(pending.url, R.string.object_link_unavailable, true)
        } else LogManager.addLog("INFO", "LINK", "Object opened revision=$revision group=${pending.target.group}")
        _objectNavigation.value = null
    }

    /** Main: общий вход ссылок из сообщения/буфера/Intent; проверка доступа и чтение идут на IO. */
    fun openLink(link: String) {
        cancelObjectNavigation()
        if (!ObjectLinks.isObjectLink(link)) { openInviteLink(link); return }
        val target = ObjectLinks.decode(link)
        if (target == null) {
            LogManager.addLog("WARN", "LINK", "Invalid object link bytes=${link.length}")
            _objectLinkError.value = ObjectLinkError(link, R.string.object_link_invalid, false); return
        }
        val r = repo
        if (r == null || !NativeLib.isReady()) {
            LogManager.addLog("ERROR", "LINK", "Core unavailable for object link")
            _objectLinkError.value = ObjectLinkError(link, R.string.invite_core_unavailable, true); return
        }
        val revision = objectLinkRevision
        viewModelScope.launch {
            val (channel, member) = withContext(Dispatchers.IO) {
                val channel = r.getChannels()?.firstOrNull { it.groupEd == target.group }
                channel to (channel != null && NativeLib.isLocalChannelMember(channel.id))
            }
            if (repo !== r || revision != objectLinkRevision) return@launch
            if (channel == null || !member) {
                LogManager.addLog("WARN", "LINK", "Group unavailable group=${target.group} member=$member")
                _objectLinkError.value = ObjectLinkError(link, R.string.object_link_join_required, false); return@launch
            }
            if (channel.groupType != if (target.kind == 1) "chat" else "kanban") {
                LogManager.addLog("WARN", "LINK", "Group type mismatch group=${target.group} type=${channel.groupType} kind=${target.kind}")
                _objectLinkError.value = ObjectLinkError(link, R.string.object_link_invalid, false); return@launch
            }
            selectChannel(channel, preserveObjectLink = true)
            _objectNavigation.value = ObjectNavigation(target, link, revision)
            LogManager.addLog("INFO", "LINK", "Object navigation revision=$revision kind=${target.kind} group=${target.group}")
        }
    }
    private var messageRefreshRevision = 0L
    private var channelRefreshRevision = 0L
    private var jumpRevision = 0L
    private var pendingLatestJump: MessageJump? = null
    private val _originalMissing = MutableStateFlow(false)
    val originalMissing: StateFlow<Boolean> = _originalMissing
    var cameraReply: ChannelReply? = null
        private set

    fun replyFor(target: ConversationTarget): ChannelReply? =
        if (target is ConversationTarget.Channel) _replies.value[target.id] else null

    fun selectReply(message: Message, quote: String = "") {
        val channel = _currentChannel.value?.id ?: return
        if (!validMessageId(message.messageId)) { LogManager.addLog("ERROR", "Reply", "invalid original ID"); return }
        if (quote.toByteArray(Charsets.UTF_8).size > 4000 || quote.contains('\u0000')) {
            LogManager.addLog("ERROR", "Reply", "invalid quote bytes=${quote.toByteArray(Charsets.UTF_8).size}")
            _messageSendFailed.value = true; return
        }
        _editing.value = null
        _replies.value = _replies.value + (channel to ChannelReply(channel, message, ++replyRevision, quote))
        LogManager.addLog("INFO", "Reply", "selected ch=$channel original=${message.messageId} quote_bytes=${quote.toByteArray(Charsets.UTF_8).size}")
    }

    fun cancelReply(channel: String) {
        _replies.value = _replies.value - channel
        LogManager.addLog("INFO", "Reply", "cancelled ch=$channel")
    }

    fun finishReply(reply: ChannelReply?) {
        if (reply != null && reply.channelId !in _submittingChannels.value && _replies.value[reply.channelId] == reply)
            cancelReply(reply.channelId)
    }

    fun captureCameraReply(target: ConversationTarget) { cameraReply = replyFor(target) }
    fun dismissMessageSendError() { _messageSendFailed.value = false; _messageSendTooLarge.value = false }
    fun dismissOriginalMissing() { _originalMissing.value = false }

    private fun submissionResult(id: Long, success: Boolean) {
        val result = submissions.remove(id)
        submissionChannels.remove(id)
        _submittingChannels.value = submissionChannels.values.toSet()
        if (result == null) { LogManager.addLog("WARN", "Reply", "unknown submission result request=$id success=$success"); return }
        LogManager.addLog(if (success) "INFO" else "ERROR", "Reply", "submission request=$id saved=$success")
        result.complete(success)
    }

    private suspend fun submitMessage(channel: String, reply: ChannelReply?, reportError: Boolean = true,
                                      queue: (Long) -> Boolean): Boolean {
        if (reply != null && reply.channelId != channel) {
            LogManager.addLog("ERROR", "Reply", "target mismatch ch=$channel reply=${reply.channelId}")
            _messageSendFailed.value = true
            return false
        }
        val id = submissionIds.getAndIncrement()
        val result = CompletableDeferred<Boolean>()
        submissions[id] = result
        submissionChannels[id] = channel
        _submittingChannels.value = submissionChannels.values.toSet()
        LogManager.addLog("INFO", "Reply", "submit ch=$channel request=$id original=${reply?.message?.messageId.orEmpty()}")
        try {
            if (!queue(id)) submissionResult(id, false)
            val success = result.await()
            if (!success && reportError) _messageSendFailed.value = true
            return success
        } catch (e: kotlinx.coroutines.CancellationException) { throw e }
        catch (e: Exception) {
            LogManager.addLog("ERROR", "Reply", "submission failed ch=$channel request=$id: ${e.message}")
            if (reportError) _messageSendFailed.value = true
            return false
        } finally {
            submissions.remove(id)
            submissionChannels.remove(id)
            _submittingChannels.value = submissionChannels.values.toSet()
        }
    }

    fun sendChannelText(target: ConversationTarget.Channel, text: String, reply: ChannelReply?): Deferred<Boolean> = viewModelScope.async {
        submitMessage(target.id, reply) { NativeLib.sendMessage(target.id, text, reply?.message?.messageId.orEmpty(), it, reply?.quote.orEmpty()) }
    }

    data class PmInviteState(val peer: String, val name: String, val source: String, val groups: List<Channel>,
                             val groupId: String = "", val requestId: Long = 0, val pending: Boolean = false, val errorRes: Int = 0)
    private val _pmInvite = MutableStateFlow<PmInviteState?>(null)
    val pmInvite: StateFlow<PmInviteState?> = _pmInvite
    private var nextPmInviteId = 0L

    data class LinkJoinState(val link: String, val channelId: String, val peer: String,
                             val pending: Boolean = true, val result: Int? = null, val request: Long = 0,
                             val serverAddition: Boolean = false)
    private val _linkJoin = MutableStateFlow<LinkJoinState?>(null)
    val linkJoin: StateFlow<LinkJoinState?> = _linkJoin
    private val _serverLink = MutableStateFlow<String?>(null)
    val serverLink: StateFlow<String?> = _serverLink
    fun dismissServerLink() { _serverLink.value = null }
    private val _joinedChannel = MutableStateFlow<Channel?>(null)
    val joinedChannel: StateFlow<Channel?> = _joinedChannel

    data class AttachmentPreparation(val jobs: Set<String> = emptySet(), val failed: Boolean = false)
    private val _attachmentPreparations = MutableStateFlow<Map<String, AttachmentPreparation>>(emptyMap())
    val attachmentPreparations: StateFlow<Map<String, AttachmentPreparation>> = _attachmentPreparations
    private val discardedPreparationJobs = mutableSetOf<String>()

    private fun preparationState(target: String, job: String, state: Int) {
        if (job in discardedPreparationJobs) { if (state != 0) discardedPreparationJobs.remove(job); return }
        _attachmentPreparations.update { map ->
            val previous = map[target] ?: AttachmentPreparation()
            val value = if (state == 0) AttachmentPreparation(previous.jobs + job)
                else previous.copy(jobs = previous.jobs - job, failed = previous.failed || state == 2)
            if (value.jobs.isEmpty() && !value.failed) map - target else map + (target to value)
        }
    }

    data class PlaybackState(
        val filePath: String? = null,
        val playing: Boolean = false,
        val paused: Boolean = false,
        val elapsedMs: Int = 0,
        val progress: Float = 0f,
        val durMs: Int = 0
    )

    private var repo: ChatRepository? = null
    private val _contentReady = MutableStateFlow(false)
    val contentReady: StateFlow<Boolean> = _contentReady
    private var dbReady: Boolean
        get() = _contentReady.value
        set(value) { _contentReady.value = value }

    val callState = CallController.state

    val radioAudio = RadioAudioEngine.instance()

    /* рация: активна ли для текущего канала, кто говорит (Ed25519, null = никто) */
    private val _radioActive = MutableStateFlow(false)
    val radioActive: StateFlow<Boolean> = _radioActive

    private val _radioTalking = MutableStateFlow<String?>(null)
    val radioTalking: StateFlow<String?> = _radioTalking
    private val _radioTalkState = MutableStateFlow(RadioTalkState())
    val radioTalkState: StateFlow<RadioTalkState> = _radioTalkState

    private var radioToggleCount = 0

    /* рация: активный канал (null = выключена). Одна рация — один канал. */
    private val radioChannelId: String? get() = radioAudio.activeGroup.value

    val muted = CallController.muted
    val audioRoute = CallController.route
    val headsetAvailable = CallController.headset
    val earpieceAvailable = CallController.earpiece
    val proximityNear = CallController.proximityNear

    private val _playbackState = MutableStateFlow(PlaybackState())
    val playbackState: StateFlow<PlaybackState> = _playbackState

    private val _channels = MutableStateFlow<List<Channel>>(emptyList())
    val channels: StateFlow<List<Channel>> = _channels

    private val _messages = MutableStateFlow<List<Message>>(emptyList())
    val messages: StateFlow<List<Message>> = _messages
    private val feedbackCache = mutableMapOf<String, FeedbackState>()
    private val _reactionCatalog = MutableStateFlow<List<ReactionState>>(emptyList())
    val reactionCatalog: StateFlow<List<ReactionState>> = _reactionCatalog
    private val _interactionFailed = MutableStateFlow(false)
    val interactionFailed: StateFlow<Boolean> = _interactionFailed
    private val _radioMuteFailed = MutableStateFlow(false)
    val radioMuteFailed: StateFlow<Boolean> = _radioMuteFailed
    private var nextInteractionId = 0L

    private val _currentChannel = MutableStateFlow<Channel?>(null)
    val currentChannel: StateFlow<Channel?> = _currentChannel
    private val _streamSources = MutableStateFlow<List<StreamSource>>(emptyList())
    val streamSources: StateFlow<List<StreamSource>> = _streamSources
    private val _streamSourcesFailed = MutableStateFlow(false)
    val streamSourcesFailed: StateFlow<Boolean> = _streamSourcesFailed
    private val _streamSelection = MutableStateFlow<StreamSource?>(null)
    val streamSelection: StateFlow<StreamSource?> = _streamSelection
    private val _streamViewAll = MutableStateFlow(false)
    val streamViewAll: StateFlow<Boolean> = _streamViewAll

    fun selectStream(source: StreamSource?) {
        _streamSelection.value = source; _streamViewAll.value = false
        LogManager.addLog("INFO", "stream", "selected channel=${_currentChannel.value?.id} source=${source?.id} epoch=${source?.epoch}")
    }
    fun watchAllStreams() {
        _streamSelection.value = null; _streamViewAll.value = true
        LogManager.addLog("INFO", "stream", "watch all active streams channel=${_currentChannel.value?.id}")
    }

    private fun watchStreams() {
        if (!NativeLib.isReady()) return
        val group = _currentChannel.value?.id?.takeIf { _foreground.value && MemberFilesProtocol.validAddress(it) }
        if (!NativeLib.streamWatch(group)) LogManager.addLog("WARN", "stream", "source watch rejected group=$group")
    }

    private val _soundOn = MutableStateFlow(true)
    val soundOn: StateFlow<Boolean> = _soundOn

    private val _pubKey = MutableStateFlow("")
    val pubKey: StateFlow<String> = _pubKey

    private val _generatingKeys = MutableStateFlow(false)
    val generatingKeys: StateFlow<Boolean> = _generatingKeys
    private var keyGenerationJob: kotlinx.coroutines.Job? = null

    private val _members = MutableStateFlow<List<ChatMember>>(emptyList())
    val members: StateFlow<List<ChatMember>> = _members
    private val membersRepository = ChatApplication.instance.members
    private var memberOwnerJob: kotlinx.coroutines.Job? = null

    private val _channelOwner = MutableStateFlow(false)
    val channelOwner: StateFlow<Boolean> = _channelOwner

    private val _selectedMember = MutableStateFlow<MemberDetail?>(null)
    val selectedMember: StateFlow<MemberDetail?> = _selectedMember
    private var memberDetailTarget: Pair<String, String>? = null
    private var memberDetailJob: kotlinx.coroutines.Job? = null

    private val _memberLinks = MutableStateFlow<MemberLinks>(MemberLinks())
    val memberLinks: StateFlow<MemberLinks> = _memberLinks

    private val _isStuck = MutableStateFlow(false)
    val isStuck: StateFlow<Boolean> = _isStuck

    private val _cpuLoad = MutableStateFlow(0)
    val cpuLoad: StateFlow<Int> = _cpuLoad
    private val _cpuLoadVisible = MutableStateFlow(false)
    private var cpuLoadPending = false

    fun setCpuLoadVisible(visible: Boolean) { _cpuLoadVisible.value = visible }


    private val _connecting = MutableStateFlow<Map<String, Set<String>>>(emptyMap())
    val connecting: StateFlow<Map<String, Set<String>>> = _connecting

    private val _localSockets = MutableStateFlow<List<LocalSocketState>>(emptyList())
    val localSockets: StateFlow<List<LocalSocketState>> = _localSockets

    private val _inviteCandidates = MutableStateFlow<List<InviteCandidate>>(emptyList())
    val inviteCandidates: StateFlow<List<InviteCandidate>> = _inviteCandidates

    private val _inviteLink = MutableStateFlow<String?>(null)
    val inviteLink: StateFlow<String?> = _inviteLink

    private var inviteRequestId = 0L
    private var inviteChannelId: String? = null
    private val _inviteLinkPending = MutableStateFlow(false)
    val inviteLinkPending: StateFlow<Boolean> = _inviteLinkPending

    private val _dmConversations = MutableStateFlow<List<DmConversation>>(emptyList())
    val dmConversations: StateFlow<List<DmConversation>> = _dmConversations
    private var nextDmDeleteRequest = System.nanoTime().ushr(1)
    private val dmDeleteRequests = mutableMapOf<Long, Pair<String, Long>>()
    private val _dmDeleting = MutableStateFlow<Set<String>>(emptySet())
    val dmDeleting: StateFlow<Set<String>> = _dmDeleting
    private val _dmDeleteFailed = MutableStateFlow(false)
    val dmDeleteFailed: StateFlow<Boolean> = _dmDeleteFailed

    private val _dmMessages = MutableStateFlow<List<Message>>(emptyList())
    val dmMessages: StateFlow<List<Message>> = _dmMessages

    private val _currentDm = MutableStateFlow<DmConversation?>(null)
    val currentDm: StateFlow<DmConversation?> = _currentDm
    private var dmPresenceRefreshJob: kotlinx.coroutines.Job? = null
    private val _dmCallAvailable = MutableStateFlow(false)
    val dmCallAvailable: StateFlow<Boolean> = _dmCallAvailable

    /* Одноразовый сигнал навигации: MainActivity открывает экран DM и сбрасывает. */
    private val _dmOpen = MutableStateFlow<String?>(null)
    val dmOpen: StateFlow<String?> = _dmOpen

    private val pendingDmOpen = PendingDmOpen()
    private data class DmFileOpening(val target: ConversationTarget.Pm, val author: String, val seq: Long)
    private val openingDmFiles = mutableMapOf<String, DmFileOpening>() // GUI-поток; snapshot переживает смену беседы.
    private var dmStartTimeout: kotlinx.coroutines.Job? = null
    private val _dmStartFailed = MutableStateFlow(false)
    val dmStartFailed: StateFlow<Boolean> = _dmStartFailed
    fun dismissDmStartError() { _dmStartFailed.value = false }

    fun cancelDmStart() {
        pendingDmOpen.cancel(); dmStartTimeout?.cancel(); dmStartTimeout = null
    }

    val audioRecorder = AudioRecorderManager()

    private val _isRecording = MutableStateFlow(false)
    val isRecording: StateFlow<Boolean> = _isRecording

    private val _isRecordingLocked = MutableStateFlow(false)
    val isRecordingLocked: StateFlow<Boolean> = _isRecordingLocked

    private val _recordingDurationMs = MutableStateFlow(0)
    val recordingDurationMs: StateFlow<Int> = _recordingDurationMs

    private val _isVideoMode = MutableStateFlow(false)
    val isVideoMode: StateFlow<Boolean> = _isVideoMode

    fun toggleVideoMode() { _isVideoMode.value = !_isVideoMode.value }

    private var recordingStartTimeMs = 0L

    @Volatile private var realPlaybackElapsedMs = 0

    private val _foreground = MutableStateFlow(false)

    fun setForeground(fg: Boolean) {
        _foreground.value = fg
        watchStreams()
        if (fg) {
            _isStuck.value = false
            if (dbReady && repo != null) {
                refreshChannels()
                refreshLocalSockets()
                _currentChannel.value?.let { refreshMessages(it.id); refreshMembers(it.id) }
            }
        }
    }

    init {
        viewModelScope.launch {
            combine(membersRepository.members, _currentChannel, _channelOwner, _connecting) { all, channel, owner, connecting ->
                all.filterKeys { it.group == channel?.id }.values.filter { owner || !it.isDeleted }
                    .sortedWith(compareByDescending<ChatMember> { it.online }.thenBy { it.ed25519 })
                    .map { it.copy(connecting = it.ed25519 in connecting[channel?.id].orEmpty()) }
            }.collect { members ->
                _members.value = members
                _selectedMember.value?.let { detail ->
                    _selectedMember.value = detail.withMember(members.firstOrNull { it.ed25519 == detail.ed25519 })
                }
            }
        }
        viewModelScope.launch {
            radioAudio.activeGroup.collect { group ->
                if (group == null) clearRadioControls()
                else {
                    _radioActive.value = radioChannelId == _currentChannel.value?.id
                    _radioTalkState.value = RadioTalkState(group.orEmpty())
                }
                syncRadioOverlay()
            }
        }
        viewModelScope.launch {
            CallController.state.collect { call ->
                if (call?.preparing == true || CallController.blocksMedia()) { selectStream(null); cancelVoiceRecording() }
            }
        }
        viewModelScope.launch {
            _cpuLoadVisible.collectLatest { visible ->
                if (!visible) return@collectLatest
                LogManager.addLog("DEBUG", "CPU", "GUI polling started: period=333ms")
                try {
                    while (isActive) {
                        if (!NativeLib.isReady()) { cpuLoadPending = false; _cpuLoad.value = 0 }
                        else if (!cpuLoadPending) cpuLoadPending = NativeLib.requestCpuLoad()
                        delay(333)
                    }
                } finally {
                    LogManager.addLog("DEBUG", "CPU", "GUI polling stopped")
                }
            }
        }
        viewModelScope.launch {
            AppEventHandler.events.collect { (type, data) ->
                try { handleEvent(type, data) }
                catch (e: kotlinx.coroutines.CancellationException) { throw e }
                catch (e: Exception) { LogManager.addLog("ERROR", "VM", "Event handling failed type=$type bytes=${data?.size}: $e") }
            }
        }
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val key = ChatApplication.instance.configProvider.getString("node.public_key")
                if (key != null && key.length == 64) _pubKey.value = key
            } catch (e: kotlinx.coroutines.CancellationException) { throw e }
            catch (e: Exception) { LogManager.addLog("ERROR", "KEYS", "Public key preferences unavailable: $e") }
        }
        viewModelScope.launch {
            val provider = ChatApplication.instance.configProvider
            provider.changes.observe("channel sound", provider.notificationSoundSettings).collect { (_, muted) ->
                _currentChannel.value?.let { _soundOn.value = it.id !in muted }
            }
        }
        viewModelScope.launch {
            delay(500)
            if (!dbReady && NativeLib.isReady()) {
                repo = ChatRepository()
                dbReady = true
                LogManager.addLog("INFO", "VM", "DB ready (proactive)")
                refreshChannels()
            }
        }
        viewModelScope.launch {
            _foreground.collectLatest { fg ->
                while (fg && isActive) {
                    delay(3000)
                    if (!dbReady || !NativeLib.isReady()) { _isStuck.value = true; continue }
                    NativeLib.ping()
                    delay(100)
                    _isStuck.value = !NativeLib.isResponsive()
                }
            }
        }
        viewModelScope.launch {
            _foreground.collectLatest { fg ->
                while (fg && isActive) {
                    if (_isRecording.value) {
                        _recordingDurationMs.value = (System.currentTimeMillis() - recordingStartTimeMs).toInt()
                    }
                    delay(80)
                }
            }
        }
        viewModelScope.launch {
            _foreground.collectLatest { fg ->
                while (fg && isActive) {
                    delay(50)
                    val s = _playbackState.value
                    if (s.playing && !s.paused) {
                        val real = realPlaybackElapsedMs
                        val targetMs = if (real >= s.durMs && s.durMs > 0) s.durMs else real
                        val newMs = (s.elapsedMs + (targetMs - s.elapsedMs) * 0.3f).toInt().coerceIn(0, s.durMs)
                        val newProg = if (s.durMs > 0) newMs.toFloat() / s.durMs else 0f

                        if (real >= s.durMs && s.durMs > 0 && newMs >= s.durMs - 30) {
                            _playbackState.value = PlaybackState()
                            realPlaybackElapsedMs = 0
                        } else {
                            _playbackState.value = s.copy(elapsedMs = newMs, progress = newProg)
                        }
                    }
                }
            }
        }
    }

    private fun handleEvent(type: Int, data: ByteArray?) {
        if (type == 13) {
            cancelObjectNavigation()
            if (pendingDmOpen.pending) { _dmStartFailed.value = true; LogManager.addLog("WARN", "PM", "Conversation start interrupted by core stop") }
            cancelDmStart()
            cpuLoadPending = false; _cpuLoad.value = 0
            dmPresenceRefreshJob?.cancel()
            _dmCallAvailable.value = false
            _dmConversations.value = _dmConversations.value.map { it.copy(online = false) }
        } else if (type in listOf(4, 5, 12, 16, 17, 24, 25, 31, 33)) refreshDmCallRoute()
        when (type) {
            57 -> { /* CPU_LOAD: valid:1, average_percent:1. */
                cpuLoadPending = false
                if (data == null || data.size != 2 || data[0].toInt() !in 0..1 || data[1].toInt() !in 0..100) {
                    LogManager.addLog("ERROR", "CPU", "Invalid CPU load response: size=${data?.size}")
                    _cpuLoad.value = 0
                    return
                }
                if (_cpuLoadVisible.value) _cpuLoad.value = if (data[0].toInt() == 1) data[1].toInt() else 0
            }
            56 -> { /* MSG_SUBMITTED: request_id:8LE, result: 0=failed,1=commit,2=too large. */
                if (data == null || data.size != 9 || data[8].toInt() !in 0..2) {
                    LogManager.addLog("ERROR", "Reply", "invalid submission result"); return
                }
                val id = java.nio.ByteBuffer.wrap(data).order(java.nio.ByteOrder.LITTLE_ENDIAN).long
                if (data[8].toInt() == 2 && submissions.containsKey(id)) _messageSendTooLarge.value = true
                submissionResult(id, data[8].toInt() == 1)
            }
            55 -> { /* RADIO_MUTE_RESULT: group Ed25519:32, member Ed25519:32, success. */
                if (data == null || data.size != 65) {
                    LogManager.addLog("ERROR", "Radio", "Invalid member mute result"); _radioMuteFailed.value = true; return
                }
                if (data[64].toInt() == 0) _radioMuteFailed.value = true
            }
            53 -> { /* FEEDBACK_CHANGED: channel, message_id, kind. */
                try {
                    val event = parseFeedbackChangedEvent(data)
                    if (_currentChannel.value?.id != event.channelId) return
                    sendInteraction(JSONObject().put("cmd", "feedback").put("ch", event.channelId).put("message_id", event.messageId), false)
                } catch (e: Exception) { LogManager.addLog("ERROR", "POLL", "invalid change event bytes=${data?.size}: ${e.message}") }
            }
            54 -> { /* FEEDBACK_RESULT: request_id:8LE, success, UTF-8 JSON. */
                try {
                    val event = parseFeedbackResultEvent(data)
                    val result = event.json
                    if (!event.success) {
                        LogManager.addLog("ERROR", "POLL", "command failed request=${event.requestId}: ${result.optString("error")}")
                        if (event.requestId != 0L) _interactionFailed.value = true
                    } else if (result.has("catalog")) {
                        val values = result.getJSONArray("catalog")
                        _reactionCatalog.value = (0 until values.length()).map { index -> values.getJSONObject(index).let {
                            ReactionState(it.getInt("id"), it.getString("emoji"))
                        } }
                    } else if (result.optString("ch") == _currentChannel.value?.id) {
                        if (result.optBoolean("snapshot_pending")) {
                            val message = result.getString("message_id")
                            LogManager.addLog("WARN", "POLL", "command saved, requesting snapshot message=$message")
                            sendInteraction(JSONObject().put("cmd", "feedback").put("ch", result.getString("ch"))
                                .put("message_id", message), false)
                        } else {
                            val state = parseFeedback(result)
                            feedbackCache[state.messageId] = state
                            _messages.update { rows -> rows.map { if (it.messageId == state.messageId) it.copy(feedback = state) else it } }
                        }
                    }
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "POLL", "invalid result bytes=${data?.size}: ${e.message}")
                    _interactionFailed.value = true
                }
            }
            9 -> { /* DB_READY */
                if (dbReady) return
                repo = ChatRepository()
                dbReady = true
                LogManager.addLog("INFO", "VM", "DB ready")
                refreshChannels()
                refreshLocalSockets()
            }
            11 -> { /* KEYS_GENERATED: [pub_hex:64] */
                if (data != null && data.size >= 64) {
                    _pubKey.value = String(data, 0, 64)
                    _generatingKeys.value = false
                }
            }
            67 -> { /* MSG_UPDATED: существующая строка без уведомлений и unread. */
                val size = data?.firstOrNull()?.toInt()?.and(0xff) ?: 0
                if (size == 0 || data == null || data.size != 1 + size + 32) {
                    LogManager.addLog("ERROR", "Edit", "invalid update event bytes=${data?.size}"); return
                }
                val channel = String(data, 1, size, Charsets.UTF_8)
                if (_currentChannel.value?.id == channel) refreshMessages(channel)
            }
            1 -> {
                if (repo == null) return
                val event = ChatMessageEvent.decode(data) ?: return
                if (_currentChannel.value?.id == event.channel) refreshMessages(event.channel)
                refreshChannels()
            }
            5 -> { /* MEMBERS_CHANGED: [ch_id_len:1][ch_id:var] */
                if (repo == null || data == null || data.size < 2) return
                val chLen = data[0].toInt() and 0xFF
                if (data.size >= 1 + chLen) {
                    val chId = String(data, 1, chLen)
                    val cur = _currentChannel.value
                    if (cur != null && cur.id == chId) refreshMembers(chId)
                    refreshSelectedMemberDetail(chId)
                }
            }
            8 -> if (repo != null) refreshChannels()  /* CHANNEL_PEERS_ONLINE */
            14 -> { /* ATTACHMENT_DOWNLOADED: [ch_id_len:1][ch_id:var][msg_id:8] */
                if (repo == null || data == null || data.size < 2) return
                val chLen = data[0].toInt() and 0xFF
                if (data.size >= 1 + chLen + 8) {
                    val chId = String(data, 1, chLen)
                    val cur = _currentChannel.value
                    if (cur != null && cur.id == chId) refreshMessages(chId)
                    refreshChannels()
                }
            }
            60 -> { /* ATTACHMENT_KEYS_CHANGED: [channel_len:1][channel][success?:1] */
                val size = data?.firstOrNull()?.toInt()?.and(0xFF) ?: 0
                if (size == 0 || data == null || data.size < 1 + size) {
                    LogManager.addLog("ERROR", "Attachment", "invalid encryption state event bytes=${data?.size}")
                    return
                }
                val channel = String(data, 1, size, Charsets.UTF_8)
                LogManager.addLog("DEBUG", "Attachment", "encryption state changed ch=$channel")
                if (_currentChannel.value?.id == channel) refreshMessages(channel)
            }
            2 -> { /* CONNECT_RESULT: [peer X25519:32][result:4][group Ed25519:32][request:8] */
                if (repo == null || data == null || data.size != 76) { LogManager.addLog("WARN", "VM", "CONNECT_RESULT skipped repo=${repo != null} data=${data != null}"); return }
                val buf = java.nio.ByteBuffer.wrap(data).order(java.nio.ByteOrder.LITTLE_ENDIAN)
                val peer = MemberFilesProtocol.addressHex(ByteArray(32).also { buf.get(it) })
                val result = buf.getInt()
                val groupEd = MemberFilesProtocol.addressHex(ByteArray(32).also { buf.get(it) })
                val request = buf.getLong()
                val joining = _linkJoin.value
                if (joining != null && joining.pending && joining.request == request && joining.peer == peer) {
                    _linkJoin.value = joining.copy(pending = false, result = result)
                    if (result == 0) viewModelScope.launch {
                        val r = repo ?: return@launch
                        val channels = withContext(Dispatchers.IO) { r.getChannels() } ?: return@launch
                        if (repo !== r) return@launch
                        _channels.value = channels
                        val channel = channels.find { it.groupEd == groupEd }
                        if (_linkJoin.value?.link == joining.link && channel != null) {
                            _linkJoin.value = null; _joinedChannel.value = channel
                        } else if (channel == null) LogManager.addLog("ERROR", "JOIN", "joined group missing key=$groupEd")
                    }
                }
                LogManager.addLog(
                    if (result == 0) "INFO" else "ERROR",
                    "JOIN", "connect to $peer group=$groupEd result=$result")
                LogManager.addLog("INFO", "VM", "CONNECT_RESULT result=$result — refreshChannels")
                if (result == 0) refreshChannels()
            }
            4 -> { if (repo != null) { LogManager.addLog("INFO", "VM", "CHANNEL_UPDATED — refreshChannels"); refreshChannels() } else { LogManager.addLog("WARN", "VM", "CHANNEL_UPDATED skipped — repo is NULL") } }
            12 -> { LogManager.addLog("INFO", "VM", "service started"); watchStreams() }
            13 -> {
                repo = null; dbReady = false
                ++channelRefreshRevision; ++messageRefreshRevision
                _streamSources.value = emptyList(); _streamSourcesFailed.value = false; selectStream(null)
                for (id in submissions.keys.toList()) submissionResult(id, false)
                _radioTalkState.value = RadioTalkState()
                _radioTalking.value = null
                _radioActive.value = false
                radioAudio.stop()
                memberOwnerJob?.cancel(); memberDetailJob?.cancel()
                _selectedMember.value = null; _memberLinks.value = MemberLinks()
                _attachmentPreparations.value = emptyMap(); discardedPreparationJobs.clear()
                _pmInvite.value?.let { state ->
                    if (state.pending) _pmInvite.value = state.copy(pending = false, errorRes = R.string.invite_core_unavailable)
                }
                _linkJoin.value?.let { state ->
                    if (state.pending) _linkJoin.value = state.copy(pending = false, result = -7)
                }
                LogManager.addLog("INFO", "VM", "service stopped; pending invitation UI released")
                if (dmDeleteRequests.isNotEmpty()) {
                    dmDeleteRequests.clear(); _dmDeleting.value = emptySet(); _dmDeleteFailed.value = true
                    LogManager.addLog("WARN", "PM", "Service stopped before pending delete results")
                }
            }
            48 -> { /* [request_id:8][result:4][peer:8][conv:8][generation:8] */
                if (data == null || data.size != 60) { LogManager.addLog("ERROR", "INVITE", "invalid PM invite result"); return }
                val buf = java.nio.ByteBuffer.wrap(data).order(java.nio.ByteOrder.LITTLE_ENDIAN)
                val request = buf.long; val result = buf.int
                val peer = MemberFilesProtocol.addressHex(ByteArray(32).also { buf.get(it) })
                val conv = buf.long.toULong().toString()
                val generation = buf.long
                val state = _pmInvite.value ?: return
                if (!state.pending || state.requestId != request || state.peer != peer) return
                LogManager.addLog(if (result == 0) "INFO" else "ERROR", "INVITE", "PM invite result request=$request peer=$peer result=$result")
                if (result == 0) {
                    _pmInvite.value = null
                    openDm(DmConversation(conv, peerEd25519 = peer, peerName = state.name, generation = generation)); _dmOpen.value = conv; refreshDmConversations()
                } else {
                    val error = when (result) {
                        -1 -> R.string.invite_pm_start_failed
                        -3 -> R.string.invite_pm_send_failed
                        -5 -> R.string.invite_pm_busy
                        else -> R.string.invite_build_failed
                    }
                    _pmInvite.value = state.copy(pending = false, errorRes = error)
                }
            }
            47 -> {
                if (data == null || data.size < 66 || data.size != 65 + (data[0].toInt() and 0xff)) {
                    LogManager.addLog("WARN", "Call", "invalid PM route snapshot size=${data?.size}"); return
                }
                val len = data[0].toInt() and 0xff
                val conv = String(data, 1, len, Charsets.UTF_8)
                val buf = java.nio.ByteBuffer.wrap(data, 1 + len, 64).order(java.nio.ByteOrder.LITTLE_ENDIAN)
                val peer = MemberFilesProtocol.addressHex(ByteArray(32).also { buf.get(it) })
                val group = MemberFilesProtocol.addressHex(ByteArray(32).also { buf.get(it) })
                val available = peer.isNotEmpty() && group.isNotEmpty()
                _dmConversations.value = _dmConversations.value.map {
                    if (it.convId != conv) it else {
                        val online = available
                        if (it.online != online) LogManager.addLog("INFO", "Call", "PM presence conv=$conv peer=$peer online=$online")
                        it.copy(online = online)
                    }
                }
                val dm = _currentDm.value ?: return
                if (conv != dm.convId) return
                val canCall = available
                if (_dmCallAvailable.value != canCall)
                    LogManager.addLog("INFO", "Call", "PM button conv=$conv peer=$peer group=$group enabled=$canCall")
                _dmCallAvailable.value = canCall
            }
            16 -> Unit /* Только сигнал для PM route; live-состояние участников приходит с ID канала. */
            17 -> { /* LINK_STATUS_CHANGED: [peer Ed25519:32][link_id:1][state:1][status:1][links_up:1][initialized:1] */
                if (data == null || data.size != 37) return
                val buf = java.nio.ByteBuffer.wrap(data).order(java.nio.ByteOrder.LITTLE_ENDIAN)
                val peer = ByteArray(32).also { buf.get(it) }.toHexString()
                val linkId = buf.get().toInt() and 0xFF
                val state = buf.get().toInt() and 0xFF
                val status = buf.get().toInt() and 0xFF
                val linksUp = buf.get().toInt() and 0xFF
                val initialized = buf.get().toInt() and 0xFF
                LogManager.addLog("DEBUG", "VM", "LINK_STATUS_CHANGED peer=%s link=%d st=%d ls=%d up=%d init=%d"
                    .format(peer, linkId, state, status, linksUp, initialized.toInt()))
                val detail = _selectedMember.value
                if (detail != null && detail.ed25519 == peer) {
                    val curLinks = _memberLinks.value
                    val updated = curLinks.links.map { lk ->
                        if (lk.linkId == linkId) lk.copy(linkState = state, linkStatus = status) else lk
                    }
                    val found = curLinks.links.any { it.linkId == linkId }
                    _memberLinks.value = curLinks.copy(linksUp = linksUp, links = updated)
                    if (!found || status != 1) refreshMemberLinks(peer)
                }
            }
            58 -> { /* STREAM_SOURCES: group:Ed25519, valid:1, JSON. Старый канал не меняет новую шапку. */
                if (data == null || data.size < 35 || data[32].toInt() !in 0..1) {
                    LogManager.addLog("ERROR", "stream", "invalid source snapshot bytes=${data?.size}"); return
                }
                val group = data.copyOfRange(0, 32).toHexString()
                if (_currentChannel.value?.id != group || !_foreground.value) return
                try {
                    _streamSourcesFailed.value = data[32].toInt() == 0
                    if (!_streamSourcesFailed.value) _streamSources.value = StreamSource.parse(String(data, 33, data.size - 33, Charsets.UTF_8))
                    LogManager.addLog("INFO", "stream", "sources group=$group count=${_streamSources.value.size} valid=${!_streamSourcesFailed.value}")
                } catch (e: Exception) {
                    _streamSourcesFailed.value = true
                    LogManager.addLog("ERROR", "stream", "source snapshot parse group=$group: ${e.message}")
                }
            }
            19 -> { /* LOCAL_SOCKETS: снимок создан в потоке ядра; пустое событие просит обновить его. */
                if (data == null || data.isEmpty()) refreshLocalSockets()
                else repo?.parseLocalSockets(String(data, Charsets.UTF_8))?.let { _localSockets.value = it }
            }
            23 -> Unit /* Применено общим MemberRepository до доставки UI-события. */
            24 -> {
                if (data == null || data.isEmpty()) return
                val length = data[0].toInt() and 255
                if (length == 0 || data.size != length + 2 + MEMBER_WIRE_SIZE) return
                val channel = String(data, 1, length)
                val wire = parseMemberWire(data, length + 2) ?: return
                refreshSelectedMemberDetail(channel, wire.ed25519)
            }
            25 -> {
                if (data == null || data.isEmpty()) return
                val length = data[0].toInt() and 255
                if (length == 0 || data.size != length + 33) return
                val channel = String(data, 1, length)
                val peer = MemberFilesProtocol.addressHex(data.copyOfRange(length + 1, data.size))
                if (_radioTalkState.value.groupId == channel) {
                    _radioTalkState.value = _radioTalkState.value.copy(speakers = _radioTalkState.value.speakers - peer)
                    _radioTalking.value = if (_radioActive.value) _radioTalkState.value.speakers.firstOrNull() else null
                }
                val target = memberDetailTarget
                if (target != null && membersRepository.member(target.first, target.second) == null) {
                    memberDetailJob?.cancel(); memberDetailTarget = null; _selectedMember.value = null
                } else refreshSelectedMemberDetail(channel, peer)
            }
            27 -> { /* CONNECTING_NODES: [ch_id_len:1][ch_id:var][count:2][Ed25519:32]* */
                val snapshot = ChannelEventFrames.nodes(data)
                _connecting.value = _connecting.value + (snapshot.channel to snapshot.addresses.toSet())
            }
            28 -> { /* ADMIN_KEY_RECEIVED: [ch_id_len:1][ch_id:var][from_ed:32] */
                if (data == null || data.isEmpty()) return
                val cl = data[0].toInt() and 0xFF
                if (data.size != 1 + cl + 32) return
                val chId = String(data, 1, cl)
                val from = data.copyOfRange(1 + cl, data.size).joinToString("") { "%02x".format(it.toInt() and 255) }
                LogManager.addLog("INFO", "VM", "admin key received ch=$chId from=$from — now co-owner")
                if (_currentChannel.value?.id == chId) refreshMembers(chId)
            }
            30 -> { /* INVITE_CANDIDATES: [ch_id_len:1][ch_id][auto_ed25519:32][count:2][ed25519:32]* */
                val snapshot = ChannelEventFrames.candidates(data)
                val chId = snapshot.channel
                if (inviteChannelId != chId) return
                val request = inviteRequestId
                val autoId = snapshot.automatic
                val ids = snapshot.addresses
                LogManager.addLog("INFO", "VM", "INVITE_CANDIDATES ch=$chId auto=$autoId count=${ids.size}")
                if (autoId.isEmpty()) { _inviteLinkPending.value = false; return }
                viewModelScope.launch {
                    try {
                        val myId = NativeLib.getMySigningAddress()
                        val nameOf = withContext(Dispatchers.IO) {
                            (repo?.getMembers(chId) ?: emptyList()).associate { it.ed25519 to it.name }
                        }
                        fun label(id: String): String = nameOf[id]?.takeIf { it.isNotEmpty() } ?: id
                        val list = mutableListOf<InviteCandidate>()
                        list.add(InviteCandidate(autoId, label(autoId), isAuto = true, isSelf = autoId == myId))
                        for (id in ids) {
                            if (id == autoId) continue
                            list.add(InviteCandidate(id, label(id), isSelf = id == myId))
                        }
                        if (inviteChannelId != chId || inviteRequestId != request) return@launch
                        _inviteCandidates.value = list
                        requestInviteLink(chId, autoId)
                    } catch (e: kotlinx.coroutines.CancellationException) { throw e
                    } catch (e: Exception) {
                        LogManager.addLog("ERROR", "Invite", "Candidate names unavailable channel=$chId: $e")
                    }
                }
            }
            29 -> { /* [ch_id_len:1][ch_id][request_id:8][link_len:2][link] */
                val result = ChannelEventFrames.invite(data)
                if (result.channel != inviteChannelId || result.request != inviteRequestId) return
                LogManager.addLog(if (result.link != null) "INFO" else "ERROR", "VM", "INVITE_LINK_READY ch=${result.channel} bytes=${data?.size}")
                _inviteLink.value = result.link
                _inviteLinkPending.value = false
            }
            31 -> { /* CHANNEL_DELETED: [ch_id_len:1][ch_id:var] */
                if (repo == null || data == null || data.size < 2) return
                val cl = data[0].toInt() and 0xFF
                if (data.size < 1 + cl) return
                val chId = String(data, 1, cl)
                LogManager.addLog("INFO", "VM", "CHANNEL_DELETED ch=$chId")
                if (radioChannelId == chId) turnRadioOff()
                if (_currentChannel.value?.id == chId) {
                    _currentChannel.value = null
                    _messages.value = emptyList()
                    _members.value = emptyList()
                    _channelOwner.value = false
                }
                refreshChannels()
            }
            33 -> { /* DM_CONV_UPDATED: [conv_id_len:1][conv_id:var][generation:8] */
                if (data == null || data.size < 2) return
                val cl = data[0].toInt() and 0xFF
                if (data.size != cl + 9) { LogManager.addLog("ERROR", "PM", "Invalid conversation update bytes=${data.size}"); return }
                val convId = String(data, 1, cl)
                val generation = java.nio.ByteBuffer.wrap(data, 1 + cl, 8).order(java.nio.ByteOrder.LITTLE_ENDIAN).long
                LogManager.addLog("INFO", "VM", "DM_CONV_UPDATED conv=$convId")
                refreshDmConversations()
                _currentDm.value?.let {
                    if (it.convId == convId && generation >= it.generation) {
                        if (generation != it.generation) _dmMessages.value = emptyList()
                        _currentDm.value = it.copy(generation = generation)
                        refreshDmMessages()
                    }
                }
                pendingDmOpen.complete(convId, generation)?.let { started ->
                    dmStartTimeout?.cancel(); dmStartTimeout = null
                    openDm(started)
                    if (_currentDm.value?.convId == convId) _dmOpen.value = convId
                    LogManager.addLog("INFO", "VM", "DM start confirmed conv=$convId peer=${started.peerEd25519}")
                }
            }
            32 -> { /* DM_MSG_RECEIVED: [conv_id_len:1][conv_id:var][author Ed25519:32] */
                if (data == null || data.size < 34) return
                val cl = data[0].toInt() and 0xFF
                if (cl == 0 || data.size != 1 + cl + 32) return
                val convId = String(data, 1, cl)
                val author = MemberFilesProtocol.addressHex(data.copyOfRange(1 + cl, data.size))
                LogManager.addLog("INFO", "VM", "DM_MSG_RECEIVED conv=$convId author=$author")
                val cur = _currentDm.value
                if (cur != null && cur.convId == convId) refreshDmMessages()
                refreshDmConversations()
            }
            41 -> { /* DM_MESSAGES: [conv_len:1][conv:utf8][generation:8][JSON:utf8] */
                if (data == null || data.isEmpty()) return
                val length = data[0].toInt() and 0xFF
                if (length == 0 || data.size <= 9 + length) {
                    LogManager.addLog("ERROR", "PM", "Invalid history snapshot bytes=${data.size}"); return
                }
                val dm = _currentDm.value ?: return
                val conv = String(data, 1, length, Charsets.UTF_8)
                val generation = java.nio.ByteBuffer.wrap(data, 1 + length, 8).order(java.nio.ByteOrder.LITTLE_ENDIAN).long
                if (conv != dm.convId || generation != dm.generation || conv in _dmDeleting.value) {
                    LogManager.addLog("DEBUG", "PM", "Discarded stale history conv=$conv generation=$generation"); return
                }
                val json = String(data, 9 + length, data.size - 9 - length, Charsets.UTF_8)
                repo?.parseDmMessages(json, dm.peerName)?.let { _dmMessages.value = it }
            }
            45 -> {
                if (data == null) return
                val previous = _dmConversations.value.associateBy { it.convId }
                val snapshot = repo?.parseDmConversations(String(data, Charsets.UTF_8)) ?: return
                _dmConversations.value = snapshot.map {
                    it.copy(online = previous[it.convId]?.online == true)
                }
                _currentDm.value?.let { current ->
                    _dmConversations.value.find { it.convId == current.convId }?.let {
                        _currentDm.value = it
                        if (it.generation != current.generation) { _dmMessages.value = emptyList(); refreshDmMessages() }
                    }
                }
                refreshDmCallRoute()
            }
            59 -> { /* DM_DELETE_RESULT: request, conv, generation, success. */
                if (data == null || data.size != 25) { LogManager.addLog("ERROR", "PM", "Invalid delete result"); return }
                val b = java.nio.ByteBuffer.wrap(data).order(java.nio.ByteOrder.LITTLE_ENDIAN)
                val request = b.long; val conv = b.long.toULong().toString(); val generation = b.long; val success = b.get().toInt() == 1
                val expected = dmDeleteRequests[request] ?: return
                if (expected != (conv to generation)) { LogManager.addLog("ERROR", "PM", "Delete result target mismatch"); return }
                dmDeleteRequests.remove(request); _dmDeleting.value = dmDeleteRequests.values.map { it.first }.toSet()
                LogManager.addLog(if (success) "INFO" else "ERROR", "PM", "Delete result conv=$conv generation=$generation success=$success")
                if (!success) { _dmDeleteFailed.value = true; return }
                val deleted = _dmConversations.value.find { it.convId == conv && it.generation == generation }
                _dmConversations.value = _dmConversations.value.filterNot { it.convId == conv && it.generation == generation }
                _currentDm.value?.let { if (it.convId == conv && it.generation == generation) closeDm() }
                val target = ConversationTarget.Pm(conv, generation)
                _attachmentPreparations.value[target.nativeId]?.let { discardedPreparationJobs.addAll(it.jobs) }
                _attachmentPreparations.value = _attachmentPreparations.value - target.nativeId
                cancelVoiceRecording(target)
                if (deleted != null && _pmInvite.value?.peer == deleted.peerEd25519) dismissPmInvite()
                refreshDmConversations()
            }
            46 -> {
                if (data == null || data.size < 18) return
                val length = data[0].toInt() and 255
                val state = data.last().toInt() and 255
                if (data.size != length + 18 || state > 2) { LogManager.addLog("ERROR", "VM", "invalid preparation event"); return }
                val target = String(data, 1, length, Charsets.UTF_8)
                val job = android.util.Base64.encodeToString(data.copyOfRange(1 + length, 17 + length), android.util.Base64.NO_WRAP)
                preparationState(target, job, state)
            }
            43 -> { /* RADIO_TALK: [group Ed25519:32][source Ed25519:32][stream:2][on:1] */
                val event = parseRadioTalkEvent(data)
                if (event == null) { LogManager.addLog("ERROR", "radio", "invalid RADIO_TALK bytes=${data?.size ?: 0}"); return }
                val previous = _radioTalkState.value
                val next = previous.onTalk(event)
                if (event.groupId != previous.groupId) {
                    LogManager.addLog("DEBUG", "radio", "ignored talk group=${event.groupId} active=${previous.groupId} peer=${event.source}")
                    return
                }
                if (next == previous) return
                _radioTalkState.value = next
                _radioTalking.value = if (_radioActive.value) next.speakers.firstOrNull() else null
                LogManager.addLog("DEBUG", "radio", "talk group=${event.groupId} peer=${event.source} stream=${event.stream} " +
                    "on=${event.on} speakers=${next.speakers.size}")
            }
        }
    }

    /* Звонком владеет CallController; ViewModel передаёт только пользовательские команды. */
    fun startCall(channelId: String, peerAddress: String?, peerName: String, devices: Boolean = false) =
        CallController.start(channelId, peerAddress, peerName, devices)
    fun acceptCall() = CallController.accept()
    fun declineCall() = CallController.end(reason = 2)
    fun hangupCall() = CallController.end()
    fun dismissCall() = CallController.dismiss()

    /* ── рация ── */

    fun toggleRadio() {
        val ch = _currentChannel.value ?: return
        setRadioEnabled(ch, radioChannelId != ch.id)
    }

    /** Вызов выбирает конкретный канал; повторное принятие не выключает уже работающую рацию. */
    fun enableRadioForCall(channelId: String, id: Long, onReady: (Boolean) -> Unit) {
        val channel = _channels.value.firstOrNull { it.id == channelId }
        if (channel == null) {
            LogManager.addLog("ERROR", "radio", "Call activation channel missing id=$channelId")
            onReady(false); return
        }
        if (!RadioCallController.ownActivation(id) {
            if (radioAudio.cancelInvitation(id)) { _radioActive.value = false; syncRadioOverlay() }
        }) {
            LogManager.addLog("DEBUG", "radio", "Cancelled call activation ignored id=$id")
            return
        }
        selectChannel(channel)
        val group = channelId
        val previous = radioChannelId
        if (previous != null && previous != channelId) NativeLib.radioSetActive(previous, false)
        NativeLib.radioSetActive(group, true)
        radioAudio.startForInvitation(group, id) { ready ->
            if (!ready) radioAudio.cancelInvitation(id)
            _radioActive.value = radioChannelId == _currentChannel.value?.id
            onReady(ready)
        }
        _radioActive.value = radioChannelId == channelId
        syncRadioOverlay()
    }

    private fun setRadioEnabled(ch: Channel, next: Boolean, onReady: ((Boolean) -> Unit)? = null) {
        val groupId = ch.id.takeIf(MemberFilesProtocol::validAddress) ?: return
        radioToggleCount++
        val caller = Throwable().stackTrace.drop(1).take(8)
            .joinToString(" <- ") { "${it.className.substringAfterLast('.')}.${it.methodName}:${it.lineNumber}" }
        LogManager.addLog("DEBUG", "RADIO_DBG",
            "toggleRadio#$radioToggleCount next=$next prevActive=${_radioActive.value} " +
            "radioChannelId=$radioChannelId cur=${ch.id} " +
            "thread=${Thread.currentThread().name} t=${System.currentTimeMillis()} caller=$caller")
        if (next && CallController.blocksMedia()) {
            LogManager.addLog("WARN", "radio", "enable rejected: call owns audio group=$groupId")
            android.widget.Toast.makeText(ChatApplication.instance, R.string.call_media_busy, android.widget.Toast.LENGTH_SHORT).show()
            onReady?.invoke(false)
            return
        }
        if (next) {
            radioChannelId?.takeIf { it != ch.id }
                ?.let { NativeLib.radioSetActive(it, false) }   /* выключаем старый канал */
            _radioTalkState.value = RadioTalkState(groupId)
            _radioTalking.value = null
            NativeLib.radioSetActive(groupId, true)
            radioAudio.start(groupId) { ready ->
                if (!ready && radioChannelId == ch.id && !radioAudio.isActive()) turnRadioOff()
                onReady?.invoke(ready)
            }
        } else {
            NativeLib.radioSetActive(groupId, false)
            radioAudio.stop()
            _radioTalking.value = null
            _radioTalkState.value = RadioTalkState()
        }
        _radioActive.value = next
        if (next) RadioOverlayService.requestPermissions() else syncRadioOverlay()
        LogManager.addLog("INFO", "VM", "toggleRadio -> ${if (next) "on" else "off"} group=$groupId")
    }

    fun radioTalkBegin(): Boolean = _radioActive.value && radioAudio.talkBegin()

    fun radioTalkEnd() {
        if (!_radioActive.value) return
        radioAudio.talkEnd()
    }

    fun setRadioVolumePtt(enabled: Boolean) {
        if (!_radioActive.value) return
        val provider = ChatApplication.instance.configProvider
        provider.changes.submit("panel volume PTT") { provider.setRadioVolumePtt(enabled) }
    }

    /** Переключатель авто-PTT на панели рации: сохранить выбор и перенастроить захват. */
    fun setRadioAutomatic(enabled: Boolean) {
        if (!_radioActive.value) return
        val ctx = ChatApplication.instance
        ctx.configProvider.changes.submit("panel automatic PTT") {
            ctx.configProvider.setValue("radio.auto_enabled", if (enabled) "1" else "0")
            LogManager.addLog("INFO", "RadioAudio", "Panel automatic transmission enabled=$enabled channel=$radioChannelId")
            if (enabled) RadioOverlayService.requestPermissions() else RadioOverlayService.sync(ctx)
            radioAudio.reconfigure(restartAuto = enabled)
        }
    }

    /* Полностью выключить рацию (используется при удалении активного канала). */
    private fun turnRadioOff() {
        val chId = radioChannelId ?: return
        NativeLib.radioSetActive(chId, false)
        radioAudio.stop()
        clearRadioControls()
        syncRadioOverlay()
    }

    private fun clearRadioControls() {
        _radioActive.value = false
        _radioTalking.value = null
        _radioTalkState.value = RadioTalkState()
    }

    private fun syncRadioOverlay() {
        val ctx = ChatApplication.instance
        val overlayEnabled = ctx.configProvider.getRadioOverlay()
        val radioOn = radioChannelId != null
        LogManager.addLog("DEBUG", "RADIO_DBG",
            "syncRadioOverlay radioOn=$radioOn channel=$radioChannelId overlayEnabled=$overlayEnabled " +
            "isShown=${RadioOverlayService.isShown.value} t=${System.currentTimeMillis()}")
        RadioOverlayService.sync(ctx)
    }

    fun pollAuthorName(key: String): String =
        _members.value.firstOrNull { it.ed25519.equals(key, ignoreCase = true) }?.name?.takeIf { it.isNotBlank() } ?: key

    fun radioName(peer: String): String =
        members.value.firstOrNull { it.ed25519 == peer }?.name?.takeIf { it.isNotEmpty() } ?: peer

    fun toggleMute() = CallController.toggleMute()
    fun setAudioRoute(r: AudioRoute) = CallController.setRoute(r)

    fun requestRegenerateKeys() {
        if (_generatingKeys.value || _databaseReset.value.running) return
        if (!NativeLib.isReady()) {
            AppEventHandler.recoverStartup(fromSettings = true)
            return
        }
        keyGenerationJob?.cancel()
        _generatingKeys.value = true
        keyGenerationJob = viewModelScope.launch {
            try {
                HeadlessService.awaitStartup()
                val config = ChatApplication.instance.configProvider.buildConfigText().await()
                check(NativeLib.regenerateKeys(config)) { "key regeneration request rejected" }
                delay(5000)
                if (_generatingKeys.value) {
                    _generatingKeys.value = false
                    LogManager.addLog("ERROR", "KEYS", "key regeneration timeout")
                }
            } catch (e: kotlinx.coroutines.CancellationException) { throw e
            } catch (e: Exception) {
                _generatingKeys.value = false
                LogManager.addLog("ERROR", "KEYS", "key regeneration failed: ${e.javaClass.simpleName}: ${e.message}")
            }
        }
    }

    fun requestRefresh() {
        if (dbReady && repo != null) refreshChannels()
    }

    fun requestInvite(channelId: String) {
        clearInvite()
        inviteChannelId = channelId
        inviteRequestId = inviteRequestIds.getAndIncrement()
        _inviteLinkPending.value = true
        if (!NativeLib.requestInviteCandidates(channelId)) {
            _inviteLinkPending.value = false
            LogManager.addLog("ERROR", "INVITE", "Candidate request rejected ch=$channelId")
        }
        LogManager.addLog("INFO", "VM", "requestInvite ch=$channelId")
    }

    fun requestInviteLink(channelId: String, target: String) {
        if (inviteChannelId != channelId) return
        val request = inviteRequestIds.getAndIncrement()
        if (!NativeLib.requestInviteLink(channelId, target, request)) {
            _inviteLinkPending.value = false
            LogManager.addLog("ERROR", "INVITE", "Link request rejected ch=$channelId target=$target")
            return
        }
        inviteRequestId = request
        _inviteLink.value = null
        _inviteLinkPending.value = true
        LogManager.addLog("INFO", "VM", "requestInviteLink ch=$channelId target=$target")
    }

    fun clearInvite(channelId: String? = null) {
        if (channelId != null && inviteChannelId != channelId) return
        val request = inviteRequestId
        inviteChannelId = null
        inviteRequestId = 0
        if (request != 0L && NativeLib.isReady() && !NativeLib.cancelInviteLink(request)) {
            LogManager.addLog("ERROR", "INVITE", "Link revocation rejected request=$request")
        }
        _inviteCandidates.value = emptyList()
        _inviteLink.value = null
        _inviteLinkPending.value = false
    }

    fun refreshChannels() {
        val r = repo ?: return
        val revision = ++channelRefreshRevision
        viewModelScope.launch {
            val channels = withContext(Dispatchers.IO) { r.getChannels() } ?: return@launch
            if (repo !== r || revision != channelRefreshRevision) return@launch
            _channels.value = channels
            _currentChannel.value?.let { current ->
                channels.firstOrNull { it.id == current.id }?.let { updated -> _currentChannel.value = updated }
            }
            refreshDmConversations()
        }
    }

    fun refreshMessages(chId: String) {
        if (_currentChannel.value?.groupType != "chat") { _messages.value = emptyList(); return; }
        val r = repo ?: return
        if (_currentChannel.value?.id != chId) return
        val anchor = _historyAnchor.value
        val revision = ++messageRefreshRevision
        viewModelScope.launch {
            val (rows, originals) = withContext(Dispatchers.IO) {
                val rows = r.getMessages(chId, 100, anchor) ?: return@withContext null
                val loaded = rows.associateBy { it.messageId }.toMutableMap()
                for (id in rows.map { it.replyTo }.filter { it.isNotEmpty() }.distinct()) {
                    if (id !in loaded) r.getMessages(chId, 1, id, true)?.firstOrNull()?.let { loaded[id] = it }
                }
                rows to loaded
            } ?: return@launch
            if (repo !== r || _currentChannel.value?.id != chId || revision != messageRefreshRevision) return@launch
            _messages.value = rows.map { it.copy(feedback = feedbackCache[it.messageId]) }
            _replyOriginals.value = originals
            if (anchor.isEmpty()) pendingLatestJump?.let { _messageJump.value = it; pendingLatestJump = null }
            if (_reactionCatalog.value.isEmpty()) sendInteraction(JSONObject().put("cmd", "reaction_catalog"), false)
            for (row in rows) if (row.messageId.length == 64)
                sendInteraction(JSONObject().put("cmd", "feedback").put("ch", chId).put("message_id", row.messageId), false)
        }
    }

    fun showOriginal(id: String, objectRevision: Long = 0) {
        val channel = _currentChannel.value?.id ?: return
        val r = repo ?: return
        val revision = ++jumpRevision
        pendingLatestJump = null
        viewModelScope.launch {
            val rows = withContext(Dispatchers.IO) { r.getMessages(channel, 100, id) }
            if (rows == null) {
                if (objectRevision != 0L) completeObjectLink(objectRevision, false)
                return@launch
            }
            if (repo !== r || _currentChannel.value?.id != channel || jumpRevision != revision ||
                (objectRevision != 0L && _objectNavigation.value?.revision != objectRevision)) return@launch
            if (rows.none { it.messageId == id }) {
                LogManager.addLog("WARN", "Reply", "original unavailable ch=$channel id=$id")
                if (objectRevision != 0L) completeObjectLink(objectRevision, false) else _originalMissing.value = true
                return@launch
            }
            ++messageRefreshRevision
            _historyAnchor.value = id
            _messages.value = rows.map { it.copy(feedback = feedbackCache[it.messageId]) }
            _messageJump.value = MessageJump(id, revision)
            if (objectRevision != 0L) completeObjectLink(objectRevision, true)
            refreshMessages(channel)
            LogManager.addLog("INFO", "Reply", "history jump ch=$channel id=$id rows=${rows.size}")
        }
    }

    fun showLatestMessages() {
        _historyAnchor.value = ""
        pendingLatestJump = MessageJump("", ++jumpRevision)
        _currentChannel.value?.id?.let(::refreshMessages)
    }

    /** Main: поставить JSON-команду feedback в native; userAction даёт ненулевой ID для показа отказа пользователю.
     * Успех enqueue не меняет результаты оптимистично: FEEDBACK_RESULT применяет снимок текущего канала.
     * После committed/snapshot_pending повторяется только feedback-чтение, а не изменяющая команда. */
    private fun sendInteraction(command: JSONObject, userAction: Boolean = true) {
        if (!command.has("ch")) command.put("ch", _currentChannel.value?.id ?: return)
        if (!NativeLib.feedbackCommand(command.toString(), if (userAction) ++nextInteractionId else 0)) {
            LogManager.addLog("ERROR", "POLL", "cannot queue command ${command.optString("cmd")}")
            if (userAction) _interactionFailed.value = true
        }
    }

    fun createPoll(question: String, options: List<String>, multiple: Boolean, custom: Boolean) {
        sendInteraction(JSONObject().put("cmd", "poll_create").put("question", question)
            .put("options", JSONArray(options)).put("mode", if (multiple) "multiple" else "single").put("allow_custom_answer", custom))
    }

    fun vote(message: Message, choices: List<Int>, custom: String) {
        sendInteraction(JSONObject().put("cmd", "vote").put("message_id", message.messageId)
            .put("choices", JSONArray(choices)).put("custom", custom))
    }

    fun react(message: Message, emoji: Int?) {
        val command = JSONObject().put("cmd", "react").put("message_id", message.messageId)
        if (emoji == null) command.put("clear", true) else command.put("emoji", emoji)
        sendInteraction(command)
    }

    fun dismissInteractionError() { _interactionFailed.value = false }

    fun selectChannel(channel: Channel, preserveObjectLink: Boolean = false) {
        if (!preserveObjectLink) cancelObjectNavigation()
        val r = repo ?: return
        if (_currentChannel.value?.id != channel.id) {
            _editing.value = null
            _streamSources.value = emptyList(); _streamSourcesFailed.value = false; selectStream(null)
            _historyAnchor.value = ""
            _messageJump.value = null
            pendingLatestJump = null
            _replyOriginals.value = emptyMap()
            ++jumpRevision
            feedbackCache.clear()
            memberDetailJob?.cancel(); memberDetailTarget = null; _selectedMember.value = null
            _channelOwner.value = false
        }
        LogManager.addLog("DEBUG", "radio", "navigation from=${_currentChannel.value?.id} to=${channel.id} active=$radioChannelId")
        _currentChannel.value = channel
        watchStreams()
        _radioActive.value = (radioChannelId == channel.id)
        _radioTalking.value = if (_radioActive.value) _radioTalkState.value.speakers.firstOrNull() else null
        _soundOn.value = !NotificationSound.isMuted(channel.id)
        NativeLib.setSoundOnMessage(channel.id, _soundOn.value)
        r.connectChannel(channel.id)
        refreshMessages(channel.id)
        refreshMembers(channel.id)
    }

    /* ── per-channel «play sound on message» ── */
    fun toggleSound() {
        val ch = _currentChannel.value ?: return
        val on = !_soundOn.value
        val provider = ChatApplication.instance.configProvider
        provider.changes.submit("channel notification sound") {
            provider.setChannelMuted(ch.id, !on)
            NotificationSound.setMuted(ch.id, !on)
            if (NativeLib.isReady()) NativeLib.setSoundOnMessage(ch.id, on)
            LogManager.addLog("INFO", "Sound", "Channel sound saved channel=${ch.id} enabled=$on")
        }
    }

    /* ── DM (личные сообщения) ── */

    private fun refreshDmCallRoute() {
        if (dmPresenceRefreshJob?.isActive == true) return
        dmPresenceRefreshJob = viewModelScope.launch {
            kotlinx.coroutines.delay(50)
            if (!NativeLib.isReady()) { _dmCallAvailable.value = false; return@launch }
            val conversations = _dmConversations.value + listOfNotNull(_currentDm.value)
            conversations.distinctBy { it.convId }.forEach { NativeLib.dmRequestCallRoute(it.convId) }
        }
    }

    fun startDmCall() {
        val dm = _currentDm.value ?: return
        if (!_dmCallAvailable.value) {
            LogManager.addLog("WARN", "Call", "PM call unavailable conv=${dm.convId}"); return
        }
        startCall("dm:${dm.convId}", null, dm.displayName)
    }

    fun refreshDmConversations() {
        if (repo != null) NativeLib.dmRequestConversations()
    }

    fun refreshDmMessages() {
        val dm = _currentDm.value ?: return
        if (repo != null && dm.convId !in _dmDeleting.value) NativeLib.dmRequestMessages(dm.convId, dm.generation)
    }

    fun startDm(channelId: String, peer: String, name: String, groups: Boolean = false) {
        val r = repo ?: run {
            _dmStartFailed.value = true; LogManager.addLog("WARN", "PM", "Conversation start rejected: repository unavailable"); return
        }
        if (!MemberFilesProtocol.validAddress(peer) || peer == NativeLib.getMySigningAddress()) return
        cancelDmStart(); _dmStartFailed.value = false
        try {
            val conversation = checkNotNull(r.dmStart(peer, channelId, name, groups)) { "Conversation start rejected" }
            val token = pendingDmOpen.begin(conversation, peer, name)
            LogManager.addLog("INFO", "PM", "Conversation start accepted channel=$channelId peer=$peer conv=$conversation")
            dmStartTimeout = viewModelScope.launch {
                delay(10_000)
                if (pendingDmOpen.isPending(token)) {
                    pendingDmOpen.cancel(); _dmStartFailed.value = true
                    LogManager.addLog("ERROR", "PM", "Conversation start confirmation timed out conv=$conversation peer=$peer")
                }
            }
        } catch (e: Exception) { _dmStartFailed.value = true; LogManager.addLog("ERROR", "PM", "Conversation start failed peer=$peer: $e") }
    }

    fun consumeDmOpen() { _dmOpen.value = null }

    fun openDm(fallback: DmConversation) {
        cancelDmStart()
        if (repo == null) return
        _editing.value = null
        val dm = _dmConversations.value.firstOrNull { it.convId == fallback.convId && it.generation == fallback.generation } ?: fallback
        _currentDm.value = dm
        _dmCallAvailable.value = false
        refreshDmCallRoute()
        _dmMessages.value = emptyList()
        refreshDmMessages()
    }

    fun closeDm() {
        _currentDm.value = null
        _dmCallAvailable.value = false
        _dmMessages.value = emptyList()
    }

    /** Единая проверка PM до записи, копирования и native-публикации; ядро повторяет проверку поколения. */
    private fun dmTargetCurrent(target: ConversationTarget.Pm): Boolean {
        val dm = _dmConversations.value.find { it.convId == target.id } ?: _currentDm.value?.takeIf { it.convId == target.id }
        if (dm?.generation != target.generation || dm.closed || target.id in _dmDeleting.value) {
            LogManager.addLog("INFO", "PM", "Discarded stale operation conv=${target.id} generation=${target.generation}"); return false
        }
        return true
    }

    fun sendDmText(target: ConversationTarget.Pm, text: String): Deferred<Boolean> = viewModelScope.async {
        val r = repo ?: return@async false
        if (text.isBlank() || !dmTargetCurrent(target)) return@async false
        submitMessage(target.nativeId, null) { r.dmSend(target.id, text, target.generation, it) }
    }

    fun deleteDmConversation(dm: DmConversation) {
        if (dm.convId in _dmDeleting.value) return
        val request = ++nextDmDeleteRequest
        dmDeleteRequests[request] = dm.convId to dm.generation
        _dmDeleting.value = _dmDeleting.value + dm.convId
        LogManager.addLog("INFO", "PM", "Delete requested conv=${dm.convId} generation=${dm.generation} request=$request")
        if (!NativeLib.dmDelete(dm.convId, dm.generation, request)) {
            dmDeleteRequests.remove(request); _dmDeleting.value = _dmDeleting.value - dm.convId; _dmDeleteFailed.value = true
            LogManager.addLog("ERROR", "PM", "Delete request rejected conv=${dm.convId}")
        }
    }

    fun dismissDmDeleteError() { _dmDeleteFailed.value = false }

    fun showPmInvite(peer: String, name: String, source: String = "") {
        if (!MemberFilesProtocol.validAddress(peer) || peer == NativeLib.getMySigningAddress()) {
            LogManager.addLog("ERROR", "INVITE", "invalid PM recipient"); return
        }
        if (_pmInvite.value != null) return
        val groups = _channels.value.filter { NativeLib.isLocalChannelMember(it.id) }
        _pmInvite.value = PmInviteState(peer, name, source, groups)
        LogManager.addLog("INFO", "INVITE", "PM group picker peer=$peer source=$source groups=${groups.size}")
    }

    fun selectPmInviteGroup(groupId: String) {
        _pmInvite.value?.let { if (!it.pending) _pmInvite.value = it.copy(groupId = groupId, errorRes = 0) }
    }

    fun sendPmInvite() {
        val state = _pmInvite.value ?: return
        if (state.pending) return
        val channel = state.groupId.takeIf { it.length == 64 } ?: return
        val request = ++nextPmInviteId
        _pmInvite.value = state.copy(requestId = request, pending = true, errorRes = 0)
        if (!NativeLib.dmInvite(request, state.peer, state.source, channel, state.name)) {
            _pmInvite.value = state.copy(errorRes = R.string.invite_core_unavailable)
        }
    }

    fun dismissPmInvite() {
        val state = _pmInvite.value ?: return
        _pmInvite.value = null
        if (state.pending) NativeLib.dmInvite(state.requestId, state.peer, state.source, "", state.name, cancel = true)
        LogManager.addLog("INFO", "INVITE", "PM group picker closed request=${state.requestId} pending=${state.pending}")
    }

    fun openInviteLink(link: String, serverChannel: String = "") {
        if (_linkJoin.value?.pending == true) return
        val invite = InviteLink.decode(link)
        if (invite == null) {
            LogManager.addLog("ERROR", "JOIN", "invalid message invite link")
            _linkJoin.value = LinkJoinState(link, "", "", pending = false, result = -1); return
        }
        val serverAddition = invite.kind == com.utun.chat.data.INVITE_KIND_SERVER
        if (serverAddition && serverChannel.isEmpty()) { _serverLink.value = link; return }
        val channelId = if (serverAddition) serverChannel else ""
        val peer = MemberFilesProtocol.addressHex(invite.pubkey)
        if (serverAddition && !NativeLib.isLocalChannelMember(channelId)) {
            LogManager.addLog("WARN", "JOIN", "server addition requires own channel ch=$channelId")
            _linkJoin.value = LinkJoinState(link, channelId, peer, pending = false, result = -7, serverAddition = true); return
        }
        if (repo == null || !NativeLib.isReady()) {
            LogManager.addLog("ERROR", "JOIN", "core unavailable for message invite")
            _linkJoin.value = LinkJoinState(link, channelId, peer, pending = false, result = -7, serverAddition = serverAddition); return
        }
        val request = NativeLib.nextJoinRequest()
        _serverLink.value = null
        _linkJoin.value = LinkJoinState(link, channelId, peer, request = request, serverAddition = serverAddition)
        val result = if (serverAddition) NativeLib.addServer(channelId, link, request) else repo?.joinChannel(link, request) ?: -7
        if (result != 0) _linkJoin.value = _linkJoin.value?.copy(pending = false, result = result)
    }

    fun dismissLinkJoin() { _linkJoin.value = null }
    fun consumeJoinedChannel() { _joinedChannel.value = null }

    fun refreshMembers(chId: String) {
        val r = repo ?: return
        membersRepository.refresh(chId)
        memberOwnerJob?.cancel()
        val generation = membersRepository.generation
        memberOwnerJob = viewModelScope.launch {
            val owner = withContext(Dispatchers.IO) { r.isChannelOwner(chId) }
            if (_currentChannel.value?.id == chId && generation == membersRepository.generation) _channelOwner.value = owner
        }
    }

    fun showMemberDetail(channelId: String, peer: String) {
        if (!MemberFilesProtocol.validAddress(peer)) return
        memberDetailTarget = channelId to peer
        refreshSelectedMemberDetail(channelId)
    }

    /** Повторная проверка живого снимка при подтверждении связывания; ViewModel не хранит окна/Context. */
    fun linkMyDevice(channelId: String, peer: String): Boolean {
        val member = membersRepository.members.value.entries.firstOrNull { it.key.group == channelId && it.value.ed25519 == peer }?.value
        val group = channelId.takeIf { it.length == 64 }
        if (_currentChannel.value?.id != channelId || group == null ||
            member?.canLinkAsMyDevice != true || !NativeLib.isReady()) {
            LogManager.addLog("WARN", "Devgroup", "Link cancelled: target changed/unavailable channel=$channelId peer=$peer")
            return false
        }
        LogManager.addLog("INFO", "Devgroup", "Link confirmed channel=$channelId peer=$peer")
        DevgroupRepository.command(1, group, peer)
        return true
    }

    fun clearMemberDetail() {
        memberDetailJob?.cancel(); memberDetailTarget = null
        _selectedMember.value = null; _memberLinks.value = MemberLinks()
    }

    /* Перечитываем родителя при изменении участника или его инвайтера, сохраняя актуальный live-статус. */
    private fun refreshSelectedMemberDetail(channelId: String, changedPeer: String? = null) {
        val target = memberDetailTarget ?: return
        if (target.first != channelId || _currentChannel.value?.id != channelId) return
        if (changedPeer != null && changedPeer != target.second && !changedPeer.equals(_selectedMember.value?.inviterId, true)) return
        val r = repo ?: return
        memberDetailJob?.cancel()
        val generation = membersRepository.generation
        memberDetailJob = viewModelScope.launch {
            val d = withContext(Dispatchers.IO) { r.getMemberDetail(channelId, target.second) }
            if (memberDetailTarget != target || _currentChannel.value?.id != channelId || generation != membersRepository.generation) return@launch
            _selectedMember.value = d?.withMember(membersRepository.member(channelId, target.second))
        }
    }

    fun setMemberNick(channelId: String, ed: String, nick: String) {
        val r = repo ?: return
        r.setMemberNick(channelId, ed, nick)
        refreshMembers(channelId)
        refreshSelectedMemberDetail(channelId, ed)
    }

    fun toggleMemberRadioMute(channelId: String, address: String) {
        if (repo == null || !NativeLib.toggleMemberRadioMute(channelId, address)) _radioMuteFailed.value = true
    }

    fun dismissRadioMuteError() { _radioMuteFailed.value = false }

    fun setMemberFlags(channelId: String, ed: String, admin: Boolean, supernode: Boolean, moder: Boolean, storage: Boolean,
                       verified: Boolean, deleted: Boolean) {
        val r = repo ?: return
        r.setMemberFlags(channelId, ed, admin, supernode, moder, storage, verified, deleted)
        refreshMembers(channelId)
    }

    fun transferAdmin(channelId: String, ed25519: String) {
        val r = repo ?: return
        r.transferAdmin(channelId, ed25519)
    }

    fun connectMemberNode(channelId: String, ed25519: String) {
        val r = repo ?: return
        r.connectMemberNode(channelId, ed25519)
        refreshMembers(channelId)
    }

    fun refreshMemberLinks(peer: String) {
        if (peer.isEmpty()) { _memberLinks.value = MemberLinks(); return }
        val r = repo ?: return
        val generation = membersRepository.generation
        viewModelScope.launch {
            val links = withContext(Dispatchers.IO) { r.getMemberLinks(peer) } ?: return@launch
            if (_selectedMember.value?.ed25519 == peer && generation == membersRepository.generation) _memberLinks.value = links
        }
    }

    fun refreshLocalSockets() {
        if (repo != null) NativeLib.requestLocalSockets()
    }

    fun sendMessage(text: String) {
        val ch = _currentChannel.value ?: return
        val target = ConversationTarget.Channel(ch.id)
        val reply = replyFor(target)
        viewModelScope.launch {
            if (sendChannelText(target, text, reply).await()) finishReply(reply)
        }
    }

    fun createChannel(name: String, channelId: String, groupType: String = "chat") {
        val r = repo ?: return
        r.createChannel(name, channelId, groupType)
        refreshChannels()
    }

    fun deleteChannel(channelId: String) {
        val r = repo ?: return
        LogManager.addLog("INFO", "VM", "deleteChannel ch=$channelId")
        r.deleteChannel(channelId)
        if (radioChannelId == channelId) turnRadioOff()
        if (_currentChannel.value?.id == channelId) {
            _currentChannel.value = null
            _messages.value = emptyList()
            _members.value = emptyList()
            _channelOwner.value = false
        }
        NotificationSound.setMuted(channelId, false)
        refreshChannels()
    }

    fun connectNode(address: String, port: Int, pubkeyHex: String) {
        val r = repo ?: return
        r.connectNode(address, port, pubkeyHex)
    }

    private var recordingStopping = false
    private var recordingTarget: ConversationTarget? = null
    private var recordingReply: ChannelReply? = null
    private var recordingSubmission: Pair<Long, CompletableDeferred<Boolean>>? = null

    fun startVoiceRecording(target: ConversationTarget) {
        if (target is ConversationTarget.Card) {
            LogManager.addLog("WARN", "Kanban", "Voice recording unavailable group=${target.id} card=${target.cardId}"); return
        }
        if (target is ConversationTarget.Pm && !dmTargetCurrent(target)) return
        if (recordingStopping) { LogManager.addLog("WARN", "VM", "recording is still stopping"); return }
        if (_isRecording.value) { LogManager.addLog("WARN", "VM", "recording already active"); return }
        if (target.nativeId in _submittingChannels.value || composerSubmissions.states.value[target]?.finished == false) {
            LogManager.addLog("WARN", "Reply", "voice start blocked by pending submission ch=${target.id}"); return
        }
        val reply = replyFor(target)
        val request = submissionIds.getAndIncrement() to CompletableDeferred<Boolean>()
        submissions[request.first] = request.second
        if (!audioRecorder.startRecording(target.nativeId, reply?.message?.messageId.orEmpty(), request.first,
                (target as? ConversationTarget.Pm)?.generation ?: 0, reply?.quote.orEmpty())) {
            submissions.remove(request.first)
            LogManager.addLog("ERROR", "VM", "voice recording failed to start")
            return
        }
        _isRecording.value = true
        recordingTarget = target
        recordingReply = reply
        recordingSubmission = request
        _isRecordingLocked.value = false
        recordingStartTimeMs = System.currentTimeMillis()
        _recordingDurationMs.value = 0
        LogManager.addLog("INFO", "VM", "voice recording started target=${target.nativeId}")
    }

    fun lockVoiceRecording() {
        _isRecordingLocked.value = true
        LogManager.addLog("DEBUG", "VM", "voice recording locked")
    }

    fun stopVoiceRecording() {
        if (!_isRecording.value || recordingStopping) return
        val target = recordingTarget
        if (target is ConversationTarget.Pm && !dmTargetCurrent(target)) { cancelVoiceRecording(target); return }
        recordingStopping = true
        _isRecording.value = false
        _isRecordingLocked.value = false
        _recordingDurationMs.value = 0
        val request = recordingSubmission
        val reply = recordingReply
        if (request != null) {
            submissionChannels[request.first] = recordingTarget!!.nativeId
            _submittingChannels.value = submissionChannels.values.toSet()
        }
        viewModelScope.launch {
            var discarded = false
            try {
                val duration = withContext(Dispatchers.IO) { audioRecorder.stopRecording() }
                LogManager.addLog("INFO", "VM", "voice detached target=${recordingTarget?.nativeId} duration=$duration")
                if (request != null && duration < AudioRecorderManager.MIN_DURATION_MS) {
                    discarded = duration >= 0
                    submissions.remove(request.first)
                    submissionChannels.remove(request.first)
                    _submittingChannels.value = submissionChannels.values.toSet()
                    request.second.complete(false)
                    if (duration < 0) _messageSendFailed.value = true
                }
            } catch (e: kotlinx.coroutines.CancellationException) { throw e }
            catch (e: Exception) {
                LogManager.addLog("ERROR", "Voice", "Recording stop failed target=${target?.nativeId}: $e")
                request?.let { submissionResult(it.first, false) }
                _messageSendFailed.value = true
            } finally {
                recordingStopping = false
                recordingTarget = null
                recordingSubmission = null
                recordingReply = null
            }
            if (request != null) {
                if (request.second.await()) finishReply(reply)
                else if (!discarded) {
                    _messageSendFailed.value = true
                    LogManager.addLog("ERROR", "Reply", "voice submission failed request=${request.first}")
                }
            }
        }
    }

    fun cancelVoiceRecording(target: ConversationTarget? = null) {
        if (recordingStopping || !_isRecording.value || (target != null && target != recordingTarget)) return
        recordingStopping = true
        _isRecording.value = false
        _isRecordingLocked.value = false
        _recordingDurationMs.value = 0
        recordingSubmission?.let { submissions.remove(it.first); it.second.complete(false) }
        recordingSubmission = null
        recordingReply = null
        viewModelScope.launch {
            try { withContext(Dispatchers.IO) { audioRecorder.cancelRecording() } }
            catch (e: kotlinx.coroutines.CancellationException) { throw e }
            catch (e: Exception) { LogManager.addLog("ERROR", "Voice", "Recording cancel failed: $e"); _messageSendFailed.value = true }
            finally { recordingStopping = false; recordingTarget = null }
        }
        LogManager.addLog("INFO", "VM", "voice recording cancelled target=${recordingTarget?.nativeId}")
    }

    fun sendVoiceMessage() {
        stopVoiceRecording()
    }

    fun sendAttachment(filePath: String, target: ConversationTarget, name: String = File(filePath).name,
                       reply: ChannelReply? = replyFor(target)): Deferred<Boolean> = viewModelScope.async {
        val ext = filePath.substringAfterLast('.', "").lowercase()
        if (ext in IMAGE_EXTENSIONS && (target is ConversationTarget.Channel || ext in setOf("jpg", "jpeg", "png")))
            return@async sendPhotoMessage(filePath, target, reply, clearReply = false).await()
        var accepted = false
        try {
            submitMessage(target.nativeId, reply) { request ->
                (when (target) {
                    is ConversationTarget.Pm -> dmTargetCurrent(target) && NativeLib.dmSendFile(target.id, filePath, name, target.generation, request)
                    is ConversationTarget.Card -> NativeLib.kanbanAttachmentSend(target.id, target.cardId, filePath,
                        "application/octet-stream", request = request)
                    is ConversationTarget.Channel -> NativeLib.attachmentSend(target.id, filePath, reply?.message?.messageId.orEmpty(), request,
                        reply?.quote.orEmpty())
                }).also { accepted = it }
            }
        } finally {
            withContext(kotlinx.coroutines.NonCancellable + Dispatchers.IO) { cleanupAttachmentCache(filePath, !accepted) }
        }
    }

    /** Cache-файл передаётся native только после успешного чтения метаданных. */
    fun sendVideoMessage(filePath: String, target: ConversationTarget, reply: ChannelReply? = replyFor(target),
                         clearReply: Boolean = true): Deferred<Boolean> = viewModelScope.async {
        val job = java.util.UUID.randomUUID().toString()
        preparationState(target.nativeId, job, 0)
        var accepted = false
        var saved = false
        try {
            val meta = withContext(Dispatchers.IO) { VideoFileMetadata().use { it.submission(filePath) } }
            val queue: (Long) -> Boolean = { request ->
                if (target is ConversationTarget.Pm && !dmTargetCurrent(target)) {
                    discardedPreparationJobs.add(job); false
                } else if (target is ConversationTarget.Card) NativeLib.kanbanAttachmentSend(target.id, target.cardId, filePath,
                    "video/mp4", meta.width, meta.height, meta.durationMs, request).also { accepted = it }
                else NativeLib.videoSend(target.nativeId, filePath, meta.durationMs, meta.width, meta.height,
                    reply?.message?.messageId.orEmpty(), request, (target as? ConversationTarget.Pm)?.generation ?: 0,
                    reply?.quote.orEmpty()).also { accepted = it }
            }
            saved = submitMessage(target.nativeId, reply, queue = queue)
            if (!saved && job !in discardedPreparationJobs) _messageSendFailed.value = true
            if (saved && clearReply) finishReply(reply)
        } catch (e: kotlinx.coroutines.CancellationException) { throw e }
        catch (e: Exception) {
            LogManager.addLog("ERROR", "VM", "video failed target=${target.nativeId}: ${e.message}")
            _messageSendFailed.value = true
        } finally {
            preparationState(target.nativeId, job, if (saved) 1 else 2)
            withContext(kotlinx.coroutines.NonCancellable + Dispatchers.IO) { cleanupAttachmentCache(filePath, !accepted) }
        }
        saved
    }

    /** Снимок камеры и выбранный JPG/PNG имеют один путь и неизменного адресата. */
    fun sendPhotoMessage(filePath: String, target: ConversationTarget, reply: ChannelReply?,
                         clearReply: Boolean = true): Deferred<Boolean> = viewModelScope.async {
        var accepted = false
        var saved = false
        var legacy = false
        var completed = false
        try {
            val meta = withContext(Dispatchers.IO) { PhotoUtil.metadata(filePath) }
            legacy = meta.mime !in setOf("image/jpeg", "image/png")
            val queue: (Long) -> Boolean = { request ->
                if (target is ConversationTarget.Pm && !dmTargetCurrent(target)) {
                    LogManager.addLog("INFO", "Image", "Discarded image for deleted PM target=${target.nativeId}"); false
                } else if (target is ConversationTarget.Card) NativeLib.kanbanAttachmentSend(target.id, target.cardId, filePath,
                    meta.mime, meta.width, meta.height, request = request).also { accepted = it }
                else NativeLib.photoSend(target.nativeId, filePath, meta.width, meta.height, meta.mime,
                    reply?.message?.messageId.orEmpty(), request, (target as? ConversationTarget.Pm)?.generation ?: 0,
                    reply?.quote.orEmpty()).also { accepted = it }
            }
            saved = submitMessage(target.nativeId, reply, queue = queue)
            completed = true
            if (!saved) _messageSendFailed.value = true
            if (saved && clearReply) finishReply(reply)
            LogManager.addLog("INFO", "Image", "Image submission target=${target.nativeId} mime=${meta.mime} " +
                "dimensions=${meta.width}x${meta.height} accepted=$accepted saved=$saved")
        } catch (e: kotlinx.coroutines.CancellationException) { throw e }
        catch (e: Exception) {
            LogManager.addLog("ERROR", "Image", "Image submission failed target=${target.nativeId}: ${e.message}")
            _messageSendFailed.value = true
        } finally {
            withContext(kotlinx.coroutines.NonCancellable + Dispatchers.IO) { cleanupAttachmentCache(filePath, !accepted || (legacy && completed)) }
        }
        saved
    }

    fun downloadAttachment(msg: Message) {
        val ch = _currentChannel.value ?: return
        NativeLib.attachmentDownload(ch.id, msg.id)
        viewModelScope.launch {
            delay(500)
            refreshMessages(ch.id)
        }
    }

    fun downloadDmAttachment(target: ConversationTarget.Pm, msg: Message) {
        if (!dmTargetCurrent(target)) return
        if (!NativeLib.dmMediaAction(target.id, msg.authorAddress, msg.id, target.generation, false))
            LogManager.addLog("WARN", "PM", "Download rejected conv=${target.id} author=${msg.authorAddress} seq=${msg.id}")
    }

    /** Вызывается при явном открытии; сама подготовка не отправляет TAKEN. */
    fun prepareDmAttachment(target: ConversationTarget.Pm, msg: Message) {
        if (msg.filePath.isNotEmpty()) openingDmFiles[msg.filePath] = DmFileOpening(target, msg.authorAddress, msg.id)
    }

    /** Просмотрщики вызывают после успешного декодирования, voice — после первых принятых AudioTrack сэмплов. */
    fun attachmentOpened(path: String) {
        viewModelScope.launch {
            val action = openingDmFiles.remove(path) ?: return@launch
            if (!NativeLib.dmMediaAction(action.target.id, action.author, action.seq, action.target.generation, true))
                LogManager.addLog("WARN", "PM", "Open acknowledgement rejected conv=${action.target.id} seq=${action.seq}")
        }
    }

    fun getVoicePreset(): Int = NativeLib.voiceGetPreset()
    fun setVoicePreset(preset: Int) { NativeLib.voiceSetPreset(preset) }
    fun getVoiceCompressor(): Boolean = NativeLib.voiceGetCompressor()
    fun setVoiceCompressor(enabled: Boolean) { NativeLib.voiceSetCompressor(enabled) }
    fun setCompressorConfig(maxGainDb: Int, lookbackMs: Int, lookaheadMs: Int,
                             riseRate: Float, targetLevelDb: Float, gainOnlyOnSpeech: Boolean, vadMaxGainDb: Int) {
        NativeLib.voiceSetCompressorConfig(maxGainDb, lookbackMs, lookaheadMs, riseRate, targetLevelDb, gainOnlyOnSpeech, vadMaxGainDb)
    }

    fun playVoiceMessage(filePath: String, durMs: Int) {
        if (CallController.blocksMedia()) { LogManager.addLog("WARN", "Voice", "Playback rejected: call owns audio"); return }
        var reported = false
        val progress: (Float, Int) -> Unit = { _, elapsed ->
            realPlaybackElapsedMs = elapsed
            if (!reported && elapsed > 0) { reported = true; attachmentOpened(filePath) }
        }
        val cur = _playbackState.value
        if (cur.filePath == filePath) {
            if (cur.paused) {
                realPlaybackElapsedMs = cur.elapsedMs
                audioRecorder.playFromPosition(filePath, cur.elapsedMs,
                    onProgress = progress,
                    onComplete = { realPlaybackElapsedMs = durMs })
                _playbackState.value = cur.copy(playing = true, paused = false)
            } else {
                audioRecorder.pausePlayback()
                realPlaybackElapsedMs = 0
                _playbackState.value = cur.copy(playing = false, paused = true)
            }
            return
        }
        audioRecorder.stopPlayback()
        realPlaybackElapsedMs = 0
        _playbackState.value = PlaybackState(filePath, playing = true, durMs = durMs)
        audioRecorder.playVoiceFile(filePath,
            onProgress = progress,
            onComplete = {
                realPlaybackElapsedMs = 0
                _playbackState.value = PlaybackState()
            })
    }

    fun seekVoice(filePath: String, seekMs: Int, durMs: Int) {
        if (CallController.blocksMedia()) { LogManager.addLog("WARN", "Voice", "Seek rejected: call owns audio"); return }
        audioRecorder.stopPlayback()
        var reported = false
        realPlaybackElapsedMs = seekMs
        _playbackState.value = PlaybackState(filePath, playing = true, durMs = durMs, elapsedMs = seekMs,
            progress = if (durMs > 0) seekMs.toFloat() / durMs.toFloat() else 0f)
        audioRecorder.playFromPosition(filePath, seekMs,
            onProgress = { _, elapsed ->
                realPlaybackElapsedMs = elapsed
                if (!reported && elapsed > seekMs) { reported = true; attachmentOpened(filePath) }
            },
            onComplete = {
                realPlaybackElapsedMs = 0
                _playbackState.value = PlaybackState()
            })
    }

    /** IO: удалять только собственный cache; принятый native source остаётся его worker до завершения. */
    private fun cleanupAttachmentCache(filePath: String, deleteSource: Boolean) {
        try {
            val file = File(filePath)
            if (deleteSource && !file.delete() && file.exists()) LogManager.addLog("ERROR", "Attachment", "Cache removal failed path=$filePath")
            val parent = file.parentFile
            if (parent?.parentFile?.name == "attachments" && parent.list()?.isEmpty() == true && !parent.delete() && parent.exists()) {
                LogManager.addLog("ERROR", "Attachment", "Empty cache directory removal failed path=$parent")
            }
        } catch (e: Exception) { LogManager.addLog("ERROR", "Attachment", "Cache cleanup failed path=$filePath: $e") }
    }

    suspend fun copyUriToCache(context: Context, uri: Uri, fileName: String): String? {
        var cacheFile: File? = null
        try {
            val name = fileName.substringAfterLast('/').substringAfterLast('\\')
            require(name.isNotBlank() && name != "." && name != "..") { AppLanguage.text(R.string.invalid_file_name) }
            val cacheDir = File(context.cacheDir, "attachments/${java.util.UUID.randomUUID()}")
            check(cacheDir.mkdirs()) { AppLanguage.text(R.string.attachment_directory_failed) }
            val outputFile = File(cacheDir, name)
            cacheFile = outputFile
            context.contentResolver.openInputStream(uri)?.use { input ->
                FileOutputStream(outputFile).use { output ->
                    val buffer = ByteArray(65536); var size = 0L
                    while (true) {
                        kotlinx.coroutines.currentCoroutineContext().ensureActive()
                        val n = input.read(buffer)
                        if (n < 0) break
                        size += n; check(size <= 1024L * 1024 * 1024) { AppLanguage.text(R.string.file_too_large) }
                        output.write(buffer, 0, n)
                    }
                }
            } ?: throw IllegalStateException(AppLanguage.text(R.string.attachment_open_failed))
            LogManager.addLog("INFO", "VM", "copied attachment to cache: ${outputFile.absolutePath}")
            return outputFile.absolutePath
        } catch (e: Exception) {
            cacheFile?.let { cleanupAttachmentCache(it.path, true) }
            if (e is kotlinx.coroutines.CancellationException) throw e
            LogManager.addLog("ERROR", "VM", "copyUriToCache failed: ${e.message}")
            return null
        }
    }

    /** IO caller: копирует локальный файл в выбранный SAF-документ; true только после закрытия обоих потоков. */
    fun saveDownloadedFile(srcPath: String, saveUri: Uri): Boolean {
        try {
            val src = File(srcPath)
            if (!src.isFile) {
                LogManager.addLog("ERROR", "VM", "saveDownloadedFile: src not found $srcPath")
                return false
            }
            val ctx = ChatApplication.instance
            ctx.contentResolver.openOutputStream(saveUri)?.use { output ->
                src.inputStream().use { input -> input.copyTo(output) }
            } ?: throw java.io.IOException("Cannot open selected document")
            LogManager.addLog("INFO", "VM", "file saved: $srcPath -> $saveUri")
            return true
        } catch (e: kotlinx.coroutines.CancellationException) {
            throw e
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "VM", "saveDownloadedFile failed: ${e.message}")
            return false
        }
    }

    data class DatabaseResetState(val running: Boolean = false, val failed: Boolean = false)
    private val _databaseReset = MutableStateFlow(DatabaseResetState())
    val databaseReset: StateFlow<DatabaseResetState> = _databaseReset

    fun dismissDatabaseResetError() { _databaseReset.update { it.copy(failed = false) } }

    /** Принятый сброс принадлежит очереди Application, включая закрытие Activity; NativeLib сериализует stop/clear/start. */
    fun resetDatabase() {
        if (_databaseReset.value.running || _generatingKeys.value) return
        val retryCleanup = _databaseReset.value.failed && NativeLib.isCoreStopping()
        _databaseReset.value = DatabaseResetState(running = true)
        val provider = ChatApplication.instance.configProvider
        provider.changes.submit("reset database") {
            try {
                if (!retryCleanup) HeadlessService.awaitStartup()
                val restore = (NativeLib.isReady() || retryCleanup) && withContext(Dispatchers.IO) { provider.getServiceEnabled() }
                val path = withContext(Dispatchers.IO) { provider.getString("db.path") ?: ChatApplication.instance.filesDir.absolutePath }
                val config = if (restore) provider.buildConfigText().await() else null
                NativeLib.resetDatabase(path, 9999, config, ::clearState)
                LogManager.addLog("INFO", "CFG", "Database reset completed directory=$path restart=$restore")
            } catch (e: kotlinx.coroutines.CancellationException) { throw e
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CFG", "database reset failed: ${e.javaClass.simpleName}: ${e.message}")
                _databaseReset.update { it.copy(failed = true) }
                throw e
            } finally { _databaseReset.update { it.copy(running = false) } }
        }
    }

    /** Main, после подтверждённого join worker: убрать все ссылки на удаляемую БД и завершить её ожидающие отправки. */
    fun clearState() {
        cancelObjectNavigation()
        cancelDmStart(); _dmStartFailed.value = false; _dmOpen.value = null
        openingDmFiles.clear()
        ++channelRefreshRevision; ++messageRefreshRevision; ++jumpRevision
        ++editRevision; ++replyRevision
        for (id in submissions.keys.toList()) submissionResult(id, false)
        _editing.value = null; _replies.value = emptyMap(); _replyOriginals.value = emptyMap(); cameraReply = null
        _historyAnchor.value = ""; _messageJump.value = null; pendingLatestJump = null; _originalMissing.value = false
        feedbackCache.clear(); _reactionCatalog.value = emptyList()
        memberOwnerJob?.cancel(); _members.value = emptyList(); _memberLinks.value = MemberLinks()
        memberDetailJob?.cancel(); memberDetailTarget = null; _selectedMember.value = null
        dismissPmInvite(); _linkJoin.value = null; _joinedChannel.value = null; _serverLink.value = null
        clearInvite()
        _connecting.value = emptyMap(); dmPresenceRefreshJob?.cancel(); _dmCallAvailable.value = false
        _currentDm.value = null; _dmConversations.value = emptyList(); _dmMessages.value = emptyList()
        dmDeleteRequests.clear(); _dmDeleting.value = emptySet(); _dmDeleteFailed.value = false
        _attachmentPreparations.value = emptyMap(); discardedPreparationJobs.clear()
        repo?.close()
        repo = null
        dbReady = false
        _channels.value = emptyList()
        _messages.value = emptyList()
        _currentChannel.value = null
        _streamSources.value = emptyList(); selectStream(null); _streamSourcesFailed.value = false
        _generatingKeys.value = false
        _localSockets.value = emptyList()
        _channelOwner.value = false
        _playbackState.value = PlaybackState(); realPlaybackElapsedMs = 0
        _radioActive.value = false; _radioTalking.value = null; _radioTalkState.value = RadioTalkState()
        LogManager.addLog("INFO", "VM", "Database state cleared: conversations, drafts, members, history and pending submissions")
    }

    override fun onCleared() {
        clearInvite()
        audioRecorder.requestClose()
        super.onCleared()
    }

    class Factory : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T = ChatViewModel() as T
    }
}
