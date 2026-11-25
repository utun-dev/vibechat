/* file_transfer — signed router stream с ограниченной очередью и CM-владением. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <openssl/rand.h>

#include "../../lib/mem.h"
#include "../../lib/platform_compat.h"
#include "../../lib/debug_config.h"
#include "file_transfer.h"
#include "../utun_instance.h"
#include "../routing_layer/conn_mgr.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/topo_group.h"
#include "../chat/group_policy.h"
#include "../transport_layer/etcp.h"

#define FT_REQUEST 1 /* tid:16, object:16, size:8, offset:8 */
#define FT_DATA    2 /* tid:16, offset:8, bytes */
#define FT_DONE    3 /* tid:16 */
#define FT_REFUSE  4 /* tid:16 */
#define FT_CHUNK 8192u /* Вместе с подписью и полным путём помещается в кодограмму router (16 КиБ). */
#define FT_MAX_OPS 16

struct file_transfer {
    struct file_transfer* next;
    uint8_t service;
    int (*allowed)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32]);
    struct UTUN_INSTANCE* inst;
    file_transfer_lookup_fn lookup;
    file_transfer_lookup_async_fn lookup_async;
    void* arg;
    struct file_transfer_op* ops;
    struct ft_lookup* lookups;
    unsigned count, lookup_count, lookup_slots;
    int closing;
};
struct ft_lookup {
    struct ft_lookup* next;
    struct file_transfer* owner; /* Только uasync; timeout/cancel/destroy отсоединяет до позднего provider ready. */
    uint64_t group;
    uint8_t ed[32], request[48];
    void* timer;
    int new_slot; /* Повторная проверка существующей выдачи не резервирует второй transfer slot. */
};
static void ft_lookup_cancel_tid(struct file_transfer* ft, uint64_t group, const uint8_t ed[32], const uint8_t tid[16]);

struct file_transfer_op {
    struct file_transfer_op* next;
    struct file_transfer* ft;
    uint64_t group, size, offset, progress_tb, request_tb;
    uint8_t ed[32], tid[16], object[16];
    char path[1024]; /* Конкретная локальная копия, независимо от UUID содержимого. */
    int receiving, up;
    FILE* file;
    struct CONN_MGR_HANDLE* cm;
    struct queue_waiter_handle waiter;
    void* timer;
    void* pump;
    file_transfer_done_fn done;
    void* arg;
};

static int ft_allowed(struct file_transfer* ft, uint64_t group, const uint8_t ed[32]) {
    return ft->allowed(ft->inst, group, ed) == 1;
}

/* Очистить все локальные триггеры до callback владельца. */
static void ft_finish(struct file_transfer_op* op, int error) {
    struct file_transfer* ft = op->ft;
    if (!op->receiving) ft_lookup_cancel_tid(ft, op->group, op->ed, op->tid);
    if (op->timer) uasync_cancel_timeout(ft->inst->ua, op->timer);
    if (op->pump) uasync_call_soon_cancel(ft->inst->ua, op->pump);
    etcp_router_cancel_send_ready(&op->waiter);
    if (op->cm) conn_mgr_close(op->cm);
    if (op->file && fclose(op->file) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: close failed peer=%s: %s",
                    sc_public_key_hex(op->ed, (char[65]){0}), strerror(errno));
        error = -1;
    }
    struct file_transfer_op** p = &ft->ops;
    while (*p && *p != op) p = &(*p)->next;
    if (*p) *p = op->next;
    ft->count--;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file_transfer: finish svc=%u group=%llu peer=%s receive=%d bytes=%llu/%llu error=%d active=%u",
               ft->service, (unsigned long long)op->group, sc_public_key_hex(op->ed, (char[65]){0}), op->receiving, (unsigned long long)op->offset,
               (unsigned long long)op->size, error, ft->count);
    file_transfer_done_fn done = op->done;
    void* arg = op->arg;
    u_free(op);
    if (done) done(arg, error);
}

/* Отложенный callback не продолжает I/O после отзыва; до возврата 0 операция уже освобождена. */
static int ft_active(struct file_transfer_op* op) {
    if (ft_allowed(op->ft, op->group, op->ed)) return 1;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file_transfer: revoke group=%llu peer=%s receive=%d",
        (unsigned long long)op->group, sc_public_key_hex(op->ed, (char[65]){0}), op->receiving);
    ft_finish(op, -2); return 0;
}

