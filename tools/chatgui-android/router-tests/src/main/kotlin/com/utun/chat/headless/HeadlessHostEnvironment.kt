package com.utun.chat.headless

/** Host-наблюдение обновлений уведомления: никаких Android services или native workers. */
object HeadlessService {
    var fileRefreshes = 0
    fun refreshFiles() { fileRefreshes++ }
}
