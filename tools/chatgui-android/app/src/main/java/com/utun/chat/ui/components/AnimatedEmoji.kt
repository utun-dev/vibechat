package com.utun.chat.ui.components

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import android.icu.text.BreakIterator
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.text.InlineTextContent
import androidx.compose.foundation.text.appendInlineContent
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.Placeholder
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.PlaceholderVerticalAlign
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.TextUnit
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import com.airbnb.lottie.compose.LottieAnimation
import com.airbnb.lottie.compose.LottieCompositionSpec
import com.airbnb.lottie.compose.LottieConstants
import com.airbnb.lottie.compose.rememberLottieComposition
import com.airbnb.lottie.compose.rememberLottieAnimatable
import com.utun.chat.data.LogManager
import com.utun.chat.data.MessageLinks
import java.util.Locale
import kotlin.math.ceil
import kotlinx.coroutines.CancellationException

private const val EMOJI_PLAYBACK_MIN_MS = 4000f

// Соответствие Unicode и TGS такое же, как в desktop src/emoji.cpp.
private val animatedEmojiAssets = linkedMapOf(
    "✨" to "sparkles_emoji.tgs",
    "🏳️" to "white_flag_emoji.tgs",
    "👋" to "greeting.tgs",
    "✋" to "stop.tgs",
    "🤖" to "robot.tgs"
)
val animatedEmojiCharacters: List<String> = animatedEmojiAssets.keys.toList()
private val normalizedEmojiAssets = animatedEmojiAssets.mapKeys { it.key.removeSuffix("\uFE0F") }
private fun animationAsset(emoji: String): String? = normalizedEmojiAssets[emoji.removeSuffix("\uFE0F")]

/** Lottie загружает/кеширует пять общих TGS; RESUMED управляет проигрыванием, отмена эффекта сохраняет progress.
 * loop=false завершает целое число циклов не короче 4 секунд и разрешает replay; ошибка даёт Unicode fallback.
 */
