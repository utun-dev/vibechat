package com.utun.chat.data

import android.media.AudioAttributes
import android.media.AudioManager
import android.media.MediaPlayer
import android.os.Looper
import com.utun.chat.ChatApplication
import com.utun.chat.R

/**
 * Однократный msg_incoming.mp3 через профиль уведомлений. play вызывается на main.
 * enabled и mutedChannels — снимки настроек; их меняет main. Независимые звуки могут перекрываться.
 * Владелец хранит сильную ссылку на каждый плеер до успешного release, включая время проигрывания.
 * Каждый плеер получает release по completion/error или при ошибке настройки/запуска.
 * Ошибка сохраняет владельца для следующего callback/play; успешная очистка повторно не вызывается.
 * Play сначала повторяет отложенную очистку, даже при mute/disabled или отказе создания нового звука.
 * Ошибка прежней очистки не блокирует новый звук. Таймера повторных попыток нет.
 */
object NotificationSound {
    @Volatile var enabled: Boolean = true
    @Volatile var mutedChannels: Set<String> = emptySet()

    /** Один звук; cleanupPending отличает неудачный release от ещё играющего плеера. */
    private class Playback(val player: MediaPlayer, val channelId: String) { var cleanupPending = false }
    private val playbacks = mutableSetOf<Playback>() // Сильные ссылки до успешного release; доступ только на main.

    /** Прочитать снимок настройки mute канала/личной беседы (DM использует ID с префиксом dm:). */
    fun isMuted(channelId: String): Boolean = channelId in mutedChannels

    /** На main обновить снимок mute для channelId; уже запущенные звуки не останавливаются. */
    fun setMuted(channelId: String, muted: Boolean) {
        mutedChannels = if (muted) mutedChannels + channelId else mutedChannels - channelId
    }

    /** На main повторить отложенные release, затем при разрешённом звуке запустить один сигнал для channelId. */
    fun play(channelId: String) {
        check(Looper.myLooper() == Looper.getMainLooper()) {
            LogManager.addLog("ERROR", "Sound", "play requires main thread")
            "NotificationSound requires main thread"
        }
        for (playback in playbacks.filter { it.cleanupPending }) release(playback, "retry before play")
        if (!enabled || isMuted(channelId)) return
        val mp = try {
            val attrs = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build()
            MediaPlayer.create(ChatApplication.instance, R.raw.msg_incoming, attrs, AudioManager.AUDIO_SESSION_ID_GENERATE)
        } catch (e: Exception) {
            LogManager.addLog("WARN", "Sound", "create failed ch=$channelId: $e")
            null
        }
        if (mp == null) {
            LogManager.addLog("WARN", "Sound", "notification unavailable ch=$channelId")
            return
        }
        val playback = Playback(mp, channelId)
        playbacks.add(playback)
        try {
            mp.setOnCompletionListener { release(playback, "completed") }
            mp.setOnErrorListener { _, what, extra ->
                if (playback in playbacks) LogManager.addLog("WARN", "Sound", "playback error ch=$channelId what=$what extra=$extra")
                release(playback, "error")
                true
            }
            mp.start()
            LogManager.addLog("DEBUG", "Sound", "notification played ch=$channelId player=${System.identityHashCode(mp)}")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "Sound", "play failed ch=$channelId player=${System.identityHashCode(mp)}: $e")
            release(playback, "start failed")
        }
    }

    /** Один release независимо от остальных звуков; исключение сохраняет владельца для следующего callback/play. */
    private fun release(playback: Playback, reason: String) {
        if (playback !in playbacks) return
        val mp = playback.player
        try { mp.release() } catch (e: Exception) {
            playback.cleanupPending = true
            LogManager.addLog("WARN", "Sound", "release failed ch=${playback.channelId} reason=$reason " +
                "player=${System.identityHashCode(mp)}; retained for retry: $e")
            return
        }
        playbacks.remove(playback)
        LogManager.addLog("DEBUG", "Sound", "notification released ch=${playback.channelId} reason=$reason " +
            "player=${System.identityHashCode(mp)}")
    }
}