/* Отправить подписанную кодограмму, не превышая порог очереди. */
static int ft_send(struct file_transfer* ft, uint64_t group, const uint8_t ed[32],
                    uint8_t cmd, const uint8_t* data, size_t size) {
    if (!ft_allowed(ft, group, ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: send denied group=%llu peer=%s cmd=%u",
            (unsigned long long)group, sc_public_key_hex(ed, (char[65]){0}), cmd); return -1;
    }
    struct ETCP_ROUTER_CONN* rc = etcp_router_conn_get_key(ft->inst, group, ed, ft->service);
    if (!rc) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: router allocation failed"); return -1; }
    queue_set_threshold(rc->send_q, 0, 0);
    etcp_router_set_max_inflight(rc, 16);
    if (!etcp_router_conn_send_q_has_room(rc)) return 1;
    uint8_t packet[1 + 24 + FT_CHUNK];
    packet[0] = cmd;
    memcpy(packet + 1, data, size);
    int error = etcp_router_conn_send(rc, packet, size + 1, ROUTER_FLAG_SIGNED);
    if (error) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: send failed peer=%s cmd=%u rc=%d",
                          sc_public_key_hex(ed, (char[65]){0}), cmd, error);
    return error ? -1 : 0;
}

/* Повторный REQUEST возобновляет приём со следующего неполученного байта. */
static void ft_request(struct file_transfer_op* op) {
    uint8_t body[48];
    memcpy(body, op->tid, 16);
    memcpy(body + 16, op->object, 16);
    memcpy(body + 32, &op->size, 8);
    memcpy(body + 40, &op->offset, 8);
    if (ft_send(op->ft, op->group, op->ed, FT_REQUEST, body, sizeof(body)) == 0)
        op->request_tb = get_time_tb();
}

static void ft_pump(void* arg);

/* Waiter может сработать синхронно; продолжение всегда через call_soon. */
static void ft_ready(struct ll_queue* q, void* arg) {
    (void)q;
    struct file_transfer_op* op = arg;
    if (op->pump) return;
    op->pump = uasync_call_soon(op->ft->inst->ua, op, ft_pump);
    if (!op->pump) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: pump timer allocation failed");
}

/* Ограничить работу одного callback и количество байтов в router. */
static void ft_pump(void* arg) {
    struct file_transfer_op* op = arg;
    op->pump = NULL;
    if (!ft_active(op)) return;
    if (!op->up || op->receiving) return;
    struct file_transfer* ft = op->ft;
    if (op->offset < op->size) {
        struct ETCP_ROUTER_CONN* route = etcp_router_conn_lookup(ft->inst, op->group, op->ed, ft->service);
        if (!etcp_router_conn_send_q_has_room(route)) {
            etcp_router_conn_on_send_ready(route, &op->waiter, ft_ready, op);
            return;
        }
        uint8_t body[24 + FT_CHUNK];
        size_t n = op->size - op->offset < FT_CHUNK ? (size_t)(op->size - op->offset) : FT_CHUNK;
        memcpy(body, op->tid, 16);
        memcpy(body + 16, &op->offset, 8);
        if (fseeko(op->file, (off_t)op->offset, SEEK_SET) != 0 || fread(body + 24, 1, n, op->file) != n) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: read failed peer=%s offset=%llu: %s",
                        sc_public_key_hex(op->ed, (char[65]){0}), (unsigned long long)op->offset, strerror(errno));
            ft_finish(op, -1);
            return;
        }
        int error = ft_send(ft, op->group, op->ed, FT_DATA, body, 24 + n);
        if (error < 0) { ft_finish(op, -1); return; }
        if (error > 0) return;
        op->offset += n;
        op->progress_tb = get_time_tb();
    }
    if (op->offset < op->size) ft_ready(NULL, op);
}

/* DOWN/TIMEOUT сохраняет CM handle; таймер проверяет позднее восстановление. */
static void ft_connected(struct CONN_MGR_HANDLE* h, const uint8_t peer[32], uint64_t group,
                          enum conn_mgr_event event, void* arg) {
    (void)h; (void)peer; (void)group;
    struct file_transfer_op* op = arg;
    if (!ft_active(op)) return;
    op->up = event == CONN_EVENT_UP;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "file_transfer: CM event=%d peer=%s receive=%d offset=%llu",
                event, sc_public_key_hex(op->ed, (char[65]){0}), op->receiving, (unsigned long long)op->offset);
    if (op->up) {
        if (op->receiving) ft_request(op);
        else ft_ready(NULL, op);
    }
}

