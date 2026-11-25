package com.utun.chat.ui.components

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.utun.chat.data.LogManager

private data class EmojiCategory(val nameRes: Int, val emoji: List<String>, val animated: Boolean = false)

private val emojiCategories = listOf(
    EmojiCategory(R.string.emoji_faces, ("😀 😃 😄 😁 😆 😅 😂 🤣 😊 🙂 🙃 😉 😍 🥰 😘 😋 😛 😜 🤪 🤗 🤭 🤫 🤔 😎 " +
        "🥳 😏 😒 😔 😢 😭 😤 😠 😡 🤯 😱 😨 😰 😴 🤤 🤢 🤮 🤧 🥺 😇 🤡 💩 👻 🤖").split(" ")),
    EmojiCategory(R.string.emoji_animations, animatedEmojiCharacters, animated = true),
    EmojiCategory(R.string.emoji_gestures, ("👍 👎 👏 🙌 👐 🤲 🙏 🤝 ✌️ 🤞 🤟 🤘 👌 🤌 👈 👉 " +
        "👆 👇 ☝️ ✋ 👋 🤚 🖐️ ✊ 👊 🤛 🤜 💪 🫶 👀 🧠 🫂").split(" ")),
    EmojiCategory(R.string.emoji_hearts, ("❤️ 🧡 💛 💚 💙 💜 🖤 🤍 🤎 💔 ❤️‍🔥 ❤️‍🩹 💕 💞 💓 💗 " +
        "💖 💘 💝 💟 ❣️ 💋 💯 💢 💥 💫 💦 💤 ✨ ⭐ 🌟 🔥").split(" ")),
    EmojiCategory(R.string.emoji_nature, ("🐶 🐱 🐭 🐹 🐰 🦊 🐻 🐼 🐨 🐯 🦁 🐮 🐷 🐸 🐵 🐔 🐧 🐦 🦆 " +
        "🦉 🦋 🐝 🐢 🐬 🐳 🐟 🌸 🌹 🌻 🌲 🌴 🍀 ☀️ 🌙 🌈 ☁️ 🌧️ ❄️").split(" ")),
    EmojiCategory(R.string.emoji_food, ("🍏 🍎 🍐 🍊 🍋 🍌 🍉 🍇 🍓 🫐 🍒 🍑 🍍 🥝 🥑 🍅 🥕 🌽 🥐 🍞 🧀 🥚 " +
        "🍳 🥞 🍔 🍟 🍕 🌭 🌮 🍣 🍜 🍿 🍩 🍪 🎂 🍫 🍬 🍦 ☕ 🍵 🥤 🍺 🥂").split(" ")),
    EmojiCategory(R.string.emoji_other, ("🎉 🎊 🎁 🎈 🎀 🏆 🥇 ⚽ 🏀 🎮 🎲 🎵 🎶 🎸 🎤 🎧 📷 🎬 💡 📚 ✏️ 💼 " +
        "💰 💎 🔑 🔒 🛠️ ⚙️ 📱 💻 🚗 ✈️ 🚀 🏠 🌍 ✅ ❌ ❓ ❗ ⚠️ ♻️ 🏳️ 🏁").split(" "))
)

/** Семь категорий, сетка Adaptive(48dp) высотой 200dp; смена категории возвращает начало списка.
 * onSelect получает полную Unicode-графему; панель остаётся открытой, animated категория повторяет TGS циклически.
 */
@Composable
fun EmojiPickerPanel(onSelect: (String) -> Unit) {
    var categoryIndex by remember { mutableStateOf(0) }
    val gridState = rememberLazyGridState()
    LaunchedEffect(categoryIndex) { gridState.scrollToItem(0) }
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            emojiCategories.forEachIndexed { index, category ->
                FilterChip(selected = categoryIndex == index, onClick = {
                    categoryIndex = index
                    LogManager.addLog("DEBUG", "Composer", "emoji category selected: ${category.nameRes}")
                }, label = { Text(stringResource(category.nameRes)) })
            }
        }
        LazyVerticalGrid(columns = GridCells.Adaptive(48.dp), state = gridState, modifier = Modifier.fillMaxWidth().height(200.dp)) {
            items(emojiCategories[categoryIndex].emoji, key = { it }) { emoji ->
                val description = stringResource(R.string.insert_emoji, emoji)
                TextButton(onClick = { onSelect(emoji) }, contentPadding = PaddingValues(0.dp),
                    modifier = Modifier.height(48.dp).semantics { contentDescription = description }) {
                    if (emojiCategories[categoryIndex].animated) AnimatedEmoji(emoji, 26.sp, Modifier.size(32.dp), loop = true)
                    else Text(emoji, fontSize = 26.sp)
                }
            }
        }
    }
}
