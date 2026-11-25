// test_reality_config.c — автогенерация reality private_key и short_ids в конфиге
//
// Проверяет config_ensure_keys():
//   1. [reality] enabled=1 без private_key/short_ids → генерируются (privkey + 4 id).
//   2. Пустая строка short_ids= заменяется на 4 id (без пустых строк и дублей).
//   3. [reality] enabled=0 → ничего не генерируется.
//   4. Без секции [reality] → ничего не генерируется.
//   5. Идемпотентность: повторный вызов не добавляет дублей.
#include "config_parser.h"
#include "config_updater.h"
#include "../lib/debug_config.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

static int test_failed = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "FAIL: %s", msg); test_failed = 1; } \
    else { DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS: %s", msg); } \
} while (0)

static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

static int read_file(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return (int)n;
}

// Возвращает: 0 — в секции [section] нет пустых строк, option встречается ровно want раз.
// 1 — секции нет, 2 — есть пустая строка, 3 — неверное число вхождений option.
static int check_section(const char *content, const char *section, const char *option, int want) {
    char hdr[64]; snprintf(hdr, sizeof(hdr), "[%s]", section);
    const char *sec = strstr(content, hdr);
    if (!sec) return 1;
    sec += strlen(hdr);
    while (*sec == '\r' || *sec == '\n') sec++;
    const char *end = strchr(sec, '[');
    if (!end) end = content + strlen(content);

    int found = 0, empty = 0;
    const char *line = sec;
    while (line < end) {
        const char *nl = strchr(line, '\n');
        const char *line_end = (nl && nl < end) ? nl : end;
        size_t len = (size_t)(line_end - line);
        while (len && (line[0] == ' ' || line[0] == '\t' || line[0] == '\r')) { line++; len--; }
        if (len == 0) empty++;
        if (option && strncmp(line, option, strlen(option)) == 0 && line[strlen(option)] == '=') found++;
        line = nl ? nl + 1 : end;
    }
    if (empty) return 2;
    if (found != want) return 3;
    return 0;
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);

    char tdir[] = "/tmp/utun_reality_cfg_XXXXXX";
    if (test_mkdtemp(tdir) != 0) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "mkdtemp failed"); return 1; }
    char path[512];
    snprintf(path, sizeof(path), "%s/r.conf", tdir);

    // ── Сценарий 1: enabled=1 без ключей → генерация ──
    {
        const char *cfg =
            "[global]\nmy_node_name=test\n"
            "[reality]\n"
            "enabled=1\n"
            "server_name=www.microsoft.com\n"
            "dest=www.microsoft.com:443\n";
        CHECK(write_file(path, cfg) == 0, "write config (scenario 1)");
        CHECK(config_ensure_keys(path) == 0, "ensure keys (scenario 1)");
        struct utun_config *uc = parse_config(path);
        CHECK(uc != NULL, "parse after generate (scenario 1)");
        if (uc) {
            CHECK(uc->global.reality.enabled == 1, "enabled=1");
            CHECK(uc->global.reality.has_private_key == 1, "private_key generated");
            CHECK(uc->global.reality.short_id_count == 4, "4 short_ids generated");
            free_config(uc);
        }
        char content[4096];
        CHECK(read_file(path, content, sizeof(content)) > 0, "read file (scenario 1)");
        CHECK(check_section(content, "reality", NULL, 0) == 0, "no empty lines in [reality]");
        CHECK(check_section(content, "reality", "private_key", 1) == 0, "exactly one private_key");
        CHECK(check_section(content, "reality", "short_ids", 1) == 0, "exactly one short_ids");
    }

    // ── Сценарий 2: пустая строка short_ids= заменяется ──
    {
        const char *cfg =
            "[global]\nmy_node_name=test\n"
            "[reality]\n"
            "enabled=1\n"
            "short_ids=\n"
            "server_name=www.microsoft.com\n";
        CHECK(write_file(path, cfg) == 0, "write config (scenario 2)");
        CHECK(config_ensure_keys(path) == 0, "ensure keys (scenario 2)");
        struct utun_config *uc = parse_config(path);
        CHECK(uc != NULL, "parse after generate (scenario 2)");
        if (uc) {
            CHECK(uc->global.reality.short_id_count == 4, "4 short_ids generated (replace)");
            CHECK(uc->global.reality.has_private_key == 1, "private_key generated (scenario 2)");
            free_config(uc);
        }
        char content[4096];
        CHECK(read_file(path, content, sizeof(content)) > 0, "read file (scenario 2)");
        CHECK(check_section(content, "reality", NULL, 0) == 0, "no empty lines (scenario 2)");
        CHECK(check_section(content, "reality", "short_ids", 1) == 0, "short_ids replaced, no dup (scenario 2)");
    }

    // ── Сценарий 3: enabled=0 → без генерации ──
    {
        const char *cfg =
            "[global]\nmy_node_name=test\n"
            "[reality]\n"
            "enabled=0\n"
            "server_name=www.microsoft.com\n";
        CHECK(write_file(path, cfg) == 0, "write config (scenario 3)");
        CHECK(config_ensure_keys(path) == 0, "ensure keys (scenario 3)");
        struct utun_config *uc = parse_config(path);
        CHECK(uc != NULL, "parse (scenario 3)");
        if (uc) {
            CHECK(uc->global.reality.has_private_key == 0, "no private_key (disabled)");
            CHECK(uc->global.reality.short_id_count == 0, "no short_ids (disabled)");
            free_config(uc);
        }
    }

    // ── Сценарий 4: без секции [reality] → без генерации ──
    {
        const char *cfg = "[global]\nmy_node_name=test\n";
        CHECK(write_file(path, cfg) == 0, "write config (scenario 4)");
        CHECK(config_ensure_keys(path) == 0, "ensure keys (scenario 4)");
        struct utun_config *uc = parse_config(path);
        CHECK(uc != NULL, "parse (scenario 4)");
        if (uc) {
            CHECK(uc->global.reality.enabled == 0, "reality disabled (no section)");
            CHECK(uc->global.reality.has_private_key == 0, "no private_key (no section)");
            CHECK(uc->global.reality.short_id_count == 0, "no short_ids (no section)");
            free_config(uc);
        }
    }

    // ── Сценарий 5: идемпотентность ──
    {
        const char *cfg =
            "[global]\nmy_node_name=test\n"
            "[reality]\n"
            "enabled=1\n"
            "server_name=www.microsoft.com\n";
        CHECK(write_file(path, cfg) == 0, "write config (scenario 5)");
        CHECK(config_ensure_keys(path) == 0, "ensure #1 (scenario 5)");
        struct utun_config *uc1 = parse_config(path);
        CHECK(uc1 != NULL, "parse #1 (scenario 5)");
        CHECK(config_ensure_keys(path) == 0, "ensure #2 (scenario 5)");
        struct utun_config *uc2 = parse_config(path);
        CHECK(uc2 != NULL, "parse #2 (scenario 5)");
        if (uc1 && uc2) {
            CHECK(uc1->global.reality.short_id_count == 4 && uc2->global.reality.short_id_count == 4, "ids stable");
            CHECK(memcmp(uc1->global.reality.private_key, uc2->global.reality.private_key, REALITY_AUTH_KEY_SIZE) == 0, "privkey stable");
            free_config(uc1);
            free_config(uc2);
        }
        char content[4096];
        CHECK(read_file(path, content, sizeof(content)) > 0, "read file (scenario 5)");
        CHECK(check_section(content, "reality", "private_key", 1) == 0, "no dup private_key (scenario 5)");
        CHECK(check_section(content, "reality", "short_ids", 1) == 0, "no dup short_ids (scenario 5)");
    }

    test_unlink(path);
    test_rmdir(tdir);

    if (test_failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "=== Reality Config Test: FAILED ===");
        return 1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "=== Reality Config Test: PASSED ===");
    return 0;
}
