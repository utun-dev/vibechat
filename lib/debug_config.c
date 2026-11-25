/**
 * Debug module
 */

#include "debug_config.h"
#include "platform_compat.h"
#include "mem.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <pthread.h>
#endif

void log_dump(int level, int category, const char* prefix, const uint8_t* data, size_t len) {
    if (!debug_should_output(level, category)) return;

    char hex_buf[513] = {'\0'};
    size_t hex_len = 0;
    size_t show_len = (len > 128) ? 128 : len;

    for (size_t i = 0; i < show_len && hex_len < 512 - 3; i++) {
        hex_len += snprintf(hex_buf + hex_len, sizeof(hex_buf) - hex_len, "%02x", data[i]);
        if (i < show_len - 1 && (i + 1) % 32 == 0)
            hex_len += snprintf(hex_buf + hex_len, sizeof(hex_buf) - hex_len, " ");
    }

    if (len > 128)
        hex_len += snprintf(hex_buf + hex_len, sizeof(hex_buf) - hex_len, "...");

    switch (level) {
    case DEBUG_LEVEL_ERROR: DEBUG_ERROR(category, "%s: len=%zu hex=%s", prefix, len, hex_buf); break;
    case DEBUG_LEVEL_WARN:  DEBUG_WARN(category, "%s: len=%zu hex=%s", prefix, len, hex_buf); break;
    case DEBUG_LEVEL_INFO:  DEBUG_INFO(category, "%s: len=%zu hex=%s", prefix, len, hex_buf); break;
    case DEBUG_LEVEL_DEBUG: DEBUG_DEBUG(category, "%s: len=%zu hex=%s", prefix, len, hex_buf); break;
    case DEBUG_LEVEL_TRACE: DEBUG_TRACE(category, "%s: len=%zu hex=%s", prefix, len, hex_buf); break;
    }
}


ip_str_t ip_to_str(const void *addr, int family) {
    ip_str_t result;
    result.str[0] = '\0';

    if (!addr) {
        return result;
    }

    if (family == AF_INET) {
        inet_ntop(AF_INET, addr, result.str, 16);
    } else if (family == AF_INET6) {
        inet_ntop(AF_INET6, addr, result.str, 48);
    }

    return result;
}

ip_str_t sockaddr_storage_to_str(const struct sockaddr_storage *addr) {
    ip_str_t result;
    result.str[0] = '\0';

    if (!addr) {
        return result;
    }

    char ip_buf[INET6_ADDRSTRLEN];
    ip_buf[0] = '\0';

    uint16_t port = 0;
    void *src = NULL;

    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)addr;
        src = (void *)&ipv4->sin_addr;
        port = ntohs(ipv4->sin_port);
    }
    else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)addr;
        src = (void *)&ipv6->sin6_addr;
        port = ntohs(ipv6->sin6_port);
    }
    else {
        return result;
    }

    if (!inet_ntop(addr->ss_family, src, ip_buf, sizeof(ip_buf))) {
        return result;
    }

    if (addr->ss_family == AF_INET6) {
        // IPv6 обязательно в []
        snprintf(result.str, sizeof(result.str), "[%s]:%u", ip_buf, port);
    } else {
        snprintf(result.str, sizeof(result.str), "%s:%u", ip_buf, port);
    }

    return result;
}



