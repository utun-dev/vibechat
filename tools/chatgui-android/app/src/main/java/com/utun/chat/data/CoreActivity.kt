package com.utun.chat.data

import android.os.Looper

/** Main thread: объединяет запросы активности экрана, звонка, приглашения рации и передачи файлов через OR.
 * Пока ядро не готово, сохраняет намерение владельцев. Новый SERVICE_STARTED повторно отправляет актуальный запрос.
 * Кэш хранит последнюю принятую native-очередью команду, а не подтверждённый режим: ядро применяет команду
 * асинхронно и учитывает собственные удержания активности, например трансляции. */
object CoreActivity {
    private var visible = false
    private var call = false
    private var radioCall = false
    private var transfer = false
    private var queued: Boolean? = null
    fun visible(value: Boolean) { checkMain(); visible = value; sync() }
    fun call(value: Boolean) { checkMain(); call = value; sync() }
    fun radioCall(value: Boolean) { checkMain(); radioCall = value; sync() }
    fun transfer(value: Boolean) { checkMain(); transfer = value; sync() }

    /** Сбрасывает только кэш команды: владельцы сохраняются, следующий sync отправит актуальное намерение. */
    fun reset() {
        checkMain()
        queued = null
        LogManager.addLog("DEBUG", "Activity", "activity request cache reset")
    }

    private fun checkMain() {
        check(Looper.myLooper() === Looper.getMainLooper()) {
            val message = "CoreActivity requires main thread: caller=${Thread.currentThread().name}"
            LogManager.addLog("ERROR", "Activity", message)
            message
        }
    }

    /** Новый экземпляр ядра ещё не получал состояние владельцев активности. */
    fun onCoreStarted() {
        checkMain()
        queued = null
        LogManager.addLog("INFO", "Activity", "core started; resynchronizing activity")
        sync()
    }
    fun sync() {
        checkMain()
        if (!NativeLib.isReady()) {
            queued = null
            LogManager.addLog("DEBUG", "Activity", "activity request deferred: core is not ready")
            return
        }
        val active = visible || call || radioCall || transfer
        if (queued == active) return
        val accepted = try {
            NativeLib.setActive(active)
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "Activity", "activity request failed active=$active: ${e.javaClass.simpleName}: ${e.message}")
            throw e
        }
        if (!accepted) {
            LogManager.addLog("WARN", "Activity", "activity request deferred active=$active: native queue rejected command")
            return
        }
        queued = active
        LogManager.addLog("INFO", "Activity", "core activity queued=$active visible=$visible call=$call radioCall=$radioCall transfer=$transfer")
    }
}
