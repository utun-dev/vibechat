package com.utun.chat.ui.screens

import androidx.compose.animation.core.tween
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.boundsInRoot
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.LocalPinnableContainer
import androidx.compose.ui.layout.PinnableContainer
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import com.utun.chat.data.ObjectLinks
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.Dp
import com.utun.chat.AppEventHandler
import com.utun.chat.ChatApplication
import com.utun.chat.R
import com.utun.chat.data.Channel
import com.utun.chat.data.LogManager
import com.utun.chat.data.NativeLib
import com.utun.chat.ui.components.GroupPropertiesDialog
import com.utun.chat.ui.components.UserAvatar
import com.utun.chat.data.Message
import com.utun.chat.viewmodel.ChatViewModel
import java.text.SimpleDateFormat
import java.util.Locale
import kotlin.math.abs
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.DateFormat
import java.util.Date
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.roundToInt

private val boardRequests = AtomicLong()
private val boardJsonSaver = Saver<JSONObject?, String>(
    save = { it?.toString().orEmpty() }, restore = { if (it.isEmpty()) null else JSONObject(it) })
private data class BoardPending(val action: String, val text: String, val onResult: (JSONObject) -> Unit)
private data class CardBounds(val column: String, val rect: Rect, val container: PinnableContainer?)
private fun JSONArray.objects(): List<JSONObject> = List(length()) { getJSONObject(it) }
private fun JSONObject.command(action: String): JSONObject = put("action", action)

