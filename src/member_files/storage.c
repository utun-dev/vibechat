#include "storage.h"
#include "../chat/chat_core.h"
#include "../transport_layer/secure_channel.h"
#include "disk.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/u_async.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#define STORE_FILES 64
#define STORE_UPLOADS 16
#define STORE_RECEIPTS 64
#define STORE_USERS 256

// Windows сравнивает имена без регистра; алиас не должен обходить неизменяемость avatars.
static int store_avatar_namespace(const char* path) {
    const char name[] = "avatars";
    for (unsigned i = 0; i < sizeof(name) - 1; i++) if (tolower((unsigned char)path[i]) != name[i]) return 0;
    return !path[7] || path[7] == '/';
}

struct store_file { char path[MF_PATH_STORAGE_MAX]; int fd; uint64_t touched; };
struct store_user { struct store_user* next; uint64_t used, reserved, files, directories; uint8_t owner[MF_ADDRESS_SIZE]; int valid; };
struct store_upload {
    char group[65];
    uint64_t size, previous, reservation, touched;
    uint8_t peer[MF_ADDRESS_SIZE], owner[MF_ADDRESS_SIZE];
    struct store_user* user;
    uint8_t id[16], hash[32], expected[16];
    char path[MF_PATH_STORAGE_MAX], temp[64];
    int active;
};
struct store_receipt {
    char group[65];
    uint64_t until;
    uint8_t peer[MF_ADDRESS_SIZE];
    uint8_t fingerprint[32];
    struct mf_message response;
};
struct mf_storage {
    char path[2048];
    struct mf_disk_root root;
    int ready, init_status;
    uint8_t epoch[16];
    uint64_t generation, quota, upload_ttl;
    unsigned user_count;
    struct store_user* users[STORE_USERS];
    struct store_file files[STORE_FILES];
    struct store_upload uploads[STORE_UPLOADS];
    struct store_receipt receipts[STORE_RECEIPTS];
};

static struct store_user* store_user_get(struct mf_storage* s, const uint8_t owner[MF_ADDRESS_SIZE], int create) {
    unsigned bucket = (unsigned)mf_get64(owner) % STORE_USERS;
    for (struct store_user* u = s->users[bucket]; u; u = u->next) if (!memcmp(u->owner, owner, MF_ADDRESS_SIZE)) return u;
    if (!create) return NULL;
    struct store_user* u = u_calloc(1, sizeof(*u));
    if (!u) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: quota account allocation failed owner=%s", sc_public_key_hex(owner, (char[65]){0})); return NULL; }
    memcpy(u->owner, owner, MF_ADDRESS_SIZE); u->valid = 1; u->next = s->users[bucket]; s->users[bucket] = u; s->user_count++;
    return u;
}

static void store_users_free(struct mf_storage* s) {
    for (unsigned i = 0; i < STORE_USERS; i++) {
        while (s->users[i]) { struct store_user* next = s->users[i]->next; u_free(s->users[i]); s->users[i] = next; }
    }
    s->user_count = 0;
}

/* Один worker корректирует счётчики вместе. file_delta/dir_delta равны -1, 0 или +1; обхода дерева здесь нет. */
static int store_user_update(struct store_user* u, uint64_t previous, uint64_t size, uint64_t release,
                             int file_delta, int dir_delta, const char* operation) {
    if (!u->valid || previous > u->used || release > u->reserved || size > UINT64_MAX - (u->used - previous) ||
        (file_delta < 0 && !u->files) || (file_delta > 0 && u->files == UINT64_MAX) ||
        (dir_delta < 0 && !u->directories) || (dir_delta > 0 && u->directories == UINT64_MAX)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA,
            "member_files: quota accounting invalid owner=%s operation=%s used=%llu previous=%llu size=%llu reserved=%llu release=%llu"
            " files=%llu file_delta=%d directories=%llu dir_delta=%d",
            sc_public_key_hex(u->owner, (char[65]){0}), operation, (unsigned long long)u->used, (unsigned long long)previous,
            (unsigned long long)size, (unsigned long long)u->reserved, (unsigned long long)release,
            (unsigned long long)u->files, file_delta, (unsigned long long)u->directories, dir_delta);
        u->valid = 0; return MF_IO_ERROR;
    }
    u->used = u->used - previous + size; u->reserved -= release;
    if (file_delta > 0) u->files++; else if (file_delta < 0) u->files--;
    if (dir_delta > 0) u->directories++; else if (dir_delta < 0) u->directories--;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA,
        "member_files: quota updated owner=%s operation=%s used=%llu reserved=%llu files=%llu directories=%llu",
        sc_public_key_hex(u->owner, (char[65]){0}), operation, (unsigned long long)u->used, (unsigned long long)u->reserved,
        (unsigned long long)u->files, (unsigned long long)u->directories);
    return MF_OK;
}

