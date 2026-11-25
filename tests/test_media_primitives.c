/* Регрессии сохранности файлов, больших размеров, ошибок RNG/обхода и владения async задачами. */
#include "../lib/platform_compat.h"
#include "../lib/file_io.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "../src/media_async/media_async.h"
#include "../src/media_delivery/media_index.h"
#include "../src/member_files/worker.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#endif

static char root[256], source[512], destination[512];
static unsigned visits;

#ifdef __linux__
enum fault { NONE, READ, WRITE, FLUSH, SOURCE_CLOSE, OUTPUT_CLOSE, FDOPEN, PUBLISH, OPEN_DIR, READ_DIR, CLOSE_DIR, SEEK, THREAD,
             SYNC_FILE, SYNC_DIRECTORY };
static enum fault fault;
static int injected, read_failed, rng_after = -1, allocation_after = -1;
static FILE *input_file, *output_file;
static pthread_barrier_t *post_barrier;
static pthread_barrier_t *temp_barrier;
extern FILE *__real_fopen(const char*, const char*);
extern FILE *__real_fdopen(int, const char*);
extern size_t __real_fread(void*, size_t, size_t, FILE*);
#ifdef __GLIBC__
extern size_t __real___fread_chk(void*, size_t, size_t, size_t, FILE*);
#endif
extern size_t __real_fwrite(const void*, size_t, size_t, FILE*);
extern int __real_ferror(FILE*), __real_fflush(FILE*), __real_fclose(FILE*);
extern int __real_rename(const char*, const char*), __real_RAND_bytes(unsigned char*, int);
extern int __real_fseeko(FILE*, off_t, int);
extern int __real_fsync(int);
extern DIR *__real_opendir(const char*);
extern struct dirent *__real_readdir(DIR*);
extern int __real_closedir(DIR*);
extern int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern void *__real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern void __real_uasync_post_reserved(struct UASYNC*, struct posted_task*);

