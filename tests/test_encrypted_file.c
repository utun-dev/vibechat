/* Проверка контейнера на границах порций, подмены metadata/header, неверного ключа
 * и отсутствия публикации частичного plaintext после любого отказа. */
#include "../src/media_async/encrypted_file.h"
#include "../src/media_async/media_async.h"
#include "../src/media_async/media_crypto.h"
#include "../lib/debug_config.h"
#include "test_utils.h"
#include <assert.h>
#include <openssl/rand.h>
#include <errno.h>
#ifdef __linux__
#include <dirent.h>

enum fault { F_NONE, F_OPEN, F_FDOPEN, F_READ, F_WRITE, F_SEEK, F_FLUSH, F_INPUT_CLOSE, F_OUTPUT_CLOSE,
             F_SYNC, F_RENAME, F_DIRECTORY, F_RANDOM };
static enum fault fault;
static int injected, fault_after, read_failed;
static FILE *input_file, *output_file;
FILE* __real_fopen(const char*, const char*);
FILE* __real_fdopen(int, const char*);
size_t __real_fread(void*, size_t, size_t, FILE*), __real_fwrite(const void*, size_t, size_t, FILE*);
#ifdef __GLIBC__
size_t __real___fread_chk(void*, size_t, size_t, size_t, FILE*);
#endif
int __real_ferror(FILE*), __real_fflush(FILE*), __real_fclose(FILE*), __real_fsync(int);
int __real_fseeko(FILE*, off_t, int), __real_rename(const char*, const char*);
int __real_random_bytes(uint8_t*, size_t);
static int inject(enum fault value) {
    if (fault != value || injected || --fault_after > 0) return 0;
    injected = 1; errno = EIO; return 1;
}
FILE* __wrap_fopen(const char* path, const char* mode) {
    if (!strcmp(mode, "rb") && inject(F_OPEN)) return NULL;
    FILE* file = __real_fopen(path, mode);
    if (!strcmp(mode, "rb")) input_file = file;
    return file;
}
FILE* __wrap_fdopen(int fd, const char* mode) {
    if (inject(F_FDOPEN)) return NULL;
    return output_file = __real_fdopen(fd, mode);
}
size_t __wrap_fread(void* data, size_t size, size_t count, FILE* file) {
    if (file == input_file && inject(F_READ)) { read_failed = 1; return 0; }
    return __real_fread(data, size, count, file);
}
#ifdef __GLIBC__
size_t __wrap___fread_chk(void* data, size_t cap, size_t size, size_t count, FILE* file) {
    if (file == input_file && inject(F_READ)) { read_failed = 1; return 0; }
    return __real___fread_chk(data, cap, size, count, file);
}
#endif
int __wrap_ferror(FILE* file) { return file == input_file && read_failed ? 1 : __real_ferror(file); }
size_t __wrap_fwrite(const void* data, size_t size, size_t count, FILE* file) {
    return file == output_file && inject(F_WRITE) ? 0 : __real_fwrite(data, size, count, file);
}
int __wrap_fflush(FILE* file) { return file == output_file && inject(F_FLUSH) ? EOF : __real_fflush(file); }
int __wrap_fseeko(FILE* file, off_t offset, int whence) { return inject(F_SEEK) ? -1 : __real_fseeko(file, offset, whence); }
int __wrap_fclose(FILE* file) {
    int error = (file == input_file && inject(F_INPUT_CLOSE)) || (file == output_file && inject(F_OUTPUT_CLOSE));
    if (file == input_file) { input_file = NULL; read_failed = 0; }
    if (file == output_file) output_file = NULL;
    int result = __real_fclose(file);
    if (error) { errno = EIO; return EOF; }
    return result;
}
int __wrap_fsync(int fd) {
    struct stat data; assert(!fstat(fd, &data));
    return inject(S_ISDIR(data.st_mode) ? F_DIRECTORY : F_SYNC) ? -1 : __real_fsync(fd);
}
int __wrap_rename(const char* from, const char* to) { return inject(F_RENAME) ? -1 : __real_rename(from, to); }
int __wrap_random_bytes(uint8_t* data, size_t len) { return inject(F_RANDOM) ? -1 : __real_random_bytes(data, len); }
static unsigned count_fds(void) {
    DIR* dir = opendir("/proc/self/fd"); assert(dir);
    unsigned count = 0;
    while (readdir(dir)) count++;
    assert(!closedir(dir)); return count;
}
#endif