/* Версия переживает закрытие fd, но не restart. Метаданные снимка — непрозрачные байты. */
static void store_version(struct mf_storage* s, const struct mf_disk_info* i, uint8_t out[16]) {
    uint8_t bytes[81], hash[32];
    memcpy(bytes, s->epoch, 16);
    for (unsigned n = 0; n < 6; n++) mf_put64(bytes + 16 + n * 8, i->identity[n]);
    mf_put64(bytes + 64, i->size); bytes[72] = i->type;
    mf_put64(bytes + 73, i->type == MF_DIRECTORY ? s->generation : 0);
    SHA256(bytes, sizeof(bytes), hash); memcpy(out, hash, 16);
}

static void store_invalidate(struct mf_storage* s) {
    for (unsigned i = 0; i < STORE_FILES; i++) {
        if (s->files[i].fd >= 0) mf_disk_close(s->files[i].fd);
        s->files[i].fd = -1; s->files[i].path[0] = 0;
    }
    s->generation++;
}

static int store_fd(struct mf_storage* s, const char* path, int write, int* fd) {
    struct store_file* slot = NULL;
    for (unsigned i = 0; i < STORE_FILES; i++) {
        struct store_file* f = &s->files[i];
        if (f->fd >= 0 && !strcmp(f->path, path)) { f->touched = get_time_tb(); *fd = f->fd; return MF_OK; }
        if (!slot && f->fd < 0) slot = f;
    }
    if (!slot) return MF_BUSY;
    int rc = mf_disk_open(&s->root, path, 0, write, &slot->fd);
    if (rc) return rc;
    snprintf(slot->path, sizeof(slot->path), "%s", path); slot->touched = get_time_tb(); *fd = slot->fd;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: open path=%s fd=%d writable=%d descriptors=%u",
                path, *fd, write, mf_storage_open_count(s));
    return MF_OK;
}

struct scan_context {
    struct mf_storage* s;
    struct store_user* user;
    char path[MF_PATH_STORAGE_MAX];
    unsigned depth;
    int cleanup;
};

/* Стартовый обход: содержимое UID по всем группам; технические корни и .uploads не входят в счётчики. */
static int store_scan_entry(void* arg, const char* name, const struct mf_disk_info* info) {
    struct scan_context* c = arg;
    char child[MF_PATH_STORAGE_MAX];
    if (snprintf(child, sizeof(child), "%s%s%s", c->path, *c->path ? "/" : "", name) >= (int)sizeof(child)) return -MF_INVALID_PATH;
    if (c->cleanup) {
        if (strlen(name) != 32 || info->type != MF_FILE) return -MF_INVALID_PATH;
        for (const char* p = name; *p; p++) if (!strchr("0123456789abcdef", *p)) return -MF_INVALID_PATH;
        int rc = mf_disk_remove(&c->s->root, child, 0);
        return rc ? -rc : 0;
    }
    if (!strcmp(name, ".uploads") && !*c->path) return 0;
    struct store_user* user = c->user;
    if (c->depth < 2) {
        uint8_t owner[MF_ADDRESS_SIZE], x[MF_ADDRESS_SIZE];
        int valid = c->depth == 0 ? chat_channel_valid(name) :
            strlen(name) == 2 * MF_ADDRESS_SIZE && strspn(name, "0123456789abcdef") == 2 * MF_ADDRESS_SIZE &&
            !sc_hex_to_binary(name, owner, sizeof(owner)) && sc_identity_public_x("member files directory", owner, x) == SC_OK;
        if (info->type != MF_DIRECTORY || !valid) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: unexpected storage namespace path=%s", child);
            return -MF_INVALID_PATH;
        }
        if (c->depth == 1 && !(user = store_user_get(c->s, owner, 1))) return -MF_IO_ERROR;
    }
    if (info->type == MF_FILE) {
        if (info->size > UINT64_MAX - user->used || user->files == UINT64_MAX) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: scan size overflow owner=%s", sc_public_key_hex(user->owner, (char[65]){0}));
            return -MF_IO_ERROR;
        }
        user->used += info->size; user->files++; return 0;
    }
    if (c->depth >= 2) {
        if (user->directories == UINT64_MAX) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: scan directory count overflow owner=%s", sc_public_key_hex(user->owner, (char[65]){0}));
            return -MF_IO_ERROR;
        }
        user->directories++;
    }
    if (c->depth >= 64) return -MF_INVALID_PATH;
    struct scan_context next = { .s = c->s, .user = user, .depth = c->depth + 1 };
    snprintf(next.path, sizeof(next.path), "%s", child);
    int rc = mf_disk_list(&c->s->root, child, store_scan_entry, &next);
    return rc ? -rc : 0;
}

