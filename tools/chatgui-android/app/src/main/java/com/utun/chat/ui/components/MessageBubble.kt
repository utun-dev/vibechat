package com.utun.chat.ui.components

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.icu.text.BreakIterator
import android.widget.Toast
import androidx.core.content.ContextCompat
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.BackHandler
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.InsertEmoticon
import androidx.compose.material.icons.outlined.Keyboard
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import com.utun.chat.ui.theme.ChatColors
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.utun.chat.data.Message
import com.utun.chat.data.LogManager
import com.utun.chat.data.VideoFileMetadata
import com.utun.chat.data.PhotoUtil
import com.utun.chat.viewmodel.ChatViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

private fun isEmojiCodePoint(cp: Int): Boolean =
    (cp in 0x1F000..0x1FAFF) ||
    (cp in 0x2600..0x27BF) ||
    (cp in 0x2300..0x23FF) ||
    (cp in 0x2B00..0x2BFF) ||
    (cp in 0xFE00..0xFE0F) ||
    cp == 0x200D || cp == 0x20E3

private fun detectEmojiOnly(text: String): Int {
    val t = text.trim()
    if (t.isEmpty()) return 0
    val it = BreakIterator.getCharacterInstance(java.util.Locale.US)
    it.setText(t)
    var count = 0
    var start = it.first()
    var end = it.next()
    while (end != BreakIterator.DONE) {
        val cluster = t.substring(start, end)
        if (!cluster.isBlank()) {
            val allEmoji = cluster.codePoints().allMatch { cp -> isEmojiCodePoint(cp) }
            if (!allEmoji) return 0
            count++
            if (count > 3) return 0
        }
        start = end
        end = it.next()
    }
    return if (count in 1..3) count else 0
}

@Composable
private fun deliveryLabel(message: Message): String =
    if (message.deliveryStatusRes == 0) "" else stringResource(message.deliveryStatusRes)

