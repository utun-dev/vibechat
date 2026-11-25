package com.utun.chat.ui.screens

import android.content.ClipData
import android.content.Intent
import android.net.Uri
import android.text.format.Formatter
import android.webkit.MimeTypeMap
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.InsertDriveFile
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LifecycleStartEffect
import com.utun.chat.R
import com.utun.chat.data.*
import com.utun.chat.viewmodel.MemberFilesViewModel
import java.text.DateFormat
import java.util.Date

/**
 * Браузер файлов CHAT-группы; caller предварительно задаёт адрес через viewModel.open/openLocation.
 * STARTED управляет LIST/QUOTA, уход с экрана сохраняет app-scoped передачу. Общая очередь и её конфликт
 * показываются независимо от текущего каталога; решение замены действует только для совпадающего token.
 * Запись доступна собственному UID вне служебного avatars; чтение/экспорт — для доступных файлов группы.
 *
 * Готовый export автоматически выдаётся лишь при RESUMED и совпадении родительского location.
 * OPEN/SHARE используют FileProvider с read grant, SAVE — CreateDocument и IO-копирование в репозитории.
 * Перед внешним действием consumeExport снимает ожидание UI; копия сохраняется для повторного SAVE,
 * ошибка Android Intent показывается отдельно и не вызывает повторное скачивание.
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun MemberFilesScreen(channel: Channel, viewModel: MemberFilesViewModel, onBack: () -> Unit) {
    val state by viewModel.state.collectAsState()
    val operation by viewModel.operation.collectAsState()
    val conflict by viewModel.conflict.collectAsState()
    val export by viewModel.export.collectAsState()
    val context = LocalContext.current
    val lifecycle by LocalLifecycleOwner.current.lifecycle.currentStateFlow.collectAsState()
    val location = state.location
    val own = location?.owner != "" && location?.owner == NativeLib.getMySigningAddress()
    val service = location?.let { MemberFilesProtocol.servicePath(it.path) } == true
    val server = state.servers.find { it.ed25519 == location?.server }
    val available = server != null && NativeLib.isReady()
    val busy = operation?.active == true || state.mutating
    var serverMenu by remember { mutableStateOf(false) }
    var addMenu by remember { mutableStateOf(false) }
    var selected by remember { mutableStateOf<MemberFilesProtocol.Entry?>(null) }
    var editing by remember { mutableStateOf<MemberFilesProtocol.Entry?>(null) }
    var newFolder by rememberSaveable { mutableStateOf(false) }
    var deleteEntry by remember { mutableStateOf<MemberFilesProtocol.Entry?>(null) }
    var properties by remember { mutableStateOf<MemberFilesProtocol.Entry?>(null) }
    var editName by rememberSaveable { mutableStateOf("") }
    var externalFailed by rememberSaveable { mutableStateOf(false) }
    var ready by remember { mutableStateOf(viewModel.readyExport) }
    val list = key(location) { rememberLazyListState() }
    fun size(bytes: Long) = Formatter.formatShortFileSize(context, bytes)
    fun date(seconds: Long): String = if (seconds == 0L || seconds > Long.MAX_VALUE / 1000) "—" else
        DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT, context.resources.configuration.locales[0])
            .format(Date(seconds * 1000))
    fun name(owner: String): String = state.members.find { it.ed25519 == owner }?.name?.takeIf { it.isNotEmpty() }
        ?: owner
    fun back() { if (!viewModel.up()) onBack() }

    LifecycleStartEffect(Unit) { viewModel.setVisible(true); onStopOrDispose { viewModel.setVisible(false) } }
    LaunchedEffect(location) { selected = null; editing = null; newFolder = false; deleteEntry = null; properties = null }
    BackHandler { when {
        selected != null -> selected = null
        serverMenu -> serverMenu = false
        addMenu -> addMenu = false
        newFolder || editing != null -> { newFolder = false; editing = null }
        deleteEntry != null -> deleteEntry = null
        properties != null -> properties = null
        else -> back()
    } }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        val target = viewModel.pickerLocation; viewModel.pickerLocation = null
        if (target != null && uris.isNotEmpty()) viewModel.repository.upload(target, uris)
    }
    val saver = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("*/*")) { uri ->
        val prepared = viewModel.pendingSave; viewModel.pendingSave = null
        if (prepared != null && uri != null) viewModel.repository.save(prepared, uri)
    }
    fun deliver(prepared: FileExport, action: Int = prepared.action) {
        viewModel.readyExport = prepared; ready = prepared
        viewModel.repository.consumeExport(prepared.token)
        try {
            if (action == MemberFilesRepository.SAVE) { viewModel.pendingSave = prepared; saver.launch(prepared.name) }
            else {
                val uri = FileProvider.getUriForFile(context, "${context.packageName}.memberfiles", prepared.file)
                val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(prepared.name.substringAfterLast('.', "").lowercase())
                    ?: "application/octet-stream"
                val intent = Intent(if (action == MemberFilesRepository.SHARE) Intent.ACTION_SEND else Intent.ACTION_VIEW).apply {
                    if (action == MemberFilesRepository.SHARE) { type = mime; putExtra(Intent.EXTRA_STREAM, uri) }
                    else setDataAndType(uri, mime)
                    clipData = ClipData.newRawUri(prepared.name, uri)
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                context.startActivity(Intent.createChooser(intent, null))
            }
            LogManager.addLog("INFO", "Files", "export dispatched token=${prepared.token} action=$action file=${prepared.file}")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "Files", "export failed token=${prepared.token} action=$action: $e")
            externalFailed = true
        }
    }
    LaunchedEffect(export, lifecycle, location) {
        val prepared = export
        if (prepared != null && lifecycle.isAtLeast(Lifecycle.State.RESUMED) && prepared.location == location) deliver(prepared)
    }

    Column(Modifier.fillMaxSize().navigationBarsPadding()) {
        TopAppBar(title = { Column {
            Text(stringResource(R.string.files_title))
            Text(channel.name, style = MaterialTheme.typography.bodySmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
        } }, navigationIcon = {
            IconButton(onClick = ::back) { Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back)) }
        }, actions = {
            IconButton(onClick = viewModel::refresh, enabled = available && !state.loading) {
                Icon(Icons.Default.Refresh, stringResource(R.string.files_refresh))
            }
        })
        TextButton(onClick = { serverMenu = true }, modifier = Modifier.fillMaxWidth(), enabled = state.servers.isNotEmpty()) {
            Icon(Icons.Default.Storage, null); Spacer(Modifier.width(8.dp))
            Text(stringResource(R.string.files_server, server?.name?.takeIf { it.isNotEmpty() } ?: stringResource(R.string.files_choose_server)),
                maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
            Icon(Icons.Default.ArrowDropDown, null)
        }
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = { viewModel.selectOwner("") }) { Text(stringResource(R.string.files_members)) }
            if (location != null && location.owner != "") {
                Text("/")
                TextButton(onClick = { viewModel.breadcrumb("") }) { Text(name(location.owner)) }
                var path = ""
                location.path.split('/').filter { it.isNotEmpty() }.forEach { component ->
                    path = if (path.isEmpty()) component else "$path/$component"
                    val target = path
                    Text("/"); TextButton(onClick = { viewModel.breadcrumb(target) }) { Text(component) }
                }
            }
            if (!own) TextButton(onClick = { viewModel.selectOwner(NativeLib.getMySigningAddress()) }, enabled = NativeLib.isReady()) {
                Text(stringResource(R.string.files_mine))
            }
        }
        if (state.loading || state.mutating) LinearProgressIndicator(Modifier.fillMaxWidth())
        state.status?.let { status -> Row(Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(fileStatusResource(status)), color = MaterialTheme.colorScheme.error, modifier = Modifier.weight(1f))
            TextButton(onClick = viewModel::refresh) { Text(stringResource(R.string.retry)) }
        } }
        Box(Modifier.weight(1f).fillMaxWidth()) {
            when {
                state.servers.isEmpty() -> Text(stringResource(R.string.files_no_server), Modifier.align(Alignment.Center).padding(24.dp))
                server == null -> Text(stringResource(R.string.files_server_missing), Modifier.align(Alignment.Center).padding(24.dp))
                state.entries.isEmpty() -> Text(stringResource(if (state.loading) R.string.files_loading else R.string.files_empty),
                    Modifier.align(Alignment.Center).padding(24.dp))
                else -> LazyColumn(state = list, contentPadding = PaddingValues(bottom = if (own && !service) 88.dp else 12.dp)) {
                    items(state.entries, key = { it.name }) { entry ->
                        val directory = entry.type == MemberFilesProtocol.DIRECTORY
                        val root = location?.owner == ""
                        val label = if (root) name(entry.name) ?: entry.name else entry.name
                        ListItem(headlineContent = { Text(label, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                            supportingContent = {
                                Text(if (directory) stringResource(if (!root && MemberFilesProtocol.servicePath(entry.name) && location?.path == "")
                                    R.string.files_service_directory else R.string.files_folder_type) else "${size(entry.size)} · ${date(entry.mtime)}")
                            }, leadingContent = { Icon(if (directory) Icons.Default.Folder else Icons.AutoMirrored.Filled.InsertDriveFile, null) },
                            trailingContent = {
                                if (!root) IconButton(onClick = { selected = entry }) { Icon(Icons.Default.MoreVert, stringResource(R.string.options)) }
                            }, modifier = Modifier.combinedClickable(onClick = {
                                if (directory) viewModel.enter(entry)
                                else if (!busy && available && location != null) viewModel.repository.download(location, entry, MemberFilesRepository.OPEN)
                            }, onLongClick = { if (!root) selected = entry }))
                    }
                }
            }
            if (own && !service && available) FloatingActionButton(onClick = { addMenu = true },
                modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp), containerColor = MaterialTheme.colorScheme.primaryContainer) {
                Icon(Icons.Default.Add, stringResource(R.string.files_add))
            }
        }
        if (service || location?.owner != "" && !own) Text(stringResource(if (service) R.string.files_service_directory else R.string.files_read_only),
            style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
        state.quota?.let { q ->
            Text(stringResource(R.string.files_quota, size(q.used), size(q.limit)), style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
            if (q.reserved > 0) Text(stringResource(R.string.files_quota_reserved, size(q.reserved)),
                style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(horizontal = 16.dp))
        }
        operation?.let { op -> Card(Modifier.fillMaxWidth().padding(8.dp)) {
            Column(Modifier.padding(12.dp)) {
                Text(op.name.ifEmpty { stringResource(R.string.files_title) }, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(stringResource(op.stage) + if (op.count > 1) " · " + stringResource(R.string.files_batch, op.index, op.count) else "")
                if (op.upload) Text(stringResource(R.string.files_summary, op.uploaded, op.skipped))
                op.status?.let { Text(stringResource(fileStatusResource(it)), color = MaterialTheme.colorScheme.error) }
                if (op.active) {
                    if (op.total > 0) {
                        LinearProgressIndicator(progress = { (op.done.toDouble() / op.total).toFloat() }, modifier = Modifier.fillMaxWidth())
                        Text(stringResource(R.string.files_bytes, size(op.done), size(op.total)))
                    } else LinearProgressIndicator(Modifier.fillMaxWidth())
                    TextButton(onClick = viewModel.repository::cancel) { Text(stringResource(R.string.cancel)) }
                } else Row {
                    if (op.retryable && (op.status != null || op.cancelled) && op.status != MemberFilesProtocol.STALE)
                        TextButton(onClick = viewModel.repository::retry) { Text(stringResource(R.string.retry)) }
                    TextButton(onClick = viewModel.repository::dismiss) { Text(stringResource(R.string.close)) }
                }
            }
        } }
        val prepared = export ?: ready
        if (prepared != null && !busy) Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.files_ready, prepared.name), modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
            TextButton(onClick = { deliver(prepared, MemberFilesRepository.SAVE) }) { Text(stringResource(R.string.save)) }
        }
    }

    if (serverMenu) ModalBottomSheet(onDismissRequest = { serverMenu = false }) {
        Text(stringResource(R.string.files_choose_server), style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(16.dp))
        LazyColumn { items(state.servers, key = { it.ed25519 }) { storage ->
            ListItem(headlineContent = { Text(storage.name.ifEmpty { storage.ed25519 }) },
                leadingContent = { Icon(Icons.Default.Storage, null) }, trailingContent = {
                    if (storage.ed25519 == location?.server) Icon(Icons.Default.Check, null)
                }, modifier = Modifier.combinedClickable(onClick = { serverMenu = false; viewModel.selectServer(storage.ed25519) }))
        } }
    }
    if (addMenu) ModalBottomSheet(onDismissRequest = { addMenu = false }) {
        TextButton(onClick = { addMenu = false; viewModel.pickerLocation = location; picker.launch(arrayOf("*/*")) },
            enabled = !busy, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.files_upload)) }
        TextButton(onClick = { addMenu = false; editName = ""; newFolder = true }, enabled = !busy,
            modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.files_folder)) }
    }
    selected?.let { entry -> ModalBottomSheet(onDismissRequest = { selected = null }) {
        Text(entry.name, style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(16.dp))
        if (entry.type == MemberFilesProtocol.FILE) listOf(MemberFilesRepository.OPEN to R.string.files_open,
            MemberFilesRepository.SAVE to R.string.files_save, MemberFilesRepository.SHARE to R.string.files_share).forEach { (action, label) ->
            TextButton(onClick = { selected = null; location?.let { viewModel.repository.download(it, entry, action) } },
                enabled = !busy && available, modifier = Modifier.fillMaxWidth()) { Text(stringResource(label)) }
        }
        TextButton(onClick = { selected = null; properties = entry }, modifier = Modifier.fillMaxWidth()) {
            Text(stringResource(R.string.files_properties))
        }
        val editable = own && !service && (location?.path?.isNotEmpty() == true || !MemberFilesProtocol.servicePath(entry.name))
        if (editable) {
            TextButton(onClick = { selected = null; editing = entry; editName = entry.name }, enabled = !busy && available,
                modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.files_rename)) }
            TextButton(onClick = { selected = null; deleteEntry = entry }, enabled = !busy && available,
                modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.delete), color = MaterialTheme.colorScheme.error) }
        }
    } }
    if (newFolder || editing != null) AlertDialog(onDismissRequest = { newFolder = false; editing = null },
        title = { Text(stringResource(if (newFolder) R.string.files_folder else R.string.files_rename)) }, text = {
            val valid = location != null && runCatching { MemberFilesProtocol.child(location.path, editName) }.isSuccess &&
                !MemberFilesProtocol.servicePath(if (location.path.isEmpty()) editName else "${location.path}/$editName")
            OutlinedTextField(value = editName, onValueChange = { editName = it }, label = { Text(stringResource(R.string.files_name)) },
                singleLine = true, isError = editName.isNotEmpty() && !valid)
        }, confirmButton = {
            val valid = location != null && runCatching { MemberFilesProtocol.child(location.path, editName) }.isSuccess &&
                !MemberFilesProtocol.servicePath(if (location.path.isEmpty()) editName else "${location.path}/$editName")
            TextButton(onClick = {
                viewModel.mutate(if (newFolder) MemberFilesProtocol.MKDIR else MemberFilesProtocol.RENAME, editName, editing)
                newFolder = false; editing = null
            }, enabled = valid && !busy) { Text(stringResource(R.string.save)) }
        }, dismissButton = { TextButton(onClick = { newFolder = false; editing = null }) { Text(stringResource(R.string.cancel)) } })
    deleteEntry?.let { entry -> AlertDialog(onDismissRequest = { deleteEntry = null },
        text = { Text(stringResource(R.string.files_delete_confirm, entry.name)) }, confirmButton = {
            TextButton(onClick = { deleteEntry = null; viewModel.mutate(MemberFilesProtocol.REMOVE, entry = entry) }) {
                Text(stringResource(R.string.delete))
            }
        }, dismissButton = { TextButton(onClick = { deleteEntry = null }) { Text(stringResource(R.string.cancel)) } }) }
    conflict?.let { item -> AlertDialog(onDismissRequest = { viewModel.repository.decideReplace(item.token, null) },
        title = { Text(stringResource(R.string.files_replace_title)) }, text = { Text(stringResource(R.string.files_replace_confirm, item.name)) },
        confirmButton = { TextButton(onClick = { viewModel.repository.decideReplace(item.token, true) }) { Text(stringResource(R.string.files_replace)) } },
        dismissButton = { Row {
            TextButton(onClick = { viewModel.repository.decideReplace(item.token, false) }) { Text(stringResource(R.string.files_skip)) }
            TextButton(onClick = { viewModel.repository.decideReplace(item.token, null) }) { Text(stringResource(R.string.cancel)) }
        } }) }
    properties?.let { entry -> AlertDialog(onDismissRequest = { properties = null },
        title = { Text(stringResource(R.string.files_properties)) }, text = {
            Text(stringResource(R.string.files_property_values, entry.name,
                if (entry.type == MemberFilesProtocol.FILE) size(entry.size) else stringResource(R.string.files_folder_type),
                date(entry.mtime), location?.owner?.let { name(it) } ?: ""))
        }, confirmButton = { TextButton(onClick = { properties = null }) { Text(stringResource(R.string.close)) } }) }
    if (externalFailed) AlertDialog(onDismissRequest = { externalFailed = false },
        text = { Text(stringResource(R.string.files_external_failed)) }, confirmButton = {
            TextButton(onClick = { externalFailed = false }) { Text(stringResource(R.string.close)) }
        })
}
