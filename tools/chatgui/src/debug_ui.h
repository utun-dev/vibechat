/* Макросы GUI_ERROR/WARN/INFO/DEBUG: printf-формат и аргументы в общий logger, категория general.
 * Уровень, файл и фильтры задаются debug_config; используются стандартные DEBUG_* с контекстом вызова.
 * Аргументы форматирования не вычисляются, если уровень отфильтрован. Файл не содержит своего состояния. */
#pragma once

extern "C" {
#include "../../../lib/debug_config.h"
}

#define GUI_ERROR(fmt, ...) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, fmt, ##__VA_ARGS__)
#define GUI_WARN(fmt, ...)  DEBUG_WARN(DEBUG_CATEGORY_GENERAL, fmt, ##__VA_ARGS__)
#define GUI_INFO(fmt, ...)  DEBUG_INFO(DEBUG_CATEGORY_GENERAL, fmt, ##__VA_ARGS__)
#define GUI_DEBUG(fmt, ...) DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, fmt, ##__VA_ARGS__)
