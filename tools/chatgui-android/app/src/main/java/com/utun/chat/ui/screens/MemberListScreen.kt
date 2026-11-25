package com.utun.chat.ui.screens

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.CircleShape
import com.utun.chat.ui.components.UserAvatar
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import com.utun.chat.data.LogManager
import androidx.compose.material.icons.automirrored.filled.Message
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Remove
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Storage
import androidx.compose.material.icons.filled.Bolt
import androidx.compose.material.icons.filled.Shield
import androidx.compose.material.icons.filled.Star
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import com.utun.chat.data.DevgroupRepository
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.sp
import com.utun.chat.data.Channel
import com.utun.chat.data.ChatMember
import com.utun.chat.data.MemberDetail
import com.utun.chat.data.MemberLinks
import com.utun.chat.data.LinkDirection
import com.utun.chat.data.LocalSocketState
import com.utun.chat.ui.components.LocalSocketNatInfo
import com.utun.chat.ui.components.PortableRadioIcon
import com.utun.chat.ui.components.RadioSpeakerIcon
import com.utun.chat.viewmodel.ChatViewModel

/** Список общего MemberRepository и карточка БД/live-состояния выбранного узла текущего канала.
 * Локальный nickname доступен без ключа владельца; изменение флагов и передача прав требуют isOwner.
 * Nickname записывается в локальную БД, флаги/права направляются в ядро; итог подтверждает последующий снимок, не закрытие редактора. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MemberListScreen(
    channel: Channel,
    members: List<ChatMember>,
    selectedMember: MemberDetail?,
    viewModel: ChatViewModel,
    onFiles: (String) -> Unit = {},
    onBack: () -> Unit
) {
    var showEditDialog by remember(channel.id) { mutableStateOf(false) }
    var editMember by remember(channel.id) { mutableStateOf<ChatMember?>(null) }
    var expandedGroups by rememberSaveable(channel.id) { mutableStateOf(listOf<String>()) }
    var deletedExpanded by rememberSaveable(channel.id) { mutableStateOf(false) }
    var selectedGroup by rememberSaveable(channel.id) { mutableStateOf<String?>(null) }
    var renamingGroup by rememberSaveable(channel.id) { mutableStateOf<String?>(null) }
    var renameName by rememberSaveable(channel.id) { mutableStateOf("") }
    var linkingDevice by rememberSaveable(channel.id) { mutableStateOf<String?>(null) }
    var messageChoice by rememberSaveable(channel.id) { mutableStateOf<String?>(null) }
    var linkFailed by remember(linkingDevice) { mutableStateOf(false) }
    val devices by DevgroupRepository.state.collectAsState()
    var clipboardAllowed by remember(channel.id) { mutableStateOf(false) }
    LaunchedEffect(channel.id) {
        try {
            val result = com.utun.chat.ui.components.groupInfoRequest(channel.id, null, null)
            clipboardAllowed = result.optBoolean("ok") && result.optJSONObject("description")?.optBoolean("clipboard_members") == true
        } catch (e: kotlinx.coroutines.CancellationException) { throw e }
        catch (e: Exception) { LogManager.addLog("WARN", "Clipboard", "Cannot read group permission channel=${channel.id}: $e") }
    }
    val orderedMembers = remember(members) { members.filterNot { it.isDeleted }.sortedByDescending { it.isSelf } }
    val deletedMembers = remember(members) { members.filter { it.isDeleted } }
    LaunchedEffect(channel.id, deletedMembers.size, deletedExpanded) {
        LogManager.addLog("DEBUG", "Members", "Deleted group: channel=${channel.id} count=${deletedMembers.size} expanded=$deletedExpanded")
    }
    val rows = remember(orderedMembers, expandedGroups) {
        buildList<Pair<ChatMember, Boolean>> {
            val seen = mutableSetOf<String>()
            for (member in orderedMembers) {
                if (member.devgroup.isEmpty()) add(member to false)
                else if (seen.add(member.devgroup)) {
                    add(member to true)
                    if (member.devgroup in expandedGroups) orderedMembers.filter { it.devgroup == member.devgroup }.forEach { add(it to false) }
                }
            }
        }
    }
    val selection = selectedMember?.takeIf { it.channelId == channel.id }
    val selectedGroupMembers = orderedMembers.filter { it.devgroup == selectedGroup }
    LaunchedEffect(selectedGroup, orderedMembers) {
        if (selectedGroup != null && selectedGroupMembers.isEmpty()) selectedGroup = null
        if (renamingGroup != null && orderedMembers.none { it.isSelf && !it.isDeleted && it.devgroup == renamingGroup }) {
            LogManager.addLog("WARN", "Members", "Device group rename cancelled: membership changed")
            renamingGroup = null
        }
    }

    LaunchedEffect(channel.id) {
        LogManager.addLog("DEBUG", "Members", "Member list: channel=${channel.id} order=self-first actionWidth=36dp statusIconGap=2dp")
        while (true) { viewModel.refreshMembers(channel.id); kotlinx.coroutines.delay(2000) }
    }

    val memberLinks by viewModel.memberLinks.collectAsState()
    val localSockets by viewModel.localSockets.collectAsState()
    val channelOwner by viewModel.channelOwner.collectAsState()
    val radioMuteFailed by viewModel.radioMuteFailed.collectAsState()
    val dmStartFailed by viewModel.dmStartFailed.collectAsState()
    val radioActive by viewModel.radioActive.collectAsState()
    val radioTalkState by viewModel.radioTalkState.collectAsState()
    val transmittingGroup by viewModel.radioAudio.transmittingGroup.collectAsState()
    val groupId = channel.id
    val isSpeaking: (ChatMember) -> Boolean = { member -> radioActive &&
        if (member.isSelf) transmittingGroup == groupId
        else radioTalkState.groupId == groupId && member.ed25519 in radioTalkState.speakers }

    LaunchedEffect(channel.id, selection?.ed25519) {
        viewModel.refreshMemberLinks(selection?.ed25519.orEmpty())
    }

    Column(modifier = Modifier.fillMaxSize().navigationBarsPadding()) {
        TopAppBar(
            title = { Text(stringResource(if (channelOwner) R.string.members_owner_count else R.string.members_count, members.size)) },
            navigationIcon = {
                IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back)) }
            }
        )

        LazyColumn(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
        ) {
            itemsIndexed(rows, key = { _, item -> if (item.second) "group:" + item.first.devgroup else "node:" + item.first.ed25519 }) { rowIndex, (m, header) ->
                val accounts = if (header) orderedMembers.filter { it.devgroup == m.devgroup && !it.isDeleted } else listOf(m)
                val target = if (header) accounts.firstOrNull { it.online && !it.isSelf } else m
                val device = target ?: m
                val avatar = if (header) com.utun.chat.data.deviceGroupAvatar(accounts) ?: m else m
                val groupRadio = accounts.any { it.radioActive }
                val groupPhoneKnown = accounts.any { it.phoneStatusKnown }
                val groupPhoneBusy = accounts.any { it.phoneBusy }
                LaunchedEffect(header, target?.ed25519, avatar.ed25519, groupRadio, groupPhoneKnown, groupPhoneBusy) {
                    if (header) LogManager.addLog("DEBUG", "Members", "Device group: group=${m.devgroup} " +
                        "target=${target?.ed25519} avatar=${avatar.ed25519} radio=$groupRadio phoneKnown=$groupPhoneKnown phoneBusy=$groupPhoneBusy speakerColumn=right")
                }
                MemberListItem(
                    member = device,
                    avatarMember = avatar,
                    groupName = if (header) m.devgroupName else null,
                    expanded = m.devgroup in expandedGroups,
                    groupChild = !header && m.devgroup.isNotEmpty(),
                    lastGroupChild = rows.getOrNull(rowIndex + 1)?.let { it.second || it.first.devgroup != m.devgroup } ?: true,
                    actionsEnabled = target != null,
                    radioEnabled = groupRadio,
                    phoneStatusKnown = groupPhoneKnown,
                    phoneBusy = groupPhoneBusy,
                    onToggleGroup = {
                        expandedGroups = if (m.devgroup in expandedGroups) expandedGroups - m.devgroup else expandedGroups + m.devgroup
                        selectedGroup = m.devgroup; viewModel.clearMemberDetail()
                        LogManager.addLog("DEBUG", "Members", "Device group: toggled group=${m.devgroup} expanded=${m.devgroup in expandedGroups}")
                    },
                    group = channel.id,
                    speaking = target != null && isSpeaking(device),
                    isSelected = if (header) selectedGroup == m.devgroup else selectedGroup == null && selection?.ed25519 == device.ed25519,
                    onClick = {
                        if (header) {
                            selectedGroup = m.devgroup; viewModel.clearMemberDetail()
                            LogManager.addLog("DEBUG", "Members", "Device group selected group=${m.devgroup} devices=${accounts.size}")
                        } else { selectedGroup = null; viewModel.showMemberDetail(channel.id, device.ed25519) }
                    },
                    onCall = { target?.let { viewModel.startCall(channel.id, it.ed25519, if (header) m.devgroupName else it.name, devices = header) } },
                    onRadioCall = { target?.let { com.utun.chat.data.RadioCallController.start(channel.id, it.ed25519, devices = header) } },
                    onRadioMute = { target?.let { viewModel.toggleMemberRadioMute(channel.id, it.ed25519) } },
                    onClipboard = if (!header && clipboardAllowed && !device.isSelf && !device.isDeleted && device.online && device.ed25519.isNotEmpty())
                        ({ com.utun.chat.data.ClipboardExchange.sendTo(channel.id, device.ed25519) }) else null,
                    onMyDevice = if (!header && device.canLinkAsMyDevice) ({
                        linkingDevice = device.ed25519
                        LogManager.addLog("INFO", "Devgroup", "Link explanation opened channel=${channel.id} peer=${device.ed25519}")
                    }) else null,
                    onFinishPairing = if ((if (header) accounts.any { it.isSelf } else device.isSelf) && devices.invitations.any { it.pending })
                        ({ DevgroupRepository.openPairing() }) else null
                )
            }
            if (deletedMembers.isNotEmpty()) {
                item(key = "deleted") {
                    Row(Modifier.fillMaxWidth().clickable {
                        deletedExpanded = !deletedExpanded
                    }.padding(horizontal = 16.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                        IconButton(onClick = { deletedExpanded = !deletedExpanded }, modifier = Modifier.size(width = 24.dp, height = 48.dp)) {
                            Icon(if (deletedExpanded) Icons.Filled.Remove else Icons.Filled.Add,
                                stringResource(if (deletedExpanded) R.string.deleted_accounts_collapse else R.string.deleted_accounts_expand),
                                modifier = Modifier.size(18.dp))
                        }
                        Text(stringResource(R.string.deleted_accounts), fontWeight = FontWeight.Bold, modifier = Modifier.padding(start = 4.dp))
                    }
                }
                if (deletedExpanded) itemsIndexed(deletedMembers, key = { _, member -> "node:" + member.ed25519 }) { index, member ->
                    MemberListItem(member = member, group = channel.id, speaking = isSpeaking(member),
                        groupChild = true, lastGroupChild = index == deletedMembers.lastIndex,
                        isSelected = selectedGroup == null && selection?.ed25519 == member.ed25519,
                        onClick = { selectedGroup = null; viewModel.showMemberDetail(channel.id, member.ed25519) },
                        onCall = { viewModel.startCall(channel.id, member.ed25519, member.name) },
                        onRadioCall = { com.utun.chat.data.RadioCallController.start(channel.id, member.ed25519) },
                        onRadioMute = { viewModel.toggleMemberRadioMute(channel.id, member.ed25519) })
                }
            }
        }

        if (selectedGroupMembers.isNotEmpty()) Card(Modifier.fillMaxWidth().padding(8.dp)) {
            Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(selectedGroupMembers.first().devgroupName, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
                if (selectedGroupMembers.any { it.isSelf && !it.isDeleted }) TextButton(
                    enabled = !devices.renaming && !devices.leaving,
                    onClick = { renamingGroup = selectedGroup; renameName = selectedGroupMembers.first().devgroupName }
                ) { Text(stringResource(R.string.devgroup_rename)) }
                else TextButton(onClick = {
                    selectedGroupMembers.firstOrNull { !it.isDeleted }?.let {
                        viewModel.startDm(channel.id, it.ed25519, it.devgroupName, groups = true)
                    }
                }) { Text(stringResource(R.string.message)) }
            }
            if (selectedGroup !in expandedGroups) LazyColumn(Modifier.heightIn(max = 240.dp)) {
                itemsIndexed(selectedGroupMembers, key = { _, member -> member.ed25519 }) { _, member ->
                    MemberListItem(member = member, group = channel.id, speaking = isSpeaking(member),
                        onClick = { selectedGroup = null; viewModel.showMemberDetail(channel.id, member.ed25519) },
                        onCall = { viewModel.startCall(channel.id, member.ed25519, member.name) },
                        onRadioCall = { com.utun.chat.data.RadioCallController.start(channel.id, member.ed25519) },
                        onRadioMute = { viewModel.toggleMemberRadioMute(channel.id, member.ed25519) },
                        onClipboard = if (clipboardAllowed && !member.isSelf && !member.isDeleted && member.online && member.ed25519.isNotEmpty())
                            ({ com.utun.chat.data.ClipboardExchange.sendTo(channel.id, member.ed25519) }) else null,
                        onFinishPairing = if (member.isSelf && devices.invitations.any { it.pending })
                            ({ DevgroupRepository.openPairing() }) else null)
                }
            }
        }

        AnimatedVisibility(
            visible = selectedGroup == null && selection != null,
            enter = slideInVertically(initialOffsetY = { it }),
            exit = slideOutVertically(targetOffsetY = { it })
        ) {
            selection?.let { detail ->
                val member = members.find { it.ed25519 == detail.ed25519 }
                MemberDetailCard(
                    detail = detail,
                    memberLinks = memberLinks,
                    localSocketStates = localSockets,
                    isSelf = member?.isSelf == true,
                    canEdit = member != null && detail.ed25519.length == 64,
                    onFiles = { onFiles(detail.ed25519) },
                    deviceActionEnabled = member?.isSelf == true && !member.isDeleted && !devices.leaving && devices.member,
                    onDeviceAction = { DevgroupRepository.command(4) },
                    onInvite = { viewModel.showPmInvite(detail.ed25519, member?.name ?: detail.name, channel.id) },
                    onMessage = {
                        if (devices.member && member?.devgroup.isNullOrEmpty()) messageChoice = detail.ed25519
                        else viewModel.startDm(channel.id, detail.ed25519, member?.name ?: detail.name)
                    },
                    onConnect = {
                        LogManager.addLog("INFO", "Members", "Member detail connect: channel=${channel.id} peer=${detail.ed25519}")
                        viewModel.connectMemberNode(channel.id, detail.ed25519)
                    },
                    onEdit = {
                        member?.let {
                            LogManager.addLog("INFO", "Members", "Member detail edit: channel=${channel.id} peer=${it.ed25519}")
                            editMember = it.copy(ed25519 = detail.ed25519)
                            showEditDialog = true
                        }
                    }
                )
            }
        }
    }

    messageChoice?.let { peer ->
        val target = members.firstOrNull { it.ed25519 == peer && !it.isDeleted }
        AlertDialog(onDismissRequest = { messageChoice = null },
            title = { Text(stringResource(R.string.dm_mode_title)) },
            confirmButton = { TextButton(enabled = target != null, onClick = {
                messageChoice = null
                target?.let { viewModel.startDm(channel.id, it.ed25519, it.name, groups = true) }
            }) { Text(stringResource(R.string.dm_mode_groups)) } },
            dismissButton = { TextButton(enabled = target != null, onClick = {
                messageChoice = null
                target?.let { viewModel.startDm(channel.id, it.ed25519, it.name) }
            }) { Text(stringResource(R.string.dm_mode_device)) } })
    }

    if (linkingDevice != null) {
        val target = members.firstOrNull { it.ed25519 == linkingDevice }
        AlertDialog(
            onDismissRequest = { linkingDevice = null },
            title = { Text(stringResource(R.string.devgroup_this_is_my_device)) },
            text = { Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.devgroup_link_explanation, target?.name ?: linkingDevice.orEmpty()))
                if (linkFailed || target?.canLinkAsMyDevice != true)
                    Text(stringResource(R.string.devgroup_link_unavailable), color = MaterialTheme.colorScheme.error)
            } },
            confirmButton = { TextButton(enabled = target?.canLinkAsMyDevice == true && !devices.leaving, onClick = {
                val node = linkingDevice ?: return@TextButton
                if (viewModel.linkMyDevice(channel.id, node)) linkingDevice = null else linkFailed = true
            }) { Text(stringResource(R.string.devgroup_start_linking)) } },
            dismissButton = { TextButton(onClick = { linkingDevice = null }) { Text(stringResource(android.R.string.cancel)) } }
        )
    }

    if (renamingGroup != null) AlertDialog(
        onDismissRequest = { renamingGroup = null },
        title = { Text(stringResource(R.string.devgroup_rename)) },
        text = { OutlinedTextField(value = renameName, onValueChange = { renameName = it }, singleLine = true,
            label = { Text(stringResource(R.string.devgroup_name)) }) },
        confirmButton = { TextButton(
            enabled = !devices.leaving && !devices.renaming && renameName.trim().isNotEmpty() &&
                renameName.trim().toByteArray(Charsets.UTF_8).size <= 63 &&
                '\u0000' !in renameName && orderedMembers.any { it.isSelf && !it.isDeleted && it.devgroup == renamingGroup },
            onClick = { DevgroupRepository.command(3, text = renameName.trim()); renamingGroup = null }
        ) { Text(stringResource(android.R.string.ok)) } },
        dismissButton = { TextButton(onClick = { renamingGroup = null }) { Text(stringResource(android.R.string.cancel)) } }
    )

    if (radioMuteFailed) {
        AlertDialog(
            onDismissRequest = viewModel::dismissRadioMuteError,
            text = { Text(stringResource(R.string.radio_member_mute_failed)) },
            confirmButton = { TextButton(onClick = viewModel::dismissRadioMuteError) { Text(stringResource(android.R.string.ok)) } }
        )
    }
    if (dmStartFailed) AlertDialog(
        onDismissRequest = viewModel::dismissDmStartError,
        text = { Text(stringResource(R.string.invite_pm_start_failed)) },
        confirmButton = { TextButton(onClick = viewModel::dismissDmStartError) { Text(stringResource(android.R.string.ok)) } }
    )

    val editingMember = editMember
    if (showEditDialog && editingMember != null) key(channel.id, editingMember.ed25519) {
        MemberEditDialog(
            member = editingMember,
            isOwner = channelOwner,
            onDismiss = { showEditDialog = false },
            onSave = { nick, admin, supernode, moder, storage, verified, deleted ->
                if (nick != editingMember.name) viewModel.setMemberNick(channel.id, editingMember.ed25519, nick)
                if (admin != editingMember.isAdmin || supernode != editingMember.isSuper || moder != editingMember.isModer ||
                    storage != editingMember.isStorage || verified != editingMember.isVerified || deleted != editingMember.isDeleted)
                    viewModel.setMemberFlags(channel.id, editingMember.ed25519, admin, supernode, moder, storage, verified, deleted)
                showEditDialog = false
            },
            canDisconnect = devices.member && !devices.leaving,
            onDeviceAction = { DevgroupRepository.command(4) },
            onTransferAdmin = {
                viewModel.transferAdmin(channel.id, editingMember.ed25519)
                showEditDialog = false
            }
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
private fun MemberListItem(
    member: ChatMember,
    avatarMember: ChatMember = member,
    groupName: String? = null,
    expanded: Boolean = false,
    groupChild: Boolean = false,
    lastGroupChild: Boolean = false,
    actionsEnabled: Boolean = true,
    radioEnabled: Boolean = member.radioActive,
    phoneStatusKnown: Boolean = member.phoneStatusKnown,
    phoneBusy: Boolean = member.phoneBusy,
    onToggleGroup: () -> Unit = {},
    group: String,
    speaking: Boolean,
    isSelected: Boolean = false,
    onClick: () -> Unit,
    onCall: () -> Unit,
    onRadioCall: () -> Unit,
    onRadioMute: () -> Unit,
    onMyDevice: (() -> Unit)? = null,
    onFinishPairing: (() -> Unit)? = null,
    onClipboard: (() -> Unit)? = null
) {
    val header = groupName != null
    var menuExpanded by remember(member.ed25519, group, onMyDevice != null, onFinishPairing != null, onClipboard != null) { mutableStateOf(false) }
    val onlineColor = when {
        member.connecting -> Color(0xFFFFEB3B)  // yellow: connecting
        member.online -> Color(0xFF81C784)  // green: reachable (direct/indirect/relay)
        else -> Color(0xFFAEBCC8)  // gray: offline
    }
    val bgColor = if (isSelected) MaterialTheme.colorScheme.primaryContainer else Color.Transparent
    val branchColor = MaterialTheme.colorScheme.outlineVariant

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .combinedClickable(onClick = onClick, onLongClickLabel = stringResource(R.string.options),
                onLongClick = if (onMyDevice != null || onFinishPairing != null) ({ onClick(); menuExpanded = true }) else null),
        color = bgColor,
        tonalElevation = 0.dp
    ) {
        Row(
            modifier = Modifier.drawBehind {
                val rtl = layoutDirection == LayoutDirection.Rtl
                val x = if (rtl) size.width - 28.dp.toPx() else 28.dp.toPx()
                val endX = if (rtl) size.width - 36.dp.toPx() else 36.dp.toPx()
                val midY = size.height / 2
                val stroke = 1.dp.toPx()
                if (header && expanded) drawLine(branchColor, Offset(x, midY), Offset(x, size.height), stroke)
                if (groupChild) {
                    drawLine(branchColor, Offset(x, 0f), Offset(x, if (lastGroupChild) midY else size.height), stroke)
                    drawLine(branchColor, Offset(x, midY), Offset(endX, midY), stroke)
                }
            }.padding(horizontal = 16.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            CompositionLocalProvider(LocalMinimumInteractiveComponentEnforcement provides false) {
                if (header) IconButton(onClick = onToggleGroup, modifier = Modifier.size(width = 24.dp, height = 48.dp)) {
                    Icon(if (expanded) Icons.Filled.Remove else Icons.Filled.Add,
                        stringResource(if (expanded) R.string.devgroup_collapse else R.string.devgroup_expand), modifier = Modifier.size(18.dp))
                } else Spacer(Modifier.width(24.dp))
            }
            Box {
                UserAvatar(group, avatarMember.ed25519, avatarMember.name)
                if (member.isSelf && !header) Text("✓", modifier = Modifier.align(Alignment.BottomEnd),
                    fontSize = 12.sp, color = Color(0xFF81C784))
                else Box(Modifier.align(Alignment.BottomEnd).size(10.dp).clip(CircleShape).background(onlineColor))
            }
            Spacer(Modifier.width(10.dp))

            Column(modifier = Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    if (!header && member.isSuper) {
                        Icon(Icons.Filled.Bolt, stringResource(R.string.supernode), tint = Color(0xFFFFC107), modifier = Modifier.size(16.dp))
                        Spacer(Modifier.width(2.dp))
                    }
                    if (!header && member.isAdmin) {
                        Icon(Icons.Filled.Star, stringResource(R.string.admin), tint = Color(0xFFFFD700), modifier = Modifier.size(16.dp))
                        Spacer(Modifier.width(2.dp))
                    }
                    if (!header && member.isModer) {
                        Icon(Icons.Filled.Shield, stringResource(R.string.moderator), tint = Color(0xFF79B8FF), modifier = Modifier.size(16.dp))
                        Spacer(Modifier.width(2.dp))
                    }
                    if (!header && member.isStorage) {
                        Icon(Icons.Filled.Storage, stringResource(R.string.storage), tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(16.dp))
                        Spacer(Modifier.width(2.dp))
                    }
                    if (!header && member.protocolState == 2) {
                        Icon(Icons.Filled.Warning, stringResource(R.string.group_protocol_incompatible,
                            member.protocolLocal, member.protocolRemote), tint = Color(0xFFFFC107), modifier = Modifier.size(16.dp))
                        Spacer(Modifier.width(2.dp))
                    }
                    if (!header && !member.isVerified) {
                        Icon(Icons.Filled.Close, stringResource(R.string.not_verified), tint = Color.Red, modifier = Modifier.size(16.dp))
                        Spacer(Modifier.width(2.dp))
                    }
                    Text(
                        text = groupName ?: member.name,
                        fontWeight = if (header) FontWeight.Bold else FontWeight.Medium,
                        fontSize = 15.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        textDecoration = if (!header && member.isDeleted) TextDecoration.LineThrough else TextDecoration.None,
                        color = if (member.isSelf) Color(0xFF81C784) else Color.Unspecified,
                        modifier = Modifier.weight(1f)
                    )
                }
                val connectionLabel = when {
                    member.isSelf -> stringResource(R.string.self)
                    member.directUp -> "D" + if (member.rtt > 0) stringResource(R.string.rtt_suffix, member.rtt / 10) else ""
                    member.bgpUp && !member.indirectUp -> "B" + if (member.bgpRtt > 0) stringResource(R.string.rtt_suffix, member.bgpRtt / 10) else ""
                    member.indirectUp -> stringResource(R.string.indirect_up) + if (member.rtt > 0) stringResource(R.string.rtt_suffix, member.rtt / 10) else ""
                    member.connecting -> stringResource(R.string.connecting)
                    else -> stringResource(R.string.offline)
                }
                Text(
                    text = if (header && actionsEnabled) member.name + " · " + connectionLabel else connectionLabel, fontSize = 11.sp,
                    color = if (member.isSelf || member.online) Color(0xFF81C784) else Color(0xFFAEBCC8)
                )
            }

            if (onMyDevice != null || onFinishPairing != null || onClipboard != null) Box {
                IconButton(onClick = { onClick(); menuExpanded = true }, modifier = Modifier.size(48.dp)) {
                    Icon(Icons.Filled.MoreVert, stringResource(R.string.options))
                }
                DropdownMenu(expanded = menuExpanded, onDismissRequest = { menuExpanded = false }) {
                    if (onClipboard != null) DropdownMenuItem(text = { Text(stringResource(R.string.clipboard_send_member)) },
                        onClick = { menuExpanded = false; onClipboard() })
                    if (onMyDevice != null) DropdownMenuItem(text = { Text(stringResource(R.string.devgroup_this_is_my_device)) },
                        onClick = { menuExpanded = false; onMyDevice() })
                    if (onFinishPairing != null) DropdownMenuItem(text = { Text(stringResource(R.string.devgroup_finish_pairing)) },
                        onClick = { menuExpanded = false; onFinishPairing() })
                }
            }
            val radioStatus = stringResource(if (speaking) R.string.radio_member_speaking else R.string.radio_member_silent) +
                if (member.radioMuted) " · " + stringResource(R.string.radio_member_muted) else ""
            CompositionLocalProvider(LocalMinimumInteractiveComponentEnforcement provides false) {
                val actionModifier = Modifier.size(width = 36.dp, height = 48.dp)
                if (if (header) actionsEnabled else !member.isSelf) {
                    IconButton(onClick = onRadioCall, enabled = !member.isSelf && actionsEnabled && member.online, modifier = actionModifier) {
                        val label = stringResource(R.string.radio_call_start) +
                            if (radioEnabled) " · " + stringResource(R.string.radio_active) else ""
                        PortableRadioIcon(label, tint = if (radioEnabled) Color(0xFF4CAF50) else LocalContentColor.current)
                    }
                    IconButton(onClick = onCall, enabled = !member.isSelf && actionsEnabled && member.online, modifier = actionModifier) {
                        val status = if (!member.online || !phoneStatusKnown) R.string.phone_status_unknown
                            else if (phoneBusy) R.string.phone_status_busy else R.string.phone_status_free
                        Icon(Icons.Filled.Call, stringResource(R.string.call) + " · " + stringResource(status),
                            tint = if (!member.online || !phoneStatusKnown) LocalContentColor.current
                                else if (phoneBusy) Color(0xFFE53935) else Color(0xFF4CAF50))
                    }
                }
                if (member.isSelf) {
                    Box(actionModifier, contentAlignment = Alignment.Center) {
                        RadioSpeakerIcon(speaking, false, radioStatus)
                    }
                } else {
                    IconToggleButton(checked = member.radioMuted, enabled = actionsEnabled, onCheckedChange = { onRadioMute() }, modifier = actionModifier) {
                        RadioSpeakerIcon(speaking, member.radioMuted, radioStatus + ". " +
                            stringResource(if (member.radioMuted) R.string.radio_member_listen else R.string.radio_member_mute))
                    }
                }
            }
        }
    }
}

/** Адрес без порта; IPv6 получает одну пару скобок, пустое/неизвестное значение сохраняется. */
fun formatAddr(addr: String): String {
    if (addr.isEmpty() || addr == "?") return addr
    val trimmed = addr.trim('[', ']')
    return if (':' in trimmed) "[$trimmed]" else trimmed
}

