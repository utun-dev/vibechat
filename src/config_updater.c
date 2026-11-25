/* Идентичность INI: эффективные значения, точечная правка текста и публикация файла. */
#define _POSIX_C_SOURCE 200809L
#include "config_updater.h"
#include "config_parser.h"
#include "config_ini.h"
#include "secure_channel.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <openssl/rand.h>
#ifdef _WIN32
#include <io.h>
#endif

enum { CFG_PRIVATE, CFG_PUBLIC, CFG_REALITY_PRIVATE, CFG_REALITY_IDS, CFG_REALITY_ENABLED,
       CFG_SERVER_KEY, CFG_SERVER_ENABLED, CFG_OLD_NODE_ID, CFG_FIELD_COUNT };
enum { CFG_OTHER_SECTION, CFG_GLOBAL_SECTION, CFG_REALITY_SECTION, CFG_CHATSERVER_SECTION, CFG_SECTION_COUNT };
static const char* config_section_names[] = {"", "global", "reality", "chatserver"};

struct config_field {
    const char *key;
    int section;
    int present;
    int update;
    int invalid; // хотя бы одно вхождение поля содержит ошибочный синтаксис
    char value[CONFIG_INI_LINE_SIZE];
};



/* Форматировать без усечения; проверка ёмкости не переполняет size_t. */
void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex_str, size_t hex_len) {
    static const char digits[] = "0123456789abcdef";
    if (!bytes || !hex_str || !hex_len || len > (hex_len - 1) / 2) {
        if (hex_str && hex_len) hex_str[0] = '\0';
        return;
    }
    for (size_t i = 0; i < len; i++) { hex_str[i * 2] = digits[bytes[i] >> 4]; hex_str[i * 2 + 1] = digits[bytes[i] & 15]; }
    hex_str[len * 2] = '\0';
}

/* Единый перечень полей для сбора, редактирования и проверки результата. */
static void config_fields_init(struct config_field fields[CFG_FIELD_COUNT]) {
    memset(fields, 0, sizeof(*fields) * CFG_FIELD_COUNT);
    fields[CFG_PRIVATE].key = "my_private_key"; fields[CFG_PRIVATE].section = CFG_GLOBAL_SECTION;
    fields[CFG_PUBLIC].key = "my_public_key"; fields[CFG_PUBLIC].section = CFG_GLOBAL_SECTION;
    fields[CFG_REALITY_PRIVATE].key = "private_key"; fields[CFG_REALITY_PRIVATE].section = CFG_REALITY_SECTION;
    fields[CFG_REALITY_IDS].key = "short_ids"; fields[CFG_REALITY_IDS].section = CFG_REALITY_SECTION;
    fields[CFG_REALITY_ENABLED].key = "enabled"; fields[CFG_REALITY_ENABLED].section = CFG_REALITY_SECTION;
    fields[CFG_SERVER_KEY].key = "add_key"; fields[CFG_SERVER_KEY].section = CFG_CHATSERVER_SECTION;
    fields[CFG_SERVER_ENABLED].key = "chatserver_enabled"; fields[CFG_SERVER_ENABLED].section = CFG_GLOBAL_SECTION;
    fields[CFG_OLD_NODE_ID].key = "my_node_id"; fields[CFG_OLD_NODE_ID].section = CFG_GLOBAL_SECTION;
}

/* Любой заголовок завершает предыдущую секцию; значения разбирает общий INI-код. */
static int config_field_line(char *line, int *section, const struct config_field fields[CFG_FIELD_COUNT], char *value) {
    config_ini_trim(line);
    if (line[0] == '[') {
        char name[128];
        *section = CFG_OTHER_SECTION;
        if (!config_ini_section(line, name, sizeof(name))) {
            if (!strcasecmp(name, "global")) *section = CFG_GLOBAL_SECTION;
            else if (!strcasecmp(name, "reality")) *section = CFG_REALITY_SECTION;
            else if (!strcasecmp(name, "chatserver")) *section = CFG_CHATSERVER_SECTION;
        }
        return -1;
    }
    if (!line[0] || line[0] == '#') return -1;
    char key[CONFIG_INI_LINE_SIZE];
    if (config_ini_key_value(line, key, sizeof(key), value, CONFIG_INI_LINE_SIZE)) return -1;
    for (int i = 0; i < CFG_FIELD_COUNT; i++) if (fields[i].section == *section && !strcmp(fields[i].key, key)) return i;
    return -1;
}