/* Одна нижняя строка внутри пузыря: реакции слева, время справа. Ширина учитывает оба блока. */
@Composable
internal fun MessageWithFooter(message: Message, footer: @Composable () -> Unit,
                               modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Layout(modifier = modifier, content = {
        Box { content() }
        Box { footer() }
        val edited = if (message.editedAt > 0) stringResource(R.string.message_edited_at, formatEditedTime(message.editedAt)) else ""
        Text(listOf(formatTime(message.ts), edited, deliveryLabel(message)).filter { it.isNotEmpty() }.joinToString(" · "),
            fontSize = 11.sp, color = ChatColors.Secondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }) { children, constraints ->
        val childConstraints = constraints.copy(minWidth = 0, minHeight = 0)
        val body = children[0].measure(childConstraints)
        val preferred = children[1].maxIntrinsicWidth(Int.MAX_VALUE).coerceAtMost(constraints.maxWidth)
        val gap = if (preferred > 0) 6.dp.roundToPx() else 0
        val time = children[2].measure(childConstraints.copy(
            maxWidth = (constraints.maxWidth - gap - if (preferred > 0) 32.dp.roundToPx() else 0).coerceAtLeast(0)))
        val width = maxOf(constraints.minWidth, body.width, minOf(constraints.maxWidth, preferred + gap + time.width))
        val reactions = children[1].measure(childConstraints.copy(maxWidth = (width - time.width - gap).coerceAtLeast(0)))
        val rowHeight = maxOf(reactions.height, time.height)
        val lastLineHeight = maxOf(time.height, minOf(reactions.height, 32.dp.roundToPx()))
        val top = body.height + 4.dp.roundToPx()
        layout(width, top + rowHeight) {
            body.placeRelative(0, 0)
            reactions.placeRelative(0, top)
            time.placeRelative(width - time.width, top + rowHeight - lastLineHeight + (lastLineHeight - time.height) / 2)
        }
    }
}

/** Отрисовка текста/media/приглашений без владения сетевой загрузкой: действия передаются caller.
 * Bitmap preview принадлежит composition; metadata reader освобождается независимо от успешности thumbnail.
 * Большие 1..3 emoji без reply показываются без обычного пузыря/footer; locked attachment не раскрывает подпись/preview. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun MessageBubble(message: Message, playbackState: ChatViewModel.PlaybackState = ChatViewModel.PlaybackState(),
                   onPlayVoice: (String, Int) -> Unit = { _, _ -> },
                   onSeekVoice: (String, Int, Int) -> Unit = { _, _, _ -> },
                   onDownloadFile: (Message) -> Unit = {},
                   onOpenFile: (Message) -> Unit = {},
                   onOpenImage: (Message) -> Unit = {},
                   onPlayVideo: (Message) -> Unit = {},
                   onLink: (String) -> Unit = {},
                   footer: @Composable () -> Unit = {}, header: @Composable () -> Unit = {}, onMenu: (() -> Unit)? = null) {
    val bgColor = if (message.isOutgoing) ChatColors.OutgoingBubble else ChatColors.IncomingBubble
    val textColor = ChatColors.Text
    val alignment = if (message.isOutgoing) Arrangement.End else Arrangement.Start
    val minBubbleWidth = (LocalConfiguration.current.screenWidthDp / 3).dp
    val voiceMaxWidth = (LocalConfiguration.current.screenWidthDp * 0.75f).dp

    val isVoice = message.contentType == "audio/opus"
    val isVideo = isVideoMessage(message)
    val isImage = isImageMessage(message)
    val isMedia = message.contentType.isNotEmpty() && !isVoice && message.contentType != "text/plain"
    val isActive = isVoice && message.filePath.isNotEmpty() && playbackState.filePath == message.filePath

    val emojiCount = if (isVoice || isVideo || isImage || isMedia) 0 else detectEmojiOnly(message.text)
    if (emojiCount > 0 && message.replyTo.isEmpty()) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 2.dp),
            horizontalArrangement = alignment
        ) {
            EmojiText(
                text = message.text.trim(),
                fontSize = if (emojiCount == 1) 100.sp else 51.sp,
                color = textColor,
                modifier = if (onMenu != null) Modifier.clickable(onClick = onMenu) else Modifier
            )
        }
        return
    }

    Row(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 2.dp),
        horizontalArrangement = alignment
    ) {
        MessageWithFooter(message, footer,
                modifier = Modifier
                    .widthIn(min = minOf(minBubbleWidth, if (isVoice) voiceMaxWidth else 300.dp),
                        max = if (isVoice) voiceMaxWidth else 300.dp)
                    // Тень рисуем до clip, чтобы выступающие справа и снизу края оставались видны.
                    .drawBehind {
                        val offset = 2.dp.toPx()
                        val radius = 12.dp.toPx()
                        drawRoundRect(ChatColors.BubbleShadow, topLeft = Offset(offset, offset), size = size,
                            cornerRadius = CornerRadius(radius, radius))
                    }
                    .clip(RoundedCornerShape(12.dp))
                    .background(bgColor)
                    .then(if (onMenu != null) Modifier.clickable(onClick = onMenu) else Modifier)
                    .padding(12.dp)
            ) {
            Column {
                header()
                if (!isVoice) Text(if (message.isOutgoing) stringResource(R.string.you) else message.author,
                    fontSize = 13.sp, fontWeight = FontWeight.Bold, color = ChatColors.Secondary)
                Spacer(Modifier.height(2.dp))

                when {
                    message.isLocked -> EncryptedAttachmentBubble(message, textColor, onDownloadFile)
                    message.inviteGroupName != null -> {
                        val clipboard = LocalClipboardManager.current
                        val context = LocalContext.current
                        LaunchedEffect(message.id, message.inviteGroupName) {
                            LogManager.addLog("DEBUG", "PM", "invitation rendered: seq=${message.id} " +
                                "group_name_bytes=${message.inviteGroupName.toByteArray().size} link_bytes=${message.text.length}")
                        }
                        Text(stringResource(R.string.group_invitation, message.inviteGroupName), fontSize = 17.sp, color = textColor)
                        Text(message.text, fontSize = 17.sp, color = Color(0xFF3390EC), textDecoration = TextDecoration.Underline,
                            maxLines = 1, overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.fillMaxWidth().combinedClickable(
                                onClick = { onLink(message.text) },
                                onLongClickLabel = stringResource(R.string.copy_link),
                                onLongClick = {
                                    try {
                                        clipboard.setText(AnnotatedString(message.text))
                                        LogManager.addLog("DEBUG", "PM", "invitation link copied: seq=${message.id} bytes=${message.text.length}")
                                    } catch (e: Exception) {
                                        LogManager.addLog("ERROR", "PM", "Invitation clipboard write failed seq=${message.id}: ${e.javaClass.simpleName}")
                                        Toast.makeText(context, R.string.invite_copy_failed, Toast.LENGTH_LONG).show()
                                    }
                                }))
                    }
                    isVoice -> VoiceBubble(message, textColor, isActive, playbackState, onPlayVoice, onSeekVoice, onDownloadFile)
                    isVideo -> VideoBubble(message, textColor, onDownloadFile, onPlayVideo)
                    isImage -> ImageBubble(message, textColor, onDownloadFile, onOpenImage)
                    isMedia -> FileBubble(message, textColor, onDownloadFile, onOpenFile)
                    else -> {
                        EmojiText(message.text, fontSize = 17.sp, color = textColor, onLink = onLink, onTextClick = onMenu)
                    }
                }
                if (message.caption.isNotEmpty() && !message.isLocked) {
                    Spacer(Modifier.height(4.dp))
                    EmojiText(message.caption, fontSize = 17.sp, color = textColor, onLink = onLink)
                }
            }
        }
    }
}
@Composable
private fun EncryptedAttachmentBubble(message: Message, textColor: Color, onDownload: (Message) -> Unit) {
    val canDownload = message.encryptionState == "download" || message.encryptionState == "error"
    val status = when {
        message.downloadState == "error" -> R.string.download_failed_retry
        message.downloadState == "downloading" -> R.string.downloading
        message.encryptionState == "download" -> R.string.attachment_encrypted_download
        message.encryptionState == "requesting" -> R.string.attachment_key_requesting
        message.encryptionState == "pending" -> R.string.attachment_key_pending
        message.encryptionState == "denied" -> R.string.attachment_key_denied
        message.encryptionState == "decrypting" -> R.string.attachment_decrypting
        else -> R.string.attachment_decrypt_failed
    }
    LaunchedEffect(message.messageId, message.encryptionState, message.downloadState) {
        LogManager.addLog("DEBUG", "Attachment", "encrypted card id=${message.messageId} " +
            "state=${message.encryptionState} download=${message.downloadState} action=$canDownload")
    }
    Column(Modifier.clickable(enabled = canDownload) { onDownload(message) }) {
        Text("🔒 " + stringResource(R.string.attachment_encrypted_title), fontSize = 15.sp,
            color = textColor, fontWeight = FontWeight.Bold)
        Spacer(Modifier.height(2.dp))
        Text(listOf(formatFileSize(message.fileSize), stringResource(status)).filter { it.isNotEmpty() }.joinToString(" · "),
            fontSize = 12.sp, color = ChatColors.Secondary)
    }
}

@Composable
private fun VoiceBubble(message: Message, textColor: Color, isActive: Boolean,
                         ps: ChatViewModel.PlaybackState,
                         onPlay: (String, Int) -> Unit,
                         onSeek: (String, Int, Int) -> Unit,
                         onDownload: (Message) -> Unit = {}) {
    val wf = message.waveform
    val wfCount = wf.size
    val durMs = message.voiceDurationMs
    val playing = isActive && ps.playing
    val paused = isActive && ps.paused
    val elapsedMs = ps.elapsedMs
    val progress = ps.progress
    val downloading = message.downloadState == "downloading"
    val notDownloaded = message.filePath.isEmpty() && !downloading

    Column {
        Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Box(
                modifier = Modifier.size(32.dp).background(ChatColors.Background, CircleShape)
                    .clickable {
                        when {
                            downloading -> {}
                            notDownloaded -> onDownload(message)
                            else -> onPlay(message.filePath, durMs)
                        }
                    },
                contentAlignment = Alignment.Center
            ) {
                Text(
                    when {
                        downloading -> "\u23F3"
                        notDownloaded -> "\u2B07"
                        playing && !paused -> "\u23F8"
                        else -> "\u25B6"
                    },
                    fontSize = 14.sp, color = ChatColors.Text
                )
            }
            Spacer(Modifier.width(8.dp))

            if (wfCount >= 2) {
                Box(
                    modifier = Modifier.weight(1f).height(28.dp)
                        .pointerInput(message.filePath) {
                            detectTapGestures { offset ->
                                if (message.filePath.isEmpty()) return@detectTapGestures
                                val seekFrac = (offset.x / size.width).coerceIn(0f, 1f)
                                val seekMs = (seekFrac * durMs).toInt()
                                onSeek(message.filePath, seekMs, durMs)
                            }
                        }
                ) {
                    Canvas(modifier = Modifier.fillMaxSize()) {
                        val barCount = wfCount
                        val slotW = size.width / barCount
                        val barW = slotW * 0.75f
                        val ph = size.height
                        val showCursor = playing || paused
                        val playedIdx = if (showCursor && progress > 0f) (progress * barCount).toInt() else 0
                        for (i in 0 until barCount) {
                            val level = wf[i].coerceIn(0f, 1f)
                            val barH = (2f + level * (ph - 2f)).coerceAtMost(ph)
                            val x = i * slotW
                            val y = ph - barH
                            val played = showCursor && i < playedIdx
                            drawRect(
                                color = if (played) ChatColors.Accent else ChatColors.Secondary,
                                topLeft = Offset(x, y),
                                size = Size(barW, barH)
                            )
                        }
                        if (showCursor && progress > 0f && progress < 1f) {
                            val cursorX = progress * size.width
                            drawLine(Color(0xFFFFD600), Offset(cursorX, 0f), Offset(cursorX, ph), strokeWidth = 2.5f)
                        }
                    }
                }
            }
        }

        Spacer(Modifier.height(4.dp))

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.Bottom
        ) {
            Text(
                when {
                    downloading -> stringResource(R.string.downloading)
                    notDownloaded -> stringResource(R.string.tap_download)
                    playing || paused -> "${formatDurationMs(elapsedMs)} / ${formatDurationMs(durMs)}"
                    else -> formatDurationMs(durMs)
                },
                fontSize = 12.sp,
                color = ChatColors.Secondary
            )
        }
    }
}

@Composable
private fun FileBubble(message: Message, textColor: Color, onDownload: (Message) -> Unit = {},
                       onOpen: (Message) -> Unit = {}) {
    val state = message.downloadState
    val stateIcon = when {
        state == "downloaded" -> "\u2705"
        state == "downloading" -> "\u23F3"
        state == "error" -> "\u26A0\uFE0F"
        else -> "\u2B07\uFE0F"
    }
    val stateText = when {
        state == "downloaded" -> stringResource(R.string.tap_save)
        state == "downloading" -> stringResource(R.string.downloading)
        state == "error" -> stringResource(R.string.download_failed_retry)
        else -> stringResource(R.string.tap_download)
    }

    Column(
        modifier = Modifier.clickable {
            when {
                state == "downloaded" -> onOpen(message)
                state == "downloading" -> Unit
                else -> onDownload(message)
            }
        }
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stateIcon, fontSize = 18.sp)
            Spacer(Modifier.width(6.dp))
            Text(message.text, fontSize = 15.sp, color = textColor, fontWeight = FontWeight.Bold)
        }
        Spacer(Modifier.height(2.dp))
        Text(
            "${formatFileSize(message.fileSize)} · $stateText",
            fontSize = 12.sp,
            color = ChatColors.Secondary
        )
    }
}


private fun isVideoMessage(message: Message): Boolean {
    return message.contentType.startsWith("video/")
}

private fun isImageMessage(message: Message): Boolean = message.contentType.startsWith("image/")

@Composable
private fun VideoBubble(message: Message, textColor: Color,
                        onDownload: (Message) -> Unit = {},
                        onPlay: (Message) -> Unit = {}) {
    val downloading = message.downloadState == "downloading"
    val downloaded = message.filePath.isNotEmpty()
    val isError = message.downloadState == "error"

    var thumb by remember(message.filePath) { mutableStateOf<Bitmap?>(null) }
    var durMs by remember(message.filePath) { mutableStateOf(0) }

    LaunchedEffect(message.filePath) {
        if (message.filePath.isNotEmpty()) {
            val meta = try { withContext(Dispatchers.IO) { VideoFileMetadata().use { it.preview(message.filePath) } } }
            catch (e: CancellationException) { throw e }
            catch (e: Exception) { LogManager.addLog("WARN", "Video", "Preview failed id=${message.id}: $e"); null }
            thumb = meta?.bitmap
            durMs = meta?.durationMs?.takeIf { it > 0 } ?: message.videoDurationMs
        }
    }

    Column(
        modifier = Modifier.clickable {
            when {
                downloading -> {}
                !downloaded -> onDownload(message)
                else -> onPlay(message)
            }
        }
    ) {
        Box(
            modifier = Modifier.size(width = 220.dp, height = 140.dp)
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF1E1E23))
        ) {
            thumb?.let { Image(it.asImageBitmap(), contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize()) }
            Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Box(modifier = Modifier.size(44.dp).background(Color(0xCC000000), CircleShape), contentAlignment = Alignment.Center) {
                    Text(
                        when { downloading -> "\u23F3"; isError -> "\u267B"; !downloaded -> "\u2B07"; else -> "\u25B6" },
                        fontSize = 18.sp, color = Color.White
                    )
                }
            }
            if (durMs > 0) {
                Text(
                    formatDurationMs(durMs), fontSize = 11.sp, color = Color.White,
                    modifier = Modifier.align(Alignment.BottomEnd).padding(6.dp)
                        .background(Color(0xCC000000), RoundedCornerShape(4.dp))
                        .padding(horizontal = 4.dp, vertical = 1.dp)
                )
            }
        }
        Spacer(Modifier.height(4.dp))
        Text(message.text, fontSize = 15.sp, color = textColor, fontWeight = FontWeight.Bold,
             maxLines = 1, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.height(2.dp))
        Text(
            buildString {
                if (message.videoWidth > 0 && message.videoHeight > 0)
                    append("${message.videoWidth}×${message.videoHeight} · ")
                val sz = formatFileSize(message.fileSize)
                if (sz.isNotEmpty()) append("$sz · ")
                if (message.fileSize > 0 && message.videoDurationMs > 0)
                    append("${formatBitrate(message.fileSize, message.videoDurationMs)} · ")
                append(when {
                    downloading -> stringResource(R.string.downloading)
                    isError -> stringResource(R.string.failed_retry)
                    !downloaded -> stringResource(R.string.tap_download)
                    else -> stringResource(R.string.tap_play)
                })
            }.trimEnd(),
            fontSize = 12.sp, color = ChatColors.Secondary
        )
    }
}

@Composable
private fun ImageBubble(message: Message, textColor: Color,
                        onDownload: (Message) -> Unit = {},
                        onOpen: (Message) -> Unit = {}) {
    val downloading = message.downloadState == "downloading"
    val downloaded = message.filePath.isNotEmpty()
    val isError = message.downloadState == "error"

    var bmp by remember(message.filePath) { mutableStateOf<Bitmap?>(null) }
    var decoded by remember(message.filePath) { mutableStateOf(false) }

    LaunchedEffect(message.id, message.filePath, message.downloadState) {
        LogManager.addLog("DEBUG", "Image", "Preview state id=${message.id} state=${message.downloadState} " +
            "local=$downloaded downloadControl=${!downloaded}")
    }
    LaunchedEffect(message.filePath) {
        if (message.filePath.isNotEmpty()) {
            bmp = withContext(Dispatchers.IO) {
                try {
                    val image = PhotoUtil.decodeFitted(message.filePath, 640)
                    if (image == null) LogManager.addLog("WARN", "Image", "Preview decode failed id=${message.id} path=${message.filePath}")
                    else LogManager.addLog("DEBUG", "Image", "Preview ready id=${message.id} size=${image.width}x${image.height}")
                    image
                } catch (e: Exception) {
                    LogManager.addLog("WARN", "Image", "Preview decode failed id=${message.id} path=${message.filePath}: ${e.message}")
                    null
                }
            }
            decoded = true
        }
    }

    val resText = if (message.imageWidth > 0 || message.imageHeight > 0)
        "${message.imageWidth}×${message.imageHeight} · " else ""

    Column(
        modifier = Modifier.clickable {
            when {
                downloading -> {}
                downloaded -> onOpen(message)
                else -> onDownload(message)
            }
        }
    ) {
        Box(
            modifier = Modifier.width(220.dp).height(if (message.imageWidth > 0 && message.imageHeight > 0)
                (220f * message.imageHeight / message.imageWidth).coerceIn(80f, 300f).dp else 160.dp)
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF1E1E23))
        ) {
            bmp?.let { Image(it.asImageBitmap(), contentDescription = null, contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize()) }
            if (downloaded && decoded && bmp == null) Text(stringResource(R.string.image_failed), color = textColor,
                modifier = Modifier.align(Alignment.Center).padding(8.dp))
            if (!downloaded) Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Box(modifier = Modifier.size(44.dp).background(Color(0xCC000000), CircleShape), contentAlignment = Alignment.Center) {
                    Text(
                        when { downloading -> "\u23F3"; isError -> "\u267B"; else -> "\u2B07" },
                        fontSize = 18.sp, color = Color.White
                    )
                }
            }
        }
        Spacer(Modifier.height(4.dp))
        Text(message.text, fontSize = 15.sp, color = textColor, fontWeight = FontWeight.Bold,
             maxLines = 1, overflow = TextOverflow.Ellipsis)
        Spacer(Modifier.height(2.dp))
        Text(
            resText + formatFileSize(message.fileSize) + " · " + when {
                downloading -> stringResource(R.string.downloading)
                isError -> stringResource(R.string.failed_retry)
                !downloaded -> stringResource(R.string.tap_download)
                else -> stringResource(R.string.tap_open)
            },
            fontSize = 12.sp, color = ChatColors.Secondary
        )
    }
}

private fun formatFileSize(bytes: Long): String {
    if (bytes <= 0) return ""
    if (bytes < 1024) return "${bytes}B"
    val kb = bytes / 1024.0
    if (kb < 1024.0) return if (kb < 10.0) "%.1fKB".format(kb) else "%.0fKB".format(kb)
    val mb = kb / 1024.0
    if (mb < 1024.0) return if (mb < 10.0) "%.1fMB".format(mb) else "%.0fMB".format(mb)
    val gb = mb / 1024.0
    return "%.1fGB".format(gb)
}

private fun formatBitrate(bytes: Long, durMs: Int): String {
    if (bytes <= 0 || durMs <= 0) return ""
    val kbps = bytes * 8 / durMs
    return if (kbps >= 1000) "%.1fMbps".format(kbps / 1000.0) else "${kbps}kbps"
}

private fun formatDurationMs(ms: Int): String {
    val secs = ms.coerceAtLeast(0) / 1000
    return "%d:%02d".format(secs / 60, secs % 60)
}

private val LOCK_SWIPE_THRESHOLD_DP = 80f
private const val HOLD_THRESHOLD_MS = 250L

/** Общий ввод: короткий тап меняет voice/video, удержание 250 мс начинает захват, swipe вверх 80 dp фиксирует voice.
 * Отпускание отправляет, отменённый незакреплённый жест отменяет запись. Разрешение не запускает запись без нового удержания. */
@Composable
fun InputBar(
    text: TextFieldValue,
    onTextChange: (TextFieldValue) -> Unit,
    onSend: () -> Unit,
    enabled: Boolean = true,
    isRecording: Boolean = false,
    isRecordingLocked: Boolean = false,
    recordingDurationMs: Int = 0,
    recordingLevel: Float = 0f,
    onPttPressed: () -> Unit = {},
    onPttReleased: () -> Unit = {},
    onPttLock: () -> Unit = {},
    onVoiceSend: () -> Unit = {},
    onVoiceCancel: () -> Unit = {},
    onAttachClicked: () -> Unit = {},
    pendingAttachmentName: String = "",
    pendingAttachmentSize: Long = 0L,
    onClearAttachment: () -> Unit = {},
    isVideoMode: Boolean = false,
    onToggleMode: () -> Unit = {},
    onVideoRecordOpen: () -> Unit = {},
    focusRevision: Long = 0,
    editing: Boolean = false,
    modifier: Modifier = Modifier
) {
    val ctx = LocalContext.current
    val density = LocalDensity.current
    val focusManager = LocalFocusManager.current
    val keyboard = LocalSoftwareKeyboardController.current
    val inputFocus = remember { FocusRequester() }
    LaunchedEffect(focusRevision) {
        if (focusRevision > 0 && enabled && !isRecording) { inputFocus.requestFocus(); keyboard?.show() }
    }
    var emojiPanel by remember { mutableStateOf(false) }
    fun closeEmojiPanel(reason: String) {
        if (!emojiPanel) return
        emojiPanel = false
        LogManager.addLog("DEBUG", "Composer", "emoji panel closed: $reason")
    }
    BackHandler(enabled = emojiPanel) { closeEmojiPanel("back") }
    LaunchedEffect(isRecording, enabled) {
        if (isRecording || !enabled) closeEmojiPanel(if (isRecording) "recording" else "disabled")
    }
    DisposableEffect(Unit) {
        onDispose { if (emojiPanel) LogManager.addLog("DEBUG", "Composer", "emoji panel closed: conversation left") }
    }
    val lockThresholdPx = with(density) { LOCK_SWIPE_THRESHOLD_DP.dp.toPx() }

    var hasMicPerm by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED
        )
    }
    val permLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        hasMicPerm = granted
        LogManager.addLog(if (granted) "INFO" else "WARN", "Voice", "Recording microphone permission=$granted")
        if (!granted) Toast.makeText(ctx, R.string.microphone_access_required, Toast.LENGTH_LONG).show()
    }

    val hasAttachment = pendingAttachmentName.isNotEmpty()

    Column(modifier = modifier.fillMaxWidth().padding(8.dp)) {
        /* Attachment preview bar */
        if (hasAttachment) {
            Surface(
                modifier = Modifier.fillMaxWidth().height(36.dp).padding(bottom = 4.dp),
                shape = RoundedCornerShape(8.dp),
                color = MaterialTheme.colorScheme.primaryContainer
            ) {
                Row(
                    modifier = Modifier.fillMaxSize().padding(horizontal = 8.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text("\uD83D\uDCCE", fontSize = 14.sp)
                    Spacer(Modifier.width(6.dp))
                    Text(
                        pendingAttachmentName,
                        fontSize = 14.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f)
                    )
                    if (pendingAttachmentSize > 0) {
                        Text(
                            formatFileSize(pendingAttachmentSize),
                            fontSize = 12.sp,
                            color = ChatColors.Secondary
                        )
                        Spacer(Modifier.width(8.dp))
                    }
                    IconButton(
                        onClick = onClearAttachment,
                        modifier = Modifier.size(24.dp)
                    ) {
                        Text("\u2715", fontSize = 14.sp)
                    }
                }
            }
        }

        Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            /* Attach button */
            if (!isRecording) {
                IconButton(onClick = onAttachClicked, enabled = enabled && !editing) {
                    Text("\uD83D\uDCCE", fontSize = 20.sp)
                }
            } else {
                Spacer(Modifier.width(4.dp))
            }

            /* Recording overlay or text input */
            if (isRecording) {
                Surface(
                    modifier = Modifier.weight(1f).height(48.dp),
                    shape = RoundedCornerShape(8.dp),
                    color = Color(0xFF54252A)
                ) {
                    Row(
                        modifier = Modifier.fillMaxSize().padding(horizontal = 12.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = "\uD83D\uDD34 ${formatDuration(recordingDurationMs)}",
                            color = Color.White,
                            fontWeight = FontWeight.Bold,
                            fontSize = 16.sp,
                            modifier = Modifier.weight(1f)
                        )
                        val lvlPct = (recordingLevel * 40f).coerceIn(0f, 1f)
                        LinearProgressIndicator(
                            progress = { lvlPct },
                            modifier = Modifier.width(60.dp).height(4.dp).padding(end = 8.dp),
                            color = Color.White,
                            trackColor = Color.White.copy(alpha = 0.3f),
                        )
                    }
                }
            } else {
                OutlinedTextField(
                    value = text,
                    onValueChange = onTextChange,
                    enabled = enabled,
                    modifier = Modifier.weight(1f).focusRequester(inputFocus).onFocusChanged {
                        if (it.isFocused) closeEmojiPanel("text input focused")
                    },
                    placeholder = { Text(stringResource(R.string.message_placeholder)) },
                    leadingIcon = {
                        IconButton(enabled = enabled, onClick = {
                            if (emojiPanel) {
                                closeEmojiPanel("keyboard button")
                                inputFocus.requestFocus()
                                keyboard?.show()
                            } else {
                                // Compose сворачивает выделение при потере фокуса; сохраняем диапазон для вставки.
                                focusManager.clearFocus()
                                onTextChange(text.copy(composition = null))
                                keyboard?.hide()
                                emojiPanel = true
                                LogManager.addLog("DEBUG", "Composer",
                                    "emoji panel opened: selection=${text.selection.start}..${text.selection.end}")
                            }
                        }) {
                            Icon(if (emojiPanel) Icons.Outlined.Keyboard else Icons.Outlined.InsertEmoticon,
                                contentDescription = stringResource(if (emojiPanel) R.string.open_keyboard else R.string.open_emoji))
                        }
                    },
                    maxLines = 4
                )
            }

            Spacer(Modifier.width(4.dp))

            if (isRecording && isRecordingLocked) {
                /* Locked: cancel + send voice */
                IconButton(onClick = onVoiceCancel) {
                    Text("\u2715", fontSize = 20.sp, color = Color(0xFFFFB4AB))
                }
                IconButton(onClick = onVoiceSend) {
                    Text("\u25B6", fontSize = 20.sp, color = Color(0xFF81C784))
                }
            } else if (!isRecording && (editing || text.text.isNotBlank() || hasAttachment)) {
                /* Send button — round, primary color */
                val sendColor = MaterialTheme.colorScheme.primary
                Box(
                    modifier = Modifier
                        .size(44.dp)
                        .background(sendColor, CircleShape)
                        .clickable(enabled = enabled && (text.text.isNotBlank() || hasAttachment)) { onSend() },
                    contentAlignment = Alignment.Center
                ) {
                    Text(if (editing) "✓" else "\u27A4", fontSize = 22.sp, color = MaterialTheme.colorScheme.onPrimary)
                }
            } else {
                /* Unified mode button — short tap toggles voice/video, hold activates */
                val pttBg = if (isRecording) Color(0xFFB32632) else Color.Transparent
                Box(
                    modifier = Modifier
                        .size(48.dp)
                        .background(pttBg, RoundedCornerShape(4.dp))
                        .pointerInput(isVideoMode, hasMicPerm, enabled) {
                            if (!enabled) return@pointerInput
                            awaitPointerEventScope {
                                while (true) {
                                    val down = awaitFirstDown(requireUnconsumed = false)
                                    val downId = down.id
                                    down.consume()

                                    /* wait: release before threshold = tap, else hold */
                                    var released = false
                                    withTimeoutOrNull(HOLD_THRESHOLD_MS) {
                                        while (true) {
                                            val ev = awaitPointerEvent()
                                            val c = ev.changes.firstOrNull { it.id == downId } ?: return@withTimeoutOrNull
                                            if (!c.pressed) { released = true; return@withTimeoutOrNull }
                                            c.consume()
                                        }
                                    }

                                    if (released) {
                                        onToggleMode()
                                        continue
                                    }

                                    if (isVideoMode) {
                                        onVideoRecordOpen()
                                        while (true) {
                                            val ev = awaitPointerEvent()
                                            val c = ev.changes.firstOrNull { it.id == downId } ?: break
                                            c.consume()
                                            if (!c.pressed) break
                                        }
                                        continue
                                    }

                                    if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                                        try { permLauncher.launch(Manifest.permission.RECORD_AUDIO) } catch (e: Exception) {
                                            LogManager.addLog("ERROR", "Voice", "Cannot request recording microphone permission: $e")
                                            Toast.makeText(ctx, R.string.microphone_request_failed, Toast.LENGTH_LONG).show()
                                        }
                                        while (true) {
                                            val ev = awaitPointerEvent()
                                            val c = ev.changes.firstOrNull { it.id == downId } ?: break
                                            c.consume()
                                            if (!c.pressed) break
                                        }
                                        continue
                                    }

                                    var locked = false
                                    var ended = false
                                    var totalDragY = 0f
                                    var lastY = down.position.y
                                    try {
                                        onPttPressed()
                                        while (true) {
                                            val ev = awaitPointerEvent()
                                            val c = ev.changes.firstOrNull { it.id == downId } ?: break
                                            if (c.pressed) {
                                                val dy = c.position.y - lastY
                                                lastY = c.position.y
                                                totalDragY += dy
                                                if (totalDragY < -lockThresholdPx && !locked) { onPttLock(); locked = true }
                                                c.consume()
                                            } else {
                                                ended = true; c.consume(); break
                                            }
                                        }
                                    } finally {
                                        if (ended) onPttReleased()
                                        else if (!locked) {
                                            LogManager.addLog("INFO", "Voice", "Recording gesture canceled before release")
                                            onVoiceCancel()
                                        }
                                    }
                                }
                            }
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        if (isVideoMode) "\uD83D\uDCF7" else "\uD83C\uDFA4",
                        fontSize = 20.sp,
                        color = if (isRecording) Color.White else Color.Unspecified
                    )
                }
            }
        }
        if (emojiPanel && !isRecording && enabled) EmojiPickerPanel(onSelect = { emoji ->
            val start = text.selection.min
            val end = text.selection.max
            val updated = text.text.replaceRange(start, end, emoji)
            onTextChange(TextFieldValue(updated, selection = TextRange(start + emoji.length)))
            LogManager.addLog("DEBUG", "Composer", "emoji inserted: start=$start replaced=${end - start} length=${emoji.length}")
        })
    }
}

private fun formatDuration(ms: Int): String {
    val secs = ms.coerceAtLeast(0) / 1000
    return "%d:%02d".format(secs / 60, secs % 60)
}

private fun formatEditedTime(ts: Long): String {
    val today = java.text.SimpleDateFormat("yyyyMMdd", java.util.Locale.getDefault())
    val format = if (today.format(java.util.Date(ts)) == today.format(java.util.Date())) "HH:mm" else "dd.MM.yyyy HH:mm"
    return java.text.SimpleDateFormat(format, java.util.Locale.getDefault()).format(java.util.Date(ts))
}

private fun formatTime(ts: Long): String {
    if (ts == 0L) return ""
    val dt = java.time.Instant.ofEpochMilli(ts).atZone(java.time.ZoneId.systemDefault())
    return "%02d:%02d".format(dt.hour, dt.minute)
}
