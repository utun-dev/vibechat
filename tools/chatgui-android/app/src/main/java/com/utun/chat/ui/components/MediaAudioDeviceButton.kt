package com.utun.chat.ui.components

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.HeadsetMic
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.PopupProperties
import androidx.core.content.ContextCompat
import com.utun.chat.R
import com.utun.chat.data.MediaAudioController
import com.utun.chat.data.MediaAudioProfile
import com.utun.chat.data.LogManager

/** Общий профиль рации/трансляций: preferred и actual раздельны, недоступные устройства выключены.
 * expanded=null хранит transient menu state локально; иначе caller управляет им через onExpandedChange.
 * Для Bluetooth на API 31+ выбор применяется после permission ACK; отказ сохраняет прежний профиль.
 */
@Composable
fun MediaAudioDeviceButton(tint: Color = LocalContentColor.current, expanded: Boolean? = null,
                          properties: PopupProperties = PopupProperties(focusable = true), onExpandedChange: (Boolean) -> Unit = {}) {
    val state by MediaAudioController.state.collectAsState()
    val context = LocalContext.current
    var localExpanded by remember { mutableStateOf(false) }
    val menuExpanded = expanded ?: localExpanded
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) MediaAudioController.select(MediaAudioProfile.BLUETOOTH)
        else LogManager.addLog("WARN", "media_audio", "Bluetooth selection cancelled: BLUETOOTH_CONNECT denied")
    }
    val bluetoothPermission = Build.VERSION.SDK_INT < 31 ||
        ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    fun expand(value: Boolean) {
        if (expanded == null) localExpanded = value
        onExpandedChange(value)
        LogManager.addLog("DEBUG", "media_audio",
            "Device menu expanded=$value preferred=${state.preferred} active=${state.active} notice=${state.notice}")
    }
    val labels = mapOf(MediaAudioProfile.AUTO to R.string.media_audio_auto, MediaAudioProfile.PHONE to R.string.media_audio_phone,
        MediaAudioProfile.WIRED to R.string.media_audio_wired, MediaAudioProfile.BLUETOOTH to R.string.media_audio_bluetooth)
    Box {
        IconButton(onClick = { expand(!menuExpanded) }) {
            Icon(if (state.active == MediaAudioProfile.PHONE) Icons.AutoMirrored.Filled.VolumeUp else Icons.Filled.HeadsetMic,
                stringResource(R.string.media_audio_devices, stringResource(labels.getValue(state.preferred))),
                tint = if (state.notice?.isError == true) MaterialTheme.colorScheme.error else tint)
        }
        DropdownMenu(menuExpanded, properties = properties, onDismissRequest = { expand(false) }) {
            Text(stringResource(R.string.media_audio_title),
                Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp).semantics { heading() },
                style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
            HorizontalDivider()
            for ((profile, label) in labels) {
                val available = when (profile) {
                    MediaAudioProfile.WIRED -> state.wiredAvailable
                    MediaAudioProfile.BLUETOOTH -> state.bluetoothAvailable || !bluetoothPermission
                    else -> true
                }
                DropdownMenuItem(text = { Text(stringResource(label)) }, enabled = available,
                    trailingIcon = { if (state.preferred == profile) Text("✓") }, onClick = {
                        expand(false)
                        try {
                            if (profile == MediaAudioProfile.BLUETOOTH && !bluetoothPermission)
                                permission.launch(Manifest.permission.BLUETOOTH_CONNECT)
                            else MediaAudioController.select(profile)
                        } catch (e: Exception) {
                            LogManager.addLog("ERROR", "media_audio", "Device selection request failed profile=$profile: $e")
                            Toast.makeText(context, R.string.media_audio_failed, Toast.LENGTH_LONG).show()
                        }
                    })
            }
            HorizontalDivider()
            DropdownMenuItem(text = { Text(stringResource(R.string.media_audio_actual, state.inputName ?: "—", state.outputName ?: "—")) },
                onClick = {}, enabled = false)
            if (state.switching) DropdownMenuItem(text = { Text(stringResource(R.string.media_audio_switching)) }, onClick = {}, enabled = false)
            state.notice?.let { notice ->
                val color = if (notice.isError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant
                DropdownMenuItem(text = { Text(stringResource(notice.message)) }, onClick = {}, enabled = false,
                    colors = MenuDefaults.itemColors(disabledTextColor = color))
            }
        }
    }
}