int mf_storage_initialize(struct mf_storage* s) {
    if (s->ready) return MF_OK;
    if (s->init_status != MF_BUSY) return s->init_status;
    uint64_t started = get_time_tb();
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: quota scan started root=%s user_quota=%llu", s->path, (unsigned long long)s->quota);
    int rc = mf_disk_root_open(&s->root, s->path);
    if (rc) goto failed;
    rc = mf_disk_mkdir(&s->root, ".uploads", NULL);
    if (rc == MF_EXISTS) rc = MF_OK;
    struct scan_context c = { .s = s, .cleanup = 1 };
    snprintf(c.path, sizeof(c.path), ".uploads");
    if (!rc) rc = mf_disk_list(&s->root, c.path, store_scan_entry, &c);
    c.cleanup = 0; c.path[0] = 0;
    if (!rc) rc = mf_disk_list(&s->root, "", store_scan_entry, &c);
    if (rc) { mf_disk_root_close(&s->root); goto failed; }
    s->ready = 1; s->init_status = MF_OK;
    for (unsigned i = 0; i < STORE_USERS; i++) for (struct store_user* u = s->users[i]; u; u = u->next) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: quota scan owner=%s used=%llu limit=%llu files=%llu directories=%llu",
            sc_public_key_hex(u->owner, (char[65]){0}), (unsigned long long)u->used, (unsigned long long)s->quota,
            (unsigned long long)u->files, (unsigned long long)u->directories);
        if (u->used > s->quota) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: user exceeds quota owner=%s used=%llu limit=%llu",
            sc_public_key_hex(u->owner, (char[65]){0}), (unsigned long long)u->used, (unsigned long long)s->quota);
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: quota scan ready root=%s users=%u elapsed_ms=%llu", s->path, s->user_count,
        (unsigned long long)((get_time_tb() - started) / 10));
    return MF_OK;
failed:
    store_users_free(s); s->init_status = rc;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: quota scan failed root=%s status=%s", s->path, mf_status_text(rc));
    return rc;
}

struct mf_storage* mf_storage_create(const char* root, uint64_t quota, uint64_t ttl) {
    if (!root || !*root || strlen(root) >= 2048 || !quota || !ttl) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: invalid storage settings root=%s quota=%llu ttl=%llu",
                    root ? root : "(null)", (unsigned long long)quota, (unsigned long long)ttl); return NULL;
    }
    struct mf_storage* s = u_calloc(1, sizeof(*s));
    if (!s || RAND_bytes(s->epoch, 16) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: storage allocation/random failed"); u_free(s); return NULL;
    }
    snprintf(s->path, sizeof(s->path), "%s", root); s->quota = quota; s->upload_ttl = ttl; s->init_status = MF_BUSY;
    for (unsigned i = 0; i < STORE_FILES; i++) s->files[i].fd = -1;
    return s;
}

unsigned mf_storage_open_count(const struct mf_storage* s) {
    unsigned count = 0;
    for (unsigned i = 0; i < STORE_FILES; i++) if (s->files[i].fd >= 0) count++;
    return count;
}

static int store_upload_discard(struct mf_storage* s, struct store_upload* u) {
    store_invalidate(s);
    int rc = mf_disk_remove(&s->root, u->temp, 0);
    if (rc && rc != MF_NOT_FOUND) {
        struct mf_disk_info after;
        if (mf_disk_stat(&s->root, u->temp, &after) != MF_NOT_FOUND) return rc;
    }
    int updated = store_user_update(u->user, 0, 0, u->reservation, 0, 0, "abort"); u->active = 0;
    return rc && rc != MF_NOT_FOUND ? rc : updated;
}

void mf_storage_sweep(struct mf_storage* s, uint64_t now) {
    if (!s) return;
    for (unsigned i = 0; i < STORE_FILES; i++) {
        struct store_file* f = &s->files[i];
        if (f->fd < 0 || now < f->touched || now - f->touched < MF_IDLE_TB) continue;
        int rc = mf_disk_close(f->fd); f->fd = -1;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: idle close path=%s status=%s idle_ms=%llu descriptors=%u",
                    f->path, mf_status_text(rc), (unsigned long long)((now - f->touched) / 10), mf_storage_open_count(s));
        f->path[0] = 0;
    }
    for (unsigned i = 0; i < STORE_UPLOADS; i++) {
        struct store_upload* u = &s->uploads[i];
        if (!u->active || now < u->touched || now - u->touched < s->upload_ttl) continue;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: upload expired group=%s owner=%s path=%s",
                   u->group, sc_public_key_hex(u->owner, (char[65]){0}), u->path);
        store_upload_discard(s, u);
    }
}

