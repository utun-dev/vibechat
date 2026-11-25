package com.utun.chat.ui.components

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.utun.chat.R
import com.utun.chat.data.*

/** Форма нового опроса: пункты разделены строками, тексты trim-ятся, лимиты проверяются в UTF-8 байтах.
 * Подтверждение вызывает onCreate(question, options, multiple, allowCustom), затем закрывает форму.
 * Публикация выполняется core асинхронно; результат/ошибку показывает владелец callback, оптимистического poll здесь нет. */
@Composable
fun CreatePollDialog(onDismiss: () -> Unit, onCreate: (String, List<String>, Boolean, Boolean) -> Unit) {
    var question by rememberSaveable { mutableStateOf("") }
    var variants by rememberSaveable { mutableStateOf("") }
    var multiple by rememberSaveable { mutableStateOf(false) }
    var custom by rememberSaveable { mutableStateOf(false) }
    val options = variants.lines().map { it.trim() }.filter { it.isNotEmpty() }
    val valid = validPollCreation(question.trim(), options)
    AlertDialog(onDismissRequest = onDismiss, title = { Text(stringResource(R.string.poll_create)) },
        text = { Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(question, { question = it }, label = { Text(stringResource(R.string.poll_question)) })
            OutlinedTextField(variants, { variants = it }, label = { Text(stringResource(R.string.poll_options_hint)) }, minLines = 3)
            Row(Modifier.clickable { multiple = false }) {
                RadioButton(!multiple, onClick = { multiple = false }); Text(stringResource(R.string.poll_single))
            }
            Row(Modifier.clickable { multiple = true }) {
                RadioButton(multiple, onClick = { multiple = true }); Text(stringResource(R.string.poll_multiple))
            }
            Row(Modifier.clickable { custom = !custom }) {
                Checkbox(custom, { custom = it }); Text(stringResource(R.string.poll_allow_custom))
            }
            if (!valid) Text(stringResource(R.string.poll_invalid_input), style = MaterialTheme.typography.bodySmall)
        } },
        confirmButton = { TextButton(enabled = valid, onClick = { onCreate(question.trim(), options, multiple, custom); onDismiss() }) {
            Text(stringResource(R.string.poll_create))
        } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } })
}

/** Опрос из message.feedback: результаты и проценты берутся из core, без локального пересчёта.
 * Пока снимок poll отсутствует, показывается placeholder; нажатие с готовым poll открывает выбор/ответы участников. */
@Composable
fun PollMessageBubble(message: Message, onVote: (Message, List<Int>, String) -> Unit, authorName: (String) -> String,
                      footer: @Composable () -> Unit = {}, header: @Composable () -> Unit = {}, onMenu: (() -> Unit)? = null) {
    var showVote by rememberSaveable(message.messageId) { mutableStateOf(false) }
    val poll = message.feedback?.poll
    Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 2.dp),
        horizontalArrangement = if (message.isOutgoing) Arrangement.End else Arrangement.Start) {
        Card(onClick = { if (onMenu != null) onMenu() else if (poll != null) showVote = true }, modifier = Modifier.widthIn(max = 300.dp),
            colors = CardDefaults.cardColors(containerColor = if (message.isOutgoing) MaterialTheme.colorScheme.primaryContainer
                else MaterialTheme.colorScheme.surfaceVariant)) {
            MessageWithFooter(message, footer, Modifier.padding(12.dp)) {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    header()
                    if (!message.isOutgoing) Text(message.author, style = MaterialTheme.typography.labelMedium)
                    if (poll == null) Text(stringResource(R.string.poll_pending))
                    else {
                        Text(poll.question, style = MaterialTheme.typography.titleMedium)
                        Text(stringResource(if (poll.multiple) R.string.poll_multiple else R.string.poll_single),
                            style = MaterialTheme.typography.labelSmall)
                        for (option in poll.options) {
                            Text(option.text)
                            LinearProgressIndicator(progress = { (option.percent / 100).toFloat().coerceIn(0f, 1f) },
                                modifier = Modifier.fillMaxWidth())
                            Text(stringResource(R.string.poll_option_result, option.count, option.percent),
                                style = MaterialTheme.typography.labelSmall)
                        }
                        Text(stringResource(R.string.poll_voters, poll.voters))
                        TextButton(onClick = { showVote = true }) { Text(stringResource(R.string.poll_vote_results)) }
                    }
                }
            }
        }
    }
    if (showVote && poll != null) PollVoteDialog(message, poll, { showVote = false }, onVote, authorName)
}

