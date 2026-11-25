package com.utun.chat.data

import android.content.Context
import android.net.Uri
import android.os.Looper
import android.os.PowerManager
import android.provider.OpenableColumns
import android.provider.DocumentsContract
import com.utun.chat.R
import com.utun.chat.headless.HeadlessService
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.io.File
import java.io.IOException

/** Адрес каталога: u64-ID группы/хранителя/владельца и относительный UTF-8 path.
 * пустой owner — виртуальный корень группы, допустим только с пустым path. */
data class FileLocation(val group: String, val server: String, val owner: String, val path: String)
/** Состояние одной очереди/экспорта: token — локальная операция, index/count — текущий файл/длина очереди.
 * done/total — байты прогресса native, stage — ID строки UI, status — MF-ошибка либо null.
 * inactive не гарантирует успех: дополнительно проверять status/cancelled; uploaded/skipped относятся к текущему запуску очереди. */
data class FileOperation(val token: Long, val location: FileLocation, val name: String = "", val index: Int = 1,
                         val count: Int = 1, val done: Long = 0, val total: Long = 0, val stage: Int = R.string.files_preparing,
                         val status: Int? = null, val active: Boolean = true, val cancelled: Boolean = false, val retryable: Boolean = true,
                         val upload: Boolean = false, val uploaded: Int = 0, val skipped: Int = 0)
/** Конфликт загрузки: true — заменить, false — пропустить, null — отменить оставшуюся очередь. */
data class FileConflict(val token: Long, val name: String, val decision: CompletableDeferred<Boolean?>)
/** Готовая локальная копия для OPEN/SAVE/SHARE; location — исходный родительский каталог, не путь файла. */
data class FileExport(val token: Long, val location: FileLocation, val name: String, val file: File, val action: Int)
/** Статус общего C-протокола; timeout ожидания Kotlin также переводится в MF_TIMEOUT=13. */
class FileStatusException(val status: Int) : IOException("member_files status=$status")

/**
 * App-scoped владелец файловых передач; публичные вызовы и обработка событий — main thread.
 * Одновременно выполняется одна upload-очередь, download или сохранение URI. Непотреблённый export
 * также блокирует следующий запуск; отдельные request для LIST/QUOTA/мутаций живут независимо.
 * AppEventHandler передаёт события 49/50 напрямую без UI replay; контекст ответа сверяется с ожидающим ID.
 *
 * URI копируется на Dispatchers.IO в стабильный upload-source. При отмене transfer ждёт terminal
 * native-событие (закрытый handle) до удаления своего source/destination; остановка core завершает ожидания.
 * Работа удерживает wake lock и CoreActivity.transfer, ожидание решения о замене снимает оба удержания.
     * Ошибки cleanup логируются независимо; неосвобождённый wake lock сохраняется до следующей операции/остановки core.
 * Скачанная копия остаётся в cacheDir/member_files для внешнего приложения/повторного SAVE.
 */
class MemberFilesRepository(context: Context) {
    companion object { const val OPEN = 0; const val SAVE = 1; const val SHARE = 2 }
    private val app = context.applicationContext
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private data class Pending(val location: FileLocation, val command: Int, val result: CompletableDeferred<MemberFilesProtocol.Reply>)
    private data class Transfer(val location: FileLocation, val result: CompletableDeferred<MemberFilesProtocol.Progress>)
    private val pending = mutableMapOf<Long, Pending>()
    private val transfers = mutableMapOf<Long, Transfer>()
    private var task: Job? = null
    private var retry: (() -> Unit)? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private val _operation = MutableStateFlow<FileOperation?>(null)
    val operation: StateFlow<FileOperation?> = _operation
    private val _conflict = MutableStateFlow<FileConflict?>(null)
    val conflict: StateFlow<FileConflict?> = _conflict
    private val _export = MutableStateFlow<FileExport?>(null)
    val export: StateFlow<FileExport?> = _export
    private val _revision = MutableStateFlow(0L)
    val revision: StateFlow<Long> = _revision
    private fun log(level: String, message: String) = LogManager.addLog(level, "Files", message)
    private fun main() = check(Looper.myLooper() == Looper.getMainLooper()) {
        val message = "MemberFilesRepository requires main thread: caller=${Thread.currentThread().name}"
        log("ERROR", message)
        message
    }

