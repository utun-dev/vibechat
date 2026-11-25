package com.utun.chat.headless

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.async
import kotlinx.coroutines.delay

/** Один незавершённый stop на процесс. Scope принадлежит приложению, переживает onDestroy Service.
 * Методы вызываются на main thread. Новый Service ждёт await перед init; отмена этого ожидания не отменяет stop.
 * Ошибка не подтверждает stop: та же задача повторяет очистку с backoff 250 мс..30 с до успеха. */
internal class CoreShutdown(
    private val scope: CoroutineScope,
    private val stop: suspend () -> Unit,
    private val log: (String, String) -> Unit,
    private val retryWait: suspend (Long) -> Unit = { delay(it) },
) {
    private var pending: Deferred<Unit>? = null

    /** Повторный запрос присоединяется к текущей очистке; native stop должен сам перенести ожидание worker на IO. */
    fun request(): Deferred<Unit> {
        pending?.takeUnless { it.isCompleted }?.let { return it }
        return scope.async {
            log("INFO", "core shutdown started")
            var retryMs = 250L
            while (true) {
                try {
                    stop()
                    log("INFO", "core shutdown completed")
                    break
                } catch (e: CancellationException) {
                    log("ERROR", "core shutdown cancelled before completion")
                    throw e
                } catch (e: Exception) {
                    log("ERROR", "core shutdown failed; retry in ${retryMs}ms: ${e.javaClass.simpleName}: ${e.message}")
                }
                retryWait(retryMs)
                retryMs = (retryMs * 2).coerceAtMost(30_000)
            }
        }.also { pending = it }
    }

    /** Нет остановки — вернуть управление; иначе новый init ждёт успешную очистку, включая все повторы. */
    suspend fun await() { pending?.await() }
}
