package com.utun.chat.data

import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/**
 * Единственная очередь файловых операций ConfigProvider, живёт вместе с Application.
 * Последовательные submit сохраняют порядок вызова; конкурирующие запросы сериализует Mutex. IO выполняется вне UI.
 * Возвращённый Deferred принадлежит очереди: отмена coroutine, ожидающей await(), не отменяет принятую запись.
 * Чтение, формирование снимка и replace выполняются внутри submit. Лог содержит метаданные, без текста/ключей.
 */
internal class ConfigFileStore(private val log: (String, String) -> Unit) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val mutex = Mutex()
    private val sequence = AtomicLong()

    fun <T> submit(name: String, operation: suspend ConfigFileStore.() -> T): Deferred<T> {
        val id = sequence.incrementAndGet()
        log("DEBUG", "config operation queued id=$id name=$name")
        return scope.async(start = CoroutineStart.UNDISPATCHED) {
            try {
                mutex.withLock {
                    withContext(Dispatchers.IO) {
                        log("DEBUG", "config operation started id=$id name=$name")
                        val result = operation()
                        log("DEBUG", "config operation complete id=$id name=$name")
                        result
                    }
                }
            } catch (e: CancellationException) {
                log("DEBUG", "config operation cancelled id=$id name=$name")
                throw e
            } catch (e: Throwable) {
                log("ERROR", "config operation failed id=$id name=$name: ${e.javaClass.simpleName}: ${e.message}")
                throw e
            }
        }
    }

    /** Только внутри submit: записать UTF-8 во временный файл рядом с целью, sync и атомарно заменить цель. */
    fun replace(file: File, text: String) {
        val target = file.absoluteFile.toPath()
        Files.createDirectories(target.parent)
        val temporary = Files.createTempFile(target.parent, ".${file.name}.", ".tmp")
        try {
            val bytes = text.toByteArray(Charsets.UTF_8)
            FileOutputStream(temporary.toFile()).use { stream ->
                stream.write(bytes)
                stream.fd.sync()
            }
            Files.move(temporary, target, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
            log("DEBUG", "config file committed path=$target bytes=${bytes.size}")
        } finally {
            try {
                Files.deleteIfExists(temporary)
            } catch (e: Exception) {
                log("WARN", "config temporary cleanup failed path=$temporary: ${e.javaClass.simpleName}: ${e.message}")
            }
        }
    }
}