/* Неактивная операция завершается; durable владелец решает, когда повторять. */
static void ft_tick(void* arg) {
    struct file_transfer_op* op = arg;
    op->timer = NULL;
    if (!ft_active(op)) return;
    uint64_t now = get_time_tb();
    if (now - op->progress_tb >= 300000) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: inactivity timeout peer=%s offset=%llu/%llu",
                   sc_public_key_hex(op->ed, (char[65]){0}), (unsigned long long)op->offset, (unsigned long long)op->size);
        ft_finish(op, -1);
        return;
    }
    if (!op->cm) op->up = etcp_router_peer_reachable(op->ft->inst, op->group, op->ed, op->ft->service);
    if (op->up) {
        if (op->receiving && now - op->request_tb >= 50000 && now - op->progress_tb >= 50000) ft_request(op);
        else if (!op->receiving && !op->pump && !op->waiter.internal) ft_ready(NULL, op);
    }
    op->timer = uasync_set_timeout(op->ft->inst->ua, 10000, op, ft_tick, "file_transfer");
    if (!op->timer) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: watchdog allocation failed"); ft_finish(op, -1); }
}

/* Добавить операцию до conn_mgr_open; callbacks CM используют уже живой owner. */
static struct file_transfer_op* ft_open(struct file_transfer* ft, uint64_t group, const uint8_t ed[32],
                                        const uint8_t object[16], uint64_t size, const char* path, int receiving) {
    if (!ft || ft->closing || !group || !ed || !object || !path || strlen(path) >= 1024 ||
        !size || size > INT64_MAX || ft->count + ft->lookup_slots >= FT_MAX_OPS) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: invalid request or operation limit size=%llu",
                   (unsigned long long)size);
        return NULL;
    }
    uint8_t x[32];
    if (sc_identity_public_x("file transfer peer", ed, x) != SC_OK) return NULL;
    if (!ft_allowed(ft, group, ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: open denied group=%llu peer=%s receive=%d",
            (unsigned long long)group, sc_public_key_hex(ed, (char[65]){0}), receiving); return NULL;
    }
    struct file_transfer_op* op = u_calloc(1, sizeof(*op));
    if (!op) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: operation allocation failed"); return NULL; }
    op->ft = ft; op->group = group; memcpy(op->ed, ed, 32); op->size = size; op->receiving = receiving;
    memcpy(op->object, object, 16);
    snprintf(op->path, sizeof(op->path), "%s", path);
    op->file = fopen(path, receiving ? "wb" : "rb");
    if (!op->file) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: open failed path=%s: %s", path, strerror(errno));
        u_free(op);
        return NULL;
    }
    op->next = ft->ops; ft->ops = op; ft->count++;
    op->progress_tb = get_time_tb();
    op->timer = uasync_set_timeout(ft->inst->ua, 10000, op, ft_tick, "file_transfer");
    if (!op->timer || !etcp_router_conn_get_key(ft->inst, group, ed, ft->service)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: router/timer launch failed peer=%s", sc_public_key_hex(ed, (char[65]){0}));
        ft_finish(op, -1);
        return NULL;
    }
    if (topo_group_chat_key_allowed(ft->inst, group, ed))
        conn_mgr_open(ft->inst, group, ed, ft_connected, op, &op->cm);
    if (!op->cm) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "file_transfer: tracking exact router path peer=%s", sc_public_key_hex(ed, (char[65]){0}));
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file_transfer: start svc=%u group=%llu peer=%s receive=%d size=%llu active=%u",
               ft->service, (unsigned long long)group, sc_public_key_hex(ed, (char[65]){0}), receiving, (unsigned long long)size, ft->count);
    return op;
}

static struct file_transfer_op* ft_find(struct file_transfer* ft, uint64_t group, const uint8_t ed[32],
                                       const uint8_t tid[16], int receiving) {
    for (struct file_transfer_op* op = ft->ops; op; op = op->next)
        if (op->group == group && !memcmp(op->ed, ed, 32) && op->receiving == receiving && !memcmp(op->tid, tid, 16)) return op;
    return NULL;
}