/* Собрать последние значения и точку вставки до завершающих пустых строк каждой секции. */
static void config_scan(const char *text, size_t length, char *probe, int repair_reality,
                        struct config_field fields[CFG_FIELD_COUNT], size_t ends[CFG_SECTION_COUNT]) {
    int section = CFG_OTHER_SECTION;
    size_t content_end = 0;
    for (int i = 0; i < CFG_SECTION_COUNT; i++) ends[i] = SIZE_MAX;
    for (size_t pos = 0; pos < length;) {
        const char *nl = memchr(text + pos, '\n', length - pos);
        size_t end = nl ? (size_t)(nl - text) + 1 : length;
        char line[CONFIG_INI_LINE_SIZE], value[CONFIG_INI_LINE_SIZE];
        memcpy(line, text + pos, end - pos); line[end - pos] = '\0';
        char *start = line;
        while (isspace((unsigned char)*start)) start++;
        int nonempty = *start != '\0';
        if (*start == '[') ends[section] = content_end;
        int index = config_field_line(line, &section, fields, value);
        if (index >= 0) {
            /* Пробный разбор проверяет пользовательские строки на их исходных местах.
             * Эти поля проверяются отдельно и могут потребовать точечной правки. */
            if (probe && (index == CFG_PRIVATE || index == CFG_PUBLIC || index == CFG_OLD_NODE_ID ||
                ((index == CFG_REALITY_PRIVATE || index == CFG_REALITY_IDS || index == CFG_REALITY_ENABLED) && repair_reality))) probe[pos] = '#';
            strcpy(fields[index].value, value); fields[index].present = 1;
            if (index == CFG_REALITY_IDS) {
                int count;
                if (config_ini_hex_list(value, NULL, REALITY_SHORT_ID_SIZE, REALITY_MAX_SHORT_IDS, &count) || !count)
                    fields[index].invalid = 1;
            } else if (index == CFG_REALITY_ENABLED || index == CFG_SERVER_ENABLED) {
                if (strcmp(value, "0") && strcmp(value, "1") && strcasecmp(value, "no") && strcasecmp(value, "yes") &&
                    strcasecmp(value, "false") && strcasecmp(value, "true")) fields[index].invalid = 1;
            } else if (index != CFG_OLD_NODE_ID && config_ini_hex(value, NULL, SC_PRIVKEY_SIZE)) fields[index].invalid = 1;
        }
        if (nonempty) content_end = end;
        pos = end;
    }
    ends[section] = content_end;
}

/* Вставить отсутствующие поля в нужную секцию, при необходимости создать заголовок. */
static int config_insert_missing(struct strbuf *output, const struct config_field fields[CFG_FIELD_COUNT],
                                 int section, int has_section, const char *eol) {
    int started = 0;
    for (int i = 0; i < CFG_FIELD_COUNT; i++) {
        if (fields[i].section != section || !fields[i].update || fields[i].present) continue;
        if (!started) {
            if (output->len && output->buf[output->len - 1] != '\n' && strbuf_addf(output, "%s", eol) < 0) return -1;
            if (!has_section && strbuf_addf(output, "[%s]%s", config_section_names[section], eol) < 0) return -1;
            started = 1;
        }
        if (strbuf_addf(output, "%s=%s%s", fields[i].key, fields[i].value, eol) < 0) return -1;
        DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "inserted [%s] %s before section separator", config_section_names[section], fields[i].key);
    }
    return 0;
}

