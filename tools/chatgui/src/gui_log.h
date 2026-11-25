/* Диагностика Qt через общий logger ядра, с его фильтрами уровней/категорий и текущим файлом вывода.
 * Qt debug/info/warning → debug/info/warn; critical/fatal → error. Категории с префиксом
 * qt.multimedia направляются в video, остальные — в general. Контекст file/line/function
 * сохраняется; QString синхронно преобразуется в UTF-8. Старый handler не вызывается.
 * Qt может вызывать обработчик из разных потоков; он использует общий debug_config.
 * Настройка logger-а и время жизни заимствованного пути его файла остаются у вызывающего. */
#pragma once
#include <QtGlobal>

// Установить handler на весь процесс, желательно до QApplication. Вернуть прежний handler;
// для восстановления передать возвращённый указатель в qInstallMessageHandler().
QtMessageHandler installGuiLogHandler();