static void hash_same(const char* path, const uint8_t expected[32]) {
    uint8_t actual[32]; assert(!ma_sha256_file(path, actual) && !memcmp(actual, expected, 32));
}

static void roundtrip(const char* dir, unsigned size) {
    char source[768], cipher[768], output[768];
    snprintf(source, sizeof(source), "%s/source", dir); snprintf(cipher, sizeof(cipher), "%s/cipher", dir);
    snprintf(output, sizeof(output), "%s/output", dir);
    FILE* f = fopen(source, "wb"); assert(f);
    for (unsigned i = 0; i < size; i++) assert(fputc((i * 73 + 19) & 255, f) != EOF);
    assert(!fclose(f));
    uint8_t key[32], wrong[32], id[16], hash[32];
    assert(RAND_bytes(key, 32) == 1 && RAND_bytes(wrong, 32) == 1 && RAND_bytes(id, 16) == 1);
    struct attachment_info info = { .kind = ATTACHMENT_FILE, .name = "секретный-документ.bin" }, result;
    struct file_io_temp temporary = {0};
    char type[32];
    assert(!encrypted_file_write(source, cipher, &temporary, key, (const uint8_t[32]){42}, id, &info, "application/octet-stream"));
    assert(file_io_remove_orphan(temporary.temporary) == 0);
    assert(!file_io_temp_publish(&temporary, source, cipher)); file_io_temp_discard(&temporary);
    assert(!ma_sha256_file(cipher, hash));
    assert(!encrypted_file_read(cipher, output, &temporary, key, (const uint8_t[32]){42}, id, hash, &result, type));
    assert(ma_file_size(output) < 0 && file_io_remove_orphan(temporary.temporary) == 0);
    assert(!file_io_temp_publish(&temporary, cipher, output)); file_io_temp_discard(&temporary);
    assert(!strcmp(info.name, result.name) && !strcmp(type, "application/octet-stream"));
    f = fopen(output, "rb"); assert(f);
    for (unsigned i = 0; i < size; i++) assert(fgetc(f) == (int)((i * 73 + 19) & 255));
    assert(fgetc(f) == EOF); assert(!fclose(f)); assert(!remove(output));
    assert(encrypted_file_read(cipher, output, &temporary, wrong, (const uint8_t[32]){42}, id, hash, &result, type) < 0);
    assert(ma_file_size(output) < 0);
    assert(encrypted_file_read(cipher, output, &temporary, key, (const uint8_t[32]){43}, id, hash, &result, type) < 0);
    id[0] ^= 1;
    assert(encrypted_file_read(cipher, output, &temporary, key, (const uint8_t[32]){42}, id, hash, &result, type) < 0); id[0] ^= 1;
    /* Подмена metadata даже с заново вычисленным внешним hash не проходит CCM. */
    f = fopen(cipher, "rb+"); assert(f && !fseek(f, 70, SEEK_SET));
    int c = fgetc(f); assert(c != EOF && !fseek(f, 70, SEEK_SET) && fputc(c ^ 1, f) != EOF && !fclose(f));
    assert(encrypted_file_read(cipher, output, &temporary, key, (const uint8_t[32]){42}, id, hash, &result, type) < 0);
    assert(!ma_sha256_file(cipher, hash));
    assert(encrypted_file_read(cipher, output, &temporary, key, (const uint8_t[32]){42}, id, hash, &result, type) < 0 && ma_file_size(output) < 0);
    assert(!encrypted_file_write(source, cipher, &temporary, key, (const uint8_t[32]){42}, id, &info, "application/octet-stream"));
    assert(!file_io_temp_publish(&temporary, source, cipher)); file_io_temp_discard(&temporary);
    /* Обрезанный контейнер и лишние байты отвергаются независимо от manifest hash. */
    f = fopen(cipher, "ab"); assert(f && fputc(0, f) != EOF && !fclose(f));
    assert(!ma_sha256_file(cipher, hash));
    assert(encrypted_file_read(cipher, output, &temporary, key, (const uint8_t[32]){42}, id, hash, &result, type) < 0 && ma_file_size(output) < 0);
    printf("PASS: encrypted container size=%u, metadata, identity, key and integrity checks\n", size);
}

