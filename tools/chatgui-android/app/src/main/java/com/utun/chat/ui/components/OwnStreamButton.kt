package com.utun.chat.ui.components

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.runtime.*
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import com.utun.chat.R
import com.utun.chat.data.LogManager
import com.utun.chat.data.StreamController

/** Локальный publisher в STARTING/LIVE/STOPPING: onOpen получает group ID; location служит диагностике размещения кнопки. */
@Composable
fun OwnStreamButton(location: String, onOpen: (String) -> Unit) {
    val state by StreamController.state.collectAsState()
    val publisher = state?.takeIf { it.ongoing }
    LaunchedEffect(location, publisher?.id, publisher?.phase) {
        LogManager.addLog("INFO", "stream", "own stream header location=$location publisher=${publisher?.id} " +
            "group=${publisher?.group} phase=${publisher?.phase} visible=${publisher != null}")
    }
    if (publisher != null) IconButton(onClick = {
        LogManager.addLog("INFO", "stream", "own stream header clicked location=$location publisher=${publisher.id} group=${publisher.group}")
        onOpen(publisher.group)
    }) {
        Icon(Icons.Filled.Videocam, stringResource(R.string.stream_manage), tint = Color(0xFFE53935))
    }
}
