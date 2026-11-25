package com.utun.chat.data

import android.media.AudioAttributes
import android.media.AudioManager
import android.media.MediaPlayer
import android.os.Looper
import com.utun.chat.ChatApplication
import com.utun.chat.R

/**
 * Гудки исходящего звонка (ringback) для вызывающего через зацикленный MediaPlayer.
 *
 * Две фазы с разными гудками:
 *  - фаза 1 «пытаемся связаться» — call_connecting (короткие бипы 425 Гц);
 *  - фаза 2 «звоним» (у пира идёт входящий звонок, ожидаем ответа) — call_ringing (длинные гудки).
 *
 * Готовые MP3 из res/raw играют через профиль MEDIA. Все вызовы — на main.
 * Повтор активной фазы сохраняет плеер; смена фазы сначала запрашивает освобождение предыдущего.
 * На ACCEPTED/ENDED/hangup вызывающий делает stop; error/неожиданный completion также запрашивает release.
 * До release callbacks выключаются. Ошибка сохраняет плеер/ID ресурса для следующего stop/start;
 * пока очистка не подтверждена, новая фаза не запускается. Таймера повторной очистки нет.
 */
object CallRingback {

    private var player: MediaPlayer? = null // Активный либо ожидающий успешного release плеер.
    private var resourceId = 0 // Ресурс сохранённого player; сбрасывается только после успешного release.
    private var active = false // Разрешает callbacks/повтор активной фазы; false при любой попытке очистки.

    /** Фаза 1: короткие бипы «устанавливаем соединение». Идемпотентно. */
    fun startConnecting() {
        start(R.raw.call_connecting, "connecting")
    }

    /** Фаза 2: длинные гудки (у пира идёт вызов). Идемпотентно. */
    fun startRinging() {
        start(R.raw.call_ringing, "ringing")
    }

    /** На main запросить release текущего гудка; повторяет отказавшую очистку, успешную не повторяет. */
    fun stop() {
        checkMain()
        release("stopped")
    }

    private fun start(resId: Int, tag: String) {
        checkMain()
        if (active && resourceId == resId) return
        release("phase changed")
        if (player != null) {
            LogManager.addLog("WARN", "Ringback", "start skipped phase=$tag requested_resource=$resId: cleanup pending " +
                "resource=$resourceId player=${System.identityHashCode(player)}")
            return
        }
        val ctx = ChatApplication.instance
        val mp = try {
            val attrs = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build()
            MediaPlayer.create(ctx, resId, attrs, AudioManager.AUDIO_SESSION_ID_GENERATE)
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "Ringback", "MediaPlayer.create failed ($tag): $e")
            null
        }
        if (mp == null) {
            LogManager.addLog("ERROR", "Ringback", "player unavailable phase=$tag resource=$resId")
            return
        }
        player = mp
        resourceId = resId
        active = true
        try {
            mp.setOnErrorListener { failed, what, extra ->
                if (active && player === failed) {
                    LogManager.addLog("ERROR", "Ringback", "playback error phase=$tag what=$what extra=$extra")
                    release("error")
                }
                true
            }
            mp.setOnCompletionListener { completed ->
                if (active && player === completed) {
                    LogManager.addLog("WARN", "Ringback", "loop unexpectedly completed phase=$tag")
                    release("completed")
                }
            }
            mp.isLooping = true
            mp.start()
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "Ringback", "MediaPlayer start failed ($tag) resource=$resId: $e")
            release("start failed")
            return
        }
        LogManager.addLog("INFO", "Ringback", "started $tag resource=$resId player=${System.identityHashCode(mp)}")
    }

    private fun release(reason: String) {
        active = false
        val p = player ?: return
        val previous = resourceId
        // release допустим и в Error; отдельный stop в этом состоянии вызвал бы ещё одну ошибку.
        try { p.release() } catch (e: Exception) {
            LogManager.addLog("WARN", "Ringback", "release failed resource=$previous reason=$reason " +
                "player=${System.identityHashCode(p)}; retained for retry: $e")
            return
        }
        player = null
        resourceId = 0
        LogManager.addLog("INFO", "Ringback", "released resource=$previous reason=$reason player=${System.identityHashCode(p)}")
    }

    private fun checkMain() = check(Looper.myLooper() == Looper.getMainLooper()) {
        LogManager.addLog("ERROR", "Ringback", "operation requires main thread")
        "CallRingback requires main thread"
    }
}
