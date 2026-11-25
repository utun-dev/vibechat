/* Протокол и настоящее дисковое хранилище: атомарность, случайные диапазоны,
 * версии, квота, повторы, ссылки и закрытие дескрипторов через 10 секунд. */
#ifndef _WIN32
/* На Linux тоже воспроизводим пространство имён Win32 до подключения протокола. */
#define MF_REMOVE 0x00001000L
#endif
#include "../src/member_files/storage.h"
#include "../src/member_files/disk.h"
#include "../src/member_files/worker.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"
#include "../src/transport_layer/secure_channel.h"
#include <assert.h>
#include <openssl/sha.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#endif

static const char* storage_group_key(uint64_t group, char out[65]) {
    snprintf(out, 65, "%064llx", (unsigned long long)group);
    return out;
}

static int failures;
#define EXPECT(condition, name) do { if (!(condition)) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "FAIL: %s line=%d", name, __LINE__); failures++; } } while (0)
static struct mf_message q, r;
static uint64_t sequence;
static const uint8_t* fixture_key(uint64_t id, uint8_t ed[32]) {
    uint8_t seed[32] = {0}, x[32], xp[32];
    mf_put64(seed, id);
    for (unsigned attempt = 0; attempt < 256; attempt++) {
        seed[8] = (uint8_t)attempt;
        if (sc_identity_from_seed(seed, x, xp, ed) == SC_OK) return ed;
    }
    abort();
}

static void run_key(struct mf_storage* s, uint64_t group, const uint8_t peer[32], int command, int expected) {
    q.command = (uint8_t)command; q.kind = MF_REQUEST; q.request_id = ++sequence;
    mf_storage_execute(s, storage_group_key(group, (char[65]){0}), peer, &q, &r);
    EXPECT(r.status == expected, mf_status_text(r.status));
}

static void run(struct mf_storage* s, uint64_t group, uint64_t peer, int command, int expected) {
    run_key(s, group, fixture_key(peer, (uint8_t[32]){0}), command, expected);
}

static void protocol_test(void) {
    EXPECT(MF_REMOVE_PATH == 10, "remove command preserves wire code 10");
#ifdef MF_REMOVE
    EXPECT(MF_REMOVE == 0x00001000L, "protocol preserves the Win32 menu flag");
#endif
    static const char* invalid[] = { "../x", "a/../b", "/x", "a\\b", "a//b", "a/", "a/.x", "CON", "con.txt", "LPT1", "x:", "a ", "\xc0\xaf", "\xed\xa0\x80" };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) EXPECT(!mf_path_valid(invalid[i], 0), invalid[i]);
    EXPECT(mf_path_valid("Документы/пример.webp", 0), "UTF-8 path");
    EXPECT(!mf_path_valid("", 0) && mf_path_valid("", 1), "root path");
    memset(&q, 0, sizeof(q)); q.kind = MF_REQUEST; q.command = MF_WRITE; q.request_id = 42; fixture_key(UINT64_MAX, q.owner);
    q.offset = (1ULL << 40) + 7; q.size = q.data_len = MF_BLOCK_SIZE;
    snprintf(q.path, sizeof(q.path), "Документы/пример.webp"); memset(q.data, 171, sizeof(q.data));
    uint8_t wire[MF_WIRE_MAX]; int n = mf_encode(&q, wire, sizeof(wire));
    EXPECT(n > 0 && !mf_decode(wire, (size_t)n, &r) && !memcmp(&q, &r, sizeof(q)), "wire roundtrip and 64-bit offset");
    EXPECT(mf_decode(wire, (size_t)n - 1, &r) != 0, "truncated packet");
    wire[MF_WIRE_HEADER] = 0; EXPECT(mf_decode(wire, (size_t)n, &r) != 0, "embedded NUL");
}

static void avatar_storage_test(const char* directory) {
    char root[1200]; snprintf(root, sizeof(root), "%s/avatar-store", directory);
    struct mf_storage* s = mf_storage_create(root, MF_AVATAR_MAX_BYTES, 200000);
    EXPECT(s && mf_storage_initialize(s) == MF_OK, "avatar storage initialized"); if (!s) return;
    memset(&q, 0, sizeof(q)); fixture_key(77, q.owner); snprintf(q.path, sizeof(q.path), "avatars");
    run(s, 55, 77, MF_MKDIR, MF_OK);
    const uint8_t contents[] = {1, 2, 3, 4}; SHA256(contents, sizeof(contents), q.hash);
    char hash[65]; for (unsigned i = 0; i < 32; i++) snprintf(hash + i * 2, 3, "%02x", q.hash[i]);
    snprintf(q.path, sizeof(q.path), "avatars/%s.png", hash); q.size = sizeof(contents); memset(q.upload, 1, 16);
    run(s, 55, 77, MF_BEGIN_WRITE, MF_OK);
    memcpy(q.data, contents, sizeof(contents)); q.data_len = sizeof(contents); run(s, 55, 77, MF_WRITE, MF_OK);
    q.data_len = 0; run(s, 55, 77, MF_COMMIT, MF_OK);
    uint8_t version[16]; memcpy(version, r.version, 16);
    memset(q.upload, 2, 16); run(s, 55, 77, MF_BEGIN_WRITE, MF_EXISTS);
    memcpy(q.version, version, 16); run(s, 55, 77, MF_BEGIN_WRITE, MF_EXISTS);
    memset(q.version, 0, 16); q.hash[0] ^= 1; run(s, 55, 77, MF_BEGIN_WRITE, MF_HASH_MISMATCH); q.hash[0] ^= 1;
    q.size = MF_AVATAR_ORIGINAL_MAX_BYTES + 1; run(s, 55, 77, MF_BEGIN_WRITE, MF_INVALID_RANGE);
    snprintf(q.path, sizeof(q.path), "avatars/%s.json", hash);
    q.size = MF_AVATAR_MANIFEST_MAX_BYTES + 1; run(s, 55, 77, MF_BEGIN_WRITE, MF_INVALID_RANGE);
    EXPECT(mf_avatar_manifest_valid(q.path + 8), "manifest name accepted");
    snprintf(q.path, sizeof(q.path), "avatars/%s.png", hash);
    q.size = 0; run(s, 55, 77, MF_BEGIN_WRITE, MF_INVALID_RANGE);
    memcpy(q.version, version, 16); snprintf(q.target, sizeof(q.target), "renamed.png"); run(s, 55, 77, MF_RENAME, MF_INVALID_PATH);
    snprintf(q.path, sizeof(q.path), "Avatars/%s.png", hash); run(s, 55, 77, MF_BEGIN_WRITE, MF_INVALID_PATH);
    snprintf(q.path, sizeof(q.path), "AVATARS"); run(s, 55, 77, MF_MKDIR, MF_INVALID_PATH);
    memset(&q, 0, sizeof(q)); fixture_key(77, q.owner); run(s, 55, 88, MF_QUOTA, MF_OK);
    EXPECT(mf_get64(r.data) == sizeof(contents) && mf_get64(r.data + 16) == 0 &&
        mf_get64(r.data + 24) == 1 && mf_get64(r.data + 32) == 1, "avatars counted once in UID quota");
    EXPECT(!mf_avatar_file_valid("../avatar.png") && !mf_avatar_file_valid("avatar.png"), "noncanonical avatar names rejected");
    mf_storage_destroy(s);
}

