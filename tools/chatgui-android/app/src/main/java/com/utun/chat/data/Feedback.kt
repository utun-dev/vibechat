package com.utun.chat.data

import org.json.JSONArray
import org.json.JSONObject
import org.json.JSONTokener
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CodingErrorAction

/* Модели снимка chat_feedback_snapshot, бинарные события 53/54 и подготовка форм опроса для core API.
 * Сетевой формат/подписи/подсчёт голосов остаются в C; ошибки декодирования логирует вызывающий обработчик UI. */

/** Элемент каталога/снимка реакций: id=0..15 общего каталога, count — участники, mine — выбор текущего узла.
 * Участник хранит одну реакцию; нулевой count/mine=false также используются элементами каталога UI. */
data class ReactionState(val id: Int, val emoji: String, val count: Int = 0, val mine: Boolean = false)
/** Пункт опроса: id — индекс с нуля; percent=count/voters*100, в multiple сумма процентов может превышать 100. */
data class PollOption(val id: Int, val text: String, val count: Int, val percent: Double)
/** Последний допустимый непустой ответ участника. Автор задан публичным Ed25519. */
data class PollResponse(val authorId: String, val choices: List<Int>, val custom: String)
/** Готовый снимок core: multiple допускает пункты вместе со своим текстом, single — один пункт либо текст.
 * voters включает и участников, выбравших только свой текст; ver — версия ответа текущего узла, не версия опроса.
 * myChoices/myCustom — сохранённый выбор текущего узла; answers не содержит отменённых/недопущенных ответов. */
data class PollState(val question: String, val multiple: Boolean, val allowCustom: Boolean,
                     val options: List<PollOption>, val voters: Int, val ver: Long,
                     val myChoices: List<Int>, val myCustom: String, val answers: List<PollResponse>)
/** messageId — 64 lowercase hex; pending означает отсутствие исходного сообщения у core, не ожидающую команду UI.
 * poll=null допустимо для обычного сообщения или ещё отсутствующего исходного опроса. */
data class FeedbackState(val messageId: String, val pending: Boolean, val reactions: List<ReactionState>, val poll: PollState?)

/** Уведомление 53 после commit: channelId — публичный Ed25519 в строчном HEX, kind=1 реакция/2 голос.
 * messageId — 32 бинарных байта в lowercase hex; уведомление требует нового снимка, не меняет результаты само. */
data class FeedbackChangedEvent(val channelId: String, val messageId: String, val kind: Int)
/** Ответ 54: requestId сохраняет все 64 бита; 0 используется фоновыми чтениями, ненулевой — действием пользователя.
 * json — ошибка, каталог, снимок либо committed/snapshot_pending ack; вариант определяет обработчик результата. */
data class FeedbackResultEvent(val requestId: Long, val success: Boolean, val json: JSONObject)

/** Проверить точную длину 1+channelLen+32+1, ID канала и kind; ошибка, включая null/усечение, бросает исключение. */
fun parseFeedbackChangedEvent(data: ByteArray?): FeedbackChangedEvent {
    require(data != null && data.size >= 35) { "feedback change length=${data?.size}; expected channel, message ID and kind" }
    val size = data[0].toInt() and 255
    require(size == 64 && data.size == size + 34) { "feedback change length=${data.size} channelLength=$size" }
    val channel = String(data, 1, size, Charsets.UTF_8)
    require(channel.all { it in '0'..'9' || it in 'a'..'f' } && channel.drop(16).any { it != '0' }) { "feedback change invalid channel=$channel" }
    val kind = data.last().toInt() and 255
    require(kind in 1..2) { "feedback change invalid kind=$kind channel=$channel" }
    val id = data.copyOfRange(size + 1, size + 33).joinToString("") { "%02x".format(it.toInt() and 255) }
    return FeedbackChangedEvent(channel, id, kind)
}

/** Заголовок requestId:8LE/success:1 (0/1), затем один JSON-объект в строгом UTF-8 без NUL.
 * Повреждённые байты не заменяются; ошибка бросает исключение для диагностики и error-state вызывающего UI. */
fun parseFeedbackResultEvent(data: ByteArray?): FeedbackResultEvent {
    require(data != null && data.size > 9) { "feedback result length=${data?.size}; expected 9-byte header and JSON" }
    val id = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).long
    val success = data[8].toInt()
    require(success in 0..1) { "feedback result invalid success=$success request=${id.toULong()}" }
    val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
        .decode(ByteBuffer.wrap(data, 9, data.size - 9)).toString()
    require('\u0000' !in text) { "feedback result contains NUL request=${id.toULong()}" }
    val tokener = JSONTokener(text)
    val json = JSONObject(tokener)
    require(tokener.nextClean() == '\u0000') { "feedback result trailing JSON data request=${id.toULong()}" }
    return FeedbackResultEvent(id, success == 1, json)
}