@Composable
fun AnimatedEmoji(emoji: String, fontSize: TextUnit, modifier: Modifier = Modifier, color: Color = Color.Unspecified,
                  loop: Boolean = false) {
    val asset = animationAsset(emoji)
    if (asset == null) {
        Text(emoji, fontSize = fontSize, color = color, modifier = modifier)
        return
    }
    val result = rememberLottieComposition(LottieCompositionSpec.Asset(asset))
    val composition = result.value
    var playbackFailed by remember(asset, composition) { mutableStateOf(false) }
    var replay by remember(asset) { mutableIntStateOf(0) }
    LaunchedEffect(asset, composition, result.error) {
        if (result.error != null) LogManager.addLog("ERROR", "Emoji", "animation load failed: asset=$asset error=${result.error}")
        else if (composition != null) {
            LogManager.addLog("DEBUG", "Emoji", "animation ready: asset=$asset frames=${composition.durationFrames} fps=${composition.frameRate}")
            composition.warnings.forEach { LogManager.addLog("WARN", "Emoji", "animation warning: asset=$asset $it") }
        }
    }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    var playing by remember(lifecycle) { mutableStateOf(lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) }
    DisposableEffect(lifecycle, asset) {
        val observer = LifecycleEventObserver { _, _ ->
            val resumed = lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)
            if (playing != resumed) LogManager.addLog("DEBUG", "Emoji", "animation visibility: asset=$asset resumed=$resumed")
            playing = resumed
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    val replayModifier = if (loop) Modifier else Modifier.clickable(enabled = composition != null, onClickLabel = stringResource(R.string.replay)) {
        playbackFailed = false; replay++
        LogManager.addLog("DEBUG", "Emoji", "animation replay requested: asset=$asset replay=$replay")
    }
    Box(modifier.then(replayModifier).semantics { contentDescription = emoji }, contentAlignment = Alignment.Center) {
        if (composition == null || playbackFailed) Text(emoji, fontSize = fontSize, color = color)
        else key(asset, replay, loop) {
            val animation = rememberLottieAnimatable()
            // Останавливаемся только на границе цикла, после минимум четырёх секунд воспроизведения.
            val iterations = if (loop) LottieConstants.IterateForever
                else ceil(EMOJI_PLAYBACK_MIN_MS / composition.duration.coerceAtLeast(1f)).toInt().coerceAtLeast(1)
            LaunchedEffect(composition, playing) {
                if (!playing || (!loop && animation.composition == composition && animation.isAtEnd)) return@LaunchedEffect
                LogManager.addLog("DEBUG", "Emoji",
                    "animation run: asset=$asset replay=$replay cycles=$iterations cycle_ms=${composition.duration} " +
                        "iteration=${animation.iteration} progress=${animation.progress}")
                try {
                    animation.animate(composition, iterations = iterations, initialProgress = animation.progress,
                        ignoreSystemAnimationsDisabled = true)
                } catch (e: CancellationException) { throw e }
                catch (e: Exception) {
                    LogManager.addLog("ERROR", "Emoji", "animation playback failed asset=$asset replay=$replay; Unicode fallback: $e")
                    playbackFailed = true; return@LaunchedEffect
                }
                LogManager.addLog("DEBUG", "Emoji",
                    "animation finished: asset=$asset replay=$replay cycles=${animation.iteration} progress=${animation.progress}")
            }
            LottieAnimation(composition, progress = { animation.progress }, modifier = Modifier.fillMaxSize())
        }
    }
}

/** Преобразовать только обычный текст: адреса остаются точными, без emoji placeholders. */
private fun AnnotatedString.Builder.appendEmojiText(text: String) {
    val iterator = BreakIterator.getCharacterInstance(Locale.US)
    iterator.setText(text)
    var start = iterator.first()
    var end = iterator.next()
    while (end != BreakIterator.DONE) {
        val cluster = text.substring(start, end)
        val asset = animationAsset(cluster)
        if (asset == null) append(cluster) else appendInlineContent(asset, cluster)
        start = end
        end = iterator.next()
    }
}

/** Полные графемы сохраняют модификаторы кожи и ZWJ-последовательности обычными Unicode-смайликами. */
@Composable
fun EmojiText(text: String, fontSize: TextUnit, color: Color, modifier: Modifier = Modifier,
              onLink: ((String) -> Unit)? = null, onTextClick: (() -> Unit)? = null) {
    val annotated = remember(text) {
        buildAnnotatedString {
            var position = 0
            for (link in MessageLinks.parse(text)) {
                appendEmojiText(text.substring(position, link.start))
                val start = length
                append(link.url)
                addStringAnnotation("link", link.url, start, length)
                addStyle(SpanStyle(color = Color(0xFF3390EC), textDecoration = TextDecoration.Underline), start, length)
                position = link.end
            }
            appendEmojiText(text.substring(position))
        }
    }
    val inline = remember(fontSize, color) {
        animatedEmojiAssets.values.associateWith {
            InlineTextContent(Placeholder(fontSize * 1.3f, fontSize * 1.3f, PlaceholderVerticalAlign.Center)) { emoji ->
                AnimatedEmoji(emoji, fontSize, Modifier.fillMaxSize(), color)
            }
        }
    }
    var layout by remember(annotated, fontSize) { mutableStateOf<TextLayoutResult?>(null) }
    val callback by rememberUpdatedState(onLink)
    val textClick by rememberUpdatedState(onTextClick)
    val links = annotated.getStringAnnotations("link", 0, annotated.length)
    val linkModifier = if (onLink == null || links.isEmpty()) modifier else modifier.pointerInput(annotated) {
        detectTapGestures { point ->
            val result = layout ?: return@detectTapGestures
            val line = result.getLineForVerticalPosition(point.y)
            if (point.y < 0 || point.y >= result.size.height || point.x < result.getLineLeft(line) ||
                point.x >= result.getLineRight(line)) return@detectTapGestures
            val offset = result.getOffsetForPosition(point)
            val link = annotated.getStringAnnotations("link", offset, offset).firstOrNull { offset >= it.start && offset < it.end }
            if (link != null) callback?.invoke(link.item) else textClick?.invoke()
        }
    }
    Text(annotated, inlineContent = inline, fontSize = fontSize, color = color, modifier = linkModifier,
        onTextLayout = { layout = it })
}
