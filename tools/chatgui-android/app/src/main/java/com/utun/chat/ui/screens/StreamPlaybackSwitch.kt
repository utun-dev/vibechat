package com.utun.chat.ui.screens

import androidx.compose.runtime.*
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.res.stringResource
import com.utun.chat.R
import com.utun.chat.ChatApplication
import com.utun.chat.data.LogManager

/** Общая настройка всех плееров, сохраняется независимо от lifecycle экрана. */
@Composable
internal fun StreamPlaybackSwitch(modifier: Modifier = Modifier) {
    val provider = ChatApplication.instance.configProvider
    val low by provider.streamLowLatency.collectAsState(initial = com.utun.chat.data.StreamPlayerEngine.lowLatency)
    var saving by remember { mutableStateOf(false) }
    Row(modifier.padding(horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(stringResource(R.string.stream_low_latency), color = Color.White, style = MaterialTheme.typography.labelSmall)
        Switch(checked = low, enabled = !saving, onCheckedChange = { value ->
            provider.changes.submit("stream playback mode") {
                saving = true
                try { provider.saveStreamLowLatency(value) }
                catch (e: kotlinx.coroutines.CancellationException) { throw e }
                catch (e: Exception) { LogManager.addLog("ERROR", "stream", "Playback preference failed: $e") }
                finally { saving = false }
            }
        })
    }
}
