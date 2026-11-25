package com.utun.chat.ui.screens

import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.res.stringResource
import com.utun.chat.R
import com.utun.chat.data.LogManager

/** Явное открытие URL: чтение буфера заполняет поле, переход выполняется только кнопкой. */
@Composable
fun OpenLinkDialog(onOpen: (String) -> Unit, onDismiss: () -> Unit) {
    val clipboard = LocalClipboardManager.current
    var text by rememberSaveable { mutableStateOf("") }
    var pasteFailed by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        try { if (text.isEmpty()) text = clipboard.getText()?.text?.trim().orEmpty() }
        catch (e: Exception) { pasteFailed = true; LogManager.addLog("ERROR", "LINK", "Clipboard read failed: ${e.javaClass.simpleName}") }
    }
    AlertDialog(onDismissRequest = onDismiss, title = { Text(stringResource(R.string.open_link)) },
        text = { OutlinedTextField(value = text, onValueChange = { text = it }, label = { Text("utun://…") },
            isError = pasteFailed, supportingText = { if (pasteFailed) Text(stringResource(R.string.invite_paste_failed)) }) },
        confirmButton = { TextButton(enabled = text.isNotBlank(), onClick = { onOpen(text.trim()) }) {
            Text(stringResource(R.string.open_link))
        } }, dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) } })
}
