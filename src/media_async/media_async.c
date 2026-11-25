// media_async.c — thread-per-task async engine + crypto/file primitives
#include "media_async.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/file_io.h"

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <dirent.h>
#endif

struct ma_thread_ctx;
struct media_async { struct ma_thread_ctx* jobs; int closing; };

struct ma_thread_ctx {
    struct media_async* owner;
    struct ma_thread_ctx* next;
    struct UASYNC* ua;
    pthread_t thread;
    struct posted_task* completion;
    ma_work_fn work;
    void* data;
    ma_done_fn done;
    void* arg;
};

static void ma_finish(struct ma_thread_ctx* job, int error) {
    struct ma_thread_ctx** p = &job->owner->jobs;
    while (*p && *p != job) p = &(*p)->next;
    if (*p) *p = job->next;
    pthread_join(job->thread, NULL);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "media task completed job=%p error=%d remaining=%d", (void*)job, error, job->owner->jobs != NULL);
    job->done(job->arg, error);
    u_free(job);
}

/* Completion приходит только после работы; ожидать незавершённый worker здесь нельзя. */
static void ma_completed(void* arg) {
    struct ma_thread_ctx* job = arg;
    job->completion = NULL;
    ma_finish(job, 0);
}

static void* ma_thread_entry(void* arg) {
    struct ma_thread_ctx* job = arg;
    job->work(job->data);
    uasync_post_reserved(job->ua, job->completion);
    return NULL;
}

struct media_async* media_async_create(void) {
    struct media_async* ma = u_calloc(1, sizeof(*ma));
    if (!ma) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media worker owner allocation failed");
    else DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "media_async: created");
    return ma;
}

void media_async_destroy(struct media_async* ma) {
    if (!ma) return;
    if (ma->closing) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media owner recursive destroy rejected"); return; }
    ma->closing = 1;
    unsigned cancelled = 0;
    while (ma->jobs) {
        struct ma_thread_ctx* job = ma->jobs;
        pthread_join(job->thread, NULL);
        uasync_cancel_post(job->ua, job->completion);
        ma->jobs = job->next;
        job->done(job->arg, MEDIA_ASYNC_CANCELLED);
        u_free(job); cancelled++;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "media_async: destroyed cancelled=%u", cancelled);
    u_free(ma);
}

void media_async_submit(struct media_async* ma, struct UASYNC* ua,
                        ma_work_fn work, void* data, ma_done_fn done, void* arg) {
    if (!ma || !ua || !work || !done) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media task rejected: invalid arguments ma=%p ua=%p work=%d done=%d",
                    (void*)ma, (void*)ua, work != NULL, done != NULL);
        if (done) done(arg, -1);
        return;
    }
    if (ma->closing) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media task rejected: owner stopping"); done(arg, MEDIA_ASYNC_CANCELLED); return; }
    unsigned active = 0;
    for (struct ma_thread_ctx* pending = ma->jobs; pending; pending = pending->next) active++;
    if (active >= MEDIA_ASYNC_MAX_TASKS) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media task limit reached active=%u", active); done(arg, -1); return;
    }
    struct ma_thread_ctx* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media task allocation failed"); done(arg, -1); return; }
    job->owner = ma; job->ua = ua; job->work = work; job->data = data; job->done = done; job->arg = arg;
    job->completion = u_calloc(1, sizeof(*job->completion));
    if (!job->completion) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media completion allocation failed"); u_free(job); done(arg, -1); return; }
    job->completion->callback = ma_completed;
    job->completion->arg = job;
    int rc = pthread_create(&job->thread, NULL, ma_thread_entry, job);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media task thread failed rc=%d", rc);
        u_free(job->completion); u_free(job); done(arg, -1); return;
    }
    job->next = ma->jobs; ma->jobs = job;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "media task accepted job=%p pending=%u", (void*)job, active + 1);
}

