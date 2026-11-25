/* Распознавание двойного нажатия одного PTT-хоткея; общим замком владеет RadioAudioEngine.
 * observe получает полное состояние сочетания и монотонное время в мс; повторы одного состояния игнорируются.
 * reset снимает историю двойного нажатия, сохраняя физическое состояние сочетания. */
#pragma once
#include <QtGlobal>

struct PttHotkeyState {
    bool pressed = false; // Последнее физическое состояние, включая нажатие, снявшее фиксацию.
    qint64 lastPress = -1; // Начало первого нажатия в мс, -1 — двойное нажатие не ожидается.

    /* true только на втором нажатии пары; interval — системный интервал в мс, 0 отключает распознавание. */
    bool observe(bool held, qint64 now, int interval) {
        if (pressed == held) return false;
        pressed = held;
        if (!held) return false;
        bool doublePress = lastPress >= 0 && now - lastPress < interval;
        lastPress = doublePress ? -1 : now;
        return doublePress;
    }

    /* Сохраняет pressed, чтобы удерживаемый при сбросе хоткей не включил передачу снова. */
    void reset() { lastPress = -1; }
};