/* Оба API отвергают разные имена одного файла, не меняя содержимое и не удаляя alias. */
static void aliases(const char* dir) {
    char source[768], cipher[768], target[768];
    snprintf(source, sizeof(source), "%s/source", dir); snprintf(cipher, sizeof(cipher), "%s/cipher", dir);
    uint8_t key[32] = {1}, id[16] = {2}, hash[32], before[32];
    struct attachment_info info = {.kind = ATTACHMENT_FILE, .name = "example.bin"}, result;
    struct file_io_temp temporary = {0};
    char type[32];
    assert(!encrypted_file_write(source, cipher, &temporary, key, (const uint8_t[32]){42}, id, &info, "application/octet-stream"));
    assert(!file_io_temp_publish(&temporary, source, cipher));
    for (int decrypt = 0; decrypt < 2; decrypt++) {
        const char* input = decrypt ? cipher : source;
        assert(!ma_sha256_file(input, before));
        for (int kind = 0; kind < 4; kind++) {
            if (!kind) snprintf(target, sizeof(target), "%s", input);
            else if (kind == 1) snprintf(target, sizeof(target), "%s/./%s", dir, decrypt ? "cipher" : "source");
            else {
#ifndef _WIN32
                snprintf(target, sizeof(target), "%s/alias", dir);
                assert(kind == 2 ? !link(input, target) : !symlink(input, target));
#else
                continue;
#endif
            }
            int error = decrypt ? encrypted_file_read(input, target, &temporary, key, (const uint8_t[32]){42}, id, before, &result, type)
                                : encrypted_file_write(input, target, &temporary, key, (const uint8_t[32]){42}, id, &info, "application/octet-stream");
            assert(error < 0 && !temporary.file && !temporary.temporary[0]);
            hash_same(input, before); hash_same(target, before);
            if (kind >= 2) assert(!remove(target));
        }
    }
    /* Активный output чужой операции не теряется даже при ошибочных аргументах. */
    snprintf(target, sizeof(target), "%s/output", dir);
    assert(!file_io_temp_open(&temporary, target, 0));
    assert(encrypted_file_write(source, target, &temporary, key, (const uint8_t[32]){42}, id, &info, "application/octet-stream") < 0);
    assert(encrypted_file_read(cipher, target, &temporary, key, (const uint8_t[32]){42}, id, before, &result, type) < 0);
    assert(temporary.file && temporary.temporary[0] && file_io_remove_orphan(temporary.temporary) == 0);
    file_io_temp_discard(&temporary);
    /* Между подготовкой и publish цель стала alias исходника: второй контроль сохраняет его. */
#ifndef _WIN32
    assert(!ma_sha256_file(source, before));
    assert(!encrypted_file_write(source, target, &temporary, key, (const uint8_t[32]){42}, id, &info, "application/octet-stream"));
    assert(!link(source, target));
    assert(file_io_temp_publish(&temporary, source, target) < 0 && temporary.temporary[0]);
    file_io_temp_discard(&temporary); hash_same(source, before); hash_same(target, before); assert(!remove(target));
#endif
    assert(!ma_sha256_file(cipher, hash));
    printf("PASS: encrypt/decrypt aliases, active output and alias before publication\n");
}

