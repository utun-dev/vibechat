package com.utun.chat.ui.components

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp

/** Индикатор 12dp из готового online snapshot; сетевую проверку не запускает, статус также доступен через semantics. */
@Composable
fun PeerStatusDot(online: Boolean, modifier: Modifier = Modifier) {
    val description = stringResource(if (online) R.string.online else R.string.not_online)
    Box(modifier.size(12.dp).background(if (online) Color(0xFF4CAF50) else Color(0xFFE53935), CircleShape)
        .semantics { contentDescription = description })
}