static void storage_test(const char* directory) {
    struct mf_storage* s = mf_storage_create(directory, 40000, 200000);
    EXPECT(s != NULL, "create storage"); if (!s) return;
    memset(&q, 0, sizeof(q)); fixture_key(77, q.owner);
    run(s, 55, 77, MF_QUOTA, MF_BUSY);
    EXPECT(!mf_storage_initialize(s), "startup scan");
    uint8_t bytes[10001], version[16], upload[16];
    for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)(i * 73 + 19);
    memset(&q, 0, sizeof(q)); fixture_key(77, q.owner);
    run(s, 55, 77, MF_LIST, MF_OK); EXPECT(!r.data_len, "empty initialized root");
    snprintf(q.path, sizeof(q.path), "пример.webp"); q.size = sizeof(bytes); memset(q.upload, 9, 16); memcpy(upload, q.upload, 16);
    SHA256(bytes, sizeof(bytes), q.hash);
    run(s, 55, 88, MF_BEGIN_WRITE, MF_ACCESS_DENIED);
    run(s, 55, 77, MF_BEGIN_WRITE, MF_OK);
    run(s, 55, 77, MF_BEGIN_WRITE, MF_OK);
    run(s, 55, 88, MF_STAT, MF_NOT_FOUND);
    q.offset = MF_BLOCK_SIZE; q.size = q.data_len = sizeof(bytes) - MF_BLOCK_SIZE;
    memcpy(q.data, bytes + MF_BLOCK_SIZE, q.data_len); run(s, 55, 77, MF_WRITE, MF_OK);
    q.offset = 0; q.size = q.data_len = MF_BLOCK_SIZE; memcpy(q.data, bytes, MF_BLOCK_SIZE);
    run(s, 55, 77, MF_WRITE, MF_OK); run(s, 55, 77, MF_WRITE, MF_OK);
    q.data_len = 0; run(s, 55, 77, MF_COMMIT, MF_OK);
    memcpy(version, r.version, 16); EXPECT(r.size == sizeof(bytes), "published size");
    uint64_t commit_id = q.request_id; mf_storage_execute(s, storage_group_key(55, (char[65]){0}), fixture_key(77, (uint8_t[32]){0}), &q, &r); EXPECT(r.status == MF_OK, "same commit replay");
    run(s, 55, 77, MF_COMMIT, MF_OK); EXPECT(q.request_id != commit_id, "upload commit replay with new request id");
    q.size = MF_BLOCK_SIZE; q.offset = 0; memset(q.version, 0, 16);
    run(s, 55, 88, MF_READ, MF_STALE);
    memcpy(q.version, version, 16); run(s, 55, 88, MF_READ, MF_OK);
    EXPECT(r.data_len == MF_BLOCK_SIZE && !memcmp(r.data, bytes, MF_BLOCK_SIZE), "read by another member");
    EXPECT(mf_storage_open_count(s) == 1, "cached descriptor");
    uint64_t now = get_time_tb(); mf_storage_sweep(s, now + MF_IDLE_TB - 100);
    EXPECT(mf_storage_open_count(s) == 1, "fd retained before 10 seconds");
    mf_storage_sweep(s, now + MF_IDLE_TB + 100); EXPECT(!mf_storage_open_count(s), "fd closed after 10 seconds");
    run(s, 55, 88, MF_READ, MF_OK); EXPECT(r.data_len == MF_BLOCK_SIZE, "reopen preserves file version");
    q.offset = sizeof(bytes) - 2; run(s, 55, 88, MF_READ, MF_OK); EXPECT(r.data_len == 2 && !r.more, "short final read");
    q.offset = sizeof(bytes); run(s, 55, 88, MF_READ, MF_OK); EXPECT(!r.data_len && !r.more, "EOF read");
    q.offset++; run(s, 55, 88, MF_READ, MF_INVALID_RANGE);
    q.offset = 0; run(s, 56, 88, MF_STAT, MF_NOT_FOUND);
    q.size = 30000; memset(q.upload, 10, 16); snprintf(q.path, sizeof(q.path), "big"); memset(q.version, 0, 16);
    run(s, 55, 77, MF_BEGIN_WRITE, MF_NO_SPACE);
    q.size = 1; memset(q.upload, 11, 16); snprintf(q.path, sizeof(q.path), "bad"); memset(q.hash, 0, 32);
    run(s, 55, 77, MF_BEGIN_WRITE, MF_OK); run(s, 55, 77, MF_COMMIT, MF_HASH_MISMATCH); run(s, 55, 77, MF_ABORT, MF_OK);
    run(s, 55, 77, MF_STAT, MF_NOT_FOUND);
    q.size = 0; memset(q.upload, 12, 16); snprintf(q.path, sizeof(q.path), "empty"); SHA256(bytes, 0, q.hash);
    run(s, 55, 77, MF_BEGIN_WRITE, MF_OK); run(s, 55, 77, MF_COMMIT, MF_OK); EXPECT(!r.size, "empty file");
    memset(&q, 0, sizeof(q)); fixture_key(77, q.owner); snprintf(q.path, sizeof(q.path), "пример.webp"); memcpy(q.version, version, 16);
    q.size = 2; memset(q.upload, 13, 16); SHA256(bytes, 2, q.hash);
    run(s, 55, 77, MF_BEGIN_WRITE, MF_OK);
    struct mf_message saved = q;
    run(s, 55, 88, MF_STAT, MF_OK); EXPECT(r.size == sizeof(bytes), "old version visible during replacement");
    q = saved; q.size = q.data_len = 2; memcpy(q.data, bytes, 2); run(s, 55, 77, MF_WRITE, MF_OK);
    q.data_len = 0; run(s, 55, 77, MF_COMMIT, MF_OK); EXPECT(r.size == 2 && memcmp(version, r.version, 16), "short replacement");
    q.offset = 0; q.size = MF_BLOCK_SIZE; memcpy(q.version, version, 16); run(s, 55, 88, MF_READ, MF_STALE);
    memset(q.version, 0, 16); run(s, 55, 77, MF_STAT, MF_OK); memcpy(q.version, r.version, 16);
    snprintf(q.target, sizeof(q.target), "renamed"); run(s, 55, 77, MF_RENAME, MF_OK);
    run(s, 55, 77, MF_STAT, MF_NOT_FOUND);
    snprintf(q.path, sizeof(q.path), "renamed"); memset(q.version, 0, 16); run(s, 55, 77, MF_STAT, MF_OK);
    memcpy(q.version, r.version, 16); run(s, 55, 88, MF_REMOVE_PATH, MF_ACCESS_DENIED); run(s, 55, 77, MF_REMOVE_PATH, MF_OK);
    snprintf(q.path, sizeof(q.path), "folder"); memset(q.version, 0, 16); run(s, 55, 77, MF_MKDIR, MF_OK);
    snprintf(q.path, sizeof(q.path), "folder/child"); run(s, 55, 77, MF_MKDIR, MF_OK);
    snprintf(q.path, sizeof(q.path), "folder"); run(s, 55, 77, MF_STAT, MF_OK); memcpy(q.version, r.version, 16);
    run(s, 55, 77, MF_REMOVE_PATH, MF_DIRECTORY_NOT_EMPTY);
    memset(q.version, 0, 16); snprintf(q.path, sizeof(q.path), "folder/child"); run(s, 55, 77, MF_STAT, MF_OK);
    memcpy(q.version, r.version, 16); run(s, 55, 77, MF_REMOVE_PATH, MF_OK);
    memset(q.version, 0, 16);
    q.path[0] = 0; run(s, 55, 88, MF_LIST, MF_OK); EXPECT(r.data_len > 0, "directory listing");