#ifdef __linux__
static void temp_count(void* arg, const char* path, int64_t size) {
    (void)size; if (strstr(path, ".tmp.")) (*(unsigned*)arg)++;
}
/* Реальные файловые операции и CCM; каждый отказ сохраняет source и прежний target до rename. */
static void failures(const char* dir) {
    char source[768], cipher[768], target[768];
    snprintf(source, sizeof(source), "%s/source", dir); snprintf(cipher, sizeof(cipher), "%s/cipher", dir);
    snprintf(target, sizeof(target), "%s/target", dir);
    uint8_t key[32] = {1}, id[16] = {2}, hash[32], before[32], previous[32];
    struct attachment_info info = {.kind = ATTACHMENT_FILE, .name = "example.bin"};
    struct file_io_temp output = {0};
    assert(!ma_sha256_file(cipher, hash));
    for (int decrypt = 0; decrypt < 2; decrypt++) for (enum fault test = F_OPEN; test <= F_RANDOM; test++) {
        if (decrypt && test == F_SEEK) continue;
        const char* input = decrypt ? cipher : source;
        assert(!ma_sha256_file(input, before));
        FILE* file = fopen(target, "wb"); assert(file && fputs("previous", file) >= 0 && !fclose(file));
        assert(!ma_sha256_file(target, previous));
        struct attachment_info result, unchanged; memset(&result, 0xa5, sizeof(result)); unchanged = result;
        char type[32], old_type[32]; memset(type, 0xa5, sizeof(type)); memcpy(old_type, type, sizeof(type));
        unsigned fds = count_fds();
        fault = test; injected = 0; fault_after = decrypt && test == F_INPUT_CLOSE ? 2 : 1;
        int error = decrypt ? encrypted_file_read(input, target, &output, key, (const uint8_t[32]){42}, id, hash, &result, type)
                            : encrypted_file_write(input, target, &output, key, (const uint8_t[32]){42}, id, &info, "application/octet-stream");
        int prepared = !error;
        if (prepared) error = file_io_temp_publish(&output, input, target);
        fault = F_NONE;
        assert(error < 0 && injected);
        if (decrypt && !prepared) assert(!memcmp(&result, &unchanged, sizeof(result)) && !memcmp(type, old_type, sizeof(type)));
        if (test == F_DIRECTORY) assert(!output.temporary[0]);
        file_io_temp_discard(&output);
        assert(count_fds() == fds); hash_same(input, before);
        if (test != F_DIRECTORY) hash_same(target, previous);
        unsigned temps = 0; assert(!ma_dir_walk(dir, temp_count, &temps) && !temps);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS encrypted I/O failure decrypt=%d phase=%d installed=%d", decrypt, test, test == F_DIRECTORY);
    }
    assert(!remove(target));
}
#endif

