package com.utun.chat.ui.screens

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Color as AndroidColor
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.google.zxing.BarcodeFormat
import com.google.zxing.MultiFormatWriter
import com.google.zxing.common.BitMatrix
import com.utun.chat.data.InviteCandidate
import com.utun.chat.data.LogManager
import com.utun.chat.data.LocalDiscovery
import com.utun.chat.data.NativeLib

@Composable
private fun candidateLabel(c: InviteCandidate): String =
    c.name + when {
        c.isAuto -> stringResource(R.string.candidate_auto)
        c.isSelf -> stringResource(R.string.candidate_self)
        else -> ""
    }

/** Показ зарегистрированной ссылки/QR и отправка её устройству в LAN; генерацией и корреляцией link/pending владеет caller.
 * Первый candidate — визуальный fallback; явный выбор вызывает onSelectNode, начальный запрос делает caller.
 * При открытии запрашивает LAN-снимок и снимает только прежний терминальный результат; активная отправка переживает закрытие.
 * Результат LAN относится к сохранённому адресату, не означает JOIN. Состояние копирования сбрасывается при смене ссылки.
 * Ошибки QR оставляют текст/копирование доступными; ошибки clipboard показываются в диалоге. Содержимое ссылки не логируется. */
@Composable
fun InviteLinkDialog(
    candidates: List<InviteCandidate>,
    link: String?,
    pending: Boolean,
    onSelectNode: (String) -> Unit,
    onDismiss: () -> Unit
) {
    val context = LocalContext.current
    var copied by remember(link) { mutableStateOf(false) }
    var copyFailed by remember(link) { mutableStateOf(false) }
    var selectedNodeId by remember { mutableStateOf<String?>(null) }
    var menuOpen by remember { mutableStateOf(false) }
    val devices by LocalDiscovery.devices.collectAsState()
    val delivery by LocalDiscovery.delivery.collectAsState()
    var selectedDevice by remember { mutableStateOf<String?>(null) }
    var devicesOpen by remember { mutableStateOf(false) }
    val device = devices.firstOrNull { it.address == selectedDevice } ?: devices.firstOrNull()
    LaunchedEffect(Unit) {
        LocalDiscovery.clearDelivery()
        try {
            if (!NativeLib.requestLocalDevices()) LogManager.addLog("WARN", "INVITE", "LAN device snapshot request rejected")
        } catch (error: Exception) {
            LogManager.addLog("ERROR", "INVITE", "LAN device snapshot request failed: ${error.javaClass.simpleName}")
        }
    }

    val effectiveSelected = remember(candidates, selectedNodeId) {
        candidates.firstOrNull { it.address == selectedNodeId } ?: candidates.firstOrNull()
    }
    val qrBitmap = remember(link) { link?.let { generateQRCode(it) } }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.invite_link), fontWeight = FontWeight.Bold) },
        text = {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(stringResource(R.string.invite_window_lifetime), style = MaterialTheme.typography.bodySmall)
                Spacer(Modifier.height(8.dp))
                Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(R.string.invite_connect_via))
                    Box(modifier = Modifier.weight(1f)) {
                        OutlinedButton(onClick = { menuOpen = true }, modifier = Modifier.fillMaxWidth()) {
                            Text(
                                effectiveSelected?.let { candidateLabel(it) } ?: stringResource(R.string.select_node),
                                maxLines = 1, overflow = TextOverflow.Ellipsis
                            )
                        }
                        DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                            candidates.forEach { c ->
                                DropdownMenuItem(
                                    text = { Text(candidateLabel(c)) },
                                    onClick = {
                                        menuOpen = false
                                        if (selectedNodeId != c.address) {
                                            selectedNodeId = c.address
                                            copied = false
                                            onSelectNode(c.address)
                                        }
                                    }
                                )
                            }
                        }
                    }
                }
                Spacer(modifier = Modifier.height(8.dp))

                when {
                    pending -> Text(stringResource(R.string.invite_generating), fontFamily = FontFamily.Monospace, fontSize = 11.sp)
                    link != null -> {
                        qrBitmap?.let {
                            Image(bitmap = it.asImageBitmap(), contentDescription = stringResource(R.string.qr_code),
                                modifier = Modifier.size(200.dp))
                            Spacer(modifier = Modifier.height(8.dp))
                        }
                        Text(link, fontFamily = FontFamily.Monospace, fontSize = 11.sp,
                            maxLines = 1, overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.fillMaxWidth().padding(8.dp))
                    }
                    else -> Text(stringResource(R.string.invite_build_failed), fontFamily = FontFamily.Monospace, fontSize = 11.sp)
                }
                if (copyFailed) Text(stringResource(R.string.invite_copy_failed), color = MaterialTheme.colorScheme.error)
                if (devices.isNotEmpty()) {
                    Spacer(Modifier.height(8.dp))
                    Text(stringResource(R.string.local_invite_devices), style = MaterialTheme.typography.bodyMedium)
                    Box {
                        OutlinedButton(onClick = { devicesOpen = true }) { Text(device?.label.orEmpty(), maxLines = 1) }
                        DropdownMenu(expanded = devicesOpen, onDismissRequest = { devicesOpen = false }) {
                            devices.forEach { item ->
                                DropdownMenuItem(text = { Text(item.label) }, onClick = {
                                    selectedDevice = item.address; devicesOpen = false
                                })
                            }
                        }
                    }
                    Button(enabled = link != null && !pending && device != null && delivery?.pending != true, onClick = {
                        if (device != null && link != null) LocalDiscovery.send(device, link)
                    }) { Text(stringResource(R.string.local_invite_send)) }
                }
                delivery?.let { status ->
                    val text = stringResource(when {
                        status.pending -> R.string.local_invite_sending
                        status.forbidden -> R.string.local_invite_forbidden
                        status.received -> R.string.local_invite_delivered
                        else -> R.string.local_invite_failed
                    })
                    Text(stringResource(R.string.invite_delivery_status, status.device.label, text),
                        style = MaterialTheme.typography.bodySmall)
                }
            }
        },
        confirmButton = {
            Button(
                enabled = link != null && !pending,
                onClick = {
                    link?.let {
                        copyFailed = false; copied = false
                        try {
                            val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
                            if (clipboard == null) {
                                copyFailed = true
                                LogManager.addLog("ERROR", "INVITE", "Clipboard service unavailable")
                            } else {
                                clipboard.setPrimaryClip(ClipData.newPlainText("invite", it))
                                copied = true
                                LogManager.addLog("INFO", "INVITE", "Link copied to clipboard")
                            }
                        } catch (error: Exception) {
                            copyFailed = true
                            LogManager.addLog("ERROR", "INVITE", "Clipboard copy failed: ${error.javaClass.simpleName}")
                        }
                    }
                }
            ) { Text(stringResource(if (copied) R.string.copied else R.string.copy_link)) }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) }
        }
    )
}

/** QR 400×400 для текущей ссылки; ошибка логируется без секретных данных и оставляет владельцу текстовый fallback. */
private fun generateQRCode(text: String): Bitmap? {
    return try {
        val writer = MultiFormatWriter()
        val matrix: BitMatrix = writer.encode(text, BarcodeFormat.QR_CODE, 400, 400)
        val bitmap = Bitmap.createBitmap(400, 400, Bitmap.Config.ARGB_8888)
        for (x in 0 until 400) {
            for (y in 0 until 400) {
                bitmap.setPixel(x, y, if (matrix[x, y]) AndroidColor.BLACK else AndroidColor.WHITE)
            }
        }
        bitmap
    } catch (e: Exception) {
        LogManager.addLog("ERROR", "INVITE", "QR generation failed length=${text.length}: ${e.javaClass.simpleName}")
        null
    }
}
