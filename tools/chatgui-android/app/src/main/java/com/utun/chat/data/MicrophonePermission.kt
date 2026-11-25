package com.utun.chat.data

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Looper
import androidx.core.content.ContextCompat
import com.utun.chat.ChatApplication
import com.utun.chat.MainActivity

/** App-scoped вход в запрос RECORD_AUDIO; сам диалог и pending callback принадлежат видимой Activity.
 * Activity устанавливает requester при onStart и снимает только свой обработчик при onStop.
 * Без Activity открывается MainActivity с EXTRA_REQUEST: исходное действие/onGranted не сохраняются,
 * после выдачи разрешения пользователь повторяет команду (например, PTT).
 */
object MicrophonePermission {
    const val EXTRA_REQUEST = "request_microphone_permission"
    var requester: ((String, () -> Unit) -> Unit)? = null

    fun granted(): Boolean = ContextCompat.checkSelfPermission(ChatApplication.instance, Manifest.permission.RECORD_AUDIO) ==
        PackageManager.PERMISSION_GRANTED

    /** Main thread: при готовом разрешении вызвать onGranted синхронно, иначе передать запрос Activity.
     * Обработчик Activity решает, принять ли параллельный запрос; отказ разрешения не вызывает onGranted.
     * Ошибка открытия Activity логируется; исключения пользовательского requester/onGranted передаются caller. */
    fun request(reason: String, onGranted: () -> Unit = {}) {
        check(Looper.myLooper() == Looper.getMainLooper()) {
            val message = "MicrophonePermission requires main thread: caller=${Thread.currentThread().name}"
            LogManager.addLog("ERROR", "PERMISSION", message)
            message
        }
        if (granted()) { onGranted(); return }
        LogManager.addLog("INFO", "PERMISSION", "Microphone required: $reason")
        val handler = requester
        if (handler != null) {
            handler(reason, onGranted)
        } else {
            // PTT из оверлея: открыть видимый экран; после выдачи нужна новая команда передачи.
            try {
                ChatApplication.instance.startActivity(Intent(ChatApplication.instance, MainActivity::class.java).apply {
                    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                    putExtra(EXTRA_REQUEST, true)
                })
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "PERMISSION", "Cannot open microphone permission screen: $e")
            }
        }
    }
}
