package com.utun.chat.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.*
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.onLongClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.unit.dp
import com.utun.chat.R
import com.utun.chat.data.Message

/** Меню удержания поверх ссылок/плеера/голосования: наблюдает Initial-pass даже за уже потреблённым down.
 * Отпускание/движение дальше touchSlop отменяет удержание; после timeout вызывает актуальный callback
 * и поглощает события до отпускания всех указателей. Accessibility onLongClick вызывает тот же callback. */
@Composable
internal fun Modifier.messageLongPress(label: String, onLongPress: () -> Unit): Modifier {
    val action by rememberUpdatedState(onLongPress)
    return semantics { onLongClick(label) { action(); true } }.pointerInput(Unit) {
        awaitEachGesture {
            val down = awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
            val releasedOrMoved = withTimeoutOrNull(viewConfiguration.longPressTimeoutMillis) {
                do {
                    val event = awaitPointerEvent(PointerEventPass.Initial)
                    val change = event.changes.firstOrNull { it.id == down.id } ?: return@withTimeoutOrNull true
                    if (!change.pressed || (change.position - down.position).getDistance() > viewConfiguration.touchSlop)
                        return@withTimeoutOrNull true
                } while (true)
            }
            if (releasedOrMoved == null) {
                action()
                do {
                    val event = awaitPointerEvent(PointerEventPass.Initial)
                    event.changes.forEach { it.consume() }
                } while (event.changes.any { it.pressed })
            }
        }
    }
}

/** Цитата оригинала: null показывает отсутствие, locked скрывает текст, media/poll используют подписи типа/вопрос опроса.
 * Обычный текст сводится к одной строке до 160 UTF-16 единиц без разрыва surrogate-пары на границе.
 * onClick делегируется владельцу, в том числе при отсутствующем оригинале; сама цитата не запрашивает БД/сеть. */
@Composable
fun ReplyQuote(original: Message?, modifier: Modifier = Modifier, quote: String = "", onClick: (() -> Unit)? = null) {
    val preview = when {
        quote.isNotEmpty() -> quote
        original == null -> stringResource(R.string.reply_original_missing)
        original.isLocked -> stringResource(R.string.attachment_encrypted_title)
        original.contentType == "audio/opus" -> stringResource(R.string.reply_voice)
        original.contentType.startsWith("image/") -> stringResource(R.string.photo)
        original.contentType.startsWith("video/") -> stringResource(R.string.video_message)
        original.contentType == "poll" -> original.feedback?.poll?.question ?: stringResource(R.string.reply_poll)
        else -> original.text.replace(Regex("\\s+"), " ").take(160).dropLastWhile { it.isHighSurrogate() }
    }
    Row(modifier.then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
        .height(IntrinsicSize.Min).padding(vertical = 4.dp)) {
        Box(Modifier.width(3.dp).fillMaxHeight().background(MaterialTheme.colorScheme.primary))
        Column(Modifier.padding(horizontal = 8.dp).weight(1f)) {
            Text(original?.let { if (it.isOutgoing) stringResource(R.string.you) else it.author } ?: stringResource(R.string.reply),
                style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.primary,
                maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(preview, style = MaterialTheme.typography.bodySmall, maxLines = if (quote.isEmpty()) 1 else Int.MAX_VALUE,
                overflow = if (quote.isEmpty()) TextOverflow.Ellipsis else TextOverflow.Clip)
        }
    }
}

/** Отдельный read-only текст не конфликтует с меню удержания/ссылками пузыря; TextFieldValue сохраняет маркеры при повороте. */
@Composable
fun MessageSelectionDialog(text: String, onQuote: (String) -> Unit, onDismiss: () -> Unit) {
    var value by rememberSaveable(text, stateSaver = TextFieldValue.Saver) { mutableStateOf(TextFieldValue(text)) }
    val fragment = com.utun.chat.data.quoteFragment(text, value.selection.start, value.selection.end)
    AlertDialog(onDismissRequest = onDismiss, title = { Text(stringResource(R.string.select_text)) },
        text = {
            BasicTextField(value = value, onValueChange = { if (it.text == text) value = it }, readOnly = true,
                textStyle = MaterialTheme.typography.bodyLarge.copy(color = MaterialTheme.colorScheme.onSurface),
                modifier = Modifier.fillMaxWidth().heightIn(min = 120.dp, max = 360.dp).verticalScroll(rememberScrollState()))
        },
        confirmButton = { TextButton(onClick = { fragment?.let(onQuote) }, enabled = fragment != null) {
            Text(stringResource(R.string.quote_selection))
        } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } })
}
