package com.utun.chat.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.material3.LocalContentColor
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp

/** Canvas-иконка на сетке 24×24: minDimension задаёт масштаб; label — accessibility, tint — цвет всех штрихов. */
@Composable
fun PortableRadioIcon(label: String, modifier: Modifier = Modifier.size(24.dp), tint: Color = LocalContentColor.current) {
    val color = tint
    Canvas(modifier.semantics { contentDescription = label }) {
        val unit = size.minDimension / 24f
        val stroke = 1.7f * unit
        fun line(x1: Float, y1: Float, x2: Float, y2: Float) {
            drawLine(color, Offset(x1 * unit, y1 * unit), Offset(x2 * unit, y2 * unit),
                strokeWidth = stroke, cap = StrokeCap.Round)
        }
        line(8.5f, 1f, 8.5f, 9f)
        line(14.5f, 7f, 14.5f, 9f)
        drawRoundRect(color, Offset(7f * unit, 9f * unit), Size(10f * unit, 13f * unit),
            CornerRadius(2f * unit), style = Stroke(stroke))
        drawRoundRect(color, Offset(9.5f * unit, 11.5f * unit), Size(5f * unit, 3f * unit),
            CornerRadius(0.5f * unit), style = Stroke(stroke))
        for (y in listOf(16f, 18f, 20f)) line(9.5f, y, 14.5f, y)
    }
}
