package com.utun.chat.ui.screens

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import android.graphics.Bitmap
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.utun.chat.data.LogManager
import com.utun.chat.data.PhotoUtil
import com.utun.chat.ui.components.ZoomableImage
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.withContext

private const val MAX_DECODE_DIM = 4096

/** Локальное изображение с EXIF и Fit/zoom; декодирование в IO ограничено стороной 4096 пикселей.
 * Экран владеет Bitmap через Compose; смена пути отменяет доставку старого результата. Ошибка оставляет доступным выход. */
@Composable
fun PhotoViewerScreen(filePath: String, onClose: () -> Unit, onOpened: (String) -> Unit = {}) {
    var bmp by remember(filePath) { mutableStateOf<Bitmap?>(null) }
    var loaded by remember(filePath) { mutableStateOf(false) }
    BackHandler(onBack = onClose)

    ViewerFullscreenWindow("photo")

    LaunchedEffect(filePath) {
        bmp = try { withContext(Dispatchers.IO) { PhotoUtil.decodeFitted(filePath, MAX_DECODE_DIM) } }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) {
            LogManager.addLog("ERROR", "PhotoViewer", "decode exception path=$filePath: $e")
            null
        }
        loaded = true
        if (bmp == null) LogManager.addLog("ERROR", "PhotoViewer", "decode failed: $filePath")
        else {
            LogManager.addLog("DEBUG", "PhotoViewer", "Opened image ${bmp?.width}x${bmp?.height} path=$filePath")
            onOpened(filePath)
        }
    }

    Box(
        modifier = Modifier.fillMaxSize().background(Color.Black)
    ) {
        bmp?.let { ZoomableImage(it, Modifier.fillMaxSize()) }
        if (!loaded) CircularProgressIndicator(Modifier.align(Alignment.Center))
        else if (bmp == null) Text(stringResource(R.string.image_failed), color = Color.White, modifier = Modifier.align(Alignment.Center))

        /* close button (top-right) */
        Row(
            modifier = Modifier.fillMaxWidth()
                .background(Color(0xAA000000))
                .statusBarsPadding()
                .padding(horizontal = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.End
        ) {
            IconButton(onClick = onClose) {
                Icon(Icons.Filled.Close, stringResource(R.string.close), tint = Color.White)
            }
        }
    }
}
