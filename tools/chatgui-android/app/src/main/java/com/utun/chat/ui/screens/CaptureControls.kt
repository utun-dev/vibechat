package com.utun.chat.ui.screens

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.ScreenRotation
import androidx.compose.material.icons.outlined.Person
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.PopupProperties
import com.utun.chat.R
import com.utun.chat.data.StreamOrientation
import java.text.NumberFormat

internal enum class CaptureIconShape { CAMERA, CHIP, GAUGE, SCREEN }

/** Положительный bitrate в бит/с отображается в десятичных k/M; дробь до 10 M и разделитель берутся из locale. */
@Composable
internal fun captureBitrateLabel(bitrate: Int): String {
    val mega = bitrate >= 1_000_000
    val value = NumberFormat.getNumberInstance(LocalConfiguration.current.locales[0]).apply {
        maximumFractionDigits = if (mega && bitrate < 10_000_000) 1 else 0
        isGroupingUsed = false
    }.format(bitrate.toDouble() / if (mega) 1_000_000 else 1_000)
    return stringResource(if (mega) R.string.stream_bitrate_short_m else R.string.stream_bitrate_short_k, value)
}

/** Единый размер касания, подсказки и семантика для обычных кнопок и переключателя звука. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun CaptureControlButton(description: String, enabled: Boolean = true, checked: Boolean? = null,
                                containerColor: Color? = null, onClick: () -> Unit, icon: @Composable () -> Unit) {
    TooltipBox(positionProvider = TooltipDefaults.rememberPlainTooltipPositionProvider(), tooltip = { PlainTooltip { Text(description) } },
        state = rememberTooltipState()) {
        val modifier = Modifier.size(48.dp).semantics { contentDescription = description }
        val colors = MaterialTheme.colorScheme
        val background = containerColor ?: colors.surfaceVariant.copy(alpha = 0.55f)
        val disabledBackground = containerColor?.copy(alpha = 0.38f) ?: colors.surfaceVariant.copy(alpha = 0.2f)
        val content = if (containerColor != null) Color.White else colors.onSurface
        val shape = RoundedCornerShape(12.dp)
        if (checked == null) FilledTonalIconButton(onClick = onClick, enabled = enabled, modifier = modifier, shape = shape,
            colors = IconButtonDefaults.filledTonalIconButtonColors(containerColor = background, contentColor = content,
                disabledContainerColor = disabledBackground, disabledContentColor = content.copy(alpha = 0.38f))) {
            Box(Modifier.clearAndSetSemantics {}) { icon() }
        } else FilledTonalIconToggleButton(checked = checked, onCheckedChange = { onClick() }, enabled = enabled,
            modifier = modifier, shape = shape,
            colors = IconButtonDefaults.filledTonalIconToggleButtonColors(containerColor = background, contentColor = colors.onSurfaceVariant,
                disabledContainerColor = disabledBackground, checkedContainerColor = colors.primaryContainer,
                checkedContentColor = colors.onPrimaryContainer)) {
            Box(Modifier.clearAndSetSemantics {}) { icon() }
        }
    }
}

/** Различимые силуэты камеры, микросхемы, шкалы скорости и экрана содержат текущие значения. */
@Composable
internal fun CaptureValueIcon(shape: CaptureIconShape, value: String, detail: String? = null, front: Boolean = false) {
    val frame = LocalContentColor.current
    val accent = MaterialTheme.colorScheme.primary.copy(alpha = frame.alpha)
    Box(Modifier.size(40.dp, 36.dp), contentAlignment = Alignment.Center) {
        Canvas(Modifier.matchParentSize()) {
            val w = size.width; val h = size.height
            val stroke = Stroke(1.5.dp.toPx(), cap = StrokeCap.Round)
            when (shape) {
                CaptureIconShape.CAMERA -> {
                    val body = Path().apply {
                        moveTo(w * 0.12f, h * 0.24f); lineTo(w * 0.28f, h * 0.24f)
                        lineTo(w * 0.36f, h * 0.08f); lineTo(w * 0.64f, h * 0.08f); lineTo(w * 0.72f, h * 0.24f)
                        lineTo(w * 0.88f, h * 0.24f); quadraticBezierTo(w * 0.96f, h * 0.24f, w * 0.96f, h * 0.34f)
                        lineTo(w * 0.96f, h * 0.84f); quadraticBezierTo(w * 0.96f, h * 0.94f, w * 0.88f, h * 0.94f)
                        lineTo(w * 0.12f, h * 0.94f); quadraticBezierTo(w * 0.04f, h * 0.94f, w * 0.04f, h * 0.84f)
                        lineTo(w * 0.04f, h * 0.34f); quadraticBezierTo(w * 0.04f, h * 0.24f, w * 0.12f, h * 0.24f); close()
                    }
                    drawPath(body, frame, style = stroke)
                    drawLine(accent, Offset(w * 0.43f, h * 0.08f), Offset(w * 0.57f, h * 0.08f), stroke.width, StrokeCap.Round)
                }
                CaptureIconShape.CHIP -> {
                    drawRoundRect(frame, Offset(w * 0.14f, h * 0.14f), Size(w * 0.72f, h * 0.72f),
                        CornerRadius(3.dp.toPx()), style = stroke)
                    for (position in listOf(0.28f, 0.5f, 0.72f)) {
                        drawLine(accent, Offset(w * position, h * 0.02f), Offset(w * position, h * 0.14f), stroke.width)
                        drawLine(accent, Offset(w * position, h * 0.86f), Offset(w * position, h * 0.98f), stroke.width)
                        drawLine(accent, Offset(w * 0.02f, h * position), Offset(w * 0.14f, h * position), stroke.width)
                        drawLine(accent, Offset(w * 0.86f, h * position), Offset(w * 0.98f, h * position), stroke.width)
                    }
                }
                CaptureIconShape.GAUGE -> {
                    drawArc(accent, 180f, 180f, false, Offset(w * 0.05f, h * 0.06f), Size(w * 0.9f, h * 0.9f), style = stroke)
                    drawLine(frame, Offset(w * 0.05f, h * 0.51f), Offset(w * 0.15f, h * 0.51f), stroke.width)
                    drawLine(frame, Offset(w * 0.5f, h * 0.06f), Offset(w * 0.5f, h * 0.16f), stroke.width)
                    drawLine(frame, Offset(w * 0.85f, h * 0.51f), Offset(w * 0.95f, h * 0.51f), stroke.width)
                    drawLine(frame, Offset(w * 0.16f, h * 0.94f), Offset(w * 0.84f, h * 0.94f), stroke.width, StrokeCap.Round)
                }
                CaptureIconShape.SCREEN -> {
                    drawRoundRect(frame, Offset(w * 0.03f, h * 0.05f), Size(w * 0.94f, h * 0.76f),
                        CornerRadius(3.dp.toPx()), style = stroke)
                    drawLine(accent, Offset(w * 0.5f, h * 0.81f), Offset(w * 0.5f, h * 0.96f), stroke.width)
                    drawLine(accent, Offset(w * 0.3f, h * 0.96f), Offset(w * 0.7f, h * 0.96f), stroke.width, StrokeCap.Round)
                }
            }
        }
        when (shape) {
            CaptureIconShape.CAMERA -> Box(Modifier.offset(y = 4.dp)) {
                if (front) Icon(Icons.Outlined.Person, contentDescription = null, modifier = Modifier.size(24.dp))
                else Text(value, fontSize = 11.sp, lineHeight = 13.sp, fontWeight = FontWeight.Medium, maxLines = 1, softWrap = false)
            }
            CaptureIconShape.CHIP -> Text(value, fontSize = 11.sp, lineHeight = 13.sp,
                fontWeight = FontWeight.SemiBold, maxLines = 1, softWrap = false)
            CaptureIconShape.GAUGE -> {
                if (detail != null) Text(detail, Modifier.align(Alignment.TopCenter).padding(top = 7.dp),
                    fontSize = 8.sp, lineHeight = 9.sp, fontWeight = FontWeight.Medium, color = accent, maxLines = 1)
                Text(value, Modifier.offset(y = 7.dp), fontSize = 12.sp, lineHeight = 14.sp,
                    fontWeight = FontWeight.SemiBold, maxLines = 1, softWrap = false)
            }
            CaptureIconShape.SCREEN -> Column(Modifier.padding(bottom = 5.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                Text(value, fontSize = 11.sp, lineHeight = 12.sp, fontWeight = FontWeight.SemiBold, maxLines = 1, softWrap = false)
                if (detail != null) Text(detail, fontSize = 8.sp, lineHeight = 9.sp, maxLines = 1, softWrap = false)
            }
        }
    }
}

/** Название параметра над вариантами меню; заголовок не является действием. */
@Composable
internal fun CaptureMenuHeader(title: String) {
    Text(title, Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp).semantics { heading() },
        style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
    HorizontalDivider()
}

/** Текущее значение отмечено галочкой и доступно средствам чтения экрана. */
@Composable
internal fun CaptureOptionItem(label: String, active: Boolean, onClick: () -> Unit, icon: @Composable (() -> Unit)? = null) {
    DropdownMenuItem(text = { Text(label) }, onClick = onClick, leadingIcon = icon,
        trailingIcon = { if (active) Icon(Icons.Filled.Check, contentDescription = null) }, modifier = Modifier.semantics { selected = active })
}

/** Общий выбор AUTO/0/90/270° для фото, видеосообщений и трансляции; маркер показывает верх записываемого кадра.
 * displayDegrees — 0/90/180/270°; front меняет направление компенсации. Изменение применяет вызывающий владелец. */
@Composable
internal fun CaptureOrientationControl(orientation: StreamOrientation?, displayDegrees: Int, front: Boolean,
                                       enabled: Boolean, expanded: Boolean, properties: PopupProperties,
                                       onExpandedChange: (Boolean) -> Unit, onChange: (StreamOrientation) -> Unit) {
    val auto = orientation == StreamOrientation.AUTO
    val degrees = orientation?.degrees?.takeIf { it >= 0 } ?: 0
    fun top(turn: Int) = if (front) (displayDegrees - turn + 360) % 360 else (turn - displayDegrees + 360) % 360
    val label = if (auto) stringResource(R.string.stream_orientation_auto) else stringResource(R.string.stream_orientation, degrees)
    Box {
        CaptureControlButton(label, enabled, onClick = { onExpandedChange(!expanded) }) {
            if (auto) Icon(Icons.Filled.ScreenRotation, contentDescription = null, modifier = Modifier.size(32.dp))
            else CaptureRotationMark(top(degrees), degrees)
        }
        DropdownMenu(expanded = expanded && enabled, properties = properties, onDismissRequest = { onExpandedChange(false) }) {
            CaptureMenuHeader(stringResource(R.string.stream_orientation_title))
            CaptureOptionItem(stringResource(R.string.stream_orientation_auto), auto,
                onClick = { onExpandedChange(false); onChange(StreamOrientation.AUTO) },
                icon = { Icon(Icons.Filled.ScreenRotation, contentDescription = null, modifier = Modifier.size(32.dp)) })
            for (option in listOf(StreamOrientation.UPRIGHT, StreamOrientation.RIGHT, StreamOrientation.LEFT)) {
                CaptureOptionItem(stringResource(R.string.stream_orientation, option.degrees), orientation == option,
                    onClick = { onExpandedChange(false); onChange(option) },
                    icon = { CaptureRotationMark(top(option.degrees), option.degrees) })
            }
        }
    }
}

/** Активное ребро выделено сплошной полосой и треугольником; угол сдвинут от маркера внутрь кадра. */
@Composable
private fun CaptureRotationMark(top: Int, degrees: Int) {
    val frame = LocalContentColor.current
    val marker = MaterialTheme.colorScheme.primary.copy(alpha = frame.alpha)
    Box(Modifier.size(40.dp, 36.dp), contentAlignment = Alignment.Center) {
        Canvas(Modifier.matchParentSize()) {
            val left = size.width * 0.2f; val right = size.width * 0.8f
            val upper = size.height * 0.04f; val lower = size.height * 0.96f
            rotate(top.toFloat()) {
                drawRoundRect(frame.copy(alpha = frame.alpha * 0.6f), Offset(left, upper), Size(right - left, lower - upper),
                    CornerRadius(4.dp.toPx()), style = Stroke(1.5.dp.toPx()))
                drawRoundRect(marker, Offset(left, upper), Size(right - left, 4.dp.toPx()), CornerRadius(2.dp.toPx()))
                val triangle = Path().apply {
                    moveTo(center.x - 5.dp.toPx(), upper + 3.dp.toPx()); lineTo(center.x + 5.dp.toPx(), upper + 3.dp.toPx())
                    lineTo(center.x, upper + 7.dp.toPx()); close()
                }
                drawPath(triangle, marker)
            }
        }
        val x = when (top) { 90 -> (-2).dp; 270 -> 2.dp; else -> 0.dp }
        val y = when (top) { 0 -> 2.dp; 180 -> (-2).dp; else -> 0.dp }
        Text("${degrees}°", Modifier.offset(x, y), fontSize = 10.sp, lineHeight = 12.sp,
            fontWeight = FontWeight.Medium, maxLines = 1, softWrap = false)
    }
}
