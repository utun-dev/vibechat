package com.utun.chat.ui.screens

import android.graphics.Paint
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.Lifecycle
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.lifecycle.repeatOnLifecycle
import com.utun.chat.R
import com.utun.chat.data.CallAudioHistory
import com.utun.chat.data.LogManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import kotlin.math.max

private val graphBlue = Color(0xFF3498DB)
private val graphGreen = Color(0xFF27AE60)
private val graphOrange = Color(0xFFE67E22)
private val graphPurple = Color(0xFF9B59B6)
private val graphRed = Color(0xFFE74C3C)
private class HistoryLine(val count: Int, val color: Color, val time: (Int) -> Long, val value: (Int) -> Float,
                          val connected: (Int) -> Boolean = { true })

/** Только копии native-снимков: закрытие/фон останавливают UI-таймер, сбор истории остаётся у аудиомодуля. */
@Composable
internal fun CallAudioGraphsDialog(callId: Long, onDismiss: () -> Unit) {
    var history by remember(callId) { mutableStateOf<CallAudioHistory?>(null) }
    var live by remember(callId) { mutableStateOf(false) }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(callId) {
        LogManager.addLog("INFO", "call",
            "audio graphs opened call=$callId window_ms=30000 half_life_ms=10000 headroom=after-pcm-block pause=0")
        onDispose { LogManager.addLog("DEBUG", "call", "audio graphs closed call=$callId") }
    }
    LaunchedEffect(callId, lifecycle) {
        lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            while (isActive) {
                val snapshot = withContext(Dispatchers.Default) { CallAudioHistory.read(callId) }
                live = snapshot != null
                if (snapshot != null) history = snapshot
                delay(250)
            }
        }
    }
    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(Modifier.fillMaxWidth().fillMaxHeight(.94f).padding(10.dp), shape = MaterialTheme.shapes.large) {
            Column(Modifier.padding(12.dp)) {
                Row(Modifier.fillMaxWidth()) {
                    Text(stringResource(R.string.audio_graphs), Modifier.weight(1f), style = MaterialTheme.typography.titleLarge)
                    IconButton(onClick = onDismiss) { Icon(Icons.Filled.Close, stringResource(R.string.audio_graph_close)) }
                }
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    Text(stringResource(R.string.audio_graph_window), style = MaterialTheme.typography.bodySmall)
                    Text(stringResource(if (live) R.string.audio_graph_live else R.string.audio_graph_waiting),
                        style = MaterialTheme.typography.bodySmall)
                    val data = history
                    if (data != null) {
                        val fraction = if (data.observedUs > 0) 100.0 * data.missingUs / data.observedUs else 0.0
                        Text(stringResource(R.string.audio_graph_missing, fraction), style = MaterialTheme.typography.bodySmall)
                        val rxTime: (Int) -> Long = { data.rx(it, 0) }
                        val txTime: (Int) -> Long = { data.tx(it, 0) }
                        AudioHistoryPlot(stringResource(R.string.audio_graph_rx), stringResource(R.string.audio_graph_rx_legend), data.nowMs,
                            listOf(HistoryLine(data.rxCount, graphBlue, rxTime, { data.rx(it, 4).toFloat() }),
                                HistoryLine(data.rxCount, graphBlue, rxTime, { data.rx(it, 5).toFloat() }),
                                HistoryLine(data.rxCount, graphGreen, rxTime, { data.rx(it, 2).toFloat() }),
                                HistoryLine(data.rxCount, graphOrange, rxTime, { data.rx(it, 8).toFloat() }),
                                HistoryLine(data.rxCount, graphOrange, rxTime, { data.rx(it, 9).toFloat() }),
                                HistoryLine(data.rxCount, graphRed, rxTime, { if (data.rx(it, 7) > 0) data.rx(it, 4).toFloat() else Float.NaN })),
                            100f, stringResource(R.string.audio_graph_ms), threshold = true)
                        AudioHistoryPlot(stringResource(R.string.audio_graph_tx), stringResource(R.string.audio_graph_tx_legend), data.nowMs,
                            listOf(HistoryLine(data.txCount, graphBlue, txTime, { (data.tx(it, 1) + data.tx(it, 2)).toFloat() }),
                                HistoryLine(data.txCount, graphOrange, txTime, { data.tx(it, 2).toFloat() }),
                                HistoryLine(data.txCount, graphPurple, txTime, { data.tx(it, 3).toFloat() })),
                            1024f, stringResource(R.string.audio_graph_bytes), stringResource(R.string.audio_graph_kib))
                        AudioHistoryPlot(stringResource(R.string.audio_graph_rate), stringResource(R.string.audio_graph_rate_legend), data.nowMs,
                            listOf(HistoryLine(data.txCount, graphGreen, txTime, { data.tx(it, 4).toFloat().takeIf { v -> v >= 0 } ?: Float.NaN },
                                { it > 0 && data.tx(it, 5) == data.tx(it - 1, 5) && data.tx(it, 6) == data.tx(it - 1, 6) })),
                            256f, stringResource(R.string.audio_graph_bps), stringResource(R.string.audio_graph_kibps))
                        AudioHistoryHistogram(data)
                    }
                }
            }
        }
    }
}