static void ft_serve_path(struct file_transfer* ft, uint64_t group, const uint8_t ed[32], const uint8_t* p, const char* path) {
    uint64_t size, offset;
    memcpy(&size, p + 32, 8); memcpy(&offset, p + 40, 8);
    struct file_transfer_op* op = ft_find(ft, group, ed, p, 0);
    if (op && (op->size != size || memcmp(op->object, p + 16, 16))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: conflicting transfer ID peer=%s", sc_public_key_hex(ed, (char[65]){0}));
        return;
    }
    if (!op) {
        op = ft_open(ft, group, ed, p + 16, size, path, 0);
        if (!op) { ft_send(ft, group, ed, FT_REFUSE, p, 16); return; }
        memcpy(op->tid, p, 16);
    }
    op->offset = offset;
    op->progress_tb = get_time_tb();
    if (op->up) ft_ready(NULL, op);
}

static void ft_lookup_unlink(struct ft_lookup* lookup) {
    struct file_transfer* ft = lookup->owner; if (!ft) return;
    if (lookup->timer) uasync_cancel_timeout(ft->inst->ua, lookup->timer);
    lookup->timer = NULL;
    struct ft_lookup** slot = &ft->lookups;
    while (*slot && *slot != lookup) slot = &(*slot)->next;
    if (*slot) { *slot = lookup->next; ft->lookup_count--; if (lookup->new_slot) ft->lookup_slots--; }
    else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: lookup lost from owner group=%llx peer=%s",
        (unsigned long long)lookup->group, sc_public_key_hex(lookup->ed, (char[65]){0}));
    lookup->owner = NULL; lookup->next = NULL;
}
static void ft_lookup_cancel_tid(struct file_transfer* ft, uint64_t group, const uint8_t ed[32], const uint8_t tid[16]) {
    for (struct ft_lookup* lookup = ft->lookups, *next; lookup; lookup = next) {
        next = lookup->next;
        if (lookup->group == group && !memcmp(lookup->ed, ed, 32) && !memcmp(lookup->request, tid, 16)) ft_lookup_unlink(lookup);
    }
}
static void ft_lookup_timeout(void* arg) {
    struct ft_lookup* lookup = arg; lookup->timer = NULL; struct file_transfer* ft = lookup->owner;
    if (!ft) return;
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: lookup timeout group=%llx peer=%s", (unsigned long long)lookup->group,
        sc_public_key_hex(lookup->ed, (char[65]){0}));
    ft_lookup_unlink(lookup);
    struct file_transfer_op* op = ft_find(ft, lookup->group, lookup->ed, lookup->request, 0);
    if (op) ft_finish(op, -2);
    if (!ft->closing) ft_send(ft, lookup->group, lookup->ed, FT_REFUSE, lookup->request, 16);
}
static void ft_lookup_ready(void* arg, int error, const char* path) {
    struct ft_lookup* lookup = arg; struct file_transfer* ft = lookup->owner;
    if (!ft) { u_free(lookup); return; }
    ft_lookup_unlink(lookup);
    if (error || !path || !*path || strlen(path) >= 1024 || ft->closing || !ft_allowed(ft, lookup->group, lookup->ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: async lookup refused group=%llx peer=%s error=%d", (unsigned long long)lookup->group,
            sc_public_key_hex(lookup->ed, (char[65]){0}), error);
        struct file_transfer_op* op = ft_find(ft, lookup->group, lookup->ed, lookup->request, 0);
        if (op) ft_finish(op, -2);
        if (!ft->closing) ft_send(ft, lookup->group, lookup->ed, FT_REFUSE, lookup->request, 16);
    } else ft_serve_path(ft, lookup->group, lookup->ed, lookup->request, path);
    u_free(lookup);
}
static void ft_lookup_start(struct file_transfer* ft, uint64_t group, const uint8_t ed[32], const uint8_t* request, uint64_t size) {
    for (struct ft_lookup* lookup = ft->lookups; lookup; lookup = lookup->next) {
        if (lookup->group != group || memcmp(lookup->ed, ed, 32) || memcmp(lookup->request, request, 16)) continue;
        if (memcmp(lookup->request + 16, request + 16, 24)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: conflicting pending lookup peer=%s", sc_public_key_hex(ed, (char[65]){0})); return;
        }
        memcpy(lookup->request + 40, request + 40, 8); return;
    }
    struct file_transfer_op* existing = ft_find(ft, group, ed, request, 0);
    if (existing && (existing->size != size || memcmp(existing->object, request + 16, 16))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: conflicting transfer during lookup peer=%s", sc_public_key_hex(ed, (char[65]){0})); return;
    }
    if (ft->lookup_count >= FT_MAX_OPS || (!existing && ft->count + ft->lookup_slots >= FT_MAX_OPS)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: lookup limit active=%u pending=%u", ft->count, ft->lookup_count);
        ft_send(ft, group, ed, FT_REFUSE, request, 16); return;
    }
    struct ft_lookup* lookup = u_calloc(1, sizeof(*lookup));
    if (!lookup) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: lookup allocation failed"); ft_send(ft, group, ed, FT_REFUSE, request, 16); return;
    }
    lookup->owner = ft; lookup->group = group; memcpy(lookup->ed, ed, 32); memcpy(lookup->request, request, sizeof(lookup->request));
    lookup->new_slot = existing == NULL;
    lookup->timer = uasync_set_timeout(ft->inst->ua, 300000, lookup, ft_lookup_timeout, "file lookup");
    if (!lookup->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: lookup timeout allocation failed"); u_free(lookup);
        ft_send(ft, group, ed, FT_REFUSE, request, 16); return;
    }
    lookup->next = ft->lookups; ft->lookups = lookup; ft->lookup_count++; if (lookup->new_slot) ft->lookup_slots++;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "file_transfer: async lookup started group=%llx peer=%s pending=%u", (unsigned long long)group,
        sc_public_key_hex(ed, (char[65]){0}), ft->lookup_count);
    /* Входной object заимствован до возврата provider, даже если ready синхронно освобождает lookup. */
    ft->lookup_async(ft->arg, group, ed, request + 16, size, ft_lookup_ready, lookup);
}

