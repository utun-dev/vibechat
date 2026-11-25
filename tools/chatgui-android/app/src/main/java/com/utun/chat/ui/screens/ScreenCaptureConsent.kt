package com.utun.chat.ui.screens

import android.app.Activity
import android.content.Intent
import android.media.projection.MediaProjectionManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.*
import androidx.compose.ui.platform.LocalContext
import com.utun.chat.data.LogManager

/** Разовый запрос от видимого UI. Смена владельца инвалидирует ожидающий результат. */
@Composable
internal fun rememberScreenConsent(owner: Any?): ((Intent) -> Unit) -> Unit = key(owner) {
    val context = LocalContext.current
    var pending by remember(owner) { mutableStateOf<((Intent) -> Unit)?>(null) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val apply = pending
        pending = null
        val grant = result.data
        LogManager.addLog("INFO", "video", "screen consent result=${result.resultCode} pending=${apply != null}")
        if (result.resultCode == Activity.RESULT_OK && grant != null) apply?.invoke(grant)
    }
    val request: ((Intent) -> Unit) -> Unit = { apply ->
        if (pending == null) {
            pending = apply
            try {
                LogManager.addLog("INFO", "video", "requesting screen capture consent")
                launcher.launch(context.getSystemService(MediaProjectionManager::class.java).createScreenCaptureIntent())
            } catch (e: Exception) {
                pending = null
                LogManager.addLog("ERROR", "video", "cannot request screen consent: $e")
            }
        }
    }
    request
}
