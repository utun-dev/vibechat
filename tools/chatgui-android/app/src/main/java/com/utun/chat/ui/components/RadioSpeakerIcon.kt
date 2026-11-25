package com.utun.chat.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp

/** Волны показывают речь независимо от mute; красное перечёркивание всегда показывает локальное отключение. */
@Composable
fun RadioSpeakerIcon(speaking: Boolean, muted: Boolean, label: String, modifier: Modifier = Modifier.size(24.dp)) {
    val color = when { muted -> Color(0xFFEF5350); speaking -> Color(0xFF4CAF50); else -> Color(0xFF9E9E9E) }
    Canvas(modifier.semantics { contentDescription = label }) {
        val unit = size.minDimension / 24f
        val speaker = Path().apply {
            moveTo(4 * unit, 9 * unit); lineTo(8 * unit, 9 * unit); lineTo(13 * unit, 5 * unit)
            lineTo(13 * unit, 19 * unit); lineTo(8 * unit, 15 * unit); lineTo(4 * unit, 15 * unit); close()
        }
        drawPath(speaker, color)
        if (speaking) {
            val stroke = Stroke(1.8f * unit, cap = StrokeCap.Round)
            drawArc(color, -60f, 120f, false, Offset(9 * unit, 7 * unit), Size(10 * unit, 10 * unit), style = stroke)
            drawArc(color, -60f, 120f, false, Offset(6 * unit, 4 * unit), Size(16 * unit, 16 * unit), style = stroke)
        }
        if (muted) drawLine(color, Offset(2 * unit, 22 * unit), Offset(22 * unit, 2 * unit),
            strokeWidth = 1.8f * unit, cap = StrokeCap.Round)
    }
}