#ifndef _WIN32
    char link[2048]; snprintf(link, sizeof(link), "%s/%s/%s/link", directory, storage_group_key(55, (char[65]){0}), sc_public_key_hex(fixture_key(77, (uint8_t[32]){0}), (char[65]){0}));
    EXPECT(!symlink("/tmp", link), "create hostile symlink");
    snprintf(q.path, sizeof(q.path), "link/escape"); run(s, 55, 77, MF_MKDIR, MF_INVALID_PATH);
    test_unlink(link);
#endif
    snprintf(q.path, sizeof(q.path), "expired"); q.size = 10; memset(q.upload, 14, 16); memset(q.version, 0, 16);
    run(s, 55, 77, MF_BEGIN_WRITE, MF_OK); mf_storage_sweep(s, get_time_tb() + 200001);
    run(s, 55, 77, MF_WRITE, MF_NOT_FOUND);
    mf_storage_destroy(s);
    s = mf_storage_create(directory, 40000, 200000);
    EXPECT(s && !mf_storage_initialize(s), "restart scan");
    snprintf(q.path, sizeof(q.path), "empty"); q.data_len = 0; run(s, 55, 88, MF_STAT, MF_OK); EXPECT(r.size == 0, "restart retains published files");
    memset(&q, 0, sizeof(q)); fixture_key(77, q.owner);
    for (unsigned i = 0; i < 63; i++) {
        int n = snprintf(q.path, sizeof(q.path), "dir_%02u_", i);
        memset(q.path + n, 'a', 200); q.path[n + 200] = 0; run(s, 55, 77, MF_MKDIR, MF_OK);
    }
    q.path[0] = 0; run(s, 55, 88, MF_LIST, MF_OK); EXPECT(r.more && r.data_len <= MF_BLOCK_SIZE, "bounded directory page");
    struct mf_message page = q; page.cursor = r.cursor; memcpy(page.version, r.version, 16);
    snprintf(q.path, sizeof(q.path), "fresh"); run(s, 55, 77, MF_MKDIR, MF_OK);
    q = page; run(s, 55, 88, MF_LIST, MF_STALE);
    memset(q.version, 0, 16); q.cursor = 0;
    unsigned entries = 0, pages = 0;
    do {
        run(s, 55, 88, MF_LIST, MF_OK); pages++;
        for (unsigned at = 0; at < r.data_len;) {
            unsigned n = r.data[at] | ((unsigned)r.data[at + 1] << 8);
            EXPECT(n && at + MF_ENTRY_HEADER + n <= r.data_len, "directory entry bounds");
            if (!n || at + MF_ENTRY_HEADER + n > r.data_len) break;
            entries++; at += MF_ENTRY_HEADER + n;
        }
        q.cursor = r.cursor; memcpy(q.version, r.version, 16);
    } while (r.more && pages < 8);
    EXPECT(entries == 66 && pages >= 2 && !r.more, "all directory entries across pages");
    mf_storage_destroy(s);
}

