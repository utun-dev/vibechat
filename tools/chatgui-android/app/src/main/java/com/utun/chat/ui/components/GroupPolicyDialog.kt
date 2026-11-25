package com.utun.chat.ui.components

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.utun.chat.AppEventHandler
import com.utun.chat.R
import com.utun.chat.data.LogManager
import com.utun.chat.data.NativeLib
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.first
import org.json.JSONArray
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicLong

private val policySequence = AtomicLong(0)

/** Подписка создаётся до enqueue; ID исключает ответ предыдущего окна/запроса. */
private suspend fun policyRequest(group: String, all: Boolean, token: String?, keep: Set<String>): JSONObject = coroutineScope {
    val id = policySequence.incrementAndGet()
    val response = async(start = CoroutineStart.UNDISPATCHED) {
        withTimeout(15000) {
            AppEventHandler.events.first { (type, bytes) ->
                type == 74 && bytes != null && bytes.size >= 42 &&
                    ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).long == id
            }.second!!
        }
    }
    if (!NativeLib.nativeGroupPolicy(id, group, all, token, JSONArray(keep.toList()).toString())) {
        response.cancel(); throw IllegalStateException("policy enqueue rejected")
    }
    val bytes = response.await()
    JSONObject(bytes.copyOfRange(40, bytes.size).toString(Charsets.UTF_8))
}

/** Ядро строит список тем же алгоритмом, которым применяет допуск. Изменения требуют отдельного Apply. */
@Composable
fun GroupPolicyDialog(channel: String, onDismiss: () -> Unit) {
    val scope = rememberCoroutineScope()
    var snapshot by remember { mutableStateOf<JSONObject?>(null) }
    var desired by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(true) }
    var selected by remember { mutableStateOf(setOf<String>()) }
    var status by remember { mutableStateOf<Int?>(null) }
    val current = snapshot?.optBoolean("all_members") ?: false
    val canEdit = snapshot?.optBoolean("can_edit") ?: false
    fun request(all: Boolean, apply: Boolean) {
        busy = true; status = null
        scope.launch {
            try {
                val result = policyRequest(channel, all, if (apply) snapshot?.getString("token") else null, if (apply) selected else emptySet())
                if (!result.optBoolean("ok")) {
                    status = when (result.optString("error")) {
                        "conflict" -> R.string.group_policy_conflict
                        "sync_pending" -> R.string.group_policy_sync
                        else -> R.string.group_policy_failed
                    }
                    if (result.optString("error") == "conflict") {
                        val refreshed = policyRequest(channel, all, null, emptySet())
                        snapshot = refreshed.takeIf { it.optBoolean("ok") }; selected = emptySet()
                    } else snapshot = null
                } else {
                    if (snapshot == null || apply) desired = result.getBoolean("all_members")
                    snapshot = result; selected = emptySet()
                    if (apply) status = R.string.group_policy_saved
                }
            } catch (e: CancellationException) {
                if (e !is TimeoutCancellationException) throw e
                status = R.string.group_policy_timeout; snapshot = null
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "GroupPolicy", "Request failed channel=$channel: $e")
                status = R.string.group_policy_failed; snapshot = null
            } finally { busy = false }
        }
    }
    LaunchedEffect(channel) { request(false, false) }
    val affected = if (desired || desired == current) emptyList() else {
        val array = snapshot?.optJSONArray("affected") ?: JSONArray()
        (0 until array.length()).map { array.getJSONObject(it) }
    }
    AlertDialog(onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.group_policy_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                for (all in listOf(false, true)) Row(verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(selected = desired == all, enabled = canEdit && !busy, onClick = { desired = all; request(all, false) })
                    Text(stringResource(if (all) R.string.group_policy_all else R.string.group_policy_admins))
                }
                if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                Text(stringResource(R.string.group_policy_explanation))
                if (snapshot != null && !canEdit) Text(stringResource(R.string.group_policy_owner_only))
                if (affected.isNotEmpty()) {
                    Text(stringResource(R.string.group_policy_affected, affected.size))
                    LazyColumn(Modifier.heightIn(max = 240.dp)) {
                        items(affected, key = { it.getString("node") }) { member ->
                            val node = member.getString("node")
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Checkbox(checked = node in selected, enabled = !busy && canEdit, onCheckedChange = {
                                    selected = if (it) selected + node else selected - node
                                })
                                Column {
                                    Text(member.optString("name").ifEmpty { node })
                                    Text(node, style = MaterialTheme.typography.bodySmall)
                                    Text(stringResource(R.string.group_policy_inviter, member.getString("inviter")), style = MaterialTheme.typography.bodySmall)
                                }
                            }
                        }
                    }
                }
                status?.let { Text(stringResource(it)) }
                if (!busy && snapshot == null) TextButton(onClick = { request(desired, false) }) { Text(stringResource(R.string.group_policy_refresh)) }
            }
        },
        confirmButton = { TextButton(enabled = !busy && canEdit && snapshot?.optBoolean("ok") == true && desired != current,
            onClick = { request(desired, true) }) { Text(stringResource(R.string.group_policy_apply)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) } })
}
