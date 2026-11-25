package com.utun.chat.data

/** Код native-отказа. KEY_SAVED используется локально после подтверждённой замены файла. */
enum class StartupIssue { ERROR, DATABASE, INVALID_KEY, KEY_ACCESS, KEY_SAVED }

data class StartupFailure(
    val message: String,
    val directory: String,
    val issue: StartupIssue,
    val recreating: Boolean = false,
    val resetFailed: Boolean = false,
    val recoveryError: String? = null,
    val fromSettings: Boolean = false,
) {
    val canRecreate get() = issue == StartupIssue.DATABASE && directory.isNotBlank()

    companion object {
        /** Main: резервировать одну операцию для диалога/настроек до первой точки приостановки. */
        fun beginRecovery(current: StartupFailure?, fromSettings: Boolean): StartupFailure? {
            if (fromSettings) return if (current == null) StartupFailure("", "", StartupIssue.INVALID_KEY, recreating = true, fromSettings = true) else null
            if (current == null || current.recreating || current.canRecreate) return null
            return current.copy(recreating = true, recoveryError = null)
        }

        /** CHAT_EVT_STARTUP_FAILED: kind:1, directory_len:2LE, directory UTF-8, reason UTF-8. */
        fun decode(data: ByteArray?): StartupFailure? {
            if (data == null || data.size < 4) return null
            val kind = when (data[0].toInt()) {
                0 -> StartupIssue.ERROR
                1 -> StartupIssue.DATABASE
                2 -> StartupIssue.INVALID_KEY
                3 -> StartupIssue.KEY_ACCESS
                else -> return null
            }
            val size = (data[1].toInt() and 255) or ((data[2].toInt() and 255) shl 8)
            if (size > data.size - 4) return null
            val directory = data.decodeToString(3, 3 + size)
            val message = data.decodeToString(3 + size, data.size)
            if ('\u0000' in directory || message.isBlank() || (kind == StartupIssue.INVALID_KEY && directory.isBlank())) return null
            return StartupFailure(message, directory, kind)
        }
    }
}