/* Сохранить всё вне значений исправляемых полей, включая комментарии и CRLF. */
static int config_edit(const char *text, size_t length, const struct config_field fields[CFG_FIELD_COUNT],
                       const size_t ends[CFG_SECTION_COUNT], struct strbuf *output) {
    const char *nl = memchr(text, '\n', length);
    const char *eol = nl && nl > text && nl[-1] == '\r' ? "\r\n" : "\n";
    int section = CFG_OTHER_SECTION;
    for (size_t pos = 0;;) {
        /* Запас покрывает вставки полей и строки: strbuf не должен вырасти до 2^32 байтов. */
        if (output->len > UINT32_MAX / 2 - 16384) {
            CONFIG_ERROR("edited INI exceeds allocator size limit");
            return -1;
        }
        for (int s = CFG_GLOBAL_SECTION; s < CFG_SECTION_COUNT; s++) {
            if (ends[s] == pos && config_insert_missing(output, fields, s, 1, eol)) return -1;
        }
        if (pos == length) break;
        nl = memchr(text + pos, '\n', length - pos);
        size_t end = nl ? (size_t)(nl - text) + 1 : length;
        char line[CONFIG_INI_LINE_SIZE], value[CONFIG_INI_LINE_SIZE];
        memcpy(line, text + pos, end - pos); line[end - pos] = '\0';
        int index = config_field_line(line, &section, fields, value);
        if (index == CFG_OLD_NODE_ID && fields[index].update) { pos = end; continue; }
        if (index >= 0 && fields[index].update) {
            const char *start = (const char *)memchr(text + pos, '=', end - pos) + 1;
            const char *tail = text + end;
            while (tail > start && (tail[-1] == '\n' || tail[-1] == '\r')) tail--;
            const char *comment = memchr(start, '#', (size_t)(tail - start));
            if (comment) tail = comment;
            while (start < tail && isspace((unsigned char)*start)) start++;
            while (tail > start && isspace((unsigned char)tail[-1])) tail--;
            if (strbuf_addf(output, "%.*s%s%.*s", (int)(start - text - pos), text + pos, fields[index].value,
                            (int)(text + end - tail), tail) < 0) return -1;
        } else if (strbuf_addf(output, "%.*s", (int)(end - pos), text + pos) < 0) return -1;
        pos = end;
    }
    if (output->len > UINT32_MAX / 2 - 16384) {
        CONFIG_ERROR("edited INI exceeds allocator size limit");
        return -1;
    }
    for (int s = CFG_GLOBAL_SECTION; s < CFG_SECTION_COUNT; s++) {
        if (ends[s] == SIZE_MAX && config_insert_missing(output, fields, s, 0, eol)) return -1;
    }
    return 0;
}

/* Проверить выбранную идентичность и REALITY строгими декодерами и штатным парсером. */
static int config_verify(const char *text, size_t length, const char *filename, const uint8_t private_key[SC_PRIVKEY_SIZE],
                         const uint8_t public_key[SC_PUBKEY_SIZE], int reality_enabled,
                         const uint8_t reality_private[REALITY_AUTH_KEY_SIZE],
                         const uint8_t ids[REALITY_MAX_SHORT_IDS][REALITY_SHORT_ID_SIZE], int id_count) {
    struct config_field fields[CFG_FIELD_COUNT];
    size_t ends[CFG_SECTION_COUNT];
    uint8_t decoded[SC_PRIVKEY_SIZE], decoded_ids[REALITY_MAX_SHORT_IDS][REALITY_SHORT_ID_SIZE];
    int parsed_count;
    config_fields_init(fields);
    config_scan(text, length, NULL, 0, fields, ends);
    if (config_ini_hex(fields[CFG_PRIVATE].value, decoded, sizeof(decoded)) || memcmp(decoded, private_key, sizeof(decoded)) ||
        config_ini_hex(fields[CFG_PUBLIC].value, decoded, sizeof(decoded)) || memcmp(decoded, public_key, sizeof(decoded))) return -1;
    if (reality_enabled && (config_ini_hex(fields[CFG_REALITY_PRIVATE].value, decoded, sizeof(decoded)) ||
        memcmp(decoded, reality_private, sizeof(decoded)) ||
        config_ini_hex_list(fields[CFG_REALITY_IDS].value, &decoded_ids[0][0], REALITY_SHORT_ID_SIZE, REALITY_MAX_SHORT_IDS, &parsed_count) ||
        parsed_count != id_count || memcmp(decoded_ids, ids, (size_t)id_count * REALITY_SHORT_ID_SIZE))) return -1;
    struct utun_config *config = parse_config_from_buf(text, length, filename);
    if (!config) return -1;
    int valid = !config_ini_hex(config->global.my_private_key_hex, decoded, sizeof(decoded)) &&
        !memcmp(decoded, private_key, sizeof(decoded)) &&
        !config_ini_hex(config->global.my_public_key_hex, decoded, sizeof(decoded)) && !memcmp(decoded, public_key, sizeof(decoded));
    if (reality_enabled) valid = valid && config->global.reality.enabled && config->global.reality.has_private_key &&
        !memcmp(config->global.reality.private_key, reality_private, REALITY_AUTH_KEY_SIZE) &&
        config->global.reality.short_id_count == id_count &&
        !memcmp(config->global.reality.short_ids, ids, (size_t)id_count * REALITY_SHORT_ID_SIZE);
    free_config(config);
    return valid ? 0 : -1;
}

