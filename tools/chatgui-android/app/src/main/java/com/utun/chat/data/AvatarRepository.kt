package com.utun.chat.data

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.util.LruCache
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import java.io.File
import java.io.IOException
import java.security.MessageDigest

/** Собственный узел либо суперузел с подписанной storageRole; group — одна из его CHAT-групп, nodeId хранит исходный u64. */
data class AvatarStorageState(val group: String, val owner: String, val server: String, val serverName: String,
                              val ownDevice: Boolean, val pending: Boolean)
data class AvatarStorage(val group: String, val address: String, val name: String)
/** Метаданные снимка группы; hasImage независимо от наличия декодированного Bitmap в LRU. */
data class GroupAvatarState(val version: Long = 0, val hash: String = "", val canEdit: Boolean = false,
                            val loading: Boolean = false, val error: Boolean = false, val hasImage: Boolean = false)
/** SAVED: у группы получен успешный ответ core, у профиля сохранено локальное намерение; репликация ещё может ожидать. */
enum class AvatarEditState { IDLE, WORKING, SAVED, ERROR }

/**
 * Один репозиторий аватаров на приложение; публичные методы, включая suspend, вызываются на main.
 * AppEventHandler сначала обновляет MemberRepository, затем передаёт сюда живые события без UI replay.
 * Старт по 9/12 привязывает аккаунт; stop/13 меняет generation, отменяет задачи/файловые запросы
 * и очищает память. Поздние результаты прежней generation не должны применяться к новому аккаунту.
 *
 * Пользовательские объекты загружаются из member_files в filesDir/avatars/<Ed25519 hex> с проверкой SHA256;
 * одинаковые имена делят загрузку, последний уходящий waiter отменяет незавершённую операцию.
 * Две общие transfer-позиции ограничивают загрузки объектов и репликации; IO/декодирование вынесены на Dispatchers.IO.
 * LRU Bitmap — 32 МиБ. После загрузки набора prune пытается сократить дисковые объекты до 100 МиБ,
 * сохраняя текущий профиль; это очистка кеша, не жёсткая квота каталога.
 *
 * Профиль сохраняется как локальное намерение в SharedPreferences и публикуется отдельно в каждой
 * подходящей группе после загрузки оригинала, превью и манифеста. Аватар группы запрашивается/публикуется
 * отдельным core API без member_files. Каждый ожидаемый JNI-ответ имеет timeout 60 с и проверку контекста.
 */
