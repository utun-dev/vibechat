/* Регрессии идентичности INI и ошибки файловых операций без тестовых веток в production-коде. */
#define _GNU_SOURCE
#include "../src/config_updater.h"
#include "../src/config_parser.h"
#include "../src/transport_layer/secure_channel.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum fault { NONE, READ_OPEN, READ_DATA, READ_CLOSE, TEMP_OPEN, WRITE_DATA, FLUSH, FILE_SYNC, WRITE_CLOSE, MODE, REPLACE, DIR_SYNC };
static enum fault fault;
static int injected;
static FILE *input_stream, *output_stream;
static int allocation_fail_at, allocation_attempts;
static char directory[128], path[256];
static const char *private_hex = "abababababababababababababababababababababababababababababababab";

extern FILE *__real_fopen(const char *, const char *);
extern FILE *__real_fdopen(int, const char *);
extern size_t __real_fread(void *, size_t, size_t, FILE *);
extern size_t __real_fwrite(const void *, size_t, size_t, FILE *);
extern int __real_ferror(FILE *);
extern int __real_fflush(FILE *);
extern int __real_fclose(FILE *);
extern int __real_fsync(int);
extern int __real_fchmod(int, mode_t);
extern int __real_rename(const char *, const char *);
extern int __real_open(const char *, int, ...);
extern void *__real_u_malloc_impl(uint32_t, const char *);
extern void *__real_u_calloc_impl(uint32_t, uint32_t, const char *);
extern void *__real_u_realloc_impl(void *, uint32_t, const char *);
extern char *__real_u_strdup_impl(const char *, const char *);

static int allocation_fault(void) {
    if (!allocation_fail_at || ++allocation_attempts != allocation_fail_at) return 0;
    errno = ENOMEM;
    return 1;
}

void *__wrap_u_malloc_impl(uint32_t size, const char *where) {
    return allocation_fault() ? NULL : __real_u_malloc_impl(size, where);
}

void *__wrap_u_calloc_impl(uint32_t count, uint32_t size, const char *where) {
    return allocation_fault() ? NULL : __real_u_calloc_impl(count, size, where);
}

void *__wrap_u_realloc_impl(void *ptr, uint32_t size, const char *where) {
    return allocation_fault() ? NULL : __real_u_realloc_impl(ptr, size, where);
}

char *__wrap_u_strdup_impl(const char *text, const char *where) {
    return allocation_fault() ? NULL : __real_u_strdup_impl(text, where);
}

static int inject(enum fault wanted) {
    if (fault != wanted || injected) return 0;
    injected = 1;
    errno = wanted == READ_OPEN ? EACCES : ENOSPC;
    return 1;
}

FILE *__wrap_fopen(const char *name, const char *mode) {
    if (fault && !strcmp(name, path) && !strcmp(mode, "rb") && inject(READ_OPEN)) return NULL;
    FILE *file = __real_fopen(name, mode);
    if (fault && !strcmp(name, path) && !strcmp(mode, "rb")) input_stream = file;
    return file;
}

FILE *__wrap_fdopen(int fd, const char *mode) {
    FILE *file = __real_fdopen(fd, mode);
    if (fault) output_stream = file;
    return file;
}

size_t __wrap_fread(void *buffer, size_t size, size_t count, FILE *file) {
    if (file == input_stream && inject(READ_DATA)) return 0;
    return __real_fread(buffer, size, count, file);
}
size_t __wrap___fread_chk(void* buffer, size_t capacity, size_t size, size_t count, FILE* file) {
    assert(!size || count <= capacity / size);
    return __wrap_fread(buffer, size, count, file);
}

int __wrap_ferror(FILE *file) {
    if (file == input_stream && fault == READ_DATA && injected) return 1;
    return __real_ferror(file);
}

size_t __wrap_fwrite(const void *buffer, size_t size, size_t count, FILE *file) {
    if (file == output_stream && inject(WRITE_DATA)) return __real_fwrite(buffer, size, count / 2, file);
    return __real_fwrite(buffer, size, count, file);
}