static const struct {
    const char* name;
    debug_category_t category;
} g_debug_categories[] = {
    {"none", DEBUG_CATEGORY_NONE},
    {"sys", DEBUG_CATEGORY_SYS},
    {"connection", DEBUG_CATEGORY_CONNECTION},
    {"etcp", DEBUG_CATEGORY_ETCP},
    {"crypto", DEBUG_CATEGORY_CRYPTO},
    {"config", DEBUG_CATEGORY_CONFIG},
    {"tun", DEBUG_CATEGORY_TUN},
    {"routing", DEBUG_CATEGORY_ROUTING},
    {"timers", DEBUG_CATEGORY_TIMERS},
    {"bgp", DEBUG_CATEGORY_BGP},
    {"chat", DEBUG_CATEGORY_CHAT},
    {"socket", DEBUG_CATEGORY_SOCKET},
    {"control", DEBUG_CATEGORY_CONTROL},
    {"dump", DEBUG_CATEGORY_DUMP},
    {"traffic", DEBUG_CATEGORY_TRAFFIC},
    {"debug", DEBUG_CATEGORY_DEBUG},
    {"general", DEBUG_CATEGORY_GENERAL},
    {"nat", DEBUG_CATEGORY_NAT},
    {"keepalive", DEBUG_CATEGORY_KEEPALIVE},
    {"media", DEBUG_CATEGORY_MEDIA},
    {"etcp_route", DEBUG_CATEGORY_ETCPROUTE},
    {"etcp_dump", DEBUG_CATEGORY_ETCP_DUMP},
    {"chat_sync", DEBUG_CATEGORY_CHAT_SYNC},
    {"member_sync", DEBUG_CATEGORY_MEMBER_SYNC},
    {"proxy", DEBUG_CATEGORY_PROXY},
    {"video", DEBUG_CATEGORY_VIDEO},
    {"reality", DEBUG_CATEGORY_REALITY},
    {"dm", DEBUG_CATEGORY_DM},
    {"call", DEBUG_CATEGORY_CALL},
    {"radio", DEBUG_CATEGORY_RADIO},
    {"vad", DEBUG_CATEGORY_VAD},
    {"aec", DEBUG_CATEGORY_AEC},
    {"stream", DEBUG_CATEGORY_STREAM},
    {"discovery", DEBUG_CATEGORY_DISCOVERY},
    {"group", DEBUG_CATEGORY_GROUP},
    {"kanban", DEBUG_CATEGORY_KANBAN},
    {"db_changes", DEBUG_CATEGORY_DB_CHANGES},
    {"all", DEBUG_CATEGORY_ALL},
    {NULL, DEBUG_CATEGORY_NONE}
};

static const struct {
    const char* name;
    debug_level_t level;
} g_level_table[] = {
    {"disabled", DEBUG_LEVEL_DISABLED},
    {"none",     DEBUG_LEVEL_NONE},
    {"error",    DEBUG_LEVEL_ERROR},
    {"warn",     DEBUG_LEVEL_WARN},
    {"info",     DEBUG_LEVEL_INFO},
    {"debug",    DEBUG_LEVEL_DEBUG},
    {"trace",    DEBUG_LEVEL_TRACE},
    {NULL,       DEBUG_LEVEL_NONE}
};

debug_category_t get_category_by_name(const char* name) {
    if (!name) return DEBUG_CATEGORY_NONE;
    for (int i = 0; g_debug_categories[i].name; i++) {
        if (strcasecmp(g_debug_categories[i].name, name) == 0) {
            return g_debug_categories[i].category;
        }
    }
    return DEBUG_CATEGORY_NONE;
}

