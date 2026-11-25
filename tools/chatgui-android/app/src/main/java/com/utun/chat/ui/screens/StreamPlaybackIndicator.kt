package com.utun.chat.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.utun.chat.R
import com.utun.chat.data.StreamPlaybackTelemetry

/** Горизонтальная цепочка nodes: секунды над коротким именем, down=×/красный, stale=?/серый, unknown время=?;
 * tempo>1x выводится справа после локального узла; предупреждение об отставании не скрывает показ или controls.
 */
@Composable
internal fun StreamPlaybackIndicator(state: StreamPlaybackTelemetry, modifier: Modifier = Modifier, showModeSwitch: Boolean = false) {
    if (state.nodes.isEmpty() && !state.waitingLive && !showModeSwitch) return
    Column(modifier.background(Color.Black.copy(alpha = 0.55f)).padding(4.dp)) {
        if (state.waitingLive && state.liveLagMs != null) Text(
            stringResource(R.string.stream_behind_live, state.liveLagMs / 1000.0),
            color = Color(0xffffcc80), style = MaterialTheme.typography.labelSmall)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Row(Modifier.weight(1f).alignByBaseline().horizontalScroll(rememberScrollState()), verticalAlignment = Alignment.Top) {
                state.nodes.forEachIndexed { index, node ->
                    if (index != 0) Text(" → ", color = Color.LightGray, style = MaterialTheme.typography.labelSmall)
                    val color = if (node.down) Color(0xffc88888) else if (node.stale) Color.Gray else Color.LightGray
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        val mark = if (node.down) " ×" else if (node.stale && node.durationUs != null) " ?" else ""
                        Text(node.time + mark, color = color, style = MaterialTheme.typography.labelSmall)
                        Text(node.shortName, color = color, style = MaterialTheme.typography.labelSmall, maxLines = 1)
                    }
                }
                state.speed?.let { Text("  $it", color = Color.LightGray, style = MaterialTheme.typography.labelSmall) }
            }
            if (showModeSwitch) StreamPlaybackSwitch(Modifier.alignByBaseline())
        }
    }
}