/* Синхронизировать временный файл в том же каталоге, затем заменить имя. */
static enum config_profile_status config_publish(const char *filename, const char *text, size_t length, int exists, const struct stat *original) {
    FILE *file = NULL;
    char *temp = NULL;
    int result = -1, published = 0, temp_owned = 0;
    const char *phase = "inspect destination";
#ifndef _WIN32
    int directory = -1;
    char *parent = NULL;
    struct stat current;
    if (!lstat(filename, &current)) {
        if (!exists || !S_ISREG(current.st_mode) || current.st_nlink != 1 ||
            current.st_dev != original->st_dev || current.st_ino != original->st_ino) {
            CONFIG_ERROR("%s: destination changed or unsupported: existed=%d mode=%o links=%llu",
                        filename, exists, (unsigned)current.st_mode, (unsigned long long)current.st_nlink);
            errno = EINVAL;
            goto done;
        }
    } else if (exists || errno != ENOENT) goto done;
    parent = u_strdup(filename);
    if (!parent) goto done;
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    phase = "open parent directory";
    directory = open(parent, O_RDONLY | O_DIRECTORY);
    if (directory < 0) goto done;
#else
    (void)original;
    DWORD attributes = GetFileAttributesA(filename);
    if (attributes != INVALID_FILE_ATTRIBUTES && (!exists || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))) {
        errno = EINVAL;
        goto done;
    }
    if (exists) {
        HANDLE handle = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        BY_HANDLE_FILE_INFORMATION info;
        int valid = handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &info) && info.nNumberOfLinks == 1;
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (!valid) { errno = EINVAL; goto done; }
    }
#endif
    phase = "create temporary file";
    size_t name_length = strlen(filename);
    if (name_length > UINT32_MAX - 64) { errno = EOVERFLOW; goto done; }
    temp = u_malloc(name_length + 64);
    if (!temp) goto done;
    temp[0] = '\0';
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t random[16];
        char suffix[33];
        if (random_bytes(random, sizeof(random))) goto done;
        bytes_to_hex(random, sizeof(random), suffix, sizeof(suffix));
        snprintf(temp, name_length + 64, "%s.tmp.%s", filename, suffix);
