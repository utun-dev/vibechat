package com.utun.chat.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.utun.chat.R
import com.utun.chat.data.*
import com.utun.chat.ui.components.PeerStatusDot
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

/** Панель GROUPS: опрос живёт ровно столько, сколько открыта панель выбранного поколения. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DmMembersSheet(conv: DmConversation, onDismiss: () -> Unit) {
    val target = ConversationTarget.Pm(conv.convId, conv.generation)
    var members by remember(target) { mutableStateOf<List<DmMember>>(emptyList()) }
    var unavailable by remember(target) { mutableStateOf(false) }
    LaunchedEffect(target, conv.closed) {
        if (conv.closed) { members = emptyList(); return@LaunchedEffect }
        while (true) {
            try {
                val snapshot = withContext(Dispatchers.IO) {
                    parseDmMembers(checkNotNull(NativeLib.dmMembers(target.id, target.generation)), target)
                }
                if (snapshot != members) LogManager.addLog("DEBUG", "PM", "Members updated conv=${target.id} count=${snapshot.size}")
                members = snapshot; unavailable = false
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                LogManager.addLog("WARN", "PM", "Members unavailable conv=${target.id}: $e"); unavailable = true
            }
            delay(2000)
        }
    }
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Text(stringResource(R.string.dm_members_title), style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(16.dp))
        if (unavailable) Text(stringResource(R.string.dm_members_unavailable), modifier = Modifier.padding(horizontal = 16.dp))
        LazyColumn(Modifier.fillMaxWidth().heightIn(max = 480.dp)) {
            for (side in 0..1) {
                item(key = "side:$side") {
                    HorizontalDivider()
                    Text(stringResource(if (side == 0) R.string.dm_my_devices else R.string.dm_peer_devices),
                        fontWeight = FontWeight.Bold, modifier = Modifier.padding(16.dp))
                }
                items(members.filter { it.side == side }, key = { "node:${it.member.ed25519}" }) { entry ->
                    val member = entry.member
                    Row(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                        PeerStatusDot(member.online)
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f)) {
                            Text(member.name, fontWeight = if (member.isSelf) FontWeight.Bold else FontWeight.Normal)
                            Text(member.ed25519, style = MaterialTheme.typography.bodySmall)
                        }
                        Text(stringResource(if (member.online) R.string.online else R.string.offline))
                    }
                }
            }
        }
        Spacer(Modifier.height(16.dp))
    }
}