debug_level_t debug_level_from_name(const char* name) {
    if (!name) return DEBUG_LEVEL_NONE;
    for (int i = 0; g_level_table[i].name; i++) {
        if (strcasecmp(g_level_table[i].name, name) == 0) {
            return g_level_table[i].level;
        }
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Invalid debug level '%s' (valid: disabled,none,error,warn,info,debug,trace)", name);
    return DEBUG_LEVEL_INFO;  /* fallback */
}

void debug_set_category_level_by_name(const char* category_name, const char* level_name) {
    if (!category_name || !level_name) return;
    debug_category_t cat = get_category_by_name(category_name);
    if (cat == DEBUG_CATEGORY_NONE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Unknown debug category '%s'", category_name);
        return;
    }
    debug_level_t lvl = debug_level_from_name(level_name);
    debug_set_category_level(cat, lvl);
}

int debug_apply_category_config(const char* category_name, const char* level_name) {
    if (!category_name || !level_name) return -1;
    debug_category_t cat = get_category_by_name(category_name);
    if (cat == DEBUG_CATEGORY_NONE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Unknown debug category '%s'", category_name);
        return -1;
    }
    debug_level_t lvl = debug_level_from_name(level_name);
    debug_set_category_level(cat, lvl);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Applied per-category debug: %s=%s", category_name, level_name);
    return 0;
}

void debug_apply_global_level(const char* level_name) {
    if (!level_name || !*level_name) return;
    debug_level_t level = debug_level_from_name(level_name);
    debug_set_level(level);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Applied global debug_level: %s", level_name);
}

/* Get category name for logging */
const char* debug_get_category_name(debug_category_t category_idx) {
    for (int i = 0; g_debug_categories[i].name; i++)
        if (g_debug_categories[i].category == category_idx) return g_debug_categories[i].name;
    return "UNKNOWN";
}

/* Global debug configuration */
debug_config_t g_debug_config = {.category_levels = {[DEBUG_CATEGORY_DB_CHANGES] = DEBUG_LEVEL_INFO}};
static debug_log_hook_fn g_log_hook = NULL;

/* Запись/смена FILE и ротация сериализованы: fclose не пересекается с чужим fwrite.
 * Файловый код logger-а не вызывает file_io/DEBUG_* под lock: они сами логируют ошибки. */
#ifdef _WIN32
static SRWLOCK log_mutex = SRWLOCK_INIT;
#define LOG_LOCK() AcquireSRWLockExclusive(&log_mutex)
#define LOG_UNLOCK() ReleaseSRWLockExclusive(&log_mutex)
#else
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOG_LOCK() pthread_mutex_lock(&log_mutex)
#define LOG_UNLOCK() pthread_mutex_unlock(&log_mutex)
#endif
#define LOG_PATH_MAX 4096
static uint64_t log_bytes, log_limit, log_rotate_at;
static unsigned log_files;

#ifdef _WIN32
static int log_wide_path(const char* path, wchar_t wide[LOG_PATH_MAX]) {
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, LOG_PATH_MAX)) return 0;
    errno = EINVAL;
    return -1;
}
#endif

static FILE* log_open(const char* path, int truncate, uint64_t* bytes) {
    if (!path || !*path || strlen(path) >= LOG_PATH_MAX - 4) { errno = ENAMETOOLONG; return NULL; }
#ifdef _WIN32
    wchar_t wide[LOG_PATH_MAX];
    if (log_wide_path(path, wide)) return NULL;
    FILE* file = _wfopen(wide, truncate ? L"wb" : L"ab");
    struct _stat64 info;
    int result = file ? _fstat64(_fileno(file), &info) : -1;
#else
    FILE* file = fopen(path, truncate ? "wb" : "ab");
    struct stat info;
    int result = file ? fstat(fileno(file), &info) : -1;
#endif
    if (!file) return NULL;
    if (result) { int error = errno; fclose(file); errno = error; return NULL; }
    *bytes = info.st_size > 0 ? (uint64_t)info.st_size : 0;
    return file;
}

static int log_remove(const char* path) {
#ifdef _WIN32
    wchar_t wide[LOG_PATH_MAX];
    if (log_wide_path(path, wide)) return -1;
    return _wremove(wide);
#else
    return remove(path);
#endif
}

static int log_rename(const char* from, const char* to) {
#ifdef _WIN32
    wchar_t source[LOG_PATH_MAX], destination[LOG_PATH_MAX];
    if (log_wide_path(from, source) || log_wide_path(to, destination)) return -1;
    return _wrename(source, destination);
#else
    return rename(from, to);
#endif
}

