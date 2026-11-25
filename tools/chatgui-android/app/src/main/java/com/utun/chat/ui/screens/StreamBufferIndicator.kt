package com.utun.chat.ui.screens

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.PlainTooltip
import androidx.compose.material3.Text
import androidx.compose.material3.TooltipBox
import androidx.compose.material3.TooltipDefaults
import androidx.compose.material3.rememberTooltipState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.utun.chat.R
import com.utun.chat.data.StreamBufferPositions

/** До 16 показанных отстающих peers: линия worst..best в seq-окне источника, fraction ограничивает крайние значения.
 * Tooltip содержит total всех получателей; пустой список рисует нейтральную линию в поле 96×48dp.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun StreamBufferIndicator(positions: StreamBufferPositions) {
    val description = stringResource(R.string.stream_buffer_indicator, positions.total)
    val background = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.55f)
    val marker = MaterialTheme.colorScheme.onSurface
    val empty = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.3f)
    val zones = listOf(Color(0xFFE15B55), Color(0xFFE1AD36), Color(0xFF40B878))
    TooltipBox(positionProvider = TooltipDefaults.rememberPlainTooltipPositionProvider(),
        tooltip = { PlainTooltip { Text(description) } }, state = rememberTooltipState()) {
        Canvas(Modifier.size(96.dp, 48.dp).semantics { contentDescription = description }) {
            drawRoundRect(background, cornerRadius = CornerRadius(12.dp.toPx()))
            val left = 6.dp.toPx()
            val width = size.width - left * 2
            val height = size.height - 12.dp.toPx()
            if (positions.peers.isEmpty()) {
                drawLine(empty, Offset(left, size.height / 2), Offset(left + width, size.height / 2), 2.dp.toPx())
            } else {
                val pitch = height / positions.peers.size
                val thickness = minOf(4.dp.toPx(), pitch * 0.65f)
                positions.peers.forEachIndexed { index, peer ->
                    val y = 6.dp.toPx() + pitch * (index + 0.5f)
                    zones.forEachIndexed { zone, color ->
                        drawRect(color.copy(alpha = 0.4f), Offset(left + width * zone / 3, y - thickness / 2), Size(width / 3, thickness))
                    }
                    val worst = left + width * positions.fraction(peer.worst)
                    val best = left + width * positions.fraction(peer.best)
                    drawLine(marker.copy(alpha = 0.8f), Offset(worst, y), Offset(best, y), thickness)
                    drawLine(marker, Offset(worst, y - thickness), Offset(worst, y + thickness), 1.dp.toPx())
                    drawLine(marker, Offset(best, y - thickness), Offset(best, y + thickness), 1.dp.toPx())
                }
            }
        }
    }
}