static int inject(enum fault value) {
    if (fault != value || injected) return 0;
    injected = 1; errno = EIO; return 1;
}
FILE *__wrap_fopen(const char *path, const char *mode) {
    FILE *file = __real_fopen(path, mode);
    if (!strcmp(mode, "rb")) input_file = file;
    return file;
}
FILE *__wrap_fdopen(int fd, const char *mode) {
    if (inject(FDOPEN)) return NULL;
    FILE *file = __real_fdopen(fd, mode);
    if (!strcmp(mode, "wb+")) output_file = file;
    pthread_barrier_t *barrier = __atomic_load_n(&temp_barrier, __ATOMIC_ACQUIRE);
    if (barrier) { pthread_barrier_wait(barrier); pthread_barrier_wait(barrier); }
    return file;
}
size_t __wrap_fread(void *data, size_t size, size_t count, FILE *file) {
    if (file == input_file && inject(READ)) { read_failed = 1; return 0; }
    return __real_fread(data, size, count, file);
}
/* _FORTIFY_SOURCE при -Og вызывает checked-вариант; ошибка должна внедряться и в sanitizer-сборке. */
#ifdef __GLIBC__
size_t __wrap___fread_chk(void *data, size_t capacity, size_t size, size_t count, FILE *file) {
    if (file == input_file && inject(READ)) { read_failed = 1; return 0; }
    return __real___fread_chk(data, capacity, size, count, file);
}
#endif
int __wrap_ferror(FILE *file) { return file == input_file && read_failed ? 1 : __real_ferror(file); }
size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *file) {
    return file == output_file && inject(WRITE) ? 0 : __real_fwrite(data, size, count, file);
}
int __wrap_fflush(FILE *file) { return file == output_file && inject(FLUSH) ? EOF : __real_fflush(file); }
int __wrap_fclose(FILE *file) {
    int failed = (file == input_file && inject(SOURCE_CLOSE)) || (file == output_file && inject(OUTPUT_CLOSE));
    if (file == input_file) { input_file = NULL; read_failed = 0; }
    if (file == output_file) output_file = NULL;
    int result = __real_fclose(file);
    if (failed) { errno = EIO; return EOF; }
    return result;
}
int __wrap_rename(const char *from, const char *to) { return inject(PUBLISH) ? -1 : __real_rename(from, to); }
int __wrap_fsync(int fd) {
    struct stat data; assert(!fstat(fd, &data));
    if (inject(S_ISDIR(data.st_mode) ? SYNC_DIRECTORY : SYNC_FILE)) return -1;
    return __real_fsync(fd);
}
int __wrap_RAND_bytes(unsigned char *out, int size) {
    if (rng_after == 0) { injected = 1; return 0; }
    if (rng_after > 0) rng_after--;
    return __real_RAND_bytes(out, size);
}
int __wrap_fseeko(FILE *file, off_t offset, int whence) {
    return inject(SEEK) ? -1 : __real_fseeko(file, offset, whence);
}
DIR *__wrap_opendir(const char *path) { return inject(OPEN_DIR) ? NULL : __real_opendir(path); }
struct dirent *__wrap_readdir(DIR *dir) {
    if (visits && inject(READ_DIR)) return NULL;
    return __real_readdir(dir);
}
int __wrap_closedir(DIR *dir) {
    int result = __real_closedir(dir);
    if (inject(CLOSE_DIR)) return -1;
    return result;
}
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void* (*fn)(void*), void *arg) {
    return inject(THREAD) ? EAGAIN : __real_pthread_create(thread, attr, fn, arg);
}
void *__wrap_u_calloc_impl(uint32_t count, uint32_t size, const char *where) {
    if (!allocation_after) { injected = 1; return NULL; }
    if (allocation_after > 0) allocation_after--;
    return __real_u_calloc_impl(count, size, where);
}
void __wrap_uasync_post_reserved(struct UASYNC *ua, struct posted_task *task) {
    __real_uasync_post_reserved(ua, task);
    pthread_barrier_t *barrier = __atomic_load_n(&post_barrier, __ATOMIC_ACQUIRE);
    if (barrier) pthread_barrier_wait(barrier);
}
#endif

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text)); assert(!fclose(file));
}
static void check_text(const char *path, const char *text) {
    FILE *file = file_io_open_read(path); assert(file);
    char bytes[64] = {0}; size_t size = fread(bytes, 1, sizeof(bytes), file); assert(!fclose(file));
    assert(size == strlen(text) && !memcmp(bytes, text, size));
}
static void visit(void *arg, const char *path, int64_t size) {
    (void)arg; assert(size >= 0 && !strstr(path, ".tmp.")); visits++;
}
static void remove_file(void *arg, const char *path, int64_t size) {
    (void)arg; (void)size; assert(!remove(path));
}

static void copy_tests(void) {
    write_text(source, "source-payload"); write_text(destination, "previous");
    assert(!ma_copy_file(source, source)); check_text(source, "source-payload");
    char alias[512]; snprintf(alias, sizeof(alias), "%s/./source", root);
    assert(!ma_copy_file(source, alias)); check_text(source, "source-payload");
    snprintf(alias, sizeof(alias), "%s/missing", root);
    assert(ma_copy_file(alias, alias) == -1);
    snprintf(alias, sizeof(alias), "%s/missing/output", root); assert(ma_copy_file(source, alias) == -1);
#ifndef _WIN32
    snprintf(alias, sizeof(alias), "%s/alias", root);
    assert(!link(source, alias)); assert(!ma_copy_file(source, alias)); assert(!remove(alias));
    assert(!symlink("source", alias)); assert(ma_copy_file(source, alias) == -1); assert(!remove(alias));
    assert(ma_copy_file(source, "/dev/full") == -1);
#endif
    check_text(source, "source-payload"); check_text(destination, "previous");
#ifdef __linux__
    for (enum fault test = READ; test <= PUBLISH; test++) {
        fault = test; injected = 0; read_failed = 0;
        int error = ma_copy_file(source, destination); fault = NONE;
        assert(error == -1 && injected);
        check_text(source, "source-payload"); check_text(destination, "previous");
        visits = 0; assert(!ma_dir_walk(root, visit, NULL) && visits == 2);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS copy injected failure=%d: both files unchanged, no temp", test);
    }
#endif
    assert(!ma_copy_file(source, destination)); check_text(destination, "source-payload");
    write_text(source, ""); assert(!ma_copy_file(source, destination) && ma_file_size(destination) == 0);
    write_text(source, "source-payload");
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS copy aliases, links, missing source, successful and empty file");
}