/* Writer обязан отвергать всё, что отвергает reader; wire-подмены здесь имеют правильные CCM и внешний hash. */
static void mime_contract(const char* dir) {
    unsigned accepted = 0;
    for (unsigned byte = 0; byte < 256; byte++) {
        char type[] = {(char)byte, 'x', 0};
        int valid = byte >= 33 && byte <= 126 && byte != '"' && byte != '\\';
        assert((encrypted_file_validate_type(type) == 0) == valid); accepted += valid;
    }
    assert(accepted == 92 && encrypted_file_validate_type(NULL) < 0);
    char maximum[32], too_long[33], unterminated[32];
    memset(maximum, 'a', 31); maximum[31] = 0;
    memset(too_long, 'a', 32); too_long[32] = 0; memset(unterminated, 'a', sizeof(unterminated));
    const char* bad[] = {NULL, "", "text/plain\n", "text/\"plain", "text/\\plain", "text/\xc3\xa9", too_long, unterminated};
    const char* good[] = {"a", "application/octet-stream", maximum};
    char source[768], cipher[768], target[768];
    snprintf(source, sizeof(source), "%s/mime-source", dir); snprintf(cipher, sizeof(cipher), "%s/mime-cipher", dir);
    snprintf(target, sizeof(target), "%s/mime-target", dir);
    const char* paths[] = {source, cipher, target};
    for (unsigned i = 0; i < 3; i++) {
        FILE* file = fopen(paths[i], "wb"); assert(file && fputs("previous", file) >= 0 && !fclose(file));
    }
    uint8_t key[32] = {1}, id[16] = {2}, source_hash[32], cipher_hash[32], target_hash[32];
    assert(!ma_sha256_file(source, source_hash) && !ma_sha256_file(cipher, cipher_hash) && !ma_sha256_file(target, target_hash));
    struct attachment_info info = {.kind = ATTACHMENT_FILE, .name = "type.bin"};
    struct file_io_temp output = {0};
    unsigned char owner_before[sizeof(output)]; memcpy(owner_before, &output, sizeof(output));
#ifdef __linux__
    unsigned fds = count_fds();
#endif
    debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_WARN);
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(encrypted_file_write(source, cipher, &output, key, (const uint8_t[32]){42}, id, &info, bad[i]) < 0);
        assert(!memcmp(owner_before, &output, sizeof(output)));
        hash_same(source, source_hash); hash_same(cipher, cipher_hash); hash_same(target, target_hash);
    }
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        id[1] = (uint8_t)i;
        assert(!encrypted_file_write(source, cipher, &output, key, (const uint8_t[32]){42}, id, &info, good[i]));
        assert(!file_io_temp_publish(&output, source, cipher));
        assert(!ma_sha256_file(cipher, cipher_hash));
        struct attachment_info result; char type[32];
        assert(!encrypted_file_read(cipher, target, &output, key, (const uint8_t[32]){42}, id, cipher_hash, &result, type));
        assert(!strcmp(type, good[i]) && !strcmp(result.name, info.name));
        hash_same(output.temporary, source_hash); file_io_temp_discard(&output);
        hash_same(source, source_hash); hash_same(target, target_hash);
    }
    uint8_t packet[1024], meta[32 + ATTACHMENT_WIRE_MAX] = {0}, encrypted[sizeof(meta) + 16], nonce[13];
    FILE* file = fopen(cipher, "rb"); assert(file);
    size_t bytes = fread(packet, 1, sizeof(packet), file); assert(bytes < sizeof(packet) && feof(file) && !fclose(file));
    assert(bytes >= 66 && !memcmp(packet, "UTENC002", 8));
    size_t old_size = ((size_t)packet[32] << 8) | packet[33];
    assert(old_size <= bytes - 66);
    int n = attachment_encode(&info, meta + 32, ATTACHMENT_WIRE_MAX); assert(n > 0);
    media_cipher_nonce((const uint8_t[32]){42}, id, UINT64_MAX, nonce);
    for (unsigned i = 1; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memset(meta, 0, 32); memcpy(meta, bad[i], strnlen(bad[i], 32));
        size_t encrypted_size = 0;
        assert(!media_ccm_encrypt(key, nonce, meta, (size_t)n + 32, encrypted, &encrypted_size) && encrypted_size == old_size);
        memcpy(packet + 66, encrypted, encrypted_size);
        file = fopen(cipher, "wb"); assert(file && fwrite(packet, 1, bytes, file) == bytes && !fclose(file));
        assert(!ma_sha256_file(cipher, cipher_hash));
        struct attachment_info result; memset(&result, 0xa5, sizeof(result));
        unsigned char before[sizeof(result)]; memcpy(before, &result, sizeof(result));
        char type[32], previous[32]; memset(type, 0xa5, sizeof(type)); memcpy(previous, type, sizeof(type));
        assert(encrypted_file_read(cipher, target, &output, key, (const uint8_t[32]){42}, id, cipher_hash, &result, type) < 0);
        assert(!memcmp(before, &result, sizeof(result)) && !memcmp(previous, type, sizeof(type)));
        assert(!output.file && !output.temporary[0]);
        hash_same(source, source_hash); hash_same(target, target_hash); hash_same(cipher, cipher_hash);
    }
#ifdef __linux__
    unsigned temps = 0; assert(count_fds() == fds && !ma_dir_walk(dir, temp_count, &temps) && !temps);
#endif
    debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_ERROR);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS MIME: all256 bytes,92 accepted;8 early writer refusals,3 roundtrips,7 authenticated bad metadata");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    char dir[512] = "/tmp/utun_encrypted_file_XXXXXX"; assert(!test_mkdtemp(dir));
    const unsigned sizes[] = { 0, 1, 32767, 32768, 32769, 98321 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) roundtrip(dir, sizes[i]);
    aliases(dir);
#ifdef __linux__
    failures(dir);
#endif
    mime_contract(dir);
    return 0;
}
