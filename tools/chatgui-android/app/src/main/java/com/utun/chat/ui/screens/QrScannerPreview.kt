package com.utun.chat.ui.screens

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import android.Manifest
import android.app.Activity
import android.content.ContextWrapper
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.view.PreviewView
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import com.utun.chat.data.InviteLink
import com.utun.chat.data.LogManager
import com.utun.chat.data.QrScannerSession

/** Camera permission и preview QR-приглашений; одна принятая ссылка передаётся onResult на main.
 * Владельцем CameraX/ML Kit и незавершённых кадров служит QrScannerSession; dispose не закрывает кадр под распознавателем. */
@Composable
fun QrScannerPreview(modifier: Modifier = Modifier, onResult: (String) -> Unit) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val result by rememberUpdatedState(onResult)
    var hasPermission by remember { mutableStateOf(
        ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
    ) }
    var cameraError by remember { mutableStateOf<Int?>(null) }
    var permanentDenial by remember { mutableStateOf(false) }
    val activity = remember(context) {
        var current = context
        while (current is ContextWrapper && current !is Activity) current = current.baseContext
        current as? Activity
    }
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                val granted = ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
                if (granted != hasPermission) {
                    LogManager.addLog("INFO", "QR", "Camera permission on resume=$granted")
                    hasPermission = granted
                    cameraError = null
                }
                if (granted) permanentDenial = false
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        hasPermission = granted
        permanentDenial = !granted && activity?.shouldShowRequestPermissionRationale(Manifest.permission.CAMERA) == false
        LogManager.addLog(if (granted) "INFO" else "WARN", "QR", "Camera permission=$granted permanentDenial=$permanentDenial")
    }
    LaunchedEffect(Unit) {
        if (!hasPermission) {
            LogManager.addLog("INFO", "QR", "Requesting CAMERA permission")
            try { launcher.launch(Manifest.permission.CAMERA) } catch (e: Exception) {
                LogManager.addLog("ERROR", "QR", "Camera permission request failed: $e")
                cameraError = R.string.camera_unavailable
            }
        }
    }

    // CameraX масштабирует TextureView; ограничиваем видео областью сканера, чтобы оно не заходило под поле ссылки.
    Box(modifier.clipToBounds(), contentAlignment = Alignment.Center) {
        if (!hasPermission) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(stringResource(R.string.camera_permission_required))
                TextButton(onClick = {
                    try {
                        cameraError = null
                        if (permanentDenial) {
                            context.startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                                Uri.parse("package:${context.packageName}")).apply {
                                    if (activity == null) addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                                })
                        } else launcher.launch(Manifest.permission.CAMERA)
                    } catch (e: Exception) {
                        LogManager.addLog("ERROR", "QR", "Cannot request camera permission/settings: $e")
                        cameraError = R.string.camera_unavailable
                    }
                }) { Text(stringResource(if (permanentDenial) R.string.settings else R.string.grant)) }
            }
        } else {
            val previewView = remember { PreviewView(context).apply {
                implementationMode = PreviewView.ImplementationMode.COMPATIBLE
                scaleType = PreviewView.ScaleType.FIT_CENTER // Вписываем весь кадр с сохранением пропорций.
            } }
            DisposableEffect(lifecycleOwner, previewView) {
                cameraError = null
                val session = QrScannerSession(context, lifecycleOwner, previewView, onScan = { values ->
                    val link = values.firstOrNull { it.startsWith("utun://") && InviteLink.decode(it) != null }
                    if (link != null) result(link)
                    link != null
                }, onError = { cameraError = when (it) {
                    QrScannerSession.Failure.CAMERA -> R.string.camera_unavailable
                    QrScannerSession.Failure.SCAN -> R.string.qr_scan_failed
                } })
                session.start()
                onDispose { session.close() }
            }
            AndroidView(factory = { previewView }, modifier = Modifier.fillMaxSize().onSizeChanged {
                LogManager.addLog("DEBUG", "QR", "Preview viewport=${it.width}x${it.height} scale=${previewView.scaleType} " +
                    "clipped=true mode=${previewView.implementationMode}")
            })
            Canvas(Modifier.fillMaxSize()) {
                val side = size.minDimension * 0.75f
                val left = (size.width - side) / 2
                val top = (size.height - side) / 2
                val length = 20.dp.toPx()
                val stroke = 3.dp.toPx()
                for (xSign in listOf(-1, 1)) {
                    for (ySign in listOf(-1, 1)) {
                        val x = if (xSign == -1) left else left + side
                        val y = if (ySign == -1) top else top + side
                        drawLine(Color.Red, Offset(x, y), Offset(x - xSign * length, y), stroke)
                        drawLine(Color.Red, Offset(x, y), Offset(x, y - ySign * length), stroke)
                    }
                }
            }
        }
        cameraError?.let { Text(stringResource(it), color = MaterialTheme.colorScheme.error, modifier = Modifier.align(Alignment.BottomCenter)) }
    }
}