/* Ошибку logger-а пишем напрямую, без рекурсии, в stderr и доступный текущий лог. */
static void log_io_error(const char* phase, const char* path, int error) {
    char message[1024];
    snprintf(message, sizeof(message), "[ERROR] [sys] log %s failed: path=%s errno=%d (%s)\n", phase, path, error, strerror(error));
    fputs(message, stderr); fflush(stderr);
    if (g_debug_config.file_output) {
        log_bytes += fwrite(message, 1, strlen(message), g_debug_config.file_output);
        fflush(g_debug_config.file_output);
    }
}

/* Под lock, без проверок размера на диске. Пустые файлы не занимают поколения. */
static int log_rotate(const char* trigger) {
    if (!log_bytes) return 0;
    uint64_t previous_size = log_bytes;
    const char* path = g_debug_config.output_file;
    char from[LOG_PATH_MAX], to[LOG_PATH_MAX];
    const char* phase = "rotate close";
    int error = 0;
    FILE* old = g_debug_config.file_output;
    g_debug_config.file_output = NULL;
    if (fclose(old)) { error = errno; goto reopen; }
    if (log_files > 1) {
        snprintf(to, sizeof(to), "%s.%u", path, log_files - 1);
        phase = "rotate remove oldest";
        if (log_remove(to) && errno != ENOENT) { error = errno; goto reopen; }
        for (unsigned i = log_files - 1; i > 1; --i) {
            snprintf(from, sizeof(from), "%s.%u", path, i - 1);
            snprintf(to, sizeof(to), "%s.%u", path, i);
            phase = "rotate rename generation";
            if (log_rename(from, to) && errno != ENOENT) { error = errno; goto reopen; }
        }
        snprintf(to, sizeof(to), "%s.1", path);
        phase = "rotate rename current";
        if (log_rename(path, to)) { error = errno; goto reopen; }
    }
reopen:
    g_debug_config.file_output = log_open(path, !error && log_files == 1, &log_bytes);
    if (!g_debug_config.file_output) {
        log_io_error("rotate reopen", path, errno);
        g_debug_config.console_enabled = 1;
        return -1;
    }
    if (error) log_io_error(phase, path, error);
    else if (debug_should_output(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_SYS)) {
        char message[192];
        int size = snprintf(message, sizeof(message), "[INFO] [sys] Log rotated: trigger=%s previous_bytes=%llu files=%u\n",
                            trigger, (unsigned long long)previous_size, log_files);
        log_bytes += fwrite(message, 1, (size_t)size, g_debug_config.file_output);
        if (fflush(g_debug_config.file_output)) log_io_error("flush rotation notice", path, errno);
    }
    /* При ошибке не повторяем файловые операции на каждой строке: ещё limit байтов до попытки. */
    log_rotate_at = error ? log_bytes + log_limit : log_limit;
    return error ? -1 : 0;
}

int debug_set_file_rotation(uint64_t max_bytes, unsigned file_count) {
    if (max_bytes && (max_bytes < 4096 || max_bytes > UINT64_MAX / 2 || !file_count || file_count > 99)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Invalid log rotation: bytes=%llu files=%u", (unsigned long long)max_bytes, file_count);
        return -1;
    }
    LOG_LOCK();
    log_limit = max_bytes; log_files = file_count; log_rotate_at = max_bytes;
    LOG_UNLOCK();
    return 0;
}

int debug_rotate_log(void) {
    LOG_LOCK();
    if (!log_limit || !g_debug_config.file_output) {
        LOG_UNLOCK();
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Cannot rotate log: file output or rotation is disabled");
        return -1;
    }
    int result = log_rotate("startup/manual");
    LOG_UNLOCK();
    return result;
}

