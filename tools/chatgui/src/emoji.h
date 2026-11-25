/* Встроенный каталог emoji для панели и отрисовки сообщений; все обращения — в GUI-потоке.
 * builtinEmojiSet возвращает категории и пополняет карту Unicode → путь анимации, не загружая ресурсы.
 * detectEmojiOnly распознаёт только элементы встроенного каталога, а не произвольные Unicode emoji/ZWJ-последовательности. */
#pragma once

#include <QString>
#include <QVector>
#include <QHash>

enum class EmojiType { Unicode, Image, Animation }; // Встроенный набор использует только Unicode/Animation.

struct EmojiData {
    EmojiType type = EmojiType::Unicode; // Представление записи; значения строк хранятся собственной копией.
    QString   unicode; // Точная строка вставки в сообщение, включая variation selector, если он нужен записи.
    QString   shortcode; // Подсказка кнопки вида :robot:, не синтаксис замены в composer.
    QString   imagePath; // Поле модели Image; фабрики ниже его не заполняют, панель его не рисует.
    QString   animPath; // Ключ ресурса/AnimTimer для Animation; сам LottieIcon записью не принадлежит.

    /* Создать Unicode-запись с текстом u и подсказкой s; глобальная карта не меняется. */
    static EmojiData unicodeEmoji(const QString &u, const QString &s);
    /* Создать Animation-запись и записать/заменить g_animatedEmojiMap[u]=animPath.
     * Строки не проверяются; ресурс не загружается и значок в AnimTimer не регистрируется. */
    static EmojiData animatedEmoji(const QString &u, const QString &s, const QString &animPath);
};

struct EmojiCategory {
    QString name; // Заголовок вкладки; builtinEmojiSet переводит его при каждом вызове.
    QVector<EmojiData> items; // Собственные записи в порядке кнопок; один Unicode может встречаться в разных категориях.
};

/* Новый набор четырёх категорий с Unicode и Animation; каждый вызов заново регистрирует встроенные анимационные соответствия.
 * Существующие сторонние ключи глобальной карты сохраняются, совпавшие встроенные заменяются. */
QVector<EmojiCategory> builtinEmojiSet();

/* GUI-only: Unicode → ключ AnimTimer/путь TGS. Пополняется animatedEmoji/builtinEmojiSet, автоматически не очищается.
 * Содержимое карты само по себе не означает наличие загруженного LottieIcon. */
extern QHash<QString, QString> g_animatedEmojiMap;

/* 1..3 — столько точных записей встроенного набора в text; QChar::isSpace допускается между ними и по краям.
 * 0 — пустой/пробельный текст, неизвестная последовательность, обычный текст либо больше трёх записей.
 * При первом вызове кеширует уникальные Unicode из builtinEmojiSet, заодно пополняя глобальную карту;
 * дальнейшие изменения g_animatedEmojiMap не расширяют набор распознавания. */
int detectEmojiOnly(const QString &text);
