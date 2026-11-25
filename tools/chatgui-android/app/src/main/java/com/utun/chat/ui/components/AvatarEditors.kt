package com.utun.chat.ui.components

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.utun.chat.ChatApplication
import com.utun.chat.R
import com.utun.chat.data.*

/** Свойства CHAT-группы: отдельный аватар группы и локальный звук уведомлений.
 * При открытии обновляет снимок; canEdit показывает кнопки публикации, окончательный допуск проверяет core.
 * Выбранный URI передаётся в editGroup без crop-диалога: репозиторий берёт центральный квадрат.
 * Account/group фиксируются до picker, изменения применяются сразу и продолжаются после закрытия диалога.
 */
@Composable
fun GroupPropertiesDialog(channel: Channel, onDismiss: () -> Unit) {
    var showPolicy by remember { mutableStateOf(false) }
    var showRename by remember { mutableStateOf(false) }
    if (showPolicy) GroupPolicyDialog(channel.id) { showPolicy = false }
    if (showRename) GroupRenameDialog(channel.id) { showRename = false }
    val repository = ChatApplication.instance.avatars
    val groups by repository.groups.collectAsState()
    val edits by repository.edits.collectAsState()
    val state = groups[channel.id]
    val editState = edits["group:${channel.id}"] ?: AvatarEditState.IDLE
    val working = editState == AvatarEditState.WORKING
    val provider = ChatApplication.instance.configProvider
    val soundPreferences = remember(provider) { provider.changes.observe("group sound", provider.notificationSoundSettings) }
    val soundSettings by soundPreferences.collectAsState(initial = NotificationSound.enabled to NotificationSound.mutedChannels)
    val soundFailed by provider.changes.failed.collectAsState()
    var pickerFailed by remember { mutableStateOf(false) }
    var pickedAccount by rememberSaveable { mutableStateOf("") }
    var pickedGroup by rememberSaveable { mutableStateOf("") }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null) repository.editGroup(pickedGroup, uri, pickedAccount)
    }
    LaunchedEffect(channel.id) { repository.ensureGroup(channel.id, true) }
    AlertDialog(onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.group_properties)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = { showPolicy = true }) { Text(stringResource(R.string.group_policy_title)) }
                TextButton(onClick = { showRename = true }) { Text(stringResource(R.string.group_name_clipboard)) }
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    GroupAvatar(channel.id, channel.name, size = 64.dp)
                    Column { Text(channel.name, style = MaterialTheme.typography.titleMedium); Text(channel.id) }
                }
                if (state?.loading == true || working) LinearProgressIndicator(Modifier.fillMaxWidth())
                if (state?.error == true) {
                    Text(stringResource(R.string.avatar_load_failed), color = MaterialTheme.colorScheme.error)
                    TextButton(onClick = { repository.ensureGroup(channel.id, true) }) { Text(stringResource(R.string.avatar_retry)) }
                }
                if (state?.canEdit == true) {
                    Text(stringResource(R.string.group_avatar_limits), style = MaterialTheme.typography.bodySmall)
                    Row {
                        TextButton(enabled = !working, onClick = {
                            pickedAccount = repository.accountId; pickedGroup = channel.id
                            try { pickerFailed = false; picker.launch("image/*") }
                            catch (e: Exception) { pickerFailed = true; LogManager.addLog("ERROR", "Avatar", "Group image picker failed: $e") }
                        }) { Text(stringResource(R.string.avatar_upload)) }
                        TextButton(enabled = !working && state.hasImage, onClick = {
                            repository.editGroup(channel.id, null, repository.accountId)
                        }) { Text(stringResource(R.string.avatar_remove)) }
                    }
                } else Text(stringResource(R.string.group_avatar_key_required), style = MaterialTheme.typography.bodySmall)
                if (editState == AvatarEditState.ERROR || pickerFailed)
                    Text(stringResource(R.string.avatar_edit_failed), color = MaterialTheme.colorScheme.error)
                if (soundFailed) Text(stringResource(R.string.settings_update_failed), color = MaterialTheme.colorScheme.error)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(stringResource(R.string.sound), modifier = Modifier.weight(1f))
                    Switch(checked = channel.id !in soundSettings.second, onCheckedChange = { enabled ->
                        provider.changes.submit("group notification sound") {
                            provider.setChannelMuted(channel.id, !enabled)
                            NotificationSound.setMuted(channel.id, !enabled)
                            if (NativeLib.isReady()) NativeLib.setSoundOnMessage(channel.id, enabled)
                            LogManager.addLog("INFO", "Sound", "Group sound saved channel=${channel.id} enabled=$enabled")
                        }
                    })
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) } })
}