/* Initialize debug system with default settings */
void debug_config_init(void) {
    LOG_LOCK();
    log_bytes = log_limit = log_rotate_at = 0; log_files = 0;
    g_debug_config.level = DEBUG_LEVEL_ERROR;        // Global level: errors only by default
    for (int i = 0; i < DEBUG_CATEGORY_COUNT; i++) {
        g_debug_config.category_levels[i] = DEBUG_LEVEL_NONE;  // 0 = disabled per category
    }
    g_debug_config.category_levels[DEBUG_CATEGORY_DB_CHANGES] = DEBUG_LEVEL_INFO;
    g_debug_config.timestamp_enabled = 1;
    g_debug_config.function_name_enabled = 1;
    g_debug_config.file_line_enabled = 1;
    g_debug_config.output_file = NULL;
    
    // Dual output defaults
    g_debug_config.file_output = NULL;                  // No file by default
    g_debug_config.console_enabled = 1;                 // Console enabled
    g_debug_config.thread_marker = 0;                   // No thread marker by default
    LOG_UNLOCK();
}

/* Set debug level */
void debug_set_level(debug_level_t level) {
    g_debug_config.level = level;
}

/* Set debug level for specific category (0 = disabled, otherwise uses that level) */
void debug_set_category_level(debug_category_t category_idx, debug_level_t level) {
    if ((unsigned)category_idx >= DEBUG_CATEGORY_COUNT) {
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "Invalid debug category index: %d", category_idx);
        return;
    }
    if (category_idx == DEBUG_CATEGORY_NONE) return;
    g_debug_config.category_levels[category_idx] = level;
}

/* Enable/disable specific categories (now sets level) */
void debug_enable_category(debug_category_t category_idx) {
    if (category_idx >= 0 && category_idx < DEBUG_CATEGORY_COUNT) {
        g_debug_config.category_levels[category_idx] = g_debug_config.level;
    }
}

void debug_disable_category(debug_category_t category_idx) {
    if (category_idx >= 0 && category_idx < DEBUG_CATEGORY_COUNT) {
        g_debug_config.category_levels[category_idx] = DEBUG_LEVEL_NONE;
    }
}

void debug_set_categories(debug_category_t category_idx) {
    /* Deprecated - use per-category levels */
    if (category_idx >= 0 && category_idx < DEBUG_CATEGORY_COUNT) {
        g_debug_config.category_levels[category_idx] = g_debug_config.level;
    }
}

/* Set level for category (main API) */
void debug_set_masks(debug_category_t category_idx, debug_level_t level) {
    debug_set_category_level(category_idx, level);
}

void debug_enable_timestamp(int enable) {
    g_debug_config.timestamp_enabled = !!enable;
}

void debug_enable_function_name(int enable) {
    g_debug_config.function_name_enabled = enable;
}

void debug_enable_file_line(int enable) {
    g_debug_config.file_line_enabled = enable;
}

void debug_set_output_file(const char* file_path) {
    if (!file_path) return;
    LOG_LOCK();
    if (g_debug_config.file_output) { LOG_UNLOCK(); return; }
    FILE* new_file = log_open(file_path, 0, &log_bytes);
    if (new_file) {
        g_debug_config.file_output = new_file;
        g_debug_config.output_file = file_path;
        log_rotate_at = log_limit;
    }
    int error = errno;
    LOG_UNLOCK();
    if (!new_file) DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Cannot open log '%s': %s", file_path, strerror(error));
}

/* Set console log level */
void debug_enable_console(int enable) {
    g_debug_config.console_enabled = enable;
}

/* Enable file output with optional truncation */
void debug_enable_file_output(const char* file_path, int truncate) {
    if (!file_path) return;
    LOG_LOCK();
    uint64_t bytes;
    FILE* new_file = log_open(file_path, truncate, &bytes);
    int error = errno;
    if (new_file) {
        FILE* old = g_debug_config.file_output;
        g_debug_config.file_output = new_file;
        g_debug_config.output_file = file_path;
        g_debug_config.console_enabled = 0;
        log_bytes = bytes; log_rotate_at = log_limit;
        if (old && fclose(old)) log_io_error("close previous", file_path, errno);
    }
    LOG_UNLOCK();
    if (!new_file) DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Cannot open log '%s': %s", file_path, strerror(error));
}