#ifdef _WIN32
        int fd = _open(temp, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        int fd = open(temp, O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
        if (fd < 0) { if (errno == EEXIST) continue; goto done; }
        temp_owned = 1;
#ifdef _WIN32
        file = _fdopen(fd, "wb");
        if (!file) _close(fd);
#else
        file = fdopen(fd, "wb");
        if (!file) close(fd);
#endif
        if (!file) goto done;
        break;
    }
    if (!file) goto done;
    phase = "write temporary file";
    if (fwrite(text, 1, length, file) != length || fflush(file)) goto done;
#ifdef _WIN32
    phase = "sync temporary file";
    if (_commit(_fileno(file))) goto done;
#else
    phase = "preserve ownership and permissions";
    struct stat temporary;
    if (fstat(fileno(file), &temporary)) goto done;
    if (exists && (temporary.st_uid != original->st_uid || temporary.st_gid != original->st_gid) &&
        fchown(fileno(file), original->st_uid, original->st_gid)) goto done;
    if (fchmod(fileno(file), exists ? original->st_mode & 0777 : 0600)) goto done;
    phase = "sync temporary file";
    if (fsync(fileno(file))) goto done;
#endif
    phase = "close temporary file";
    int close_result = fclose(file);
    file = NULL;
    if (close_result) goto done;
    phase = "replace destination";
#ifdef _WIN32
    BOOL replaced = exists ? ReplaceFileA(filename, temp, NULL, 0, NULL, NULL) : MoveFileExA(temp, filename, MOVEFILE_WRITE_THROUGH);
    if (!replaced) { CONFIG_ERROR("%s: Windows replace failed: %lu", filename, GetLastError()); errno = EIO; goto done; }
#else
    if (rename(temp, filename)) goto done;
#endif
    published = 1;
#ifndef _WIN32
    phase = "sync parent directory after replacement";
    if (fsync(directory)) goto done;
#endif
    result = 0;
done:
    if (result) CONFIG_ERROR("%s: config update failed at '%s': %s (replaced=%d)", filename, phase, strerror(errno), published);
    if (file && fclose(file)) DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "%s: temporary close during cleanup failed: %s", filename, strerror(errno));
    if (temp_owned && !published && remove(temp) && errno != ENOENT) {
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "%s: temporary cleanup failed: %s", temp, strerror(errno));
    }
    u_free(temp);
#ifndef _WIN32
    if (directory >= 0 && close(directory)) DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "%s: parent directory close failed: %s", filename, strerror(errno));
    u_free(parent);
#endif
    if (!result) DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Config file updated: %s (%zu bytes)", filename, length);
    if (!result) return CONFIG_PROFILE_READY;
    return published ? CONFIG_PROFILE_IO_AFTER_REPLACE : CONFIG_PROFILE_IO_ERROR;
}

