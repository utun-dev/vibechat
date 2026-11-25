package com.utun.chat.viewmodel

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import androidx.lifecycle.createSavedStateHandle
import com.utun.chat.ChatApplication
import com.utun.chat.data.*
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collect

/** Снимок браузера: members исключает удалённые записи, servers дополнительно требует подписанную storageRole.
 * entries публикуются только после полного LIST; loading относится к LIST, mutating — к MKDIR/RENAME/REMOVE.
 * status — MF-ошибка либо null; quota — отдельный успешный ответ для выбранного UID, без виртуального корня. */
data class FilesBrowserState(val location: FileLocation? = null, val members: List<ChatMember> = emptyList(),
                             val servers: List<ChatMember> = emptyList(), val entries: List<MemberFilesProtocol.Entry> = emptyList(),
                             val loading: Boolean = false, val mutating: Boolean = false, val status: Int? = null,
                             val quota: MemberFilesProtocol.Quota? = null)

/**
 * Main-thread ViewModel файлового браузера. group/server/owner/path сохраняются в SavedStateHandle,
 * последний server также запоминается отдельно для каждой группы. owner=0 открывает виртуальный список UID.
 * Вызвать open/openLocation, затем setVisible по lifecycle экрана: скрытие/навигация отменяют LIST и QUOTA,
 * уничтожение ViewModel отменяет её корутины. Передачи приложения продолжаются в MemberFilesRepository.
 *
 * LIST собирает единый снимок cursor/version, отклоняет дубликаты и отсутствие прогресса страниц.
 * BUSY повторяется через секунду до 30 раз, STALE перезапускает LIST один раз; частичный список не показывается.
 * QUOTA загружается параллельно, её отказ логируется и не подменяет успешный список ошибкой.
 * Мутация использует версию entry и собственный UID, служебное avatars доступно только для чтения.
 */
class MemberFilesViewModel(private val saved: SavedStateHandle) : ViewModel() {
    companion object { val Factory = viewModelFactory { initializer { MemberFilesViewModel(createSavedStateHandle()) } } }
    val repository = ChatApplication.instance.memberFiles
    private val membersRepository = ChatApplication.instance.members
    private val _state = MutableStateFlow(FilesBrowserState())
    val state: StateFlow<FilesBrowserState> = _state
    private var load: Job? = null
    private var quota: Job? = null
    private var visible = false
    /** Контекст внешнего CreateDocument; переживает пересоздание Activity с той же ViewModel, но не смерть процесса. */
    var pendingSave: FileExport? = null
    /** Каталог, выбранный до OpenMultipleDocuments; ответ picker не использует текущую навигацию. */
    var pickerLocation: FileLocation? = null
    /** Последняя выданная копия для повторного SAVE без нового download; не хранится в SavedStateHandle. */
    var readyExport: FileExport? = null
    val operation = repository.operation
    val conflict = repository.conflict
    val export = repository.export

    init {
        viewModelScope.launch {
            membersRepository.members.collect { all ->
                val location = _state.value.location ?: return@collect
                val group = location.group
                val members = all.filterKeys { it.group == group }.values.filter { !it.isDeleted }
                    .sortedWith(compareByDescending<ChatMember> { it.isSelf }.thenBy { it.name.lowercase() })
                val servers = members.filter { it.storageRole && MemberFilesProtocol.validAddress(it.ed25519) }
                val wasAvailable = _state.value.servers.any { it.ed25519 == location.server }
                _state.value = _state.value.copy(members = members, servers = servers)
                if (location.server == "" && servers.isNotEmpty()) selectServer(servers.first().ed25519)
                else if (!wasAvailable && servers.any { it.ed25519 == location.server } && visible) refresh()
                else if (location.server != "" && servers.none { it.ed25519 == location.server }) {
                    load?.cancel(); quota?.cancel()
                    _state.value = _state.value.copy(entries = emptyList(), quota = null, loading = false, status = 1)
                }
            }
        }
        viewModelScope.launch {
            repository.revision.collect {
                val op = repository.operation.value
                if (visible && op?.location == _state.value.location) refresh()
            }
        }
    }

