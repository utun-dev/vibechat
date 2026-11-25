/* Один handle на поток файла. Router управляет отправкой и подтверждениями;
 * ограниченное кольцо связывает uasync с общим FIFO дискового worker. */
#include "private.h"
#include "disk.h"
#include "../utun_instance.h"
#include "../chat/chat_event.h"
#include "../routing_layer/conn_mgr.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/etcp_router.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <string.h>
#include <stdio.h>

#define TRANSFER_SLOTS 32
#define TRANSFER_BATCH 16
#define TRANSFER_MAX 16
#define TRANSFER_TIMEOUT_TB 1200000ULL

enum transfer_work { TRANSFER_PREPARE, TRANSFER_READ, TRANSFER_WRITE, TRANSFER_VERIFY, TRANSFER_PUBLISH, TRANSFER_FINISH };
struct mf_chunk { uint32_t bytes; uint8_t data[MF_BLOCK_SIZE]; };
struct mf_transfer {
    struct mf_transfer* next;
    struct member_files* files;
    struct mf_transfer_request request;
    char group_key[65];
    struct mf_pending* pending;
    struct CONN_MGR_HANDLE* cm;
    struct mf_work* cleanup;
    struct queue_waiter_handle waiter;
    void* pump;
    mf_transfer_fn callback;
    void* arg;
    struct mf_message open, result, outgoing;
    struct mf_disk_info source;
    EVP_MD_CTX* digest;
    int fd, server, sending, starting, active, opened, busy, stopping, finished_io, peer_down;
    int status, work_status, reply_ready, reply_final, verified, committed, end_sent, end_received;
    int temp_owned; /* Только worker: локальный download-temp успешно создан через O_EXCL. */
    int cleanup_abort; /* Неизменяемый снимок для обязательной очистки; worker не читает живой status. */
    enum transfer_work work;
    uint64_t size, offset, received, read_offset, io_offset, io_bytes, io_size, progress_tb, last_event, last_log, logged_offset;
    unsigned head, count, io_head, io_count;
    uint8_t upload[16], version[16], hash[32];
    char temp[MF_PATH_MAX + 80];
    struct mf_chunk chunks[TRANSFER_SLOTS];
};

static void transfer_pump(void* arg);
static void transfer_submit(struct mf_transfer* t, enum transfer_work work);
static void transfer_stop(struct mf_transfer* t, int status);
static void transfer_send_open(struct mf_transfer* t, uint8_t command);

static void transfer_event(struct mf_transfer* t, int finished) {
    if (t->server || !t->callback) return;
    uint64_t now = get_time_tb();
    if (!finished && t->last_event && now - t->last_event < 1000) return;
    t->last_event = now;
    struct mf_transfer_progress progress = { t->request.id, t->offset, t->size, (uint16_t)t->status, (uint8_t)finished };
    t->callback(t->arg, &progress);
}

static void transfer_wake(struct mf_transfer* t) {
    if (t->stopping && !t->reply_ready) return;
    if (t->pump || t->waiter.internal || t->waiter.call_soon_id || t->files->closing) return;
    t->pump = uasync_call_soon(t->files->inst->ua, t, transfer_pump);
    if (!t->pump) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: stream pump allocation failed id=%llu", (unsigned long long)t->request.id);
}
static void transfer_ready(struct ll_queue* q, void* arg) { (void)q; transfer_wake(arg); }

static struct mf_message transfer_packet(struct mf_transfer* t, uint8_t command) {
    struct mf_message q = { .kind = t->server ? MF_RESPONSE : MF_REQUEST, .command = command,
        .request_id = t->request.id, .offset = t->offset, .size = t->size };
    memcpy(q.owner, t->request.owner, MF_ADDRESS_SIZE);
    snprintf(q.path, sizeof(q.path), "%s", t->request.remote);
    memcpy(q.upload, t->upload, 16); memcpy(q.version, t->version, 16); memcpy(q.hash, t->hash, 32);
    return q;
}

static struct mf_message transfer_storage_request(struct mf_transfer* t, uint8_t command) {
    struct mf_message q = t->open;
    q.command = command; q.kind = MF_REQUEST; q.data_len = 0;
    return q;
}

static int transfer_allowed(struct mf_transfer* t) {
    struct UTUN_INSTANCE* inst = t->files->inst;
    if (!t->server) {
        struct mf_message access = {.command=t->request.upload ? MF_STREAM_WRITE : MF_STREAM_READ};
        memcpy(access.owner, t->request.owner, MF_ADDRESS_SIZE);
        memcpy(access.path, t->request.remote, sizeof(access.path));
        return member_files_member(inst, t->request.group, inst->my_ed25519_pubkey, 0) &&
            mf_storage_allowed(inst, t->request.group, t->request.server, &access) &&
            member_files_member(inst, t->request.group, t->request.owner, 0);
    }
    struct mf_message q = t->open;
    return mf_server_allowed(t->files, t->request.group, t->request.server, &q);
}

