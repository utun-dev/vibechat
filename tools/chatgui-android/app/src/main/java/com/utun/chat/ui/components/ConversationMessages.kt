package com.utun.chat.ui.components

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.lazy.*
import androidx.compose.runtime.*
import androidx.compose.material3.Text
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.ImageShader
import androidx.compose.ui.graphics.ShaderBrush
import androidx.compose.ui.graphics.TileMode
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.res.imageResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.utun.chat.R
import com.utun.chat.data.ConversationTarget
import com.utun.chat.data.LogManager
import com.utun.chat.data.Message
import com.utun.chat.viewmodel.ChatViewModel
import androidx.compose.runtime.saveable.rememberSaveable

/**
 * Общий список канала/PM: messages идут от старых к новым, reverseLayout размещает последний элемент внизу.
 * Ключ строки содержит target.nativeId и глобальный messageId; без него — пару isOutgoing/id, поскольку PM имеет два seq-потока.
 * Каналы получают меню по тапу/удержанию, реакции/голосование и цитаты из originals; PM использует обычные MessageBubble.
 * Меню читает актуальное сообщение по ID и сбрасывается при смене target. Редактирование доступно своему незашифрованному тексту.
 * utun:// передаётся onInviteLink, внешние ссылки допускаются только через MessageLinks.isWebUrl;
 * отсутствие обработчика/отказ разрешений логируются и показываются диалогом, загрузки и отправку выполняет владелец callbacks.
 */
