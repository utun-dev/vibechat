/**
 * Тест загрузки debug настроек из конфигурационного файла
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <sys/stat.h>
#include "../lib/debug_config.h"
#include "../lib/socket_compat.h"
#include "../src/config_parser.h"

// Temp config file path
static char temp_dir[] = "/tmp/utun_test_XXXXXX";
static char config_path[256];

// Config content
static const char* config_content =
    "[global]\n"

    "my_private_key=38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68\n"
    "my_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
    "tun_ip=10.99.0.1/24\n"
    "tun_ifname=tun99\n"
    "# Debug and logging configuration\n"
    "log_file=/tmp/utun_debug.log\n"
    "debug_level=debug\n"
    "enable_timestamp=1\n"
    "enable_function_names=1\n"
    "enable_file_lines=1\n"
    "enable_colors=1\n"
    "\n"
    "[server: test]\n"
    "addr=127.0.0.1:9001\n"
    "type=public\n";

// Create temp config file
static int create_temp_config(void) {
    if (test_mkdtemp(temp_dir) != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return -1;
    }

    snprintf(config_path, sizeof(config_path), "%s/test.conf", temp_dir);

    FILE* f = fopen(config_path, "w");
    if (!f) {
        fprintf(stderr, "Failed to create config file\n");
        return -1;
    }
    fprintf(f, "%s", config_content);
    fclose(f);

    return 0;
}

// Cleanup temp config file
static void cleanup_temp_config(void) {
    if (config_path[0]) test_unlink(config_path);
    if (temp_dir[0]) test_rmdir(temp_dir);
}

// Все категории, включая последние в списке, сохраняются при чтении файла и текста.
static void test_all_debug_categories(void) {
    char text[4096];
    int length = snprintf(text, sizeof(text), "%s\n[debug]\n", config_content);
    int count = 0;
    for (int category = 1; category < DEBUG_CATEGORY_COUNT; category++) {
        const char* name = debug_get_category_name(category);
        if (strcmp(name, "UNKNOWN") == 0) continue;
        int written = snprintf(text + length, sizeof(text) - length, "%s=%s\n", name, category == DEBUG_CATEGORY_SYS ? "disabled" : "debug");
        assert(written > 0 && written < (int)sizeof(text) - length);
        length += written; count++;
    }
    assert(count > 16);
    FILE* file = fopen(config_path, "w");
    assert(file);
    assert(fwrite(text, 1, length, file) == (size_t)length);
    assert(fclose(file) == 0);
    struct utun_config* configs[2] = {parse_config(config_path), parse_config_from_buf(text, length, "debug-categories")};
    for (int parser = 0; parser < 2; parser++) {
        struct utun_config* config = configs[parser];
        assert(config && config->global.debug_levels.count == count);
        assert(strcmp(config->global.debug_level, "debug") == 0);
        int index = 0;
        for (int category = 1; category < DEBUG_CATEGORY_COUNT; category++) {
            const char* name = debug_get_category_name(category);
            if (strcmp(name, "UNKNOWN") == 0) continue;
            assert(strcmp(config->global.debug_levels.category[index], name) == 0);
            debug_config_init();
            assert(debug_apply_category_config(name, config->global.debug_levels.level[index++]) == 0);
            assert(debug_should_output(DEBUG_LEVEL_DEBUG, category) == (category != DEBUG_CATEGORY_SYS));
        }
        free_config(config);
    }
    printf("All %d debug categories preserved by file and buffer parsers\n", count);
}

static int captured_count;
static char captured_category[32];
static char captured_message[4096];

/* Снимок фактически выведенного сообщения, включая форматирование. */
static void capture_debug_message(int level, const char* category, const char* message) {
    assert(level >= DEBUG_LEVEL_ERROR && level <= DEBUG_LEVEL_TRACE);
    captured_count++;
    snprintf(captured_category, sizeof(captured_category), "%s", category);
    snprintf(captured_message, sizeof(captured_message), "%s", message);
}