#ifndef _WIN32
/* Ошибочный local source не должен блокировать единственный disk worker до проверки типа. */
static void local_fifo_test(const char* directory) {
    char path[1200]; snprintf(path, sizeof(path), "%s/local-fifo", directory);
    EXPECT(!mkfifo(path, 0600), "FIFO source fixture");
    pid_t child = fork();
    EXPECT(child >= 0, "FIFO source child");
    if (!child) {
        alarm(2);
        int fd = -1; struct mf_disk_info info;
        int opened = mf_disk_local_open(path, 0, &fd);
        int valid = opened == MF_OK && mf_disk_info(fd, &info) == MF_INVALID_PATH;
        if (fd >= 0 && mf_disk_close(fd)) valid = 0;
        _exit(valid ? 0 : 1);
    }
    if (child > 0) {
        int status = 0;
        EXPECT(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "FIFO open returns before type rejection");
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: FIFO source child status=%d; expected normal exit before2s", status);
    }
    test_unlink(path);
}

static void large_offset_test(const char* directory) {
    char path[1200]; snprintf(path, sizeof(path), "%s/large-offset", directory);
    int fd = -1; uint64_t end = (1ULL << 33) + 4;
    EXPECT(mf_disk_local_open(path, 1, &fd) == MF_OK, "open sparse file"); if (fd < 0) return;
    EXPECT(mf_disk_resize(fd, end) == MF_OK, "resize beyond 4 GiB");
    uint8_t data[4] = {1, 2, 3, 4}, read[4] = {0};
    EXPECT(mf_disk_write(fd, end - 4, data, 4) == MF_OK && mf_disk_read(fd, end - 4, read, 4) == MF_OK,
           "disk I/O beyond 4 GiB");
    struct mf_disk_info info;
    EXPECT(!mf_disk_info(fd, &info) && info.size == end && !memcmp(data, read, 4), "64-bit file size and bytes");
    mf_disk_close(fd); mf_disk_local_remove(path);
}
#endif

struct worker_test { struct UASYNC* ua; struct mf_worker* worker; pthread_t caller; int ran, finished; };
static void worker_work(void* arg) {
    struct worker_test* t = arg; t->ran = !pthread_equal(t->caller, pthread_self());
}
static void worker_done(void* arg, int error) {
    struct worker_test* t = arg; t->finished = !error && pthread_equal(t->caller, pthread_self()); uasync_stop(t->ua);
}
static void check_quota(struct mf_storage* s, uint64_t group, uint64_t owner, uint64_t used, uint64_t reserved, uint64_t limit) {
    struct mf_message request = { .command = MF_QUOTA, .kind = MF_REQUEST, .request_id = ++sequence }, response;
    fixture_key(owner, request.owner);
    mf_storage_execute(s, storage_group_key(group, (char[65]){0}), request.owner, &request, &response);
    EXPECT(response.status == MF_OK && response.data_len == MF_QUOTA_BYTES, "quota response");
    EXPECT(mf_get64(response.data) == used && mf_get64(response.data + 8) == limit && mf_get64(response.data + 16) == reserved,
        "used/limit/reservation snapshot");
}

static void quota_begin(struct mf_storage* s, uint64_t group, uint64_t owner, const char* name,
                         unsigned size, unsigned token, const uint8_t* version, int status) {
    memset(&q, 0, sizeof(q)); fixture_key(owner, q.owner); q.size = size;
    memset(q.upload, token, 16); snprintf(q.path, sizeof(q.path), "%s", name);
    if (version) memcpy(q.version, version, 16);
    uint8_t bytes[32] = {0}; SHA256(bytes, size, q.hash);
    run(s, group, owner, MF_BEGIN_WRITE, status);
}

static void quota_commit(struct mf_storage* s, uint64_t group) {
    q.data_len = (uint32_t)q.size; memset(q.data, 0, q.data_len);
    run_key(s, group, q.owner, MF_WRITE, MF_OK);
    q.data_len = 0; run_key(s, group, q.owner, MF_COMMIT, MF_OK);
}

static void check_counts(struct mf_storage* s, uint64_t group, uint64_t owner, uint64_t files, uint64_t directories) {
    struct mf_message request = { .command = MF_QUOTA, .kind = MF_REQUEST, .request_id = ++sequence }, response;
    fixture_key(owner, request.owner);
    mf_storage_execute(s, storage_group_key(group, (char[65]){0}), request.owner, &request, &response);
    EXPECT(response.status == MF_OK && response.data_len == MF_QUOTA_BYTES, "count snapshot response");
    EXPECT(mf_get64(response.data + 24) == files && mf_get64(response.data + 32) == directories, "file/directory counters");
}

static void count_mutation(struct mf_storage* s, uint64_t group, uint64_t owner, const char* path,
                            const char* target, int command, int status) {
    memset(&q, 0, sizeof(q)); fixture_key(owner, q.owner); snprintf(q.path, sizeof(q.path), "%s", path);
    if (command != MF_MKDIR) { run(s, group, owner, MF_STAT, MF_OK); memcpy(q.version, r.version, 16); }
    if (target) snprintf(q.target, sizeof(q.target), "%s", target);
    run(s, group, owner, command, status);
}