/* REQUEST проверяется владельцем файла при каждом повторе, включая TTL/права. */
static void ft_serve(struct file_transfer* ft, uint64_t group, const uint8_t ed[32], const uint8_t* p) {
    uint64_t size, offset; memcpy(&size, p + 32, 8); memcpy(&offset, p + 40, 8);
    if (!size || size > INT64_MAX || offset >= size) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: invalid size/offset peer=%s", sc_public_key_hex(ed, (char[65]){0}));
        ft_send(ft, group, ed, FT_REFUSE, p, 16); return;
    }
    if (ft->lookup_async) { ft_lookup_start(ft, group, ed, p, size); return; }
    char path[1024] = {0};
    if (ft->lookup(ft->arg, group, ed, p + 16, size, path, sizeof(path)) || !path[0]) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: lookup refused peer=%s size=%llu", sc_public_key_hex(ed, (char[65]){0}), (unsigned long long)size);
        ft_send(ft, group, ed, FT_REFUSE, p, 16); return;
    }
    ft_serve_path(ft, group, ed, p, path);
}

/* Signed source, transfer ID и точные границы отделяют операции и повторы. */
static void ft_recv(struct ETCP_CONN* conn, struct ll_entry* e) {
    if (!e || !conn || !e->dgram || e->len < ROUTER_SVC_PAYLOAD_OFF) goto done;
    struct file_transfer* ft = conn->instance->file_transfer_services;
    while (ft && ft->service != e->dgram[0]) ft = ft->next;
    if (!ft || ft->closing) goto done;
    uint64_t group;
    memcpy(&group, e->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* ed = e->dgram + ROUTER_SVC_SRC_ED_OFF;
    if (e->len == ROUTER_SVC_PAYLOAD_OFF) { file_transfer_revalidate_members(ft, group); goto done; }
    if (!ft_allowed(ft, group, ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: receive denied group=%llu peer=%s",
            (unsigned long long)group, sc_public_key_hex(ed, (char[65]){0})); goto done;
    }
    if (!(e->dgram[ROUTER_SVC_FLAGS_OFF] & ROUTER_FLAG_SIGNED)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: unsigned packet peer=%s", sc_public_key_hex(ed, (char[65]){0}));
        goto done;
    }
    const uint8_t* p = e->dgram + ROUTER_SVC_PAYLOAD_OFF;
    uint8_t cmd = *p++;
    size_t len = e->len - ROUTER_SVC_PAYLOAD_OFF - 1;
    if (cmd == FT_REQUEST && len == 48) ft_serve(ft, group, ed, p);
    else if (cmd == FT_DONE && len == 16) {
        ft_lookup_cancel_tid(ft, group, ed, p);
        struct file_transfer_op* op = ft_find(ft, group, ed, p, 0);
        if (op && op->offset == op->size) ft_finish(op, 0);
    } else if (cmd == FT_REFUSE && len == 16) {
        struct file_transfer_op* op = ft_find(ft, group, ed, p, 1);
        if (op) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: source refused peer=%s", sc_public_key_hex(ed, (char[65]){0})); ft_finish(op, -1); }
    } else if (cmd == FT_DATA && len > 24 && len <= 24 + FT_CHUNK) {
        struct file_transfer_op* op = ft_find(ft, group, ed, p, 1);
        if (!op) goto done; /* Поздний пакет завершённой/отменённой операции. */
        uint64_t offset;
        memcpy(&offset, p + 16, 8);
        size_t n = len - 24;
        if (offset < op->offset && n <= op->offset - offset) goto done;
        if (offset != op->offset || n > op->size - op->offset) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: wrong range peer=%s offset=%llu expected=%llu bytes=%zu",
                       sc_public_key_hex(ed, (char[65]){0}), (unsigned long long)offset, (unsigned long long)op->offset, n);
            ft_request(op);
            goto done;
        }
        if (fwrite(p + 24, 1, n, op->file) != n) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: write failed peer=%s: %s", sc_public_key_hex(ed, (char[65]){0}), strerror(errno));
            ft_finish(op, -1);
            goto done;
        }
        op->offset += n;
        op->progress_tb = get_time_tb();
        if (op->offset == op->size) {
            ft_send(ft, group, ed, FT_DONE, op->tid, 16);
            ft_finish(op, 0);
        }
    } else DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file_transfer: malformed packet cmd=%u bytes=%zu", cmd, len);
