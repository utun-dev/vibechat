package com.utun.chat.ui.screens

import android.view.WindowManager
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import com.utun.chat.ChatApplication
import com.utun.chat.R
import com.utun.chat.data.Channel
import com.utun.chat.data.ConversationTarget
import com.utun.chat.data.LogManager
import com.utun.chat.data.KanbanMessage
import com.utun.chat.data.Message
import com.utun.chat.data.ObjectLinks
import com.utun.chat.ui.components.messageLinkHandler
import com.utun.chat.ui.components.messageLongPress
import com.utun.chat.data.StreamController
import com.utun.chat.data.parseKanbanMessage
import com.utun.chat.ui.components.MessageBubble
import com.utun.chat.ui.components.ConversationComposer
import com.utun.chat.ui.components.UserAvatar
import com.utun.chat.viewmodel.ChatViewModel
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File

/** Экран карточки: текст, исполнители, постраничный чат. Команды и корреляция ответов принадлежат доске. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun KanbanCardScreen(channel: Channel, card: JSONObject, board: JSONObject, boardFailed: Boolean,
    onCommand: (JSONObject, (JSONObject) -> Unit) -> Long, viewModel: ChatViewModel,
    onViewPhoto: (Message) -> Unit, onPlayVideo: (Message) -> Unit, onBack: () -> Unit) {
    val id = card.getString("id")
    val members by ChatApplication.instance.members.members.collectAsState()
    val membersByEd = remember(members, channel.id) { members.filterKeys { it.group == channel.id }.values.associateBy { it.ed25519 } }
    val canWrite = board.optBoolean("can_write")
    val self = board.optString("self")
    val canAssignOthers = card.optString("author") == self || (!card.optBoolean("author_active") && board.optBoolean("can_manage"))
    val assignees = card.optJSONArray("assignees")?.let { values -> List(values.length()) { values.getString(it) } }.orEmpty()
    var pickingAssignees by remember(id) { mutableStateOf(false) }
    var assigning by remember(id) { mutableStateOf(false) }
    val target = ConversationTarget.Card(channel.id, id)
    var captureMode by rememberSaveable(channel.id, id) { mutableStateOf<CaptureMode?>(null) }
    var messages by remember(id) { mutableStateOf<List<JSONObject>>(emptyList()) }
    val navigation by viewModel.objectNavigation.collectAsState()
    var historyAround by remember(id) { mutableStateOf("") }
    var jumpMessage by remember(id) { mutableStateOf("") }
    var highlighted by remember(id) { mutableStateOf("") }
    var navigationRevision by remember(id) { mutableLongStateOf(0L) }
    var readRevision by remember(id) { mutableLongStateOf(0L) }
    val clipboard = LocalClipboardManager.current
    val onLink = messageLinkHandler(viewModel::openLink)
    var selectedMessage by remember(id) { mutableStateOf<Message?>(null) }
    var before by remember(id) { mutableStateOf("") }
    var loading by remember(id) { mutableStateOf(false) }
    var failed by remember(id) { mutableStateOf(false) }
    var reloadPending by remember(id) { mutableStateOf(false) }
    var refreshUntil by remember(id) { mutableStateOf("") }
    val refreshed = remember(id) { linkedMapOf<String, JSONObject>() }
    var active by remember(id) { mutableStateOf(true) }
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val playback by viewModel.playbackState.collectAsState()
    var base by remember(channel.id) { mutableStateOf<File?>(null) }
    var media by remember(id) { mutableStateOf<List<KanbanMessage>>(emptyList()) }
    val downloads = remember(id) { mutableStateMapOf<String, String>() }
    val paths = remember(id) { mutableStateMapOf<String, String>() }
    val deletedText = stringResource(R.string.kanban_message_deleted)
    val invalidMediaText = stringResource(R.string.kanban_attachment_unavailable)
    var pendingSave by rememberSaveable(channel.id, id) { mutableStateOf<String?>(null) }
    var saveError by remember(id) { mutableStateOf(false) }
    val export = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("*/*")) { uri ->
        val path = pendingSave; pendingSave = null
        if (uri != null && path != null) scope.launch {
            try { saveError = !withContext(Dispatchers.IO) { viewModel.saveDownloadedFile(path, uri) } }
            catch (e: CancellationException) { throw e }
            catch (e: Exception) { saveError = true; LogManager.addLog("ERROR", "Kanban", "File export failed group=${channel.id}: $e") }
        }
    }
    val listState = rememberLazyListState()
    val focus = LocalFocusManager.current
    val keyboard = LocalSoftwareKeyboardController.current
    val close = { focus.clearFocus(); keyboard?.hide(); onBack() }
    fun openCamera(mode: CaptureMode) {
        if (!canWrite || StreamController.blocksCapture()) {
            LogManager.addLog("WARN", "Kanban", "Camera unavailable group=${channel.id} card=$id can_write=$canWrite"); return
        }
        focus.clearFocus(); keyboard?.hide(); captureMode = mode
        LogManager.addLog("INFO", "Kanban", "Camera opened group=${channel.id} card=$id mode=$mode")
    }
    BackHandler(onBack = close)
    DisposableEffect(id) { active = true; onDispose { active = false } }
    LaunchedEffect(channel.id) { ChatApplication.instance.members.ensure(channel.id) }
    LaunchedEffect(channel.id) {
        base = withContext(Dispatchers.IO) { File(ChatApplication.instance.configProvider.getString("db.path") ?: context.filesDir.path) }
    }
    LaunchedEffect(messages, base, self, deletedText) {
        val directory = base ?: return@LaunchedEffect
        media = withContext(Dispatchers.IO) { messages.map { row ->
            try { parseKanbanMessage(row, directory, self, deletedText) }
            catch (e: Exception) {
                LogManager.addLog("ERROR", "Kanban", "Invalid message media group=${channel.id} message=${row.optString("id")}: $e")
                KanbanMessage(Message(id = 0, messageId = row.optString("id"), author = row.optString("author"),
                    text = invalidMediaText, ts = row.optLong("created_at") / 1000))
            }
        } }
        media.forEach { entry ->
            val message = entry.message
            // Ответ загрузки нужен до нового снимка; дальше доступность файла определяет каталог/проверка кеша.
            if (message.filePath.isNotEmpty() || entry.objectId.isEmpty()) {
                downloads.remove(message.messageId); paths.remove(message.messageId)
            }
            if (entry.objectId.isNotEmpty()) LogManager.addLog("DEBUG", "Kanban", "Media bound group=${channel.id} " +
                "message=${message.messageId} type=${message.contentType} object=${entry.objectId} bytes=${message.fileSize} " +
                "local=${message.filePath.isNotEmpty()} size=${message.videoWidth}x${message.videoHeight}")
        }
    }

    fun download(entry: KanbanMessage) {
        val message = entry.message
        if (downloads[message.messageId] == "downloading") return
        downloads[message.messageId] = "downloading"
        val request = onCommand(JSONObject().put("action", "download").put("entity", message.messageId)
            .put("object", entry.objectId)) result@{ result ->
            if (!active) return@result
            val path = result.optString("path")
            if (result.optBoolean("ok") && path.isNotEmpty()) {
                paths[message.messageId] = path; downloads.remove(message.messageId)
            } else { paths.remove(message.messageId); downloads[message.messageId] = "error" }
        }
        if (request == 0L) downloads[message.messageId] = "error"
    }

    // При обновлении перечитываем до самой старой открытой записи: правки и удаления видны на всех загруженных страницах.
    fun loadMessages(cursor: String = "") {
        if (loading) { if (cursor.isEmpty()) reloadPending = true; return }
        if (cursor.isEmpty()) {
            refreshUntil = messages.firstOrNull()?.optString("id").orEmpty()
            refreshed.clear()
        }
        loading = true
        val revision = ++readRevision
        val query = JSONObject().put("action", "messages").put("entity", id).put("before", cursor)
        if (cursor.isEmpty() && historyAround.isNotEmpty()) query.put("around", historyAround)
        val request = onCommand(query) result@{ result ->
            if (!active || revision != readRevision) return@result
            loading = false
            if (!result.optBoolean("ok")) {
                failed = true; reloadPending = false
                if (jumpMessage.isNotEmpty()) { jumpMessage = ""; viewModel.completeObjectLink(navigationRevision, false) }
                return@result
            }
            if (result.optString("entity") != id || result.optJSONArray("messages") == null) {
                LogManager.addLog("ERROR", "Kanban", "Invalid card messages group=${channel.id} card=$id entity=${result.optString("entity")}")
                failed = true; reloadPending = false
                if (jumpMessage.isNotEmpty()) { jumpMessage = ""; viewModel.completeObjectLink(navigationRevision, false) }
                return@result
            }
            failed = false
            if (jumpMessage.isNotEmpty() && !result.optBoolean("target_found")) {
                jumpMessage = ""; viewModel.completeObjectLink(navigationRevision, false); return@result
            }
            val page = result.getJSONArray("messages")
            val rows = List(minOf(page.length(), 50)) { page.getJSONObject(it) }
            val next = if (page.length() > 50) rows.last().getString("id") else ""
            rows.forEach { refreshed[it.getString("id")] = it }
            LogManager.addLog("DEBUG", "Kanban", "Card messages group=${channel.id} card=$id count=${rows.size} " +
                "older=${next.isNotEmpty()} refreshing=${refreshUntil.isNotEmpty()}")
            if (refreshUntil.isNotEmpty() && refreshUntil !in refreshed && next.isNotEmpty()) {
                loadMessages(next); return@result
            }
            before = next; refreshUntil = ""
            messages = refreshed.values.sortedWith(compareBy<JSONObject> { it.getLong("created_at") }
                .thenBy { it.getString("id") })
            if (reloadPending) { reloadPending = false; loadMessages() }
        }
        if (request == 0L) {
            loading = false; failed = true; reloadPending = false
            if (jumpMessage.isNotEmpty()) { jumpMessage = ""; viewModel.completeObjectLink(navigationRevision, false) }
        }
    }
    LaunchedEffect(board, id, navigation?.revision) {
        val pending = navigation
        if (pending != null && pending.target.group == channel.groupEd && pending.target.card == id &&
            pending.target.message.isNotEmpty() && pending.revision != navigationRevision) {
            navigationRevision = pending.revision; historyAround = pending.target.message; jumpMessage = historyAround
            ++readRevision; loading = false; reloadPending = false; messages = emptyList(); refreshed.clear()
        }
        if (board.length() > 0) loadMessages()
    }
    LaunchedEffect(media, jumpMessage) {
        if (jumpMessage.isEmpty()) return@LaunchedEffect
        val index = media.indexOfFirst { it.message.messageId == jumpMessage }
        if (index >= 0) {
            listState.scrollToItem(index + 2); highlighted = jumpMessage; jumpMessage = ""
            viewModel.completeObjectLink(navigationRevision, true)
        }
    }
    LaunchedEffect(highlighted) { if (highlighted.isNotEmpty()) { kotlinx.coroutines.delay(1500); highlighted = "" } }
    selectedMessage?.let { selected ->
        ModalBottomSheet(onDismissRequest = { selectedMessage = null }) {
            ListItem(headlineContent = { Text(stringResource(R.string.copy_link)) }, modifier = Modifier.clickable {
                ObjectLinks.build(channel.groupEd, channel.name, id, card.optString("text"), selected.messageId,
                    ObjectLinks.preview(selected))?.let { clipboard.setText(AnnotatedString(it)) }
                selectedMessage = null
            })
        }
    }

    Scaffold(modifier = Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).imePadding(), topBar = {
            TopAppBar(title = { Text(stringResource(R.string.kanban_card), maxLines = 1) }, navigationIcon = {
                IconButton(onClick = close) { Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back)) }
            }, actions = { TextButton(onClick = {
                ObjectLinks.build(channel.groupEd, channel.name, id, card.optString("text"))?.let { clipboard.setText(AnnotatedString(it)) }
            }) { Text(stringResource(R.string.copy_link)) } })
        }) { padding ->
        Column(Modifier.fillMaxSize().padding(padding)) {
            if (historyAround.isNotEmpty()) TextButton(onClick = {
                viewModel.cancelObjectNavigation(); ++readRevision; historyAround = ""; jumpMessage = ""; loading = false
                reloadPending = false; messages = emptyList(); refreshed.clear(); loadMessages()
                scope.launch { listState.scrollToItem(0) }
            }) { Text(stringResource(R.string.object_link_latest)) }
            val mediaFailed = downloads.values.any { it == "error" }
            if (boardFailed || failed || mediaFailed) Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(if (mediaFailed) R.string.download_failed_retry else R.string.kanban_failed),
                    color = MaterialTheme.colorScheme.error, modifier = Modifier.weight(1f))
                TextButton(enabled = !loading, onClick = { loadMessages() }) { Text(stringResource(R.string.kanban_retry)) }
            }
            if (saveError) Text(stringResource(R.string.file_save_failed), color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(8.dp))
            LazyColumn(state = listState, modifier = Modifier.weight(1f).fillMaxWidth(),
                contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                item(key = "details") {
                    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        KanbanCardDetails(channel.id, card, board, onCommand)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(stringResource(R.string.kanban_assignees), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f))
                            TextButton(enabled = canWrite && !assigning, onClick = { pickingAssignees = true }) { Text(stringResource(R.string.edit)) }
                        }
                        assignees.forEach { assigned ->
                            val name = membersByEd[assigned]?.name?.ifBlank { assigned } ?: assigned
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                UserAvatar(channel.id, assigned, name, size = 28.dp, enabled = false)
                                Text(name)
                            }
                        }
                        if (assignees.isEmpty()) Text(stringResource(R.string.kanban_no_assignees), color = MaterialTheme.colorScheme.onSurfaceVariant)
                        HorizontalDivider()
                        Text(stringResource(R.string.chat), style = MaterialTheme.typography.titleSmall)
                    }
                }
                item(key = "older") {
                    if (loading) Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) { CircularProgressIndicator(Modifier.size(24.dp)) }
                    else if (before.isNotEmpty()) TextButton(onClick = { loadMessages(before) }) { Text(stringResource(R.string.kanban_older_messages)) }
                }
                items(media, key = { it.message.messageId }) { entry ->
                    val author = entry.message.author
                    val name = membersByEd[author]?.name?.ifBlank { author } ?: author
                    val path = paths[entry.message.messageId] ?: entry.message.filePath
                    val message = entry.message.copy(author = name, filePath = path,
                        downloadState = if (path.isNotEmpty()) "downloaded" else downloads[entry.message.messageId].orEmpty())
                    Box(Modifier.fillMaxWidth().messageLongPress(stringResource(R.string.message_actions)) { selectedMessage = message }
                        .then(if (message.messageId == highlighted) Modifier.background(MaterialTheme.colorScheme.primary.copy(alpha = 0.18f)) else Modifier)) {
                    MessageBubble(message, playbackState = playback, onPlayVoice = viewModel::playVoiceMessage, onLink = onLink,
                        onSeekVoice = viewModel::seekVoice, onDownloadFile = { download(entry) },
                        onOpenImage = onViewPhoto, onPlayVideo = onPlayVideo,
                        header = { if (message.contentType == "audio/opus") Text(name, style = MaterialTheme.typography.labelMedium) },
                        onOpenFile = {
                            if (pendingSave == null) {
                                pendingSave = message.filePath; saveError = false
                                try { export.launch(message.text) }
                                catch (e: Exception) {
                                    pendingSave = null; saveError = true
                                    LogManager.addLog("ERROR", "Kanban", "Cannot launch file export group=${channel.id}: $e")
                                }
                            }
                        })
                    }
                }
            }
            ConversationComposer(target, viewModel, enabled = canWrite,
                onRecordVideo = { openCamera(CaptureMode.VIDEO) }, onCapturePhoto = { openCamera(CaptureMode.PHOTO) })
        }
    }
    captureMode?.let { mode ->
        Dialog(onDismissRequest = { captureMode = null },
            properties = DialogProperties(decorFitsSystemWindows = false)) {
            val window = (LocalView.current.parent as DialogWindowProvider).window
            DisposableEffect(window) {
                // Compose 1.6 с usePlatformDefaultWidth=false измеряет по дисплею, а не по доступной области окна.
                window.setLayout(WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT)
                LogManager.addLog("DEBUG", "Kanban", "Camera dialog uses available window bounds group=${channel.id} card=$id mode=$mode")
                onDispose { }
            }
            CameraRecordScreen(initialMode = mode, onSend = { path, captured ->
                LogManager.addLog("INFO", "Kanban", "Camera submission group=${target.id} card=${target.cardId} mode=$captured")
                if (captured == CaptureMode.PHOTO) viewModel.sendPhotoMessage(path, target, null)
                else viewModel.sendVideoMessage(path, target)
                captureMode = null
            }, onClose = { captureMode = null })
        }
    }
    if (pickingAssignees) AlertDialog(onDismissRequest = { pickingAssignees = false },
        title = { Text(stringResource(R.string.kanban_assignees)) }, text = {
            val candidates = membersByEd.values.filter { !it.isDeleted }.sortedBy { it.name }
            LazyColumn(Modifier.heightIn(max = 420.dp)) {
                items(candidates, key = { it.ed25519 }) { member ->
                    val target = member.ed25519
                    val assigned = target in assignees
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = assigned, enabled = canWrite && !assigning && (canAssignOthers || target == self), onCheckedChange = {
                            assigning = true
                            val request = onCommand(JSONObject().put("action", if (assigned) "unassign" else "assign")
                                .put("entity", id).put("target", target).put("grant", card.getString("grant"))) { assigning = false }
                            if (request == 0L) assigning = false
                        })
                        UserAvatar(channel.id, member.ed25519, member.name, size = 28.dp, enabled = false)
                        Text(member.name, modifier = Modifier.padding(start = 8.dp), maxLines = 2, overflow = TextOverflow.Ellipsis)
                    }
                }
            }
        }, confirmButton = { TextButton(onClick = { pickingAssignees = false }) { Text(stringResource(android.R.string.ok)) } })
}