static void count_test(const char* directory) {
    char root[1200]; snprintf(root, sizeof(root), "%s/counts", directory);
    struct mf_storage* s = mf_storage_create(root, 40, 200000);
    EXPECT(s && !mf_storage_initialize(s), "count storage startup"); if (!s) return;
    check_counts(s, 10, 101, 0, 0);
    count_mutation(s, 10, 101, "folder", NULL, MF_MKDIR, MF_OK);
    check_counts(s, 20, 101, 0, 1);
    mf_storage_execute(s, storage_group_key(10, (char[65]){0}), fixture_key(101, (uint8_t[32]){0}), &q, &r); EXPECT(r.status == MF_OK, "mkdir replay");
    run(s, 10, 101, MF_MKDIR, MF_EXISTS); check_counts(s, 10, 101, 0, 1);
    count_mutation(s, 10, 101, "folder/sub", NULL, MF_MKDIR, MF_OK);
    check_counts(s, 10, 101, 0, 2);
    quota_begin(s, 10, 101, "folder/zero", 0, 61, NULL, MF_OK);
    check_counts(s, 10, 101, 0, 2); quota_commit(s, 10); check_counts(s, 10, 101, 1, 2);
    uint8_t version[16]; memcpy(version, r.version, 16);
    quota_begin(s, 10, 101, "folder/zero", 0, 62, version, MF_OK); quota_commit(s, 10);
    memcpy(version, r.version, 16); check_counts(s, 10, 101, 1, 2);
    quota_begin(s, 10, 101, "folder/zero", 2, 63, version, MF_OK); quota_commit(s, 10);
    mf_storage_execute(s, storage_group_key(10, (char[65]){0}), fixture_key(101, (uint8_t[32]){0}), &q, &r); EXPECT(r.status == MF_OK, "replacement replay");
    check_counts(s, 20, 101, 1, 2);
    quota_begin(s, 20, 101, "other", 3, 64, NULL, MF_OK); quota_commit(s, 20);
    check_counts(s, 10, 101, 2, 2); check_quota(s, 10, 101, 5, 0, 40);
    quota_begin(s, 10, 202, "other-user", 3, 65, NULL, MF_OK); quota_commit(s, 10);
    check_counts(s, 10, 101, 2, 2); check_counts(s, 10, 202, 1, 0);
    count_mutation(s, 10, 101, "folder", "moved", MF_RENAME, MF_OK);
    check_counts(s, 10, 101, 2, 2);
    count_mutation(s, 10, 101, "moved", NULL, MF_REMOVE_PATH, MF_DIRECTORY_NOT_EMPTY);
    check_counts(s, 10, 101, 2, 2);
    count_mutation(s, 10, 101, "moved/zero", NULL, MF_REMOVE_PATH, MF_OK);
    mf_storage_execute(s, storage_group_key(10, (char[65]){0}), fixture_key(101, (uint8_t[32]){0}), &q, &r); EXPECT(r.status == MF_OK, "file remove replay");
    check_counts(s, 20, 101, 1, 2); check_quota(s, 20, 101, 3, 0, 40);
    count_mutation(s, 10, 101, "moved/sub", NULL, MF_REMOVE_PATH, MF_OK);
    mf_storage_execute(s, storage_group_key(10, (char[65]){0}), fixture_key(101, (uint8_t[32]){0}), &q, &r); EXPECT(r.status == MF_OK, "directory remove replay");
    check_counts(s, 20, 101, 1, 1);
    count_mutation(s, 10, 101, "moved", NULL, MF_REMOVE_PATH, MF_OK); check_counts(s, 10, 101, 1, 0);
    quota_begin(s, 10, 101, "aborted", 4, 66, NULL, MF_OK);
    check_counts(s, 10, 101, 1, 0); run(s, 10, 101, MF_ABORT, MF_OK); check_counts(s, 10, 101, 1, 0);
    quota_begin(s, 10, 101, "expired", 4, 67, NULL, MF_OK);
    mf_storage_sweep(s, get_time_tb() + 200001); check_counts(s, 20, 101, 1, 0);
    // Внешние изменения появляются в статистике только после нового старта, не после мутации/QUOTA.
    char external[1400], folder[1400];
    snprintf(external, sizeof(external), "%s/%s/%s/external.bin", root, storage_group_key(10, (char[65]){0}), sc_public_key_hex(fixture_key(101, (uint8_t[32]){0}), (char[65]){0}));
    FILE* file = fopen(external, "wb"); EXPECT(file != NULL, "external file fixture");
    if (file) { fputs("extra", file); fclose(file); }
    snprintf(folder, sizeof(folder), "%s/%s/external-dir", storage_group_key(10, (char[65]){0}), sc_public_key_hex(fixture_key(101, (uint8_t[32]){0}), (char[65]){0}));
    struct mf_disk_root disk; EXPECT(!mf_disk_root_open(&disk, root), "external directory fixture root");
    int created = 0; EXPECT(!mf_disk_mkdir(&disk, folder, &created) && created, "mkdir reports creation");
    created = 1; EXPECT(mf_disk_mkdir(&disk, folder, &created) == MF_EXISTS && !created, "existing mkdir reports no creation");
    mf_disk_root_close(&disk);
    count_mutation(s, 10, 101, "api-dir", NULL, MF_MKDIR, MF_OK);
    check_counts(s, 10, 101, 1, 1); check_quota(s, 20, 101, 3, 0, 40);
    quota_begin(s, 10, 101, "api-file", 1, 68, NULL, MF_OK); quota_commit(s, 10);
    check_counts(s, 20, 101, 2, 1); check_quota(s, 10, 101, 4, 0, 40);
    mf_storage_destroy(s); s = mf_storage_create(root, 40, 200000);
    EXPECT(s && !mf_storage_initialize(s), "count restart scan"); if (!s) return;
    check_counts(s, 20, 101, 3, 2); check_quota(s, 10, 101, 9, 0, 40);
    check_counts(s, 10, 202, 1, 0); mf_storage_destroy(s);
}

