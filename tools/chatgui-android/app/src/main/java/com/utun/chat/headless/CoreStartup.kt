package com.utun.chat.headless

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeout

/** Первоначальный запуск одного Service: конфиг → приём nativeStart → подтверждение SERVICE_STARTED.
 * Все методы вызываются на main thread. Отмена отдельного await не отменяет запуск или подготовку конфига.
 * stop закрывает ожидания и отменяет собственную coroutine; освобождением native worker владеет Service.
 * Restart/неожиданный stop закрывают прежнюю готовность и ждут recovery не дольше timeoutMillis.
 * Повторные SERVICE_STARTED вызывают onReady также для прежнего ядра после отклонённого restart. */
internal class CoreStartup(
    private val scope: CoroutineScope,
    private val isReady: () -> Boolean,
    private val onReady: () -> Unit,
    private val onFailure: (Exception) -> Unit,
    private val log: (String, String) -> Unit,
    private val timeoutMillis: Long = 10_000,
) {
    private var ready = CompletableDeferred<Unit>()
    private var job: Job? = null
    private var recovery: Job? = null
    private var requested = false
    private var closed = false

    /** prepare готовит frontend/сеть и INI; submit=true подтверждает приём start, включая уже существующий worker. */
    fun start(prepare: suspend () -> String, submit: suspend (String) -> Boolean) {
        if (job != null || closed) {
            log("WARN", "startup ignored: requested=$requested closed=$closed")
            return
        }
        job = scope.launch {
            try {
                val text = prepare()
                val alreadyRunning = isReady()
                requested = true
                check(submit(text)) { "native start rejected" }
                log("INFO", "native startup accepted; waiting for core readiness")
                // NativeLib.start не создаёт новый поток/событие, когда ядро уже готово.
                if (alreadyRunning) coreStarted()
                withTimeout(timeoutMillis) { ready.await() }
            } catch (e: TimeoutCancellationException) {
                fail(IllegalStateException("core readiness timeout after ${timeoutMillis}ms", e))
            } catch (e: CancellationException) {
                if (!closed) fail(IllegalStateException("startup coroutine cancelled", e))
                throw e
            } catch (e: Exception) {
                fail(e)
            }
        }
    }

    /** Успешный await подтверждает готовность ядра и завершённый onReady; ошибка запуска распространяется всем ожидающим. */
    suspend fun awaitReady() {
        if (!closed && requested && ready.isCompleted && !isReady()) restarting()
        ready.await()
        check(!closed) { "service startup is closed" }
    }

    fun coreStarted() {
        if (!requested || closed || !isReady()) {
            log("DEBUG", "core ready signal ignored: requested=$requested closed=$closed")
            return
        }
        try {
            onReady()
            recovery?.cancel(); recovery = null
            if (ready.complete(Unit)) log("INFO", "core readiness confirmed")
        } catch (e: Exception) { fail(e) }
    }

    /** Закрыть прежнюю готовность до async restart; тот же deferred ждёт новый/возобновлённый ready. */
    fun restarting() {
        if (closed || !requested) return
        if (ready.isCompleted) ready = CompletableDeferred()
        recovery?.cancel()
        val pending = ready
        recovery = scope.launch {
            try { withTimeout(timeoutMillis) { pending.await() } }
            catch (e: TimeoutCancellationException) { fail(IllegalStateException("core recovery timeout after ${timeoutMillis}ms", e)) }
            catch (e: CancellationException) { throw e }
            catch (e: Exception) { if (!closed) fail(e) }
        }
        log("INFO", "core readiness suspended; waiting for restart/recovery")
    }

    fun coreStopped() {
        if (!closed && requested && ready.isCompleted) restarting()
    }

    /** Также используется для ошибок init/регистрации сети до start; stopSelf вызывается один раз. */
    fun fail(error: Exception) {
        if (closed) return
        closed = true
        recovery?.cancel(); recovery = null
        ready.completeExceptionally(error)
        log("ERROR", "core startup failed: ${error.javaClass.simpleName}: ${error.message}")
        try { onFailure(error) }
        catch (e: Exception) { log("ERROR", "startup failure cleanup callback failed: ${e.javaClass.simpleName}: ${e.message}") }
    }

    /** Сначала закрывает готовность; поздний callback и готовый конфиг уже не могут завершить запуск. */
    fun stop() {
        if (!closed) {
            closed = true
            ready.completeExceptionally(IllegalStateException("service stopped during startup"))
            log("INFO", "service startup closed")
        }
        job?.cancel()
        recovery?.cancel(); recovery = null
    }
}