void mf_storage_destroy(struct mf_storage* s) {
    if (!s) return;
    store_invalidate(s);
    if (s->ready) {
        for (unsigned i = 0; i < STORE_UPLOADS; i++) if (s->uploads[i].active) store_upload_discard(s, &s->uploads[i]);
        mf_disk_root_close(&s->root);
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: storage stopped users=%u", s->user_count);
    store_users_free(s);
    u_free(s);
}

static int store_base(struct mf_storage* s, const char* group, const uint8_t owner[MF_ADDRESS_SIZE]) {
    char path[130]; struct mf_disk_info info;
    snprintf(path, sizeof(path), "%s", group);
    int rc = mf_disk_mkdir(&s->root, path, NULL);
    if (rc == MF_EXISTS) rc = mf_disk_stat(&s->root, path, &info);
    if (rc) return rc;
    snprintf(path, sizeof(path), "%s/%s", group, sc_public_key_hex(owner, (char[65]){0}));
    rc = mf_disk_mkdir(&s->root, path, NULL);
    if (rc == MF_EXISTS) rc = mf_disk_stat(&s->root, path, &info);
    return rc;
}

static int store_expected(struct mf_storage* s, const char* path, const uint8_t expected[16], struct mf_disk_info* info) {
    int rc = mf_disk_stat(&s->root, path, info);
    if (rc == MF_NOT_FOUND) return mf_zero(expected, 16) ? MF_OK : MF_STALE;
    if (rc) return rc;
    if (mf_zero(expected, 16)) return MF_EXISTS;
    uint8_t version[16]; store_version(s, info, version);
    return memcmp(expected, version, 16) ? MF_STALE : MF_OK;
}

struct list_context { struct mf_storage* s; const struct mf_message* req; struct mf_message* resp; uint64_t seen; };

static int store_list_entry(void* arg, const char* name, const struct mf_disk_info* info) {
    struct list_context* c = arg;
    if (!mf_path_valid(name, 0)) return -MF_INVALID_PATH;
    if (c->seen++ < c->req->cursor) return 0;
    size_t n = strlen(name), at = c->resp->data_len;
    if (at + MF_ENTRY_HEADER + n > MF_BLOCK_SIZE) { c->resp->more = 1; return 1; }
    uint8_t* p = c->resp->data + at;
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); p[2] = info->type;
    mf_put64(p + 3, info->size); mf_put64(p + 11, info->mtime); store_version(c->s, info, p + 19);
    memcpy(p + MF_ENTRY_HEADER, name, n);
    c->resp->data_len += (uint32_t)(MF_ENTRY_HEADER + n); c->resp->cursor++;
    return 0;
}

static struct store_upload* store_upload_find(struct mf_storage* s, const char* group, const uint8_t peer[MF_ADDRESS_SIZE], const uint8_t id[16]) {
    for (unsigned i = 0; i < STORE_UPLOADS; i++) {
        struct store_upload* u = &s->uploads[i];
        if (u->active && !strcmp(u->group, group) && !memcmp(u->peer, peer, MF_ADDRESS_SIZE) && !memcmp(u->id, id, 16)) return u;
    }
    return NULL;
}