static void quota_test(const char* directory) {
    char root[1200]; snprintf(root, sizeof(root), "%s/quota", directory);
    struct mf_storage* s = mf_storage_create(root, 40, 200000);
    EXPECT(s && !mf_storage_initialize(s), "quota storage startup"); if (!s) return;
    check_quota(s, 10, 101, 0, 0, 40);
    quota_begin(s, 10, 101, "a", 30, 31, NULL, MF_OK); quota_commit(s, 10);
    uint8_t version[16]; memcpy(version, r.version, 16);
    quota_begin(s, 10, 202, "b", 30, 32, NULL, MF_OK); quota_commit(s, 10);
    check_quota(s, 10, 101, 30, 0, 40); check_quota(s, 10, 202, 30, 0, 40);
    quota_begin(s, 20, 101, "pending-a", 8, 33, NULL, MF_OK);
    struct mf_message pending = q;
    check_quota(s, 10, 101, 30, 8, 40);
    quota_begin(s, 10, 101, "overflow", 8, 34, NULL, MF_NO_SPACE);
    quota_begin(s, 20, 202, "pending-b", 8, 34, NULL, MF_OK);
    check_quota(s, 10, 202, 30, 8, 40);
    run(s, 20, 202, MF_ABORT, MF_OK); run(s, 20, 202, MF_ABORT, MF_OK);
    check_quota(s, 20, 202, 30, 0, 40);
    q = pending; run(s, 20, 101, MF_BEGIN_WRITE, MF_OK);
    check_quota(s, 20, 101, 30, 8, 40); quota_commit(s, 20);
    check_quota(s, 10, 101, 38, 0, 40);
    quota_begin(s, 10, 101, "a", 20, 35, version, MF_OK);
    check_quota(s, 10, 101, 38, 0, 40); quota_commit(s, 10);
    memcpy(version, r.version, 16);
    mf_storage_execute(s, storage_group_key(10, (char[65]){0}), fixture_key(101, (uint8_t[32]){0}), &q, &r); EXPECT(r.status == MF_OK, "commit replay retains quota");
    check_quota(s, 20, 101, 28, 0, 40);
    quota_begin(s, 10, 101, "a", 30, 36, version, MF_OK);
    check_quota(s, 20, 101, 28, 10, 40);
    pending = q; quota_begin(s, 20, 101, "overflow", 3, 37, NULL, MF_NO_SPACE);
    q = pending; run(s, 10, 101, MF_ABORT, MF_OK);
    check_quota(s, 20, 101, 28, 0, 40);
    memset(&q, 0, sizeof(q)); fixture_key(101, q.owner); memcpy(q.version, version, 16);
    snprintf(q.path, sizeof(q.path), "a"); snprintf(q.target, sizeof(q.target), "renamed");
    run(s, 10, 101, MF_RENAME, MF_OK); check_quota(s, 10, 101, 28, 0, 40);
    snprintf(q.path, sizeof(q.path), "renamed"); memset(q.version, 0, 16);
    run(s, 10, 101, MF_STAT, MF_OK); memcpy(q.version, r.version, 16);
    run(s, 10, 101, MF_REMOVE_PATH, MF_OK);
    mf_storage_execute(s, storage_group_key(10, (char[65]){0}), fixture_key(101, (uint8_t[32]){0}), &q, &r); EXPECT(r.status == MF_OK, "remove replay retains quota");
    check_quota(s, 20, 101, 8, 0, 40);
    memset(&q, 0, sizeof(q)); fixture_key(101, q.owner); snprintf(q.path, sizeof(q.path), "folder");
    run(s, 10, 101, MF_MKDIR, MF_OK); run(s, 10, 101, MF_STAT, MF_OK); memcpy(q.version, r.version, 16);
    run(s, 10, 101, MF_REMOVE_PATH, MF_OK); check_quota(s, 10, 101, 8, 0, 40);
    quota_begin(s, 10, 101, "expired", 6, 38, NULL, MF_OK);
    check_quota(s, 20, 101, 8, 6, 40); mf_storage_sweep(s, get_time_tb() + 200001);
    check_quota(s, 10, 101, 8, 0, 40); run(s, 10, 101, MF_ABORT, MF_OK);
    quota_begin(s, 10, 101, "persist", 12, 39, NULL, MF_OK); quota_commit(s, 10);
    for (unsigned i = 0; i < 80; i++) check_quota(s, 20, 101, 20, 0, 40);
    mf_storage_destroy(s);
    char abandoned[1400]; snprintf(abandoned, sizeof(abandoned), "%s/.uploads/abcdef0123456789abcdef0123456789", root);
    FILE* file = fopen(abandoned, "wb"); EXPECT(file != NULL, "abandoned upload fixture");
    if (file) { fputs("unfinished", file); fclose(file); }
    s = mf_storage_create(root, 10, 200000);
    EXPECT(s && !mf_storage_initialize(s), "rescan with smaller per-user quota"); if (!s) return;
    check_quota(s, 10, 101, 20, 0, 10); check_quota(s, 20, 202, 30, 0, 10);
    file = fopen(abandoned, "rb"); EXPECT(!file, "startup removes abandoned temp"); if (file) fclose(file);
    quota_begin(s, 20, 101, "over-limit", 1, 40, NULL, MF_NO_SPACE);
    memset(&q, 0, sizeof(q)); fixture_key(101, q.owner); snprintf(q.path, sizeof(q.path), "persist");
    run(s, 10, 101, MF_STAT, MF_OK); memcpy(version, r.version, 16);
    quota_begin(s, 10, 101, "persist", 2, 41, version, MF_OK); quota_commit(s, 10);
    check_quota(s, 20, 101, 10, 0, 10);
    quota_begin(s, 20, 101, "at-limit", 1, 42, NULL, MF_NO_SPACE);
    check_quota(s, 10, 202, 30, 0, 10); mf_storage_destroy(s);
}

