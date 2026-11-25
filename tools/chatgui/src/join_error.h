/* Единые тексты ошибок JOIN/OFFER для Qt. Вызывать только для ненулевого CHAT_CONNECT_* результата.
 * Общий код не раскрывает точную внутреннюю причину: её нельзя угадывать по числу.
 * Вызывающий диалог логирует request/channel/peer/result; неизвестный код получает общий текст ошибки. */
#pragma once

#include <QCoreApplication>
#include <QString>

#include "../../../src/chat/chat_sync.h"

inline QString joinErrorText(int result) {
    switch (result) {
    case CHAT_CONNECT_AUTH:
        return QCoreApplication::translate("ChatGui", "Invitation authorization failed.");
    case CHAT_CONNECT_INVITE_DENIED:
        return QCoreApplication::translate("ChatGui",
            "You do not have permission to invite members to this group. Ask the owner or an administrator.");
    case CHAT_CONNECT_SERVER_KEY:
        return QCoreApplication::translate("ChatGui", "The server access key is invalid or has changed.");
    case CHAT_CONNECT_INVALID:
        return QCoreApplication::translate("ChatGui", "Invalid request or chat service unavailable.");
    case CHAT_CONNECT_TIMEOUT:
        return QCoreApplication::translate("ChatGui", "The request timed out.");
    case CHAT_CONNECT_BUSY:
        return QCoreApplication::translate("ChatGui", "Another join or invitation is in progress. Try again later.");
    case CHAT_CONNECT_CANCELLED:
        return QCoreApplication::translate("ChatGui", "The request was cancelled.");
    default:
        return QCoreApplication::translate("ChatGui", "Could not complete the request.");
    }
}