/* Disable file output */
void debug_disable_file_output(void) {
    LOG_LOCK();
    if (g_debug_config.file_output) {
        FILE* old = g_debug_config.file_output;
        g_debug_config.file_output = NULL;
        if (fclose(old)) log_io_error("close", g_debug_config.output_file, errno);
    }
    g_debug_config.output_file = NULL; log_bytes = 0;
    LOG_UNLOCK();
}

/* Переоткрыть лог в append-режиме; при ошибке открытия сохранить прежний поток. */
int debug_reopen_log(void) {
    LOG_LOCK();
    if (!g_debug_config.output_file) {
        LOG_UNLOCK();
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "No log path for reopen");
        return -1;
    }
    const char* path = g_debug_config.output_file;
    uint64_t bytes;
    FILE* new_file = log_open(path, 0, &bytes);
    if (!new_file) {
        int error = errno;
        LOG_UNLOCK();
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Cannot reopen log '%s': %s", path, strerror(error));
        return -1;
    }
    FILE* old_file = g_debug_config.file_output;
    g_debug_config.file_output = new_file;
    log_bytes = bytes; log_rotate_at = log_limit;
    if (old_file && fclose(old_file) != 0) {
        int error = errno;
        LOG_UNLOCK();
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Cannot close previous log '%s': %s", path, strerror(error));
        return -1;
    }
    LOG_UNLOCK();
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Log reopened: %s", path);
    return 0;
}

/* Get current debug level */
debug_level_t debug_get_level(void) {
    return g_debug_config.level;
}

/* Get level name */
static const char* get_level_name(debug_level_t level) {
    switch (level) {
        case DEBUG_LEVEL_ERROR: return "ERROR";
        case DEBUG_LEVEL_WARN:  return "WARN";
        case DEBUG_LEVEL_INFO:  return "INFO";
        case DEBUG_LEVEL_DEBUG: return "DEBUG";
        case DEBUG_LEVEL_TRACE: return "TRACE";
        default: return "UNKNOWN";
    }
}

/* Format and output debug message */
void debug_output(debug_level_t level, debug_category_t category_idx, 
                 const char* function, const char* file, int line,
                 const char* format, ...) {
    
#define BUFFER_SIZE 4096
    char buffer[BUFFER_SIZE];
    int offset = 0;
    size_t remaining = BUFFER_SIZE;
    
#define BUF_ADVANCE(n) do { offset += (int)(n); if (offset > (int)BUFFER_SIZE - 1) offset = (int)BUFFER_SIZE - 1; remaining = BUFFER_SIZE - (size_t)offset; } while(0)
    
    va_list args;
    va_start(args, format);
    
    /* Add timestamp with microseconds: hh:mm:ss-xxx.yyy */
    if (g_debug_config.timestamp_enabled) {
        struct timeval tv;
        utun_gettimeofday(&tv, NULL);
        time_t tv_sec = (time_t)tv.tv_sec;
        struct tm tm_info;
#ifdef _WIN32
        localtime_s(&tm_info, &tv_sec);
#else
        localtime_r(&tv_sec, &tm_info);
#endif
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_info);
        BUF_ADVANCE(snprintf(buffer + offset, remaining, "[%s-%03ld.%03ld] ", time_str, tv.tv_usec / 1000, tv.tv_usec % 1000));
    }
    
    /* Add thread marker if set */
    if (g_debug_config.thread_marker > 0) {
        BUF_ADVANCE(snprintf(buffer + offset, remaining, "[%d] ", g_debug_config.thread_marker));
    }
    
    /* Add level */
    const char* level_name = get_level_name(level);
    BUF_ADVANCE(snprintf(buffer + offset, remaining, "[%s] ", level_name));
    
    /* Add category name */
    const char* cat_name = debug_get_category_name(category_idx);
    BUF_ADVANCE(snprintf(buffer + offset, remaining, "[%s] ", cat_name));
    
    /* Add file:line if enabled */
    if (g_debug_config.file_line_enabled && file) {
        const char* bn = strrchr(file, '/');
        bn = bn ? bn + 1 : file;
        BUF_ADVANCE(snprintf(buffer + offset, remaining, "(%s:%d) ", bn, line));
    }

    /* Add function name if enabled */
    if (g_debug_config.function_name_enabled && function) {
        BUF_ADVANCE(snprintf(buffer + offset, remaining, "%s() ", function));
    }
    
    /* Add the actual message */
    BUF_ADVANCE(vsnprintf(buffer + offset, remaining, format, args));
    
    /* Add newline */
    if (remaining > 1) {
        buffer[offset] = '\n';
        buffer[offset + 1] = '\0';
    } else {
        buffer[BUFFER_SIZE - 2] = '\n';
        buffer[BUFFER_SIZE - 1] = '\0';
    }