int __wrap_fflush(FILE *file) {
    if (file == output_stream && inject(FLUSH)) return EOF;
    return __real_fflush(file);
}

int __wrap_fclose(FILE *file) {
    int fail = (file == input_stream && inject(READ_CLOSE)) || (file == output_stream && inject(WRITE_CLOSE));
    int result = __real_fclose(file);
    if (fail) { errno = ENOSPC; return EOF; }
    return result;
}

int __wrap_fsync(int fd) {
    struct stat info;
    assert(!fstat(fd, &info));
    if (inject(S_ISDIR(info.st_mode) ? DIR_SYNC : FILE_SYNC)) return -1;
    return __real_fsync(fd);
}

int __wrap_fchmod(int fd, mode_t mode) {
    if (inject(MODE)) return -1;
    return __real_fchmod(fd, mode);
}

int __wrap_rename(const char *source, const char *destination) {
    if (inject(REPLACE)) return -1;
    return __real_rename(source, destination);
}

int __wrap_open(const char *name, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags); mode = va_arg(args, int); va_end(args);
        if (inject(TEMP_OPEN)) return -1;
    }
    return __real_open(name, flags, mode);
}

static void write_bytes(const void *text, size_t length) {
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(text, 1, length, file) == length);
    assert(!fclose(file));
}

static void write_text(const char *text) { write_bytes(text, strlen(text)); }

static size_t read_text(char *text, size_t capacity) {
    FILE *file = fopen(path, "rb");
    assert(file);
    size_t length = fread(text, 1, capacity - 1, file);
    assert(!ferror(file) && feof(file));
    text[length] = '\0';
    assert(!fclose(file));
    return length;
}

/* Проверить реальный файл и буфер, сохранение private и производные public/node_id. */
static void check_identity(const char *expected_private) {
    char text[8192];
    size_t length = read_text(text, sizeof(text));
    struct utun_config *configs[] = {parse_config(path), parse_config_from_buf(text, length, path)};
    assert(!strstr(text, "my_node_id="));
    for (size_t i = 0; i < 2; i++) {
        assert(configs[i]);
        uint8_t private_key[32], public_key[32], stored_public[32], ed[32], transport_secret[32];
        assert(!sc_hex_to_binary(configs[i]->global.my_private_key_hex, private_key, sizeof(private_key)));
        assert(!sc_hex_to_binary(configs[i]->global.my_public_key_hex, stored_public, sizeof(stored_public)));
        assert(sc_identity_from_seed(private_key, public_key, transport_secret, ed) == SC_OK);
        assert(!memcmp(public_key, stored_public, sizeof(public_key)));
        if (expected_private) assert(!strcasecmp(expected_private, configs[i]->global.my_private_key_hex));
        free_config(configs[i]);
    }
}

static void check_no_temporary(void) {
    DIR *dir = opendir(directory);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) assert(!strstr(entry->d_name, ".tmp."));
    assert(!closedir(dir));
}

