package com.utun.chat.ui.screens

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ContentPaste
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.unit.dp
import com.utun.chat.data.InviteData
import com.utun.chat.data.InviteLink
import com.utun.chat.data.LogManager
import kotlinx.coroutines.CancellationException

/** Автоматически передаёт валидную ссылку из ввода/QR/буфера в onConnect; сетевой результат показывает вызывающий экран.
 * Callback должен принять запрос и закрыть диалог. Ошибка запуска разрешает повторить ту же ссылку. Секреты ссылки не логируются. */
@Composable
fun JoinChannelDialog(onConnect: (InviteData) -> Unit, onDismiss: () -> Unit) {
    val clipboard = LocalClipboardManager.current
    var link by rememberSaveable { mutableStateOf("") }
    var error by remember { mutableStateOf<Int?>(null) }
    var connecting by remember { mutableStateOf(false) }
    var retry by remember { mutableIntStateOf(0) }
    val connect by rememberUpdatedState(onConnect)

    fun paste(onlyInvite: Boolean) {
        try {
            val text = clipboard.getText()?.text?.trim().orEmpty()
            if (!onlyInvite || text.startsWith("utun://")) {
                LogManager.addLog("INFO", "INVITE", "Invite input read from clipboard")
                link = text
            }
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "INVITE", "Clipboard read failed: $e")
            error = R.string.invite_paste_failed
        }
    }

    LaunchedEffect(Unit) {
        if (link.isEmpty()) paste(onlyInvite = true)
    }

    LaunchedEffect(link, retry) {
        val text = link.trim()
        if (text.isEmpty() || connecting) return@LaunchedEffect
        error = null
        try {
            val invite = InviteLink.decode(text)
            if (invite == null) {
                LogManager.addLog("WARN", "INVITE", "Invalid invite link in join dialog")
                error = R.string.invalid_invite_link
            } else {
                connecting = true
                val peer = com.utun.chat.data.MemberFilesProtocol.addressHex(invite.pubkey)
                LogManager.addLog("INFO", "INVITE", "connect: x25519=$peer addrs=${invite.addrs.size}")
                connect(invite)
            }
        } catch (e: CancellationException) {
            connecting = false
            throw e
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "INVITE", "Invite connection request failed: ${e.javaClass.simpleName}")
            connecting = false
            error = R.string.invite_connect_failed
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.join_channel)) },
        text = {
            Column(
                modifier = Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                if (!connecting) {
                    QrScannerPreview(
                        modifier = Modifier.fillMaxWidth().aspectRatio(3f / 4f),
                        onResult = { link = it }
                    )
                }
                OutlinedTextField(
                    value = link,
                    onValueChange = { error = null; link = it },
                    label = { Text(stringResource(R.string.invite_link)) },
                    placeholder = { Text("utun://...") },
                    trailingIcon = {
                        IconButton(onClick = { paste(onlyInvite = false) }, enabled = !connecting) {
                            Icon(Icons.Filled.ContentPaste, stringResource(R.string.paste_invite_link))
                        }
                    },
                    singleLine = true,
                    enabled = !connecting,
                    isError = error != null,
                    modifier = Modifier.fillMaxWidth()
                )
                error?.let { Text(stringResource(it), color = MaterialTheme.colorScheme.error) }
                if (error == R.string.invite_connect_failed) {
                    TextButton(onClick = { retry++ }) { Text(stringResource(R.string.retry)) }
                }
                if (connecting) Text(stringResource(R.string.connecting_peer), color = MaterialTheme.colorScheme.primary)
            }
        },
        confirmButton = {},
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } }
    )
}