static int store_begin(struct mf_storage* s, const char* group, const uint8_t peer[MF_ADDRESS_SIZE], const char* path, const struct mf_message* q) {
    if (q->size > INT64_MAX || mf_zero(q->upload, 16)) return MF_INVALID_RANGE;
    if (!strncmp(q->path, "avatars/", 8)) {
        if (!q->size || q->size > mf_avatar_file_limit(q->path + 8)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: invalid object path=%s bytes=%llu", q->path, (unsigned long long)q->size);
            return MF_INVALID_RANGE;
        }
        if (!mf_zero(q->version, 16)) return MF_EXISTS;
        char hash[65]; for (unsigned i = 0; i < 32; i++) snprintf(hash + i * 2, 3, "%02x", q->hash[i]);
        if (strncmp(q->path + 8, hash, 64)) return MF_HASH_MISMATCH;
    }
    struct store_upload* u = store_upload_find(s, group, peer, q->upload);
    if (u) {
        if (strcmp(u->path, path) || u->size != q->size || memcmp(u->hash, q->hash, 32) || memcmp(u->expected, q->version, 16))
            return MF_BAD_PACKET;
        int fd;
        int rc = store_fd(s, u->temp, 1, &fd);
        if (!rc) u->touched = get_time_tb();
        return rc;
    }
    struct store_upload* slot = NULL;
    for (unsigned i = 0; i < STORE_UPLOADS; i++) {
        if (s->uploads[i].active && !strcmp(s->uploads[i].path, path)) return MF_BUSY;
        if (!s->uploads[i].active && !slot) slot = &s->uploads[i];
    }
    if (!slot) return MF_BUSY;
    struct mf_disk_info existing = {0};
    int rc = store_expected(s, path, q->version, &existing);
    if (rc) return rc;
    if (existing.type && existing.type != MF_FILE) return MF_INVALID_PATH;
    struct store_user* user = store_user_get(s, q->owner, 1);
    if (!user) return MF_IO_ERROR;
    if (!user->valid || existing.size > user->used) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: quota inconsistent before upload owner=%s used=%llu old_size=%llu",
            sc_public_key_hex(q->owner, (char[65]){0}), (unsigned long long)user->used, (unsigned long long)existing.size);
        user->valid = 0; return MF_IO_ERROR;
    }
    uint64_t reservation = q->size > existing.size ? q->size - existing.size : 0;
    if (reservation && (user->used > s->quota || user->reserved > s->quota - user->used ||
        reservation > s->quota - user->used - user->reserved)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: user quota exceeded owner=%s used=%llu reserved=%llu growth=%llu limit=%llu",
            sc_public_key_hex(q->owner, (char[65]){0}), (unsigned long long)user->used, (unsigned long long)user->reserved,
            (unsigned long long)reservation, (unsigned long long)s->quota);
        return MF_NO_SPACE;
    }
    rc = store_base(s, group, q->owner);
    if (rc) return rc;
    memset(slot, 0, sizeof(*slot));
    char hex[33]; for (unsigned i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", q->upload[i]);
    snprintf(slot->temp, sizeof(slot->temp), ".uploads/%s", hex);
    int fd = -1;
    rc = mf_disk_open(&s->root, slot->temp, 1, 1, &fd);
    if (rc) return rc;
    rc = mf_disk_resize(fd, q->size);
    struct store_file* cached = NULL;
    if (!rc) for (unsigned i = 0; i < STORE_FILES; i++) if (s->files[i].fd < 0) { cached = &s->files[i]; break; }
    if (!rc && !cached) rc = MF_BUSY;
    if (rc) { mf_disk_close(fd); mf_disk_remove(&s->root, slot->temp, 0); return rc; }
    cached->fd = fd; cached->touched = get_time_tb(); snprintf(cached->path, sizeof(cached->path), "%s", slot->temp);
    slot->active = 1; strcpy(slot->group, group); memcpy(slot->peer, peer, MF_ADDRESS_SIZE); memcpy(slot->owner, q->owner, MF_ADDRESS_SIZE); slot->size = q->size;
    slot->user = user; slot->previous = existing.size; slot->reservation = reservation;
    memcpy(slot->id, q->upload, 16); memcpy(slot->hash, q->hash, 32); memcpy(slot->expected, q->version, 16);
    snprintf(slot->path, sizeof(slot->path), "%s", path); slot->touched = get_time_tb(); user->reserved += reservation;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: begin group=%s owner=%s path=%s size=%llu reserved=%llu",
               group, sc_public_key_hex(q->owner, (char[65]){0}), path, (unsigned long long)q->size, (unsigned long long)user->reserved);
    return MF_OK;
}