static void quota_scan_failure_test(const char* directory) {
    char root[1200], rogue[1400]; snprintf(root, sizeof(root), "%s/quota-invalid", directory);
    struct mf_storage* s = mf_storage_create(root, 40, 200000);
    EXPECT(s && !mf_storage_initialize(s), "scan failure fixture startup"); if (!s) return;
    quota_begin(s, 10, 101, "persist", 12, 51, NULL, MF_OK); quota_commit(s, 10); mf_storage_destroy(s);
    snprintf(rogue, sizeof(rogue), "%s/rogue.bin", root);
    FILE* file = fopen(rogue, "wb"); EXPECT(file != NULL, "invalid namespace fixture"); if (file) fclose(file);
    s = mf_storage_create(root, 40, 200000); EXPECT(s != NULL, "scan failure storage allocation"); if (!s) return;
    EXPECT(mf_storage_initialize(s) == MF_INVALID_PATH, "invalid namespace rejects partial startup count");
    memset(&q, 0, sizeof(q)); fixture_key(101, q.owner);
    run(s, 10, 101, MF_QUOTA, MF_INVALID_PATH); EXPECT(!r.data_len, "failed scan does not expose partial quota");
    mf_storage_destroy(s); test_unlink(rogue);
    s = mf_storage_create(root, 40, 200000);
    EXPECT(s && !mf_storage_initialize(s), "restart after namespace repair");
    if (s) { check_quota(s, 20, 101, 12, 0, 40); mf_storage_destroy(s); }
}

static void worker_test(void) {
    struct worker_test t = {0}; t.ua = uasync_create(); t.caller = pthread_self(); t.worker = mf_worker_create(t.ua);
    EXPECT(t.worker != NULL, "worker start");
    if (t.worker) {
        mf_worker_submit(t.worker, worker_work, &t, worker_done, &t); uasync_mainloop(t.ua);
        EXPECT(t.ran && t.finished, "work on disk thread, done in uasync");
        t.ran = t.finished = 0; mf_worker_submit(t.worker, worker_work, &t, worker_done, &t);
        mf_worker_destroy(t.worker); EXPECT(t.ran && t.finished, "shutdown drains queued completion");
    }
    uasync_destroy(t.ua, 1);
}

#ifdef __linux__
static pthread_mutex_t post_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t post_condition = PTHREAD_COND_INITIALIZER;
static struct UASYNC* observed_ua;
static pthread_t observed_owner;
static unsigned worker_posts;

/* Барьер только наблюдает настоящую публикацию: completion не подменяется и не исполняется вручную. */
extern void __real_uasync_post_reserved(struct UASYNC*, struct posted_task*);
void __wrap_uasync_post_reserved(struct UASYNC* ua, struct posted_task* task) {
    __real_uasync_post_reserved(ua, task);
    pthread_mutex_lock(&post_lock);
    if (ua == observed_ua && !pthread_equal(pthread_self(), observed_owner)) {
        worker_posts++; pthread_cond_signal(&post_condition);
    }
    pthread_mutex_unlock(&post_lock);
}

static void wait_worker_posts(unsigned count) {
    pthread_mutex_lock(&post_lock);
    while (worker_posts < count) pthread_cond_wait(&post_condition, &post_lock);
    pthread_mutex_unlock(&post_lock);
}

struct drain_case;
struct drain_job { struct drain_case* test; unsigned index, worked, done; int disk_thread, owner_thread, error; };
struct drain_case {
    struct UASYNC* ua;
    struct mf_worker* worker;
    struct mf_work* cleanup;
    pthread_t owner;
    struct drain_job jobs[3];
    int destroy, stops;
};

static void drain_work(void* arg) {
    struct drain_job* j = arg;
    j->worked++; j->disk_thread = !pthread_equal(pthread_self(), j->test->owner);
}

/* Первый done добавляет обязательную cleanup continuation, в том числе во время drain/destroy. */
static void drain_done(void* arg, int error) {
    struct drain_job* j = arg;
    struct drain_case* t = j->test;
    j->done++; j->error = error; j->owner_thread = pthread_equal(pthread_self(), t->owner);
    if (!j->index) {
        struct mf_work* cleanup = t->cleanup; t->cleanup = NULL;
        mf_worker_submit_reserved(t->worker, cleanup, drain_work, &t->jobs[2], drain_done, &t->jobs[2]);
    }
}

static void drain_in_batch(void* arg) {
    struct drain_case* t = arg; t->stops++;
    if (t->destroy) { mf_worker_destroy(t->worker); t->worker = NULL; }
    else mf_worker_drain(t->worker);
    for (unsigned i = 0; i < 3; i++) EXPECT(t->jobs[i].done == 1, "stop delivers each done before returning");
}

