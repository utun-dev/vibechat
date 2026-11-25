package com.utun.chat.data

import org.json.JSONObject
import com.utun.chat.R

/** Постоянная цель: подписи из URL не используются; ID получены общим C-кодеком. */
data class ObjectLinkTarget(val kind: Int, val group: String, val card: String, val message: String)

object ObjectLinks {
    fun preview(message: Message): String = when {
        message.isLocked -> AppLanguage.text(R.string.attachment_encrypted_title)
        message.contentType == "audio/opus" -> AppLanguage.text(R.string.reply_voice)
        message.contentType.startsWith("image/") -> AppLanguage.text(R.string.photo)
        message.contentType.startsWith("video/") -> AppLanguage.text(R.string.video_message)
        message.contentType == "poll" -> message.feedback?.poll?.question ?: AppLanguage.text(R.string.reply_poll)
        else -> message.text.ifBlank { AppLanguage.text(R.string.message) }
    }

    fun isObjectLink(url: String): Boolean = url.startsWith("utun://chat/") || url.startsWith("utun://kanban/")

    fun decode(url: String): ObjectLinkTarget? {
        val raw = NativeLib.decodeObjectLink(url) ?: return null
        return try {
            val value = JSONObject(raw)
            ObjectLinkTarget(value.getInt("kind"), value.getString("group"), value.getString("card"), value.getString("message"))
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "LINK", "Invalid native object link result: ${e.javaClass.simpleName}"); null
        }
    }

    fun build(group: String, groupLabel: String, card: String = "", cardLabel: String = "", message: String = "",
              messageLabel: String = ""): String? =
        NativeLib.buildObjectLink(group, card, message, groupLabel, cardLabel, messageLabel)
}
