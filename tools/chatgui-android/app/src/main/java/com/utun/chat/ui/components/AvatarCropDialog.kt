package com.utun.chat.ui.components

import android.graphics.Bitmap
import android.net.Uri
import android.view.WindowManager
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.ClipOp
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import com.utun.chat.R
import com.utun.chat.data.*
import kotlinx.coroutines.*
import java.io.File
import kotlin.math.roundToInt

/**
 * Редактор пользовательского аватара: копирует URI в cacheDir на IO worker, учитывает EXIF,
 * принимает JPEG/PNG/WebP до 32 МиБ и 16×1024×1024 пикселей. Для экрана декодирует уменьшенную копию.
 * Сдвиг и zoom 1..16 выбирают квадрат полного ориентированного оригинала; круг — только маска просмотра.
 * onConfirm получает временный File и нормализованный AvatarCrop и принимает обязанность удалить файл
 * (ProfileAvatarControls передаёт его в AvatarRepository.editProfile). После подтверждения диалог его не удаляет.
 * onCancel должен убрать диалог из композиции: при dispose неподтверждённая копия удаляется на IO worker.
 */
@Composable
fun AvatarCropDialog(uri: Uri, onCancel: () -> Unit, onConfirm: (File, AvatarCrop) -> Unit) {
    val context=LocalContext.current
    var source by remember(uri) { mutableStateOf<File?>(null) }
    var bitmap by remember(uri) { mutableStateOf<Bitmap?>(null) }
    var dimensions by remember(uri) { mutableStateOf(IntSize.Zero) }
    var failed by remember(uri) { mutableStateOf(false) }
    var handedOver by remember(uri) { mutableStateOf(false) }
    var zoom by rememberSaveable(uri.toString()) { mutableStateOf(1f) }
    var ox by rememberSaveable(uri.toString()) { mutableStateOf(0f) }
    var oy by rememberSaveable(uri.toString()) { mutableStateOf(0f) }
    var bounds by remember { mutableStateOf(IntSize.Zero) }
    LaunchedEffect(uri) {
        var copied: File?=null
        try {
            val image=withContext(Dispatchers.IO) {
                val file=AvatarAssets.copySource(context,uri); copied=file
                val format=android.graphics.BitmapFactory.Options().apply { inJustDecodeBounds=true }
                android.graphics.BitmapFactory.decodeFile(file.path,format)
                require(format.outMimeType in listOf("image/jpeg","image/png","image/webp")) { "unsupported source format=${format.outMimeType}" }
                val (w,h)=PhotoUtil.orientedDimensions(file.path)
                require(w>0 && h>0 && w.toLong()*h<=16L*1024*1024) { "original exceeds 16 megapixels" }
                var sample=1; while (maxOf(w,h)/sample>1600) sample*=2
                requireNotNull(PhotoUtil.decodeOriented(file.path,sample)) { "cannot decode editor image" } to IntSize(w,h)
            }
            source=copied; bitmap=image.first; dimensions=image.second
            LogManager.addLog("INFO","Avatar","Editor loaded original=${dimensions.width}x${dimensions.height} " +
                "preview=${image.first.width}x${image.first.height} mask=circle")
        } catch (e: Exception) {
            withContext(NonCancellable+Dispatchers.IO) {
                copied?.let { if (!it.delete()) LogManager.addLog("WARN","Avatar","Cannot remove editor source=$it") }
            }
            if (e is CancellationException) throw e
            failed=true; LogManager.addLog("ERROR","Avatar","Editor load failed: ${e.message}")
        }
    }
    DisposableEffect(source) {
        val file=source
        onDispose { if (!handedOver && file!=null) CoroutineScope(Dispatchers.IO).launch {
            if (!file.delete()) LogManager.addLog("WARN","Avatar","Cannot remove cancelled source=$file")
        } }
    }
    Dialog(onDismissRequest=onCancel,properties=DialogProperties(decorFitsSystemWindows=false)) {
        val window=(LocalView.current.parent as DialogWindowProvider).window
        DisposableEffect(window) {
            // Compose 1.6 с usePlatformDefaultWidth=false измеряет по дисплею, а не по доступной области окна.
            window.setLayout(WindowManager.LayoutParams.MATCH_PARENT,WindowManager.LayoutParams.MATCH_PARENT)
            onDispose { }
        }
        Column(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.surface).systemBarsPadding().padding(12.dp)) {
            Text(stringResource(R.string.avatar_crop_title),style=MaterialTheme.typography.titleLarge)
            Text(stringResource(R.string.avatar_crop_hint))
            if (bitmap==null && !failed) LinearProgressIndicator(Modifier.fillMaxWidth())
            if (failed) Text(stringResource(R.string.avatar_load_failed),color=MaterialTheme.colorScheme.error)
            val image=bitmap
            val photoW=dimensions.width; val photoH=dimensions.height
            val side=minOf(bounds.width,bounds.height)*0.8f
            val scale=if (image==null) 1f else maxOf(side/photoW,side/photoH)*zoom
            val mx=if (image==null) 0f else ((photoW*scale-side)/2).coerceAtLeast(0f)
            val my=if (image==null) 0f else ((photoH*scale-side)/2).coerceAtLeast(0f)
            val dx=ox.coerceIn(-mx,mx); val dy=oy.coerceIn(-my,my)
            Canvas(Modifier.weight(1f).fillMaxWidth().clipToBounds().onSizeChanged {
                bounds=it; LogManager.addLog("DEBUG","Avatar","Editor canvas=${it.width}x${it.height}")
            }
                .pointerInput(image,bounds) {
                    if (image!=null) detectTransformGestures { _,pan,factor,_ ->
                        val old=zoom; zoom=(zoom*factor).coerceIn(1f,16f)
                        val s=maxOf(side/photoW,side/photoH)*zoom
                        // При минимальном масштабе погрешность Float может дать отрицательный предел.
                        val limitX=((photoW*s-side)/2).coerceAtLeast(0f)
                        val limitY=((photoH*s-side)/2).coerceAtLeast(0f)
                        ox=(ox*zoom/old+pan.x).coerceIn(-limitX,limitX)
                        oy=(oy*zoom/old+pan.y).coerceIn(-limitY,limitY)
                        LogManager.addLog("DEBUG","Avatar","Editor transform zoom=$zoom limits=$limitX,$limitY offset=$ox,$oy")
                    }
                }) {
                drawRect(Color.Black)
                if (image!=null && side>0) {
                    drawImage(image.asImageBitmap(),dstOffset=IntOffset(((size.width-photoW*scale)/2+dx).roundToInt(),
                        ((size.height-photoH*scale)/2+dy).roundToInt()),
                        dstSize=IntSize((photoW*scale).roundToInt(),(photoH*scale).roundToInt()))
                    val x=(size.width-side)/2; val y=(size.height-side)/2; val shade=Color.Black.copy(alpha=0.6f)
                    val circle=Path().apply { addOval(Rect(x,y,x+side,y+side)) }
                    clipPath(circle,clipOp=ClipOp.Difference) { drawRect(shade) }
                    drawCircle(Color.White,radius=side/2,center=Offset(size.width/2,size.height/2),style=Stroke(2.dp.toPx()))
                }
            }
            Row(Modifier.fillMaxWidth().onGloballyPositioned {
                LogManager.addLog("DEBUG","Avatar","Editor actions bounds=${it.boundsInWindow()}")
            },horizontalArrangement=Arrangement.End) {
                TextButton(onClick=onCancel) { Text(stringResource(R.string.cancel)) }
                Button(enabled=image!=null && source!=null && side>0,onClick={
                    val crop=AvatarCrop(((photoW*scale-side-2*dx)/(2*scale*photoW)).toDouble(),
                        ((photoH*scale-side-2*dy)/(2*scale*photoH)).toDouble(),
                        side.toDouble()/scale/photoW,side.toDouble()/scale/photoH)
                    LogManager.addLog("INFO","Avatar","Editor confirmed crop=$crop zoom=$zoom")
                    handedOver=true; onConfirm(requireNotNull(source),crop)
                }) { Text(stringResource(R.string.avatar_upload)) }
            }
        }
    }
}