    /** Открыть группу и владельца: при первом восстановлении того же контекста взять сохранённый путь,
     * иначе открыть корень owner с последним сервером этой группы. Обновить снимок участников. */
    fun open(group: String, owner: String) {
        if (!MemberFilesProtocol.validAddress(group)) { LogManager.addLog("ERROR", "Files", "Invalid channel=$group"); return }
        val oldGroup = saved.get<String>("group")
        val savedOwner = saved.get<String>("owner")
        val location = if (oldGroup == group && savedOwner == owner && _state.value.location == null)
            FileLocation(group, saved.get<String>("server") ?: "", owner, saved.get<String>("path") ?: "")
        else FileLocation(group, saved.get<String>("server:$group") ?: "", owner, "")
        navigate(location)
        updateMembers(group)
        membersRepository.refresh(group)
    }

    /** Открыть явный адрес, например из уведомления передачи, и обновить участников.
     * Ненулевой server сохраняется; при server=0 выбирается первый известный хранитель. */
    fun openLocation(location: FileLocation) {
        navigate(location)
        val group = location.group
        updateMembers(group)
        membersRepository.refresh(group)
    }

    private fun updateMembers(group: String) {
        val members = membersRepository.members.value.filterKeys { it.group == group }.values.filter { !it.isDeleted }
            .sortedWith(compareByDescending<ChatMember> { it.isSelf }.thenBy { it.name.lowercase() })
        val servers = members.filter { it.storageRole }
        _state.value = _state.value.copy(members = members, servers = servers)
        if (_state.value.location?.server == "" && servers.isNotEmpty()) selectServer(servers.first().ed25519)
        else if (visible) refresh()
    }

    /** true запускает refresh; false отменяет только LIST/QUOTA и снимает loading, не отменяя app-scoped передачу. */
    fun setVisible(value: Boolean) {
        visible = value
        if (value) refresh() else { load?.cancel(); quota?.cancel(); _state.value = _state.value.copy(loading = false) }
    }

    private fun navigate(location: FileLocation) {
        require(MemberFilesProtocol.validPath(location.path) && (location.owner != "" || location.path.isEmpty()))
        load?.cancel(); quota?.cancel()
        saved["group"] = location.group; saved["server"] = location.server
        saved["owner"] = location.owner; saved["path"] = location.path
        _state.value = _state.value.copy(location = location, entries = emptyList(), quota = null, status = null)
        LogManager.addLog("INFO", "Files", "navigate location=$location")
        if (visible) refresh()
    }

    /** Выбрать только известного подписанного хранителя; owner/path сохраняются, список перечитывается. */
    fun selectServer(server: String) {
        val location = _state.value.location ?: return
        if (_state.value.servers.none { it.ed25519 == server }) return
        saved["server:${location.group}"] = server
        navigate(location.copy(server = server))
    }
    fun selectOwner(owner: String) { _state.value.location?.let { navigate(it.copy(owner = owner, path = "")) } }
    fun enter(entry: MemberFilesProtocol.Entry) {
        val location = _state.value.location ?: return
        if (location.owner == "") {
            val owner = entry.name
            if (!MemberFilesProtocol.validAddress(owner)) { LogManager.addLog("ERROR", "Files", "Invalid owner entry=${entry.name}"); return }
            selectOwner(owner)
        } else navigate(location.copy(path = MemberFilesProtocol.child(location.path, entry.name)))
    }
    /** Перейти к родителю path, затем к виртуальному списку UID; false — уже корень, экран может закрыться. */
    fun up(): Boolean {
        val location = _state.value.location ?: return false
        if (location.path.isNotEmpty()) navigate(location.copy(path = location.path.substringBeforeLast('/', "")))
        else if (location.owner != "") selectOwner("")
        else return false
        return true
    }
    fun breadcrumb(path: String) { _state.value.location?.let { navigate(it.copy(path = path)) } }

