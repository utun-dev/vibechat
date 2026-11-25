#include "message_links.h"
#include <QRegularExpression>
#include <QUrl>
#include <cstring>
#include "../../../src/chat/object_link.h"

/* Отделить адрес от текста, сохраняя сбалансированные скобки внутри пути/query. */
QVector<MessageLink> messageLinks(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral(
        "(?:utun://|(?i:https?://))[^\\s\\p{Z}\\p{Cc}<>\"'`«»“”‘’]+"));
    QVector<MessageLink> links;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        QString url = match.captured();
        {
            // Один подсчёт и один проход по хвосту: повторный count() на каждой скобке даёт O(n²).
            qsizetype balance[3] = {};
            for (const QChar ch : url) {
                const int opening = QStringLiteral("([{").indexOf(ch);
                const int closing = QStringLiteral(")]}").indexOf(ch);
                if (opening >= 0) balance[opening]--;
                if (closing >= 0) balance[closing]++;
            }
            auto end = url.size();
            while (end > 0) {
                const QChar last = url.at(end - 1);
                const int closing = QStringLiteral(")]}").indexOf(last);
                const bool unmatched = closing >= 0 && balance[closing] > 0;
                if (!QStringLiteral(".,;:!?").contains(last) && !unmatched) break;
                if (closing >= 0) balance[closing]--;
                end--;
            }
            url.truncate(end);
            if (!url.startsWith(QStringLiteral("utun://"))) {
                const QUrl parsed(url, QUrl::StrictMode);
                if (!parsed.isValid() || parsed.host().isEmpty()) continue;
            }
        }
        links.append({(int)match.capturedStart(), (int)url.length(), url});
    }
    return links;
}

QString makeObjectLink(const QByteArray& groupKey, const QString& groupLabel, const QString& card, const QString& cardLabel,
    const QString& message, const QString& messageLabel) {
    object_link_target target = {};
    if (groupKey.size() != 32) return {};
    memcpy(target.group, groupKey.toHex().constData(), 64);
    target.kind = card.isEmpty() ? OBJECT_LINK_CHAT_MESSAGE : message.isEmpty() ? OBJECT_LINK_CARD : OBJECT_LINK_CARD_MESSAGE;
    const auto cardId = card.toLatin1(), messageId = message.toLatin1();
    if (cardId.size() > 64 || messageId.size() > 64) return {};
    memcpy(target.card, cardId.constData(), size_t(cardId.size())); memcpy(target.message, messageId.constData(), size_t(messageId.size()));
    char out[OBJECT_LINK_MAX + 1]; const auto g = groupLabel.toUtf8(), c = cardLabel.toUtf8(), m = messageLabel.toUtf8();
    const int size = object_link_build(&target, g.constData(), c.constData(), m.constData(), out, sizeof(out));
    return size < 0 ? QString() : QString::fromUtf8(out, size);
}
