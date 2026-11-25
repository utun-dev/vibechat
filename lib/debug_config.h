/**
 * Управление диагностикой: debug_config_init → уровни категорий и настройки вывода → DEBUG_*.
 * NONE наследует общий уровень, DISABLED отключает категорию. Имена настроек — category=level.
 * Открытый файл имеет приоритет над консолью; hook получает сообщения независимо от выбора выхода.
 * Уровни: disabled, none, error, warn, info, debug, trace; отдельный уровень категории заменяет общий, не max().
 * Пример: debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); debug_parse_config("etcp=trace,config=info").
 * DEBUG_* фильтруют до форматирования; прямой debug_output фильтр не проверяет.
 * Строка с префиксами ограничена буфером 4096 байт и обрезается; каждый вывод fflush.
 * Запись/смена файла и ротация синхронизированы; уровни/настройки форматирования согласует владелец.
 */

#ifndef DEBUG_CONFIG_H
#define DEBUG_CONFIG_H

#include "platform_compat.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>   // struct sockaddr_storage
#endif
#include <stddef.h>       // size_t
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <stdio.h>        // FILE

#ifdef __cplusplus
extern "C" {
#endif

/* Debug levels */
typedef enum {
    DEBUG_LEVEL_DISABLED = -1, // Category completely disabled, never outputs
    DEBUG_LEVEL_NONE     = 0,  // Fallback to global level
    DEBUG_LEVEL_ERROR    = 1,  // Errors only
    DEBUG_LEVEL_WARN     = 2,  // Warnings and errors
    DEBUG_LEVEL_INFO     = 3,  // Info, warnings, errors
    DEBUG_LEVEL_DEBUG    = 4,  // Full debug output
    DEBUG_LEVEL_TRACE    = 5   // Trace everything
} debug_level_t;

/* Debug categories as indices (0-based) */
typedef int debug_category_t;

#define DEBUG_CATEGORY_NONE        0
#define DEBUG_CATEGORY_SYS         1  // System: u_async, ll_queue, memory, timing
// #define DEBUG_CATEGORY_LL_QUEUE  2  (merged into SYS)
#define DEBUG_CATEGORY_CONNECTION  3  // Connection: connect/reconnect/disconnect events only
#define DEBUG_CATEGORY_ETCP        4  // ETCP: protocol, BBR, normalizer
#define DEBUG_CATEGORY_CRYPTO      5  // crypto operations
// #define DEBUG_CATEGORY_MEMORY    6  (merged into SYS)
// #define DEBUG_CATEGORY_TIMING    7  (merged into SYS)
#define DEBUG_CATEGORY_CONFIG      8  // configuration parsing
#define DEBUG_CATEGORY_TUN         9  // TUN interface
#define DEBUG_CATEGORY_ROUTING     10  // routing table
#define DEBUG_CATEGORY_TIMERS      11  // timer management
// #define DEBUG_CATEGORY_NORMALIZER 12 (merged into ETCP)
#define DEBUG_CATEGORY_BGP         13  // BGP route exchange
#define DEBUG_CATEGORY_SOCKET      14  // Socket operations
#define DEBUG_CATEGORY_CONTROL     15  // Control/monitoring server
#define DEBUG_CATEGORY_DUMP        16  // Packet dump/logging
#define DEBUG_CATEGORY_TRAFFIC     17  // Traffic flow (src/dst node IDs)
#define DEBUG_CATEGORY_DEBUG       18  // Current debugging
#define DEBUG_CATEGORY_GENERAL     19  // Messages for user (common log)
#define DEBUG_CATEGORY_NAT         20  // EIM NAT module
#define DEBUG_CATEGORY_KEEPALIVE   21  // Keepalive periodic monitoring only
#define DEBUG_CATEGORY_ETCPROUTE   22  // ETCP routing
#define DEBUG_CATEGORY_MEDIA       23  // Media delivery + media async
#define DEBUG_CATEGORY_ETCP_DUMP   24  // ETCP packet dump
#define DEBUG_CATEGORY_CHAT        25  // Chat modules (chat_core, whisper, etc.)
#define DEBUG_CATEGORY_CHAT_SYNC    26  // Message DB sync (INIT_SYNC, PUSH, chain hash, recalc)
#define DEBUG_CATEGORY_MEMBER_SYNC 27  // Member + address + merkle sync
#define DEBUG_CATEGORY_PROXY      28  // Proxy modules (SOCKS5, TCP, UDP, ICMP)
#define DEBUG_CATEGORY_VIDEO      29  // Video module (probe/transcode)
#define DEBUG_CATEGORY_REALITY    30  // REALITY TLS camouflage
#define DEBUG_CATEGORY_DM         31  // DM — direct p2p chat between two users
#define DEBUG_CATEGORY_CALL       32  // Call — P2P audio call (signaling + media)
#define DEBUG_CATEGORY_RADIO      33  // Radio — PTT walkie-talkie multicast (voice)
#define DEBUG_CATEGORY_VAD        34  // VAD — Silero voice activity detector (ONNX Runtime)
#define DEBUG_CATEGORY_AEC        35  // AEC — acoustic echo cancellation (SpeexDSP)
#define DEBUG_CATEGORY_STREAM     36  // Подписанные потоки, FEC, буферы и выбор поставщиков
#define DEBUG_CATEGORY_DISCOVERY   37  // LAN UDP: discovery and delivery
#define DEBUG_CATEGORY_GROUP      38  // Shared member groups
#define DEBUG_CATEGORY_KANBAN     39  // Kanban operations and merge
#define DEBUG_CATEGORY_DB_CHANGES 40  // Сохранённые изменения и нарушения целостности
#define DEBUG_CATEGORY_COUNT      41  // Total number of categories
#define DEBUG_CATEGORY_ALL         (-1) // special value for all categories

/* Debug configuration structure */
typedef struct {
    debug_level_t level;                   // Global debug level (default: ERROR)
    debug_level_t category_levels[DEBUG_CATEGORY_COUNT];  // Уровень категории: NONE наследует общий, DISABLED выключает
    int timestamp_enabled;                 // Include timestamps in output
    int function_name_enabled;             // Include function names
    int file_line_enabled;                 // Include file:line info
    const char* output_file;               // Заимствованный путь для reopen; строка должна жить, пока поле её хранит
    
    // Выход: файл, иначе консоль при console_enabled != 0
    FILE* file_output;                     // File handle for log file (NULL if not open)
    int console_enabled;                   // Enable console output
    int thread_marker;                     // Thread identification (0=none, e.g. 2 for uasync)
} debug_config_t;

/* Global debug configuration */
extern debug_config_t g_debug_config;

/* До использования: ERROR, категории NONE (db_changes=INFO), консоль stdout, timestamp/function/file:line включены.
 * Повторный init сбрасывает поля, но не закрывает уже открытый файл; перед ним закрыть файл отдельно. */
void debug_config_init(void);

/* Set debug level */
// Общий уровень применяется к категориям с уровнем NONE.
void debug_set_level(debug_level_t level);

/* Уровень категории заменяет общий; NONE наследует его, DISABLED подавляет вывод. */
void debug_set_category_level(debug_category_t category, debug_level_t level);

/* enable фиксирует текущий общий уровень; disable возвращает к наследованию (NONE).
 * Для полного выключения использовать debug_set_category_level(cat, DEBUG_LEVEL_DISABLED). */
void debug_enable_category(debug_category_t category);
void debug_disable_category(debug_category_t category);
void debug_set_categories(debug_category_t categories);

/* Установка уровня одной категории (аргумент categories — индекс, не битовая маска). */
void debug_set_masks(debug_category_t categories, debug_level_t level);

/* Configure output options */
/* Ненулевое enable включает временную метку, 0 отключает. */
void debug_enable_timestamp(int enable);
void debug_enable_function_name(int enable);
void debug_enable_file_line(int enable);
/* Открыть ненулевой file_path в append-режиме, только если файл ещё не открыт; путь заимствован. */
void debug_set_output_file(const char* file_path);

/* Заменить файл: truncate != 0 → обрезать, иначе append. NULL ничего не меняет.
 * Путь UTF-8 заимствован; успешное открытие выключает console_enabled; при отказе прежний FILE сохраняется. */
void debug_enable_file_output(const char* file_path, int truncate);
/* Ротация выключена после init. max_bytes=0 выключает; иначе >=4096, file_count=1..99
 * включая текущий файл. После примерно max_bytes: path → path.1 → ...; последний удаляется.
 * Только rename/remove, без сжатия. Строки не разрываются: превышение до одной строки.
 * Размер читается только при открытии; далее счётчик записанных байтов. Один процесс владеет путём.
 * 0/-1; повторная настройка и reopen сами по себе не ротируют. */
int debug_set_file_rotation(uint64_t max_bytes, unsigned file_count);
/* Начать новый лог немедленно (например, при запуске приложения); 0/-1.
 * Нужны открытый файл и включённая ротация; пустой файл не создаёт пустое поколение.
 * Ошибка ротации логируется; по возможности продолжается append, повтор через max_bytes. */
int debug_rotate_log(void);

/* Закрыть файл; прежнее значение console_enabled не восстанавливается. */
void debug_disable_file_output(void);
/* Переоткрыть текущий путь в append-режиме: 0 — успех, -1 — ошибка с диагностикой.
 * При ошибке открытия сохраняет прежний поток; console_enabled не меняет. */
int debug_reopen_log(void);
/* Вывод в консоль разрешён только при отсутствии открытого файла. */
void debug_enable_console(int enable);


// IP/sockaddr в строку: результат возвращается по значению, освобождать не требуется.
typedef struct {
    char str[54];  // INET6_ADDRSTRLEN(45) + 6chars (:port) + \0
} ip_str_t;

ip_str_t ip_to_str(const void *addr, int family);// big endian addr
ip_str_t sockaddr_storage_to_str(const struct sockaddr_storage *addr);// автоопределение v4 или v6

// Hex dump: первые min(len, 128) байт; при len > 128 добавляет "...". Применяется фильтр уровня/категории.
void log_dump(int level, int category, const char* prefix, const uint8_t* data, size_t len);

/* Проверка до вычисления аргументов DEBUG_*: inline убирает вызов функции на каждое сообщение.
 * category_idx — индекс категории; неверный индекс даёт 0. NONE наследует общий уровень,
 * DISABLED подавляет категорию независимо от общего уровня. Настройки читаются без кэша. */
static inline int debug_should_output(debug_level_t level, debug_category_t category_idx) {
    if ((unsigned)category_idx >= DEBUG_CATEGORY_COUNT) return 0;
    debug_level_t cat_level = g_debug_config.category_levels[category_idx];
    if (cat_level == DEBUG_LEVEL_DISABLED) return 0;
    if (cat_level == DEBUG_LEVEL_NONE) cat_level = g_debug_config.level;
    return level <= cat_level;
}

/* Get current debug level */
debug_level_t debug_get_level(void);

/* Заимствованное имя категории; для неизвестного индекса возвращает "UNKNOWN". */
const char* debug_get_category_name(debug_category_t category_idx);

/* Поиск без учёта регистра: индекс, ALL для "all", NONE при NULL/неизвестном имени. */
debug_category_t get_category_by_name(const char* name);

/* Поиск без учёта регистра: NULL → NONE; неизвестное имя логируется и даёт INFO (fallback). */
debug_level_t debug_level_from_name(const char* name);

/* Set category level using text names (main API) */
void debug_set_category_level_by_name(const char* category_name, const char* level_name);

/* Apply full category config with error reporting */
int debug_apply_category_config(const char* category_name, const char* level_name);

/* Apply global debug level from text name */
void debug_apply_global_level(const char* level_name);

/* Форматирует сообщение printf-формата с необязательными метками; фильтрацию выполняет вызывающий.
 * Время календарное, локальное, hh:mm:ss-миллисекунды.микросекунды; положительный thread_marker выводится отдельно. */
void debug_output(debug_level_t level, debug_category_t category_idx, 
                 const char* function, const char* file, int line,
                 const char* format, ...);

/* Hook вызывается синхронно для форматированного сообщения до записи в выход; NULL снимает hook.
 * category/message заимствованы на время вызова, message указывает на стековый буфер; для хранения скопировать. */
typedef void (*debug_log_hook_fn)(int level, const char* category, const char* message);
void debug_set_log_hook(debug_log_hook_fn hook);

/* Настройка отдельных категорий: "etcp=trace,config=info" (также ':' вместо '=').
 * NULL/пустая строка ничего не меняет. Возвращает 0 при успехе, -1 при ошибке;
 * настройки до ошибочного элемента остаются применёнными. */
int debug_parse_config(const char* config_string);

/* Convenience macros for debug output */
#if defined(__GNUC__) || defined(__clang__)
#define DEBUG_UNLIKELY(condition) __builtin_expect(!!(condition), 0)
#else
#define DEBUG_UNLIKELY(condition) (condition)
#endif

#define DEBUG_ERROR(category, fmt, ...) \
    do { if (DEBUG_UNLIKELY(debug_should_output(DEBUG_LEVEL_ERROR, category))) { \
        debug_output(DEBUG_LEVEL_ERROR, category, __FUNCTION__, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } } while(0)

#define DEBUG_WARN(category, fmt, ...) \
    do { if (DEBUG_UNLIKELY(debug_should_output(DEBUG_LEVEL_WARN, category))) { \
        debug_output(DEBUG_LEVEL_WARN, category, __FUNCTION__, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } } while(0)

#define DEBUG_INFO(category, fmt, ...) \
    do { if (DEBUG_UNLIKELY(debug_should_output(DEBUG_LEVEL_INFO, category))) { \
        debug_output(DEBUG_LEVEL_INFO, category, __FUNCTION__, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } } while(0)

#define DEBUG_DEBUG(category, fmt, ...) \
    do { if (DEBUG_UNLIKELY(debug_should_output(DEBUG_LEVEL_DEBUG, category))) { \
        debug_output(DEBUG_LEVEL_DEBUG, category, __FUNCTION__, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } } while(0)

#define DEBUG_TRACE(category, fmt, ...) \
    do { if (DEBUG_UNLIKELY(debug_should_output(DEBUG_LEVEL_TRACE, category))) { \
        debug_output(DEBUG_LEVEL_TRACE, category, __FUNCTION__, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } } while(0)

/* Сообщение без явной категории: ERROR в категории GENERAL, с обычной фильтрацией. */
#define DEBUG_OUTPUT(fmt, ...) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

/* Для замеров отдельной сборки: убрать логирование вместе с аргументами и фильтрами.
 * Прямой debug_output остаётся доступен стенду для ошибок и итогового отчёта. */
#ifdef UTUN_DISABLE_LOGS
#undef DEBUG_ERROR
#undef DEBUG_WARN
#undef DEBUG_INFO
#undef DEBUG_DEBUG
#undef DEBUG_TRACE
#define DEBUG_ERROR(...) ((void)0)
#define DEBUG_WARN(...) ((void)0)
#define DEBUG_INFO(...) ((void)0)
#define DEBUG_DEBUG(...) ((void)0)
#define DEBUG_TRACE(...) ((void)0)
#define debug_should_output(...) 0
#endif

#endif // DEBUG_CONFIG_H