done:
    if (e) { queue_dgram_free(e); queue_entry_free(e); }
}

static struct file_transfer* ft_create_service(struct UTUN_INSTANCE* inst, uint8_t service,
    file_transfer_lookup_fn lookup, file_transfer_lookup_async_fn lookup_async,
    int (*allowed)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32]), void* arg) {
    if (!inst || (!lookup == !lookup_async) || !allowed || inst->router_bindings.callbacks[service]) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: invalid initialization or occupied service=%u", service); return NULL;
    }
    for (struct file_transfer* p = inst->file_transfer_services; p; p = p->next) if (p->service == service) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: duplicate service=%u", service); return NULL;
    }
    struct file_transfer* ft = u_calloc(1, sizeof(*ft));
    if (!ft) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: owner allocation failed"); return NULL; }
    ft->inst = inst; ft->lookup = lookup; ft->lookup_async = lookup_async; ft->arg = arg; ft->service = service; ft->allowed = allowed;
    if (etcp_router_bind(inst, service, ft_recv) ||
        etcp_router_require_crypto(inst, service, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) ||
        etcp_router_set_authorize(inst, service, allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: router setup failed svc=%u", service);
        etcp_router_unbind(inst, service); u_free(ft); return NULL;
    }
    ft->next = inst->file_transfer_services; inst->file_transfer_services = ft;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file_transfer: initialized svc=%u max_operations=%u", service, FT_MAX_OPS);
    return ft;
}
struct file_transfer* file_transfer_create_service(struct UTUN_INSTANCE* inst, uint8_t service,
    file_transfer_lookup_fn lookup, int (*allowed)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32]), void* arg) {
    return ft_create_service(inst, service, lookup, NULL, allowed, arg);
}
struct file_transfer* file_transfer_create_service_async(struct UTUN_INSTANCE* inst, uint8_t service,
    file_transfer_lookup_async_fn lookup, int (*allowed)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32]), void* arg) {
    return ft_create_service(inst, service, NULL, lookup, allowed, arg);
}
struct file_transfer* file_transfer_create_async(struct UTUN_INSTANCE* inst, file_transfer_lookup_async_fn lookup, void* arg) {
    struct file_transfer* ft = file_transfer_create_service_async(inst, ETCP_RT_ID_FILE_TRANSFER, lookup, topo_group_chat_key_allowed, arg);
    if (ft) inst->file_transfer = ft;
    return ft;
}
struct file_transfer* file_transfer_create(struct UTUN_INSTANCE* inst, file_transfer_lookup_fn lookup, void* arg) {
    struct file_transfer* ft = file_transfer_create_service(inst, ETCP_RT_ID_FILE_TRANSFER, lookup, topo_group_chat_key_allowed, arg);
    if (ft) inst->file_transfer = ft;
    return ft;
}

