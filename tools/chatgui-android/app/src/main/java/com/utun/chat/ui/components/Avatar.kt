package com.utun.chat.ui.components

import android.graphics.Bitmap
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.utun.chat.ChatApplication
import com.utun.chat.R
import com.utun.chat.ui.screens.AvatarViewerScreen

/** Круглое превью участника из app-scoped AvatarRepository с инициалами до загрузки.
 * group=null ищет ссылку owner среди доступных групп; revision запускает повторную проверку кеша.
 * Короткое нажатие открывает галерею оригиналов, удержание передаётся onLongClick; enabled выключает оба жеста.
 * profile=true показывает локальный профиль текущего аккаунта и отключает жесты независимо от enabled. */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun UserAvatar(group: String?, owner: String, name: String, modifier: Modifier = Modifier,
               size: Dp = 40.dp, profile: Boolean = false, enabled: Boolean = true, onLongClick: (() -> Unit)? = null) {
    val repository = ChatApplication.instance.avatars
    val revision by repository.revision.collectAsState()
    var viewing by rememberSaveable(group,owner) { mutableStateOf(false) }
    LaunchedEffect(group, owner, revision, profile) {
        if (profile) repository.ensureProfile() else repository.ensureUser(group, owner)
    }
    AvatarImage(if (profile) repository.profileBitmap() else repository.userBitmap(group, owner), name,
        modifier.combinedClickable(enabled = enabled && !profile, onLongClick = onLongClick,
            onLongClickLabel = if (onLongClick != null) stringResource(R.string.options) else null, onClick = { viewing = true }), size)
    if (viewing) AvatarViewerScreen(group,owner,name,onClose={ viewing=false })
}

/** Превью отдельного аватара группы, подгружаемое через ensureGroup; клики задаёт содержащий компонент. */
@Composable
fun GroupAvatar(group: String, name: String, modifier: Modifier = Modifier, size: Dp = 44.dp) {
    val repository = ChatApplication.instance.avatars
    val revision by repository.revision.collectAsState()
    LaunchedEffect(group, revision) { repository.ensureGroup(group) }
    AvatarImage(repository.groupBitmap(group), name, modifier, size)
}

/** Круглый crop Bitmap либо инициалы первых двух слов name; при пустом имени — "#". Bitmap не изменяется. */
@Composable
private fun AvatarImage(bitmap: Bitmap?, name: String, modifier: Modifier, size: Dp) {
    Box(modifier.size(size).clip(CircleShape).background(MaterialTheme.colorScheme.secondaryContainer),
        contentAlignment = Alignment.Center) {
        if (bitmap != null) Image(bitmap.asImageBitmap(), contentDescription = null,
            modifier = Modifier.size(size), contentScale = ContentScale.Crop)
        else Text(name.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }.take(2)
            .joinToString("") { String(Character.toChars(it.codePointAt(0))) }.uppercase().ifEmpty { "#" },
            color = MaterialTheme.colorScheme.onSecondaryContainer, style = MaterialTheme.typography.titleMedium)
    }
}

/** Подпись опубликованного хранилища; кеш изображения не меняет эту подпись. */
@Composable
fun avatarStorageText(state: com.utun.chat.data.AvatarStorageState): String {
    val node = listOf(state.serverName, state.server).filter { it.isNotBlank() }.joinToString(" · ")
    val published = when {
        state.server.isEmpty() -> stringResource(R.string.avatar_not_published)
        state.server == state.owner && state.ownDevice -> stringResource(R.string.avatar_on_your_device)
        state.server == state.owner -> stringResource(R.string.avatar_on_author, node)
        else -> stringResource(R.string.avatar_on_node, node)
    }
    return if (!state.pending) published else if (state.server.isEmpty()) stringResource(R.string.avatar_local_pending)
        else stringResource(R.string.avatar_new_local_pending, published)
}