@Composable
private fun AudioHistoryPlot(title: String, legend: String, now: Long, lines: List<HistoryLine>, minimumSpan: Float,
                             smallUnit: String, largeUnit: String = "", threshold: Boolean = false) {
    var low = Float.POSITIVE_INFINITY; var high = Float.NEGATIVE_INFINITY
    for (line in lines) for (i in 0 until line.count) {
        val v = line.value(i)
        if (v.isFinite()) { low = minOf(low, v); high = maxOf(high, v) }
    }
    val have = low.isFinite()
    if (!have) { low = 0f; high = 0f }
    val factor = if (largeUnit.isNotEmpty() && high >= 1024) 1024f else 1f
    val unit = if (factor == 1f) smallUnit else largeUnit
    val scaleLow = if (threshold) 0f else low // Доступный запас показывается от нуля, включая паузы накопления.
    val span = max(high - scaleLow, minimumSpan)
    val bottom = max(0f, (high + scaleLow - span) / 2 - span * .05f)
    val top = max(high, bottom + span) + span * .05f
    val foreground = MaterialTheme.colorScheme.onSurfaceVariant
    Spacer(Modifier.height(16.dp)); Text(title, style = MaterialTheme.typography.titleSmall)
    Text(if (have) stringResource(R.string.audio_graph_minmax, formatGraph(low / factor), formatGraph(high / factor), unit)
         else stringResource(R.string.audio_graph_empty), style = MaterialTheme.typography.bodySmall)
    Text(legend, style = MaterialTheme.typography.bodySmall)
    Canvas(Modifier.fillMaxWidth().height(142.dp)) {
        val left = 52.dp.toPx(); val right = size.width - 12.dp.toPx(); val base = size.height - 20.dp.toPx()
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = foreground.toArgb(); textSize = 10.sp.toPx() }
        fun x(at: Long) = left + (30000f - (now - at).toFloat()) / 30000 * (right - left)
        fun y(v: Float) = base - (v - bottom) / (top - bottom) * (base - 8.dp.toPx())
        drawContext.canvas.nativeCanvas.drawText(formatGraph(top / factor), 0f, 12.dp.toPx(), paint)
        drawContext.canvas.nativeCanvas.drawText(formatGraph(bottom / factor), 0f, base, paint)
        for (s in 0..30 step 10) {
            val px = left + s / 30f * (right - left)
            drawLine(foreground.copy(alpha = .2f), Offset(px, 0f), Offset(px, base))
            val label = "${s - 30} s"
            drawContext.canvas.nativeCanvas.drawText(label, px - paint.measureText(label) / 2, size.height - 2.dp.toPx(), paint)
        }
        if (threshold && top >= 3000 && bottom <= 3000)
            drawLine(graphRed.copy(alpha = .5f), Offset(left, y(3000f)), Offset(right, y(3000f)))
        if (threshold) drawLine(foreground.copy(alpha = .6f), Offset(left, y(0f)), Offset(right, y(0f)),
            pathEffect = androidx.compose.ui.graphics.PathEffect.dashPathEffect(floatArrayOf(4.dp.toPx(), 4.dp.toPx())))
        for (line in lines) {
            var previous: Offset? = null; var previousAt = 0L
            for (i in 0 until line.count) {
                val value = line.value(i); val at = line.time(i)
                if (!value.isFinite()) { previous = null; continue }
                val point = Offset(x(at), y(value))
                if (previous != null && at - previousAt <= 150 && line.connected(i)) drawLine(line.color, previous, point, 1.5.dp.toPx())
                else drawCircle(line.color, 1.5.dp.toPx(), point)
                previous = point; previousAt = at
            }
        }
    }
}

private fun formatGraph(value: Float): String = if (value >= 100 || value == 0f) "%.0f".format(value) else "%.1f".format(value)

@Composable
private fun AudioHistoryHistogram(data: CallAudioHistory) {
    var first = (0..60).firstOrNull { data.histogram(it) > 0 } ?: 0
    var last = (0..60).lastOrNull { data.histogram(it) > 0 } ?: 1
    if (first == last) { first = maxOf(0, first - 1); last = minOf(60, last + 1) }
    val peak = (0..60).maxOf { data.histogram(it) }; val scale = maxOf(5f, peak) * 1.05f
    val foreground = MaterialTheme.colorScheme.onSurfaceVariant; val ms = stringResource(R.string.audio_graph_ms)
    Spacer(Modifier.height(16.dp)); Text(stringResource(R.string.audio_graph_distribution), style = MaterialTheme.typography.titleSmall)
    Text(stringResource(R.string.audio_graph_minmax, "0", formatGraph(peak), "%"), style = MaterialTheme.typography.bodySmall)
    Text(stringResource(R.string.audio_graph_weights), style = MaterialTheme.typography.bodySmall)
    Canvas(Modifier.fillMaxWidth().height(142.dp)) {
        val left = 52.dp.toPx(); val right = size.width - 12.dp.toPx(); val base = size.height - 20.dp.toPx()
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = foreground.toArgb(); textSize = 10.sp.toPx() }
        val width = (right - left) / (last - first + 1)
        for (i in first..last) {
            val h = data.histogram(i) / scale * (base - 8.dp.toPx())
            drawRect(graphBlue, Offset(left + (i - first) * width, base - h), Size(maxOf(1f, width - 1.dp.toPx()), h))
        }
        drawContext.canvas.nativeCanvas.drawText("${formatGraph(scale)}%", 0f, 12.dp.toPx(), paint)
        drawContext.canvas.nativeCanvas.drawText("0%", 0f, base, paint)
        drawContext.canvas.nativeCanvas.drawText("${first * 50} $ms", left, size.height - 2.dp.toPx(), paint)
        val end = (if (last == 60) "3000+" else "${(last + 1) * 50}") + " $ms"
        drawContext.canvas.nativeCanvas.drawText(end, right - paint.measureText(end), size.height - 2.dp.toPx(), paint)
    }
}
