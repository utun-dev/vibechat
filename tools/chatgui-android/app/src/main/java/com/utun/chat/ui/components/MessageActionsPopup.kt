package com.utun.chat.ui.components

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.*
import androidx.compose.ui.window.Popup
import androidx.compose.ui.window.PopupPositionProvider
import androidx.compose.ui.window.PopupProperties
import com.utun.chat.R
import com.utun.chat.data.LogManager
import com.utun.chat.data.Message
import com.utun.chat.data.ReactionState
import java.io.File

/** Два блока у сообщения: прокручиваемый каталог и узкий список доступных команд.
 * Закрывается до вызова команды; владелец передаёт актуальное сообщение и снимает выбор при смене беседы.
 * Положение ограничено окном, длинный список команд прокручивается отдельно. */
@Composable
internal fun MessageActionsPopup(message: Message, catalog: List<ReactionState>, onDismiss: () -> Unit,
                                 onReact: (Int?) -> Unit, onCopy: () -> Unit, onReply: () -> Unit,
                                 onEdit: () -> Unit, onVote: () -> Unit, onSave: () -> Unit, onDownload: () -> Unit,
                                 onCopyLink: () -> Unit, onSelectText: () -> Unit) {
    val configuration = LocalConfiguration.current
    val width = minOf(360.dp, (configuration.screenWidthDp - 32).coerceAtLeast(120).dp)
    val maxHeight = (configuration.screenHeightDp - 48).coerceAtLeast(120).dp
    val position = remember(message.isOutgoing) {
        object : PopupPositionProvider {
            override fun calculatePosition(anchorBounds: IntRect, windowSize: IntSize, layoutDirection: LayoutDirection,
                                           popupContentSize: IntSize): IntOffset {
                val x = if (message.isOutgoing) anchorBounds.right - popupContentSize.width else anchorBounds.left
                val y = if (anchorBounds.bottom + popupContentSize.height <= windowSize.height) anchorBounds.bottom
                    else anchorBounds.top - popupContentSize.height
                return IntOffset(x.coerceIn(0, (windowSize.width - popupContentSize.width).coerceAtLeast(0)),
                    y.coerceIn(0, (windowSize.height - popupContentSize.height).coerceAtLeast(0)))
            }
        }
    }
    fun choose(command: String, action: () -> Unit) {
        LogManager.addLog("DEBUG", "UI", "message menu command=$command message=${message.messageId}")
        onDismiss(); action()
    }
    val media = message.contentType !in setOf("", "text", "text/plain", "poll")
    val downloaded = media && !message.isLocked && message.filePath.isNotEmpty() && File(message.filePath).isFile
    Popup(popupPositionProvider = position, onDismissRequest = onDismiss, properties = PopupProperties(focusable = true)) {
        Column(Modifier.width(width).heightIn(max = maxHeight), verticalArrangement = Arrangement.spacedBy(6.dp),
            horizontalAlignment = if (message.isOutgoing) Alignment.End else Alignment.Start) {
            val scroll = rememberScrollState()
            val thumb = MaterialTheme.colorScheme.outline
            Surface(shape = RoundedCornerShape(12.dp), tonalElevation = 3.dp, shadowElevation = 4.dp) {
                Box(Modifier.fillMaxWidth().drawWithContent {
                    drawContent()
                    if (scroll.maxValue > 0) {
                        val track = size.width - 16.dp.toPx()
                        val length = (track * size.width / (size.width + scroll.maxValue)).coerceAtLeast(24.dp.toPx())
                        val x = 8.dp.toPx() + (track - length) * scroll.value / scroll.maxValue
                        drawRoundRect(thumb, Offset(x, size.height - 4.dp.toPx()), Size(length, 3.dp.toPx()), CornerRadius(2.dp.toPx()))
                    }
                }.padding(bottom = 7.dp)) {
                    Row(Modifier.horizontalScroll(scroll).padding(horizontal = 4.dp, vertical = 2.dp)) {
                        for (reaction in catalog) IconToggleButton(
                            checked = message.feedback?.reactions.orEmpty().any { it.mine && it.id == reaction.id },
                            onCheckedChange = { choose("react:${reaction.id}") { onReact(reaction.id) } }) {
                            Text(reaction.emoji, style = MaterialTheme.typography.headlineSmall)
                        }
                    }
                }
            }
            Surface(modifier = Modifier.width(minOf(220.dp, width - 24.dp)).weight(1f, fill = false),
                shape = RoundedCornerShape(12.dp), tonalElevation = 3.dp, shadowElevation = 4.dp) {
                Column(Modifier.verticalScroll(rememberScrollState()).padding(vertical = 4.dp)) {
                    DropdownMenuItem(text = { Text(stringResource(R.string.message_copy)) }, enabled = !message.isLocked,
                        onClick = { choose("copy", onCopy) })
                    DropdownMenuItem(text = { Text(stringResource(R.string.reply)) }, onClick = { choose("reply", onReply) })
                    if (message.messageId.length == 64) DropdownMenuItem(text = { Text(stringResource(R.string.copy_link)) },
                        onClick = { choose("copy_link", onCopyLink) })
                    if (message.contentType in setOf("text", "text/plain") && message.text.isNotBlank())
                        DropdownMenuItem(text = { Text(stringResource(R.string.select_text)) },
                            onClick = { choose("select_text", onSelectText) })
                    if (message.isOutgoing && !message.isEncrypted && message.contentType in setOf("text", "text/plain"))
                        DropdownMenuItem(text = { Text(stringResource(R.string.edit_message)) }, onClick = { choose("edit", onEdit) })
                    if (message.contentType == "poll") DropdownMenuItem(text = { Text(stringResource(R.string.poll_vote_results)) },
                        enabled = message.feedback?.poll != null, onClick = { choose("vote", onVote) })
                    DropdownMenuItem(text = { Text(stringResource(R.string.poll_clear_reactions)) },
                        onClick = { choose("clear_reactions") { onReact(null) } })
                    if (media) {
                        DropdownMenuItem(text = { Text(stringResource(R.string.message_save_file)) }, enabled = downloaded,
                            onClick = { choose("save", onSave) })
                        if (!downloaded) DropdownMenuItem(text = { Text(stringResource(R.string.message_download)) },
                            enabled = message.downloadState != "downloading", onClick = { choose("download", onDownload) })
                    }
                }
            }
        }
    }
}
