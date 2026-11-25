package com.utun.chat.data

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/** Main: пакет «вложение + текст» принадлежит ViewModel, UI лишь читает подтверждения своего token.
 * null означает ожидание; true — сохранение, false — отказ. Отсутствующая часть сразу true.
 * Последнее подтверждение хранится до consume/следующей отправки этому адресату, включая поколение PM. */
class ConversationSubmissions(private val scope: CoroutineScope,
                              private val log: (String, String) -> Unit = { level, message -> LogManager.addLog(level, "Composer", message) }) {
    data class State(val token: String, val attachmentSaved: Boolean?, val textSaved: Boolean?, val finished: Boolean = false)
    private val _states = MutableStateFlow<Map<ConversationTarget, State>>(emptyMap())
    val states = _states.asStateFlow()

    /** false не принимает задачи и их файлы; true запускает обе части независимо от времени жизни UI caller. */
    fun submit(target: ConversationTarget, token: String, attachment: (suspend () -> Boolean)?, text: (suspend () -> Boolean)?,
               onSaved: () -> Unit, onFailure: () -> Unit): Boolean {
        if (!scope.isActive || _states.value[target]?.finished == false) {
            log("WARN", "Submission rejected target=${target.nativeId} ownerActive=${scope.isActive}")
            return false
        }
        var state = State(token, if (attachment == null) true else null, if (text == null) true else null)
        _states.value = _states.value + (target to state)
        scope.launch(start = CoroutineStart.UNDISPATCHED) {
            try {
                state = state.copy(attachmentSaved = attachment?.invoke() ?: true)
                _states.value = _states.value + (target to state)
                state = state.copy(textSaved = text?.invoke() ?: true)
                if (state.attachmentSaved == true && state.textSaved == true) onSaved() else onFailure()
            } catch (e: CancellationException) {
                log("WARN", "Submission owner cancelled target=${target.nativeId}")
                throw e
            } catch (e: Exception) {
                log("ERROR", "Submission failed target=${target.nativeId}: $e")
                onFailure()
            } finally {
                state = state.copy(attachmentSaved = state.attachmentSaved ?: false, textSaved = state.textSaved ?: false, finished = true)
                _states.value = _states.value + (target to state)
                log(if (state.attachmentSaved == true && state.textSaved == true) "INFO" else "WARN",
                    "Submission finished target=${target.nativeId} attachment=${state.attachmentSaved} text=${state.textSaved}")
            }
        }
        return true
    }

    fun consume(target: ConversationTarget, token: String) {
        if (_states.value[target]?.let { it.token == token && it.finished } == true) _states.value = _states.value - target
    }
}