class AvatarRepository(context: Context, private val memberRepository: MemberRepository) {
    private val app = context.applicationContext
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val transfers = Semaphore(2)
    private val preparations = Semaphore(2)
    private var generation = 0L
    private var accountAddress = ""
    private val prefs = app.getSharedPreferences("avatars", Context.MODE_PRIVATE)
    private val cache = object : LruCache<String, Bitmap>(32 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap): Int = value.allocationByteCount
    }
    private val _revision = MutableStateFlow(0L)
    /** Сигнал перечитать синхронные Bitmap-геттеры после изменения кеша/ссылок/аккаунта. */
    val revision: StateFlow<Long> = _revision
    val members: StateFlow<Map<MemberKey, ChatMember>> = memberRepository.members
    private data class AvatarReference(val server: String, val file: String, val deleted: Boolean, val storage: Boolean, val online: Boolean)
    private var memberReferences = emptyMap<MemberKey, AvatarReference>()
    private val _groups = MutableStateFlow<Map<String, GroupAvatarState>>(emptyMap())
    val groups: StateFlow<Map<String, GroupAvatarState>> = _groups
    private val _edits = MutableStateFlow<Map<String, AvatarEditState>>(emptyMap())
    val edits: StateFlow<Map<String, AvatarEditState>> = _edits
    private val loads = mutableSetOf<String>()
    private val objectLoads = mutableMapOf<String, Deferred<Unit>>()
    private val objectWaiters = mutableMapOf<String,Int>()
    private val editTokens = mutableMapOf<String, Long>()
    private val replications = mutableSetOf<String>()
    private val dirtyGroups = mutableSetOf<String>()
    private val pending = mutableMapOf<Long, Pending>()
    private var defaultAvatar: String? = null
    private val localCopies = mutableMapOf<String, String>()
    private val foreground = MutableStateFlow(false)
    /** Разрешить следующие попытки ensureUser/репликации; false не отменяет текущую передачу или запрос галереи. */
    fun setForeground(active: Boolean) { foreground.value = active }
    val accountId: String get() = accountAddress
    /** Локальное намерение профиля имеет приоритет над подписанной ссылкой; "-" означает снятый профиль. */
    fun profileBitmap(): Bitmap? = if (defaultAvatar == "-") null else
        defaultAvatar?.substringAfter('/')?.let { cache.get(it) } ?: userBitmap(null, accountAddress)
    /** Загрузить локальное превью сохранённого профиля; без своего манифеста использовать ссылку участника. */
    fun ensureProfile() {
        if (accountAddress.isEmpty()) start()
        if (accountAddress.isEmpty()) return
        val file = defaultAvatar?.substringAfter('/')
        if (file == null || !AvatarProtocol.validFile(file)) { ensureUser(null, accountAddress); return }
        if (cache.get(file) != null || !loads.add(file)) return
        val uid = accountAddress; val epoch = generation
        scope.launch {
            try {
                val bitmap = withContext(Dispatchers.IO) {
                    val root=directory(uid); AvatarAssets.image(root,AvatarAssets.record(root,file),false)
                }
                alive(epoch); cache.put(file, bitmap); changed()
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) { log("ERROR", "Default avatar cache unavailable file=$file: ${e.message}") }
            finally { if (generation == epoch) loads.remove(file) }
        }
    }

    private data class Pending(val type: Int, val group: String, val server: String, val owner: String,
                               val command: Int, val result: CompletableDeferred<ByteArray>, var cancelled: Boolean = false)
    private fun log(level: String, message: String) = LogManager.addLog(level, "Avatar", message)
    private fun changed() { _revision.value++ }
    private fun alive(epoch: Long) { if (generation != epoch || accountAddress.isEmpty()) throw CancellationException("avatar session changed") }
    private fun directory(address: String) = File(app.filesDir, "avatars/$address")
    private fun digest(bytes: ByteArray) = AvatarProtocol.hash(MessageDigest.getInstance("SHA-256").digest(bytes))
    private fun groupKey(group: String, s: GroupAvatarState) = "group:$group:${s.version}:${s.hash}"
    /** Только кеш: запросов/I/O нет; null также возможен после вытеснения Bitmap из LRU. */
    fun groupBitmap(group: String): Bitmap? = _groups.value[group]?.let { cache.get(groupKey(group, it)) }
    /** Превью подписанной ссылки участника; group=null выбирает первую подходящую запись owner среди групп. */
    fun userBitmap(group: String?, owner: String): Bitmap? {
        val member = member(group, owner)?.takeUnless { it.isDeleted } ?: return null
        return if (member.avatarServer.isNotEmpty() && AvatarProtocol.validFile(member.avatarFile)) cache.get(member.avatarFile) else null
    }
    private fun member(group: String?, owner: String): ChatMember? = if (group != null)
        members.value[MemberKey(group, owner)] else members.value.values.firstOrNull {
            it.ed25519 == owner && it.avatarServer.isNotEmpty() && AvatarProtocol.validFile(it.avatarFile)
        }
    /** Собственный узел и суперузлы-хранители; доступность проверяется загрузкой. */
    fun storageServers(): List<AvatarStorage> = members.value.mapNotNull { (k, m) ->
        if (m.storageRole && m.isSuper && !m.isDeleted && MemberFilesProtocol.validAddress(m.ed25519)) AvatarStorage(k.group, m.ed25519, m.name) else null
    }.distinctBy { it.address } + if (accountAddress.isNotEmpty()) listOf(AvatarStorage("", accountAddress, "Own storage")) else emptyList()

    /** Фактическая ссылка профиля; предпочтение копирования не является местом хранения. */
    fun storageState(group: String, owner: String): AvatarStorageState {
        val member = members.value[MemberKey(group, owner)]
        val server = member?.avatarServer.orEmpty()
        val desired = defaultAvatar?.takeUnless { it == "-" }?.substringAfter('/')
        return AvatarStorageState(group, member?.ed25519.orEmpty(), server, memberByAddress(group, server)?.name.orEmpty(),
            server == accountAddress, member?.ed25519 == accountAddress && desired != null && desired != member?.avatarFile)
    }
    fun profileStorageStates(): List<AvatarStorageState> = members.value.entries.filter { it.value.ed25519 == accountAddress }
        .map { storageState(it.key.group, it.key.owner) }.sortedBy { it.group }

    /** Вызывается из AppEventHandler, без replay UI-подписчиков. */
    fun onEvent(type: Int, data: ByteArray?) {
        if (type == 13) { reset(); return }
        if (type == 9 || type == 12) { start(); return }
        try {
            if ((type in 49..52 || type == AvatarProtocol.GROUP_SNAPSHOT_EVENT) && data != null) {
                val reply = when (type) {
                    49 -> AvatarProtocol.response(data)
                    50 -> AvatarProtocol.transfer(data) ?: return
                    51, 52 -> AvatarProtocol.published(data)
                    else -> AvatarProtocol.group(data).let { AvatarProtocol.Reply(it.id, it.group) }
                }
                val p = pending[reply.id] ?: return
                require(p.type == type && p.group == reply.group &&
                    (type !in 49..50 || (p.server == reply.server && p.owner == reply.owner &&
                        (type != 49 || p.command == reply.command)))) { "response context mismatch id=${reply.id} type=$type" }
                p.result.complete(data)
            } else if (type == 23 || type == 24 || type == 25) {
                val references = members.value.mapValues { (_, m) ->
                    AvatarReference(m.avatarServer, m.avatarFile, m.isDeleted, m.storageRole && m.isSuper, m.online)
                }
                if (references != memberReferences) {
                    memberReferences = references
                    changed()
                    references.keys.map { it.group }.distinct().forEach { replicate(it) }
                }
            } else if (type == 4 || type == 8 || type == 31) {
                if (data != null && data.isNotEmpty()) {
                    val length = data[0].toInt() and 255
                    require(length > 0 && data.size >= length + 1) { "invalid channel event" }
                    val group = String(data, 1, length)
                    if (type == 31) {
                        memberReferences = memberReferences.filterKeys { it.group != group }
                        _groups.value -= group; dirtyGroups.remove(group); changed(); return
                    }
                    if (type == 4 && _groups.value.containsKey(group)) ensureGroup(group, true)
                    memberRepository.refresh(group)
                }
            }
        } catch (e: Exception) { log("ERROR", "Rejected event type=$type bytes=${data?.size}: ${e.message}") }
    }

    private fun reset() {
        log("INFO", "Reset account=$accountAddress pending=${pending.size} cacheBytes=${cache.size()}")
        generation++
        pending.forEach { (id, p) ->
            if (p.type in 49..50 && NativeLib.isReady()) { p.cancelled = true; NativeLib.cancelAvatar(id) }
        }
        scope.coroutineContext.cancelChildren()
        pending.clear(); loads.clear(); objectLoads.clear(); objectWaiters.clear(); memberReferences = emptyMap()
        replications.clear(); dirtyGroups.clear(); editTokens.clear()
        accountAddress = ""; defaultAvatar = null; localCopies.clear(); cache.evictAll()
        _groups.value = emptyMap(); _edits.value = emptyMap(); changed()
    }
    /** Main: отменить текущую сессию и очистить память, сохранив файлы/намерение; репозиторий допускает новый start. */
    fun stop() { reset() }
    private fun start() {
        val ed = NativeLib.getMySigningAddress()
        if (!MemberFilesProtocol.validAddress(ed) || ed == accountAddress) return
        reset(); accountAddress = ed
        defaultAvatar = prefs.getString(ed, null)
        log("INFO", "Start account=$ed default=$defaultAvatar memoryLimit=32MiB diskLimit=100MiB transfers=2")
        val epoch = generation
        scope.launch {
            val channels = withContext(Dispatchers.IO) { ChatRepository().getChannels() } ?: return@launch
            alive(epoch)
            channels.forEach { requestMembers(it.id) }
        }
    }
    private fun requestMembers(group: String) {
        if (accountAddress.isEmpty()) start()
        if (accountAddress.isEmpty()) return
        if (!MemberFilesProtocol.validAddress(group)) {
            log("WARN", "Member snapshot: invalid group address=$group"); return
        }
        memberRepository.ensure(group)
    }

    private fun memberByAddress(group: String, address: String): ChatMember? =
        members.value.entries.firstOrNull { it.key.group == group && it.value.ed25519 == address }?.value

    private suspend fun request(type: Int, group: String, server: String = "", owner: String = "", command: Int = 0,
                                enqueue: (Long, String) -> Boolean): ByteArray {
        val id = MemberFilesProtocol.nextId()
        require(MemberFilesProtocol.validAddress(group)) { "invalid group address=$group" }
        val result = CompletableDeferred<ByteArray>()
        if (type in 49..50) require(MemberFilesProtocol.validAddress(server) && MemberFilesProtocol.validAddress(owner)) {
            "invalid avatar address group=$group server=$server owner=$owner"
        }
        val operation = Pending(type, group, server, owner, command, result)
        pending[id] = operation
        var complete = false
        try {
            log("DEBUG", "Request id=$id type=$type group=$group server=$server")
            if (!enqueue(id, group)) throw IOException("native enqueue failed id=$id type=$type group=$group")
            val data = withTimeout(60_000) { result.await() }
            complete = true
            return data
        } finally {
            pending.remove(id)
            if (!complete && !operation.cancelled && type in 49..50 && NativeLib.isReady()) NativeLib.cancelAvatar(id)
        }
    }

    /** Запросить снимок группы и декодировать JPEG до 128×128/16 КиБ с проверкой SHA256.
     * Без force готовый снимок переиспользуется; force во время запроса ставит один повтор после его завершения. */
    fun ensureGroup(group: String, force: Boolean = false) {
        if (accountAddress.isEmpty()) start()
        if (accountAddress.isEmpty()) return
        if (!MemberFilesProtocol.validAddress(group)) {
            log("WARN", "Group snapshot: invalid group address=$group"); return
        }
        if ("group:$group" in loads) { if (force) dirtyGroups.add(group); return }
        val current = _groups.value[group]
        if (!force && current != null && !current.error &&
            (!current.hasImage || groupBitmap(group) != null)) return
        loads.add("group:$group")
        _groups.value += group to (current ?: GroupAvatarState()).copy(loading = true)
        val epoch = generation
        scope.launch {
            try {
                val record = AvatarProtocol.group(request(AvatarProtocol.GROUP_SNAPSHOT_EVENT, group) { id, gid ->
                    NativeLib.requestGroupAvatar(id, gid)
                })
                require(!record.failed) { "core rejected stored group avatar" }
                val state = GroupAvatarState(record.version, record.hash, record.canEdit, hasImage = record.image.isNotEmpty())
                val bitmap = if (record.image.isEmpty()) null else cache.get(groupKey(group, state)) ?: withContext(Dispatchers.IO) {
                    decodeGroup(record.image, record.hash)
                }
                alive(epoch)
                if (!_groups.value.containsKey(group)) return@launch
                if (bitmap != null) cache.put(groupKey(group, state), bitmap)
                _groups.value += group to state; changed()
                log("DEBUG", "Group snapshot ready group=$group version=${state.version} bytes=${record.image.size} edit=${state.canEdit}")
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                log("ERROR", "Group load failed group=$group: ${e.message}")
                _groups.value += group to (current ?: GroupAvatarState()).copy(error = true)
            } finally {
                if (generation == epoch) {
                    loads.remove("group:$group")
                    if (dirtyGroups.remove(group) && _groups.value.containsKey(group)) ensureGroup(group, true)
                }
            }
        }
    }

    /** Поставить фоновую загрузку превью актуальной ссылки owner; до попытки ждёт foreground.
     * Обычные отказы повторяются с паузой 5..160 с и сменой источника того же хеша; отмена/timeout завершают задачу.
     * Изменение/удаление ссылки прекращает старую загрузку; готовность сообщается через revision. */
    fun ensureUser(group: String?, owner: String) {
        if (!MemberFilesProtocol.validAddress(owner)) return
        if (accountAddress.isEmpty()) start()
        if (group != null && member(group, owner) == null) requestMembers(group)
        val m = member(group, owner)?.takeUnless { it.isDeleted } ?: return
        if (m.avatarServer.isEmpty() || !AvatarProtocol.validFile(m.avatarFile) || cache.get(m.avatarFile) != null) return
        val sourceGroup = group ?: members.value.entries.firstOrNull { it.value == m }?.key?.group ?: return
        val key = m.avatarFile
        if (!loads.add(key)) return
        val epoch = generation
        scope.launch {
            var retry = 0
            try {
                while (true) {
                    alive(epoch)
                    foreground.first { it }
                    if (member(group, owner)?.let { it.isDeleted || it.avatarFile != key } != false) break
                    // Повтор берёт актуальную ссылку; тот же хеш может быть доступен через другую группу/сервер.
                    val candidates = members.value.entries.filter { !it.value.isDeleted && it.value.avatarFile == key && it.value.avatarServer.isNotEmpty() }
                        .sortedBy { if (it.key.group == sourceGroup && it.key.owner == owner) 0 else 1 }
                    if (candidates.isEmpty()) break
                    val source = candidates[retry % candidates.size]
                    try {
                        val bitmap=loadAvatar(source.key.group,source.value,false)
                        alive(epoch); cache.put(key, bitmap); changed()
                        log("INFO", "User image ready group=$sourceGroup owner=$owner file=$key retry=$retry")
                        break
                    } catch (e: CancellationException) { throw e }
                    catch (e: Exception) {
                        log("WARN", "User image failed group=${source.key.group} owner=${source.value.ed25519} server=${source.value.avatarServer} file=$key retry=$retry: ${e.message}")
                        if (member(group, owner)?.let { it.isDeleted || it.avatarFile != key } != false) break
                        delay((5000L shl retry.coerceAtMost(5))); retry++
                    }
                }
            } finally {
                if (generation == epoch) {
                    loads.remove(key)
                    if (cache.get(key) == null) changed()
                }
            }
        }
    }

    private suspend fun objectFile(group: String, m: ChatMember, file: String, limit: Int) {
        require(m.avatarServer == m.ed25519 || memberByAddress(group, m.avatarServer)?.storageRole == true) { "avatar server has no signed storage role" }
        val epoch=generation; val root=directory(accountAddress)
        val operation=objectLoads[file] ?: scope.async {
            transfers.withPermit {
                val target=File(root,file)
                val hit=withContext(Dispatchers.IO) {
                    if (!target.isFile) false else try { AvatarAssets.read(target,file,limit); true }
                    catch (e: Exception) {
                        log("WARN", "Invalid cache object=$file: ${e.message}")
                        if (!target.delete()) log("WARN", "Cannot delete invalid cache=$target")
                        false
                    }
                }
                if (!hit) {
                    withContext(Dispatchers.IO) { if (!root.isDirectory && !root.mkdirs()) throw IOException("cannot create avatar cache") }
                    val temp=File(root,".download-${MemberFilesProtocol.nextId()}")
                    try {
                        val reply=AvatarProtocol.transfer(request(50,group,m.avatarServer,m.ed25519) { id,gid ->
                            NativeLib.transferAvatar(id,gid,m.avatarServer,m.ed25519,"avatars/$file",temp.path,maxBytes=limit)
                        }) ?: throw IOException("missing transfer result")
                        require(reply.status==0 && reply.size in 1..limit) { "download status=${reply.status} size=${reply.size}" }
                        withContext(Dispatchers.IO) { AvatarAssets.store(root,file,AvatarAssets.read(temp,file,limit)) }
                    } finally { withContext(NonCancellable+Dispatchers.IO) {
                        if (temp.exists() && !temp.delete()) log("WARN", "Cannot delete download temp=$temp")
                    } }
                }
                alive(epoch); log("DEBUG", "Object ready file=$file cache=$hit group=$group")
            }
        }.also { objectLoads[file]=it }
        objectWaiters[file]=(objectWaiters[file] ?: 0)+1
        try { operation.await(); alive(epoch) }
        finally {
            if (generation==epoch) {
                val remaining=(objectWaiters[file] ?: 1)-1
                if (remaining==0) {
                    objectWaiters.remove(file)
                    if (objectLoads[file]===operation) objectLoads.remove(file)
                    if (!operation.isCompleted) { log("DEBUG", "Cancel unused object=$file"); operation.cancel() }
                } else objectWaiters[file]=remaining
            }
        }
    }

    /** Main suspend: получить набор для m.avatarFile из group/его avatarServer, не ожидая foreground.
     * Загрузка объектов разрешена у автора либо сервера с подписанной storageRole; готовое превью может вернуться сразу из LRU.
     * original=true — полный кадр с уменьшением декодирования, false — превью.
     * Ошибка/отмена передаётся вызывающему коду; превью может быть общим Bitmap LRU, recycle вызывать нельзя. */
    suspend fun loadAvatar(group: String, m: ChatMember, original: Boolean): Bitmap {
        val epoch=generation; val root=directory(accountAddress); val pinnedIntent=defaultAvatar
        if (!original) cache.get(m.avatarFile)?.let { return it }
        objectFile(group,m,m.avatarFile,AvatarProtocol.MAX_MANIFEST)
        val record=withContext(Dispatchers.IO) { AvatarAssets.record(root,m.avatarFile) }
        objectFile(group,m,if (original) record.original else record.preview,
            if (original) AvatarProtocol.MAX_ORIGINAL else AvatarProtocol.MAX_USER)
        val bitmap=withContext(Dispatchers.IO) { AvatarAssets.image(root,record,original).also { prune(root,pinnedIntent) } }
        alive(epoch); if (!original) { cache.put(m.avatarFile,bitmap); changed() }
        return bitmap
    }

    /** Группа и участник с действующей ссылкой для галереи; group=null ищет первую подходящую запись owner. */
    fun gallerySource(group: String?, owner: String): Pair<String,ChatMember>? {
        val m=member(group,owner)?.takeUnless { it.isDeleted } ?: return null
        if (m.avatarServer.isEmpty() || !AvatarProtocol.validFile(m.avatarFile)) return null
        val source=group ?: members.value.entries.firstOrNull { it.value==m }?.key?.group ?: return null
        return source to m
    }

    /** Main suspend: собрать страницы LIST avatars с cursor/version; BUSY/STALE перезапускают чтение до трёх раз.
     * Возвращает имена манифестов по убыванию mtime, затем по имени; отсутствующая текущая ссылка добавляется первой.
     * Изображения не загружаются, foreground не ожидается; ошибки/отмена передаются вызывающему коду. */
    suspend fun catalog(group: String, m: ChatMember): List<String> {
        val epoch=generation; var restarts=0
        while (true) {
            var cursor=0L; var version=ByteArray(16); val entries=linkedMapOf<String,Long>(); var restart=false
            do {
                val reply=AvatarProtocol.response(request(49,group,m.avatarServer,m.ed25519,1) { id,gid ->
                    NativeLib.avatarList(id,gid,m.avatarServer,m.ed25519,cursor,version)
                })
                alive(epoch); require(reply.path=="avatars") { "catalog path mismatch" }
                if (reply.status in listOf(6,7) && restarts++<3) { restart=true; delay(1000); break }
                require(reply.status==0) { "catalog status=${reply.status}" }
                AvatarProtocol.entries(reply).forEach { entries[it.file]=it.mtime }
                if (reply.more) require(reply.cursor>cursor) { "catalog cursor did not advance" }
                cursor=reply.cursor; version=reply.version
            } while (reply.more)
            if (restart) continue
            val files=entries.keys.sortedWith(compareByDescending<String> { entries[it] }.thenBy { it }).toMutableList()
            if (m.avatarFile !in files) files.add(0,m.avatarFile)
            log("INFO", "Catalog ready group=$group owner=${m.ed25519} count=${files.size}")
            return files
        }
    }

    /** Проверяем размер, хеш и геометрию до выделения декодированного изображения. */
    private fun decodeGroup(bytes: ByteArray, hash: String): Bitmap {
        require(bytes.size in 1..AvatarProtocol.MAX_GROUP) { "image byte limit" }
        require(digest(bytes) == hash) { "image SHA256 mismatch" }
        val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, options)
        val edge = 128
        require(options.outWidth in 1..edge && options.outHeight == options.outWidth &&
            options.outMimeType == "image/jpeg") {
            "invalid image ${options.outMimeType} ${options.outWidth}x${options.outHeight} limit=$edge"
        }
        return BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: throw IOException("image decode failed")
    }
    private fun prune(root: File, pinnedIntent: String?) {
        val pinned=mutableSetOf<String>()
        pinnedIntent?.substringAfter('/')?.takeIf { AvatarProtocol.validFile(it) }?.let { name ->
            pinned+=name
            try { val r=AvatarAssets.record(root,name); pinned+=r.original; pinned+=r.preview }
            catch (e: Exception) { log("WARN", "Cannot pin profile bundle=$name: ${e.message}") }
        }
        val entries = root.listFiles()
        if (entries == null) { log("WARN", "Cannot enumerate avatar cache directory=$root"); return }
        val files = entries.filter { AvatarProtocol.validObject(it.name) }.sortedBy { it.lastModified() }
        var size = files.sumOf { it.length() }
        for (file in files) if (size > 100L * 1024 * 1024 && file.name !in pinned) {
            val bytes = file.length()
            if (file.delete()) size -= bytes else log("WARN", "Cache eviction failed file=$file")
        }
    }

    /** Main: uri=null снимает аватар группы; иначе центральный квадрат кодируется JPEG 128×128 до 16 КиБ.
     * expectedAccount защищает от старого picker; результат core приходит через edits["group:<id>"], затем обновляется снимок. */
    fun editGroup(group: String, uri: Uri?, expectedAccount: String) = edit("group:$group", expectedAccount) { epoch ->
        val bytes = if (uri == null) ByteArray(0) else prepareGroup(uri)
        alive(epoch)
        require(editTokens["group:$group"] == epoch) { "group edit superseded" }
        val reply = AvatarProtocol.published(request(52, group) { id, gid -> NativeLib.setGroupAvatar(id, gid, bytes) })
        require(reply.status == 0) { "group publication rejected" }
        ensureGroup(group, true)
    }

    /** Main: принять во владение временный source и crop после EXIF; source удаляется и при отказе/старом аккаунте.
     * Для установки нужен server из storageServers и crop; source=null сохраняет снятие аватара, server/crop игнорируются.
     * SAVED означает локальное сохранение; загрузки файлов и публикации ссылок продолжаются отдельно по группам. */
    fun editProfile(server: String, source: File?, expectedAccount: String, crop: AvatarCrop? = null) = edit("profile", expectedAccount,source) { epoch ->
        val uid = accountAddress
        val prepared=if (source==null) null else try {
            withContext(Dispatchers.IO) { AvatarAssets.prepare(source,directory(uid),requireNotNull(crop)) }
        } finally { withContext(NonCancellable+Dispatchers.IO) {
            if (!source.delete()) log("WARN", "Cannot remove editor source=$source")
        } }
        alive(epoch)
        if (prepared != null) {
            require(storageServers().any { it.address == server }) { "no eligible storage server" }
            val (file,bitmap)=prepared
            alive(epoch)
            defaultAvatar = "$server/$file"
            localCopies.clear()
            cache.put(file,bitmap)
        } else defaultAvatar = "-"
        alive(epoch)
        val desired = defaultAvatar
        require(withContext(Dispatchers.IO) { prefs.edit().putString(uid, desired).commit() }) {
            "cannot persist avatar intent"
        }
        alive(epoch)
        changed()
        members.value.keys.map { it.group }.distinct().forEach { replicate(it) }
    }

    private fun edit(key: String, expectedAccount: String, rejectedSource: File? = null, block: suspend (Long) -> Unit) {
        if (accountAddress.isEmpty()) start()
        if (expectedAccount.isEmpty() || expectedAccount != accountAddress) {
            log("WARN", "Ignored stale picker result target=$key expected=$expectedAccount actual=$accountAddress")
            rejectedSource?.let { file -> scope.launch(Dispatchers.IO+NonCancellable) {
                if (!file.delete()) log("WARN", "Cannot remove stale editor source=$file")
            } }
            return
        }
        if (accountAddress.isEmpty() || _edits.value[key] == AvatarEditState.WORKING) {
            log("WARN", "Rejected concurrent avatar edit target=$key")
            rejectedSource?.let { file -> scope.launch(Dispatchers.IO+NonCancellable) {
                if (!file.delete()) log("WARN", "Cannot remove rejected editor source=$file")
            } }
            return
        }
        val epoch = generation; editTokens[key] = epoch
        _edits.value += key to AvatarEditState.WORKING
        scope.launch {
            try {
                block(epoch); alive(epoch); _edits.value += key to AvatarEditState.SAVED
                log("INFO", "Edit accepted target=$key")
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) { log("ERROR", "Edit failed target=$key: ${e.message}"); _edits.value += key to AvatarEditState.ERROR }
        }
    }

    private fun replicate(group: String) {
        val desired = defaultAvatar ?: return
        val self = accountAddress
        val mine = memberByAddress(group, self)?.takeUnless { it.isDeleted } ?: return
        val preferred = if (desired == "-") "" else desired.substringBefore('/')
        val file = if (desired == "-") "" else desired.substringAfter('/')
        fun replica(): String {
            val eligible = members.value.filter { (k, m) ->
                k.group == group && m.ed25519 != self && MemberFilesProtocol.validAddress(m.ed25519) && !m.isDeleted && m.isSuper && m.storageRole
            }.values
            val available = eligible.filter { it.online }
            val candidates = (available.ifEmpty { eligible }).map { it.ed25519 }.sorted()
            val current = memberByAddress(group, self)?.avatarServer.orEmpty()
            return when { current in candidates -> current; preferred in candidates -> preferred; else -> candidates.firstOrNull().orEmpty() }
        }
        if (desired == "-" && mine.avatarServer.isEmpty()) return
        val publishedServer = memberByAddress(group, mine.avatarServer)
        if (localCopies[group] == file && mine.avatarFile == file &&
            ((mine.avatarServer != self && publishedServer?.let { !it.isDeleted && it.isSuper && it.storageRole } == true) ||
                replica().isEmpty())) return
        if (!replications.add(group)) return
        val epoch = generation
        scope.launch {
            var retry = 0
            try {
                while (defaultAvatar == desired) {
                    try {
                        alive(epoch)
                        foreground.first { it }
                        transfers.withPermit {
                            suspend fun store(server: String) {
                                val root=directory(self)
                                val record=withContext(Dispatchers.IO) { AvatarAssets.record(root,file) }
                                val mkdir = AvatarProtocol.response(request(49, group, server, self, 8) { id, gid -> NativeLib.avatarMkdir(id, gid, server, self) })
                                require(mkdir.status == 0 || mkdir.status == 5) { "mkdir status=${mkdir.status}" }
                                for (name in listOf(record.original,record.preview,file)) {
                                    val local=File(root,name)
                                    val limit=if (name.endsWith(".json")) AvatarProtocol.MAX_MANIFEST else AvatarProtocol.MAX_ORIGINAL
                                    withContext(Dispatchers.IO) { AvatarAssets.read(local,name,limit) }
                                    val stat=AvatarProtocol.response(request(49,group,server,self,2) { id,gid ->
                                        NativeLib.avatarStat(id,gid,server,self,"avatars/$name")
                                    })
                                    if (stat.status==2) {
                                        val upload=AvatarProtocol.transfer(request(50,group,server,self) { id,gid ->
                                            NativeLib.transferAvatar(id,gid,server,self,"avatars/$name",local.path,true,limit)
                                        }) ?: throw IOException("missing upload result")
                                        require(upload.status==0) { "upload file=$name status=${upload.status}" }
                                    } else require(stat.status==0 && stat.fileType==1 && stat.size==local.length()) {
                                        "stat file=$name status=${stat.status} bytes=${stat.size} expected=${local.length()}"
                                    }
                                }
                            }
                            var server = ""
                            if (desired != "-") {
                                if (localCopies[group] != file) {
                                    store(self); alive(epoch); localCopies[group] = file
                                    log("INFO", "Own avatar saved group=$group file=$file")
                                }
                                if (defaultAvatar != desired) return@withPermit
                                val current = memberByAddress(group, self)?.avatarServer.orEmpty()
                                if ((current.isEmpty() || current == self) && memberByAddress(group, self)?.avatarFile != file) {
                                    val local = AvatarProtocol.published(request(51, group) { id, gid -> NativeLib.setMemberAvatar(id, gid, self, file) })
                                    require(local.status == 0) { "own avatar publication rejected" }
                                }
                                server = replica()
                                if (server.isEmpty()) {
                                    require(current.isEmpty() || current == self) { "waiting for eligible supernode; keeping published reference" }
                                    return@withPermit
                                }
                                store(server)
                            }
                            alive(epoch)
                            if (defaultAvatar != desired) return@withPermit
                            val published = AvatarProtocol.published(request(51, group) { id, gid -> NativeLib.setMemberAvatar(id, gid, server, file) })
                            require(published.status == 0) { "member publication rejected" }
                            log("INFO", "Profile published group=$group server=$server file=$file")
                        }
                        break
                    } catch (e: CancellationException) { throw e }
                    catch (e: Exception) {
                        log("WARN", "Profile pending group=$group retry=$retry: ${e.message}")
                        delay(5000L shl retry.coerceAtMost(5)); retry++
                    }
                }
            } finally {
                if (generation == epoch) {
                    replications.remove(group)
                    if (defaultAvatar != desired) replicate(group)
                }
            }
        }
    }

    /** Чтение URI, EXIF, crop и кодирование выполняются вне UI и потока ядра. */
    private suspend fun prepareGroup(uri: Uri): ByteArray = preparations.withPermit { withContext(Dispatchers.IO) {
        val temp = File.createTempFile("avatar-source-", ".image", app.cacheDir)
        try {
            app.contentResolver.openInputStream(uri)?.use { input ->
                temp.outputStream().use { output ->
                    val chunk = ByteArray(8192); var total = 0
                    while (true) {
                        val n = input.read(chunk); if (n < 0) break
                        total += n; require(total <= 32 * 1024 * 1024) { "source image exceeds 32MiB" }
                        output.write(chunk, 0, n)
                    }
                }
            } ?: throw IOException("cannot read selected image")
            val (w, h) = PhotoUtil.orientedDimensions(temp.path)
            require(w > 0 && h > 0 && w.toLong() * h <= 16L * 1024 * 1024) { "invalid source dimensions $w x $h" }
            var sample = 1
            while (maxOf(w, h) / sample > 1024) sample *= 2
            val decoded = PhotoUtil.decodeOriented(temp.path, sample) ?: throw IOException("source decode failed")
            val edge = minOf(decoded.width, decoded.height)
            val crop = Bitmap.createBitmap(decoded, (decoded.width - edge) / 2, (decoded.height - edge) / 2, edge, edge)
            val scaled = Bitmap.createScaledBitmap(crop,128,128,true)
            try {
                for (quality in listOf(85,70,50,35,20)) {
                    val output = java.io.ByteArrayOutputStream()
                    require(scaled.compress(Bitmap.CompressFormat.JPEG,quality,output)) {
                        "avatar compression failed"
                    }
                    val bytes = output.toByteArray()
                    if (bytes.size<=AvatarProtocol.MAX_GROUP) return@withContext bytes
                }
                throw IOException("encoded avatar exceeds limit")
            } finally {
                if (scaled !== crop && scaled !== decoded) scaled.recycle()
                if (crop !== decoded) crop.recycle()
                decoded.recycle()
            }
        } finally { if (!temp.delete()) log("WARN", "Cannot delete source temp=$temp") }
    } }
}