/** Нормализованный ответ формы: отправляются только активные поля; пустой выбор/текст отменяет голос. */
data class PollVoteDraft(val choices: List<Int>, val custom: String)

/** Число байтов строгого UTF-8 как в chat_poll_codec: C0 разрешены только TAB/LF/CR, NUL/непарные surrogate запрещены. */
private fun pollTextSize(text: String, limit: Int): Int? {
    if (text.length > limit || text.any { it.code < 32 && it !in "\t\n\r" }) return null
    val bytes = text.toByteArray(Charsets.UTF_8)
    return bytes.size.takeIf { it <= limit && String(bytes, Charsets.UTF_8) == text }
}

/** Проверить готовые после trim тексты формы по лимитам core: вопрос 512, пункт 256, весь poll 2048 байт.
 * 2..32 различных непустых пункта; false — обычное невалидное состояние редактируемой формы, без лог-спама. */
fun validPollCreation(question: String, options: List<String>): Boolean {
    if (question.isEmpty() || options.size !in 2..32 || options.distinct().size != options.size) return false
    var bytes = 6 + (pollTextSize(question, 512) ?: return false)
    for (option in options) {
        if (option.isEmpty()) return false
        bytes += 2 + (pollTextSize(option, 256) ?: return false)
    }
    return bytes <= 2048
}

/** Подготовить публикуемый голос: single со своим ответом игнорирует неактивные пункты, стандартный выбор — неактивный текст.
 * Trim выполняется до проверки лимита custom=1024 UTF-8 байта; пункты должны существовать и не повторяться.
 * null — форму нельзя отправить, пустой PollVoteDraft — допустимое снятие голоса. Отказ core обрабатывается отдельно. */
fun preparePollVote(poll: PollState, selected: List<Int>, custom: String, customSelected: Boolean): PollVoteDraft? {
    val own = poll.allowCustom && (poll.multiple || customSelected)
    val text = if (own) custom.trim() else ""
    val choices = if (own && !poll.multiple) emptyList() else selected.toList()
    if (pollTextSize(text, 1024) == null || own && !poll.multiple && text.isEmpty()) return null
    if (choices.size > 32 || choices.distinct().size != choices.size || choices.any { id ->
            id !in 0..31 || poll.options.none { it.id == id }
        }) return null
    if (!poll.multiple && choices.size > 1) return null
    return PollVoteDraft(choices, text)
}

private fun JSONArray?.choices(): List<Int> = if (this == null) emptyList() else (0 until length()).map { getInt(it) }

/** Разобрать JSON chat_feedback_snapshot, не изменяя его: семантику подсчёта и допуск авторов задаёт core.
 * Ошибка формата бросает исключение; ChatViewModel логирует контекст и выставляет ошибку взаимодействия.
 * Отсутствующие choices/custom трактуются как пустые; это снимок, не результат команды с snapshot_pending. */
fun parseFeedback(json: JSONObject): FeedbackState {
    val id = json.getString("message_id")
    require(id.length == 64 && id.all { it in '0'..'9' || it in 'a'..'f' }) { "Invalid message ID" }
    val reactions = json.getJSONArray("reactions").let { values ->
        (0 until values.length()).map { i -> values.getJSONObject(i).let {
            ReactionState(it.getInt("id"), it.getString("emoji"), it.getInt("count"), it.getBoolean("mine"))
        } }
    }
    val poll = if (json.isNull("poll")) null else json.getJSONObject("poll").let { value ->
        val mode = value.getString("mode")
        require(mode == "single" || mode == "multiple") { "Invalid poll mode=$mode" }
        val options = value.getJSONArray("options").let { values ->
            (0 until values.length()).map { i -> values.getJSONObject(i).let {
                PollOption(it.getInt("id"), it.getString("text"), it.getInt("count"), it.getDouble("percent"))
            } }
        }
        val mine = value.getJSONObject("mine")
        val answers = value.getJSONArray("answers").let { values ->
            (0 until values.length()).map { i -> values.getJSONObject(i).let { row ->
                val answer = row.getJSONObject("answer")
                val author = row.getString("author_id")
                require(author.length == 64 && author.all { it in '0'..'9' || it in 'a'..'f' } && author.any { it != '0' }) {
                    "Invalid feedback author key"
                }
                PollResponse(author,
                    answer.optJSONArray("choices").choices(), answer.optString("custom"))
            } }
        }
        PollState(value.getString("question"), mode == "multiple", value.getBoolean("allow_custom_answer"),
            options, value.getInt("voters"), value.getLong("ver"), mine.optJSONArray("choices").choices(), mine.optString("custom"), answers)
    }
    return FeedbackState(id, json.optBoolean("pending"), reactions, poll)
}
