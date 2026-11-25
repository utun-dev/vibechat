package com.utun.chat.ui.screens

import android.app.DatePickerDialog
import android.app.Dialog
import android.app.TimePickerDialog
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import com.utun.chat.ChatApplication
import com.utun.chat.R
import com.utun.chat.data.LogManager
import com.utun.chat.ui.components.UserAvatar
import org.json.JSONObject
import java.text.DateFormat
import java.util.Calendar
import java.util.Date

/** Текст, текущий автор, неизменная дата создания и срок; права и версии проверяет общее ядро. */
@Composable
internal fun KanbanCardDetails(group: String, card: JSONObject, board: JSONObject,
    onCommand: (JSONObject, (JSONObject) -> Unit) -> Long) {
    val id = card.getString("id")
    val members by ChatApplication.instance.members.members.collectAsState()
    val membersByEd = remember(members, group) { members.filterKeys { it.group == group }.values.associateBy { it.ed25519 } }
    val author = card.getString("author")
    val name = membersByEd[author]?.name?.ifBlank { author } ?: author
    val created = card.getLong("created_at") / 1000
    val due = card.getString("due").takeIf { it.isNotEmpty() }?.toLong()?.div(1000)
    val canWrite = board.optBoolean("can_write")
    val canChangeDue = canWrite && (author == board.optString("self") ||
        (!card.optBoolean("author_active") && board.optBoolean("can_manage")))
    var editingDue by rememberSaveable(group, id) { mutableStateOf(false) }
    LaunchedEffect(group, id, author, created, due) {
        LogManager.addLog("DEBUG", "Kanban", "Card details group=$group card=$id author=$author created=$created due=$due")
    }
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        KanbanCardText(group, card, canWrite, onCommand)
        if (card.optBoolean("conflict")) Text(stringResource(R.string.kanban_conflict), color = MaterialTheme.colorScheme.error)
        Text(stringResource(R.string.kanban_author), style = MaterialTheme.typography.titleSmall)
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            UserAvatar(group, author, name, size = 28.dp, enabled = false)
            Text(name, modifier = Modifier.weight(1f))
        }
        Text(stringResource(R.string.kanban_created_at, DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT)
            .format(Date(created))), style = MaterialTheme.typography.bodyMedium)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.kanban_due), style = MaterialTheme.typography.titleSmall)
                Text(due?.let { DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(it)) }
                    ?: stringResource(R.string.kanban_no_due), style = MaterialTheme.typography.bodyMedium)
            }
            TextButton(enabled = canChangeDue, onClick = { editingDue = true }) { Text(stringResource(R.string.edit)) }
        }
    }
    if (editingDue) KanbanDueDateDialog(group, card, canChangeDue, onCommand) { editingDue = false }
}

/** Само поле обрабатывает касание: штатный hit-test Compose сразу ставит курсор в выбранную позицию. */
@Composable
private fun KanbanCardText(group: String, card: JSONObject, canWrite: Boolean,
    onCommand: (JSONObject, (JSONObject) -> Unit) -> Long) {
    val id = card.getString("id")
    var value by rememberSaveable(group, id, stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue(card.getString("text"))) }
    var baseline by rememberSaveable(group, id) { mutableStateOf(card.toString()) }
    var editing by rememberSaveable(group, id) { mutableStateOf(false) }
    var saving by remember(group, id) { mutableStateOf(false) }
    var failed by remember(group, id) { mutableStateOf(false) }
    var active by remember(group, id) { mutableStateOf(true) }
    val focus = LocalFocusManager.current
    val keyboard = LocalSoftwareKeyboardController.current
    val textLabel = stringResource(R.string.kanban_card_text)
    DisposableEffect(group, id) { active = true; onDispose { active = false } }
    // Во время редактирования сохраняем исходные parents: удалённые правки не подменяют базу черновика.
    SideEffect {
        if (!editing) {
            baseline = card.toString()
            if (value.text != card.getString("text")) value = TextFieldValue(card.getString("text"))
        }
    }
    LaunchedEffect(editing) {
        if (editing) LogManager.addLog("DEBUG", "Kanban", "Inline text opened group=$group card=$id " +
            "cursor=${value.selection.start} length=${value.text.length}")
    }
    BasicTextField(value, { value = it }, readOnly = !canWrite || saving,
        textStyle = MaterialTheme.typography.bodyLarge.copy(color = MaterialTheme.colorScheme.onSurface),
        cursorBrush = SolidColor(MaterialTheme.colorScheme.primary), modifier = Modifier.fillMaxWidth()
            .semantics { contentDescription = textLabel }
            .onFocusChanged { if (it.isFocused && canWrite) editing = true })
    if (editing) {
        val original = JSONObject(baseline)
        val variants = original.optJSONArray("variants")
        if (original.optBoolean("conflict") && variants != null) for (i in 0 until variants.length()) {
            Text(variants.getJSONObject(i).getString("text"), color = MaterialTheme.colorScheme.onSurfaceVariant)
            HorizontalDivider()
        }
        if (failed) Text(stringResource(R.string.kanban_failed), color = MaterialTheme.colorScheme.error)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(enabled = canWrite && !saving && value.text.isNotBlank(), onClick = {
                saving = true; failed = false
                val command = JSONObject().put("action", "card_edit").put("entity", id)
                    .put("text", value.text).put("parents", original.getJSONArray("heads"))
                LogManager.addLog("DEBUG", "Kanban", "Inline text saving group=$group card=$id " +
                    "length=${value.text.length} parents=${original.getJSONArray("heads").length()}")
                val request = onCommand(command) result@{ result ->
                    if (!active) return@result
                    saving = false; failed = !result.optBoolean("ok")
                    if (!failed) { editing = false; focus.clearFocus(); keyboard?.hide() }
                }
                if (request == 0L) { saving = false; failed = true }
            }) { Text(stringResource(R.string.kanban_save)) }
            TextButton(enabled = !saving, onClick = {
                LogManager.addLog("DEBUG", "Kanban", "Inline text cancelled group=$group card=$id")
                value = TextFieldValue(card.getString("text")); editing = false; failed = false
                focus.clearFocus(); keyboard?.hide()
            }) { Text(stringResource(R.string.cancel)) }
        }
    }
}

