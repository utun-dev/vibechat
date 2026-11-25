/* Значения физических сочетаний Windows/X11, не Qt keycodes и не Wayland portal shortcuts.
 * Коды ОС 8..255 зависят от платформы; левые/правые модификаторы различаются.
 * encode/decode задают формат одного сочетания для ui_state; захват строится из полных снимков удерживаемых клавиш. */
#pragma once
#include <QSet>
#include <QString>
#include <QStringList>
#include <algorithm>

/* Множество одновременно нужных кодов, пустое означает отсутствие сочетания. */
using PttKeyChord = QSet<int>;
/* true, если непустой chord целиком входит в held; дополнительные удерживаемые клавиши допустимы. */
inline bool pttChordHeld(const PttKeyChord& chord, const PttKeyChord& held) {
    return !chord.isEmpty() && (chord - held).isEmpty();
}
/* Сортирует коды по возрастанию и соединяет десятичные числа запятыми; пустое множество даёт пустую строку.
 * Диапазон кодов здесь не проверяется; передавать допустимый PttKeyChord. */
inline QString pttEncodeChord(const PttKeyChord& chord) {
    auto keys = chord.values();
    std::sort(keys.begin(), keys.end());
    QStringList values;
    for (int key : keys) values << QString::number(key);
    return values.join(',');
}
/* Разбирает целые 8..255, удаляя дубликаты; пустой ввод либо ошибка любого элемента возвращают пустое множество. */
inline PttKeyChord pttDecodeChord(const QString& value) {
    PttKeyChord chord;
    if (value.isEmpty()) return chord;
    for (const auto& part : value.split(',')) {
        bool ok = false;
        int key = part.toInt(&ok);
        if (!ok || key < 8 || key > 255) return {};
        chord.insert(key);
    }
    return chord;
}

/* Сохраняет самый большой реально одновременный снимок, при равной мощности — последний.
 * Создать/сбросить объект перед новой записью; последовательные нажатия не объединяются. */
struct PttChordCapture {
    PttKeyChord candidate; /* Выбранный снимок; частичное отпускание его не сокращает. */
    /* true только при полном отпускании после непустого candidate: запись можно завершить.
     * Сам candidate после true не очищается; передавать вызовы в порядке опроса клавиатуры. */
    bool observe(const PttKeyChord& held) {
        if (held.isEmpty()) return !candidate.isEmpty();
        if (held.size() >= candidate.size()) candidate = held;
        return false;
    }
};
