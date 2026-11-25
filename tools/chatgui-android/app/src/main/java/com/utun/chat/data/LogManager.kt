package com.utun.chat.data

import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Одна строка диагностики; timestamp — локальное время приёма в миллисекундах, level — имя TRACE..ERROR. */
data class LogEntry(
    val timestamp: Long = System.currentTimeMillis(),
    val level: String,
    val category: String,
    val message: String
)

/** Ограниченная история в порядке приёма, общая для всех потоков.
 * revision уведомляет об изменении, а не переносит строки: медленный/новый подписчик читает актуальный getRecent().
 * Ревизии могут объединяться StateFlow; строки сохраняются до вытеснения старейших по capacity.
 */
internal class LogHistory(private val capacity: Int) {
    init { require(capacity > 0) { "Log history capacity must be positive: $capacity" } }
    private val buffer = ArrayDeque<LogEntry>(capacity)
    private val _revision = MutableStateFlow(0L)
    val revision = _revision.asStateFlow()

    @Synchronized
    fun add(entry: LogEntry) {
        if (buffer.size >= capacity) buffer.removeFirst()
        buffer.add(entry)
        _revision.value += 1
    }

    /** Независимый снимок последних count строк от старых к новым; отрицательный count вызывает IllegalArgumentException. */
    @Synchronized
    fun getRecent(count: Int): List<LogEntry> = buffer.takeLast(count)
}

/** Общий буфер последних 100000 строк Kotlin/JNI; состояние переживает Activity, при завершении процесса теряется.
 * addLog потокобезопасен: TRACE..ERROR сохраняют важность logcat, прочие level идут как DEBUG; native-фильтр/файл/UDP не используются.
 * revision — объединяемое уведомление для чтения снимка getRecent(), поэтому задержка UI не теряет строки в очереди событий.
 */
object LogManager {
    /** Категория video проходит общий native-фильтр и доставку в файл/UDP, как C-логи. */
    fun videoDebug(message: String) { NativeLib.videoLog(false, message) }
    fun videoTrace(message: String) { NativeLib.videoLog(true, message) }

    private val history = LogHistory(100000)
    val revision = history.revision

    @Synchronized
    fun addLog(level: String, category: String, message: String) {
        val priority = when (level) {
            "TRACE" -> Log.VERBOSE
            "DEBUG" -> Log.DEBUG
            "INFO" -> Log.INFO
            "WARN" -> Log.WARN
            "ERROR" -> Log.ERROR
            else -> Log.DEBUG
        }
        Log.println(priority, "utun-gui", "[$level] [$category] $message")
        history.add(LogEntry(level = level, category = category, message = message))
    }

    /** Снимок последних count строк в порядке приёма; count >= 0, превышение размера истории возвращает всю историю. */
    fun getRecent(count: Int = 100): List<LogEntry> {
        if (count < 0) {
            addLog("ERROR", "Log", "Invalid log history count=$count")
            throw IllegalArgumentException("Log history count must be non-negative: $count")
        }
        return history.getRecent(count)
    }

    /** JNI передаёт обычный UTF-8 в byte[], не Modified UTF-8 NewStringUTF: emoji сохраняются, повреждённые байты дают U+FFFD.
     * Уровни bridge: 0=TRACE..4=ERROR; DEBUG_* фильтруются в C, собственные bridge-логи могут обходить фильтр.
     * Неизвестный уровень записывается предупреждением с исходным значением; null category/message заменяются пустыми строками. */
    fun onNativeLog(level: Int, category: ByteArray?, message: ByteArray?) {
        val categoryText = category?.toString(Charsets.UTF_8).orEmpty()
        val messageText = message?.toString(Charsets.UTF_8).orEmpty()
        val lvl = when (level) {
            0 -> "TRACE"
            1 -> "DEBUG"
            2 -> "INFO"
            3 -> "WARN"
            4 -> "ERROR"
            else -> {
                addLog("WARN", "Log", "Unknown JNI log level=$level category=$categoryText message=$messageText")
                return
            }
        }
        addLog(lvl, categoryText, messageText)
    }
}
