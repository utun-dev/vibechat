/**
 * @file json_flat.h
 * @brief Парсер плоского JSON без вложенности: {"key":"value",...}
 *
 * Только строковые значения,обычные escapes и \uXXXX (surrogate pairs→UTF-8).
 * Нулевой Unicode-символ,непарные суррогаты,неверный hex и сырые ASCII-control отклоняются.
 * Callback получает полные key/value, действительные только до его возврата.
 * parse возвращает 0 при успехе/остановке callback, -1 при ошибке; остановка не проверяет остаток ввода.
 * Синтаксис проверяется нестрого: допускаются пропущенные и завершающие запятые;
 * хвост после закрывающей } игнорируется даже без остановки callback.
 * get ищет первый ключ и копирует значение с NUL, при малом буфере обрезает; 0 найден, -1 ошибка/нет ключа.
 */

#ifndef JSON_FLAT_H
#define JSON_FLAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*json_flat_cb)(const char* key, const char* value, void* arg);
/* Callback: 0 = continue, >0 = stop successfully, <0 = error. */

int json_flat_parse(const char* json, json_flat_cb cb, void* arg);
int json_flat_get(const char* json, const char* key, char* value_out, size_t value_sz);

#ifdef __cplusplus
}
#endif
#endif
