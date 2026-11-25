package com.utun.chat.data

import com.utun.chat.ChatApplication
import kotlinx.coroutines.flow.MutableStateFlow

/** Host-граница языка и контроллеров; end моделирует синхронный stopRinging из настоящего finish. */
object AppLanguage {
    var language = "en"
    var failContext = false
    var failText = false
    fun context() = ChatApplication.instance.also { check(!failContext) { "language context unavailable" } }
    fun text(id: Int): String { check(!failText) { "language text unavailable" }; return "$language:$id" }
}

object CallController {
    data class State(val ongoing: Boolean)
    val state = MutableStateFlow<State?>(null)
    var media = false
    var blocked = false
    fun blocksMedia() = blocked
    val endings = mutableListOf<Pair<Long, Int>>()
    fun hasMedia() = media
    fun mediaWarning(incoming: Boolean): Int { check(incoming); return 999 }
    fun end(id: Long, reason: Int) { endings.add(id to reason); IncomingCallManager.stopRinging() }
}

object StreamController {
    data class StreamState(val ongoing: Boolean)
    val state = MutableStateFlow<StreamState?>(null)
    val capturing get() = captureBlocked
    var captureBlocked = false
    fun blocksCapture() = captureBlocked
}

object StreamPlayerEngine {
    val viewing = MutableStateFlow(false)
}
