package com.utun.chat.ui.screens

import android.graphics.Bitmap
import android.view.WindowManager
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.layout.positionInWindow
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.utun.chat.ChatApplication
import com.utun.chat.R
import com.utun.chat.data.LogManager
import com.utun.chat.ui.components.ZoomableImage
import kotlinx.coroutines.*

/**
 * Галерея пользовательских аватаров из каталога манифестов avatars выбранного участника.
 * group=null ищет первую подходящую ссылку owner среди групп. Каталог листается через cursor/version;
 * ошибка каталога оставляет текущую ссылку доступной, повтор запускается кнопкой Retry.
 * Для выбранного манифеста параллельно загружаются превью и полный оригинал после EXIF (без crop,
 * с уменьшением декодирования); для двух соседей с каждой стороны — только превью.
 * Выбор переживает пересоздание композиции через rememberSaveable; уход отменяет корутины экрана,
 * а общий кеш/передачи других потребителей остаются у AvatarRepository. При account пуст вызывается onClose.
 */
@Composable
fun AvatarViewerScreen(group: String?, owner: String, name: String, onClose: () -> Unit) {
    val repository=ChatApplication.instance.avatars
    val members by repository.members.collectAsState()
    val revision by repository.revision.collectAsState()
    val account=repository.accountId
    DisposableEffect(group,owner,account) {
        LogManager.addLog("INFO","Avatar","Viewer opened group=$group owner=$owner account=$account")
        onDispose { LogManager.addLog("INFO","Avatar","Viewer closed group=$group owner=$owner") }
    }
    val source=remember(members,group,owner) { repository.gallerySource(group,owner) }
    var files by remember(group,owner,account) { mutableStateOf(emptyList<String>()) }
    var selected by rememberSaveable(group,owner,account) { mutableStateOf("") }
    var sourceKey by rememberSaveable(group,owner,account) { mutableStateOf("") }
    var previews by remember(group,owner,account) { mutableStateOf(emptyMap<String,Bitmap>()) }
    var original by remember { mutableStateOf<Bitmap?>(null) }
    var failed by remember { mutableStateOf(false) }
    var catalogFailed by remember { mutableStateOf(false) }
    var failedPreviews by remember { mutableStateOf(emptySet<String>()) }
    var retry by remember { mutableStateOf(0) }
    LaunchedEffect(account,revision) { if (account.isEmpty()) onClose() }
    LaunchedEffect(source?.first,source?.second?.avatarServer,source?.second?.avatarFile,retry) {
        val (channel,member)=source ?: return@LaunchedEffect
        val key="$channel:${member.ed25519}:${member.avatarServer}"
        if (sourceKey!=key) { sourceKey=key; files=emptyList(); selected=member.avatarFile; previews=emptyMap() }
        if (selected.isEmpty()) selected=member.avatarFile
        if (files.isEmpty()) files=listOf(member.avatarFile)
        catalogFailed=false
        try { files=repository.catalog(channel,member); if (selected !in files) selected=member.avatarFile }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { catalogFailed=true; LogManager.addLog("WARN","Avatar","Catalog failed owner=$owner: ${e.message}") }
    }
    val index=files.indexOf(selected)
    val visible=if (index<0) emptyList() else files.subList((index-2).coerceAtLeast(0),(index+3).coerceAtMost(files.size))
    LaunchedEffect(source?.first,source?.second?.avatarServer,selected,retry) {
        original=null; failed=false
        val (channel,member)=source ?: return@LaunchedEffect
        if (selected.isEmpty()) return@LaunchedEffect
        LogManager.addLog("INFO","Avatar","Viewer selected group=$channel owner=$owner server=${member.avatarServer} file=$selected")
        coroutineScope {
            launch {
                try { previews=previews+(selected to repository.loadAvatar(channel,member.copy(avatarFile=selected),false)) }
                catch (e: CancellationException) { throw e }
                catch (e: Exception) { failed=true; LogManager.addLog("WARN","Avatar","Preview failed file=$selected: ${e.message}") }
            }
            launch {
                try { original=repository.loadAvatar(channel,member.copy(avatarFile=selected),true) }
                catch (e: CancellationException) { throw e }
                catch (e: Exception) { failed=true; LogManager.addLog("WARN","Avatar","Original failed file=$selected: ${e.message}") }
            }
        }
    }
    LaunchedEffect(source?.first,source?.second?.avatarServer,visible,retry) {
        val (channel,member)=source ?: return@LaunchedEffect
        previews=previews.filterKeys { it in visible }
        failedPreviews=emptySet()
        coroutineScope { visible.filter { it!=selected && it !in previews }.forEach { file -> launch {
            try { previews=previews+(file to repository.loadAvatar(channel,member.copy(avatarFile=file),false)) }
            catch (e: CancellationException) { throw e }
            catch (e: Exception) { failedPreviews=failedPreviews+file; LogManager.addLog("WARN","Avatar","Neighbor failed file=$file: ${e.message}") }
        } } }
    }
    fun move(delta: Int) { val next=index+delta; if (next in files.indices) selected=files[next] }
    AvatarViewerDialog(name, onClose) {
        val view=LocalView.current
        source?.let { (channel, member) -> Text(com.utun.chat.ui.components.avatarStorageText(
            repository.storageState(channel, member.ed25519)), color=Color.White) }
        Box(Modifier.weight(1f).fillMaxWidth().clipToBounds(),contentAlignment=Alignment.Center) {
            val image=original ?: previews[selected]
            if (image!=null) ZoomableImage(image,Modifier.fillMaxSize(),onSwipe={ move(it) })
            else if (source!=null && !failed) CircularProgressIndicator()
            else Text(stringResource(if (source==null) R.string.avatar_none else R.string.avatar_load_failed),color=Color.White)
        }
        if (failed || catalogFailed || failedPreviews.isNotEmpty()) TextButton(onClick={ retry++ },modifier=Modifier.align(Alignment.CenterHorizontally)) {
            Text(stringResource(R.string.avatar_retry))
        } else if (original==null && source!=null) LinearProgressIndicator(Modifier.fillMaxWidth())
        if (index>=0) Text("${index+1} / ${files.size}",color=Color.White,modifier=Modifier.align(Alignment.CenterHorizontally))
        Row(Modifier.fillMaxWidth().padding(vertical=8.dp).onGloballyPositioned {
            val insets=ViewCompat.getRootWindowInsets(view)?.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            LogManager.addLog("DEBUG","Avatar","Viewer thumbnails position=${it.positionInWindow()} size=${it.size} " +
                "window=${view.width}x${view.height} insets=$insets")
        },verticalAlignment=Alignment.CenterVertically,
            horizontalArrangement=Arrangement.Center) {
            TextButton(enabled=index>0,onClick={ move(-1) },contentPadding=PaddingValues(4.dp),modifier=Modifier.width(32.dp)) { Text("←") }
            repeat(5) { slot ->
                val item=index+slot-2; val file=files.getOrNull(item)
                Box(Modifier.weight(1f).aspectRatio(1f).padding(2.dp)
                    .border(if (slot==2) 2.dp else 0.dp,if (slot==2) Color(0xFF77AAFF) else Color.Transparent)
                    .clickable(enabled=file!=null) { if (file!=null) selected=file },contentAlignment=Alignment.Center) {
                    previews[file]?.let { Image(it.asImageBitmap(),contentDescription=null,contentScale=ContentScale.Crop,
                        modifier=Modifier.fillMaxSize()) }
                    if (file!=null && file !in previews) {
                        if (file in failedPreviews) Text("!",color=Color.White)
                        else CircularProgressIndicator(Modifier.size(18.dp),strokeWidth=2.dp)
                    }
                }
            }
            TextButton(enabled=index>=0 && index+1<files.size,onClick={ move(1) },contentPadding=PaddingValues(4.dp),
                modifier=Modifier.width(32.dp)) { Text("→") }
        }
    }
}