static void test_identity_and_text(void) {
    char text[8192], before[8192];
    assert(!unlink(path));
    assert(!config_ensure_keys(path));
    check_identity(NULL);
    struct stat info;
    assert(!stat(path, &info) && (info.st_mode & 0777) == 0600);

    snprintf(text, sizeof(text), "# sentinel [global]\r\n[gui]\r\nmy_private_key=foreign\r\n"
        "[ GLOBAL ]\r\n  my_private_key = %s  # keep comment\r\n"
        "my_public_key=bad\r\n[global_extra]\r\nmy_public_key=untouched\r\n"
        "[global]\r\nmy_public_key=still_bad\r\n", private_hex);
    write_text(text);
    assert(!chmod(path, 0640));
    struct stat ownership;
    assert(!stat(path, &ownership));
    assert(!config_ensure_keys(path));
    check_identity(private_hex);
    assert(!stat(path, &info) && (info.st_mode & 0777) == 0640);
    assert(info.st_uid == ownership.st_uid && info.st_gid == ownership.st_gid);
    read_text(text, sizeof(text));
    assert(strstr(text, "[gui]\r\nmy_private_key=foreign\r\n"));
    assert(strstr(text, "[global_extra]\r\nmy_public_key=untouched\r\n"));
    assert(strstr(text, "  # keep comment\r\n"));
    assert(strstr(text, private_hex));
    for (const char *p = text; *p; p++) if (*p == '\n') assert(p > text && p[-1] == '\r');

    struct utun_config *config = parse_config(path);
    assert(config);
    for (char *p = config->global.my_public_key_hex; *p; p++) if (*p >= 'a' && *p <= 'f') *p -= 'a' - 'A';
    snprintf(text, sizeof(text), "[global]\nmy_private_key = %s\nmy_public_key=%s",
             private_hex, config->global.my_public_key_hex);
    free_config(config);
    write_text(text);
    struct stat old_info;
    assert(!stat(path, &old_info));
    assert(!config_ensure_keys(path));
    check_identity(private_hex);
    read_text(before, sizeof(before));
    assert(!strcmp(before, text));
    assert(!stat(path, &info) && info.st_ino == old_info.st_ino);

    snprintf(text, sizeof(text), "[global]\nmy_private_key=%064d\n[GLOBAL]\nmy_private_key = %s\n", 0, private_hex);
    write_text(text);
    assert(!config_ensure_keys(path));
    check_identity(private_hex);

    write_text("[global]\nmy_private_key=bad\nmy_private_key=still_bad\n");
    assert(!config_ensure_keys(path));
    check_identity(NULL);
    read_text(before, sizeof(before));
    assert(!strstr(before, "my_private_key=bad\n") && !strstr(before, "my_private_key=still_bad\n"));
    assert(!config_ensure_keys(path));
    read_text(text, sizeof(text));
    assert(!strcmp(before, text));

    write_text("[global_extra]\n# sentinel without final LF");
    assert(!config_ensure_keys(path));
    check_identity(NULL);
    read_text(text, sizeof(text));
    assert(strstr(text, "[global_extra]\n# sentinel without final LF\n[global]\n"));
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS identity: whitespace, sections, duplicates, HEX case, CRLF, no-op and permissions");
}