@Composable
private fun MemberDetailCard(
    detail: MemberDetail,
    memberLinks: MemberLinks,
    localSocketStates: List<LocalSocketState>,
    isSelf: Boolean = false,
    canEdit: Boolean,
    deviceActionEnabled: Boolean,
    onDeviceAction: () -> Unit,
    onFiles: () -> Unit,
    onInvite: () -> Unit,
    onMessage: () -> Unit,
    onConnect: () -> Unit,
    onEdit: () -> Unit
) {
    LaunchedEffect(detail.channelId, detail.ed25519, detail.addresses, detail.connectionOrder, isSelf) {
        LogManager.addLog("DEBUG", "Members", "Member detail: channel=${detail.channelId} peer=${detail.ed25519} self=$isSelf " +
            "connection_order=${detail.connectionOrder} addresses=${detail.addresses.size} endpoints=" + detail.addresses.joinToString { a ->
                "sock${a.socketId} ${a.protocol} ${formatAddr(a.address)}:${a.port} type=${a.addrType}"
            })
    }
    // У каждого участника своя прокрутка: прежний offset не должен прятать заголовок нового.
    val scrollState = key(detail.channelId, detail.ed25519) { rememberScrollState() }
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(8.dp)
            .heightIn(max = LocalConfiguration.current.screenHeightDp.dp / 2),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surface,
            contentColor = MaterialTheme.colorScheme.onSurface
        ),
        elevation = CardDefaults.cardElevation(defaultElevation = 4.dp)
    ) {
        Column(modifier = Modifier.verticalScroll(scrollState).padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                UserAvatar(detail.channelId, detail.ed25519, detail.name)
                Spacer(Modifier.width(8.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        val dotColor = if (detail.online) Color(0xFF81C784) else Color(0xFFAEBCC8)
                        Box(
                            modifier = Modifier
                                .size(10.dp)
                                .clip(CircleShape)
                                .background(dotColor)
                        )
                        Spacer(Modifier.width(8.dp))
                        Text(
                            detail.name, fontWeight = FontWeight.Bold, fontSize = 15.sp, lineHeight = 18.sp,
                            maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f)
                        )
                        if (isSelf) {
                            Spacer(Modifier.width(4.dp))
                            Text("✓", fontSize = 12.sp, color = Color(0xFF81C784))
                        }
                    }

                    if (!isSelf) {
                        Spacer(Modifier.height(2.dp))
                        if (detail.directPresence) {
                            Text(stringResource(R.string.direct_state, if (detail.directUp) "UP" else "DOWN") +
                                 (if (detail.directUp && detail.bestRtt > 0) stringResource(R.string.rtt_spaced, detail.bestRtt / 10) else ""),
                                 fontSize = 13.sp, lineHeight = 16.sp, color = MaterialTheme.colorScheme.onSurface)
                        }
                        if (detail.indirectPresence) {
                            Text(stringResource(R.string.indirect_state, if (detail.indirectUp) "UP" else "DOWN"),
                                 fontSize = 13.sp, lineHeight = 16.sp, color = MaterialTheme.colorScheme.onSurface)
                        }
                        if (detail.bgpUp && detail.bgpNodes > 0) {
                            Text(pluralStringResource(R.plurals.bgp_nodes, detail.bgpNodes, detail.bgpNodes) +
                                 (if (detail.bestRtt > 0) stringResource(R.string.rtt_spaced, detail.bestRtt / 10) else ""),
                                 fontSize = 13.sp, lineHeight = 16.sp, color = MaterialTheme.colorScheme.onSurface)
                        }
                        if (!detail.directPresence && !detail.indirectPresence && !detail.bgpPresence) {
                            Text(stringResource(R.string.offline), fontSize = 13.sp, lineHeight = 16.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    val inviter = when (detail.inviterId) {
                        null -> stringResource(R.string.member_inviter_unknown)
                        "group" -> stringResource(R.string.member_inviter_root)
                        else -> {
                            val id = "[${detail.inviterId}]"
                            if (detail.inviterName.isNotEmpty()) "${detail.inviterName} $id" else id
                        }
                    }
                    Text(stringResource(R.string.member_inviter, inviter), fontSize = 13.sp, lineHeight = 16.sp,
                         color = MaterialTheme.colorScheme.onSurface)
                }
            }

            // Кнопки не отнимают ширину у имени и инвайтера, особенно на узком экране.
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                if (!isSelf) TextButton(onClick = onConnect, enabled = !detail.directUp, modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.connect))
                }
                TextButton(onClick = onEdit, enabled = canEdit, modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.edit))
                }
                TextButton(onClick = onFiles, modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.files_member))
                }
            }

            if (isSelf) TextButton(onClick = onDeviceAction, enabled = deviceActionEnabled) {
                Text(stringResource(R.string.devgroup_disconnect))
            }
            if (!isSelf) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    TextButton(onClick = onMessage) {
                        Icon(Icons.AutoMirrored.Filled.Message, stringResource(R.string.message))
                        Spacer(Modifier.width(4.dp))
                        Text(stringResource(R.string.message))
                    }
                    TextButton(onClick = onInvite) {
                        Icon(Icons.Filled.Add, stringResource(R.string.invite_to_group))
                        Spacer(Modifier.width(4.dp))
                        Text(stringResource(R.string.invite_to_group))
                    }
                }
            }
            Spacer(Modifier.height(4.dp))
            val headerParts = mutableListOf(detail.ed25519)
            if (detail.lastSeen > 0) {
                val sec = (System.currentTimeMillis() / 1000 - detail.lastSeen).coerceAtLeast(0)
                val ago = when {
                    sec < 60 -> stringResource(R.string.seconds_short, sec)
                    sec < 3600 -> stringResource(R.string.minutes_short, sec / 60)
                    sec < 86400 -> stringResource(R.string.hours_short, sec / 3600)
                    else -> stringResource(R.string.days_short, sec / 86400)
                }
                headerParts.add(stringResource(R.string.last_seen_ago, ago))
            }
            if (detail.created > 0) {
                val sec = (System.currentTimeMillis() / 1000 - detail.created).coerceAtLeast(0)
                val ago = when {
                    sec < 604800 -> stringResource(R.string.days_short, sec / 86400)
                    else -> stringResource(R.string.months_short, sec / 2592000)
                }
                headerParts.add(stringResource(R.string.created_ago, ago))
            }
            Text(
                text = headerParts.joinToString(" · "),
                fontFamily = FontFamily.Monospace,
                fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            Spacer(Modifier.height(6.dp))
            Text(stringResource(R.string.member_connection_order), fontSize = 13.sp, fontWeight = FontWeight.Medium)
            val order = detail.connectionOrder
            val letters = order.takeWhile { it in "TUR" }
            val mode = order.drop(letters.length).firstOrNull()
            val policy = if (letters.isEmpty() || mode !in listOf('1', '2', '3')) stringResource(R.string.member_connection_unknown) else {
                val protocols = letters.map { when (it) { 'T' -> "TCP"; 'U' -> "UDP"; else -> "REALITY" } }.joinToString(" → ")
                val behavior = when (mode) {
                    '1' -> stringResource(R.string.member_connection_first)
                    '2' -> stringResource(R.string.member_connection_sequential, order.substringAfter('-').toIntOrNull() ?: 0)
                    else -> stringResource(R.string.member_connection_parallel)
                }
                "$protocols\n$behavior"
            }
            Text(policy, fontSize = 13.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)

            if (isSelf) {
                if (localSocketStates.isNotEmpty()) {
                    Spacer(Modifier.height(6.dp))
                    Text(stringResource(R.string.listen_sockets), fontSize = 13.sp, fontWeight = FontWeight.Medium)
                    localSocketStates.forEach { sk ->
                        val dot = if (sk.state == 2) Color(0xFF81C784) else Color(0xFFAEBCC8)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Box(Modifier.size(8.dp).clip(CircleShape).background(dot))
                            Spacer(Modifier.width(6.dp))
                            Text(
                                text = "[${sk.linksUp}/${sk.linksDown}]  ${sk.protocol}  ${sk.ifName}  ${formatAddr(sk.bindIp)}:${sk.port}",
                                fontFamily = FontFamily.Monospace,
                                fontSize = 13.sp,
                                color = MaterialTheme.colorScheme.onSurface
                            )
                        }
                        LocalSocketNatInfo(sk)
                    }
                }
            } else {
                if (detail.addresses.isNotEmpty()) {
                    Spacer(Modifier.height(6.dp))
                    Text(stringResource(R.string.addresses), fontSize = 13.sp, fontWeight = FontWeight.Medium)
                    detail.addresses.forEachIndexed { idx, a ->
                        Text(
                            text = "${formatAddr(a.address)}:${a.port}",
                            fontFamily = FontFamily.Monospace,
                            fontSize = 13.sp,
                            lineHeight = 15.sp,
                            color = MaterialTheme.colorScheme.onSurface
                        )
                        Text("[sock${a.socketId}] ${a.protocol} · ${stringResource(a.natLabelRes)}",
                            fontSize = 12.sp, lineHeight = 15.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        if (idx < detail.addresses.lastIndex) Spacer(Modifier.height(8.dp))
                    }
                }

                if (memberLinks.links.isNotEmpty()) {
                    Spacer(Modifier.height(6.dp))
                    Text(stringResource(R.string.connections), fontSize = 13.sp, fontWeight = FontWeight.Medium)
                    memberLinks.links.forEachIndexed { idx, lk ->
                        val dcol = if (lk.linkState == 3 && lk.linkStatus == 1) Color(0xFF81C784) else Color(0xFFFFB4AB)
                        val arrow = when (lk.direction) {
                            LinkDirection.INCOMING -> "◀"
                            LinkDirection.OUTGOING -> "▶"
                            LinkDirection.UNKNOWN -> ""
                        }
                        val arrowColor = if (lk.direction == LinkDirection.INCOMING) Color(0xFFFF9800) else Color(0xFF79B8FF)
                        Column {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Box(Modifier.size(8.dp).clip(CircleShape).background(dcol))
                                Spacer(Modifier.width(6.dp))
                                Text(lk.protocol, fontFamily = FontFamily.Monospace, fontSize = 12.sp,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                                if (lk.hasIp) {
                                    Spacer(Modifier.width(6.dp))
                                    Text("${formatAddr(lk.localAddr)}:${lk.localPort}",
                                        modifier = Modifier.weight(1f), textAlign = TextAlign.Start,
                                        fontFamily = FontFamily.Monospace, fontSize = 13.sp, lineHeight = 15.sp,
                                        color = MaterialTheme.colorScheme.onSurface)
                                }
                                if (arrow.isNotEmpty()) {
                                    Spacer(Modifier.width(4.dp))
                                    Text(arrow, fontWeight = FontWeight.Bold, color = arrowColor, fontSize = 16.sp)
                                }
                                if (lk.hasIp) {
                                    Spacer(Modifier.width(4.dp))
                                    Text("${formatAddr(lk.remoteAddr)}:${lk.remotePort}",
                                        modifier = Modifier.weight(1f), textAlign = TextAlign.Start,
                                        fontFamily = FontFamily.Monospace, fontSize = 13.sp, lineHeight = 15.sp,
                                        color = MaterialTheme.colorScheme.onSurface)
                                }
                            }
                            val cleanSn = com.utun.chat.data.cleanSocketName(lk.socketName)
                            if (cleanSn.isNotEmpty()) {
                                Row {
                                    Spacer(Modifier.width(14.dp))
                                    Text(cleanSn, fontSize = 12.sp, lineHeight = 14.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                            }
                        }
                        if (idx < memberLinks.links.lastIndex) Spacer(Modifier.height(8.dp))
                    }
                }


            }
        }
    }
}