/* Ошибки чтения/close и неверные аргументы не оставляют правдоподобный hash/signature. */
static void temporary_tests(void) {
    struct file_io_temp outputs[3] = {0};
    char paths[3][FILE_IO_PATH_MAX], stale[FILE_IO_PATH_MAX];
    for (unsigned i = 0; i < 3; i++) {
        assert(!file_io_temp_open(&outputs[i], destination, 0));
        snprintf(paths[i], sizeof(paths[i]), "%s", outputs[i].temporary);
        assert(file_io_remove_orphan(paths[i]) == 0);
        assert(fputs("owned", outputs[i].file) >= 0 && !file_io_temp_finish(&outputs[i]));
        assert(file_io_remove_orphan(paths[i]) == 0); check_text(paths[i], "owned");
    }
    snprintf(stale, sizeof(stale), "%s.tmp.unowned", destination); write_text(stale, "stale");
    assert(file_io_remove_orphan(stale) == 1 && file_io_remove_orphan(stale) == 0);
    file_io_temp_discard(&outputs[1]); file_io_temp_discard(&outputs[1]);
    assert(ma_file_size(paths[1]) == -1);
    write_text(paths[1], "unowned"); assert(file_io_remove_orphan(paths[1]) == 1);
    assert(!file_io_temp_publish(&outputs[0], source, destination)); check_text(destination, "owned");
    write_text(paths[0], "unowned"); assert(file_io_remove_orphan(paths[0]) == 1);
    assert(file_io_remove_orphan(paths[2]) == 0);
    file_io_temp_discard(&outputs[2]);
#ifdef __linux__
    struct file_io_temp failed = {0};
    fault = FDOPEN; injected = 0;
    assert(file_io_temp_open(&failed, destination, 0) == -1 && injected); fault = NONE;
    assert(failed.temporary[0] && file_io_remove_orphan(failed.temporary) == 0);
    file_io_temp_discard(&failed);
    assert(!file_io_temp_open(&failed, destination, 0) && !file_io_temp_finish(&failed));
    fault = PUBLISH; injected = 0;
    assert(file_io_temp_publish(&failed, source, destination) == -1 && injected); fault = NONE;
    assert(file_io_remove_orphan(failed.temporary) == 0); file_io_temp_discard(&failed);
#endif
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS active temp ownership: open/finished, multiple generations, publish/discard and failures");
}