/** Настройки локального профиля: выбор подписанного хранителя, crop-редактор и снятие аватара.
 * Список хранителей не требует online; account/server фиксируются до запуска picker.
 * Временный файл редактора передаётся во владение editProfile. SAVED подтверждает локальное намерение;
 * файлы и ссылки публикуются репозиторием отдельно в подходящих группах, без ожидания в этом UI. */
@Composable
fun ProfileAvatarControls() {
    val repository = ChatApplication.instance.avatars
    val revision by repository.revision.collectAsState()
    val members by repository.members.collectAsState()
    val edits by repository.edits.collectAsState()
    val servers = remember(members) { repository.storageServers() }
    var selectedServer by rememberSaveable { mutableStateOf("") }
    var menu by remember { mutableStateOf(false) }
    var pickedAccount by rememberSaveable { mutableStateOf("") }
    var pickedServer by rememberSaveable { mutableStateOf("") }
    var cropUri by rememberSaveable { mutableStateOf<String?>(null) }
    var pickerFailed by remember { mutableStateOf(false) }
    val state = edits["profile"] ?: AvatarEditState.IDLE
    LaunchedEffect(servers) {
        if (servers.none { it.address == selectedServer }) selectedServer = servers.firstOrNull()?.address.orEmpty()
    }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null) cropUri=uri.toString()
    }
    cropUri?.let { value -> AvatarCropDialog(android.net.Uri.parse(value),onCancel={ cropUri=null },onConfirm={ file,crop ->
        repository.editProfile(pickedServer,file,pickedAccount,crop); cropUri=null
    }) }
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(stringResource(R.string.profile_avatar), style = MaterialTheme.typography.titleMedium)
        UserAvatar(null, repository.accountId, "", size = 64.dp, profile = true)
        Text(stringResource(R.string.profile_avatar_hint), style = MaterialTheme.typography.bodySmall)
        val locations = remember(members, revision) { repository.profileStorageStates() }
        locations.forEach { location -> Text(stringResource(R.string.avatar_group_location, location.group,
            avatarStorageText(location)), style = MaterialTheme.typography.bodySmall) }
        if (locations.isEmpty() && repository.profileBitmap() != null) Text(stringResource(R.string.avatar_local_pending))
        if (servers.isEmpty()) Text(stringResource(R.string.avatar_no_storage), style = MaterialTheme.typography.bodySmall)
        else Box {
            TextButton(onClick = { menu = true }, enabled = state != AvatarEditState.WORKING) {
                Text(stringResource(R.string.avatar_storage_server, servers.firstOrNull { it.address == selectedServer }?.let {
                    "${it.name.ifBlank { it.address }} (${it.address})"
                } ?: "…"))
            }
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                servers.forEach { server ->
                    DropdownMenuItem(text = { Text("${server.name} (${server.address})") },
                        onClick = { selectedServer = server.address; menu = false })
                }
            }
        }
        Row {
            TextButton(enabled = selectedServer.isNotEmpty() && state != AvatarEditState.WORKING, onClick = {
                pickedAccount = repository.accountId; pickedServer = selectedServer
                try { pickerFailed = false; picker.launch("image/*") }
                catch (e: Exception) { pickerFailed = true; LogManager.addLog("ERROR", "Avatar", "Profile image picker failed: $e") }
            }) { Text(stringResource(R.string.avatar_upload)) }
            TextButton(enabled = repository.accountId.isNotEmpty() && state != AvatarEditState.WORKING, onClick = {
                repository.editProfile("", null, repository.accountId)
            }) { Text(stringResource(R.string.avatar_remove)) }
        }
        if (pickerFailed) Text(stringResource(R.string.avatar_edit_failed), color = MaterialTheme.colorScheme.error)
        when (state) {
            AvatarEditState.WORKING -> LinearProgressIndicator(Modifier.fillMaxWidth())
            AvatarEditState.ERROR -> Text(stringResource(R.string.avatar_edit_failed), color = MaterialTheme.colorScheme.error)
            AvatarEditState.SAVED -> Text(stringResource(R.string.profile_avatar_saved), style = MaterialTheme.typography.bodySmall)
            else -> Unit
        }
    }
}