#undef BUF_ADVANCE
    
    va_end(args);
    
    /* Route to external hook (JNI bridge, etc.) */
    if (g_log_hook) {
        const char* cat_name = debug_get_category_name(category_idx);
        g_log_hook((int)level, cat_name, buffer);
    }
    
    /* Output to console if enabled and level is sufficient */
    LOG_LOCK();
    if (g_debug_config.file_output && log_limit && log_bytes >= log_rotate_at) log_rotate("size");
    if (g_debug_config.file_output) {
        size_t size = strlen(buffer);
        size_t written = fwrite(buffer, 1, size, g_debug_config.file_output);
        log_bytes += written;
        if (fflush(g_debug_config.file_output) || written != size) {
            /* Не пытаемся снова писать в неисправный FILE и не теряем диагностику следующей строки. */
            int error = errno;
            FILE* broken = g_debug_config.file_output; g_debug_config.file_output = NULL;
            log_io_error("write", g_debug_config.output_file, error);
            if (fclose(broken)) log_io_error("close after write error", g_debug_config.output_file, errno);
            g_debug_config.console_enabled = 1;
            fputs(buffer, stderr); fflush(stderr);
        }
    } else if (g_debug_config.console_enabled) {
        fprintf(stdout, "%s", buffer);
        fflush(stdout);
    }
    LOG_UNLOCK();
#undef BUFFER_SIZE
}

void debug_set_log_hook(debug_log_hook_fn hook) {
    g_log_hook = hook;
}

/* Настройка отдельных категорий: "sys=debug,etcp=error"; разделитель ':' также допустим. */
int debug_parse_config(const char* config_string) {
    if (!config_string || !*config_string) {
        return 0;
    }

    char* str = u_strdup(config_string);
    if (!str) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Failed to allocate debug configuration copy");
        return -1;
    }

    char* token = strtok(str, ",");
    while (token) {
        /* Trim leading spaces */
        while (isspace(*token)) {
            token++;
        }

        char* separator = strpbrk(token, "=:");
        if (!separator) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Missing '=' or ':' in debug setting '%s'", token);
            u_free(str);
            return -1;
        }

        *separator = '\0';
        char* category_name = token;
        char* level_name = separator + 1;

        /* Trim leading spaces in level_name */
        while (isspace(*level_name)) {
            level_name++;
        }

        debug_category_t cat = get_category_by_name(category_name);
        if (cat <= DEBUG_CATEGORY_NONE || cat >= DEBUG_CATEGORY_COUNT) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Unknown or unsupported debug category '%s' (expected an individual category)", category_name);
            u_free(str);
            return -1;
        }

        debug_level_t lev = debug_level_from_name(level_name);

        debug_set_category_level(cat, lev);
        DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "Applied debug setting: %s=%s (category=%d, level=%d)", category_name, level_name, cat, lev);

        token = strtok(NULL, ",");
    }

    u_free(str);
    return 0;
}