/* Синхронный SHA256; результат не публикуется при ошибке чтения/close. */
int ma_sha256_file(const char* path, uint8_t hash_out[32]) {
    if (!hash_out) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "SHA256 requires output"); return -1; }
    memset(hash_out, 0, 32);
    FILE* file = file_io_open_read(path);
    if (!file) return -1;
    SHA256_CTX ctx;
    int error = SHA256_Init(&ctx) != 1;
    uint8_t buffer[65536]; size_t count;
    while (!error && (count = fread(buffer, 1, sizeof(buffer), file)) > 0)
        if (SHA256_Update(&ctx, buffer, count) != 1) error = 1;
    if (ferror(file)) error = 1;
    if (fclose(file)) error = 1;
    if (!error && SHA256_Final(hash_out, &ctx) != 1) error = 1;
    if (error) {
        memset(hash_out, 0, 32);
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "SHA256 read/digest/close failed path=%s errno=%d", path, errno); return -1;
    }
    return 0;
}

/* Подпись публикуется только после успешного EVP_DigestSign. */
int ma_sign_block(const uint8_t* key, const uint8_t* data, size_t len, uint8_t signature[64]) {
    if (signature) memset(signature, 0, 64);
    if (!key || (!data && len) || !signature) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Ed25519 invalid key/data/output length=%zu", len); return -1;
    }
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, key, 32);
    if (!pkey) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Ed25519 key allocation failed"); return -1; }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Ed25519 context allocation failed"); EVP_PKEY_free(pkey); return -1; }
    uint8_t result[64]; size_t size = sizeof(result);
    int ok = EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey) == 1 && EVP_DigestSign(ctx, result, &size, data, len) == 1;
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(pkey);
    if (!ok || size != sizeof(result)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Ed25519 signing failed length=%zu", size); return -1;
    }
    memcpy(signature, result, sizeof(result));
    return 0;
}

/* Источник открыт до сравнения идентичности; назначение меняется только после полной записи и закрытия. */
int ma_copy_file(const char* source, const char* destination) {
    FILE* input = file_io_open_read(source);
    if (!input) return -1;
    struct file_io_temp output = {0};
    const char* phase = "validate paths";
    int status = file_io_paths(source, destination);
    uint64_t total = 0;
    if (status == 0) phase = "create temporary";
    if (status == 0 && file_io_temp_open(&output, destination, 0)) status = -1;
    if (status == 0) {
        uint8_t buffer[65536]; size_t count;
        while (1) {
            phase = "read source"; count = fread(buffer, 1, sizeof(buffer), input);
            if (ferror(input)) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "copy read failed path=%s errno=%d", source, errno); status = -1; break;
            }
            if (!count) break;
            phase = "write temporary";
            if (fwrite(buffer, 1, count, output.file) != count) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "copy write failed path=%s requested=%zu errno=%d", output.temporary, count, errno);
                status = -1; break;
            }
            total += count;
        }
    }
    if (fclose(input)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "copy source close failed path=%s errno=%d", source, errno);
        phase = "close source"; status = -1;
    }
    if (status == 0) phase = "flush/close temporary";
    if (status == 0 && file_io_temp_finish(&output)) status = -1;
    if (status == 0) phase = "publish";
    if (status == 0 && file_io_temp_publish(&output, source, destination)) status = -1;
    file_io_temp_discard(&output);
    if (status < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "copy failed phase=%s source=%s destination=%s bytes=%llu",
                    phase, source, destination ? destination : "(null)", (unsigned long long)total);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "copy complete source=%s destination=%s bytes=%llu same_file=%d",
                source, destination, (unsigned long long)total, status == 1);
    return 0;
}

int64_t ma_file_size(const char* path) {
    struct file_io_info info;
    int status = file_io_inspect(path, 1, &info);
    if (status == 1) { DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "file size missing path=%s", path); return -1; }
    if (status < 0) return -1;
    if (info.type != FILE_IO_REGULAR) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file size requires regular file path=%s type=%d", path, info.type); return -1;
    }
    return info.size;
}

int ma_uuid(uint8_t uuid_out[16]) {
    if (!uuid_out) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "UUID requires output"); return -1; }
    int rc = RAND_bytes(uuid_out, 16);
    if (rc != 1) {
        memset(uuid_out, 0, 16);
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "UUID RNG failed rc=%d", rc); return -1;
    }
    return 0;
}

struct ma_walk_ctx { ma_dir_walk_cb cb; void* arg; };
static int ma_walk_recursive(const char* directory, struct ma_walk_ctx* ctx, unsigned depth);