void file_transfer_destroy(struct file_transfer* ft) {
    if (!ft) return;
    ft->closing = 1;
    while (ft->lookups) ft_lookup_unlink(ft->lookups);
    etcp_router_unbind(ft->inst, ft->service);
    while (ft->ops) ft_finish(ft->ops, -2);
    for (;;) {
        struct ETCP_ROUTER_CONN* found = NULL;
        for (struct ll_entry* e = ft->inst->router_conns->head; e; e = e->next) {
            struct ETCP_ROUTER_CONN* conn = (struct ETCP_ROUTER_CONN*)e;
            if (conn->svc_id == ft->service) { found = conn; break; }
        }
        if (!found) break;
        etcp_router_conn_close(found);
    }
    struct file_transfer** link = &ft->inst->file_transfer_services;
    while (*link && *link != ft) link = &(*link)->next;
    if (*link) *link = ft->next;
    if (ft->inst->file_transfer == ft) ft->inst->file_transfer = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file_transfer: destroyed");
    u_free(ft);
}

struct file_transfer_op* file_transfer_pull(struct file_transfer* ft, uint64_t group, const uint8_t ed[32],
                                           const uint8_t object[16], uint64_t size, const char* path,
                                           file_transfer_done_fn done, void* arg) {
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: pull without completion callback"); return NULL; }
    uint8_t tid[16];
    if (RAND_bytes(tid, sizeof(tid)) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file_transfer: transfer ID generation failed"); return NULL;
    }
    struct file_transfer_op* op = ft_open(ft, group, ed, object, size, path, 1);
    if (!op) return NULL;
    memcpy(op->tid, tid, sizeof(tid));
    op->done = done; op->arg = arg;
    return op;
}

void file_transfer_cancel(struct file_transfer_op* op) {
    if (op) ft_finish(op, -2);
}

void file_transfer_cancel_object(struct file_transfer* ft, const uint8_t object[16]) {
    if (!ft) return;
    for (struct ft_lookup* lookup = ft->lookups, *next; lookup; lookup = next) {
        next = lookup->next; if (!memcmp(lookup->request + 16, object, 16)) ft_lookup_unlink(lookup);
    }
    for (;;) {
        struct file_transfer_op* found = NULL;
        for (struct file_transfer_op* op = ft->ops; op; op = op->next)
            if (!memcmp(op->object, object, 16)) { found = op; break; }
        if (!found) break;
        ft_finish(found, -2);
    }
}

void file_transfer_cancel_path(struct file_transfer* ft, const char* path) {
    if (!ft || !path) return;
    for (;;) {
        struct file_transfer_op* found = NULL;
        for (struct file_transfer_op* op = ft->ops; op; op = op->next)
            if (!strcmp(op->path, path)) { found = op; break; }
        if (!found) break;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "file_transfer: revoke copy peer=%s path=%s", sc_public_key_hex(found->ed, (char[65]){0}), path);
        ft_finish(found, -2);
    }
}

void file_transfer_revalidate_members(struct file_transfer* ft, uint64_t group) {
    if (!ft || ft->closing) return;
    for (struct ft_lookup* lookup = ft->lookups, *next; lookup; lookup = next) {
        next = lookup->next;
        if (lookup->group == group && !ft_allowed(ft, group, lookup->ed)) ft_lookup_unlink(lookup);
    }
    /* done может отменить другие операции: следующий указатель через callback не сохраняем. */
    for (;;) {
        struct file_transfer_op* denied = NULL;
        for (struct file_transfer_op* op = ft->ops; op; op = op->next)
            if (op->group == group && !ft_allowed(ft, group, op->ed)) { denied = op; break; }
        if (!denied) return;
        ft_active(denied);
    }
}