/* Прочитать проверенный снимок, выбрать идентичность и опубликовать необходимые изменения. */
static enum config_profile_status prepare_keys(const char *filename, int desktop, int replace_invalid) {
    config_error_clear();
    if (!filename || !*filename) { CONFIG_ERROR("empty config filename"); return -1; }
    char *text = NULL;
    size_t length = 0;
    int exists = 0, result = -1;
    struct stat original = {0};
    FILE *file = fopen(filename, "rb");
    if (file) {
        exists = 1;
        int rc = fstat(fileno(file), &original);
        if (!rc && !S_ISREG(original.st_mode)) { errno = EINVAL; rc = -1; }
        if (!rc) rc = config_ini_read(file, filename, &text, &length);
        if (fclose(file)) rc = -1;
        if (rc) { CONFIG_ERROR("%s: cannot read/close configuration: %s", filename, strerror(errno)); u_free(text); return CONFIG_PROFILE_IO_ERROR; }
    } else if (desktop || errno != ENOENT) {
        CONFIG_ERROR("%s: cannot open configuration: %s", filename, strerror(errno));
        return CONFIG_PROFILE_IO_ERROR;
    }
    if (!text) text = u_strdup("");
    if (!text) { CONFIG_ERROR("%s: cannot allocate configuration buffer", filename); return -1; }
    struct config_field fields[CFG_FIELD_COUNT];
    size_t ends[CFG_SECTION_COUNT];
    config_fields_init(fields);
    config_scan(text, length, NULL, 0, fields, ends);
    const char *enabled = fields[CFG_REALITY_ENABLED].value;
    int reality_enabled = !strcmp(enabled, "1") || !strcasecmp(enabled, "yes") || !strcasecmp(enabled, "true");
    char *probe = desktop ? u_strdup(text) : NULL;
    if (desktop && !probe) { CONFIG_ERROR("%s: configuration allocation failed", filename); u_free(text); return CONFIG_PROFILE_ERROR; }
    if (desktop) {
        /* Во втором проходе скрываем лишь поля, за подготовку которых отвечает updater.
         * Остальные значения, включая выключенный REALITY, проверяет штатный парсер. */
        config_scan(text, length, probe, reality_enabled, fields, ends);
        struct utun_config *parsed = parse_config_from_buf(probe, length, filename);
        u_free(probe);
        if (!parsed) { u_free(text); return CONFIG_PROFILE_ERROR; }
        free_config(parsed);
        fields[CFG_OLD_NODE_ID].update = fields[CFG_OLD_NODE_ID].present;
    }
    uint8_t private_key[SC_PRIVKEY_SIZE], public_key[SC_PUBKEY_SIZE], decoded[SC_PUBKEY_SIZE];
    uint8_t reality_private[REALITY_AUTH_KEY_SIZE] = {0}, ids[REALITY_MAX_SHORT_IDS][REALITY_SHORT_ID_SIZE] = {{0}};
    int id_count = 0, updates = 0;
    struct strbuf output = strbuf_new();
    const char* server_enabled = fields[CFG_SERVER_ENABLED].value;
    int server = ends[CFG_CHATSERVER_SECTION] != SIZE_MAX || !strcmp(server_enabled, "1") ||
        !strcasecmp(server_enabled, "yes") || !strcasecmp(server_enabled, "true");
    if (server) {
        uint8_t add_key[32];
        if (fields[CFG_SERVER_KEY].present) {
            if (fields[CFG_SERVER_KEY].invalid || config_ini_hex(fields[CFG_SERVER_KEY].value, add_key, sizeof(add_key))) {
                CONFIG_ERROR("%s: invalid chatserver add_key; refusing implicit rotation", filename); goto done;
            }
            int nonzero = 0; for (size_t i = 0; i < sizeof(add_key); i++) nonzero |= add_key[i];
            if (!nonzero) { CONFIG_ERROR("%s: zero chatserver add_key", filename); goto done; }
        } else {
            if (RAND_bytes(add_key, sizeof(add_key)) != 1) {
                CONFIG_ERROR("%s: chatserver add_key generation failed", filename); goto done;
            }
            bytes_to_hex(add_key, sizeof(add_key), fields[CFG_SERVER_KEY].value, sizeof(fields[CFG_SERVER_KEY].value));
            fields[CFG_SERVER_KEY].update = 1;
            DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "%s: generated persistent chatserver add_key", filename);
        }
    }
    uint8_t transport_secret[32], signing_public[32];
    int malformed = config_ini_hex(fields[CFG_PRIVATE].value, private_key, sizeof(private_key));
    int public_valid = !config_ini_hex(fields[CFG_PUBLIC].value, decoded, sizeof(decoded));
    int rc = malformed ? SC_ERR_AUTH_FAILED : sc_identity_from_seed(private_key, public_key, transport_secret, signing_public);
    OPENSSL_cleanse(transport_secret, sizeof(transport_secret));
    if (rc && rc != SC_ERR_AUTH_FAILED) { CONFIG_ERROR("%s: device key derivation failed rc=%d", filename, rc); goto done; }
    int mismatch = !rc && public_valid && memcmp(public_key, decoded, sizeof(decoded));
    if (desktop && (rc || mismatch) && !replace_invalid) {
        CONFIG_ERROR("%s: device identity requires user confirmation (%s)", filename,
                     malformed ? "missing or malformed seed" : mismatch ? "public key mismatch" : "noncanonical seed");
        result = CONFIG_PROFILE_INVALID_KEY; goto done;
    }
    if (!desktop && !malformed && rc) { CONFIG_ERROR("%s: invalid device Ed25519 seed rc=%d", filename, rc); goto done; }
    if (malformed || (desktop && (rc || mismatch))) {
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "%s: generating device identity confirmed=%d", filename, replace_invalid);
        rc = sc_identity_generate(public_key, transport_secret, signing_public, private_key);
        OPENSSL_cleanse(transport_secret, sizeof(transport_secret));
        if (rc) { CONFIG_ERROR("%s: device key generation failed rc=%d", filename, rc); goto done; }
        bytes_to_hex(private_key, sizeof(private_key), fields[CFG_PRIVATE].value, sizeof(fields[CFG_PRIVATE].value));
        fields[CFG_PRIVATE].update = 1;
    }
    if (!public_valid || memcmp(decoded, public_key, sizeof(decoded))) {
        fields[CFG_PUBLIC].update = 1;
        bytes_to_hex(public_key, sizeof(public_key), fields[CFG_PUBLIC].value, sizeof(fields[CFG_PUBLIC].value));
    }
    for (int i = CFG_PRIVATE; i <= CFG_PUBLIC; i++) if (fields[i].invalid) fields[i].update = 1;
    if (fields[CFG_REALITY_ENABLED].present && !reality_enabled &&
        strcmp(enabled, "0") && strcasecmp(enabled, "no") && strcasecmp(enabled, "false")) {
        CONFIG_ERROR("%s: invalid REALITY enabled value", filename); goto done;
    }
    if (fields[CFG_REALITY_ENABLED].invalid) fields[CFG_REALITY_ENABLED].update = 1;
    if (reality_enabled) {
        if (config_ini_hex(fields[CFG_REALITY_PRIVATE].value, reality_private, sizeof(reality_private))) {
            uint8_t public_part[REALITY_AUTH_KEY_SIZE];
            DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "%s: generating REALITY private key: %s", filename,
                       fields[CFG_REALITY_PRIVATE].present ? "invalid" : "missing");
            if (reality_generate_keypair(reality_private, public_part) != REALITY_OK) goto done;
            bytes_to_hex(reality_private, sizeof(reality_private), fields[CFG_REALITY_PRIVATE].value, sizeof(fields[CFG_REALITY_PRIVATE].value));
            fields[CFG_REALITY_PRIVATE].update = 1;
        }
        if (config_ini_hex_list(fields[CFG_REALITY_IDS].value, &ids[0][0], REALITY_SHORT_ID_SIZE, REALITY_MAX_SHORT_IDS, &id_count) || !id_count) {
            DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "%s: generating 4 REALITY short IDs: effective list missing, empty or invalid", filename);
            id_count = 4;
            if (random_bytes(&ids[0][0], (size_t)id_count * REALITY_SHORT_ID_SIZE)) goto done;
            char *cursor = fields[CFG_REALITY_IDS].value;
            for (int i = 0; i < id_count; i++) {
                if (i) *cursor++ = ',';
                bytes_to_hex(ids[i], REALITY_SHORT_ID_SIZE, cursor, REALITY_SHORT_ID_SIZE * 2 + 1);
                cursor += REALITY_SHORT_ID_SIZE * 2;
            }
            fields[CFG_REALITY_IDS].update = 1;
        }
        if (fields[CFG_REALITY_PRIVATE].invalid) fields[CFG_REALITY_PRIVATE].update = 1;
        if (fields[CFG_REALITY_IDS].invalid) fields[CFG_REALITY_IDS].update = 1;
    }
    for (int i = 0; i < CFG_FIELD_COUNT; i++) {
        if (!fields[i].update) continue;
        updates++;
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "%s: updating [%s] %s", filename,
                   config_section_names[fields[i].section], fields[i].key);
    }
    if (!updates) { DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "%s: identity and REALITY unchanged", filename); result = 0; goto done; }
    if (config_edit(text, length, fields, ends, &output) || config_ini_validate(strbuf_str(&output), output.len, filename) ||
        config_verify(strbuf_str(&output), output.len, filename, private_key, public_key, reality_enabled, reality_private, ids, id_count)) {
        CONFIG_ERROR("%s: prepared config failed construction/verification", filename);
        goto done;
    }
    result = config_publish(filename, strbuf_str(&output), output.len, exists, &original);
done:
    if (result == CONFIG_PROFILE_ERROR && !*config_error_text()) CONFIG_ERROR("%s: could not prepare configuration", filename);
    OPENSSL_cleanse(private_key, sizeof(private_key));
    strbuf_free(&output);
    u_free(text);
    return result;
}

int config_ensure_keys(const char *filename) {
    return prepare_keys(filename, 0, 0) == CONFIG_PROFILE_READY ? 0 : -1;
}

enum config_profile_status config_prepare_profile(const char *filename, int replace_invalid) {
    return prepare_keys(filename, 1, replace_invalid);
}
