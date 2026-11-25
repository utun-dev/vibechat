package com.utun.chat.ui.components

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.ui.Alignment
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import com.utun.chat.data.ConversationTarget
import com.utun.chat.data.LogManager
import com.utun.chat.data.NativeLib
import com.utun.chat.viewmodel.ChatViewModel
import java.io.File
import kotlinx.coroutines.*

private data class SelectedAttachment(val target: ConversationTarget, val uri: Uri, val name: String, val size: Long, val video: Boolean,
                                      val id: String = java.util.UUID.randomUUID().toString())
private val selectedAttachmentSaver = listSaver<SelectedAttachment?, Any>(save = {
    if (it == null) emptyList() else listOf(it.target.nativeId, (it.target as? ConversationTarget.Pm)?.generation ?: 0L,
        it.uri.toString(), it.name, it.size, it.video, it.id, (it.target as? ConversationTarget.Card)?.cardId.orEmpty())
}, restore = {
    if (it.isEmpty()) null else {
        val id = it[0] as String
        val target = if (id.startsWith("dm:")) ConversationTarget.Pm(id.removePrefix("dm:"), it[1] as Long)
            else if ((it[7] as String).isNotEmpty()) ConversationTarget.Card(id, it[7] as String)
            else ConversationTarget.Channel(id)
        SelectedAttachment(target, Uri.parse(it[2] as String), it[3] as String, it[4] as Long, it[5] as Boolean, it[6] as String)
    }
})

