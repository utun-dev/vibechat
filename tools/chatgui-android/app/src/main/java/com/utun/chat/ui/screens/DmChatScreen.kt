package com.utun.chat.ui.screens

import com.utun.chat.R
import com.utun.chat.ui.components.UserAvatar
import androidx.compose.ui.res.stringResource
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Group
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import com.utun.chat.ui.components.PeerStatusDot
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.utun.chat.data.*
import com.utun.chat.ui.components.ConversationComposer
import com.utun.chat.ui.components.ConversationMessages
import com.utun.chat.viewmodel.ChatViewModel
import java.io.File
import android.content.Intent
import android.content.ClipData
import android.webkit.MimeTypeMap
import androidx.core.content.FileProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** PM отличается заголовком и доставкой; ввод, вложения и сообщения общие с каналом.
 * Target включает generation беседы. SAF экспортирует снимок локального пути, не изменяя исходный файл/сообщение. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DmChatScreen(conv: DmConversation, messages: List<Message>, viewModel: ChatViewModel,
                 onBack: () -> Unit,
                 onRecordVideo: () -> Unit, onPlayVideo: (Message) -> Unit,
                 onCapturePhoto: () -> Unit, onViewPhoto: (Message) -> Unit) {
    val target = ConversationTarget.Pm(conv.convId, conv.generation)
    var showDevices by rememberSaveable(conv.convId, conv.generation) { mutableStateOf(false) }
    if (showDevices && conv.mode == 1) DmMembersSheet(conv) { showDevices = false }
    val playback by viewModel.playbackState.collectAsState()
    val callAvailable by viewModel.dmCallAvailable.collectAsState()
    val callState by viewModel.callState.collectAsState()
    val state = rememberLazyListState()
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var pathToSave by rememberSaveable { mutableStateOf<String?>(null) }
    var fileChoice by remember(target) { mutableStateOf<Message?>(null) }
    var error by remember(conv.convId, conv.generation) { mutableStateOf<Int?>(null) }
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("*/*")) { uri ->
        val path = pathToSave
        pathToSave = null
        if (uri != null && path != null) scope.launch {
            try {
                withContext(Dispatchers.IO) {
                    context.contentResolver.openOutputStream(uri)?.use { output ->
                        File(path).inputStream().use { it.copyTo(output) }
                    } ?: throw java.io.IOException("Cannot open selected document")
                }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "PM", "save failed: ${e.message}")
                error = R.string.file_save_failed
            }
        }
    }
    LaunchedEffect(messages.size) {
        if (messages.lastOrNull()?.isOutgoing == true) state.scrollToItem(0)
    }
    fileChoice?.let { message ->
        AlertDialog(onDismissRequest = { fileChoice = null }, title = { Text(message.text) },
            confirmButton = { TextButton(onClick = {
                fileChoice = null
                viewModel.prepareDmAttachment(target, message)
                scope.launch {
                    try {
                        val file = withContext(Dispatchers.IO) {
                            val dir = File(context.cacheDir, "member_files/dm/${File(message.filePath).nameWithoutExtension}")
                            check(dir.isDirectory || dir.mkdirs()) { "Cannot create export directory" }
                            val name = File(message.text).name.takeUnless { it.isBlank() || it == "." || it == ".." } ?: "attachment.bin"
                            File(message.filePath).copyTo(File(dir, name), overwrite = true)
                        }
                        val uri = FileProvider.getUriForFile(context, "${context.packageName}.memberfiles", file)
                        val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(file.extension.lowercase()) ?: "application/octet-stream"
                        context.startActivity(Intent(Intent.ACTION_VIEW).apply {
                            setDataAndType(uri, mime); clipData = ClipData.newRawUri(file.name, uri)
                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        })
                        viewModel.attachmentOpened(message.filePath)
                    } catch (e: CancellationException) { throw e }
                    catch (e: Exception) {
                        LogManager.addLog("ERROR", "PM", "Cannot open attachment: $e"); error = R.string.file_open_failed
                    }
                }
            }) { Text(stringResource(R.string.files_open)) } },
            dismissButton = { TextButton(onClick = {
                fileChoice = null; pathToSave = message.filePath; error = null
                try { save.launch(message.text) } catch (e: Exception) {
                    pathToSave = null; error = R.string.file_save_failed
                    LogManager.addLog("ERROR", "PM", "Cannot launch document export: $e")
                }
            }) { Text(stringResource(R.string.save)) } })
    }
    Column(Modifier.fillMaxSize().imePadding()) {
        TopAppBar(title = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                UserAvatar(null, conv.peerEd25519, conv.displayName, size = 32.dp)
                Spacer(Modifier.width(8.dp))
                PeerStatusDot(callAvailable)
                Spacer(Modifier.width(8.dp))
                Text(conv.displayName)
            }
        }, navigationIcon = {
            IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back)) }
        }, actions = {
            if (conv.mode == 1) IconButton(onClick = { showDevices = true }) {
                Icon(Icons.Filled.Group, stringResource(R.string.dm_members_title))
            }
            IconButton(onClick = { viewModel.showPmInvite(conv.peerEd25519, conv.displayName, target.nativeId) }, enabled = !conv.closed && conv.peerEd25519.isNotEmpty()) {
                Icon(Icons.Filled.Add, stringResource(R.string.invite_to_group))
            }
            IconButton(onClick = { viewModel.startDmCall() }, enabled = callAvailable && callState?.ongoing != true) {
                Icon(Icons.Filled.Call, stringResource(if (callAvailable) R.string.call else R.string.peer_unavailable))
            }
        })
        ConversationMessages(target, messages, state, playback, Modifier.weight(1f).fillMaxWidth(),
            onPlayVoice = { path, duration ->
                messages.find { it.filePath == path }?.let { viewModel.prepareDmAttachment(target, it) }
                viewModel.playVoiceMessage(path, duration)
            },
            onSeekVoice = { path, position, duration ->
                messages.find { it.filePath == path }?.let { viewModel.prepareDmAttachment(target, it) }
                viewModel.seekVoice(path, position, duration)
            },
            onOpenFile = {
                if (it.filePath.isNotEmpty() && pathToSave == null) fileChoice = it
            },
            onDownloadFile = { viewModel.downloadDmAttachment(target, it) },
            onPlayVideo = { viewModel.prepareDmAttachment(target, it); onPlayVideo(it) },
            onOpenImage = { viewModel.prepareDmAttachment(target, it); onViewPhoto(it) }, onInviteLink = viewModel::openLink)
        error?.let { Text(stringResource(it), color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(8.dp)) }
        if (conv.closed) Text(stringResource(R.string.dm_archived), modifier = Modifier.padding(12.dp))
        else ConversationComposer(target, viewModel, onRecordVideo, onCapturePhoto)
    }
}