static void transfer_release(struct mf_transfer* t) {
    struct member_files* f = t->files;
    if (t->pump) uasync_call_soon_cancel(f->inst->ua, t->pump);
    etcp_router_cancel_send_ready(&t->waiter);
    if (t->cm) conn_mgr_close(t->cm);
    mf_worker_unreserve(t->cleanup);
    struct mf_transfer** link = &f->transfers;
    while (*link && *link != t) link = &(*link)->next;
    if (*link) *link = t->next;
    if (t->server) f->server_transfer_count--; else f->transfer_count--;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: stream finished id=%llu server=%d sending=%d bytes=%llu/%llu status=%s",
        (unsigned long long)t->request.id, t->server, t->sending, (unsigned long long)t->offset,
        (unsigned long long)t->size, mf_status_text(t->status));
    mf_transfer_fn callback = t->callback; void* arg = t->arg;
    struct mf_transfer_progress progress = { t->request.id, t->offset, t->size, (uint16_t)t->status, 1 };
    int server = t->server;
    u_free(t);
    mf_receive_resume(f);
    if (!server && callback) callback(arg, &progress);
}

/* Worker читает только снимок io_*; uasync не меняет занятые этим снимком ячейки. */
static void transfer_work(void* arg) {
    struct mf_transfer* t = arg;
    int rc = MF_OK;
    if (t->work == TRANSFER_PREPARE) {
        if (t->server) {
            struct mf_message q = t->open;
            q.command = t->sending ? MF_STAT : MF_BEGIN_WRITE;
            mf_storage_execute(t->files->storage, t->group_key, t->request.server, &q, &t->result);
            rc = t->result.status;
            if (!rc && t->sending && (t->result.type != MF_FILE || t->result.size != t->size ||
                memcmp(t->result.version, t->version, 16))) rc = MF_STALE;
        } else if (t->sending) {
            rc = mf_disk_local_open(t->request.local, 0, &t->fd);
            if (!rc) rc = mf_disk_info(t->fd, &t->source);
            if (!rc && (t->source.type != MF_FILE || t->source.size > INT64_MAX ||
                (t->request.max_bytes && t->source.size > t->request.max_bytes))) rc = MF_INVALID_RANGE;
            if (!rc) { t->io_size = t->source.size; rc = mf_disk_hash(t->fd, t->io_size, t->hash); }
            struct mf_disk_info after;
            if (!rc) rc = mf_disk_info(t->fd, &after);
            if (!rc && memcmp(&after, &t->source, sizeof(after))) rc = MF_STALE;
        } else {
            rc = mf_disk_local_open(t->temp, 1, &t->fd);
            if (!rc) t->temp_owned = 1;
            DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: download temp id=%llu created=%d status=%s path=%s",
                (unsigned long long)t->request.id, t->temp_owned, mf_status_text(rc), t->temp);
        }
        if (!rc && t->request.upload == 0) {
            t->digest = EVP_MD_CTX_new();
            if (!t->digest || EVP_DigestInit_ex(t->digest, EVP_sha256(), NULL) != 1) rc = MF_IO_ERROR;
        }
    } else if (t->work == TRANSFER_READ || t->work == TRANSFER_WRITE) {
        uint64_t offset = t->io_offset; t->io_bytes = 0;
        struct mf_disk_info info;
        if (!t->server && t->sending) {
            rc = mf_disk_info(t->fd, &info);
            if (!rc && memcmp(&info, &t->source, sizeof(info))) rc = MF_STALE;
        }
        for (unsigned i = 0; !rc && i < t->io_count; i++) {
            struct mf_chunk* chunk = &t->chunks[(t->io_head + i) % TRANSFER_SLOTS];
            if (t->sending) chunk->bytes = (uint32_t)(t->size - offset < MF_BLOCK_SIZE ? t->size - offset : MF_BLOCK_SIZE);
            if (t->server) {
                struct mf_message q = transfer_storage_request(t, t->sending ? MF_READ : MF_WRITE);
                q.kind = MF_REQUEST; q.offset = offset; q.size = chunk->bytes;
                if (!t->sending) { q.data_len = chunk->bytes; memcpy(q.data, chunk->data, chunk->bytes); }
                struct mf_message r;
                mf_storage_execute(t->files->storage, t->group_key, t->request.server, &q, &r);
                rc = r.status;
                if (!rc && t->sending) {
                    if (r.data_len != chunk->bytes || r.size != t->size || memcmp(r.version, t->version, 16)) rc = MF_STALE;
                    else memcpy(chunk->data, r.data, r.data_len);
                }
            } else if (t->sending) rc = mf_disk_read(t->fd, offset, chunk->data, chunk->bytes);
            else rc = mf_disk_write(t->fd, offset, chunk->data, chunk->bytes);
            if (!rc && t->digest && EVP_DigestUpdate(t->digest, chunk->data, chunk->bytes) != 1) rc = MF_IO_ERROR;
            if (!rc) { offset += chunk->bytes; t->io_bytes += chunk->bytes; }
        }
    } else if (t->work == TRANSFER_VERIFY) {
        if (t->server && !t->sending) {
            struct mf_message q = transfer_storage_request(t, MF_COMMIT);
            mf_storage_execute(t->files->storage, t->group_key, t->request.server, &q, &t->result);
            rc = t->result.status;
            t->committed = rc == MF_OK;
        } else {
            if (t->digest) {
                uint8_t hash[32]; unsigned bytes = 0;
                if (EVP_DigestFinal_ex(t->digest, hash, &bytes) != 1 || bytes != 32) rc = MF_IO_ERROR;
                else if (t->sending) memcpy(t->hash, hash, 32);
                else if (memcmp(t->hash, hash, 32)) rc = MF_HASH_MISMATCH;
            }
            if (!rc && t->server) {
                struct mf_message q = transfer_storage_request(t, MF_STAT);
                mf_storage_execute(t->files->storage, t->group_key, t->request.server, &q, &t->result);
                rc = t->result.status;
                if (!rc && (t->result.size != t->size || memcmp(t->result.version, t->version, 16))) rc = MF_STALE;
            } else if (!rc && t->sending) {
                struct mf_disk_info after; rc = mf_disk_info(t->fd, &after);
                if (!rc && memcmp(&after, &t->source, sizeof(after))) rc = MF_STALE;
            }
        }
    } else if (t->work == TRANSFER_PUBLISH) {
        rc = mf_disk_sync(t->fd);
        int closed = mf_disk_close(t->fd); t->fd = -1;
        if (!rc) rc = closed;
        if (!rc) rc = mf_disk_local_publish(t->temp, t->request.local);
        if (!rc) t->temp_owned = 0;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: download publish id=%llu status=%s path=%s",
                    (unsigned long long)t->request.id, mf_status_text(rc), t->request.local);
    } else {
        if (t->cleanup_abort) {
            struct mf_message q = transfer_storage_request(t, MF_ABORT);
            mf_storage_execute(t->files->storage, t->group_key, t->request.server, &q, &t->result);
            if (t->result.status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: stream cleanup failed id=%llu status=%s",
                (unsigned long long)t->request.id, mf_status_text(t->result.status));
        }
        if (t->fd >= 0) {
            rc = mf_disk_close(t->fd); t->fd = -1;
        }
        if (!t->server && !t->sending && t->temp_owned) {
            int removed = mf_disk_local_remove(t->temp);
            if (removed) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: owned temp cleanup failed id=%llu path=%s status=%s",
                                     (unsigned long long)t->request.id, t->temp, mf_status_text(removed));
            else t->temp_owned = 0;
            if (!rc) rc = removed;
        }
        EVP_MD_CTX_free(t->digest); t->digest = NULL;
    }
    t->work_status = rc;
}