/** Общий ввод и вложения: draft/выбор URI переживают пересоздание Activity, адресат и reply фиксируются до подготовки.
 * Экран владеет cache до передачи задачи ViewModel; отменённое копирование очищается в NonCancellable.
 * ViewModel продолжает пакет «вложение + текст» после ухода UI; token/revision очищают только подтверждённый прежний draft. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ConversationComposer(target: ConversationTarget, viewModel: ChatViewModel,
                         onRecordVideo: () -> Unit, onCapturePhoto: (() -> Unit)? = null, enabled: Boolean = true) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val activeTarget by rememberUpdatedState(target)
    var text by rememberSaveable(target, stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue()) }
    var textRevision by rememberSaveable(target) { mutableStateOf(0L) }
    val replies by viewModel.replies.collectAsState()
    val reply = if (target is ConversationTarget.Channel) replies[target.id] else null
    val editing by viewModel.editing.collectAsState()
    val edit = editing?.takeIf { target is ConversationTarget.Channel && it.channelId == target.id }
    var savedDraft by rememberSaveable(target, stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue()) }
    var activeEditRevision by rememberSaveable(target) { mutableStateOf(0L) }
    LaunchedEffect(edit) {
        if (edit != null && activeEditRevision != edit.revision) {
            if (activeEditRevision == 0L) savedDraft = text
            text = TextFieldValue(edit.message.text); textRevision++
        } else if (edit == null && activeEditRevision != 0L) { text = savedDraft; textRevision++ }
        activeEditRevision = edit?.revision ?: 0L
    }
    val sendFailed by viewModel.messageSendFailed.collectAsState()
    val sendTooLarge by viewModel.messageSendTooLarge.collectAsState()
    val submitting by viewModel.submittingChannels.collectAsState()
    val batchStates by viewModel.composerSubmissions.states.collectAsState()
    val batch = batchStates[target]
    val deletingDm by viewModel.dmDeleting.collectAsState()
    val pending = batch?.finished == false || target.nativeId in submitting || (target is ConversationTarget.Pm && target.id in deletingDm)
    if (sendFailed) AlertDialog(onDismissRequest = viewModel::dismissMessageSendError,
        text = { Text(stringResource(if (sendTooLarge) R.string.quote_too_large else R.string.message_send_failed)) },
        confirmButton = { TextButton(onClick = viewModel::dismissMessageSendError) { Text(stringResource(R.string.close)) } })
    var selected by rememberSaveable(target, stateSaver = selectedAttachmentSaver) { mutableStateOf<SelectedAttachment?>(null) }
    var pendingToken by rememberSaveable(target) { mutableStateOf<String?>(null) }
    var pendingTextRevision by rememberSaveable(target) { mutableStateOf(0L) }
    var pendingAttachmentId by rememberSaveable(target) { mutableStateOf<String?>(null) }
    LaunchedEffect(batch, pendingToken) {
        if (batch != null && batch.token == pendingToken) {
            if (batch.attachmentSaved == true && selected?.id == pendingAttachmentId) selected = null
            if (batch.textSaved == true && textRevision == pendingTextRevision) { text = TextFieldValue(); textRevision++ }
            if (batch.finished) {
                viewModel.composerSubmissions.consume(target, batch.token)
                pendingToken = null; pendingAttachmentId = null
            }
        }
    }
    var pickerId by rememberSaveable { mutableStateOf<String?>(null) }
    var pickerGeneration by rememberSaveable { mutableStateOf(0L) }
    var pickerCard by rememberSaveable { mutableStateOf("") }
    var preparing by remember(target) { mutableStateOf(false) }
    var error by remember(target) { mutableStateOf<Int?>(null) }
    var menu by remember(target) { mutableStateOf(false) }
    var chooseVideo by rememberSaveable(target) { mutableStateOf(false) }
    var sendAsVideo by rememberSaveable(target) { mutableStateOf(false) }
    LaunchedEffect(target) {
        if (selected?.target != target) { selected = null; chooseVideo = false; sendAsVideo = false }
    }
    val recording by viewModel.isRecording.collectAsState()
    val locked by viewModel.isRecordingLocked.collectAsState()
    val duration by viewModel.recordingDurationMs.collectAsState()
    val videoMode by viewModel.isVideoMode.collectAsState()
    var level by remember { mutableStateOf(0f) }
    val preparations by viewModel.attachmentPreparations.collectAsState()
    val preparation = preparations[target.nativeId]
    DisposableEffect(target) { onDispose { viewModel.cancelVoiceRecording(target) } }
    LaunchedEffect(recording) {
        try { while (recording) { level = NativeLib.voiceGetPeakLevel(); delay(80) } }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { LogManager.addLog("ERROR", "Voice", "Cannot read recording level: $e") }
        level = 0f
    }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val pinned = pickerId?.let {
            if (it.startsWith("dm:")) ConversationTarget.Pm(it.removePrefix("dm:"), pickerGeneration)
            else if (pickerCard.isNotEmpty()) ConversationTarget.Card(it, pickerCard) else ConversationTarget.Channel(it)
        }
        pickerId = null
        if (uri != null && pinned != null) scope.launch {
            try {
                val attachment = withContext(Dispatchers.IO) {
                    var name = "attachment.bin"; var size = 0L
                    context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use {
                        if (it.moveToFirst()) { name = it.getString(0) ?: name; size = if (it.isNull(1)) 0 else it.getLong(1).coerceAtLeast(0) }
                    }
                    SelectedAttachment(pinned, uri, name, size, context.contentResolver.getType(uri)?.startsWith("video/") == true)
                }
                if (pinned == activeTarget) { selected = attachment; sendAsVideo = false; chooseVideo = attachment.video }
                else LogManager.addLog("INFO", "Attachment", "picker discarded after target change")
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                LogManager.addLog("ERROR", "Attachment", "picker failed: ${e.message}")
                if (pinned == activeTarget) error = R.string.attachment_open_failed
            }
        }
    }
    Column {
        if (edit != null) Surface(color = MaterialTheme.colorScheme.surfaceVariant) {
            Row(Modifier.fillMaxWidth().padding(start = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.editing_message), Modifier.weight(1f))
                IconButton(onClick = { viewModel.cancelEdit(edit) }, enabled = !pending) {
                    Icon(Icons.Default.Close, stringResource(R.string.edit_cancel))
                }
            }
        }
        if (edit == null && reply != null) Surface(color = MaterialTheme.colorScheme.surfaceVariant) {
            Row(Modifier.fillMaxWidth().padding(start = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                ReplyQuote(reply.message, Modifier.weight(1f).heightIn(max = 120.dp).verticalScroll(rememberScrollState()), reply.quote)
                IconButton(onClick = { viewModel.cancelReply(reply.channelId) }) {
                    Icon(Icons.Default.Close, stringResource(R.string.reply_cancel))
                }
            }
        }
        error?.let { Text(stringResource(it), color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(8.dp)) }
        if (preparing || preparation?.jobs?.isNotEmpty() == true) Text(stringResource(R.string.media_preparing), modifier = Modifier.padding(8.dp))
        else if (preparation?.failed == true) Text(stringResource(R.string.attachment_prepare_failed), color = MaterialTheme.colorScheme.error,
            modifier = Modifier.padding(8.dp))
        key(target) { InputBar(
            text = text, onTextChange = { text = it; textRevision++ }, enabled = enabled && !preparing && !pending,
            focusRevision = edit?.revision ?: reply?.revision ?: 0, editing = edit != null,
            onSend = {
                if (!enabled || preparing || pending) return@InputBar
                if (edit != null) {
                    val sent = text.text
                    if (sent.isBlank()) return@InputBar
                    preparing = true
                    scope.launch {
                        try {
                            error = null
                            if (!viewModel.saveEdit(edit, sent).await()) error = R.string.edit_failed
                        } catch (e: CancellationException) { throw e }
                        catch (e: Exception) {
                            LogManager.addLog("ERROR", "Edit", "Cannot submit edit: $e")
                            error = R.string.edit_failed
                        } finally { preparing = false }
                    }
                    return@InputBar
                }
                val attachment = selected?.takeIf { it.target == target }
                val video = sendAsVideo
                val sentText = text.text
                val revision = textRevision
                val capturedReply = reply
                if (attachment == null && sentText.isBlank()) return@InputBar
                preparing = true; error = null
                scope.launch {
                    var cache: String? = null
                    try {
                        if (attachment != null) {
                            withContext(Dispatchers.IO) { cache = viewModel.copyUriToCache(context, attachment.uri, attachment.name) }
                            checkNotNull(cache) { "Attachment copy failed" }
                        }
                        val token = java.util.UUID.randomUUID().toString()
                        pendingToken = token; pendingTextRevision = revision; pendingAttachmentId = attachment?.id
                        if (viewModel.sendConversation(target, token, sentText, cache, attachment?.name.orEmpty(), video, capturedReply)) {
                            cache = null // ViewModel приняла весь пакет и владеет cache.
                        } else {
                            pendingToken = null; pendingAttachmentId = null
                            error = R.string.attachment_send_failed
                        }
                    } catch (e: CancellationException) { throw e }
                    catch (e: Exception) {
                        LogManager.addLog("ERROR", "Attachment", "send failed: ${e.message}")
                        error = R.string.attachment_send_failed
                    }
                    finally {
                        val abandoned = cache
                        if (abandoned != null) withContext(NonCancellable + Dispatchers.IO) {
                            try {
                                val file = File(abandoned)
                                if (!file.delete() && file.exists()) LogManager.addLog("ERROR", "Attachment", "Abandoned cache removal failed path=$abandoned")
                            } catch (e: Exception) { LogManager.addLog("ERROR", "Attachment", "Abandoned cache removal threw path=$abandoned: $e") }
                        }
                        preparing = false
                    }
                }
            },
            isRecording = recording, isRecordingLocked = locked, recordingDurationMs = duration, recordingLevel = level,
            onPttPressed = { if (edit == null) viewModel.startVoiceRecording(target) },
            onPttReleased = { if (!viewModel.isRecordingLocked.value) viewModel.sendVoiceMessage() },
            onPttLock = { viewModel.lockVoiceRecording() }, onVoiceSend = { viewModel.sendVoiceMessage() },
            onVoiceCancel = { viewModel.cancelVoiceRecording(target) }, onAttachClicked = { if (edit == null) menu = true },
            pendingAttachmentName = if (edit == null) selected?.name ?: "" else "", pendingAttachmentSize = selected?.size ?: 0,
            onClearAttachment = { selected = null }, isVideoMode = target is ConversationTarget.Card || videoMode,
            onToggleMode = { if (target is ConversationTarget.Card) onRecordVideo() else viewModel.toggleVideoMode() },
            onVideoRecordOpen = onRecordVideo,
            modifier = Modifier.navigationBarsPadding()
        ) }
    }
    if (menu) ModalBottomSheet(onDismissRequest = { menu = false }) {
        ListItem(headlineContent = { Text(stringResource(R.string.select_file)) }, modifier = Modifier.clickable {
            menu = false; pickerId = target.nativeId; pickerGeneration = (target as? ConversationTarget.Pm)?.generation ?: 0
            pickerCard = (target as? ConversationTarget.Card)?.cardId.orEmpty()
            try { picker.launch(arrayOf("*/*")) } catch (e: Exception) {
                pickerId = null; error = R.string.attachment_open_failed
                LogManager.addLog("ERROR", "Attachment", "Cannot launch attachment picker: $e")
            }
        })
        ListItem(headlineContent = { Text(stringResource(R.string.record_video)) }, modifier = Modifier.clickable { menu = false; onRecordVideo() })
        if (onCapturePhoto != null) ListItem(headlineContent = { Text(stringResource(R.string.take_photo)) },
            modifier = Modifier.clickable { menu = false; onCapturePhoto() })
        Spacer(Modifier.height(16.dp))
    }
    if (chooseVideo) AlertDialog(
        onDismissRequest = { chooseVideo = false }, title = { Text(stringResource(R.string.send_video)) },
        text = { Text(stringResource(R.string.attachment_display_choice)) },
        confirmButton = { TextButton(onClick = { sendAsVideo = true; chooseVideo = false }) { Text(stringResource(R.string.as_video)) } },
        dismissButton = { TextButton(onClick = { sendAsVideo = false; chooseVideo = false }) { Text(stringResource(R.string.as_file)) } }
    )
}