/* Ошибка до rename сохраняет прежний файл; отказ directory sync уже не отменяет замену. */
static void durable_tests(void) {
    struct file_io_temp output = {0};
    write_text(destination, "previous");
    assert(!file_io_temp_open(&output, destination, FILE_IO_DURABLE));
    assert(fputs("durable", output.file) >= 0 && !file_io_temp_finish(&output));
    check_text(destination, "previous"); assert(file_io_remove_orphan(output.temporary) == 0);
    assert(!file_io_temp_publish(&output, source, destination) && !output.temporary[0]);
    file_io_temp_discard(&output); check_text(destination, "durable");
    assert(!file_io_sync_file(destination) && !file_io_sync_directory("relative-name"));
#ifndef _WIN32
    assert(!file_io_sync_directory("/file"));
#endif
#ifdef __linux__
    assert(!file_io_temp_open(&output, destination, FILE_IO_DURABLE));
    assert(fputs("failed", output.file) >= 0);
    fault = SYNC_FILE; injected = 0;
    assert(file_io_temp_finish(&output) == -1 && injected && !output.file && output.temporary[0]); fault = NONE;
    file_io_temp_discard(&output); check_text(destination, "durable");
    assert(!file_io_temp_open(&output, destination, FILE_IO_DURABLE));
    assert(fputs("installed", output.file) >= 0 && !file_io_temp_finish(&output));
    fault = SYNC_DIRECTORY; injected = 0;
    assert(file_io_temp_publish(&output, source, destination) == -1 && injected && !output.temporary[0]); fault = NONE;
    file_io_temp_discard(&output); check_text(destination, "installed");
    assert(!file_io_temp_open(&output, destination, 0));
    assert(fputs("ordinary", output.file) >= 0);
    fault = SYNC_FILE; injected = 0;
    assert(!file_io_temp_finish(&output) && !injected); fault = SYNC_DIRECTORY;
    assert(!file_io_temp_publish(&output, source, destination) && !injected); fault = NONE;
    file_io_temp_discard(&output); check_text(destination, "ordinary");
#endif
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS durable temp: sync-before-close, rename commit point, directory failure and ordinary mode");
}

static void crypto_tests(void) {
    uint8_t output[64], zero[64] = {0};
    memset(output, 0xa5, sizeof(output));
    assert(ma_sha256_file(NULL, output) == -1 && !memcmp(output, zero, 32));
    memset(output, 0xa5, sizeof(output));
    assert(ma_sign_block(NULL, NULL, 0, output) == -1 && !memcmp(output, zero, sizeof(output)));
#ifdef __linux__
    const enum fault cases[] = {READ, SOURCE_CLOSE};
    for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        fault = cases[n]; injected = 0; memset(output, 0xa5, sizeof(output));
        int error = ma_sha256_file(source, output); fault = NONE;
        assert(error == -1 && injected && !memcmp(output, zero, 32));
    }
#endif
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS digest/signature failures clear output");
}

static void size_and_walk_tests(void) {
    char path[512]; snprintf(path, sizeof(path), "%s/large", root);
    const int64_t sizes[] = {INT_MAX, (int64_t)INT_MAX + 1, INT64_C(4294967296) + 17};
    for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++) {
        FILE *file = fopen(path, "wb"); assert(file);
#ifdef _WIN32
        assert(!_fseeki64(file, sizes[n] - 1, SEEK_SET));
#else
        assert(!fseeko(file, (off_t)(sizes[n] - 1), SEEK_SET));
#endif
        assert(fputc(0, file) != EOF && !fclose(file)); assert(ma_file_size(path) == sizes[n]);
    }
    visits = 0; assert(!ma_dir_walk(root, visit, NULL) && visits == 3);
    assert(ma_file_size(root) == -1 && ma_dir_walk(source, visit, NULL) == -1);
    snprintf(path, sizeof(path), "%s/missing", root); assert(ma_dir_walk(path, visit, NULL) == -1);
#ifndef _WIN32
    snprintf(path, sizeof(path), "%s/cycle", root); assert(!symlink(".", path));
    visits = 0; assert(!ma_dir_walk(root, visit, NULL) && visits == 3);
    visits = 0; assert(!ma_dir_walk(path, visit, NULL) && !visits); assert(!remove(path));