static void test_reality_and_validation(void) {
    char text[8192], before[8192], invalid_hex[65];
    for (int i = 0; i < 32; i++) { invalid_hex[i * 2] = '1'; invalid_hex[i * 2 + 1] = 'g'; }
    invalid_hex[64] = '\0';
    snprintf(text, sizeof(text), "[global]\nmy_private_key=%s\n[REALITY]\nenabled=1\nprivate_key=%s\n"
        "short_ids=0102030405060708,bad\n[reality]\nshort_ids =\n", private_hex, invalid_hex);
    struct utun_config *config = parse_config_from_buf(text, strlen(text), "malformed");
    assert(!config); // парсер отклоняет ошибку; updater по-прежнему исправляет исходный INI до загрузки
    write_text(text);
    assert(!config_ensure_keys(path));
    check_identity(private_hex);
    config = parse_config(path);
    assert(config && config->global.reality.has_private_key && config->global.reality.short_id_count == 4);
    free_config(config);
    read_text(before, sizeof(before));
    // Случайный корректный HEX может содержать "bad"; проверяем удаление исходного неверного значения целиком.
    assert(!strstr(before, "short_ids=0102030405060708,bad\n") && !strstr(before, invalid_hex));
    assert(!config_ensure_keys(path));
    read_text(text, sizeof(text));
    assert(!strcmp(before, text));

    snprintf(text, sizeof(text), "[global]\nmy_private_key=%s\n[reality]\nenabled=1\nprivate_key=%s\n"
        "short_ids=bad\n[REALITY]\nshort_ids = 0102030405060708\n", private_hex, private_hex);
    write_text(text);
    assert(!config_ensure_keys(path));
    config = parse_config(path);
    assert(config && config->global.reality.short_id_count == 1);
    assert(!strcasecmp(config->global.my_private_key_hex, private_hex));
    free_config(config);
    size_t saved_length = read_text(before, sizeof(before));
    assert(saved_length + 20 < sizeof(before));
    strcat(before, "enabled=0\nshort_ids=bad\nprivate_key=bad\n");
    write_text(before);
    assert(!config_ensure_keys(path));
    read_text(text, sizeof(text));
    assert(!strcmp(before, text));

    const char *bad_lists[] = {"0102030405060708,bad", "0102030405060708,", ",0102030405060708", "0102030405060708,,0102030405060708"};
    for (size_t i = 0; i < sizeof(bad_lists) / sizeof(*bad_lists); i++) {
        snprintf(text, sizeof(text), "[reality]\nshort_ids=%s\n", bad_lists[i]);
        config = parse_config_from_buf(text, strlen(text), "bad-list");
        assert(!config);
    }
    memset(text, 'x', 1023); text[1023] = '\n'; text[1024] = '\0';
    write_text(text);
    assert(config_ensure_keys(path) == -1 && !parse_config(path));
    assert(!parse_config_from_buf(text, strlen(text), "long-line"));
    read_text(before, sizeof(before));
    assert(!strcmp(before, text));
    const char nul[] = "[global]\n\0my_private_key=bad\n";
    write_bytes(nul, sizeof(nul) - 1);
    assert(config_ensure_keys(path) == -1 && !parse_config(path));
    assert(!parse_config_from_buf(nul, sizeof(nul) - 1, "nul"));
    char hex[3] = "xx";
    uint8_t byte = 0xab;
    bytes_to_hex(&byte, SIZE_MAX / 2 + 1, hex, sizeof(hex));
    assert(!hex[0]);
    bytes_to_hex(&byte, 1, hex, sizeof(hex));
    assert(!strcmp(hex, "ab"));
    check_no_temporary();
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS REALITY: strict HEX, whole lists, duplicate repair, idempotence, NUL and long lines");
}

static void test_failures(void) {
    const char *original = "[global]\n# sentinel\n";
    for (enum fault which = READ_OPEN; which <= DIR_SYNC; which++) {
        write_text(original);
        struct stat before, after;
        assert(!stat(path, &before));
        fault = which; injected = 0; input_stream = output_stream = NULL;
        int result = config_ensure_keys(path);
        fault = NONE; input_stream = output_stream = NULL;
        assert(injected && result == -1);
        char text[8192];
        read_text(text, sizeof(text));
        assert(!stat(path, &after));
        if (which == DIR_SYNC) { assert(strcmp(text, original) && before.st_ino != after.st_ino); check_identity(NULL); }
        else assert(!strcmp(text, original) && before.st_ino == after.st_ino);
        check_no_temporary();
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS injected file fault=%d, replaced=%d", which, which == DIR_SYNC);
    }
    /* Отказ fsync каталога после rename: повтор GUI использует уже опубликованную пару. */
    write_text(original);
    assert(config_prepare_profile(path, 0) == CONFIG_PROFILE_INVALID_KEY);
    fault = DIR_SYNC; injected = 0;
    assert(config_prepare_profile(path, 1) == CONFIG_PROFILE_IO_AFTER_REPLACE && injected);
    fault = NONE;
    char saved[8192], retried[8192]; read_text(saved, sizeof(saved));
    assert(config_prepare_profile(path, 0) == CONFIG_PROFILE_READY);
    read_text(retried, sizeof(retried)); assert(!strcmp(saved, retried)); check_identity(NULL);
    char link_path[256];
    snprintf(link_path, sizeof(link_path), "%s/linked", directory);
    write_text(original);
    assert(!link(path, link_path));
    assert(config_ensure_keys(path) == -1);
    assert(!unlink(link_path));
    assert(!rename(path, link_path));
    assert(!symlink(link_path, path));
    assert(config_ensure_keys(path) == -1);
    struct stat link_info;
    assert(!lstat(path, &link_info) && S_ISLNK(link_info.st_mode));
    assert(!unlink(path) && !rename(link_path, path));
    char text[8192]; read_text(text, sizeof(text));
    assert(!strcmp(text, original));
    check_no_temporary();
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS links: rejected updates preserve names and original bytes");
}

