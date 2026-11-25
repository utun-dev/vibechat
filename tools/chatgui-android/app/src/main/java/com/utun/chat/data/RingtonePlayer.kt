package com.utun.chat.data

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.Ringtone
import android.media.RingtoneManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.VibrationEffect
import android.os.Vibrator
import androidx.annotation.RawRes
import com.utun.chat.ChatApplication
import com.utun.chat.R

/**
 * Мелодия входящего звонка через профиль звонка (STREAM_RING) + ручная вибрация.
 *
 * Поведение по системному режиму звонка (AudioManager.getRingerMode):
 *  - NORMAL : звук + вибрация;
 *  - VIBRATE: без звука, только вибрация;
 *  - SILENT : без звука и вибрации.
 *
 * play/stop вызываются на main. Режим читается при play; ошибка звука не отменяет вибрацию.
 * Вибрация запускается вручную конечными импульсами через профиль звонка.
 *
 * API 28+  : звук через Ringtone (RingtoneManager), громкость — «громкость звонка».
 * API 26–27: MediaPlayer(STREAM_RING, loop) только после получения audio focus.
 * Потеря focus запрашивает остановку звука до следующего play, сохраняя вибрацию.
 * Stop сразу отключает callbacks/таймер; ссылки очищаются только после успешных stop/release/cancel/abandon.
 * Повтор stop или начало play повторяет незавершённую очистку. Новый звук/вибрация не заменяет неочищенного владельца.
 * Ошибка очистки звука/focus не блокирует вибрацию и наоборот. Таймера повторной очистки нет.
 */
class RingtonePlayer {

    private var ringtone: Ringtone? = null // Текущий либо ожидающий подтверждения stop плеер API 28+.
    private var mediaPlayer: MediaPlayer? = null // Текущий либо ожидающий подтверждения release плеер API 26–27.
    private var vibrator: Vibrator? = null // Сохраняется при отказе cancel, даже после снятия таймера.
    private var focusRequest: AudioFocusRequest? = null // Сохраняется до AUDIOFOCUS_REQUEST_GRANTED от abandon.
    private var focusManager: AudioManager? = null // Исходный менеджер соответствующего focusRequest.
    private var soundActive = false // При очистке callbacks выключены, хотя ссылки ещё могут оставаться.
    private var vibrating = false // Отдельно от сохранённого сервиса: false запрещает новые импульсы.
    private val attributes = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build()

    /* Вибрация идёт конечными one-shot (repeat=-1), пере-запускаемыми таймером из приложения:
       на ряде OEM-чипов (sc27xx и пр.) бесконечный repeat=0 живёт в HAL и не гасится cancel(). */
    private val vibrateHandler = Handler(Looper.getMainLooper())
    private val vibrateRunnable = object : Runnable {
        override fun run() {
            if (!vibrating) return
            val v = vibrator ?: return
            try {
                v.vibrate(VibrationEffect.createWaveform(longArrayOf(0, 1000), -1), attributes)
                if (!vibrateHandler.postDelayed(this, 2000)) {
                    LogManager.addLog("WARN", "Ringtone", "vibration timer rejected")
                    stopVibrate()
                }
            } catch (e: Exception) {
                LogManager.addLog("WARN", "Ringtone", "vibration pulse failed: $e")
                stopVibrate()
            }
        }
    }

    /** На main очистить прежнее оповещение, затем запустить resourceId/вибрацию согласно режиму ОС.
     * Пока очистка звука/focus либо вибратора не подтверждена, соответствующий новый эффект пропускается с WARN. */
    fun play(@RawRes resourceId: Int = R.raw.ringtone, preserveMedia: Boolean = false) {
        stop()
        val ctx = ChatApplication.instance
        val am: AudioManager
        val ringerMode: Int
        try {
            am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            ringerMode = am.ringerMode
        } catch (e: Exception) {
            LogManager.addLog("WARN", "Ringtone", "ringer mode unavailable: $e")
            return
        }
        if (ringerMode !in AudioManager.RINGER_MODE_SILENT..AudioManager.RINGER_MODE_NORMAL) {
            LogManager.addLog("WARN", "Ringtone", "unknown ringer mode=$ringerMode; alert suppressed")
            return
        }
        val playSound = ringerMode == AudioManager.RINGER_MODE_NORMAL
        val playVibrate = ringerMode != AudioManager.RINGER_MODE_SILENT

        LogManager.addLog("INFO", "Ringtone",
            "ringerMode=%d sound=%b vibrate=%b resource=%d".format(ringerMode, playSound, playVibrate, resourceId))

        if (playSound) playSound(resourceId, am, preserveMedia)
        if (playVibrate) startVibrate()
    }

