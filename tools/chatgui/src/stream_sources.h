/* Разбор снимка CHAT_EVT_STREAM_SOURCES для шапки чата и отдельных окон просмотра.
 * Формат: group:Ed25519, valid:1 (0/1), JSON-массив UTF-8 без завершающего NUL.
 * parseStreamSources заменяет out целиком только при успехе; ошибка сохраняет прежний out и логируется в stream.
 * Успешный разбор с valid=false сообщает недоступность: UI сохраняет прежний каталог, а не очищает его.
 * Разбор не сверяет выбранную группу, членство или подписи; получатель проверяет group перед применением. */
#pragma once
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QtEndian>
#include "../../../lib/debug_config.h"

struct LiveStreamSource {
    QByteArray id; // Полный Ed25519.
    quint64 epoch; // Ненулевые ID узла-источника и эпоха его публикации; ID уникален внутри снимка.
    QString name; // Отображаемое имя; пустое имя JSON заменяется ключом в hex.
    bool self = false; // Собственная публикация: числовое поле self из JSON, ненулевое после toInt.
};
struct StreamSourceSnapshot {
    QString group; // Ненулевой ID CHAT-группы при успешном разборе.
    bool valid = false; // Каталог доступен; false не означает подтверждённое отсутствие трансляций.
    QList<LiveStreamSource> sources; // Порядок JSON; применять к UI только при valid=true.
};

// data заимствован на время вызова; length — полное событие, включая 33-байтный префикс.
// true: JSON-массив с уникальными ненулевыми id (64 lowercase HEX), epoch (hex-строка) и строковым name.
// self необязателен: отсутствующее/неприводимое к int значение даёт false. false: out не меняется.
inline bool parseStreamSources(const uint8_t* data, int length, StreamSourceSnapshot& out) {
    if (!data || length < 35 || data[32] > 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop source event invalid bytes=%d", length); return false;
    }
    const QByteArray key(reinterpret_cast<const char*>(data), 32);
    StreamSourceSnapshot next; next.group = QString::fromLatin1(key.toHex()); next.valid = data[32];
    QJsonParseError error;
    auto document = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(data + 33), length - 33), &error);
    if (key == QByteArray(32, '\0') || error.error != QJsonParseError::NoError || !document.isArray()) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop source JSON invalid group=%s error=%s",
                    qPrintable(next.group), qPrintable(error.errorString())); return false;
    }
    QSet<QByteArray> seen;
    for (const auto& value : document.array()) {
        auto member = value.toObject(); bool epochOk = false;
        const auto textId = member.value("id").toString().toLatin1();
        const QByteArray id = QByteArray::fromHex(textId);
        const bool idOk = id.size() == 32 && id != QByteArray(32, '\0') && id.toHex() == textId;
        quint64 epoch = member.value("epoch").toString().toULongLong(&epochOk, 16);
        if (!idOk || !epochOk || !epoch || seen.contains(id) || !member.value("name").isString()) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop source identity invalid group=%s", qPrintable(next.group)); return false;
        }
        QString name = member.value("name").toString(); if (name.isEmpty()) name = QString::fromLatin1(textId);
        next.sources.append({id, epoch, name, member.value("self").toInt() != 0}); seen.insert(id);
    }
    out = std::move(next); return true;
}