static int store_command(struct mf_storage* s, const char* group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* q, struct mf_message* r) {
    if (!mf_path_valid(q->path, q->command == MF_LIST || q->command == MF_STAT || q->command == MF_QUOTA) ||
        (q->command == MF_RENAME && !mf_path_valid(q->target, 0))) return MF_INVALID_PATH;
    if (store_avatar_namespace(q->path) && strcmp(q->path, "avatars") &&
        (strncmp(q->path, "avatars/", 8) || !mf_avatar_file_valid(q->path + 8))) return MF_INVALID_PATH;
    if (q->command == MF_RENAME && (store_avatar_namespace(q->path) || store_avatar_namespace(q->target))) return MF_INVALID_PATH;
    char path[MF_PATH_STORAGE_MAX];
    snprintf(path, sizeof(path), "%s/%s%s%s", group, sc_public_key_hex(q->owner, (char[65]){0}), *q->path ? "/" : "", q->path);
    struct mf_disk_info info = {0};
    int rc, fd;
    if (q->command == MF_QUOTA) {
        if (*q->path || *q->target || q->cursor || q->data_len) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: invalid quota request owner=%s path=%s target=%s cursor=%llu data=%u",
                sc_public_key_hex(q->owner, (char[65]){0}), q->path, q->target, (unsigned long long)q->cursor, q->data_len);
            return MF_BAD_PACKET;
        }
        struct store_user* user = store_user_get(s, q->owner, 0);
        if (user && !user->valid) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: quota account invalid owner=%s; restart scan required", sc_public_key_hex(q->owner, (char[65]){0}));
            return MF_IO_ERROR;
        }
        mf_put64(r->data, user ? user->used : 0); mf_put64(r->data + 8, s->quota);
        mf_put64(r->data + 16, user ? user->reserved : 0);
        mf_put64(r->data + 24, user ? user->files : 0); mf_put64(r->data + 32, user ? user->directories : 0);
        r->data_len = MF_QUOTA_BYTES;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: quota response owner=%s used=%llu reserved=%llu limit=%llu files=%llu directories=%llu",
            sc_public_key_hex(q->owner, (char[65]){0}), (unsigned long long)(user ? user->used : 0),
            (unsigned long long)(user ? user->reserved : 0), (unsigned long long)s->quota,
            (unsigned long long)(user ? user->files : 0), (unsigned long long)(user ? user->directories : 0));
        return MF_OK;
    }
    if (q->command == MF_BEGIN_WRITE) return store_begin(s, group, peer, path, q);
    if (q->command == MF_WRITE || q->command == MF_COMMIT || q->command == MF_ABORT) {
        struct store_upload* u = store_upload_find(s, group, peer, q->upload);
        if (!u) return q->command == MF_ABORT ? MF_OK : MF_NOT_FOUND;
        if (memcmp(u->owner, q->owner, MF_ADDRESS_SIZE) || strcmp(u->path, path)) return MF_ACCESS_DENIED;
        u->touched = get_time_tb();
        if (q->command == MF_ABORT) return store_upload_discard(s, u);
        if (q->command == MF_WRITE && (q->size != q->data_len || q->size > MF_BLOCK_SIZE ||
            q->offset > u->size || q->size > u->size - q->offset)) return MF_INVALID_RANGE;
        rc = store_fd(s, u->temp, 1, &fd);
        if (rc) return rc;
        if (q->command == MF_WRITE) return mf_disk_write(fd, q->offset, q->data, q->data_len);
        uint8_t hash[32];
        rc = mf_disk_hash(fd, u->size, hash);
        if (rc) return rc;
        if (memcmp(hash, u->hash, 32)) return MF_HASH_MISMATCH;
        rc = mf_disk_sync(fd);
        if (rc) return rc;
        rc = store_expected(s, path, u->expected, &info);
        if (rc) return rc;
        if (!u->user->valid || info.size != u->previous) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: quota/old size changed before commit owner=%s", sc_public_key_hex(q->owner, (char[65]){0}));
            return MF_IO_ERROR;
        }
        uint64_t previous = info.size;
        int new_file = info.type != MF_FILE; // Нулевой размер существующего файла не означает создание нового.
        store_invalidate(s);
        rc = mf_disk_rename(&s->root, u->temp, path);
        /* Даже при ошибке fsync rename уже мог произойти; сверяем существование temp. */
        struct mf_disk_info temp;
        int temp_status = mf_disk_stat(&s->root, u->temp, &temp);
        if (temp_status != MF_NOT_FOUND) {
            if (!rc || temp_status) {
                u->user->valid = 0;
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: commit outcome uncertain owner=%s rename=%s temp_stat=%s",
                    sc_public_key_hex(q->owner, (char[65]){0}), mf_status_text(rc), mf_status_text(temp_status));
            }
            return rc ? rc : temp_status ? temp_status : MF_IO_ERROR;
        }
        int updated = store_user_update(u->user, previous, u->size, u->reservation, new_file, 0, "commit"); u->active = 0;
        if (rc) return rc;
        if (updated) return updated;
        rc = mf_disk_stat(&s->root, path, &info);
        if (rc) return rc;
        r->size = info.size; r->mtime = info.mtime; r->type = info.type;
        store_version(s, &info, r->version); memcpy(r->hash, hash, 32);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: published path=%s size=%llu used=%llu", path,
                   (unsigned long long)r->size, (unsigned long long)u->user->used);
        return MF_OK;
    }
    if (q->command == MF_MKDIR) {
        for (unsigned i = 0; i < STORE_UPLOADS; i++)
            if (s->uploads[i].active && !strcmp(s->uploads[i].path, path)) return MF_BUSY;
        struct store_user* user = store_user_get(s, q->owner, 1);
        if (!user) return MF_IO_ERROR;
        if (!user->valid) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: mkdir quota account invalid owner=%s", sc_public_key_hex(q->owner, (char[65]){0}));
            return MF_IO_ERROR;
        }
        rc = store_base(s, group, q->owner);
        if (!rc) {
            int created;
            store_invalidate(s); rc = mf_disk_mkdir(&s->root, path, &created);
            if (created) {
                int updated = store_user_update(user, 0, 0, 0, 0, 1, "mkdir");
                if (!rc) rc = updated;
            }
        }
        return rc;
    }
    rc = mf_disk_stat(&s->root, path, &info);
    if (rc == MF_NOT_FOUND && q->command == MF_LIST && !*q->path && !q->cursor) return MF_OK;
    if (rc) return rc;
    uint8_t version[16]; store_version(s, &info, version);
    if ((!mf_zero(q->version, 16) && memcmp(q->version, version, 16)) ||
        ((q->command == MF_READ || q->command == MF_RENAME || q->command == MF_REMOVE_PATH || q->cursor) && mf_zero(q->version, 16)))
        return MF_STALE;
    r->size = info.size; r->mtime = info.mtime; r->type = info.type; memcpy(r->version, version, 16);
    if (q->command == MF_STAT) return MF_OK;
    if (q->command == MF_LIST) {
        if (info.type != MF_DIRECTORY) return MF_INVALID_PATH;
        struct list_context c = { .s = s, .req = q, .resp = r };
        r->cursor = q->cursor;
        return mf_disk_list(&s->root, path, store_list_entry, &c);
    }
    if (q->command == MF_READ) {
        if (info.type != MF_FILE || q->offset > info.size || !q->size || q->size > MF_BLOCK_SIZE) return MF_INVALID_RANGE;
        rc = store_fd(s, path, 0, &fd);
        if (rc) return rc;
        struct mf_disk_info opened; rc = mf_disk_info(fd, &opened);
        if (rc) return rc;
        store_version(s, &opened, version);
        if (memcmp(version, q->version, 16)) { store_invalidate(s); return MF_STALE; }
        r->data_len = (uint32_t)(info.size - q->offset < q->size ? info.size - q->offset : q->size);
        r->more = q->offset + r->data_len < info.size;
        rc = mf_disk_read(fd, q->offset, r->data, r->data_len);
        if (!rc) {
            rc = mf_disk_info(fd, &opened); store_version(s, &opened, version);
            if (!rc && memcmp(version, q->version, 16)) rc = MF_STALE;
        }
        return rc;
    }
    for (unsigned i = 0; i < STORE_UPLOADS; i++) {
        struct store_upload* u = &s->uploads[i];
        size_t n = strlen(path);
        if (u->active && !strncmp(u->path, path, n) && (!u->path[n] || u->path[n] == '/')) return MF_BUSY;
    }
    store_invalidate(s);
    if (q->command == MF_REMOVE_PATH) {
        struct store_user* user = store_user_get(s, q->owner, 1);
        if (!user) return MF_IO_ERROR;
        rc = mf_disk_remove(&s->root, path, info.type == MF_DIRECTORY);
        int removed = rc == MF_OK;
        if (rc) {
            struct mf_disk_info after; int checked = mf_disk_stat(&s->root, path, &after);
            removed = checked == MF_NOT_FOUND;
            if (user && checked && !removed) {
                user->valid = 0;
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: remove outcome uncertain owner=%s path=%s", sc_public_key_hex(q->owner, (char[65]){0}), path);
            }
        }
        if (removed && user) {
            int updated = store_user_update(user, info.size, 0, 0, info.type == MF_FILE ? -1 : 0,
                info.type == MF_DIRECTORY ? -1 : 0, "remove");
            if (!rc) rc = updated;
        }
        return rc;
    }
    if (q->command == MF_RENAME) {
        char target[MF_PATH_STORAGE_MAX];
        snprintf(target, sizeof(target), "%s/%s/%s", group, sc_public_key_hex(q->owner, (char[65]){0}), q->target);
        for (unsigned i = 0; i < STORE_UPLOADS; i++)
            if (s->uploads[i].active && !strcmp(s->uploads[i].path, target)) return MF_BUSY;
        struct mf_disk_info destination;
        rc = mf_disk_stat(&s->root, target, &destination);
        if (!rc) return MF_EXISTS;
        if (rc != MF_NOT_FOUND) return rc;
        return mf_disk_rename(&s->root, path, target);
    }
    return MF_BAD_PACKET;
}