/* Каждая аллокация на пути обновления может отказать: исходный файл и баланс памяти сохраняются. */
static void test_allocation_failures(void) {
    const char *original = "[global]\n# sentinel\n";
    size_t baseline = u_get_allocated_count();
    int completed = 0;
    for (int index = 1; index < 64; index++) {
        write_text(original);
        allocation_attempts = 0; allocation_fail_at = index;
        int result = config_ensure_keys(path);
        allocation_fail_at = 0;
        assert(u_get_allocated_count() == baseline);
        check_no_temporary();
        if (allocation_attempts < index) { assert(result == 0); completed = 1; break; }
        assert(result == -1);
        char text[8192]; read_text(text, sizeof(text));
        assert(!strcmp(text, original));
    }
    assert(completed);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS allocation failures: every allocation, original bytes and resource balance");
}

/* Недостающие поля остаются в блоке; исходные пустые строки продолжают отделять секции. */
static void test_insertion_before_separator(void) {
    const struct { const char *input, *anchor, *first, *last, *suffix; } cases[] = {
        {"[chatserver]\nstorage_autoload=1\n\n[gui]\nutun_enabled=no\n",
            "storage_autoload=1", "add_key=", "add_key=", "\n[gui]\nutun_enabled=no\n"},
        {"[chatserver]\nstorage_autoload=1\n\n\n",
            "storage_autoload=1", "add_key=", "add_key=", "\n\n"},
        {"[chatserver]\n\n[gui]\nutun_enabled=no\n",
            "[chatserver]", "add_key=", "add_key=", "\n[gui]\nutun_enabled=no\n"},
        {"[chatserver]\nstorage_autoload=0\n\n[chatserver]\nstorage_autoload=1\n\n[gui]\nutun_enabled=no\n",
            "storage_autoload=1", "add_key=", "add_key=", "\n[gui]\nutun_enabled=no\n"},
        {"[chatserver]\nstorage_autoload=1\n \t\n\n[gui]\nutun_enabled=no\n",
            "storage_autoload=1", "add_key=", "add_key=", " \t\n\n[gui]\nutun_enabled=no\n"},
        {"[global]\nmy_private_key=%s\n\n[gui]\nutun_enabled=no\n",
            "my_private_key=", "my_public_key=", "my_public_key=", "\n[gui]\nutun_enabled=no\n"},
        {"[reality]\nenabled=1\n\n[gui]\nutun_enabled=no\n",
            "enabled=1", "private_key=", "short_ids=", "\n[gui]\nutun_enabled=no\n"}
    };
    for (int crlf = 0; crlf < 2; crlf++) for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        char plain[2048], input[4096], suffix[4096], after[8192], unchanged[8192];
        snprintf(plain, sizeof(plain), cases[i].input, private_hex);
        size_t pos = 0;
        for (const char* p = plain; *p; p++) { if (crlf && *p == '\n') input[pos++] = '\r'; input[pos++] = *p; }
        input[pos] = 0; pos = 0;
        for (const char* p = cases[i].suffix; *p; p++) { if (crlf && *p == '\n') suffix[pos++] = '\r'; suffix[pos++] = *p; }
        suffix[pos] = 0;
        const char* eol = crlf ? "\r\n" : "\n";
        write_text(input); assert(!config_ensure_keys(path)); read_text(after, sizeof(after));
        const char* anchor = strstr(after, cases[i].anchor); assert(anchor);
        const char* first = strstr(anchor, cases[i].first);
        assert(first && first == strstr(anchor, eol) + strlen(eol));
        const char* last = strstr(first, cases[i].last); assert(last);
        const char* end = strstr(last, eol); assert(end);
        assert(!strncmp(end + strlen(eol), suffix, strlen(suffix)));
        assert(!config_ensure_keys(path)); read_text(unchanged, sizeof(unchanged)); assert(!strcmp(after, unchanged));
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS field insertion: 14 LF/CRLF separator, EOF, empty/repeated section and stable-repeat cases");
}