/** UI хранит снимок и предпросмотр drag; подписи, порядок и слияние принадлежат общему ядру. */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun KanbanScreen(channel: Channel, viewModel: ChatViewModel, onViewPhoto: (Message) -> Unit, onPlayVideo: (Message) -> Unit,
    onMembers: () -> Unit, onInvite: () -> Unit, onBack: () -> Unit) {
    var board by remember(channel.id) { mutableStateOf(JSONObject()) }
    var deferred by remember(channel.id) { mutableStateOf<JSONObject?>(null) }
    var draft by rememberSaveable(channel.id) { mutableStateOf("") }
    var failed by remember(channel.id) { mutableStateOf(false) }
    var edit by rememberSaveable(channel.id, stateSaver = boardJsonSaver) { mutableStateOf<JSONObject?>(null) }
    var editText by rememberSaveable(channel.id) { mutableStateOf("") }
    var editColor by rememberSaveable(channel.id) { mutableStateOf("") }
    var selectedCard by rememberSaveable(channel.id) { mutableStateOf("") }
    val clipboard = LocalClipboardManager.current
    val navigation by viewModel.objectNavigation.collectAsState()
    var widthColumn by rememberSaveable(channel.id) { mutableStateOf("") }
    var columnWidth by rememberSaveable(channel.id) { mutableStateOf(290f) }
    val preferences = ChatApplication.instance.configProvider
    var savingEdit by remember(channel.id) { mutableStateOf(0L) }
    var history by remember(channel.id) { mutableStateOf<JSONArray?>(null) }
    var properties by remember(channel.id) { mutableStateOf(false) }
    var columnEdit by rememberSaveable(channel.id, stateSaver = boardJsonSaver) { mutableStateOf<JSONObject?>(null) }
    var columnName by rememberSaveable(channel.id) { mutableStateOf("") }
    var dragged by remember(channel.id) { mutableStateOf<JSONObject?>(null) }
    var dragPoint by remember(channel.id) { mutableStateOf(Offset.Zero) }
    var dragHeight by remember(channel.id) { mutableStateOf(80f) }
    var dragWidth by remember(channel.id) { mutableStateOf(0f) }
    var dragAnchor by remember(channel.id) { mutableStateOf(Offset.Zero) }
    var targetColumn by remember(channel.id) { mutableStateOf("") }
    var targetBefore by remember(channel.id) { mutableStateOf("") }
    val cardBounds = remember(channel.id) { mutableMapOf<String, CardBounds>() }
    val dragPin = remember(channel.id) { arrayOfNulls<PinnableContainer.PinnedHandle>(1) }
    DisposableEffect(channel.id) { onDispose { dragPin[0]?.release(); dragPin[0] = null } }
    val columnBounds = remember(channel.id) { mutableMapOf<String, Rect>() }
    val columnStates = remember(channel.id) { mutableMapOf<String, LazyListState>() }
    val bounds = remember(channel.id) { arrayOfNulls<Rect>(2) } // доска, placeholder; обновляются layout без рекомпозиции.
    val pending = remember(channel.id) { mutableMapOf<Long, BoardPending>() }
    val rowState = rememberLazyListState()
    val scope = rememberCoroutineScope()
    val density = LocalDensity.current
    val focus = LocalFocusManager.current
    val keyboard = LocalSoftwareKeyboardController.current
    var displayed by remember(channel.id) { mutableStateOf(0L) }
    fun send(command: JSONObject, onResult: (JSONObject) -> Unit = {}): Long {
        val request = boardRequests.incrementAndGet()
        val action = command.getString("action")
        pending[request] = BoardPending(action, command.optString("text"), onResult)
        if (!NativeLib.kanbanRequest(channel.id, request, command.toString())) {
            LogManager.addLog("ERROR", "Kanban", "Command enqueue failed group=${channel.id} request=$request action=$action")
            pending.remove(request); if (action != "download") failed = true; return 0
        }
        LogManager.addLog("DEBUG", "Kanban", "Command queued group=${channel.id} request=$request action=$action entity=${command.optString("entity")}")
        return request
    }
    fun updateTarget(point: Offset) {
        if (bounds[1]?.contains(point) == true) return
        val column = columnBounds.entries.firstOrNull { it.value.contains(point) }?.key ?: return
        targetColumn = column
        targetBefore = cardBounds.entries.filter { it.value.column == column && it.key != dragged?.optString("id") }
            .sortedBy { it.value.rect.top }.firstOrNull { point.y < it.value.rect.center.y }?.key.orEmpty()
    }
    fun finishDrag(commit: Boolean) {
        val card = dragged
        if (card != null) LogManager.addLog("DEBUG", "Kanban", "Drag finished group=${channel.id} card=${card.optString("id")} " +
            "commit=$commit inside=${columnBounds[targetColumn]?.contains(dragPoint)} column=$targetColumn before=$targetBefore")
        if (commit && card != null && columnBounds[targetColumn]?.contains(dragPoint) == true)
            send(JSONObject().command("card_move").put("entity", card.getString("id"))
                .put("column", targetColumn).put("before", targetBefore))
        dragged = null; bounds[1] = null
        dragPin[0]?.release(); dragPin[0] = null
        deferred?.let { board = it }; deferred = null
    }
    LaunchedEffect(channel.id) {
        coroutineScope {
            launch(start = CoroutineStart.UNDISPATCHED) {
                AppEventHandler.events.collect { (type, bytes) ->
                    if (type == 13) {
                        LogManager.addLog("WARN", "Kanban", "Core stopped group=${channel.id} pending=${pending.size}")
                        pending.values.toList().forEach { it.onResult(JSONObject().put("ok", false)) }
                        finishDrag(false); failed = true; board = JSONObject(); pending.clear(); savingEdit = 0; selectedCard = ""
                        return@collect
                    }
                    if (type != 79 && type != 80) return@collect
                    if (bytes == null || (type == 79 && bytes.size != 32) || (type == 80 && bytes.size <= 40)) return@collect
                    val data = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
                    val offset = if (type == 79) 0 else 8
                    val group = bytes.copyOfRange(offset, offset + 32).joinToString("") { "%02x".format(it.toInt() and 255) }
                    if (group != channel.id) return@collect
                    if (type == 79) { send(JSONObject().command("snapshot")); return@collect }
                    val request = data.getLong(0); val sent = pending.remove(request) ?: return@collect
                    try {
                        val result = JSONObject(String(bytes, 40, bytes.size - 40, Charsets.UTF_8))
                        if (!result.optBoolean("ok")) {
                            if (sent.action != "download") failed = true
                            if (savingEdit == request) savingEdit = 0
                            LogManager.addLog("WARN", "Kanban", "Command failed group=${channel.id} action=${sent.action} error=${result.optString("error")}")
                            sent.onResult(result)
                            return@collect
                        }
                        failed = false
                        if (sent.action == "card_create" && draft == sent.text) draft = ""
                        if (savingEdit == request) { savingEdit = 0; edit = null }
                        result.optJSONArray("history")?.let { history = it }
                        result.optJSONObject("board")?.let {
                            if (request >= displayed) { displayed = request; if (dragged != null) deferred = it else board = it }
                        }
                        LogManager.addLog("DEBUG", "Kanban", "Command complete group=${channel.id} request=$request action=${sent.action}")
                        sent.onResult(result)
                    } catch (error: Exception) {
                        failed = true; if (savingEdit == request) savingEdit = 0
                        LogManager.addLog("ERROR", "Kanban", "Invalid board response group=${channel.id}: $error")
                        sent.onResult(JSONObject().put("ok", false))
                    }
                }
            }
            send(JSONObject().command("snapshot")); awaitCancellation()
        }
    }
    LaunchedEffect(dragged?.optString("id")) {
        while (dragged != null) {
            val outer = bounds[0]
            if (outer != null) {
                val dx = when { dragPoint.x < outer.left + 40 -> -16f; dragPoint.x > outer.right - 40 -> 16f; else -> 0f }
                if (dx != 0f) rowState.scrollBy(dx)
            }
            columnBounds[targetColumn]?.let { rect ->
                val dy = when { dragPoint.y < rect.top + 60 -> -14f; dragPoint.y > rect.bottom - 40 -> 14f; else -> 0f }
                if (dy != 0f) columnStates[targetColumn]?.scrollBy(dy)
            }
            updateTarget(dragPoint); delay(30)
        }
    }
    val columns = board.optJSONArray("columns")?.objects().orEmpty().filterNot { it.optBoolean("deleted") }
    val cards = board.optJSONArray("cards")?.objects().orEmpty().filterNot { it.optBoolean("deleted") }
    val canWrite = board.optBoolean("can_write")
    val borderColors = board.optJSONArray("colors")?.let { values -> List(values.length()) { values.getString(it) } }.orEmpty()
    val borderWidth = with(density) { 2f.toDp() }
    val dividerWidth = with(density) { 1f.toDp() }
    val detailCard = cards.firstOrNull { it.optString("id") == selectedCard }
    LaunchedEffect(navigation?.revision, board) {
        val pending = navigation ?: return@LaunchedEffect
        if (pending.target.group != channel.groupEd || board.length() == 0) return@LaunchedEffect
        val card = cards.firstOrNull { it.optString("id") == pending.target.card }
        if (card == null) { selectedCard = ""; viewModel.completeObjectLink(pending.revision, false) }
        else {
            selectedCard = pending.target.card
            if (pending.target.message.isEmpty()) viewModel.completeObjectLink(pending.revision, true)
        }
    }
    val pagerState = rememberPagerState(pageCount = { if (detailCard == null) 1 else 2 })
    LaunchedEffect(selectedCard, detailCard != null) {
        if (detailCard != null) {
            LogManager.addLog("DEBUG", "Kanban", "Card pane opening group=${channel.id} card=$selectedCard side=right")
            pagerState.animateScrollToPage(1)
        }
    }
    LaunchedEffect(pagerState) {
        snapshotFlow { pagerState.settledPage }.collect { page ->
            if (page == 0) { focus.clearFocus(); keyboard?.hide() }
            LogManager.addLog("DEBUG", "Kanban", "Joined panes settled group=${channel.id} page=$page card=$selectedCard")
        }
    }
    LaunchedEffect(board) {
        if (board.length() > 0 && selectedCard.isNotEmpty() && detailCard == null) {
            LogManager.addLog("INFO", "Kanban", "Selected card removed group=${channel.id} card=$selectedCard")
            selectedCard = ""
        }
    }
    // Доска и детали занимают соседние страницы: общий скролл сдвигает оба окна одновременно.
    HorizontalPager(state = pagerState, beyondBoundsPageCount = 1, modifier = Modifier.fillMaxSize()) { page ->
        if (page == 0) Scaffold(modifier = Modifier.fillMaxSize().imePadding(), topBar = {
            TopAppBar(title = { Text(channel.name, maxLines = 1) }, navigationIcon = {
                IconButton(onClick = { focus.clearFocus(); keyboard?.hide(); onBack() }) {
                    Icon(Icons.AutoMirrored.Filled.ArrowBack, stringResource(R.string.back))
                }
            }, actions = {
                TextButton(onClick = onMembers) { Text(stringResource(R.string.kanban_members)) }
                TextButton(onClick = onInvite) { Text(stringResource(R.string.kanban_invite)) }
                TextButton(onClick = { properties = true }) { Text(stringResource(R.string.kanban_properties)) }
            })
        }) { padding ->
            Column(Modifier.fillMaxSize().padding(padding)) {
                if (board.optBoolean("pending_history")) Text(stringResource(R.string.kanban_waiting_history), modifier = Modifier.padding(8.dp))
                if (failed) Text(stringResource(R.string.kanban_failed), color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(8.dp))
                if (board.optBoolean("can_manage")) TextButton(onClick = { columnName = ""; columnEdit = JSONObject() }) {
                    Text(stringResource(R.string.kanban_add_column))
                }
                Box(Modifier.weight(1f).fillMaxWidth().onGloballyPositioned { bounds[0] = it.boundsInRoot() }) {
                    LazyRow(state = rowState, horizontalArrangement = Arrangement.spacedBy(0.dp), modifier = Modifier.fillMaxSize()) {
                        items(columns, key = { it.getString("id") }) { column ->
                            val id = column.getString("id"); val state = rememberLazyListState(); columnStates[id] = state
                            DisposableEffect(id) { onDispose { columnBounds.remove(id); columnStates.remove(id) } }
                            var menu by remember(id) { mutableStateOf(false) }
                            val width by remember(channel.id, id) { preferences.kanbanColumnWidth(channel.id, id) }.collectAsState(initial = 290)
                            Row(Modifier.fillMaxHeight().animateItemPlacement(tween(180))) {
                            Column(Modifier.padding(horizontal = 6.dp).width(width.dp).fillMaxHeight()
                                .onGloballyPositioned { columnBounds[id] = it.boundsInRoot() }) {
                                val builtin = column.optInt("builtin")
                                val title = when (builtin) {
                                    1 -> stringResource(R.string.kanban_new); 2 -> stringResource(R.string.kanban_doing)
                                    3 -> stringResource(R.string.kanban_done); else -> column.optString("name")
                                }
                                Box {
                                    TextButton(onClick = { menu = true }) { Text(title) }
                                    DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                                        DropdownMenuItem(text = { Text(stringResource(R.string.kanban_rename_column)) }, enabled = board.optBoolean("can_manage"), onClick = {
                                            menu = false; columnName = title; columnEdit = column
                                        })
                                        val index = columns.indexOfFirst { it.optString("id") == id }
                                        DropdownMenuItem(text = { Text(stringResource(R.string.kanban_left)) }, enabled = board.optBoolean("can_manage") && index > 0, onClick = {
                                            menu = false; send(JSONObject().command("column_move").put("entity", id).put("before", columns[index - 1].getString("id")))
                                        })
                                        DropdownMenuItem(text = { Text(stringResource(R.string.kanban_right)) }, enabled = board.optBoolean("can_manage") && index + 1 < columns.size, onClick = {
                                            menu = false; send(JSONObject().command("column_move").put("entity", id).put("before", columns.getOrNull(index + 2)?.getString("id").orEmpty()))
                                        })
                                        DropdownMenuItem(text = { Text(stringResource(R.string.kanban_delete_column)) }, enabled = board.optBoolean("can_manage"), onClick = {
                                            menu = false; send(JSONObject().command("column_delete").put("entity", id))
                                        })
                                        HorizontalDivider()
                                        DropdownMenuItem(text = { Text(stringResource(R.string.kanban_column_width)) }, onClick = {
                                            menu = false; widthColumn = id; columnWidth = width.toFloat()
                                        })
                                    }
                                }
                                // Источник остаётся в композиции до отпускания: удаление отменяет текущий pointer-жест.
                                val visible = cards.filter { it.optString("column") == id }
                                val gap = if (dragged != null && targetColumn == id) visible.indexOfFirst { it.optString("id") == targetBefore }
                                    .let { if (it < 0) visible.size else it } else -1
                                val order = visible.map { it.optString("id") }.toMutableList().apply { if (gap >= 0) add(gap, "drop-gap") }
                                LazyColumn(state = state, modifier = Modifier.fillMaxSize()) {
                                    items(order, key = { it }) { cardId ->
                                        if (cardId == "drop-gap") {
                                            Box(Modifier.fillMaxWidth().padding(bottom = 8.dp).height(with(density) { dragHeight.toDp() })
                                                .animateItemPlacement(tween(180)).border(2.dp, MaterialTheme.colorScheme.primary, MaterialTheme.shapes.medium)
                                                .onGloballyPositioned { bounds[1] = it.boundsInRoot() })
                                        } else {
                                            val card = visible.first { it.optString("id") == cardId }
                                            val color = card.optString("color")
                                            val cardBorder = if (color.isEmpty()) MaterialTheme.colorScheme.outline else Color(android.graphics.Color.parseColor(color))
                                            val container = LocalPinnableContainer.current
                                            val isDragged = dragged?.optString("id") == cardId
                                            DisposableEffect(cardId) { onDispose {
                                                if (dragged?.optString("id") == cardId) LogManager.addLog("WARN", "Kanban",
                                                    "Dragged source detached group=${channel.id} card=$cardId")
                                                cardBounds.remove(cardId)
                                            } }
                                            var cardMenu by remember(cardId) { mutableStateOf(false) }
                                            Card(Modifier.fillMaxWidth().animateItemPlacement(tween(180))
                                                .heightIn(max = if (isDragged) 0.dp else Dp.Infinity).alpha(if (isDragged) 0f else 1f)
                                                .padding(bottom = 8.dp)
                                                .onGloballyPositioned { cardBounds[cardId] = CardBounds(id, it.boundsInRoot(), container) }
                                                .pointerInput(channel.id, cardId, canWrite) {
                                                    if (!canWrite) return@pointerInput
                                                    detectDragGesturesAfterLongPress(onDragStart = { local ->
                                                        val source = cardBounds[cardId]
                                                        if (source == null || dragged != null) {
                                                            LogManager.addLog("WARN", "Kanban", "Drag unavailable group=${channel.id} card=$cardId " +
                                                                "bounds=${source != null} active=${dragged?.optString("id")}")
                                                            return@detectDragGesturesAfterLongPress
                                                        }
                                                        dragged = card; dragPoint = local + source.rect.topLeft; dragAnchor = local
                                                        dragHeight = source.rect.height; dragWidth = source.rect.width
                                                        dragPin[0] = source.container?.pin(); targetColumn = id
                                                        val inColumn = cards.filter { it.optString("column") == id }
                                                        val index = inColumn.indexOfFirst { it.optString("id") == cardId }
                                                        targetBefore = inColumn.getOrNull(index + 1)?.optString("id").orEmpty()
                                                        LogManager.addLog("DEBUG", "Kanban", "Drag started group=${channel.id} card=$cardId " +
                                                            "width=$dragWidth height=$dragHeight anchor=$dragAnchor pinned=${dragPin[0] != null}")
                                                    }, onDrag = { change, delta ->
                                                        if (dragged?.optString("id") == cardId) {
                                                            change.consume(); dragPoint += delta; updateTarget(dragPoint)
                                                        }
                                                    }, onDragEnd = { if (dragged?.optString("id") == cardId) finishDrag(true) },
                                                        onDragCancel = { if (dragged?.optString("id") == cardId) finishDrag(false) })
                                                }
                                                .clickable {
                                                    viewModel.cancelObjectNavigation()
                                                    focus.clearFocus(); keyboard?.hide()
                                                    if (selectedCard == cardId) scope.launch { pagerState.animateScrollToPage(1) }
                                                    else selectedCard = cardId
                                                    LogManager.addLog("DEBUG", "Kanban", "Card opened group=${channel.id} card=$cardId")
                                                }, border = BorderStroke(borderWidth, cardBorder)) {
                                                KanbanCardContent(channel.id, card) {
                                                    Box {
                                                        TextButton(onClick = { cardMenu = true }) { Text("⋮") }
                                                        DropdownMenu(expanded = cardMenu, onDismissRequest = { cardMenu = false }) {
                                                            DropdownMenuItem(text = { Text(stringResource(R.string.copy_link)) }, onClick = {
                                                                cardMenu = false
                                                                ObjectLinks.build(channel.groupEd, channel.name, cardId, card.optString("text"))?.let {
                                                                    clipboard.setText(AnnotatedString(it))
                                                                }
                                                            })
                                                            DropdownMenuItem(text = { Text(stringResource(R.string.kanban_edit)) }, enabled = canWrite, onClick = {
                                                                cardMenu = false; edit = card; editText = card.optString("text"); editColor = card.optString("color"); savingEdit = 0
                                                            })
                                                            DropdownMenuItem(text = { Text(stringResource(R.string.kanban_history)) }, onClick = {
                                                                cardMenu = false; send(JSONObject().command("history").put("entity", cardId))
                                                            })
                                                            val deleteAllowed = canWrite && (board.optBoolean("can_moderate") || card.optString("author") == board.optString("self"))
                                                            DropdownMenuItem(text = { Text(stringResource(R.string.kanban_delete)) }, enabled = deleteAllowed, onClick = {
                                                                cardMenu = false; send(JSONObject().command("card_delete").put("entity", cardId))
                                                            })
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            if (column != columns.last()) Box(Modifier.fillMaxHeight().width(dividerWidth).background(MaterialTheme.colorScheme.outlineVariant))
                            }
                        }
                    }
                    dragged?.let { card ->
                        val origin = bounds[0]?.topLeft ?: Offset.Zero
                        val color = card.optString("color")
                        val cardBorder = if (color.isEmpty()) MaterialTheme.colorScheme.outline else Color(android.graphics.Color.parseColor(color))
                        Card(Modifier.offset { IntOffset((dragPoint.x - origin.x - dragAnchor.x).roundToInt(),
                            (dragPoint.y - origin.y - dragAnchor.y).roundToInt()) }
                            .width(with(density) { dragWidth.toDp() }), border = BorderStroke(borderWidth, cardBorder),
                            elevation = CardDefaults.cardElevation(defaultElevation = 12.dp)) {
                            KanbanCardContent(channel.id, card) { TextButton(onClick = {}, enabled = false) { Text("⋮") } }
                        }
                    }
                }
                Row(Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    OutlinedTextField(draft, { draft = it }, enabled = canWrite, maxLines = 4,
                        label = { Text(stringResource(R.string.kanban_new_card)) }, modifier = Modifier.weight(1f))
                    TextButton(enabled = canWrite && draft.isNotBlank(), onClick = {
                        columns.firstOrNull()?.let { send(JSONObject().command("card_create").put("column", it.getString("id")).put("text", draft)) }
                    }) { Text(stringResource(R.string.kanban_send)) }
                }
            }
        } else detailCard?.let { card ->
            KanbanCardScreen(channel, card, board, failed, ::send, viewModel, onViewPhoto, onPlayVideo, onBack = {
                viewModel.cancelObjectNavigation()
                scope.launch {
                    pagerState.animateScrollToPage(0)
                    if (selectedCard == card.getString("id")) {
                        LogManager.addLog("DEBUG", "Kanban", "Card closed group=${channel.id} card=$selectedCard")
                        selectedCard = ""
                    }
                }
            })
        }
    }
    edit?.let { card ->
        AlertDialog(onDismissRequest = { if (savingEdit == 0L) edit = null },
            title = { Text(stringResource(if (card.optBoolean("conflict")) R.string.kanban_resolve else R.string.kanban_edit)) },
            text = { Column(Modifier.heightIn(max = 500.dp).verticalScroll(rememberScrollState())) {
                if (card.optBoolean("conflict")) card.optJSONArray("variants")?.objects()?.forEach {
                    Text(it.optString("text"), modifier = Modifier.padding(bottom = 12.dp))
                    HorizontalDivider()
                }
                OutlinedTextField(editText, { editText = it }, enabled = savingEdit == 0L, modifier = Modifier.fillMaxWidth())
                val colorLabel = stringResource(R.string.kanban_border_color)
                Row(Modifier.horizontalScroll(rememberScrollState()).padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    borderColors.forEach { color ->
                        Box(Modifier.size(20.dp).background(Color(android.graphics.Color.parseColor(color)))
                            .border(if (editColor == color) 3.dp else 1.dp, MaterialTheme.colorScheme.onSurface)
                            .semantics { contentDescription = "$colorLabel $color" }
                            .clickable(enabled = savingEdit == 0L) { editColor = color })
                    }
                }
                if (failed) Text(stringResource(R.string.kanban_failed), color = MaterialTheme.colorScheme.error)
            } }, confirmButton = { TextButton(enabled = savingEdit == 0L && editText.isNotBlank(), onClick = {
                val command = JSONObject().command("card_edit").put("entity", card.getString("id"))
                    .put("text", editText).put("parents", card.getJSONArray("heads"))
                if (editColor != card.optString("color")) command.put("color", editColor)
                savingEdit = send(command)
            }) { Text(stringResource(R.string.kanban_save)) } },
            dismissButton = { TextButton(enabled = savingEdit == 0L, onClick = { edit = null }) { Text(stringResource(R.string.cancel)) } })
    }
    if (widthColumn.isNotEmpty()) AlertDialog(onDismissRequest = { widthColumn = "" },
        title = { Text(stringResource(R.string.kanban_column_width)) }, text = { Column {
            Text(columnWidth.roundToInt().toString())
            Slider(columnWidth, { columnWidth = it }, valueRange = 220f..1200f)
        } }, confirmButton = { TextButton(onClick = {
            preferences.saveKanbanColumnWidth(channel.id, widthColumn, columnWidth.roundToInt()); widthColumn = ""
        }) { Text(stringResource(R.string.kanban_save)) } },
        dismissButton = { TextButton(onClick = { widthColumn = "" }) { Text(stringResource(R.string.cancel)) } })
    columnEdit?.let { column ->
        AlertDialog(onDismissRequest = { columnEdit = null }, title = { Text(stringResource(R.string.kanban_column_name)) },
            text = { OutlinedTextField(columnName, { columnName = it }) }, confirmButton = {
                TextButton(enabled = columnName.isNotBlank(), onClick = {
                    send(JSONObject().command(if (column.has("id")) "column_rename" else "column_create")
                        .put("entity", column.optString("id")).put("text", columnName.trim())); columnEdit = null
                }) { Text(stringResource(R.string.kanban_save)) }
            }, dismissButton = { TextButton(onClick = { columnEdit = null }) { Text(stringResource(R.string.cancel)) } })
    }
    history?.let { entries ->
        AlertDialog(onDismissRequest = { history = null }, title = { Text(stringResource(R.string.kanban_history)) },
            text = { Column(Modifier.heightIn(max = 500.dp).verticalScroll(rememberScrollState())) {
                entries.objects().forEach { entry ->
                    val body = entry.getJSONObject("body")
                    Text(DateFormat.getDateTimeInstance().format(Date(entry.getLong("time") / 1000)) + " · " + entry.optString("author"))
                    val action = when (body.optString("action")) {
                        "card_create" -> R.string.kanban_created
                        "card_edit" -> R.string.kanban_edited
                        "card_move" -> R.string.kanban_moved
                        "card_delete" -> R.string.kanban_deleted
                        else -> null
                    }
                    action?.let { Text(stringResource(it)) }
                    Text(body.optString("text"))
                    if (!entry.optBoolean("active")) Text(stringResource(R.string.kanban_archived))
                    HorizontalDivider()
                }
            } }, confirmButton = { TextButton(onClick = { history = null }) { Text(stringResource(android.R.string.ok)) } })
    }
    if (properties) GroupPropertiesDialog(channel) { properties = false }
}

/** Одинаковое содержимое карточки на доске и при перетаскивании. */
@Composable
private fun KanbanCardContent(group: String, card: JSONObject, menu: @Composable () -> Unit) {
    Column(Modifier.padding(12.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(card.optString("text"), maxLines = 8, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
            menu()
        }
        if (card.optBoolean("conflict")) Text(stringResource(R.string.kanban_conflict), color = MaterialTheme.colorScheme.error)
        KanbanCardFooter(group, card)
    }
}

/** Нижняя строка: автор, исполнители, дата без года и округлённый вверх остаток до срока. */
@Composable
private fun KanbanCardFooter(group: String, card: JSONObject) {
    val members by ChatApplication.instance.members.members.collectAsState()
    val membersByEd = remember(members, group) { members.filterKeys { it.group == group }.values.associateBy { it.ed25519 } }
    var now by remember { mutableStateOf(System.currentTimeMillis()) }
    LaunchedEffect(Unit) { while (true) { now = System.currentTimeMillis(); delay(60_000) } }
    val created = card.optLong("created_at") / 1000
    val date = if (created > 0) SimpleDateFormat("dd.MM", Locale.getDefault()).format(Date(created)) else ""
    val due = card.optString("due").toLongOrNull()?.div(1000) ?: 0
    val delta = due - now
    val magnitude = abs(delta)
    val hours = magnitude < 3 * 86_400_000L
    val unit = if (hours) 3_600_000L else 86_400_000L
    val remaining = if (due > 0) stringResource(if (hours) R.string.kanban_remaining_hours else R.string.kanban_remaining_days,
        (magnitude + unit - 1) / unit) else ""
    val assignees = card.optJSONArray("assignees") ?: JSONArray()
    LaunchedEffect(group, card.optString("id"), card.optString("creator"), assignees.toString(), created, due) {
        LogManager.addLog("DEBUG", "Kanban", "Card footer group=$group card=${card.optString("id")} " +
            "creator=${card.optString("creator")} assignees=${assignees.length()} created=$created due=$due")
    }
    @Composable fun avatar(id: String) {
        val name = membersByEd[id]?.name ?: id
        UserAvatar(group, id, name, size = 22.dp, enabled = false)
    }
    Row(Modifier.fillMaxWidth().padding(top = 6.dp), verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        avatar(card.optString("creator"))
        Spacer(Modifier.width(4.dp))
        BoxWithConstraints(Modifier.weight(1f)) {
            val capacity = (maxWidth.value / 26f).toInt().coerceAtLeast(0)
            val visible = if (assignees.length() > capacity) (capacity - 1).coerceAtLeast(0) else assignees.length()
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                for (i in 0 until visible) avatar(assignees.optString(i))
                if (visible < assignees.length()) Text("+${assignees.length() - visible}", style = MaterialTheme.typography.labelSmall,
                    maxLines = 1, overflow = TextOverflow.Clip)
            }
        }
        Text(date, style = MaterialTheme.typography.labelSmall, maxLines = 1)
        if (remaining.isNotEmpty()) Text((if (delta < 0) "−" else "") + remaining,
            color = if (delta < 0) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
            style = MaterialTheme.typography.labelSmall, maxLines = 1)
    }
}