/** Дата и время выбираются в часовом поясе устройства; протокол получает UTC в микросекундах и grant открытого снимка. */
@Composable
private fun KanbanDueDateDialog(group: String, card: JSONObject, canWrite: Boolean,
    onCommand: (JSONObject, (JSONObject) -> Unit) -> Long, onDismiss: () -> Unit) {
    val id = card.getString("id")
    val grant = rememberSaveable(group, id) { card.getString("grant") }
    val initial = card.getString("due")
    var enabled by rememberSaveable(group, id) { mutableStateOf(initial.isNotEmpty()) }
    var deadline by rememberSaveable(group, id) { mutableStateOf(if (initial.isNotEmpty()) initial.toLong() / 1000 else
        Calendar.getInstance().apply { add(Calendar.DAY_OF_MONTH, 1); set(Calendar.SECOND, 0); set(Calendar.MILLISECOND, 0) }.timeInMillis) }
    var picker by rememberSaveable(group, id) { mutableStateOf(0) }
    var saving by remember(group, id) { mutableStateOf(false) }
    var failed by remember(group, id) { mutableStateOf(false) }
    var active by remember(group, id) { mutableStateOf(true) }
    val context = LocalContext.current
    DisposableEffect(group, id) { active = true; onDispose { active = false } }
    LaunchedEffect(Unit) { LogManager.addLog("DEBUG", "Kanban", "Due editor opened group=$group card=$id due=$initial grant=$grant") }
    if (picker != 0) DisposableEffect(picker, context) {
        val selected = Calendar.getInstance().apply { timeInMillis = deadline }
        val dialog: Dialog = if (picker == 1) DatePickerDialog(context, { _, year, month, day ->
            deadline = Calendar.getInstance().apply {
                timeInMillis = deadline; set(year, month, day); set(Calendar.SECOND, 0); set(Calendar.MILLISECOND, 0)
            }.timeInMillis
            LogManager.addLog("DEBUG", "Kanban", "Due date picked group=$group card=$id millis=$deadline")
        }, selected.get(Calendar.YEAR), selected.get(Calendar.MONTH), selected.get(Calendar.DAY_OF_MONTH))
        else TimePickerDialog(context, { _, hour, minute ->
            deadline = Calendar.getInstance().apply {
                timeInMillis = deadline; set(Calendar.HOUR_OF_DAY, hour); set(Calendar.MINUTE, minute)
                set(Calendar.SECOND, 0); set(Calendar.MILLISECOND, 0)
            }.timeInMillis
            LogManager.addLog("DEBUG", "Kanban", "Due time picked group=$group card=$id millis=$deadline")
        }, selected.get(Calendar.HOUR_OF_DAY), selected.get(Calendar.MINUTE), android.text.format.DateFormat.is24HourFormat(context))
        dialog.setOnDismissListener { picker = 0 }; dialog.show()
        onDispose { dialog.setOnDismissListener(null); dialog.dismiss() }
    }
    AlertDialog(onDismissRequest = { if (!saving) onDismiss() }, title = { Text(stringResource(R.string.kanban_due)) }, text = {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Checkbox(checked = enabled, enabled = canWrite && !saving, onCheckedChange = { enabled = it })
                Text(stringResource(R.string.kanban_due))
            }
            TextButton(enabled = enabled && canWrite && !saving, onClick = { picker = 1 }) {
                Text(stringResource(R.string.kanban_due_date, DateFormat.getDateInstance(DateFormat.MEDIUM).format(Date(deadline))))
            }
            TextButton(enabled = enabled && canWrite && !saving, onClick = { picker = 2 }) {
                Text(stringResource(R.string.kanban_due_time, DateFormat.getTimeInstance(DateFormat.SHORT).format(Date(deadline))))
            }
            if (failed) Text(stringResource(R.string.kanban_failed), color = MaterialTheme.colorScheme.error)
        }
    }, confirmButton = {
        TextButton(enabled = canWrite && !saving, onClick = {
            val text = if (enabled) (deadline * 1000).toString() else ""
            saving = true; failed = false
            LogManager.addLog("DEBUG", "Kanban", "Due saving group=$group card=$id due=$text grant=$grant")
            val request = onCommand(JSONObject().put("action", "card_due").put("entity", id).put("grant", grant).put("text", text)) result@{ result ->
                if (!active) return@result
                saving = false; failed = !result.optBoolean("ok")
                if (!failed) onDismiss()
            }
            if (request == 0L) { saving = false; failed = true }
        }) { Text(stringResource(R.string.kanban_save)) }
    }, dismissButton = { TextButton(enabled = !saving, onClick = onDismiss) { Text(stringResource(R.string.cancel)) } })
}
