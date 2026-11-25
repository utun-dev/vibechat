package com.utun.chat.ui.components

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import com.utun.chat.R
import com.utun.chat.data.LocalSocketState

/** Готовый NAT snapshot сокета: label из модели, source=2 помечается STUN; непустой externalIp выводится с портом и IPv6-скобками. */
@Composable
fun LocalSocketNatInfo(socket: LocalSocketState) {
    val source = if (socket.natSource == 2) " · STUN" else ""
    Text(stringResource(socket.natLabelRes) + source,
        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    if (socket.externalIp.isNotEmpty()) {
        val ip = if (socket.externalIp.contains(':')) "[${socket.externalIp}]" else socket.externalIp
        Text(stringResource(R.string.socket_external_address, "$ip:${socket.externalPort}"),
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
