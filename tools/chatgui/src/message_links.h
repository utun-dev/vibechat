/* Ссылки в обычном тексте сообщения: messageLinks(text) → диапазоны для подсветки и нажатия.
 * HTTP/HTTPS без учёта регистра; utun:// только в нижнем регистре, полный URL включая подписи.
 * Для web удаляются конечная пунктуация и лишние закрывающие скобки, проверяются QUrl::StrictMode и host.
 * Для utun выделяется весь кандидат; C-кодеки объекта/приглашения вызываются при открытии.
 * Разбор не меняет сообщение, не открывает адреса и не обращается к ядру. */
#pragma once
#include <QString>
#include <QVector>

struct MessageLink {
    int start;    // Смещение начала в исходном QString, в кодовых единицах UTF-16.
    int length;   // Длина адреса в тех же единицах, без отделённой внешней пунктуации.
    QString url;  // Полный адрес в исходном написании; равен text.mid(start, length).
};

// Возвращает непересекающиеся диапазоны в порядке текста; пустой список, если подходящих ссылок нет.
QVector<MessageLink> messageLinks(const QString& text);
// Собрать постоянную ссылку: пустой card означает chat; пустой message при заданном card означает карточку.
QString makeObjectLink(const QByteArray& groupKey, const QString& groupLabel, const QString& card, const QString& cardLabel,
    const QString& message, const QString& messageLabel);
