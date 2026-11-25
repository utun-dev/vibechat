package com.utun.chat.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.PlainTooltip
import androidx.compose.material3.Text
import androidx.compose.material3.TooltipBox
import androidx.compose.material3.TooltipDefaults
import androidx.compose.material3.rememberTooltipState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.utun.chat.R

/** Индикатор 60×24 dp: percent ограничивается 0..100 и используется для заполнения, подписи и accessibility.
 * Градиент привязан ко всей ширине: низкая нагрузка зелёная, полная доходит до красного.
 * Компонент только отображает переданное значение; CPU измеряет владелец состояния. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CpuLoadIndicator(percent: Int, modifier: Modifier = Modifier) {
    val value = percent.coerceIn(0, 100)
    val description = stringResource(R.string.cpu_load_description)
    TooltipBox(positionProvider = TooltipDefaults.rememberPlainTooltipPositionProvider(),
        tooltip = { PlainTooltip { Text(description) } }, state = rememberTooltipState()) {
        Box(modifier.size(60.dp, 24.dp).semantics { contentDescription = "$description: $value%" }, contentAlignment = Alignment.Center) {
            Canvas(Modifier.matchParentSize()) {
                val shape = Path().apply { addRoundRect(RoundRect(0f, 0f, size.width, size.height, CornerRadius(5.dp.toPx()))) }
                clipPath(shape) {
                    drawRect(Color(0xFFE4E9E6))
                    clipRect(right = size.width * value / 100f) {
                        drawRect(Brush.horizontalGradient(listOf(Color(0xFF66BB6A), Color(0xFFFFCA28), Color(0xFFEF5350)), endX = size.width))
                    }
                }
            }
            Text("$value%", color = Color(0xFF172019), fontSize = 12.sp, fontWeight = FontWeight.Bold, maxLines = 1)
        }
    }
}