static void transfer_done(void* arg, int error) {
    struct mf_transfer* t = arg; t->busy = 0;
    int status = error ? MF_CANCELLED : t->work_status;
    if (!status && !transfer_allowed(t)) {
        status = MF_ACCESS_DENIED; t->peer_down = 1;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: disk result revoked group=%llx peer=%s id=%llu work=%d",
            (unsigned long long)t->request.group, (unsigned long long)t->request.server,
            (unsigned long long)t->request.id, t->work);
    }
    if (t->work == TRANSFER_PUBLISH) {
        if (!t->status) t->status = status;
        transfer_submit(t, TRANSFER_FINISH); return;
    }
    if (t->work == TRANSFER_FINISH) {
        t->finished_io = 1;
        if (!t->status) t->status = status;
        if (t->files->closing || t->peer_down || (!t->server && !t->status)) { transfer_release(t); return; }
        if (!t->server) {
            t->outgoing = transfer_packet(t, MF_STREAM_CANCEL);
            t->reply_ready = t->reply_final = 1; transfer_wake(t); return;
        }
        t->outgoing = t->opened ? transfer_packet(t, MF_STREAM_END) : t->open;
        t->outgoing.kind = MF_RESPONSE; t->outgoing.data_len = 0; t->outgoing.status = (uint16_t)t->status;
        t->reply_ready = t->reply_final = 1; transfer_wake(t); return;
    }
    if (t->stopping || t->files->closing || status) { transfer_stop(t, status ? status : MF_CANCELLED); return; }
    t->progress_tb = get_time_tb();
    if (t->work == TRANSFER_PREPARE) {
        if (!t->server && t->sending) t->size = t->io_size;
        if (t->server) {
            t->outgoing = t->open; t->outgoing.kind = MF_RESPONSE; t->outgoing.data_len = 0;
            t->outgoing.size = t->size; t->outgoing.status = MF_OK;
            t->reply_ready = 1;
        } else { transfer_send_open(t, t->sending ? MF_STREAM_WRITE : MF_STREAM_READ); return; }
    } else if (t->work == TRANSFER_READ) {
        t->read_offset += t->io_bytes; t->count += t->io_count;
    } else if (t->work == TRANSFER_WRITE) {
        t->offset += t->io_bytes; t->head = (t->head + t->io_count) % TRANSFER_SLOTS; t->count -= t->io_count;
        mf_receive_resume(t->files);
    } else {
        t->verified = 1;
        if (t->server || !t->sending) { transfer_stop(t, MF_OK); return; }
    }
    transfer_wake(t);
}