#endif
#ifdef __linux__
    for (enum fault test = OPEN_DIR; test <= CLOSE_DIR; test++) {
        fault = test; visits = 0; injected = 0;
        int error = ma_dir_walk(root, visit, NULL); fault = NONE;
        assert(error == -1 && injected);
        if (test == READ_DIR) assert(visits == 1);
    }
    char deep[1024]; snprintf(deep, sizeof(deep), "%s", root);
    for (unsigned n = 0; n <= MA_WALK_MAX_DEPTH; n++) { strcat(deep, "/d"); assert(!utun_mkdir(deep, 0700)); }
    assert(ma_dir_walk(root, visit, NULL) == -1);
    for (unsigned n = 0; n <= MA_WALK_MAX_DEPTH; n++) { assert(!rmdir(deep)); *strrchr(deep, '/') = 0; }
#endif
    char oversized[FILE_IO_PATH_MAX + 1]; memset(oversized, 'x', sizeof(oversized)); oversized[sizeof(oversized) - 1] = 0;
    assert(ma_dir_walk(oversized, visit, NULL) == -1);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS 64-bit sparse sizes, link cycles, directory failures and depth/path bounds");
}

struct registration { unsigned calls; int error; };
static void registered(void *arg, int error, const struct media_index_result *result) {
    struct registration *state = arg; state->calls++; state->error = error;
    assert(error ? result == NULL : result && result->file_size == 14);
}
static void wait_registration(struct UASYNC *ua, struct registration *state) {
    uint64_t deadline = get_time_tb() + 50000;
    while (!state->calls && get_time_tb() < deadline) uasync_poll(ua, 10);
    assert(state->calls == 1);
}
static int row_count(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL; assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM media_files", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW); int count = sqlite3_column_int(stmt, 0); sqlite3_finalize(stmt); return count;
}
static void registration_tests(struct UASYNC *ua) {
    sqlite3 *db = NULL; assert(sqlite3_open(":memory:", &db) == SQLITE_OK && !media_index_init(db));
    struct media_async *owner = media_async_create(); assert(owner); uint8_t key[32]; memset(key, 1, sizeof(key));
#ifdef __linux__
    for (int n = 0; n < 3; n++) {
        struct registration state = {0}; injected = 0;
        rng_after = n < 2 ? n : -1; fault = n == 2 ? SEEK : NONE;
        media_index_register_async(owner, ua, db, (const uint8_t[32]){1}, key, "123", source, source, 0, root, registered, &state);
        wait_registration(ua, &state); rng_after = -1; fault = NONE;
        assert(state.error && injected && !row_count(db));
    }
    uint8_t uuid[16], zero[16] = {0}; memset(uuid, 0xa5, sizeof(uuid)); rng_after = 0;
    assert(media_index_generate_uuid(uuid) == -1 && !memcmp(uuid, zero, sizeof(uuid))); rng_after = -1;
#endif
    struct registration state = {0};
    media_index_register_async(owner, ua, db, (const uint8_t[32]){1}, key, "123", source, source, 1, root, registered, &state);
    wait_registration(ua, &state); assert(!state.error && row_count(db) == 1);
    media_async_destroy(owner); assert(sqlite3_close(db) == SQLITE_OK);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS RNG/seek errors reach callback without DB rows; same-path registration succeeds");
}