/* Одна запись: ссылки/устройства пропускаются, ошибки делают обход неполным. */
static int ma_walk_entry(const char* directory, const char* name, struct ma_walk_ctx* ctx, unsigned depth) {
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    size_t length = strlen(directory) + strlen(name) + 2;
    if (length > FILE_IO_PATH_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory entry path too long directory=%s name=%s", directory, name); return -1;
    }
    char* full = u_malloc((uint32_t)length);
    if (!full) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory entry allocation failed bytes=%zu", length); return -1; }
    snprintf(full, length, "%s/%s", directory, name);
    struct file_io_info info;
    if (file_io_inspect(full, 0, &info)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory entry inspection failed path=%s", full); u_free(full); return -1;
    }
    int status = 0;
    if (info.type == FILE_IO_DIRECTORY) status = ma_walk_recursive(full, ctx, depth + 1);
    if (info.type == FILE_IO_REGULAR) ctx->cb(ctx->arg, full, info.size);
    u_free(full); return status;
}

/* Ограниченный по глубине обход без перехода по конечным ссылкам. */
static int ma_walk_recursive(const char* directory, struct ma_walk_ctx* ctx, unsigned depth) {
    if (depth > MA_WALK_MAX_DEPTH) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory depth limit path=%s depth=%u", directory, depth); return -1;
    }
    struct file_io_info info;
    if (file_io_inspect(directory, 0, &info)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory inspection failed path=%s", directory); return -1;
    }
    if (info.type == FILE_IO_LINK) return 0;
    if (info.type != FILE_IO_DIRECTORY) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory walk requires directory path=%s type=%d", directory, info.type); return -1;
    }
    int status = 0;
#ifdef _WIN32
    struct ma_search { wchar_t pattern[FILE_IO_PATH_MAX]; char name[FILE_IO_PATH_MAX]; WIN32_FIND_DATAW data; };
    struct ma_search* search = u_calloc(1, sizeof(*search));
    if (!search) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory search allocation failed"); return -1; }
    if (file_io_wide(directory, search->pattern)) { u_free(search); return -1; }
    size_t length = wcslen(search->pattern);
    if (length + 3 > FILE_IO_PATH_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory search path too long path=%s", directory); u_free(search); return -1;
    }
    memcpy(search->pattern + length, L"/*", 3 * sizeof(wchar_t));
    HANDLE find = FindFirstFileW(search->pattern, &search->data);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        u_free(search);
        if (error == ERROR_FILE_NOT_FOUND) return 0; // Открытый выше каталог пуст.
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory open failed path=%s winerr=%lu", directory, error); return -1;
    }
    do {
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, search->data.cFileName, -1,
                                 search->name, sizeof(search->name), NULL, NULL)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory entry UTF-8 conversion failed path=%s winerr=%lu", directory, GetLastError());
            status = -1; break;
        }
        if (ma_walk_entry(directory, search->name, ctx, depth)) { status = -1; break; }
        if (!FindNextFileW(find, &search->data)) {
            DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory read failed path=%s winerr=%lu", directory, error); status = -1;
            }
            break;
        }
    } while (1);
    if (!FindClose(find)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory close failed path=%s winerr=%lu", directory, GetLastError()); status = -1;
    }
    u_free(search);
#else
    DIR* dir = opendir(directory);
    if (!dir) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory open failed path=%s errno=%d", directory, errno); return -1; }
    while (1) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (!entry) {
            if (errno) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory read failed path=%s errno=%d", directory, errno); status = -1; }
            break;
        }
        if (ma_walk_entry(directory, entry->d_name, ctx, depth)) { status = -1; break; }
    }
    if (closedir(dir)) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory close failed path=%s errno=%d", directory, errno); status = -1; }
#endif
    return status;
}

int ma_dir_walk(const char* directory, ma_dir_walk_cb callback, void* arg) {
    if (!directory || !*directory || strlen(directory) >= FILE_IO_PATH_MAX || !callback) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "directory walk invalid path/callback"); return -1;
    }
    struct ma_walk_ctx ctx = {.cb = callback, .arg = arg};
    int result = ma_walk_recursive(directory, &ctx, 0);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "directory walk finished path=%s result=%d", directory, result);
    return result;
}