/** Просмотр единственного JPEG-аватара группы из core, без каталога пользовательских оригиналов.
 * При изменениях revision проверяет кеш через ensureGroup; ошибку можно повторить с force=true.
 * Декодированная картинка показывается с масштабированием, при account пуст вызывается onClose. */
@Composable
fun GroupAvatarViewerScreen(group: String, name: String, onClose: () -> Unit) {
    val repository = ChatApplication.instance.avatars
    val groups by repository.groups.collectAsState()
    val revision by repository.revision.collectAsState()
    val account = repository.accountId
    val state = groups[group]
    val image = remember(group, state, revision) { repository.groupBitmap(group) }
    DisposableEffect(group, account) {
        LogManager.addLog("INFO", "Avatar", "Group viewer opened group=$group account=$account")
        onDispose { LogManager.addLog("INFO", "Avatar", "Group viewer closed group=$group") }
    }
    LaunchedEffect(group, account, revision) {
        if (account.isEmpty()) onClose() else repository.ensureGroup(group)
    }
    LaunchedEffect(group, state, image) {
        LogManager.addLog("DEBUG", "Avatar", "Group viewer group=$group version=${state?.version} loading=${state?.loading} " +
            "error=${state?.error} hasImage=${state?.hasImage} bitmap=${image?.width}x${image?.height}")
    }
    AvatarViewerDialog(name, onClose) {
        Box(Modifier.weight(1f).fillMaxWidth().clipToBounds(), contentAlignment = Alignment.Center) {
            when {
                image != null -> ZoomableImage(image, Modifier.fillMaxSize())
                state?.error == true -> Text(stringResource(R.string.avatar_load_failed), color = Color.White)
                state == null || state.loading -> CircularProgressIndicator()
                else -> Text(stringResource(R.string.avatar_none), color = Color.White)
            }
        }
        if (state?.error == true) TextButton(onClick = {
            LogManager.addLog("INFO", "Avatar", "Group viewer retry group=$group")
            repository.ensureGroup(group, true)
        }, modifier = Modifier.align(Alignment.CenterHorizontally)) {
            Text(stringResource(R.string.avatar_retry))
        } else if (state?.loading == true && image != null) LinearProgressIndicator(Modifier.fillMaxWidth())
    }
}

/** Общие размеры окна, безопасные отступы и закрытие для обоих видов аватарок. */
@Composable
private fun AvatarViewerDialog(name: String, onClose: () -> Unit, content: @Composable ColumnScope.() -> Unit) {
    Dialog(onDismissRequest = onClose, properties = DialogProperties(decorFitsSystemWindows = false)) {
        val view = LocalView.current
        val window = (view.parent as DialogWindowProvider).window
        DisposableEffect(window) {
            // Compose 1.6 с usePlatformDefaultWidth=false измеряет по дисплею, а не по доступной области окна.
            window.setLayout(WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT)
            onDispose { }
        }
        Column(Modifier.fillMaxSize().background(Color.Black).safeDrawingPadding().onSizeChanged {
            LogManager.addLog("DEBUG", "Avatar", "Viewer content=${it.width}x${it.height}")
        }) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(name, color = Color.White, modifier = Modifier.weight(1f).padding(12.dp))
                TextButton(onClick = onClose) { Text(stringResource(R.string.close)) }
            }
            content()
        }
    }
}
