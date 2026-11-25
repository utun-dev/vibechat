/* Причины отказа конфигурации для CLI/GUI, отдельно от фильтров журнала.
 * Каждый поток хранит свой ограниченный список ошибок. Операция чтения/подготовки
 * очищает его на входе; вызывающий копирует результат до следующей операции.
 * Сообщения не должны содержать приватные ключи и другие секретные значения. */
#ifndef CONFIG_ERROR_H
#define CONFIG_ERROR_H

#include "../lib/debug_config.h"

#ifdef __cplusplus
extern "C" {
#endif

void config_error_clear(void); /* Начать новую операцию в текущем потоке. */
const char *config_error_text(void); /* Заимствованный UTF-8, пустой при отсутствии ошибок. */
/* Место первой ошибки разбора в исходном файле. option может быть пустым; line=0 вне строки. */
void config_error_set_location(int line, const char *option);
int config_error_line(void);
const char *config_error_option(void);
void config_error_append(const char *format, ...); /* Добавить причину, сохранив errno. */

#ifdef __cplusplus
}
#endif

/* Записать причину независимо от фильтра, затем вывести обычную CONFIG-диагностику. */
#define CONFIG_ERROR(...) do { \
    config_error_append(__VA_ARGS__); \
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, __VA_ARGS__); \
} while (0)

#endif