static void transfer_submit(struct mf_transfer* t, enum transfer_work work) {
    if (work != TRANSFER_FINISH && !transfer_allowed(t)) { transfer_stop(t, MF_ACCESS_DENIED); return; }
    t->work = work; t->busy = 1;
    if (work == TRANSFER_FINISH) {
        t->cleanup_abort = t->server && !t->sending && t->status && !t->committed;
        struct mf_work* cleanup = t->cleanup; t->cleanup = NULL;
        mf_worker_submit_reserved(t->files->disk, cleanup, transfer_work, t, transfer_done, t);
    } else mf_worker_submit(t->files->disk, transfer_work, t, transfer_done, t);
}

static void transfer_stop(struct mf_transfer* t, int status) {
    if (status && !t->status) {
        t->status = status;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: stream stopped id=%llu peer=%s reason=%s",
            (unsigned long long)t->request.id, (unsigned long long)t->request.server, mf_status_text(status));
    }
    if (t->stopping) {
        if (t->busy && t->work != TRANSFER_FINISH) mf_worker_cancel(t->files->disk, t);
        else if ((t->files->closing || status) && t->finished_io && !t->busy) transfer_release(t);
        else if (!t->busy && !t->finished_io) transfer_submit(t, TRANSFER_FINISH);
        return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: stream stopping id=%llu starting=%d active=%d io=%d pending=%d",
        (unsigned long long)t->request.id, t->starting, t->active, t->busy, t->pending != NULL);
    t->stopping = 1; t->starting = 0;
    if (t->pump) { uasync_call_soon_cancel(t->files->inst->ua, t->pump); t->pump = NULL; }
    etcp_router_cancel_send_ready(&t->waiter);
    if (t->pending) { struct mf_pending* p = t->pending; t->pending = NULL; member_files_cancel(p); }
    if (t->busy) { if (t->work != TRANSFER_FINISH) mf_worker_cancel(t->files->disk, t); return; }
    if (t->finished_io) { transfer_release(t); return; }
    transfer_submit(t, !t->status && !t->server && !t->sending ? TRANSFER_PUBLISH : TRANSFER_FINISH);
}

static int transfer_enqueue(struct mf_transfer* t, struct mf_message* q) {
    struct ETCP_ROUTER_CONN* blocked;
    int rc = mf_send(t->files, t->request.group, t->request.server, q, &blocked);
    if (blocked) etcp_router_conn_on_send_ready(blocked, &t->waiter, transfer_ready, t);
    return rc;
}