static void test_server_key(void) {
    char before[8192], after[8192]; uint8_t key[32];
    write_text("[chatserver]\n# server credential\n");
    assert(!config_ensure_keys(path));
    struct utun_config* config = parse_config(path); assert(config && config->global.chatserver_has_add_key);
    memcpy(key, config->global.chatserver_add_key, sizeof(key)); free_config(config);
    read_text(before, sizeof(before));
    assert(strstr(before, "add_key=") && !config_ensure_keys(path));
    read_text(after, sizeof(after)); assert(!strcmp(before, after));
    config = parse_config(path); assert(config && !memcmp(key, config->global.chatserver_add_key, sizeof(key))); free_config(config);
    write_text("[chatserver]\nadd_key=invalid\n"); read_text(before, sizeof(before));
    assert(config_ensure_keys(path) < 0); read_text(after, sizeof(after)); assert(!strcmp(before, after));
    write_text("[chatserver]\nadd_key=0000000000000000000000000000000000000000000000000000000000000000\n");
    read_text(before, sizeof(before)); assert(config_ensure_keys(path) < 0);
    read_text(after, sizeof(after)); assert(!strcmp(before, after));
    write_text("[chatserver]\n# atomic credential\n"); read_text(before, sizeof(before));
    fault = REPLACE; injected = 0; input_stream = output_stream = NULL;
    assert(config_ensure_keys(path) < 0 && injected); fault = NONE;
    read_text(after, sizeof(after)); assert(!strcmp(before, after)); check_no_temporary();
    assert(!config_ensure_keys(path));
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS server key: generation, persistence, invalid/zero rejection, atomic failure and retry");
}

static void check_device_key_encoding(void) {
    char zero_seed[65], pub_hex[65];
    uint8_t seed[32] = {0}, ed[32]; struct SC_MYKEYS keys;
    assert(sc_identity_from_seed(seed, keys.public_key, keys.private_key, ed) == SC_OK);
    bytes_to_hex(keys.public_key, sizeof(keys.public_key), pub_hex, sizeof(pub_hex));
    memset(zero_seed, '0', 64); zero_seed[64] = 0;
    assert(sc_identity_load(pub_hex, zero_seed, &keys, ed, seed) == SC_OK);
    assert(sc_identity_check("config test", keys.public_key, ed) == SC_OK);
    uint8_t derived[32], opposite[32];
    memcpy(opposite, ed, sizeof(opposite)); opposite[31] ^= 0x80;
    memset(derived, 0x42, sizeof(derived));
    assert(sc_identity_public_x("opposite sign after cache fill", opposite, derived) == SC_ERR_AUTH_FAILED);
    for (unsigned i = 0; i < sizeof(derived); i++) assert(derived[i] == 0x42);
    assert(sc_identity_public_x("cached identity", ed, derived) == SC_OK);
    assert(!memcmp(derived, keys.public_key, sizeof(derived)));
    zero_seed[1] = 'g';
    assert(sc_identity_load(pub_hex, zero_seed, &keys, ed, seed) == SC_ERR_INVALID_ARG);
    zero_seed[1] = '0'; pub_hex[0] = pub_hex[0] == '0' ? '1' : '0';
    assert(sc_identity_load(pub_hex, zero_seed, &keys, ed, seed) == SC_ERR_AUTH_FAILED);
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    snprintf(directory, sizeof(directory), "/tmp/utun-config-test-XXXXXX");
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/identity.ini", directory);
    write_text("");
    check_device_key_encoding();
    test_identity_and_text();
    test_reality_and_validation();
    test_failures();
    test_allocation_failures();
    test_insertion_before_separator();
    test_server_key();
    assert(!unlink(path) && !rmdir(directory));
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "PASS config updater regression suite");
    return 0;
}