@Composable
private fun MemberEditDialog(
    member: ChatMember,
    isOwner: Boolean,
    onDismiss: () -> Unit,
    onSave: (nick: String, admin: Boolean, supernode: Boolean, moder: Boolean, storage: Boolean,
             verified: Boolean, deleted: Boolean) -> Unit,
    canDisconnect: Boolean,
    onDeviceAction: () -> Unit,
    onTransferAdmin: () -> Unit
) {
    var nick by remember { mutableStateOf(member.name) }
    var admin by remember { mutableStateOf(member.isAdmin) }
    var supernode by remember { mutableStateOf(member.isSuper) }
    var moder by remember { mutableStateOf(member.isModer) }
    var storage by remember { mutableStateOf(member.isStorage) }
    var verified by remember { mutableStateOf(member.isVerified) }
    var deleted by remember { mutableStateOf(member.isDeleted) }
    var showTransferConfirm by remember { mutableStateOf(false) }
    var failed by remember { mutableStateOf(false) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.edit_member, member.name)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                OutlinedTextField(
                    value = nick,
                    onValueChange = { nick = it },
                    label = { Text(stringResource(R.string.nickname)) },
                    enabled = !member.isSelf,
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true
                )
                if (member.isSelf) TextButton(onClick = onDeviceAction, enabled = !member.isDeleted && canDisconnect) {
                    Text(stringResource(R.string.devgroup_disconnect))
                }
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = admin, onCheckedChange = { admin = it }, enabled = isOwner)
                    Text(stringResource(R.string.admin), modifier = Modifier.clickable(enabled = isOwner) { admin = !admin })
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = supernode, onCheckedChange = { supernode = it }, enabled = isOwner)
                    Text(stringResource(R.string.supernode), modifier = Modifier.clickable(enabled = isOwner) { supernode = !supernode })
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = moder, onCheckedChange = { moder = it }, enabled = isOwner)
                    Text(stringResource(R.string.moderator), modifier = Modifier.clickable(enabled = isOwner) { moder = !moder })
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = storage, onCheckedChange = { storage = it }, enabled = isOwner)
                    Text(stringResource(R.string.storage), modifier = Modifier.clickable(enabled = isOwner) { storage = !storage })
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = verified, onCheckedChange = { verified = it }, enabled = isOwner)
                    Text(stringResource(R.string.verified), modifier = Modifier.clickable(enabled = isOwner) { verified = !verified })
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = deleted, onCheckedChange = { deleted = it }, enabled = isOwner)
                    Text(stringResource(R.string.deleted), modifier = Modifier.clickable(enabled = isOwner) { deleted = !deleted })
                }
                if (isOwner && !member.isSelf) {
                    Spacer(Modifier.height(12.dp))
                    OutlinedButton(
                        onClick = { showTransferConfirm = true },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text(stringResource(R.string.transfer_admin_rights))
                    }
                } else if (!isOwner) {
                    Spacer(Modifier.height(4.dp))
                    Text(
                        stringResource(R.string.admin_requires_key),
                        fontSize = 11.sp,
                        color = Color(0xFFAEBCC8)
                    )
                }
                if (failed) Text(stringResource(R.string.member_update_failed), color = MaterialTheme.colorScheme.error)
            }
        },
        confirmButton = {
            TextButton(onClick = {
                try { failed = false; onSave(nick, admin, supernode, moder, storage, verified, deleted) } catch (e: Exception) {
                    failed = true; LogManager.addLog("ERROR", "Members", "Member changes submission failed peer=${member.ed25519}: $e")
                }
            }) { Text(stringResource(R.string.save)) }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) }
        }
    )

    if (showTransferConfirm) {
        AlertDialog(
            onDismissRequest = { showTransferConfirm = false },
            title = { Text(stringResource(R.string.transfer_admin_rights)) },
            text = {
                Text(
                    stringResource(R.string.transfer_key_confirm, member.name)
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    showTransferConfirm = false
                    try { failed = false; onTransferAdmin() } catch (e: Exception) {
                        failed = true; LogManager.addLog("ERROR", "Members", "Admin transfer submission failed peer=${member.ed25519}: $e")
                    }
                }) { Text(stringResource(R.string.send)) }
            },
            dismissButton = {
                TextButton(onClick = { showTransferConfirm = false }) { Text(stringResource(R.string.cancel)) }
            }
        )
    }
}