    private fun playSound(@RawRes resourceId: Int, am: AudioManager, preserveMedia: Boolean) {
        if (ringtone != null || mediaPlayer != null || focusRequest != null) {
            LogManager.addLog("WARN", "Ringtone", "sound skipped: cleanup pending resource=$resourceId " +
                "ringtone=${ringtone != null} player=${mediaPlayer != null} focus=${focusRequest != null}")
            return
        }
        val ctx = ChatApplication.instance
        soundActive = true
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && !preserveMedia) {
                val uri = Uri.parse("android.resource://${ctx.packageName}/$resourceId")
                val r = RingtoneManager.getRingtone(ctx, uri)
                if (r == null) {
                    LogManager.addLog("ERROR", "Ringtone", "getRingtone returned null resource=$resourceId")
                    stopSound()
                    return
                }
                ringtone = r
                r.audioAttributes = attributes
                r.isLooping = true
                r.play()
                LogManager.addLog("INFO", "Ringtone", "sound started (Ringtone, STREAM_RING) " +
                    "resource=$resourceId player=${System.identityHashCode(r)}")
            } else {
                if (!preserveMedia && !requestAudioFocus(am)) { stopSound(); return }
                val outputAttributes = if (preserveMedia) AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build() else attributes
                val mp = MediaPlayer.create(ctx, resourceId, outputAttributes, AudioManager.AUDIO_SESSION_ID_GENERATE)
                if (mp == null) {
                    LogManager.addLog("ERROR", "Ringtone", "MediaPlayer.create returned null resource=$resourceId")
                    stopSound()
                    return
                }
                mediaPlayer = mp
                mp.setOnErrorListener { failed, what, extra ->
                    if (soundActive && mediaPlayer === failed) {
                        LogManager.addLog("ERROR", "Ringtone", "playback error what=$what extra=$extra")
                        stopSound()
                    }
                    true
                }
                mp.setOnCompletionListener { completed ->
                    if (soundActive && mediaPlayer === completed) {
                        LogManager.addLog("WARN", "Ringtone", "loop unexpectedly completed")
                        stopSound()
                    }
                }
                mp.isLooping = true
                mp.start()
                LogManager.addLog("INFO", "Ringtone", "sound started (MediaPlayer fallback, STREAM_RING) " +
                    "resource=$resourceId player=${System.identityHashCode(mp)}")
            }
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "Ringtone", "sound failed resource=$resourceId: $e")
            stopSound()
        }
    }

    /** На main отключить callbacks/таймер и независимо очистить звук, focus и вибратор.
     * Ошибка сохраняет владельца для следующего stop/play; успешно очищенный ресурс повторно не вызывается. */
    fun stop() {
        check(Looper.myLooper() == Looper.getMainLooper()) {
            LogManager.addLog("ERROR", "Ringtone", "operation requires main thread")
            "RingtonePlayer requires main thread"
        }
        val active = ringtone != null || mediaPlayer != null || vibrator != null || focusRequest != null
        stopSound()
        stopVibrate()
        if (active) LogManager.addLog("INFO", "Ringtone", "stop requested sound_pending=${ringtone != null || mediaPlayer != null} " +
            "focus_pending=${focusRequest != null} vibration_pending=${vibrator != null}")
    }

    private fun stopSound() {
        soundActive = false
        val r = ringtone
        if (r != null) try {
            r.stop()
            ringtone = null
            LogManager.addLog("DEBUG", "Ringtone", "ringtone stopped player=${System.identityHashCode(r)}")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "Ringtone", "ringtone stop failed player=${System.identityHashCode(r)}; retained for retry: $e")
        }
        val mp = mediaPlayer
        if (mp != null) try {
            mp.release()
            mediaPlayer = null
            LogManager.addLog("DEBUG", "Ringtone", "player released player=${System.identityHashCode(mp)}")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "Ringtone", "player release failed player=${System.identityHashCode(mp)}; retained for retry: $e")
        }
        abandonAudioFocus()
    }

    /* ── вибрация (решение принято в play(): вибрируем всегда, кроме SILENT) ── */

    private fun startVibrate() {
        if (vibrator != null) {
            LogManager.addLog("WARN", "Ringtone", "vibration skipped: cleanup pending vibrator=${System.identityHashCode(vibrator)}")
            return
        }
        try {
            val v = ChatApplication.instance.getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
            if (!v.hasVibrator()) {
                LogManager.addLog("DEBUG", "Ringtone", "no vibrator hardware")
                return
            }
            vibrator = v
            vibrating = true
            vibrateRunnable.run()
            if (vibrating) LogManager.addLog("INFO", "Ringtone", "vibration started vibrator=${System.identityHashCode(v)}")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "Ringtone", "vibrator unavailable: $e")
            stopVibrate()
        }
    }

    private fun stopVibrate() {
        vibrating = false
        vibrateHandler.removeCallbacks(vibrateRunnable)
        val v = vibrator ?: return
        try {
            v.cancel()
            vibrator = null
            LogManager.addLog("INFO", "Ringtone", "vibration stopped vibrator=${System.identityHashCode(v)}")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "Ringtone", "vibration cancel failed vibrator=${System.identityHashCode(v)}; retained for retry: $e")
        }
    }

    /* ── audio focus (fallback API 26–27) ── */

    private fun requestAudioFocus(am: AudioManager): Boolean {
        lateinit var req: AudioFocusRequest
        req = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT).setAudioAttributes(attributes)
            .setOnAudioFocusChangeListener({ change ->
                if (soundActive && focusRequest === req && change < 0) {
                    LogManager.addLog("INFO", "Ringtone", "audio focus lost change=$change; stopping sound")
                    stopSound()
                }
            }, vibrateHandler).build()
        focusManager = am
        focusRequest = req
        val result = am.requestAudioFocus(req)
        if (result != AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
            LogManager.addLog("WARN", "Ringtone", "audio focus denied result=$result; sound suppressed")
            return false
        }
        LogManager.addLog("DEBUG", "Ringtone", "audio focus granted request=${System.identityHashCode(req)}")
        return true
    }

    private fun abandonAudioFocus() {
        val req = focusRequest ?: return
        try {
            val am = requireNotNull(focusManager) { "audio focus manager missing" }
            val result = am.abandonAudioFocusRequest(req)
            if (result != AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
                LogManager.addLog("WARN", "Ringtone", "abandon focus rejected result=$result " +
                    "request=${System.identityHashCode(req)}; retained for retry")
                return
            }
            focusRequest = null
            focusManager = null
            LogManager.addLog("DEBUG", "Ringtone", "audio focus abandoned request=${System.identityHashCode(req)}")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "Ringtone", "abandon focus failed request=${System.identityHashCode(req)}; retained for retry: $e")
        }
    }
}