/* Отключённая категория не вычисляет аргументы; изменения уровней видны без сброса/кэша. */
static void test_debug_disabled_category(void) {
    debug_config_init();
    debug_enable_console(0);
    debug_set_log_hook(capture_debug_message);
    debug_set_level(DEBUG_LEVEL_TRACE);
    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DISABLED);
    captured_count = 0;
    int evaluated = 0;
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "disabled-error %d", ++evaluated);
    DEBUG_WARN(DEBUG_CATEGORY_ETCP, "disabled-warn %d", ++evaluated);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "disabled-info %d", ++evaluated);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "disabled-debug %d", ++evaluated);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "disabled-trace %d", ++evaluated);
    assert(evaluated == 0 && captured_count == 0);
    assert(!debug_should_output(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_ALL));
    assert(!debug_should_output(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_COUNT));

    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_ERROR);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "suppressed-debug %d", ++evaluated);
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "enabled-error %d", ++evaluated);
    assert(evaluated == 1 && captured_count == 1);

    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_NONE);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "inherited-trace %d", ++evaluated);
    assert(evaluated == 2 && captured_count == 2);
    g_debug_config.level = DEBUG_LEVEL_ERROR;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "changed-global %d", ++evaluated);
    assert(evaluated == 2 && captured_count == 2);
    g_debug_config.category_levels[DEBUG_CATEGORY_ETCP] = DEBUG_LEVEL_DISABLED;
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "changed-category %d", ++evaluated);
    assert(evaluated == 2 && captured_count == 2);

    debug_set_level(DEBUG_LEVEL_DISABLED);
    debug_set_category_level(DEBUG_CATEGORY_ALL, DEBUG_LEVEL_TRACE);
    debug_set_category_level(DEBUG_CATEGORY_COUNT, DEBUG_LEVEL_TRACE);
    assert(debug_get_level() == DEBUG_LEVEL_DISABLED);
    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_TRACE);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "explicit-trace %d", ++evaluated);
    assert(evaluated == 3 && captured_count == 3);
    debug_set_log_hook(NULL);
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Disabled categories, lazy arguments and live level changes: PASS");
}

/* Проверить реальный вывод через макрос, переключатель времени и ограничение дампа. */
static void test_debug_output_contract(void) {
    debug_config_init();
    debug_enable_console(0);
    debug_enable_timestamp(0);
    debug_enable_function_name(0);
    debug_enable_file_line(0);
    debug_set_log_hook(capture_debug_message);
    captured_count = 0;
    DEBUG_OUTPUT("plain-marker");
    assert(captured_count == 1);
    assert(strcmp(captured_category, "general") == 0);
    assert(strcmp(captured_message, "[ERROR] [general] plain-marker\n") == 0);

    debug_enable_timestamp(1);
    DEBUG_OUTPUT("timed-marker");
    assert(captured_count == 2);
    assert(strncmp(captured_message, "[ERROR]", 7) != 0);
    assert(strstr(captured_message, "[ERROR] [general] timed-marker\n"));
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DISABLED);
    DEBUG_OUTPUT("suppressed-marker");
    assert(captured_count == 2);

    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_ERROR);
    uint8_t bytes[129];
    memset(bytes, 0x5a, sizeof(bytes));
    bytes[128] = 0xff;
    log_dump(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_GENERAL, "dump-marker", bytes, sizeof(bytes));
    assert(captured_count == 3);
    const char* hex = strstr(captured_message, "len=129 hex=");
    assert(hex);
    hex += strlen("len=129 hex=");
    assert(strlen(hex) == 128 * 2 + 3 + 3 + 1); // hex, три пробела, многоточие, newline
    assert(!strstr(hex, "ff"));
    assert(strstr(hex, "...\n"));
    debug_set_log_hook(NULL);
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_enable_console(1);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Debug output, timestamp, filter and hex limit: PASS");
}

/* Неудачное переоткрытие оставляет рабочий поток; успешное сохраняет append и выбор консоли. */
static void test_debug_log_reopen(void) {
    char log_path[512], missing_path[512];
    snprintf(log_path, sizeof(log_path), "%s/runtime.log", temp_dir);
    snprintf(missing_path, sizeof(missing_path), "%s/missing/runtime.log", temp_dir);
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_enable_timestamp(0);
    debug_set_log_hook(capture_debug_message);
    debug_enable_file_output(log_path, 1);
    assert(g_debug_config.file_output && g_debug_config.output_file == log_path);
    assert(g_debug_config.console_enabled == 0);
    FILE* original = g_debug_config.file_output;
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "before-reopen-marker");

    g_debug_config.output_file = missing_path;
    captured_count = 0;
    assert(debug_reopen_log() == -1);
    assert(g_debug_config.file_output == original);
    assert(captured_count == 1 && strstr(captured_message, "Cannot reopen log"));
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "still-writable-marker");

    g_debug_config.output_file = log_path;
    debug_enable_console(1);
    assert(debug_reopen_log() == 0);
    assert(g_debug_config.file_output);
    assert(g_debug_config.console_enabled == 1);
    assert(strstr(captured_message, "Log reopened:"));
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "after-reopen-marker");
    debug_enable_console(0);
    debug_disable_file_output();
    assert(!g_debug_config.file_output && !g_debug_config.output_file);
    assert(g_debug_config.console_enabled == 0);
    assert(debug_reopen_log() == -1);
    debug_set_log_hook(NULL);

    FILE* file = fopen(log_path, "r");
    assert(file);
    char text[8192];
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    assert(!ferror(file) && fclose(file) == 0);
    text[length] = '\0';
    assert(strstr(text, "before-reopen-marker"));
    assert(strstr(text, "Cannot reopen log"));
    assert(strstr(text, "still-writable-marker"));
    assert(strstr(text, "after-reopen-marker"));
    assert(test_unlink(log_path) == 0);
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Log reopen success/failure and append preservation: PASS");
}

