/**
 * mem — выделение памяти с защитными полями, местом аллокации и учётом живых блоков.
 * Использовать u_malloc/u_calloc/u_realloc/u_strdup и парный u_free; обычный free к этим блокам неприменим.
 * Макросы передают LOCATION автоматически. OOM возвращает NULL; обнаруженное повреждение завершает процесс.
 * realloc живого блока при ошибке сохраняет его, при size=0 освобождает; NULL обрабатывается как malloc.
 * Размеры аргументов _impl — uint32_t; до вызова проверить, что исходные size_t помещаются в этот диапазон.
 * Список аллокаций и счётчики защищены мьютексом; отдельные блоки можно выделять/освобождать параллельно.
 * Передачу одного блока другому потоку согласует вызывающий; одновременные free/realloc/check одного
 * блока недопустимы. Защитные поля проверяются только у живого собственного блока.
 * Отчёт копирует метаданные под lock, печатает вне lock и не читает изменяемый пользовательский payload.
 */
#ifndef MEM_H
#define MEM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define XSTR(s) STR(s)
#define STR(s) #s
#define LOCATION __FILE__ ":" XSTR(__LINE__)
// Проверяет canary/padding живого блока; text/location — строки диагностики. NULL ptr допустим.
// Обнаруженное повреждение логируется в SYS и завершает процесс через exit(EXIT_FAILURE).
void u_check(void* ptr, const char* text, const char* location);
#define u_malloc(size) u_malloc_impl(size, LOCATION)
#define u_calloc(nmemb, size) u_calloc_impl(nmemb, size, LOCATION)
#define u_realloc(ptr, size) u_realloc_impl(ptr, size, LOCATION)
#define u_free(ptr) u_free_impl(ptr, LOCATION)
#define u_strdup(s) u_strdup_impl(s, LOCATION)
// Выделяет size байт с метаданными; NULL при OOM/переполнении полного размера. size=0 тоже создаёт учтённый блок.
// location — обязательная строка места вызова, копируется в метаданные; payload не обнуляется.
void* u_malloc_impl(uint32_t size, const char* location);
// Выделяет и обнуляет nmemb*size байт; NULL при произведении > UINT32_MAX или ошибке malloc.
void* u_calloc_impl(uint32_t nmemb, uint32_t size, const char* location);
// NULL ptr → malloc(size), включая size=0. Для живого ptr: size=0 → free и NULL;
// иначе новый блок с min(старый размер, size) байтами данных. При OOM возвращает NULL, старый блок сохранён.
// Результат сохранять временно: tmp = u_realloc(ptr, size); if (tmp) ptr = tmp; (при size > 0).
void* u_realloc_impl(void* ptr, uint32_t size, const char* location);
// Проверяет и освобождает живой блок из этих обёрток; NULL допустим. Обычный free к нему неприменим.
void u_free_impl(void* ptr, const char* location);
// Копирует ASCIIZ-строку (strlen(s)+1 должно помещаться в uint32_t); NULL при NULL s/OOM. Освобождать u_free.
char* u_strdup_impl(const char* s, const char* location);
// Потокобезопасный снимок статистики/мест аллокаций. Может устареть к моменту печати; блоки не освобождает.
// Временный снимок использует обычный malloc; при OOM логирует отказ, без небезопасного обхода списка.
void u_report_unfreed_blocks(void);
// Под lock возвращает число живых блоков на момент чтения; значение может сразу измениться другим потоком.
size_t u_get_allocated_count(void);
#ifdef __cplusplus
}
#endif
#endif // MEM_H
