/* Общая настройка preset голосовых (0 Low, 1 Standard, 2 High), используется в GUI-потоке.
 * Это хранилище значения в памяти процесса, не кодер и не сохранение в БД.
 * Кодирование PCM в Opus выполняет общий voice_file через attachment_send; preset копируется в запрос отправки. */
#pragma once

#include <QString>

class VoiceEncoder {
public:
    static void setPreset(int preset); // Ограничить значение диапазоном 0..2 для последующих запросов.
    static int preset(); // Текущий preset процесса; начальное значение 1 (Standard).

    /* Создать dir с родительскими каталогами; существующий каталог допустим, ошибка только логируется. */
    static void ensureMediaDir(const QString& dir);
};