void mf_storage_execute(struct mf_storage* s, const char* group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* q, struct mf_message* r) {
    *r = *q; r->kind = MF_RESPONSE; r->status = MF_OK; r->data_len = 0; r->more = 0;
    if (!chat_channel_valid(group) || !peer || mf_zero(peer, MF_ADDRESS_SIZE) || mf_zero(q->owner, MF_ADDRESS_SIZE) || (q->command >= MF_BEGIN_WRITE && q->command != MF_QUOTA && memcmp(q->owner, peer, MF_ADDRESS_SIZE))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: storage ACL denied group=%s peer=%s owner=%s command=%u",
                   group, sc_public_key_hex(peer, (char[65]){0}), sc_public_key_hex(q->owner, (char[65]){0}), q->command);
        r->status = MF_ACCESS_DENIED; return;
    }
    uint64_t now = get_time_tb();
    if (!s->ready) {
        r->status = (uint16_t)s->init_status;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: storage not ready root=%s status=%s", s->path, mf_status_text(r->status));
        return;
    }
    int mutates = q->command == MF_COMMIT || q->command == MF_MKDIR || q->command == MF_RENAME || q->command == MF_REMOVE_PATH;
    uint64_t first_expiry = UINT64_MAX;
    if (mutates) r->cursor = 0;
    struct store_receipt* slot = NULL;
    uint8_t wire[MF_WIRE_MAX], fingerprint[32];
    int n = mf_encode(q, wire, sizeof(wire));
    if (n < 0) { r->status = MF_BAD_PACKET; return; }
    SHA256(wire, (size_t)n, fingerprint);
    for (unsigned i = 0; i < STORE_RECEIPTS; i++) {
        struct store_receipt* p = &s->receipts[i];
        if (p->until <= now) { if (!slot) slot = p; continue; }
        if (p->until < first_expiry) first_expiry = p->until;
        if (strcmp(p->group, group) || memcmp(p->peer, peer, MF_ADDRESS_SIZE)) continue;
        if (p->response.request_id == q->request_id) {
            if (memcmp(p->fingerprint, fingerprint, 32)) r->status = MF_BAD_PACKET;
            else *r = p->response;
            return;
        }
        if (q->command == MF_COMMIT && p->response.command == MF_COMMIT && !memcmp(p->response.owner, q->owner, MF_ADDRESS_SIZE) &&
            !strcmp(q->path, p->response.path) && !memcmp(q->upload, p->response.upload, 16)) {
            *r = p->response; r->request_id = q->request_id; return;
        }
    }
    if (mutates && !slot) {
        r->status = MF_BUSY; r->cursor = first_expiry - now;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: receipts full id=%llu retry_after_tb=%llu",
            (unsigned long long)q->request_id, (unsigned long long)r->cursor);
    }
    else {
        mf_storage_sweep(s, now); r->status = (uint16_t)store_command(s, group, peer, q, r);
        /* Idle отсчитывается от завершения I/O, а не от начала медленного read/write. */
        if (!r->status && (q->command == MF_READ || q->command == MF_WRITE || q->command == MF_BEGIN_WRITE)) {
            char accessed[MF_PATH_STORAGE_MAX];
            if (q->command == MF_READ) snprintf(accessed, sizeof(accessed), "%s/%s/%s",
                group, sc_public_key_hex(q->owner, (char[65]){0}), q->path);
            else {
                struct store_upload* u = store_upload_find(s, group, peer, q->upload);
                if (u) { snprintf(accessed, sizeof(accessed), "%s", u->temp); u->touched = get_time_tb(); }
                else accessed[0] = 0;
            }
            for (unsigned i = 0; i < STORE_FILES; i++) if (s->files[i].fd >= 0 && !strcmp(s->files[i].path, accessed))
                s->files[i].touched = get_time_tb();
        }
    }
    if (!r->status && mutates) {
        strcpy(slot->group, group); memcpy(slot->peer, peer, MF_ADDRESS_SIZE); slot->until = now + MF_REPLAY_TB;
        memcpy(slot->fingerprint, fingerprint, 32); slot->response = *r;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "member_files: storage result id=%llu cmd=%s status=%s bytes=%u more=%u cursor=%llu",
        (unsigned long long)q->request_id, mf_command_text(q->command), mf_status_text(r->status), r->data_len, r->more,
        (unsigned long long)r->cursor);
    if (r->status) {
        r->data_len = 0;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: refused group=%s peer=%s owner=%s cmd=%u path=%s offset=%llu size=%llu status=%s",
                   group, sc_public_key_hex(peer, (char[65]){0}), sc_public_key_hex(q->owner, (char[65]){0}), q->command, q->path,
                   (unsigned long long)q->offset, (unsigned long long)q->size, mf_status_text(r->status));
    }
}