static void transfer_pump(void* arg) {
    struct mf_transfer* t = arg; t->pump = NULL;
    if (t->files->closing) return;
    if (!t->stopping && !transfer_allowed(t)) { transfer_stop(t, MF_ACCESS_DENIED); return; }
    if (t->starting) {
        t->starting = 0;
        if (t->sending || t->server) transfer_submit(t, TRANSFER_PREPARE);
        else transfer_send_open(t, MF_STAT);
        return;
    }
    if (t->reply_ready) {
        if (!t->status && !transfer_allowed(t)) {
            if (!t->finished_io) { transfer_stop(t, MF_ACCESS_DENIED); return; }
            t->status = t->outgoing.status = MF_ACCESS_DENIED;
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: final stream reply denied id=%llu", (unsigned long long)t->request.id);
        }
        int rc = transfer_enqueue(t, &t->outgoing);
        if (rc < 0) { transfer_stop(t, MF_IO_ERROR); return; }
        if (rc > 0) return;
        t->reply_ready = 0;
        if (t->reply_final) { transfer_release(t); return; }
        t->opened = t->active = 1;
    }
    if (!t->active || t->stopping) return;
    if (!transfer_allowed(t)) { transfer_stop(t, MF_ACCESS_DENIED); return; }
    if (t->sending) {
        // Ограничиваем работу callback, сохраняя fair scheduling нескольких файлов.
        unsigned sent = 0;
        while (t->count && sent++ < 8) {
            struct mf_chunk* chunk = &t->chunks[t->head];
            struct mf_message q = transfer_packet(t, MF_STREAM_DATA);
            q.size = q.data_len = chunk->bytes; memcpy(q.data, chunk->data, chunk->bytes);
            int rc = transfer_enqueue(t, &q);
            if (rc < 0) { transfer_stop(t, MF_IO_ERROR); return; }
            if (rc > 0) break;
            t->offset += chunk->bytes; t->head = (t->head + 1) % TRANSFER_SLOTS; t->count--;
            t->progress_tb = get_time_tb();
        }
        if (!t->busy && t->read_offset < t->size && t->count <= TRANSFER_SLOTS - TRANSFER_BATCH) {
            uint64_t blocks = (t->size - t->read_offset + MF_BLOCK_SIZE - 1) / MF_BLOCK_SIZE;
            t->io_count = blocks < TRANSFER_BATCH ? (unsigned)blocks : TRANSFER_BATCH;
            t->io_head = (t->head + t->count) % TRANSFER_SLOTS; t->io_offset = t->read_offset;
            transfer_submit(t, TRANSFER_READ);
        } else if (!t->busy && !t->count && t->offset == t->size && !t->verified) transfer_submit(t, TRANSFER_VERIFY);
        if (t->verified && !t->end_sent) {
            struct mf_message q = transfer_packet(t, MF_STREAM_END);
            int rc = transfer_enqueue(t, &q);
            if (rc < 0) { transfer_stop(t, MF_IO_ERROR); return; }
            if (!rc) { t->end_sent = 1; t->progress_tb = get_time_tb(); }
        }
        if (t->count) transfer_wake(t);
    } else if (!t->busy) {
        if (t->count) {
            t->io_head = t->head; t->io_offset = t->offset;
            t->io_count = t->count < TRANSFER_BATCH ? t->count : TRANSFER_BATCH;
            transfer_submit(t, TRANSFER_WRITE);
        } else if (t->end_received) transfer_submit(t, TRANSFER_VERIFY);
    }
    transfer_event(t, 0);
}

static void transfer_response(void* arg, const struct mf_message* r) {
    struct mf_transfer* t = arg; t->pending = NULL;
    if (t->stopping) return;
    if (r->status) { transfer_stop(t, r->status); return; }
    if (r->command == MF_STAT) {
        if (r->type != MF_FILE || r->size > INT64_MAX || mf_zero(r->version, 16)) { transfer_stop(t, MF_INVALID_RANGE); return; }
        if (t->request.max_bytes && r->size > t->request.max_bytes) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: download exceeds limit id=%llu bytes=%llu limit=%llu",
                (unsigned long long)t->request.id, (unsigned long long)r->size, (unsigned long long)t->request.max_bytes);
            transfer_stop(t, MF_INVALID_RANGE); return;
        }
        t->size = r->size; memcpy(t->version, r->version, 16); transfer_submit(t, TRANSFER_PREPARE);
    } else if (r->size != t->size || r->cursor != t->request.id || memcmp(r->upload, t->upload, 16)) {
        transfer_stop(t, MF_BAD_PACKET);
    } else { t->opened = t->active = 1; t->progress_tb = get_time_tb(); transfer_wake(t); }
}

static void transfer_send_open(struct mf_transfer* t, uint8_t command) {
    struct mf_message q = transfer_packet(t, command);
    q.request_id = 0; q.cursor = command == MF_STAT ? 0 : t->request.id;
    if (command == MF_STREAM_WRITE || command == MF_STAT) memcpy(q.version, t->request.version, 16);
    t->pending = member_files_request(t->files->inst, t->request.group, t->request.server, &q, transfer_response, t);
    if (!t->pending) transfer_stop(t, MF_BUSY);
}

static void transfer_connected(struct CONN_MGR_HANDLE* h, const uint8_t peer[32], uint64_t group, enum conn_mgr_event event, void* arg) {
    (void)h; (void)peer; (void)group;
    struct mf_transfer* t = arg;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: stream CM id=%llu event=%d stopping=%d",
                (unsigned long long)t->request.id, event, t->stopping);
    if (event == CONN_EVENT_UP) transfer_wake(t);
}