/* Значение по умолчанию, все режимы и строгая проверка протоколов автосокетов. */
static void test_auto_socket_protocols(void) {
    static const struct { const char* value; int mask; } cases[] = {
        {"", CFG_AUTO_SOCKET_BOTH}, {"tcp", CFG_AUTO_SOCKET_TCP}, {"udp", CFG_AUTO_SOCKET_UDP},
        {"tcp+udp", CFG_AUTO_SOCKET_BOTH}, {"TCP+UDP", CFG_AUTO_SOCKET_BOTH}, {"TCP", CFG_AUTO_SOCKET_TCP},
        {"udp+tcp", 0}, {"none", 0}, {"invalid", 0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char text[128];
        int length = snprintf(text, sizeof(text), "[global]\nauto_sockets=yes\n%s%s\n",
                              cases[i].value[0] ? "auto_socket_protocols=" : "", cases[i].value);
        struct utun_config* config = parse_config_from_buf(text, length, "auto-socket-protocols");
        if (!cases[i].mask) { assert(!config); continue; }
        assert(config && config->global.auto_socket_protocols == cases[i].mask);
        free_config(config);
    }
    const char* empty = "[global]\nauto_socket_protocols=\n";
    assert(!parse_config_from_buf(empty, strlen(empty), "empty-auto-socket-protocols"));
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Auto socket protocols: defaults, valid values and rejection verified");
}

int main() {
    // Initialize socket platform (required for getaddrinfo on Windows)
    if (socket_platform_init() != 0) {
        fprintf(stderr, "Failed to initialize socket platform\n");
        return 1;
    }

    // Create temp config file first
    if (create_temp_config() != 0) {
        fprintf(stderr, "Failed to create temporary config file\n");
        socket_platform_cleanup();
        return 1;
    }

    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "=== Тест загрузки debug настроек из конфигурационного файла ===");

    // Инициализация отладки по умолчанию
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR); // Начальный уровень

    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "1. До загрузки конфигурации:");
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "ERROR сообщение до загрузки конфигурации");
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "INFO сообщение до загрузки конфигурации (не должно выводиться)");

    // Загрузка конфигурации из файла
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "2. Загрузка конфигурации из файла: %s", config_path);
    struct utun_config *config = parse_config(config_path);
    
    if (!config) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "❌ Ошибка загрузки конфигурации");
        return 1;
    }
    
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "✅ Конфигурация загружена успешно");
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "   Log file: %s", config->global.log_file[0] ? config->global.log_file : "(none)");
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "   Debug level: %s", config->global.debug_level[0] ? config->global.debug_level : "(none)");
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "   Enable timestamp: %d", config->global.enable_timestamp);
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "   Enable colors: %d", config->global.enable_colors);
    
    // Тестирование новых настроек
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "4. Тестирование после применения настроек:");
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "ERROR от ETCP (должен выводиться)");
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "INFO от ETCP (должен выводиться - уровень debug)");
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "DEBUG от ETCP (должен выводиться - уровень debug)");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "INFO от LL_QUEUE (должен выводиться)");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "INFO от MEMORY (должен выводиться)");
    DEBUG_INFO(DEBUG_CATEGORY_TRAFFIC, "INFO от TRAFFIC (должен выводиться)");
    DEBUG_DEBUG(DEBUG_CATEGORY_TRAFFIC, "DEBUG от TRAFFIC (должен выводиться - уровень debug)");
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "INFO от CONNECTION (НЕ должен выводиться - категория не включена)");
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "INFO от CRYPTO (НЕ должен выводиться - категория не включена)");
    
    // Проверка работы с файлом
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "5. Проверка вывода в файл:");
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Это сообщение должно быть записано в файл: /tmp/utun_debug.log");
    
    // Освобождение ресурсов
    free_config(config);
    test_all_debug_categories();
    test_debug_disabled_category();
    test_debug_output_contract();
    test_debug_log_reopen();
    test_auto_socket_protocols();

    // Cleanup temp config file
    cleanup_temp_config();

    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "=== Тест завершен ===");
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Проверьте файл /tmp/utun_debug.log для записанных сообщений");

    socket_platform_cleanup();
    return 0;
}
