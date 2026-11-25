package com.utun.chat.ui.components

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.utun.chat.AppEventHandler
import com.utun.chat.R
import com.utun.chat.data.LogManager
import com.utun.chat.data.NativeLib
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.first
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicLong

private val renameSequence = AtomicLong(0)

/** Подписка до enqueue; ответ проверяется по запросу и группе. Timeout не означает rollback команды. */
internal suspend fun groupInfoRequest(group: String, name: String?, token: String?, clipboardMembers: Int = 0): JSONObject = coroutineScope {
    val id = renameSequence.incrementAndGet()
    val response = async(start = CoroutineStart.UNDISPATCHED) {
        withTimeout(15000) {
            AppEventHandler.events.first { (type, bytes) ->
                type == 76 && bytes != null && bytes.size >= 42 &&
                    ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).let { it.long == id && bytes.copyOfRange(8, 40).joinToString("") { b -> "%02x".format(b.toInt() and 255) } == group }
            }.second!!
        }
    }
    if (!NativeLib.nativeGroupInfo(id, group, name, token, clipboardMembers)) {
        response.cancel(); throw IllegalStateException("group description enqueue rejected")
    }
    val bytes = response.await()
    JSONObject(bytes.copyOfRange(40, bytes.size).toString(Charsets.UTF_8))
}

/** Переименование сохраняет все дополнительные JSON-опции; права и конфликт проверяет ядро. */
@Composable
fun GroupRenameDialog(channel: String, onDismiss: () -> Unit) {
    val scope = rememberCoroutineScope()
    var desired by rememberSaveable(channel) { mutableStateOf("") }
    var current by remember(channel) { mutableStateOf("") }
    var clipboardMembers by rememberSaveable(channel) { mutableStateOf(false) }
    var currentClipboard by remember(channel) { mutableStateOf(false) }
    var token by remember(channel) { mutableStateOf<String?>(null) }
    var loaded by remember(channel) { mutableStateOf(false) }
    var canEdit by remember(channel) { mutableStateOf(false) }
    var busy by remember(channel) { mutableStateOf(true) }
    var status by remember(channel) { mutableStateOf<Int?>(null) }
    fun request(apply: Boolean) {
        busy = true
        scope.launch {
            try {
                var result = groupInfoRequest(channel, if (apply) desired else null, if (apply) token else null,
                    if (apply) (if (clipboardMembers) 2 else 1) else 0)
                if (!result.optBoolean("ok")) {
                    val conflict = result.optString("error") == "conflict"
                    status = if (conflict) R.string.group_rename_conflict else R.string.group_rename_failed
                    token = null
                    if (!conflict) return@launch
                    result = groupInfoRequest(channel, null, null)
                    if (!result.optBoolean("ok")) return@launch
                } else if (apply) status = R.string.group_rename_saved
                current = result.getJSONObject("description").getString("name")
                currentClipboard = result.getJSONObject("description").optBoolean("clipboard_members")
                if (!loaded) clipboardMembers = currentClipboard
                if (!loaded && desired.isEmpty()) desired = current
                loaded = true; token = result.getString("token"); canEdit = result.getBoolean("can_edit")
                if (!canEdit) status = R.string.group_rename_key_required
            } catch (e: CancellationException) {
                if (e !is TimeoutCancellationException) throw e
                LogManager.addLog("WARN", "GroupRename", "Command timed out channel=$channel")
                status = R.string.group_policy_timeout; token = null
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "GroupRename", "Command failed channel=$channel: $e")
                status = R.string.group_rename_failed; token = null
            } finally { busy = false }
        }
    }
    LaunchedEffect(channel) { request(false) }
    val bytes = desired.toByteArray(Charsets.UTF_8)
    AlertDialog(onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.group_properties)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (loaded) Text(stringResource(R.string.group_rename_current, current))
                OutlinedTextField(value = desired, onValueChange = { desired = it }, singleLine = true,
                    enabled = !busy && canEdit, label = { Text(stringResource(R.string.group_rename_name)) },
                    supportingText = { Text("${bytes.size}/127") })
                Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                    Checkbox(checked = clipboardMembers, onCheckedChange = { clipboardMembers = it }, enabled = !busy && canEdit)
                    Text(stringResource(R.string.clipboard_members_allowed))
                }
                if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                status?.let { Text(stringResource(it)) }
                if (!busy && token == null) TextButton(onClick = { request(false) }) { Text(stringResource(R.string.group_policy_refresh)) }
            }
        },
        confirmButton = { TextButton(enabled = !busy && canEdit && token != null && bytes.isNotEmpty() && bytes.size <= 127 &&
            '\u0000' !in desired && (desired != current || clipboardMembers != currentClipboard),
            onClick = { request(true) }) { Text(stringResource(R.string.group_policy_apply)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) } })
}