    /** Перезапустить LIST и отдельный QUOTA лишь для видимого экрана с известным сервером.
     * Успешные entries: каталоги первыми, затем имена без учёта регистра. Отказ LIST очищает entries и задаёт status. */
    fun refresh() {
        load?.cancel(); quota?.cancel()
        val location = _state.value.location ?: return
        if (!visible || location.server == "" || _state.value.servers.none { it.ed25519 == location.server }) return
        _state.value = _state.value.copy(loading = true, status = null)
        load = viewModelScope.launch {
            try {
                var staleRetries = 0
                var busyRetries = 0
                var cursor = 0L
                var version = ByteArray(16)
                val entries = linkedMapOf<String, MemberFilesProtocol.Entry>()
                while (true) {
                    val reply = repository.request(location, MemberFilesProtocol.LIST, cursor = cursor, version = version)
                    if (reply.status == MemberFilesProtocol.BUSY && busyRetries++ < 30) { delay(1000); continue }
                    if (reply.status == MemberFilesProtocol.STALE && staleRetries++ < 1) {
                        entries.clear(); cursor = 0; version = ByteArray(16); continue
                    }
                    repository.check(reply)
                    require(cursor == 0L || version.contentEquals(reply.version)) { "directory snapshot changed without STALE" }
                    val page = MemberFilesProtocol.entries(reply.payload)
                    require(reply.cursor == cursor + page.size) { "invalid directory cursor" }
                    page.forEach { require(entries.put(it.name, it) == null) { "duplicate directory entry=${it.name}" } }
                    if (!reply.more) break
                    require(page.isNotEmpty() && reply.version.any { it != 0.toByte() }) { "directory page made no progress" }
                    cursor = reply.cursor; version = reply.version
                }
                if (_state.value.location == location) _state.value = _state.value.copy(loading = false,
                    entries = entries.values.sortedWith(compareBy<MemberFilesProtocol.Entry> { it.type != MemberFilesProtocol.DIRECTORY }
                        .thenBy { it.name.lowercase() }))
                LogManager.addLog("DEBUG", "Files", "directory ready location=$location entries=${entries.size}")
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                LogManager.addLog("ERROR", "Files", "directory failed location=$location: $e")
                if (_state.value.location == location) _state.value = _state.value.copy(loading = false, entries = emptyList(),
                    status = (e as? FileStatusException)?.status ?: 9)
            }
        }
        if (location.owner != "") quota = viewModelScope.launch {
            try {
                val base = location.copy(path = "")
                var reply = repository.request(base, MemberFilesProtocol.QUOTA)
                var retries = 0
                while (reply.status == MemberFilesProtocol.BUSY && retries++ < 30) {
                    delay(1000); reply = repository.request(base, MemberFilesProtocol.QUOTA)
                }
                val result = MemberFilesProtocol.quota(repository.check(reply).payload)
                if (_state.value.location == location) _state.value = _state.value.copy(quota = result)
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) { LogManager.addLog("WARN", "Files", "quota failed location=$location: $e") }
        }
    }

    /** MKDIR использует name, RENAME — name и entry, REMOVE — entry; работает только в собственном дереве вне avatars.
     * Конкурирующая мутация/активная операция репозитория отклоняет запуск. При успехе перечитывает текущий каталог;
     * ошибка задаёт status и логируется, CancellationException передаётся дальше. Отмена не откатывает серверный commit. */
    fun mutate(command: Int, name: String = "", entry: MemberFilesProtocol.Entry? = null) {
        val location = _state.value.location ?: return
        if (_state.value.mutating || operation.value?.active == true) return
        viewModelScope.launch {
            _state.value = _state.value.copy(mutating = true, status = null)
            try {
                require(location.owner == NativeLib.getMySigningAddress()) { "not own directory" }
                val path = if (command == MemberFilesProtocol.MKDIR) MemberFilesProtocol.child(location.path, name)
                    else MemberFilesProtocol.child(location.path, requireNotNull(entry).name)
                val target = if (command == MemberFilesProtocol.RENAME) MemberFilesProtocol.child(location.path, name) else ""
                require(!MemberFilesProtocol.servicePath(path) && !MemberFilesProtocol.servicePath(target)) { "reserved avatars" }
                repository.check(repository.request(location.copy(path = path), command, target,
                    version = if (command == MemberFilesProtocol.MKDIR) ByteArray(16) else requireNotNull(entry).version))
                LogManager.addLog("INFO", "Files", "mutation completed command=$command location=$location path=$path target=$target")
                if (_state.value.location == location) refresh()
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                LogManager.addLog("ERROR", "Files", "mutation failed command=$command location=$location: $e")
                if (_state.value.location == location) _state.value = _state.value.copy(status = (e as? FileStatusException)?.status ?: 9)
            } finally { _state.value = _state.value.copy(mutating = false) }
        }
    }
    fun clearError() { _state.value = _state.value.copy(status = null) }
}