static struct mf_transfer* transfer_create(struct member_files* f, const struct mf_transfer_request* q, int server) {
    struct mf_transfer* t = u_calloc(1, sizeof(*t));
    if (!t) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: stream allocation failed"); return NULL; }
    t->cleanup = mf_worker_reserve();
    if (!t->cleanup) { u_free(t); return NULL; }
    strcpy(t->group_key, topo_groups_channel(f->inst->topo_groups, q->group));
    t->files = f; t->request = *q; t->fd = -1; t->server = server; t->sending = server ? !q->upload : q->upload;
    t->progress_tb = t->last_log = get_time_tb();
    t->next = f->transfers; f->transfers = t;
    if (server) f->server_transfer_count++; else f->transfer_count++;
    return t;
}

struct mf_transfer* member_files_transfer_start(const struct mf_transfer_request* q, mf_transfer_fn callback, void* arg) {
    struct member_files* f = q && q->inst ? q->inst->member_files : NULL;
    struct mf_message access = {0};
    if (q) { memcpy(access.owner, q->owner, MF_ADDRESS_SIZE); access.command = q->upload ? MF_STREAM_WRITE : MF_STREAM_READ;
        memcpy(access.path, q->remote, sizeof(access.path)); access.path[MF_PATH_MAX] = 0; }
    if (!f || f->closing || !callback || f->transfer_count >= TRANSFER_MAX || !q->group || mf_zero(q->server, MF_ADDRESS_SIZE) || mf_zero(q->owner, MF_ADDRESS_SIZE) ||
        !mf_path_valid(q->remote, 0) || !*q->local || strnlen(q->local, sizeof(q->local)) > MF_PATH_MAX ||
        (q->upload && memcmp(q->owner, q->inst->my_ed25519_pubkey, MF_ADDRESS_SIZE)) || !member_files_member(q->inst, q->group, q->inst->my_ed25519_pubkey, 0) ||
        !mf_storage_allowed(q->inst, q->group, q->server, &access) || !member_files_member(q->inst, q->group, q->owner, 0)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: stream start denied/limit reached"); return NULL;
    }
    struct mf_transfer_request request = *q;
    if (!request.id && (RAND_bytes((uint8_t*)&request.id, 8) != 1 || !request.id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: stream ID random failed"); return NULL;
    }
    for (struct mf_transfer* other = f->transfers; other; other = other->next) if (!other->server &&
        (other->request.id == request.id || (!q->upload && !other->request.upload && !strcmp(other->request.local, q->local)))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: duplicate stream ID/download destination"); return NULL;
    }
    struct mf_transfer* t = transfer_create(f, &request, 0);
    if (!t) return NULL;
    if (RAND_bytes(t->upload, 16) != 1) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: stream random failed"); goto failed; }
    snprintf(t->temp, sizeof(t->temp), "%s.utun-%016llx.part", q->local, (unsigned long long)request.id);
    t->starting = 1;
    if (mf_open_cm(q->inst, q->group, q->server, transfer_connected, t, &t->cm)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "member_files: stream CM launch failed"); goto failed;
    }
    transfer_wake(t);
    if (!t->pump) goto failed;
    t->callback = callback; t->arg = arg;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: stream started id=%llu upload=%d remote=%s local=%s active=%u",
        (unsigned long long)request.id, q->upload, q->remote, q->local, f->transfer_count);
    return t;
failed:
    t->status = MF_IO_ERROR; transfer_release(t); return NULL;
}

static struct mf_transfer* transfer_find(struct member_files* f, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], uint64_t id, int server) {
    for (struct mf_transfer* t = f->transfers; t; t = t->next)
        if (t->server == server && t->request.group == group && !memcmp(t->request.server, peer, MF_ADDRESS_SIZE) && t->request.id == id) return t;
    return NULL;
}