@Composable
@OptIn(ExperimentalMaterial3Api::class)
fun ConversationMessages(target: ConversationTarget, messages: List<Message>, state: LazyListState,
                         playbackState: ChatViewModel.PlaybackState, modifier: Modifier = Modifier,
                         onPlayVoice: (String, Int) -> Unit, onSeekVoice: (String, Int, Int) -> Unit,
                         onOpenFile: (Message) -> Unit, onPlayVideo: (Message) -> Unit,
                         onDownloadFile: (Message) -> Unit = {}, onOpenImage: (Message) -> Unit = {},
                         onInviteLink: (String) -> Unit = {}, reactionCatalog: List<com.utun.chat.data.ReactionState> = emptyList(),
                         onVote: (Message, List<Int>, String) -> Unit = { _, _, _ -> },
                         onReact: (Message, Int?) -> Unit = { _, _ -> },
                         authorName: (String) -> String = { it },
                         onReply: (Message) -> Unit = {}, onOriginal: (String) -> Unit = {},
                         originals: Map<String, Message> = emptyMap(), highlightedId: String = "",
                         onEdit: (Message) -> Unit = {}, groupName: String = "", groupEd: String = "", onQuote: (Message, String) -> Unit = { _, _ -> }) {
    val clipboard = LocalClipboardManager.current
    val haptic = LocalHapticFeedback.current
    var selectedMessageId by remember(target) { mutableStateOf("") }
    var voteMessageId by remember(target) { mutableStateOf("") }
    LaunchedEffect(selectedMessageId, messages) {
        if (selectedMessageId.isNotEmpty() && messages.none { it.messageId == selectedMessageId }) {
            LogManager.addLog("WARN", "UI", "message menu target disappeared target=${target.nativeId} message=$selectedMessageId")
            selectedMessageId = ""
        }
    }
    messages.firstOrNull { it.messageId == voteMessageId && voteMessageId.isNotEmpty() }?.let { message ->
        message.feedback?.poll?.let { poll -> PollVoteDialog(message, poll, { voteMessageId = "" }, onVote, authorName) }
    }
    var selectingId by rememberSaveable(target.nativeId) { mutableStateOf("") }
    var selectingText by rememberSaveable(target.nativeId) { mutableStateOf("") }
    val selectingMessage = messages.firstOrNull { it.messageId == selectingId && selectingId.isNotEmpty() }
    if (selectingMessage != null) MessageSelectionDialog(selectingText,
        onQuote = { quote -> onQuote(selectingMessage, quote); selectingId = "" }, onDismiss = { selectingId = "" })
    val onLink = messageLinkHandler(onInviteLink)
    val background = ImageBitmap.imageResource(R.drawable.chat_background)
    LaunchedEffect(background) {
        LogManager.addLog("DEBUG", "UI", "Chat background ready: shared bg.webp tile=${background.width}x${background.height}")
    }
    // Плитка закреплена за областью списка; плотность экрана сохраняет размер узора в dp.
    val backgroundModifier = Modifier.drawWithCache {
        val brush = ShaderBrush(ImageShader(background, TileMode.Repeated, TileMode.Repeated))
        onDrawBehind {
            scale(density, density, pivot = Offset.Zero) {
                drawRect(brush, size = Size(size.width / density, size.height / density))
            }
        }
    }
    LazyColumn(modifier = modifier.then(backgroundModifier), state = state, reverseLayout = true) {
        item { Spacer(Modifier.height(8.dp)) }
        items(messages.reversed(), key = { "${target.nativeId}:${it.messageId.ifEmpty { "${it.isOutgoing}:${it.id}" }}" }) { message ->
            val group = (target as? ConversationTarget.Channel)?.id
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Bottom) {
                if (!message.isOutgoing) UserAvatar(group, message.authorAddress, message.author,
                    modifier = Modifier.padding(start = 8.dp, bottom = 4.dp), size = 54.4.dp)
                val actions = if (target is ConversationTarget.Channel && message.messageId.isNotEmpty())
                    Modifier.messageLongPress(stringResource(R.string.message_actions)) {
                        haptic.performHapticFeedback(HapticFeedbackType.LongPress)
                        selectedMessageId = message.messageId
                        LogManager.addLog("DEBUG", "UI", "message menu hold target=${target.nativeId} message=${message.messageId}")
                    } else Modifier
                Box(Modifier.weight(1f).then(actions).then(if (message.messageId == highlightedId && highlightedId.isNotEmpty())
                    Modifier.background(MaterialTheme.colorScheme.primary.copy(alpha = 0.18f)) else Modifier)) {
                    Column {
                        val footer: @Composable () -> Unit = {
                            if (target is ConversationTarget.Channel && message.messageId.isNotEmpty())
                                MessageReactions(message, onReact)
                        }
                        val header: @Composable () -> Unit = {
                            if (target is ConversationTarget.Channel && message.replyTo.isNotEmpty())
                                ReplyQuote(originals[message.replyTo], Modifier.fillMaxWidth(), message.replyQuote) { onOriginal(message.replyTo) }
                        }
                        val openMenu: (() -> Unit)? = if (target is ConversationTarget.Channel && message.messageId.isNotEmpty()) ({
                            selectedMessageId = message.messageId
                            LogManager.addLog("DEBUG", "UI", "message menu tap target=${target.nativeId} message=${message.messageId}")
                        }) else null
                        if (target is ConversationTarget.Channel && message.contentType == "poll")
                            PollMessageBubble(message, onVote, authorName, footer, header, openMenu)
                        else MessageBubble(message, playbackState, onPlayVoice, onSeekVoice, onDownloadFile,
                            onOpenFile, onOpenImage, onPlayVideo, onLink, footer, header, openMenu)
                    }
                    if (selectedMessageId == message.messageId && selectedMessageId.isNotEmpty()) {
                        MessageActionsPopup(message, reactionCatalog, onDismiss = { selectedMessageId = "" },
                            onReact = { onReact(message, it) }, onCopy = { clipboard.setText(AnnotatedString(message.text)) },
                            onReply = { onReply(message) }, onEdit = { onEdit(message) },
                            onVote = { voteMessageId = message.messageId }, onSave = { onOpenFile(message) },
                            onDownload = { onDownloadFile(message) },
                            onCopyLink = {
                                if (target is ConversationTarget.Channel)
                                    com.utun.chat.data.ObjectLinks.build(groupEd, groupName, message = message.messageId,
                                        messageLabel = com.utun.chat.data.ObjectLinks.preview(message))
                                        ?.let { clipboard.setText(AnnotatedString(it)) }
                            },
                            onSelectText = {
                                selectingText = message.text; selectingId = message.messageId
                                LogManager.addLog("DEBUG", "Reply", "text selection opened target=${target.nativeId} original=$selectingId")
                            })
                    }
                }
                if (message.isOutgoing) UserAvatar(group, message.authorAddress, message.author,
                    modifier = Modifier.padding(end = 8.dp, bottom = 4.dp), size = 54.4.dp)
            }
        }
    }
}