/* normal, queued destroy, detached drain, detached destroy; два completion и cleanup в каждом случае. */
static void worker_completion_test(void) {
    for (unsigned mode = 0; mode < 4; mode++) {
        size_t before = u_get_allocated_count();
        struct drain_case t = {0}; t.owner = pthread_self(); t.destroy = mode == 3;
        t.ua = uasync_create(); EXPECT(t.ua != NULL, "drain fixture uasync"); if (!t.ua) return;
        t.worker = mf_worker_create(t.ua); t.cleanup = mf_worker_reserve();
        EXPECT(t.worker && t.cleanup, "drain fixture worker and cleanup reserve");
        if (!t.worker || !t.cleanup) {
            mf_worker_destroy(t.worker); mf_worker_unreserve(t.cleanup); uasync_destroy(t.ua, 1); return;
        }
        for (unsigned i = 0; i < 3; i++) { t.jobs[i].test = &t; t.jobs[i].index = i; }
        pthread_mutex_lock(&post_lock);
        observed_ua = t.ua; observed_owner = t.owner; worker_posts = 0;
        pthread_mutex_unlock(&post_lock);
        if (mode >= 2) {
            /* Stop раньше completion, а poll начнётся только после обеих публикаций. */
            struct posted_task* stop = u_calloc(1, sizeof(*stop));
            EXPECT(stop != NULL, "stop task reserve");
            if (!stop) {
                mf_worker_destroy(t.worker); mf_worker_unreserve(t.cleanup); uasync_destroy(t.ua, 1);
                pthread_mutex_lock(&post_lock); observed_ua = NULL; pthread_mutex_unlock(&post_lock); return;
            }
            stop->callback = drain_in_batch; stop->arg = &t; uasync_post_reserved(t.ua, stop);
        }
        for (unsigned i = 0; i < 2; i++) mf_worker_submit(t.worker, drain_work, &t.jobs[i], drain_done, &t.jobs[i]);
        wait_worker_posts(2);
        EXPECT(!t.jobs[0].done && !t.jobs[1].done, "publication does not deliver done before poll/drain");
        if (mode == 1) { mf_worker_destroy(t.worker); t.worker = NULL; }
        uasync_poll(t.ua, 0);
        wait_worker_posts(3); uasync_poll(t.ua, 0);
        EXPECT(t.stops == (mode >= 2), "batch stop exactly once");
        for (unsigned i = 0; i < 3; i++) {
            struct drain_job* j = &t.jobs[i];
            EXPECT(j->worked == 1 && j->done == 1 && !j->error && j->disk_thread && j->owner_thread,
                   "work and done once, correct threads, cleanup completed");
        }
        EXPECT(t.cleanup == NULL, "cleanup reserve transferred exactly once");
        mf_worker_destroy(t.worker); uasync_poll(t.ua, 0); uasync_destroy(t.ua, 1);
        pthread_mutex_lock(&post_lock); observed_ua = NULL; pthread_mutex_unlock(&post_lock);
        EXPECT(u_get_allocated_count() == before, "worker completion tasks and jobs all freed");
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "worker completion regression mode=%u: work/done=3/3, cleanup and allocations verified", mode);
    }
}
#endif

struct cancel_case {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int started, released, worked, done, cancelled;
};

static void cancel_block(void* arg) {
    struct cancel_case* t = arg;
    pthread_mutex_lock(&t->mutex); t->started = 1; pthread_cond_signal(&t->condition);
    while (!t->released) pthread_cond_wait(&t->condition, &t->mutex);
    pthread_mutex_unlock(&t->mutex); t->worked++;
}
static void cancel_work(void* arg) { ((struct cancel_case*)arg)->worked++; }
static void cancel_done(void* arg, int error) {
    struct cancel_case* t = arg;
    EXPECT(!error || error == MF_WORK_CANCELLED, "cancel completion status");
    t->done++; t->cancelled += error == MF_WORK_CANCELLED;
}

/* Диск стоит на барьере: отмена головы/хвоста не выполняет work и сохраняет обязательную cleanup. */
static void worker_cancel_test(void) {
    size_t before = u_get_allocated_count();
    struct cancel_case running = { .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER };
    struct cancel_case head = {0}, tail = {0};
    struct UASYNC* ua = uasync_create();
    struct mf_worker* worker = ua ? mf_worker_create(ua) : NULL;
    struct mf_work* cleanup = mf_worker_reserve();
    EXPECT(worker && cleanup, "cancel worker fixture");
    if (worker && cleanup) {
        mf_worker_submit(worker, cancel_block, &running, cancel_done, &running);
        pthread_mutex_lock(&running.mutex);
        while (!running.started) pthread_cond_wait(&running.condition, &running.mutex);
        pthread_mutex_unlock(&running.mutex);
        mf_worker_submit(worker, cancel_work, &head, cancel_done, &head);
        mf_worker_submit_reserved(worker, cleanup, cancel_work, &head, cancel_done, &head); cleanup = NULL;
        mf_worker_submit(worker, cancel_work, &tail, cancel_done, &tail);
        EXPECT(mf_worker_cancel(worker, &running) == 0, "running work cannot be cancelled");
        EXPECT(mf_worker_cancel(worker, &head) == 1, "cancel head preserves reserved cleanup with same data");
        EXPECT(mf_worker_cancel(worker, &tail) == 1, "cancel tail");
        EXPECT(mf_worker_cancel(worker, &head) == 0, "cancel is idempotent");
        uasync_poll(ua, 0);
        EXPECT(head.done == 1 && head.cancelled == 1 && tail.done == 1 && tail.cancelled == 1,
               "cancel completion delivered while disk remains blocked");
        EXPECT(!head.worked && !tail.worked && !running.done, "cancelled work has no side effects");
        pthread_mutex_lock(&running.mutex); running.released = 1; pthread_cond_signal(&running.condition);
        pthread_mutex_unlock(&running.mutex);
    }
    mf_worker_destroy(worker); mf_worker_unreserve(cleanup);
    if (ua) { uasync_poll(ua, 0); uasync_destroy(ua, 1); }
    EXPECT(running.worked == 1 && running.done == 1 && !running.cancelled, "running command finishes once");
    EXPECT(head.worked == 1 && head.done == 2 && head.cancelled == 1, "reserved cleanup finishes once");
    EXPECT(!tail.worked && tail.done == 1 && tail.cancelled == 1, "cancelled tail never executes");
    pthread_cond_destroy(&running.condition); pthread_mutex_destroy(&running.mutex);
    EXPECT(u_get_allocated_count() == before, "cancelled jobs/completions freed");
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "worker cancellation: queued jobs skipped, running command and reserved cleanup completed");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG);
    char directory[1024] = "/tmp/utun_member_files_XXXXXX";
    if (test_mkdtemp(directory)) return 1;
    protocol_test(); storage_test(directory); avatar_storage_test(directory); quota_test(directory);
    count_test(directory); quota_scan_failure_test(directory);
#ifndef _WIN32
    local_fifo_test(directory); large_offset_test(directory);
#endif
    worker_test(); worker_cancel_test();
#ifdef __linux__
    worker_completion_test();
#endif
    printf("member_files: %s failures=%d directory=%s\n", failures ? "FAIL" : "PASS", failures, directory);
    return failures ? 1 : 0;
}