void mf_transfer_dispatch(struct member_files* f, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], const struct mf_message* q) {
    int server = q->kind == MF_REQUEST;
    uint64_t id = q->command <= MF_STREAM_READ ? q->cursor : q->request_id;
    struct mf_transfer* t = transfer_find(f, group, peer, id, server);
    if (q->command == MF_STREAM_WRITE || q->command == MF_STREAM_READ) {
        if (t) {
            if (memcmp(&t->open, q, sizeof(*q))) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: conflicting stream open"); return; }
            if (t->active && !t->stopping) { t->outgoing = t->open; t->outgoing.kind = MF_RESPONSE; t->reply_ready = 1; transfer_wake(t); }
            return;
        }
        int status = !id || mf_zero(q->owner, MF_ADDRESS_SIZE) || q->size > INT64_MAX || q->data_len || !mf_path_valid(q->path, 0) ? MF_BAD_PACKET : MF_OK;
        if (!status && !mf_server_allowed(f, group, peer, q)) status = MF_ACCESS_DENIED;
        if (!status && f->server_transfer_count >= TRANSFER_MAX) status = MF_BUSY;
        if (status) { mf_reply(f, group, peer, q, status); return; }
        struct mf_transfer_request request = { .inst = f->inst, .id = id, .group = group, .upload = q->command == MF_STREAM_WRITE };
        memcpy(request.server, peer, MF_ADDRESS_SIZE);
        memcpy(request.owner, q->owner, MF_ADDRESS_SIZE);
        snprintf(request.remote, sizeof(request.remote), "%s", q->path);
        t = transfer_create(f, &request, 1);
        if (!t) { mf_reply(f, group, peer, q, MF_BUSY); return; }
        t->open = *q; t->size = q->size; memcpy(t->upload, q->upload, 16); memcpy(t->version, q->version, 16); memcpy(t->hash, q->hash, 32);
        f->used_storage = 1;
        t->starting = 1; transfer_wake(t);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: server stream opened id=%llu peer=%s sending=%d bytes=%llu active=%u",
            (unsigned long long)id, sc_public_key_hex(peer, (char[65]){0}), t->sending, (unsigned long long)t->size, f->server_transfer_count);
        return;
    }
    if (!t || t->stopping) {
        DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "member_files: late stream packet id=%llu command=%s",
            (unsigned long long)id, mf_command_text(q->command));
        return;
    }
    if (memcmp(q->owner, t->request.owner, MF_ADDRESS_SIZE) || strcmp(q->path, t->request.remote) || memcmp(q->upload, t->upload, 16)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: stream packet identity mismatch id=%llu", (unsigned long long)id);
        transfer_stop(t, MF_BAD_PACKET); return;
    }
    if (q->status || q->command == MF_STREAM_CANCEL) { transfer_stop(t, q->status ? q->status : MF_CANCELLED); return; }
    if (!transfer_allowed(t)) { transfer_stop(t, MF_ACCESS_DENIED); return; }
    if (q->command == MF_STREAM_DATA) {
        if (t->sending || !t->active || t->end_received || !q->data_len || q->size != q->data_len ||
            q->offset != t->received || q->data_len > t->size - t->received || t->count == TRANSFER_SLOTS) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: stream DATA invalid id=%llu offset=%llu expected=%llu bytes=%u buffered=%u",
                (unsigned long long)id, (unsigned long long)q->offset, (unsigned long long)t->received, q->data_len, t->count);
            transfer_stop(t, MF_BAD_PACKET); return;
        }
        struct mf_chunk* chunk = &t->chunks[(t->head + t->count) % TRANSFER_SLOTS];
        chunk->bytes = q->data_len; memcpy(chunk->data, q->data, q->data_len); t->count++; t->received += q->data_len;
    } else if (q->command == MF_STREAM_END) {
        if (q->data_len || q->size != t->size || q->offset != t->size ||
            (t->sending ? !t->end_sent : t->received != t->size)) { transfer_stop(t, MF_BAD_PACKET); return; }
        if (t->request.upload && memcmp(t->hash, q->hash, 32)) { transfer_stop(t, MF_HASH_MISMATCH); return; }
        memcpy(t->hash, q->hash, 32); t->end_received = 1;
        if (t->sending) { transfer_stop(t, MF_OK); return; }
    }
    t->progress_tb = get_time_tb(); transfer_wake(t);
}

int mf_transfer_receive_ready(struct member_files* f, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE], int kind, uint64_t id) {
    struct mf_transfer* t = transfer_find(f, group, peer, id, kind == MF_REQUEST);
    return !t || t->stopping || t->sending || t->count < TRANSFER_SLOTS;
}

void mf_transfers_wake(struct member_files* f) {
    if (!f->closing) for (struct mf_transfer* t = f->transfers; t; t = t->next) if (!t->stopping || t->reply_ready) transfer_wake(t);
}

void mf_transfers_tick(struct member_files* f, uint64_t now) {
    struct mf_transfer* t = f->transfers;
    while (t) {
        struct mf_transfer* next = t->next;
        if (t->stopping && t->finished_io && now - t->progress_tb >= TRANSFER_TIMEOUT_TB) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: final/cancel delivery timeout id=%llu", (unsigned long long)t->request.id);
            transfer_release(t);
        } else if (!t->stopping && now - t->progress_tb >= TRANSFER_TIMEOUT_TB) transfer_stop(t, MF_TIMEOUT);
        else if (!t->stopping && now - t->last_log >= 10000) {
            DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA,
                "member_files: stream progress id=%llu server=%d sending=%d done=%llu/%llu buffered=%u io=%d wait=%s rate=%lluB/s",
                (unsigned long long)t->request.id, t->server, t->sending, (unsigned long long)t->offset,
                (unsigned long long)t->size, t->count, t->busy, t->waiter.internal ? "router" : t->busy ? "disk" : "peer",
                (unsigned long long)((t->offset - t->logged_offset) * 10000 / (now - t->last_log)));
            t->last_log = now; t->logged_offset = t->offset;
        }
        t = next;
    }
    mf_transfers_wake(f);
}

