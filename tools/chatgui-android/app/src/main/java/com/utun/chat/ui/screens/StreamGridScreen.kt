package com.utun.chat.ui.screens

import com.utun.chat.ui.components.MediaAudioDeviceButton

import android.os.SystemClock
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.utun.chat.R
import com.utun.chat.data.LogManager
import com.utun.chat.data.StreamController
import com.utun.chat.data.StreamPhase
import com.utun.chat.data.StreamPlayerEngine
import com.utun.chat.data.StreamPlaybackTelemetry
import com.utun.chat.data.StreamSource
import com.utun.chat.data.streamGridShape
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive

private class GridStream(source: StreamSource) {
    var source by mutableStateOf(source)
    var status by mutableStateOf(R.string.stream_connecting)
    var attempt by mutableStateOf(0)
    var aspect by mutableStateOf(16f / 9)
    var listed by mutableStateOf(true) // Исчезновение BGP/каталога не закрывает viewer, ожиданием управляет ядро.
    var retryAt: Long? = null
    var failures = 0
    var engine: StreamPlayerEngine? = null
}

/** Все плитки находятся в плоском keyed Layout: смена строк/колонок не пересоздаёт TextureView.
 * Пустой каталог оставляет экран открытым; временный сбой каталога сохраняет действующие readers. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun StreamGridScreen(group: String, title: String, sources: List<StreamSource>, unavailable: Boolean,
                              ownPreviewVisible: Boolean, onOwnStream: () -> Unit, onClose: () -> Unit) {
    val tiles = remember(group) { mutableStateListOf<GridStream>() }
    var expanded by remember(group) { mutableStateOf<GridStream?>(null) }
    val focused = expanded?.takeIf { it in tiles }
    BackHandler(enabled = focused != null) { expanded = null }
    if (focused != null) ViewerFullscreenWindow("grid source=${focused.source.id} epoch=${focused.source.epoch}")
    LaunchedEffect(expanded, tiles.toList()) {
        if (expanded != null && expanded !in tiles) {
            LogManager.addLog("INFO", "stream", "multiview expanded source removed group=$group source=${expanded?.source?.id}")
            expanded = null
        }
    }
    LaunchedEffect(focused) {
        LogManager.addLog("INFO", "stream", "multiview focus group=$group source=${focused?.source?.id} epoch=${focused?.source?.epoch}")
    }
    val owner = LocalLifecycleOwner.current
    var visible by remember(owner) { mutableStateOf(owner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) }
    DisposableEffect(owner) {
        val observer = LifecycleEventObserver { _, _ ->
            visible = owner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)
        }
        owner.lifecycle.addObserver(observer)
        onDispose { owner.lifecycle.removeObserver(observer) }
    }
    LaunchedEffect(group, sources, unavailable) {
        if (unavailable) return@LaunchedEffect
        val current = sources.associateBy { it.id }
        tiles.removeAll { tile -> current[tile.source.id]?.let { it.epoch != tile.source.epoch } == true }
        for (tile in tiles) {
            if (tile.source.id in current) {
                if (!tile.listed) LogManager.addLog("INFO", "stream", "multiview source restored group=$group " +
                    "source=${tile.source.id} epoch=${tile.source.epoch} preserveViewer=true")
                tile.source = current.getValue(tile.source.id); tile.listed = true
            } else if (tile.listed) {
                tile.listed = false
                LogManager.addLog("INFO", "stream", "multiview source absent group=$group source=${tile.source.id} " +
                    "epoch=${tile.source.epoch} preserveViewer=${!tile.source.self}")
            }
        }
        for (source in sources) if (tiles.none { it.source.id == source.id }) tiles.add(GridStream(source))
        LogManager.addLog("INFO", "stream", "multiview catalogue group=$group active=${sources.size} tiles=${tiles.size}")
    }
    LaunchedEffect(group, visible, unavailable) {
        while (isActive) {
            val now = SystemClock.elapsedRealtime()
            tiles.removeAll { tile ->
                val terminal = when (tile.status) {
                    R.string.stream_ended, R.string.stream_source_offline, R.string.stream_source_changed, R.string.stream_capacity_exceeded,
                    R.string.stream_member_removed, R.string.stream_group_closed, R.string.chat_service_stopped,
                    R.string.stream_failed -> true
                    else -> false
                }
                val remove = !tile.listed && (tile.source.self || terminal)
                if (remove) LogManager.addLog("INFO", "stream", "multiview absent source finished source=${tile.source.id} status=${tile.status}")
                remove
            }
            if (visible) for (tile in tiles) {
                // Повтор после восстановления BGP допустим только для источника в актуальном каталоге.
                if (tile.source.self || !tile.listed || unavailable
                    || (tile.status != R.string.stream_failed && tile.status != R.string.stream_source_offline)) continue
                if (tile.retryAt == null) {
                    val pause = minOf(10000, 500 shl minOf(tile.failures++, 5))
                    tile.retryAt = now + pause
                    LogManager.addLog("WARN", "stream", "multiview retry source=${tile.source.id} delayMs=$pause")
                } else if (now >= tile.retryAt!!) {
                    tile.retryAt = null; tile.status = R.string.stream_connecting; tile.attempt++
                }
            }
            delay(250)
        }
    }
    Scaffold(modifier = if (ownPreviewVisible) Modifier else Modifier.clearAndSetSemantics {}, topBar = {
        if (focused == null)
            TopAppBar(title = { Text(stringResource(R.string.stream_all_title, title), maxLines = 1, overflow = TextOverflow.Ellipsis) },
                navigationIcon = { IconButton(onClick = onClose) { Icon(Icons.Filled.Close, stringResource(R.string.stream_close_view)) } },
                actions = { MediaAudioDeviceButton() })
    }, contentWindowInsets = if (focused == null) ScaffoldDefaults.contentWindowInsets else WindowInsets(0, 0, 0, 0)) { padding ->
        Column(Modifier.fillMaxSize().padding(padding).background(Color.Black)) {
            if (unavailable && focused == null)
                Text(stringResource(R.string.stream_list_unavailable), color = Color.White, modifier = Modifier.padding(8.dp))
            if (tiles.isEmpty()) Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                Text(stringResource(R.string.stream_waiting_sources), color = Color.White)
            } else BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
                val aspects = tiles.map { it.aspect.toDouble() }
                val shape = streamGridShape(aspects, maxWidth.value.toDouble(), maxHeight.value.toDouble())
                LaunchedEffect(shape, aspects, focused, maxWidth, maxHeight) {
                    LogManager.addLog("INFO", "stream", "multiview layout group=$group tiles=${tiles.size} " +
                        "columns=${shape.columns} rows=${shape.rows} aspects=$aspects " +
                        "focused=${focused?.source?.id} singleAutoTurn=${tiles.size == 1} size=${maxWidth}x$maxHeight")
                }
                Layout(content = {
                    for (tile in tiles) key(tile.source.id, tile.source.epoch) {
                        StreamGridTile(group, tile, visible && (!tile.source.self || ownPreviewVisible),
                            tile === focused, focused != null && tile !== focused, tiles.size == 1,
                            if (focused != null) tile === focused else tile === tiles.lastOrNull { !it.source.self }) {
                            if (tile.source.self) onOwnStream()
                            else expanded = if (tile === focused) null else tile
                        }
                    }
                }, modifier = Modifier.fillMaxSize()) { children, constraints ->
                    val width = constraints.maxWidth
                    val height = constraints.maxHeight
                    val selected = tiles.indexOf(focused)
                    val measured = children.mapIndexed { index, child ->
                        val row = index / shape.columns
                        val columns = shape.columnsInRow(row, children.size)
                        val column = index % shape.columns
                        val cellWidth = (column + 1) * width / columns - column * width / columns
                        val cellHeight = (row + 1) * height / shape.rows - row * height / shape.rows
                        child.measure(if (index == selected) Constraints.fixed(width, height) else Constraints.fixed(cellWidth, cellHeight))
                    }
                    layout(width, height) {
                        // Все TextureView остаются размещёнными; выбранная плитка закрывает остальные непрозрачным фоном.
                        measured.forEachIndexed { index, child ->
                            if (index == selected) child.place(0, 0, zIndex = 1f)
                            else {
                                val row = index / shape.columns
                                val columns = shape.columnsInRow(row, children.size)
                                child.place((index % shape.columns) * width / columns, row * height / shape.rows)
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun StreamGridTile(group: String, tile: GridStream, visible: Boolean, expanded: Boolean, covered: Boolean,
                           single: Boolean, showModeSwitch: Boolean, onClick: () -> Unit) {
    val displayStatus = streamViewerDisplayStatus(tile.status, tile.listed)
    LaunchedEffect(displayStatus) {
        LogManager.addLog("INFO", "stream", "multiview display group=$group source=${tile.source.id} epoch=${tile.source.epoch} " +
            "status=$displayStatus dimmed=${displayStatus == R.string.stream_waiting_source}")
    }
    var telemetry by remember(tile.source.id, tile.source.epoch) { mutableStateOf(StreamPlaybackTelemetry()) }
    val zoom = remember(expanded) { VideoZoomState() }
    LaunchedEffect(zoom.scale, zoom.offset, expanded) {
        if (expanded) {
            delay(300)
            LogManager.addLog("DEBUG", "stream", "multiview zoom group=$group source=${tile.source.id} epoch=${tile.source.epoch} " +
                "scale=${zoom.scale} offset=${zoom.offset}")
        }
    }
    BoxWithConstraints(Modifier.fillMaxSize().padding(if (expanded) 0.dp else 2.dp).clipToBounds()
        .background(if (expanded) Color.Black else Color(0xff151515))
        .then(if (covered) Modifier.clearAndSetSemantics {} else Modifier)) {
        val fit = fitVideoFrame(maxWidth.value, maxHeight.value, tile.aspect, expanded || single)
        val density = LocalDensity.current
        val videoSize = with(density) {
            if (fit.rotation != 0f) Size(fit.height.dp.toPx(), fit.width.dp.toPx())
            else Size(fit.width.dp.toPx(), fit.height.dp.toPx())
        }
        val viewport = with(density) { Size(maxWidth.toPx(), maxHeight.toPx()) }
        LaunchedEffect(zoom, videoSize, viewport) { zoom.constrain(videoSize, viewport) }
        LaunchedEffect(fit, maxWidth, maxHeight, expanded) {
            LogManager.addLog("INFO", "stream", "multiview fit group=$group source=${tile.source.id} epoch=${tile.source.epoch} " +
                "aspect=${tile.aspect} viewport=${maxWidth}x$maxHeight video=${fit.width}x${fit.height} " +
                "rotation=${fit.rotation} zoomEnabled=$expanded")
        }
        if (visible) {
            val video = Modifier.requiredSize(fit.width.dp, fit.height.dp).align(Alignment.Center).graphicsLayer {
                rotationZ = fit.rotation
                scaleX = zoom.scale; scaleY = zoom.scale
                translationX = zoom.offset.x; translationY = zoom.offset.y
            }
            if (tile.source.self) StreamGridPreview(group, tile, onAspect = { if (it.isFinite() && it > 0) tile.aspect = it }, modifier = video)
            else key(tile.attempt) {
                StreamViewerPlayback(group, tile.source.id, tile.source.epoch,
                    onAspect = { if (it.isFinite() && it > 0) tile.aspect = it },
                    onStatus = {
                        tile.status = it
                        if (it == R.string.stream_live) { tile.failures = 0; tile.retryAt = null }
                    }, modifier = video, onEngine = { tile.engine = it }, onTelemetry = { telemetry = it })
            }
        }
        if (displayStatus == R.string.stream_waiting_source)
            Box(Modifier.matchParentSize().background(Color.Black.copy(alpha = 0.6f)))
        if (!expanded || displayStatus != R.string.stream_live) {
            Column(Modifier.align(Alignment.TopStart).fillMaxWidth().background(Color.Black.copy(alpha = 0.5f)).padding(4.dp)) {
                Text(tile.source.displayName, color = Color.White, style = MaterialTheme.typography.labelSmall,
                    maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (displayStatus != R.string.stream_live)
                    Text(stringResource(displayStatus), color = Color.White, style = MaterialTheme.typography.labelSmall,
                        maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
        }
        Box(Modifier.matchParentSize().clickable(interactionSource = remember { MutableInteractionSource() }, indication = null,
            role = Role.Button, onClickLabel = stringResource(if (tile.source.self) R.string.stream_manage
                else if (expanded) R.string.collapse else R.string.expand), onClick = onClick)
            .pointerInput(expanded, videoSize, viewport) {
                if (expanded) detectTransformGestures { centroid, pan, factor, _ ->
                    zoom.transform(centroid, pan, factor, videoSize, viewport)
                }
            }
            .semantics { contentDescription = tile.source.displayName })
        if (!tile.source.self) StreamPlaybackIndicator(telemetry, Modifier.align(Alignment.BottomStart).fillMaxWidth()
            .then(if (expanded) Modifier.windowInsetsPadding(WindowInsets.safeDrawing) else Modifier),
            showModeSwitch = showModeSwitch)
        if (expanded) Box(Modifier.align(Alignment.TopEnd).windowInsetsPadding(WindowInsets.safeDrawing)) {
            MediaAudioDeviceButton(tint = Color.White)
        }
    }
}

/** Собственный источник никогда не открывает reader: звук остаётся только у удалённых плиток. */
@Composable
private fun StreamGridPreview(group: String, tile: GridStream, onAspect: (Float) -> Unit, modifier: Modifier) {
    val state by StreamController.state.collectAsState()
    val local = state?.takeIf { it.group == group }
    val active = local?.takeIf { it.ongoing && it.phase != StreamPhase.STOPPING }
    LaunchedEffect(local?.id, local?.phase, active?.config?.mode?.screen) {
        if (active?.config?.mode?.screen == true) tile.status = R.string.stream_live
        else if (active == null) tile.status = when (local?.phase) {
            StreamPhase.STOPPING -> R.string.stream_stopping
            StreamPhase.FAILED -> R.string.stream_failed
            else -> R.string.stream_ended
        }
        LogManager.addLog(if (local == null) "WARN" else "INFO", "stream",
            "multiview local preview group=$group source=${tile.source.id} epoch=${tile.source.epoch} " +
                "publisher=${local?.id} phase=${local?.phase} active=${active != null} decoder=false audio=false")
    }
    if (active?.config?.mode?.screen == true) Text(stringResource(R.string.capture_sharing_screen), modifier = modifier)
    else if (active != null) StreamPreviewPlayback(active.preview, StreamDisplayDegrees(), onAspect = onAspect,
        onStatus = { tile.status = it }, modifier = modifier)
}