/** Локальный draft сохраняется по messageId, новые счётчики poll не перезаписывают редактируемый выбор.
 * Single выбирает пункт либо свой текст, multiple позволяет оба. preparePollVote исключает неактивные поля
 * и проверяет публикуемые байты; пустой выбор/текст снимает голос. onVote ставит команду, затем диалог закрывается. */
@Composable
internal fun PollVoteDialog(message: Message, poll: PollState, onDismiss: () -> Unit,
                           onVote: (Message, List<Int>, String) -> Unit, authorName: (String) -> String) {
    var selected by rememberSaveable(message.messageId) { mutableStateOf(ArrayList(poll.myChoices)) }
    var custom by rememberSaveable(message.messageId) { mutableStateOf(poll.myCustom) }
    var customSelected by rememberSaveable(message.messageId) { mutableStateOf(!poll.multiple && custom.isNotEmpty()) }
    val draft = preparePollVote(poll, selected, custom, customSelected)
    val valid = draft != null
    AlertDialog(onDismissRequest = onDismiss, title = { Text(poll.question) },
        text = { Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            for (option in poll.options) {
                val choose = {
                    customSelected = false
                    selected = if (!poll.multiple) arrayListOf(option.id)
                    else ArrayList(if (option.id in selected) selected.filter { it != option.id } else selected + option.id)
                }
                Row(Modifier.fillMaxWidth().clickable(onClick = choose)) {
                    if (poll.multiple) Checkbox(option.id in selected, onCheckedChange = { choose() })
                    else RadioButton(!customSelected && option.id in selected, onClick = choose)
                    Text(option.text)
                }
            }
            if (poll.allowCustom) {
                if (!poll.multiple) Row(Modifier.clickable { customSelected = true; selected = arrayListOf() }) {
                    RadioButton(customSelected, onClick = { customSelected = true; selected = arrayListOf() })
                    Text(stringResource(R.string.poll_own_answer))
                }
                OutlinedTextField(custom, { custom = it }, enabled = poll.multiple || customSelected, isError = !valid,
                    label = { Text(stringResource(R.string.poll_own_answer)) })
                if (!valid) Text(stringResource(R.string.poll_answer_error), color = MaterialTheme.colorScheme.error)
            }
            Text(stringResource(R.string.poll_voters, poll.voters))
            for (answer in poll.answers) {
                val parts = answer.choices.mapNotNull { id -> poll.options.find { it.id == id }?.text } +
                    listOfNotNull(answer.custom.takeIf { it.isNotEmpty() })
                Text("${authorName(answer.authorId)}: ${parts.joinToString(", ")}", style = MaterialTheme.typography.bodySmall)
            }
        } },
        confirmButton = { TextButton(enabled = valid, onClick = {
            draft?.let { onVote(message, it.choices, it.custom); onDismiss() }
        }) { Text(stringResource(R.string.poll_save)) } },
        dismissButton = { Row {
            TextButton(onClick = { onVote(message, emptyList(), ""); onDismiss() }) { Text(stringResource(R.string.poll_withdraw)) }
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) }
        } })
}

/** Уже поставленные реакции; каталог выбора находится в общем меню сообщения. */
@OptIn(ExperimentalLayoutApi::class, ExperimentalMaterial3Api::class)
@Composable
fun MessageReactions(message: Message, onReact: (Message, Int?) -> Unit) {
    val reactions = message.feedback?.reactions.orEmpty()
    if (reactions.isEmpty()) return
    CompositionLocalProvider(LocalMinimumInteractiveComponentEnforcement provides false) {
        FlowRow(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(4.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp)) {
            for (reaction in reactions) {
                FilterChip(selected = reaction.mine, modifier = Modifier.height(32.dp), onClick = {
                    LogManager.addLog("DEBUG", "POLL", "reaction toggled message=${message.messageId} emoji=${reaction.id} mine=${reaction.mine}")
                    onReact(message, reaction.id)
                }, label = { Text("${reaction.emoji} ${reaction.count}") })
            }
        }
    }
}