void member_files_transfer_cancel(struct mf_transfer* t) { if (t && !t->stopping) transfer_stop(t, MF_CANCELLED); }

void mf_transfers_quiesce(struct member_files* f, uint64_t group) {
    for (;;) {
        struct mf_transfer* found = NULL;
        for (struct mf_transfer* t = f->transfers; t; t = t->next)
            if (!t->peer_down && (!group || t->request.group == group)) { found = t; break; }
        if (!found) break;
        found->peer_down = 1; transfer_stop(found, MF_CANCELLED);
    }
}

void mf_transfers_peer_down(struct member_files* f, uint64_t group, const uint8_t peer[MF_ADDRESS_SIZE]) {
    for (;;) {
        struct mf_transfer* found = NULL;
        for (struct mf_transfer* t = f->transfers; t; t = t->next)
            if (!t->peer_down && t->request.group == group && !memcmp(t->request.server, peer, MF_ADDRESS_SIZE)) { found = t; break; }
        if (!found) break;
        found->peer_down = 1; transfer_stop(found, MF_IO_ERROR);
    }
}

void mf_transfers_revalidate(struct member_files* f, uint64_t group) {
    for (;;) {
        struct mf_transfer* denied = NULL;
        for (struct mf_transfer* t = f->transfers; t; t = t->next)
            if (!t->peer_down && t->request.group == group && !transfer_allowed(t)) { denied = t; break; }
        if (!denied) break;
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "member_files: revoke stream group=%llx peer=%s id=%llu io=%d",
            (unsigned long long)group, (unsigned long long)denied->request.server,
            (unsigned long long)denied->request.id, denied->busy);
        denied->peer_down = 1; transfer_stop(denied, MF_ACCESS_DENIED);
    }
}

static void transfer_gui_event(void* arg, const struct mf_transfer_progress* p) {
    struct mf_transfer_request* q = arg;
    uint8_t data[MF_EVENT_TRANSFER_SIZE];
    mf_put64(data, p->id); memcpy(data + MF_EVENT_TRANSFER_GROUP, q->group_key, MF_ADDRESS_SIZE);
    memcpy(data + MF_EVENT_TRANSFER_SERVER, q->server, MF_ADDRESS_SIZE); memcpy(data + MF_EVENT_TRANSFER_OWNER, q->owner, MF_ADDRESS_SIZE);
    mf_put64(data + MF_EVENT_TRANSFER_DONE, p->done); mf_put64(data + MF_EVENT_TRANSFER_TOTAL, p->total);
    data[MF_EVENT_TRANSFER_STATUS] = (uint8_t)p->status; data[MF_EVENT_TRANSFER_STATUS + 1] = (uint8_t)(p->status >> 8);
    data[MF_EVENT_TRANSFER_STATE] = p->finished ? 2 : 1;
    chat_event_post(q->inst, CHAT_EVT_MEMBER_FILES_TRANSFER, data, sizeof(data));
    if (p->finished) u_free(q);
}

void member_files_gui_transfer(void* arg) {
    struct mf_transfer_request* q = arg;
    struct TOPO_GROUP* group = q->inst ? topo_groups_find_wire(q->inst->topo_groups, q->group_key) : NULL;
    q->group = group && group->group_type == TOPO_GROUP_TYPE_CHAT ? group->group_id : 0;
    if (!q->group || !member_files_transfer_start(q, transfer_gui_event, q)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: GUI transfer denied group=%s id=%llu",
            sc_public_key_hex(q->group_key, (char[65]){0}), (unsigned long long)q->id);
        struct mf_transfer_progress p = { .id = q->id, .status = q->group ? MF_BUSY : MF_ACCESS_DENIED, .finished = 1 };
        transfer_gui_event(q, &p);
    }
}

void member_files_gui_cancel(void* arg) {
    struct mf_cancel_request* q = arg;
    struct member_files* f = q->inst ? q->inst->member_files : NULL;
    struct mf_transfer* t = f ? f->transfers : NULL;
    while (t && (t->server || t->request.id != q->id)) t = t->next;
    if (t) member_files_transfer_cancel(t);
    else member_files_cancel_request_id(q->inst, q->id);
    u_free(q);
}
