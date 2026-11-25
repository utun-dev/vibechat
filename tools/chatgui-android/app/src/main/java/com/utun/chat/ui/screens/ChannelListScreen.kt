package com.utun.chat.ui.screens

import com.utun.chat.ui.components.PortableRadioIcon
import com.utun.chat.R
import androidx.compose.material.icons.filled.Link
import com.utun.chat.BuildConfig
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.res.painterResource
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.QrCode
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Image
import androidx.compose.material.icons.filled.TextSnippet
import com.utun.chat.data.ClipboardExchange
import kotlinx.coroutines.delay
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import com.utun.chat.ui.components.PeerStatusDot
import com.utun.chat.ui.components.CpuLoadIndicator
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.utun.chat.ui.components.GroupAvatar
import com.utun.chat.ui.components.UserAvatar
import com.utun.chat.ui.components.GroupPropertiesDialog
import com.utun.chat.ui.components.OwnStreamButton
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.utun.chat.data.Channel
import com.utun.chat.data.DmConversation
import com.utun.chat.data.LogManager

/** Каталог каналов/PM с разными ключами строк и generation PM; pending удаления блокирует повторные действия.
 * CPU polling включён только в RESUMED; ключик логов находится слева от CPU-индикатора в нижней панели.
 * Сетевой список/ошибки удаления принадлежат вызывающему владельцу; верхняя точка показывает зависание ядра. */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun ChannelListScreen(
    channels: List<Channel>,
    activeRadioGroup: String? = null,
    dmConversations: List<DmConversation> = emptyList(),
    connected: Boolean,
    isStuck: Boolean = false,
    cpuLoad: Int = 0,
    onCpuLoadVisible: (Boolean) -> Unit = {},
    onSelectChannel: (Channel) -> Unit,
    onSelectDm: (DmConversation) -> Unit = {},
    onDeleteDm: (DmConversation) -> Unit,
    deletingDm: Set<String> = emptySet(),
    dmDeleteFailed: Boolean = false,
    onDismissDmDeleteError: () -> Unit,
    onCreateChannel: (String, String) -> Unit,
    onJoinChannel: () -> Unit,
    onInviteClick: (Channel) -> Unit,
    onDeleteChannel: (Channel) -> Unit,
    onSettings: () -> Unit,
    onLogs: () -> Unit,
    onOwnStream: (String) -> Unit = {},
    onOpenLink: () -> Unit = {},
    onRefresh: () -> Unit = {}
) {
    LifecycleResumeEffect(Unit) {
        onCpuLoadVisible(true)
        onPauseOrDispose { onCpuLoadVisible(false) }
    }
    var showCreateDialog by remember { mutableStateOf(false) }
    var menuChannelId by remember { mutableStateOf<String?>(null) }
    var optionsChannel by remember { mutableStateOf<Channel?>(null) }
    var deleteConfirmChannel by remember { mutableStateOf<Channel?>(null) }
    var menuDmId by remember { mutableStateOf<String?>(null) }
    var deleteConfirmDm by remember { mutableStateOf<DmConversation?>(null) }
    var avatarChannelId by rememberSaveable { mutableStateOf<String?>(null) }
    val memberClipboards by ClipboardExchange.memberOffers.collectAsState()
    val clipboardMembers by com.utun.chat.ChatApplication.instance.members.members.collectAsState()
    var memberClipboard by remember { mutableStateOf<ClipboardExchange.MemberOffer?>(null) }
    val optionsLabel = stringResource(R.string.options)
    memberClipboard?.let { offer ->
        val sender = clipboardMembers.entries.firstOrNull { it.key.group == offer.group && it.value.ed25519 == offer.peer }?.value?.name ?: ClipboardExchange.sender(offer)
        AlertDialog(onDismissRequest = { memberClipboard = null }, title = { Text(stringResource(R.string.clipboard_member_received)) },
            text = { Text(stringResource(R.string.clipboard_member_description, sender,
                stringResource(if (offer.kind == 1) R.string.clipboard_type_text else R.string.clipboard_type_image))) },
            confirmButton = { TextButton(onClick = { ClipboardExchange.copyMember(offer) }) { Text(stringResource(R.string.clipboard_member_copy)) } },
            dismissButton = { TextButton(onClick = { ClipboardExchange.dismissMember(offer); memberClipboard = null }) { Text(stringResource(R.string.clipboard_member_dismiss)) } })
    }

    LaunchedEffect(channels, dmConversations, deletingDm) {
        if (channels.none { it.id == menuChannelId }) menuChannelId = null
        if (channels.none { it.id == avatarChannelId }) avatarChannelId = null
        optionsChannel?.let { selected -> optionsChannel = channels.firstOrNull { it.id == selected.id } }
        deleteConfirmChannel?.let { selected -> deleteConfirmChannel = channels.firstOrNull { it.id == selected.id } }
        if (dmConversations.none { it.convId == menuDmId && it.convId !in deletingDm }) menuDmId = null
        deleteConfirmDm?.let { selected ->
            if (dmConversations.none { it.convId == selected.convId && it.generation == selected.generation && it.convId !in deletingDm }) {
                LogManager.addLog("DEBUG", "UI", "Dismiss stale PM deletion conv=${selected.convId} generation=${selected.generation}")
                deleteConfirmDm = null
            }
        }
    }

    LaunchedEffect(Unit) {
        LogManager.addLog("INFO", "UI", "Channel list: version=${BuildConfig.VERSION_NAME} code=${BuildConfig.VERSION_CODE}")
        onRefresh()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("vibechat") },
                actions = {
                    Box(
                        Modifier.size(10.dp)
                            .background(if (isStuck) Color(0xFFFFB4AB) else Color(0xFF81C784), CircleShape)
                    )
                    Spacer(Modifier.width(4.dp))
                    OwnStreamButton("channel-list", onOwnStream)
                    IconButton(onClick = onJoinChannel) { Icon(Icons.Default.QrCode, stringResource(R.string.join)) }
                    IconButton(onClick = onOpenLink) { Icon(Icons.Default.Link, stringResource(R.string.open_link)) }
                    IconButton(onClick = onSettings) { Icon(Icons.Default.Settings, stringResource(R.string.settings)) }
                }
            )
        },
        floatingActionButton = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = onLogs) { Icon(Icons.Default.Build, stringResource(R.string.logs)) }
                val clipboardBusy by ClipboardExchange.sending.collectAsState()
                val receivedClipboard by ClipboardExchange.received.collectAsState()
                var clipboardFlash by remember { mutableStateOf(false) }
                LaunchedEffect(receivedClipboard.second) {
                    clipboardFlash = receivedClipboard.first != 0; delay(1500); clipboardFlash = false
                }
                if (receivedClipboard.first != 0) IconButton(onClick = ClipboardExchange::copyReceived,
                    modifier = Modifier.background(if (clipboardFlash) MaterialTheme.colorScheme.primaryContainer else Color.Transparent, CircleShape)) {
                    Icon(if (receivedClipboard.first == 1) Icons.Default.TextSnippet else Icons.Default.Image,
                        stringResource(R.string.clipboard_received))
                }
                IconButton(onClick = ClipboardExchange::send, enabled = !clipboardBusy) {
                    Icon(painterResource(R.drawable.ic_clipboard_send), stringResource(R.string.clipboard_send))
                }
                CpuLoadIndicator(cpuLoad, Modifier.padding(end = 12.dp))
                Text(
                    text = "v${BuildConfig.VERSION_NAME}",
                    modifier = Modifier.padding(end = 16.dp),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                FloatingActionButton(onClick = { showCreateDialog = true }) {
                    Icon(Icons.Default.Add, stringResource(R.string.new_channel))
                }
            }
        }
    ) { padding ->
        Column(modifier = Modifier.padding(padding)) {
            if (channels.isEmpty() && dmConversations.isEmpty()) {
                Box(Modifier.fillMaxSize().weight(1f)) {
                    Text(
                        stringResource(R.string.no_channels),
                        modifier = Modifier.padding(32.dp),
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            } else {
                LazyColumn(contentPadding = PaddingValues(bottom = 88.dp)) {
                    items(channels, key = { "channel:${it.id}" }) { ch ->
                        val barColor = if (ch.peersOnline > 0) Color(0xFF81C784) else Color(0xFFAEBCC8)
                        val badgeText = when {
                            ch.peersOnline > 99 -> "99+"
                            ch.peersOnline > 0  -> ch.peersOnline.toString()
                            else -> ""
                        }

                        val openMenu: (String) -> Unit = { source ->
                            LogManager.addLog("DEBUG", "UI", "Channel menu opened channel=${ch.id} source=$source")
                            menuDmId = null
                            menuChannelId = ch.id
                        }
                        Surface(modifier = Modifier.combinedClickable(onLongClickLabel = optionsLabel,
                            onLongClick = { openMenu("row") }, onClick = {
                                LogManager.addLog("INFO", "UI", "Channel opened channel=${ch.id}")
                                onSelectChannel(ch)
                            })) {
                            Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min)) {
                                Box(Modifier.width(4.dp).fillMaxHeight().background(barColor))
                                Row(Modifier.fillMaxWidth().padding(12.dp)) {
                                    Box(Modifier.size(44.dp).combinedClickable(onLongClickLabel = optionsLabel,
                                        onLongClick = { openMenu("avatar") }, onClick = { avatarChannelId = ch.id }),
                                        contentAlignment = Alignment.Center) {
                                        GroupAvatar(ch.id, ch.name)
                                        DropdownMenu(
                                            expanded = menuChannelId == ch.id,
                                            onDismissRequest = { menuChannelId = null }
                                        ) {
                                            DropdownMenuItem(
                                                text = { Text(stringResource(R.string.invite_link)) },
                                                onClick = {
                                                    LogManager.addLog("INFO", "UI", "Channel menu invite channel=${ch.id}")
                                                    menuChannelId = null; onInviteClick(ch)
                                                }
                                            )
                                            DropdownMenuItem(
                                                text = { Text(stringResource(R.string.group_properties)) },
                                                onClick = {
                                                    LogManager.addLog("INFO", "UI", "Channel menu properties channel=${ch.id}")
                                                    menuChannelId = null; optionsChannel = ch
                                                }
                                            )
                                            DropdownMenuItem(
                                                text = { Text(stringResource(R.string.delete_channel), color = MaterialTheme.colorScheme.error) },
                                                onClick = {
                                                    LogManager.addLog("INFO", "UI", "Channel menu delete channel=${ch.id}")
                                                    menuChannelId = null; deleteConfirmChannel = ch
                                                }
                                            )
                                        }
                                        if (badgeText.isNotEmpty()) {
                                            val badgeSize = 18.dp
                                            Box(
                                                Modifier
                                                    .align(Alignment.TopStart)
                                                    .offset(x = (-4).dp, y = (-6).dp)
                                                    .size(badgeSize)
                                                    .clip(CircleShape)
                                                    .background(Color(0xFF24633D)),
                                                contentAlignment = Alignment.Center
                                            ) {
                                                Text(
                                                    badgeText, fontSize = 9.sp, color = Color.White,
                                                    fontWeight = FontWeight.Bold, textAlign = TextAlign.Center
                                                )
                                            }
                                        }
                                    }
                                    Spacer(Modifier.width(10.dp))
                                    Column(Modifier.weight(1f)) {
                                        Row(verticalAlignment = Alignment.CenterVertically) {
                                            Text((if (ch.groupType == "kanban") "▦ " else "") + ch.name, fontWeight = FontWeight.Bold, fontSize = 16.sp, maxLines = 1,
                                                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis)
                                        }
                                        if (ch.lastMsgEncrypted || ch.lastMsg.isNotEmpty())
                                            Text(if (ch.lastMsgEncrypted) stringResource(R.string.attachment_encrypted_title) else ch.lastMsg, fontSize = 13.sp,
                                                color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
                                    }
                                    memberClipboards[ch.id]?.let { offer ->
                                        val sender = clipboardMembers.entries.firstOrNull { it.key.group == offer.group && it.value.ed25519 == offer.peer }?.value?.name ?: ClipboardExchange.sender(offer)
                                        Column(Modifier.widthIn(max = 88.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                                            IconButton(onClick = { memberClipboard = offer }) {
                                                Icon(if (offer.kind == 1) Icons.Default.TextSnippet else Icons.Default.Image,
                                                    stringResource(R.string.clipboard_member_received), tint = Color(0xFFFFC107))
                                            }
                                            if (androidx.compose.ui.platform.LocalConfiguration.current.screenWidthDp >= 360)
                                                Text(sender, fontSize = 10.sp, maxLines = 1, overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis)
                                        }
                                    }
                                    if (activeRadioGroup != null && ch.id == activeRadioGroup) {
                                        Box(Modifier.width(36.dp).height(48.dp), contentAlignment = Alignment.Center) {
                                            PortableRadioIcon(stringResource(R.string.radio_active_channel), tint = Color(0xFF4CAF50))
                                        }
                                    }
                                }
                            }
                        }
                        HorizontalDivider()
                    }

                    if (dmConversations.isNotEmpty()) {
                        item {
                            Text(
                                stringResource(R.string.private_chats),
                                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
                                fontSize = 13.sp,
                                fontWeight = FontWeight.Bold,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                        items(dmConversations, key = { "dm:${it.convId}:${it.generation}" }) { dm ->
                            val enabled = dm.convId !in deletingDm
                            val openMenu: (String) -> Unit = { source ->
                                LogManager.addLog("DEBUG", "UI", "PM menu opened conv=${dm.convId} generation=${dm.generation} source=$source")
                                menuChannelId = null
                                menuDmId = dm.convId
                            }
                            Surface(modifier = Modifier.combinedClickable(enabled = enabled, onLongClickLabel = optionsLabel,
                                onLongClick = { openMenu("row") }, onClick = {
                                    LogManager.addLog("INFO", "UI", "PM opened conv=${dm.convId} generation=${dm.generation}")
                                    onSelectDm(dm)
                                })) {
                                Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min)) {
                                    Box(Modifier.width(4.dp).fillMaxHeight().background(Color(0xFFD2A8FF)))
                                    Row(Modifier.fillMaxWidth().padding(12.dp)) {
                                        Box(Modifier.size(44.dp), contentAlignment = Alignment.Center) {
                                            UserAvatar(null, dm.peerEd25519, dm.displayName, size = 44.dp,
                                                enabled = enabled, onLongClick = { openMenu("avatar") })
                                            DropdownMenu(expanded = menuDmId == dm.convId, onDismissRequest = { menuDmId = null }) {
                                                DropdownMenuItem(
                                                    text = { Text(stringResource(R.string.delete_conversation), color = MaterialTheme.colorScheme.error) },
                                                    enabled = enabled,
                                                    onClick = {
                                                        LogManager.addLog("INFO", "UI", "PM menu delete conv=${dm.convId} generation=${dm.generation}")
                                                        menuDmId = null; deleteConfirmDm = dm
                                                    }
                                                )
                                            }
                                            PeerStatusDot(dm.online, Modifier.align(Alignment.TopStart).offset(x = (-4).dp, y = (-6).dp))
                                        }
                                        Spacer(Modifier.width(10.dp))
                                        Column {
                                            Text(dm.displayName, fontWeight = FontWeight.Bold, fontSize = 16.sp)
                                        }
                                    }
                                }
                            }
                            HorizontalDivider()
                        }
                    }
                }
            }
        }
    }

    channels.firstOrNull { it.id == avatarChannelId }?.let { ch ->
        GroupAvatarViewerScreen(ch.id, ch.name, onClose = { avatarChannelId = null })
    }

    deleteConfirmDm?.let { dm ->
        AlertDialog(
            onDismissRequest = { deleteConfirmDm = null },
            title = { Text(stringResource(R.string.delete_conversation)) },
            text = { Text(stringResource(R.string.delete_conversation_confirm, dm.displayName)) },
            confirmButton = {
                TextButton(onClick = { deleteConfirmDm = null; onDeleteDm(dm) },
                    colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error)) {
                    Text(stringResource(R.string.delete))
                }
            },
            dismissButton = { TextButton(onClick = { deleteConfirmDm = null }) { Text(stringResource(R.string.cancel)) } }
        )
    }
    if (dmDeleteFailed) {
        AlertDialog(onDismissRequest = onDismissDmDeleteError, text = { Text(stringResource(R.string.delete_conversation_failed)) },
            confirmButton = { TextButton(onClick = onDismissDmDeleteError) { Text(stringResource(android.R.string.ok)) } })
    }

    if (showCreateDialog) {
        CreateChannelDialog(
            onDismiss = { showCreateDialog = false },
            onCreate = { name, type ->
                showCreateDialog = false
                onCreateChannel(name, type)
            }
        )
    }

    optionsChannel?.let { ch ->
        GroupPropertiesDialog(
            channel = ch,
            onDismiss = { optionsChannel = null }
        )
    }

    deleteConfirmChannel?.let { ch ->
        AlertDialog(
            onDismissRequest = { deleteConfirmChannel = null },
            title = { Text(stringResource(R.string.delete_channel)) },
            text = { Text(stringResource(R.string.delete_channel_confirm, ch.name)) },
            confirmButton = {
                TextButton(onClick = { deleteConfirmChannel = null; onDeleteChannel(ch) },
                    colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error)) {
                    Text(stringResource(R.string.delete))
                }
            },
            dismissButton = {
                TextButton(onClick = { deleteConfirmChannel = null }) { Text(stringResource(R.string.cancel)) }
            }
        )
    }
}

/** Создаёт имя с обрезанными краями; генерацией ID и результатом создания владеет onCreate. */
@Composable
fun CreateChannelDialog(onDismiss: () -> Unit, onCreate: (String, String) -> Unit) {
    var name by rememberSaveable { mutableStateOf("") }
    var kanban by rememberSaveable { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.new_channel)) },
        text = {
            Column {
            OutlinedTextField(
                value = name,
                onValueChange = { name = it },
                label = { Text(stringResource(R.string.channel_name)) },
                singleLine = true
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                Checkbox(checked = kanban, onCheckedChange = { kanban = it })
                Text(stringResource(R.string.kanban_board))
            }
            }
        },
        confirmButton = {
            Button(onClick = { if (name.isNotBlank()) onCreate(name.trim(), if (kanban) "kanban" else "chat") }, enabled = name.isNotBlank()) {
                Text(stringResource(R.string.create))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) }
        }
    )
}