#ifdef __linux__
struct temp_job { struct file_io_temp output; pthread_barrier_t barrier; int publish; };
static void* temp_worker(void* arg) {
    struct temp_job* job = arg;
    assert(!file_io_temp_open(&job->output, destination, 0));
    assert(fputs("worker", job->output.file) >= 0 && !file_io_temp_finish(&job->output));
    pthread_barrier_wait(&job->barrier); pthread_barrier_wait(&job->barrier);
    if (job->publish) assert(!file_io_temp_publish(&job->output, source, destination));
    else file_io_temp_discard(&job->output);
    return NULL;
}
static void temporary_thread_tests(void) {
    for (int publish = 0; publish < 2; publish++) {
        struct temp_job job = {.publish = publish}; pthread_t thread;
        assert(!pthread_barrier_init(&job.barrier, NULL, 2));
        __atomic_store_n(&temp_barrier, &job.barrier, __ATOMIC_RELEASE);
        assert(!pthread_create(&thread, NULL, temp_worker, &job));
        pthread_barrier_wait(&job.barrier); /* fdopen ещё не вернул управление temp_open. */
        char path[FILE_IO_PATH_MAX]; snprintf(path, sizeof(path), "%s", job.output.temporary);
        assert(ma_file_size(path) == 0 && file_io_remove_orphan(path) == 0);
        pthread_barrier_wait(&job.barrier);
        pthread_barrier_wait(&job.barrier); /* Worker закрыл FILE, но сохраняет владение. */
        assert(file_io_remove_orphan(path) == 0); check_text(path, "worker");
        __atomic_store_n(&temp_barrier, NULL, __ATOMIC_RELEASE);
        pthread_barrier_wait(&job.barrier);
        assert(!pthread_join(thread, NULL) && !pthread_barrier_destroy(&job.barrier));
        assert(ma_file_size(path) == -1 && file_io_remove_orphan(path) == 0);
        write_text(path, "unowned"); assert(file_io_remove_orphan(path) == 1);
        if (publish) check_text(destination, "worker");
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS worker temp protected during open and after finish; publish/discard release ownership");
}

struct job {
    struct media_async *owner; struct UASYNC *ua;
    unsigned work, done; int error, restart, cancel_actions;
    pthread_t event_thread;
};
static void job_work(void *arg) { ((struct job*)arg)->work++; }
static void job_done(void *arg, int error) {
    struct job *job = arg; assert(pthread_equal(pthread_self(), job->event_thread));
    job->done++; job->error = error;
    if (job->restart && job->done == 1) media_async_submit(job->owner, job->ua, job_work, job, job_done, job);
    if (job->cancel_actions && job->done == 1) {
        media_async_destroy(job->owner); // Отклонённый повторный вход во время destroy.
        media_async_submit(job->owner, job->ua, job_work, job, job_done, job);
    }
}
static void engine_tests(struct UASYNC *ua) {
    size_t before = u_get_allocated_count();
    struct media_async *owner = media_async_create(); assert(owner);
    struct job jobs[MEDIA_ASYNC_MAX_TASKS + 1] = {0};
    pthread_barrier_t barrier; assert(!pthread_barrier_init(&barrier, NULL, MEDIA_ASYNC_MAX_TASKS + 1));
    __atomic_store_n(&post_barrier, &barrier, __ATOMIC_RELEASE);
    for (unsigned n = 0; n < MEDIA_ASYNC_MAX_TASKS; n++) {
        jobs[n].event_thread = pthread_self(); media_async_submit(owner, ua, job_work, &jobs[n], job_done, &jobs[n]);
    }
    pthread_barrier_wait(&barrier); __atomic_store_n(&post_barrier, NULL, __ATOMIC_RELEASE);
    struct job *rejected = &jobs[MEDIA_ASYNC_MAX_TASKS]; rejected->event_thread = pthread_self();
    media_async_submit(owner, ua, job_work, rejected, job_done, rejected);
    assert(rejected->done == 1 && rejected->error == -1 && !rejected->work);
    media_async_destroy(owner); uasync_poll(ua, 0); pthread_barrier_destroy(&barrier);
    for (unsigned n = 0; n < MEDIA_ASYNC_MAX_TASKS; n++)
        assert(jobs[n].work == 1 && jobs[n].done == 1 && jobs[n].error == MEDIA_ASYNC_CANCELLED);
    owner = media_async_create(); assert(owner);
    struct job state = {.owner = owner, .ua = ua, .event_thread = pthread_self()};
    media_async_submit(NULL, ua, job_work, &state, job_done, &state); assert(state.done == 1 && state.error == -1);
    media_async_submit(owner, ua, job_work, &state, NULL, &state); assert(state.done == 1);
    for (int n = 0; n < 3; n++) {
        state.done = 0; injected = 0; allocation_after = n < 2 ? n : -1; fault = n == 2 ? THREAD : NONE;
        media_async_submit(owner, ua, job_work, &state, job_done, &state); allocation_after = -1; fault = NONE;
        assert(injected && state.done == 1 && state.error == -1 && !state.work);
    }
    state.done = 0; state.restart = 1;
    media_async_submit(owner, ua, job_work, &state, job_done, &state);
    uint64_t deadline = get_time_tb() + 50000;
    while (state.done != 2 && get_time_tb() < deadline) uasync_poll(ua, 10);
    assert(state.done == 2 && state.work == 2 && !state.error); media_async_destroy(owner);
    owner = media_async_create(); assert(owner);
    state = (struct job){.owner = owner, .ua = ua, .cancel_actions = 1, .event_thread = pthread_self()};
    media_async_submit(owner, ua, job_work, &state, job_done, &state); media_async_destroy(owner);
    assert(state.work == 1 && state.done == 2 && state.error == MEDIA_ASYNC_CANCELLED);
    uasync_poll(ua, 0); assert(state.done == 2 && u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS 16 finished jobs retain slots; failures, reentrant submit, destroy and once-only cancellation");
}

struct batch_destroy {
    struct mf_worker *disk;
    struct media_async *media;
    unsigned calls;
};
static void destroy_in_batch(void *arg) {
    struct batch_destroy *state = arg;
    mf_worker_destroy(state->disk); media_async_destroy(state->media); state->calls++;
}

/* Барьер гарантирует очередь [destroy, completion] до poll, без зависимости от планировщика. */
static void worker_batch_tests(struct UASYNC *ua) {
    size_t before = u_get_allocated_count();
    for (int media = 0; media < 2; media++) {
        struct batch_destroy state = {0};
        struct job job = {.ua = ua, .event_thread = pthread_self()};
        if (media) { state.media = media_async_create(); assert(state.media); }
        else { state.disk = mf_worker_create(ua); assert(state.disk); }
        uasync_post(ua, destroy_in_batch, &state);
        pthread_barrier_t barrier; assert(!pthread_barrier_init(&barrier, NULL, 2));
        __atomic_store_n(&post_barrier, &barrier, __ATOMIC_RELEASE);
        if (media) media_async_submit(state.media, ua, job_work, &job, job_done, &job);
        else mf_worker_submit(state.disk, job_work, &job, job_done, &job);
        pthread_barrier_wait(&barrier); __atomic_store_n(&post_barrier, NULL, __ATOMIC_RELEASE);
        uasync_poll(ua, 0); uasync_poll(ua, 0);
        assert(state.calls == 1 && job.work == 1 && job.done == 1);
        assert(job.error == (media ? MEDIA_ASYNC_CANCELLED : 0));
        assert(!pthread_barrier_destroy(&barrier) && u_get_allocated_count() == before);
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS disk drain and media destroy cancel batched completions exactly once");
}
#endif

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_INFO);
    size_t before = u_get_allocated_count();
#ifdef _WIN32
    snprintf(root, sizeof(root), "media-primitives-%llu", (unsigned long long)get_time_us());
#else
    snprintf(root, sizeof(root), "/tmp/media-primitives-%llu", (unsigned long long)get_time_us());
#endif
    assert(!utun_mkdir(root, 0700)); snprintf(source, sizeof(source), "%s/source", root);
    snprintf(destination, sizeof(destination), "%s/destination", root);
    copy_tests(); temporary_tests(); durable_tests(); crypto_tests(); size_and_walk_tests();
    struct UASYNC *ua = uasync_create(); assert(ua);
    registration_tests(ua);
#ifdef __linux__
    temporary_thread_tests(); engine_tests(ua); worker_batch_tests(ua);
#endif
    uasync_destroy(ua, 0); assert(!ma_dir_walk(root, remove_file, NULL) && !rmdir(root));
    assert(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS media primitives regressions, no tracked allocations");
    return 0;
}
