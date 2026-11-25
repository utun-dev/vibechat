/*
 * Callback-провайдер отдельных настроек native-обёртки. Без провайдера/accessor возвращает default.
 * Основной конфиг ядра передаётся INI-текстом в instance_lite_start, а не через этот интерфейс.
 * set_provider копирует таблицу callbacks; функции, их окружение и возвращаемые строки принадлежат провайдеру.
 * Провайдер установить до запуска потребителей: синхронизации внутри модуля нет.
 */
#ifndef UTUN_CONFIG_API_H
#define UTUN_CONFIG_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Config provider interface ── */

typedef struct {
    /* Заимствованная NUL-строка; accessor возвращает результат callback без копирования и проверки на NULL. */
    const char* (*get_string)(const char* key, const char* default_val);
    /* Числовые callbacks сами применяют default_val при отсутствии ключа. */
    int64_t     (*get_int64)(const char* key, int64_t default_val);
    int         (*get_int)(const char* key, int default_val);
    int         (*get_bool)(const char* key, int default_val);
} utun_config_provider_t;

/* Скопировать таблицу по значению, caller может сразу освободить/изменить её; NULL отключает провайдер.
 * Функции и их окружение должны жить до отключения. Смена — без параллельных cfg_get_* вызовов. */
void utun_config_set_provider(const utun_config_provider_t* provider);

/* Имена callback-ключей; это не имена секций/полей общего INI-парсера. */

#define CFG_KEY_NODE_PRIVATE_KEY   "node.private_key"
#define CFG_KEY_NODE_PUBLIC_KEY    "node.public_key"
#define CFG_KEY_NODE_NAME          "node.name"

#define CFG_KEY_CONTROL_PORT        "control.port"
#define CFG_KEY_CONTROL_BIND_IP     "control.bind_ip"

#define CFG_KEY_DB_PATH             "db.path"

#define CFG_KEY_DEBUG_CONSOLE_LEVEL "debug.console_level"
#define CFG_KEY_DEBUG_FILE_LEVEL    "debug.file_level"
#define CFG_KEY_DEBUG_LOG_FILE      "debug.log_file"
#define CFG_KEY_DEBUG_CATEGORIES    "debug.categories"

/* key/def передаются соответствующему callback как есть; без callback возвращается def.
 * cfg_get_bool не нормализует число к 0/1; семантику задаёт провайдер. */

const char* cfg_get_string(const char* key, const char* def);
int64_t     cfg_get_int64(const char* key, int64_t def);
int         cfg_get_int(const char* key, int def);
int         cfg_get_bool(const char* key, int def);

#ifdef __cplusplus
}
#endif
#endif /* UTUN_CONFIG_API_H */