    init {
        // При создании репозитория удаляются экспорты/источники старше суток; периодического TTL-таймера здесь нет.
        scope.launch(Dispatchers.IO) {
            try {
                val root = File(app.cacheDir, "member_files")
                val cutoff = System.currentTimeMillis() - 24 * 60 * 60 * 1000
                root.listFiles()?.filter { it.lastModified() < cutoff }?.forEach { file ->
                    if (file.isDirectory) file.listFiles()?.forEach { remove(it) }
                    remove(file)
                }
            } catch (e: Exception) { log("WARN", "Cache cleanup failed: $e") }
        }
    }

    private fun protect(active: Boolean) {
        if (active) {
            val lock = wakeLock ?: (app.getSystemService(Context.POWER_SERVICE) as PowerManager)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "utun:member_files").also { wakeLock = it; it.setReferenceCounted(false) }
            if (!lock.isHeld) {
                lock.acquire()
                log("DEBUG", "wake lock acquired token=${_operation.value?.token}")
            }
        } else {
            try {
                wakeLock?.let { if (it.isHeld) it.release() }
                wakeLock = null
            } catch (e: Exception) {
                log("ERROR", "wake lock release failed token=${_operation.value?.token}; retained for retry: $e")
            }
        }
        try { CoreActivity.transfer(active) }
        catch (e: Exception) {
            if (active) throw e
            log("ERROR", "activity release failed token=${_operation.value?.token}: $e")
        }
    }

    /** Main: ответ 49, прогресс/завершение 50 или остановка 13; чужой ID игнорируется, неверный контекст логируется. */
    fun onEvent(type: Int, bytes: ByteArray?) {
        main()
        if (type == 13) { coreStopped(); return }
        if (type !in 49..50 || bytes == null) return
        try {
            if (type == 49) {
                val r = MemberFilesProtocol.response(bytes); val p = pending[r.id] ?: return
                require(r.group == p.location.group && r.server == p.location.server && r.owner == p.location.owner &&
                    r.command == p.command && r.path == p.location.path) { "request context mismatch id=${r.id}" }
                log("DEBUG", "response id=${r.id} command=${r.command} location=${p.location} status=${r.status}")
                p.result.complete(r)
            } else {
                val r = MemberFilesProtocol.progress(bytes); val p = transfers[r.id] ?: return
                require(r.group == p.location.group && r.server == p.location.server && r.owner == p.location.owner) {
                    "transfer context mismatch id=${r.id}"
                }
                if (r.finished) {
                    log("INFO", "transfer finished id=${r.id} location=${p.location} status=${r.status} bytes=${r.done}/${r.total}")
                    p.result.complete(r)
                } else _operation.value?.let { publish(it.copy(done = r.done, total = r.total, stage = R.string.files_transferring)) }
            }
        } catch (e: Exception) { log("ERROR", "Rejected event type=$type bytes=${bytes.size}: $e") }
    }

    /** Main suspend: одна команда с новым ID и ожиданием до 60 с; ненулевой MF-status возвращается без исключения.
     * Ошибка enqueue бросает IOException, timeout — FileStatusException(13), отмена — CancellationException.
     * Незавершённый native-запрос отменяется; уже применённую серверную мутацию отмена не откатывает. */
    suspend fun request(location: FileLocation, command: Int, target: String = "", cursor: Long = 0,
                        version: ByteArray = ByteArray(16)): MemberFilesProtocol.Reply {
        main()
        val id = MemberFilesProtocol.nextId(); val result = CompletableDeferred<MemberFilesProtocol.Reply>()
        pending[id] = Pending(location, command, result)
        var finished = false
        try {
            log("DEBUG", "request id=$id command=$command location=$location cursor=$cursor")
            val wire = MemberFilesProtocol.request(id, location.owner, command, location.path, target, cursor, version)
            if (!NativeLib.fileRequest(location.group, location.server, wire)) {
                log("ERROR", "request enqueue failed id=$id command=$command location=$location")
                throw IOException("request enqueue failed id=$id")
            }
            return withTimeout(60_000) { result.await() }.also { finished = true }
        } catch (e: TimeoutCancellationException) {
            log("WARN", "request timed out id=$id command=$command location=$location")
            throw FileStatusException(13)
        } finally {
            pending.remove(id)
            if (!finished && NativeLib.isReady()) NativeLib.fileCancel(id)
        }
    }

    /** Вернуть успешный reply либо бросить FileStatusException с его MF-status; контекст ошибки логирует caller. */
    fun check(reply: MemberFilesProtocol.Reply): MemberFilesProtocol.Reply {
        if (reply.status != 0) throw FileStatusException(reply.status)
        return reply
    }

    private suspend fun transfer(location: FileLocation, local: File, upload: Boolean, version: ByteArray) {
        val id = MemberFilesProtocol.nextId(); val result = CompletableDeferred<MemberFilesProtocol.Progress>()
        transfers[id] = Transfer(location, result)
        var queued = false
        try {
            queued = NativeLib.fileTransfer(id, location.group, location.server, location.owner, location.path, local.path, upload, version)
            if (!queued) throw IOException("transfer enqueue failed id=$id")
            val r = result.await()
            if (r.status != 0) throw FileStatusException(r.status)
        } finally {
            // Временный источник/приёмник освобождаем только после закрытия native handle.
            if (queued && !result.isCompleted) {
                if (NativeLib.isReady()) NativeLib.fileCancel(id)
                withContext(NonCancellable) { runCatching { result.await() } }
            }
            transfers.remove(id)
        }
    }

    private fun publish(operation: FileOperation) {
        val previous = _operation.value
        _operation.value = operation
        // Уведомление обновляется по процентам; UI получает каждый прогресс без спама в логах.
        fun percent(op: FileOperation?) = if (op == null || op.total == 0L) -1 else (op.done.toDouble() * 100 / op.total).toInt()
        if (previous == null || previous.name != operation.name || previous.stage != operation.stage || previous.active != operation.active ||
            percent(previous) != percent(operation)) HeadlessService.refreshFiles()
    }

    private fun launch(location: FileLocation, count: Int, retryable: Boolean = true, work: suspend (Long) -> Unit) {
        main()
        if (task?.isCompleted == false || _export.value != null) {
            log("WARN", "operation rejected while busy location=$location"); return
        }
        val token = MemberFilesProtocol.nextId()
        retry = if (retryable) ({ launch(location, count, work = work) }) else null
        publish(FileOperation(token, location, count = count, retryable = retryable))
        task = scope.launch {
            try {
                protect(true)
                log("INFO", "operation started token=$token location=$location count=$count")
                work(token)
                publish(requireNotNull(_operation.value).copy(active = false, stage = R.string.files_completed))
                _revision.value++
            } catch (e: CancellationException) {
                log("INFO", "operation cancelled token=$token location=$location")
                _operation.value?.let { publish(it.copy(active = false, cancelled = true, stage = R.string.files_cancelled)) }
                _revision.value++
            } catch (e: Exception) {
                log("ERROR", "operation failed token=$token location=$location: $e")
                _operation.value?.let { publish(it.copy(active = false, status = (e as? FileStatusException)?.status ?:
                    if (e is IllegalArgumentException) 3 else 9,
                    stage = R.string.files_failed)) }
                _revision.value++
            } finally {
                _conflict.value = null
                protect(false)
                log("DEBUG", "operation cleanup token=$token wakePending=${wakeLock != null}")
            }
        }
    }

    /** Main: последовательно загрузить уникальные URI в собственный каталог вне avatars.
     * При EXISTS запрашивается решение через conflict; замена использует версию STAT.
     * Retry начинает с текущего неуспешного URI, уже опубликованные/пропущенные элементы не повторяются. */
    fun upload(location: FileLocation, uris: List<Uri>) {
        if (uris.isEmpty()) return
        val sources = uris.distinct()
        launch(location, sources.size) { token ->
            require(location.owner == NativeLib.getMySigningAddress() && !MemberFilesProtocol.servicePath(location.path))
            var uploaded = 0
            var skipped = 0
            for ((index, uri) in sources.withIndex()) {
                retry = { upload(location, sources.drop(index)) }
                protect(true)
                var source: File? = null
                try {
                    val name = withContext(Dispatchers.IO) {
                        app.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
                            if (it.moveToFirst()) it.getString(0) else null
                        } ?: throw IOException("document has no display name uri=$uri")
                    }
                    val remote = location.copy(path = MemberFilesProtocol.child(location.path, name))
                    require(!MemberFilesProtocol.servicePath(remote.path)) { "reserved avatar directory" }
                    publish(FileOperation(token, location, name, index + 1, sources.size, upload = true, uploaded = uploaded, skipped = skipped))
                    val stat = request(remote, MemberFilesProtocol.STAT)
                    var version = ByteArray(16)
                    if (stat.status == 0) {
                        if (stat.type != MemberFilesProtocol.FILE) throw FileStatusException(5)
                        val answer = CompletableDeferred<Boolean?>()
                        _conflict.value = FileConflict(token, name, answer)
                        publish(requireNotNull(_operation.value).copy(stage = R.string.files_replace_title))
                        protect(false)
                        val replace = answer.await()
                        currentCoroutineContext().ensureActive()
                        when (replace) {
                            null -> throw CancellationException("queue cancelled")
                            false -> {
                                skipped++; publish(requireNotNull(_operation.value).copy(skipped = skipped))
                                retry = { upload(location, sources.drop(index + 1)) }
                                log("INFO", "upload skipped token=$token path=${remote.path}"); continue
                            }
                            true -> version = stat.version
                        }
                        protect(true)
                        _conflict.value = null
                    } else if (stat.status != MemberFilesProtocol.NOT_FOUND) throw FileStatusException(stat.status)
                    // Владение фиксируется внутри IO до возврата на main, в том числе при отмене.
                    withContext(Dispatchers.IO) {
                        val file = File.createTempFile("upload-", ".tmp", exportDirectory()); source = file
                        app.contentResolver.openInputStream(uri)?.use { input -> file.outputStream().use { output ->
                            val bytes = ByteArray(64 * 1024)
                            while (true) { ensureActive(); val size = input.read(bytes); if (size < 0) break; output.write(bytes, 0, size) }
                        } } ?: throw IOException("document cannot be read uri=$uri")
                    }
                    transfer(remote, requireNotNull(source), true, version)
                    uploaded++; publish(requireNotNull(_operation.value).copy(uploaded = uploaded))
                    retry = { upload(location, sources.drop(index + 1)) }
                    _revision.value++
                } finally {
                    _conflict.value = null
                    withContext(NonCancellable + Dispatchers.IO) { source?.let { remove(it) } }
                }
            }
        }
    }

    private fun exportDirectory(): File {
        val directory = File(app.cacheDir, "member_files")
        if (!directory.isDirectory && !directory.mkdirs()) throw IOException("cannot create file cache")
        return directory
    }
    private fun remove(file: File) { if (file.exists() && !file.delete()) log("WARN", "Cannot remove temporary file=$file") }

    /** Main: скачать файл из родительского location по версии entry в отдельный каталог cacheDir.
     * action=OPEN/SAVE/SHARE задаёт намерение UI; успешный native finished создаёт export, не запускает Android Intent. */
    fun download(location: FileLocation, entry: MemberFilesProtocol.Entry, action: Int) {
        launch(location, 1) { token ->
            publish(FileOperation(token, location, entry.name))
            val remote = location.copy(path = MemberFilesProtocol.child(location.path, entry.name))
            var directory: File? = null
            var file: File? = null
            var retained = false
            try {
                withContext(Dispatchers.IO) {
                    val dir = File(exportDirectory(), token.toString()); directory = dir
                    if (!dir.mkdir()) throw IOException("cannot create export directory")
                    file = File(dir, entry.name)
                }
                transfer(remote, requireNotNull(file), false, entry.version)
                _export.value = FileExport(token, location, entry.name, requireNotNull(file), action); retained = true
            } finally {
                if (!retained) withContext(NonCancellable + Dispatchers.IO) { file?.let { remove(it) }; directory?.let { remove(it) } }
            }
        }
    }

    /** Снять совпадающий export с ожидания UI, сохранив его файл; caller может удержать FileExport для SAVE. */
    fun consumeExport(token: Long) { if (_export.value?.token == token) _export.value = null }
    fun decideReplace(token: Long, replace: Boolean?) {
        _conflict.value?.takeIf { it.token == token }?.decision?.complete(replace)
    }
    /** Main: отменить текущую очередь/экспорт; запросы каталога отменяет их вызывающая корутина. Cleanup асинхронный. */
    fun cancel() {
        main()
        log("INFO", "cancel requested token=${_operation.value?.token}")
        task?.cancel()
    }
    fun retry() { if (_operation.value?.active == false) retry?.invoke() }
    /** Main после остановки core: отменить работу и разблокировать все pending/transfer ожидания исключением. */
    fun coreStopped() {
        main(); cancel()
        val error = CancellationException("core stopped")
        pending.values.toList().forEach { it.result.completeExceptionally(error) }
        transfers.values.toList().forEach { it.result.completeExceptionally(error) }
        if (task?.isCompleted != false && wakeLock != null) {
            protect(false)
            log("DEBUG", "core stop cleanup wakePending=${wakeLock != null}")
        }
        CoreActivity.reset()
    }
    fun dismiss() { if (_operation.value?.active == false) _operation.value = null; HeadlessService.refreshFiles() }

    /** Main: скопировать готовый export в URI CreateDocument, после consumeExport и при свободном репозитории.
     * При ошибке/отмене пытается удалить неполный документ provider; исходная копия сохраняется.
     * Эта операция не имеет автоматического Retry: новый SAVE требует нового URI от UI. */
    fun save(export: FileExport, uri: Uri) {
        launch(export.location, 1, retryable = false) { token ->
            publish(FileOperation(token, export.location, export.name, stage = R.string.files_saving, retryable = false))
            try {
                withContext(Dispatchers.IO) {
                    app.contentResolver.openOutputStream(uri, "w")?.use { output -> export.file.inputStream().use { input ->
                        val bytes = ByteArray(64 * 1024)
                        while (true) { ensureActive(); val n = input.read(bytes); if (n < 0) break; output.write(bytes, 0, n) }
                    } } ?: throw IOException("cannot open output document uri=$uri")
                }
            } catch (e: Exception) {
                withContext(NonCancellable + Dispatchers.IO) {
                    try {
                        if (!DocumentsContract.deleteDocument(app.contentResolver, uri)) log("WARN", "Cannot delete incomplete document uri=$uri")
                    } catch (cleanup: Exception) { log("WARN", "Incomplete document cleanup failed uri=$uri: $cleanup") }
                }
                throw e
            }
            log("INFO", "document saved file=${export.file} uri=$uri")
        }
    }
}
