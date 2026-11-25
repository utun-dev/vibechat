package com.utun.chat.ui.screens

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.utun.chat.data.LogManager

/** Последние 10000 строк общей истории, включая накопленные до открытия экрана; снимки обновляются не чаще кадра.
 * Фильтр выбирает точный уровень. Пауза отключает только автопрокрутку, сбор логов продолжается.
 * Подписка получает актуальную ревизию при открытии; прокрутка следует отфильтрованному списку и не блокирует чтение истории.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogScreen(onBack: () -> Unit) {
    var entries by remember { mutableStateOf(LogManager.getRecent(10000)) }
    val listState = rememberLazyListState()
    var filterLevel by remember { mutableStateOf("") }
    var autoScroll by remember { mutableStateOf(true) }

    LaunchedEffect(Unit) {
        LogManager.addLog("DEBUG", "Log", "Log viewer subscribed: history_limit=10000")
        try {
            LogManager.revision.collect {
                withFrameNanos { }
                entries = LogManager.getRecent(10000)
            }
        } finally {
            LogManager.addLog("DEBUG", "Log", "Log viewer unsubscribed")
        }
    }

    val filtered = remember(entries, filterLevel) {
        if (filterLevel.isEmpty()) entries else entries.filter { it.level == filterLevel }
    }
    LaunchedEffect(filtered, autoScroll) {
        if (autoScroll && filtered.isNotEmpty()) listState.scrollToItem(filtered.lastIndex)
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.logs)) },
                navigationIcon = { IconButton(onClick = onBack) { Icon(Icons.Default.ArrowBack, stringResource(R.string.back)) } },
                actions = {
                    for ((lvl, label) in arrayOf("" to "ALL", "ERROR" to "ERR", "WARN" to "WRN", "INFO" to "INF", "DEBUG" to "DBG", "TRACE" to "TRC")) {
                        val selected = filterLevel == lvl
                        Text(
                            label,
                            color = if (selected) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant,
                            fontSize = 11.sp,
                            modifier = Modifier
                                .clickable { filterLevel = lvl }
                                .background(
                                    if (selected) MaterialTheme.colorScheme.primary else Color.Transparent,
                                    MaterialTheme.shapes.extraSmall
                                )
                                .padding(horizontal = 6.dp, vertical = 2.dp)
                        )
                    }
                    TextButton(onClick = { autoScroll = !autoScroll }) {
                        Text(if (autoScroll) "|>" else "||", fontSize = 11.sp)
                    }
                }
            )
        }
    ) { padding ->
        LazyColumn(modifier = Modifier.padding(padding), state = listState) {
            items(filtered.size) { idx ->
                val entry = filtered[idx]
                val color = when (entry.level) {
                    "ERROR" -> Color(0xFFFFB4AB)
                    "WARN"  -> Color(0xFFFFAA00)
                    "INFO"  -> Color(0xFF81C784)
                    "DEBUG" -> Color(0xFF79B8FF)
                    "TRACE" -> Color(0xFFAEBCC8)
                    else    -> Color(0xFFAEBCC8)
                }
                Text(
                    text = "${entry.level} [${entry.category}] ${entry.message}",
                    color = color,
                    fontSize = 11.sp,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 1.dp)
                )
            }
            item { Spacer(Modifier.height(8.dp)) }
        }
    }
}
