/* Разметка текста сообщений: **жирный**, __курсив__, ~~зачёркнутый~~ и отдельные строки ```[язык]/```.
 * Исходник модели не меняется. GUI-поток получает кешированный видимый текст, ссылки и цвета синтаксиса.
 * Один QTextLayout используется для размеров, рисования и выделения; clipboard берёт MessageText::text. */
#pragma once

#include "message_links.h"
#include <QModelIndex>
#include <QSharedPointer>
#include <QTextLayout>

struct MessageCodeBlock {
    int start; // Начало кода в видимом тексте, UTF-16.
    int length; // Длина без строк-ограждений; ноль означает пустой блок.
    QString language; // Имя из ограждения: пустое включает автоопределение, неизвестное/text/plain отключает раскраску.
    QString languageName; // Имя для шапки после подготовки подсветки; пусто означает обычный text.
};

struct MessageText {
    QString text; // Видимый текст без распознанных маркеров; настоящие переводы строк сохранены.
    QVector<QTextLayout::FormatRange> styles; // Жирный, курсив и зачёркивание; диапазоны UTF-16.
    QVector<MessageCodeBlock> code; // Непересекающиеся блоки в порядке текста.
    QVector<MessageLink> links; // Адреса вне кода, в координатах видимого текста.
    QVector<QTextLayout::FormatRange> lightSyntax, darkSyntax; // Только цвета токенов, без изменения метрик шрифта.
    bool changed = false; // Хотя бы один маркер/escape обработан; иначе применяется прежняя отрисовка.

    // Принадлежность позиции UTF-16 к коду, включая пустые строки/блоки. Для исключения ссылок и emoji.
    bool isCode(int position) const;
};

// Чистый разбор без загрузки библиотеки синтаксиса. Незакрытые маркеры остаются буквально; вложенности нет.
MessageText parseMessageText(const QString& source);

// GUI-поток: кеш разметки/цветов с ограничением памяти. nullptr для обычного текста, медиа, опросов и compact invite.
QSharedPointer<const MessageText> formattedMessageText(const QModelIndex& index);

// Настроить layout с уже заданным базовым font; width > 0, dark выбирает цвета. Возвращает высоту строк.
qreal layoutFormattedMessage(QTextLayout& layout, const MessageText& message, int width, bool dark);

// Прямоугольник Copy в координатах готового layout; пустой, если у блока нет строки.
QRectF messageCodeCopyRect(const QTextLayout& layout, const MessageCodeBlock& block, int width);
