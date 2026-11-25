// media_download.c — скачивание блоков медиа
//
// Блочный конечный автомат с единым watchdog-таймером:
//   - таймаут простоя блока (нет трафика 2с) → фейловер на следующий узел-держатель;
//   - исчерпание попыток (3) → провал всей загрузки;
//   - перегрузка отправителя (OVERLOADED) → переход на другой узел или ожидание;
//   - RELAY_FULL → добавление новых держателей и повторный запрос.
#include "media_download.h"
#include "media_delivery.h"
#include "media_delivery_proto.h"
#include "../media_delivery/media_index.h"
#include "../utun_instance.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/conn_mgr.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../chat/group_policy.h"
#include "../transport_layer/etcp_api.h"
#include "../transport_layer/secure_channel.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "../lib/platform_compat.h"
#include "../../lib/db_async.h"
#include "../media_async/media_async.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define MDL_ID "media_download"

/* ── helpers ── */

static struct media_download* md_dl_find(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t* media_id) {
    struct ll_queue* queue = inst->md.downloads;
    if (!queue) return NULL;
    if (group) {
        uint8_t key[MEDIA_DOWNLOAD_KEY_SIZE];
        memcpy(key, &group, sizeof(group)); memcpy(key + sizeof(group), media_id, 16);
        struct ll_entry* e = queue_find_data_by_index(queue, key);
        return e ? (void*)e->data : NULL;
    }
    for (struct ll_entry* e = queue->head; e; e = e->next) {
        struct media_download* dl = (void*)e->data;
        if (!memcmp(dl->media_id, media_id, 16)) return dl;
    }
    return NULL;
}

static uint64_t md_dl_stall_tb(struct UTUN_INSTANCE* inst) {
    return inst->md.dl_stall_timeout_tb > 0 ? (uint64_t)inst->md.dl_stall_timeout_tb : MEDIA_DL_STALL_TIMEOUT_TB;
}

static int md_dl_max_attempts(struct UTUN_INSTANCE* inst) {
    return inst->md.dl_max_attempts > 0 ? inst->md.dl_max_attempts : MEDIA_DL_MAX_ATTEMPTS;
}

static int md_dl_block_index(struct media_download* dl, const uint8_t* block_id) {
    for (int i = 0; i < dl->num_blocks; i++)
        if (memcmp(dl->blocks[i].block_id, block_id, 16) == 0) return i;
    return -1;
}

static int md_dl_block_inflight(const struct media_download_block* block) {
    return block->state == MD_BLK_CONN || block->state == MD_BLK_REQ || block->state == MD_BLK_RECV;
}

static void md_dl_add_holder(struct media_download_block* b, const uint8_t ed[32]) {
    for (int n = 0; n < b->num_holders; n++) if (!memcmp(b->holders[n], ed, 32)) return;
    if (b->num_holders == MD_MAX_HOLDERS_PER_BLOCK) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: holder list full candidates=%d", MDL_ID, b->num_holders); return;
    }
    memcpy(b->holders[b->num_holders++], ed, 32);
}

/* Выбор по кругу сохраняет начальный разброс и обходит все ещё пригодные слоты. */
static int md_dl_select_holder(struct media_download_block* b, int start, uint64_t now) {
    b->retry_at_tb = 0;
    for (int n = 0; n < b->num_holders; n++) {
        int i = (start + n) % b->num_holders;
        if (b->failed_holders & (UINT64_C(1) << i)) continue;
        uint64_t retry = b->holder_retry[i];
        if (retry <= now) return i;
        if (!b->retry_at_tb || retry < b->retry_at_tb) b->retry_at_tb = retry;
    }
    return -1;
}

static void md_dl_backoff_holder(struct media_download* dl, struct media_download_block* b, uint64_t deadline) {
    int holder = b->holder_idx;
    int budget = md_dl_max_attempts(dl->inst);
    if (budget > UINT8_MAX) budget = UINT8_MAX;
    if (++b->holder_busy[holder] >= budget) {
        b->failed_holders |= UINT64_C(1) << holder;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: holder busy budget exhausted holder=%s retries=%u", MDL_ID,
            sc_public_key_hex(b->holders[holder], (char[65]){0}), b->holder_busy[holder]);
    } else b->holder_retry[holder] = deadline;
}

static int md_dl_reset_block_file(struct media_download* dl, int bi) {
    struct media_download_block* b = &dl->blocks[bi];
    if (b->source) { md_block_abort(b->source); md_block_release(b->source); b->source = NULL; }
    if (RAND_bytes(b->transfer_id, sizeof(b->transfer_id)) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: transfer UUID generation failed block=%d", MDL_ID, bi); return -1;
    }
    b->source = md_block_create(&dl->inst->md, dl->group_id, dl->media_id, b->block_id,
                                (uint32_t)bi, b->expected_size, dl->dest_path);
    return b->source ? 0 : -1;
}

static struct media_download_peer* md_dl_get_peer(struct media_download* dl, const uint8_t ed[32]) {
    for (int pi = 0; pi < dl->num_peers; pi++)
        if (!memcmp(dl->peers[pi].ed, ed, 32)) return &dl->peers[pi];
    struct media_download_peer* p = NULL;
    for (int pi = 0; pi < dl->num_peers; pi++) {
        struct media_download_peer* candidate = &dl->peers[pi];
        int used = candidate->pending_event != 0;
        for (int bi = 0; bi < dl->num_blocks && !used; bi++) {
            struct media_download_block* b = &dl->blocks[bi];
            used = md_dl_block_inflight(b) && b->holder_idx >= 0 && b->holder_idx < b->num_holders
                && !memcmp(b->holders[b->holder_idx], candidate->ed, 32);
        }
        if (!used) { p = candidate; break; }
    }
    if (!p && dl->num_peers < MD_MAX_PEERS) p = &dl->peers[dl->num_peers++];
    if (!p) return NULL; // Все слоты заняты: штатное ожидание без расходования попытки.
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: peer slot old=%s new=%s handle=%p",
                MDL_ID, sc_public_key_hex(p->ed, (char[65]){0}), sc_public_key_hex(ed, (char[65]){0}), (void*)p->cm_handle);
    if (p->cm_handle) conn_mgr_close(p->cm_handle);
    memset(p, 0, sizeof(*p));
    memcpy(p->ed, ed, sizeof(p->ed));
    return p;
}

static int md_mkdir_parent(const char* filepath) {
    char path[1024]; snprintf(path, sizeof(path), "%s", filepath);
    char* p = path;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
        p += 2;
    while (*p == '/' || *p == '\\') p++;
    while ((p = strpbrk(p, "/\\"))) {
        *p = '\0';
        if (utun_mkdir(path, 0755) != 0 && errno != EEXIST) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: mkdir(%s) failed: %s", MDL_ID, path, strerror(errno));
            *p = '/'; return -1;
        }
        *p = '/'; p++;
    }
    return 0;
}

/* ── send helpers ── */

static int md_dl_send_block_req(struct UTUN_INSTANCE* inst, const uint8_t dst[32], struct media_download* dl, int bi) {
    struct media_pkt_block_req req;
    memset(&req, 0, sizeof(req));
    req.subcmd = MEDIA_SUBCMD_BLOCK_REQ;
    memcpy(req.media_id, dl->media_id, 16);
    memcpy(req.block_id, dl->blocks[bi].block_id, 16);
    memcpy(req.transfer_id, dl->blocks[bi].transfer_id, 16);
    req.chunk = (uint32_t)bi;

    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: BLOCK_REQ to %s blk=%d/%d block_id=%02x%02x%02x%02x...",
               MDL_ID, sc_public_key_hex(dst, (char[65]){0}), bi, dl->num_blocks,
               dl->blocks[bi].block_id[0], dl->blocks[bi].block_id[1], dl->blocks[bi].block_id[2], dl->blocks[bi].block_id[3]);
    return md_send_key(inst, dl->group_id, dst, (const uint8_t*)&req, sizeof(req), 1);
}

/* ── forward decl ── */
static void md_dl_send_block_processing(struct UTUN_INSTANCE* inst, struct media_download* dl, int bi);
static void md_dl_watchdog_cb(void* arg);
static int md_dl_query_pump(struct media_download* dl, uint64_t now_tb);
static void md_dl_finish(struct media_download* dl, int err);
static int md_dl_arm_watchdog(struct media_download* dl);
static void md_dl_pump_cb(void* arg);

static void md_dl_send_have_block(struct UTUN_INSTANCE* inst, struct media_download* dl, int bi) {
    const uint8_t* super = dl->announce_peer;
    if (!memcmp(super, (const uint8_t[32]){0}, 32)) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: HAVE_BLOCK but no supernode available", MDL_ID); return; }

    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: HAVE_BLOCK to super %s block=%d",
               MDL_ID, sc_public_key_hex(super, (char[65]){0}), bi);
    media_delivery_send_have_block(inst, dl->group_id, super, dl->media_id,
                                   dl->blocks[bi].block_id, (uint32_t)bi);
}

static void md_dl_send_block_processing(struct UTUN_INSTANCE* inst, struct media_download* dl, int bi) {
    const uint8_t* super = dl->announce_peer;
    if (!memcmp(super, (const uint8_t[32]){0}, 32)) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: BLOCK_PROCESSING but no supernode", MDL_ID); return; }

    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: BLOCK_PROCESSING to super %s block=%d",
               MDL_ID, sc_public_key_hex(super, (char[65]){0}), bi);
    media_delivery_send_processing(inst, dl->group_id, super, dl->media_id, dl->blocks[bi].block_id, (uint32_t)bi);
}

/* ── запрос блока у текущего держателя ── */

static void md_dl_end_attempt(struct media_download* dl, int bi) {
    struct media_download_block* b = &dl->blocks[bi];
    if ((b->state == MD_BLK_REQ || b->state == MD_BLK_RECV) && b->holder_idx >= 0 && b->holder_idx < b->num_holders) {
        struct media_pkt_cancel can = {.subcmd = MEDIA_SUBCMD_CANCEL, .chunk = (uint32_t)bi};
        memcpy(can.media_id, dl->media_id, 16); memcpy(can.block_id, b->block_id, 16);
        memcpy(can.transfer_id, b->transfer_id, 16);
        md_send_key(dl->inst, dl->group_id, b->holders[b->holder_idx], (const uint8_t*)&can, sizeof(can), 1);
    }
    if (b->source) {
        if (!b->source->verified) md_block_abort(b->source);
        md_block_release(b->source); b->source = NULL;
    }
}

static int md_dl_request_block(struct media_download* dl, int bi) {
    struct media_download_block* b = &dl->blocks[bi];
    uint64_t now = get_time_tb();
    if (b->holder_idx < 0) b->holder_idx = md_dl_select_holder(b, bi, now);
    if (b->holder_idx < 0 || b->holder_idx >= b->num_holders) {
        b->state = MD_BLK_WAIT; b->wait_reason = b->retry_at_tb ? MD_WAIT_RETRY : MD_WAIT_SEARCH; return 0;
    }
    const uint8_t* holder = b->holders[b->holder_idx]; uint8_t x[32];
    if (sc_identity_public_x("media holder", holder, x) != SC_OK || !topo_group_chat_key_allowed(dl->inst, dl->group_id, holder)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: block holder denied group=%llx holder=%s", MDL_ID,
                   (unsigned long long)dl->group_id, sc_public_key_hex(holder, (char[65]){0})); return -1;
    }
    struct media_download_peer* p = md_dl_get_peer(dl, holder);
    if (!p || p->pending_event) { b->state = MD_BLK_WAIT; b->wait_reason = MD_WAIT_SLOT; return 0; }
    if (md_dl_reset_block_file(dl, bi)) { dl->err = -1; return -1; }
    b->bytes_received = 0; b->last_progress_tb = now;
    b->state = MD_BLK_CONN; b->wait_reason = MD_WAIT_NONE;
    if (!memcmp(holder, dl->author_pubkey, 32)) b->author_tried = 1;
    dl->inflight++;
    struct UTUN_INSTANCE* inst = dl->inst;
    int rc = 0;
    if ((p->cm_handle && p->connected) || instance_find_conn_by_key(inst, x)) {
        p->connected = 1; b->state = MD_BLK_REQ;
        rc = md_dl_send_block_req(inst, holder, dl, bi);
    } else if (!p->cm_handle) {
        struct TOPO_GROUP* grp = inst->topo_groups ? topo_groups_find(inst->topo_groups, dl->group_id) : NULL;
        if (grp && grp->conn_mgr && topo_group_chat_key_allowed(inst, dl->group_id, holder)) {
            rc = conn_mgr_open(inst, grp->group_id, holder, md_dl_conn_cb, dl, &p->cm_handle);
        } else {
            p->connected = 1; b->state = MD_BLK_REQ;
            rc = md_dl_send_block_req(inst, holder, dl, bi);
        }
    }
    if (rc != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: block launch failed block=%d holder=%s rc=%d",
                   MDL_ID, bi, sc_public_key_hex(holder, (char[65]){0}), rc);
        dl->inflight--; b->state = MD_BLK_IDLE; return -1;
    }
    return dl->err ? -1 : 0;
}

/* Запускать только готовые блоки: ожидание слота и OVERLOADED не является провалом. */
static int md_dl_pump(struct media_download* dl) {
    if (dl->err) return -1;
    for (int bi = 0; bi < dl->num_blocks; bi++) {
        struct media_download_block* b = &dl->blocks[bi];
        if (b->state != MD_BLK_IDLE && b->state != MD_BLK_WAIT) continue;
        if (md_dl_request_block(dl, bi) < 0 && (dl->err || md_dl_failover_block(dl, bi, get_time_tb()) < 0)) return -1;
    }
    return 0;
}

/* Смена кандидата после реального провала; запрос новых держателей не обнуляет бюджет. */
int md_dl_failover_block(struct media_download* dl, int bi, uint64_t now_tb) {
    struct media_download_block* b = &dl->blocks[bi];
    if (b->state == MD_BLK_DONE || b->state == MD_BLK_FAILED) return 0;
    md_dl_end_attempt(dl, bi);
    if (md_dl_block_inflight(b) && dl->inflight > 0) dl->inflight--;
    b->state = MD_BLK_IDLE; b->bytes_received = 0; b->last_progress_tb = now_tb; b->retry_at_tb = 0;
    do {
        if (dl->err) return -1;
        b->attempts++;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: failover block=%d attempts=%d/%d holder=%d/%d",
                   MDL_ID, bi, b->attempts, md_dl_max_attempts(dl->inst), b->holder_idx, b->num_holders);
        int previous = b->holder_idx;
        if (previous >= 0) b->failed_holders |= UINT64_C(1) << previous;
        b->holder_idx = md_dl_select_holder(b, previous + 1, now_tb);
        if (b->holder_idx < 0 && !b->retry_at_tb && b->attempts >= md_dl_max_attempts(dl->inst)
            && b->query_next >= dl->super_count && (b->author_tried || !dl->super_count)) {
            b->state = MD_BLK_FAILED;
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: block attempts exhausted block=%d attempts=%d", MDL_ID, bi, b->attempts);
            return -1;
        }
        if (b->holder_idx < 0) {
            b->state = MD_BLK_WAIT; b->wait_reason = b->retry_at_tb ? MD_WAIT_RETRY : MD_WAIT_SEARCH;
            return 0;
        }
    } while (md_dl_request_block(dl, bi) < 0);
    return 0;
}

/* Дедлайн QUERY независим от трафика остальных блоков. Все переходы возвращают ошибку владельцу dl. */
int md_dl_check_stall(struct media_download* dl, uint64_t now_tb) {
    if (dl->err) return -1;
    if (dl->assembled) return 0;
    uint64_t stall = md_dl_stall_tb(dl->inst);
    for (int bi = 0; bi < dl->num_blocks; bi++) {
        struct media_download_block* b = &dl->blocks[bi];
        if ((b->state == MD_BLK_REQ || b->state == MD_BLK_RECV) && now_tb - b->last_progress_tb >= stall) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: block stalled block=%d state=%u", MDL_ID, bi, b->state);
            if (md_dl_failover_block(dl, bi, now_tb) < 0) return -1;
        }
    }
    if (dl->query_deadline_tb && now_tb >= dl->query_deadline_tb) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: QUERY timeout peer=%s blocks=%d",
                   MDL_ID, sc_public_key_hex(dl->query_peer, (char[65]){0}), dl->query_count);
        dl->query_deadline_tb = 0; dl->query_count = 0;
    }
    if (md_dl_query_pump(dl, now_tb) < 0) return -1;
    return md_dl_pump(dl);
}

/* Последний обработчик освобождает данные до done_cb. Отмена из progress_cb не уничтожает его аргумент. */
static void md_dl_release(struct media_download* dl) {
    if (--dl->refs) return;
    int err = dl->err;
    struct md_download_listener* listeners = dl->listeners;
    u_free(dl->blocks);
    u_free(dl->block_sigs);
    queue_entry_free(dl->entry);
    while (listeners) {
        struct md_download_listener* listener = listeners;
        listeners = listener->next;
        listener->done(listener->arg, listener->cancelled ? -2 : err);
        u_free(listener);
    }
}

static void md_dl_notify_cancelled(struct media_download* dl) {
    if (!dl->active || dl->notifying) return;
    int removed = 0;
    dl->notifying = 1;
    while (dl->active) {
        struct md_download_listener** slot = &dl->listeners;
        while (*slot && !(*slot)->cancelled) slot = &(*slot)->next;
        if (!*slot) break;
        struct md_download_listener* listener = *slot;
        *slot = listener->next;
        removed = 1;
        listener->done(listener->arg, -2);
        u_free(listener);
    }
    dl->notifying = 0;
    if (removed && !dl->listeners) md_dl_finish(dl, -2);
}

/* Завершение снимает активное владение; выполняющийся обработчик может ещё удерживать dl. */

static void md_dl_finish(struct media_download* dl, int err) {
    if (!dl->active) return;
    struct UTUN_INSTANCE* inst = dl->inst;
    struct media_delivery_ctx* md = &inst->md;

    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: finish media=%02x%02x err=%d validated=%d/%d inflight=%d peers=%d query=%d",
                MDL_ID, dl->media_id[0], dl->media_id[1], err, dl->blocks_validated, dl->num_blocks,
                dl->inflight, dl->num_peers, dl->query_count);

    dl->active = 0; dl->err = err;
    queue_remove_data(md->downloads, dl->entry);
    if (dl->pump_timer) { uasync_call_soon_cancel(inst->ua, dl->pump_timer); dl->pump_timer = NULL; }
    if (dl->watchdog_timer) { uasync_cancel_timeout(inst->ua, dl->watchdog_timer); dl->watchdog_timer = NULL; }
    for (int pi = 0; pi < dl->num_peers; pi++) {
        if (dl->peers[pi].cm_handle) { conn_mgr_close(dl->peers[pi].cm_handle); dl->peers[pi].cm_handle = NULL; }
    }
    for (int bi = 0; bi < dl->num_blocks; bi++) md_dl_end_attempt(dl, bi);
    media_delivery_release(inst, dl->destination); dl->destination = NULL;
    md_dl_release(dl);
}

/* Один таймер обслуживает ближайший дедлайн поиска, блока или задержанного повтора. */
static int md_dl_arm_watchdog(struct media_download* dl) {
    uint64_t now = get_time_tb(), due = now + md_dl_stall_tb(dl->inst);
    if (dl->query_deadline_tb && dl->query_deadline_tb < due) due = dl->query_deadline_tb;
    for (int bi = 0; bi < dl->num_blocks; bi++) {
        struct media_download_block* b = &dl->blocks[bi];
        uint64_t block_due = 0;
        if (b->state == MD_BLK_REQ || b->state == MD_BLK_RECV)
            block_due = b->last_progress_tb + md_dl_stall_tb(dl->inst);
        else if (b->state == MD_BLK_WAIT && b->wait_reason == MD_WAIT_RETRY) block_due = b->retry_at_tb;
        if (block_due && block_due < due) due = block_due;
    }
    if (dl->watchdog_timer) uasync_cancel_timeout(dl->inst->ua, dl->watchdog_timer);
    dl->watchdog_timer = uasync_set_timeout(dl->inst->ua, due > now ? (int)(due - now) : 1,
                                           dl, md_dl_watchdog_cb, "md_dl_wd");
    if (dl->watchdog_timer) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: cannot arm download watchdog", MDL_ID); return -1;
}

static void md_dl_watchdog_cb(void* arg) {
    struct media_download* dl = arg;
    dl->watchdog_timer = NULL;
    if (!dl->active) return;
    if (md_dl_check_stall(dl, get_time_tb()) < 0 || md_dl_arm_watchdog(dl) < 0) md_dl_finish(dl, -1);
}

/* CM может уведомить внутри open. Записываем событие и обрабатываем после возврата open,
 * когда handle уже опубликован; callbacks не создают новые peer-слоты. */
void md_dl_conn_cb(struct CONN_MGR_HANDLE* h, const uint8_t ed[32], uint64_t group_id,
                   enum conn_mgr_event event, void* arg) {
    struct media_download* dl = arg;
    if (!dl || !dl->active || dl->assembled) return;
    struct media_download_peer* p = NULL;
    for (int pi = 0; pi < dl->num_peers; pi++) {
        if (dl->peers[pi].cm_handle == h) { p = &dl->peers[pi]; break; }
    }
    if (!p || group_id != dl->group_id || !h || p->cm_handle != h || memcmp(p->ed, ed, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: stale CM event peer=%s handle=%p",
                   MDL_ID, sc_public_key_hex(ed, (char[65]){0}), (void*)h); return;
    }
    p->pending_event = (uint8_t)event + 1;
    if (event != CONN_EVENT_UP) {
        p->cm_handle = NULL; p->connected = 0;
        conn_mgr_close(h); // Сразу отменить отложенный UP, до любого следующего uasync callback.
    }
    if (!dl->pump_timer) dl->pump_timer = uasync_call_soon(dl->inst->ua, dl, md_dl_pump_cb);
    if (!dl->pump_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: cannot defer CM event", MDL_ID); dl->err = -1;
    }
}

/* Все CM-переходы завершаются до нового QUERY/pump и освобождения владельца. */
static void md_dl_pump_cb(void* arg) {
    struct media_download* dl = arg;
    dl->pump_timer = NULL;
    if (!topo_group_chat_key_allowed(dl->inst, dl->group_id, dl->inst->my_ed25519_pubkey)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: download membership revoked group=%llx", MDL_ID, (unsigned long long)dl->group_id);
        md_dl_finish(dl, -1); return;
    }
    int failed = dl->err != 0;
    for (int pi = 0; pi < dl->num_peers; pi++) {
        struct media_download_peer* p = &dl->peers[pi];
        if (!p->pending_event) continue;
        int event = p->pending_event - 1; p->pending_event = 0;
        uint8_t peer[32]; memcpy(peer, p->ed, sizeof(peer));
        p->connected = event == CONN_EVENT_UP;
        if (!p->connected) {
            struct CONN_MGR_HANDLE* h = p->cm_handle; p->cm_handle = NULL;
            conn_mgr_close(h);
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: CM unavailable peer=%s event=%d", MDL_ID, sc_public_key_hex(peer, (char[65]){0}), event);
        }
        for (int bi = 0; bi < dl->num_blocks; bi++) {
            struct media_download_block* b = &dl->blocks[bi];
            if (!md_dl_block_inflight(b) || b->holder_idx < 0 || b->holder_idx >= b->num_holders
                || memcmp(b->holders[b->holder_idx], peer, 32)) continue;
            if (event != CONN_EVENT_UP) {
                if (md_dl_failover_block(dl, bi, get_time_tb()) < 0) failed = 1;
            } else if (b->state == MD_BLK_CONN) {
                b->state = MD_BLK_REQ; b->last_progress_tb = get_time_tb();
                if (md_dl_send_block_req(dl->inst, peer, dl, bi) < 0
                    && md_dl_failover_block(dl, bi, get_time_tb()) < 0) failed = 1;
            }
        }
    }
    if (failed || md_dl_query_pump(dl, get_time_tb()) < 0 || md_dl_pump(dl) < 0 || md_dl_arm_watchdog(dl) < 0)
        md_dl_finish(dl, -1);
}

/* ── сбор суперузлов ── */

/* Только суперузлы запрошенной CHAT-группы; автор всегда имеет отдельное последнее место. */
static void md_dl_collect_supernodes(struct UTUN_INSTANCE* inst, struct media_download* dl) {
    dl->super_count = 0;
    struct TOPO_GROUP* g = inst->topo_groups ? topo_groups_find(inst->topo_groups, dl->group_id) : NULL;
    int rw = db_async_can_write(inst->db_worker);
    if (g && g->group_type == TOPO_GROUP_TYPE_CHAT && !g->stopping && g->channel_id[0] && rw) {
        size_t cursor = 0; uint8_t ed[32];
        while (dl->super_count < 9 && group_policy_role_key_next(inst, g->channel_id, GROUP_MEMBER_SUPERNODE, &cursor, ed))
            if (memcmp(ed, inst->my_ed25519_pubkey, 32) && memcmp(ed, dl->author_pubkey, 32))
                memcpy(dl->super_nodes[dl->super_count++], ed, sizeof(ed));
    } else if (g && g->group_type == TOPO_GROUP_TYPE_CHAT && !g->stopping && g->channel_id[0] && inst->topo_sqlite_db && !rw) {
        char sql[256];
        snprintf(sql, sizeof(sql), "SELECT ed25519_pubkey FROM peers_%s WHERE record_state=0 AND node_type=4 ORDER BY ed25519_pubkey", g->channel_id);
        sqlite3_stmt* st = NULL;
        int rc = sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL);
        if (rc == SQLITE_OK) {
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                const uint8_t* ed = sqlite3_column_blob(st, 0);
                if (ed && sqlite3_column_bytes(st, 0) == 32 && dl->super_count < 9 &&
                    memcmp(ed, inst->my_ed25519_pubkey, 32) && memcmp(ed, dl->author_pubkey, 32))
                    memcpy(dl->super_nodes[dl->super_count++], ed, 32);
            }
            int final_rc = sqlite3_finalize(st);
            if (rc != SQLITE_DONE || final_rc != SQLITE_OK)
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: source list failed rc=%d finalize=%d", MDL_ID, rc, final_rc);
        } else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: source list prepare: %s", MDL_ID, sqlite3_errmsg(inst->topo_sqlite_db));
    }
    if (memcmp(dl->author_pubkey, inst->my_ed25519_pubkey, 32))
        memcpy(dl->super_nodes[dl->super_count++], dl->author_pubkey, 32);
    if (dl->super_count) memcpy(dl->announce_peer, dl->super_nodes[0], 32);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: lookup sources=%d group=%016llx", MDL_ID, dl->super_count,
                (unsigned long long)dl->group_id);
}

/* Один запрос в полёте. У каждого блока свой курсор источников, продвигаемый до отправки:
 * потеря/ошибка ответа не повторяет источник бесконечно и не затрагивает другие пачки. */
static int md_dl_query_pump(struct media_download* dl, uint64_t now_tb) {
    if (dl->query_deadline_tb) return 0;
    for (;;) {
        int source = -1;
        dl->query_count = 0;
        for (int bi = 0; bi < dl->num_blocks; bi++) {
            struct media_download_block* b = &dl->blocks[bi];
            if (b->state == MD_BLK_DONE || md_dl_block_inflight(b)) continue;
            if (b->state == MD_BLK_FAILED) return -1;
            if (b->holder_idx >= 0 || md_dl_select_holder(b, bi, now_tb) >= 0) continue;
            if (b->query_next >= dl->super_count) {
                if (!b->author_tried && memcmp(dl->author_pubkey, (const uint8_t[32]){0}, 32) &&
                    memcmp(dl->author_pubkey, dl->inst->my_ed25519_pubkey, 32)) {
                    md_dl_add_holder(b, dl->author_pubkey); b->author_tried = 1;
                    if (md_dl_select_holder(b, bi, now_tb) >= 0) continue;
                }
                if (b->retry_at_tb) continue;
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: holders exhausted block=%d attempts=%d sources=%d",
                            MDL_ID, bi, b->attempts, dl->super_count); return -1;
            }
            if (source < 0) source = b->query_next;
            if (b->query_next == source && dl->query_count < MEDIA_QUERY_MAX_BLOCKS)
                dl->query_blocks[dl->query_count++] = bi;
        }
        if (!dl->query_count) return 0;
        if (media_index_generate_uuid(dl->query_id)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: QUERY UUID failed", MDL_ID); return -1;
        }
        memcpy(dl->query_peer, dl->super_nodes[source], sizeof(dl->query_peer));
        uint8_t packet[MEDIA_QUERY_HDR_SIZE + MEDIA_QUERY_MAX_BLOCKS * 16] = {0};
        struct media_pkt_query* q = (struct media_pkt_query*)packet;
        q->subcmd = MEDIA_SUBCMD_QUERY; q->num_blocks = (uint16_t)dl->query_count;
        memcpy(q->media_id, dl->media_id, 16); memcpy(q->request_id, dl->query_id, 16);
        for (int i = 0; i < dl->query_count; i++) {
            struct media_download_block* b = &dl->blocks[dl->query_blocks[i]];
            memcpy(packet + MEDIA_QUERY_HDR_SIZE + i * 16, b->block_id, 16);
            b->query_next++; b->wait_reason = MD_WAIT_SEARCH; b->state = MD_BLK_WAIT;
        }
        uint64_t timeout = dl->inst->md.query_timeout_tb > 0 ? dl->inst->md.query_timeout_tb : MEDIA_QUERY_TIMEOUT_TB;
        dl->query_deadline_tb = now_tb + timeout;
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: QUERY peer=%s source=%d blocks=%d id=%02x%02x deadline=%llu",
                    MDL_ID, sc_public_key_hex(dl->query_peer, (char[65]){0}), source, dl->query_count, dl->query_id[0], dl->query_id[1],
                    (unsigned long long)dl->query_deadline_tb);
        if (!md_send_key(dl->inst, dl->group_id, dl->query_peer, packet, MEDIA_QUERY_HDR_SIZE + (size_t)q->num_blocks * 16, 1))
            return 0;
        dl->query_deadline_tb = 0; // Отказ отправки: следующий источник той же пачки.
    }
}

/* Проверить весь ответ до любых изменений. Связь с запросом не зависит от совпадений block_id в других загрузках. */
void media_download_handle_query_resp(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t from_ed[32],
                                      const uint8_t* data, size_t len) {
    if (!inst || !data || len < MEDIA_QUERY_RESP_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: short QUERY_RESP bytes=%zu", MDL_ID, len); return;
    }
    const struct media_pkt_query_resp* r = (const struct media_pkt_query_resp*)data;
    if (r->num_blocks > MEDIA_QUERY_MAX_BLOCKS || r->status > MEDIA_QUERY_DB_ERROR
        || (r->status != MEDIA_QUERY_OK && r->num_blocks != 0)
        || len != MEDIA_QUERY_RESP_HDR_SIZE + (size_t)r->num_blocks * sizeof(struct media_pkt_query_resp_entry)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: invalid QUERY_RESP status=%u blocks=%u bytes=%zu",
                   MDL_ID, r->status, r->num_blocks, len); return;
    }
    struct media_download* dl = md_dl_find(inst, group_id, r->media_id);
    if (!dl || !dl->active || !dl->query_deadline_tb || get_time_tb() >= dl->query_deadline_tb
        || group_id != dl->group_id || memcmp(from_ed, dl->query_peer, sizeof(dl->query_peer))
        || memcmp(r->request_id, dl->query_id, 16)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: stale/unexpected QUERY_RESP peer=%s group=%016llx",
                    MDL_ID, sc_public_key_hex(from_ed, (char[65]){0}), (unsigned long long)group_id); return;
    }
    const struct media_pkt_query_resp_entry* entries = (const struct media_pkt_query_resp_entry*)(data + MEDIA_QUERY_RESP_HDR_SIZE);
    if (r->status == MEDIA_QUERY_OK) {
        if (r->num_blocks != dl->query_count) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: QUERY_RESP incomplete blocks=%u expected=%d", MDL_ID, r->num_blocks, dl->query_count);
            return;
        }
        for (int i = 0; i < dl->query_count; i++) {
            const struct media_pkt_query_resp_entry* e = &entries[i];
            if (memcmp(e->block_id, dl->blocks[dl->query_blocks[i]].block_id, 16) || e->num_holders > MEDIA_QUERY_MAX_HOLDERS)
                goto invalid;
            for (int h = 0; h < MEDIA_QUERY_MAX_HOLDERS; h++) {
                const uint8_t* holder = e->holders[h];
                int empty = !memcmp(holder, (const uint8_t[32]){0}, 32);
                if (h >= e->num_holders) { if (!empty) goto invalid; continue; }
                uint8_t x[32];
                if (empty || !memcmp(holder, inst->my_ed25519_pubkey, 32) || sc_identity_public_x("media QUERY holder", holder, x) != SC_OK)
                    goto invalid;
                for (int prev = 0; prev < h; prev++) if (!memcmp(holder, e->holders[prev], 32)) goto invalid;
            }
        }
        for (int i = 0; i < dl->query_count; i++) {
            struct media_download_block* b = &dl->blocks[dl->query_blocks[i]];
            if (b->state == MD_BLK_DONE || md_dl_block_inflight(b)) continue;
            for (int h = 0; h < entries[i].num_holders; h++) md_dl_add_holder(b, entries[i].holders[h]);
        }
    } else DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: QUERY_RESP database error peer=%s", MDL_ID, sc_public_key_hex(from_ed, (char[65]){0}));
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: QUERY_RESP accepted blocks=%u status=%u", MDL_ID, r->num_blocks, r->status);
    dl->query_deadline_tb = 0; dl->query_count = 0;
    if (md_dl_query_pump(dl, get_time_tb()) < 0 || md_dl_pump(dl) < 0 || md_dl_arm_watchdog(dl) < 0)
        md_dl_finish(dl, -1);
    return;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: QUERY_RESP invalid block/holder list", MDL_ID);
}

/* Проверяем всю принадлежность попытки до доступа к файлу/таймерам. chunk задаёт индекс O(1). */
static struct media_download* md_dl_match(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32],
    const uint8_t media_id[16], const uint8_t block_id[16], const uint8_t transfer_id[16], uint32_t chunk) {
    struct media_download* dl = md_dl_find(inst, group, media_id);
    if (dl && dl->active && dl->group_id == group && chunk < (uint32_t)dl->num_blocks) {
        struct media_download_block* b = &dl->blocks[chunk];
        if ((b->state == MD_BLK_REQ || b->state == MD_BLK_RECV) && b->holder_idx >= 0
            && b->holder_idx < b->num_holders && !memcmp(b->holders[b->holder_idx], peer, 32)
            && !memcmp(b->block_id, block_id, 16) && !memcmp(b->transfer_id, transfer_id, 16)) return dl;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: stale block packet group=%016llx peer=%s chunk=%u attempt=%02x%02x",
                MDL_ID, (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), chunk, transfer_id[0], transfer_id[1]);
    return NULL;
}

static void md_dl_reject_block(struct media_download* dl, int bi) {
    if (md_dl_failover_block(dl, bi, get_time_tb()) < 0 || md_dl_query_pump(dl, get_time_tb()) < 0
        || md_dl_pump(dl) < 0 || md_dl_arm_watchdog(dl) < 0) md_dl_finish(dl, -1);
}

void media_download_handle_chunk(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t from_ed[32],
                                 const uint8_t* data, size_t len) {
    if (!data || len < MEDIA_BLOCK_CHUNK_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: truncated CHUNK header bytes=%zu", MDL_ID, len); return;
    }
    const struct media_pkt_block_chunk* ch = (const struct media_pkt_block_chunk*)data;
    struct media_download* dl = md_dl_match(inst, group_id, from_ed, ch->media_id, ch->block_id, ch->transfer_id, ch->chunk);
    if (!dl) return;
    struct media_download_block* b = &dl->blocks[ch->chunk];
    if (!ch->data_len || ch->data_len > MEDIA_BLOCK_DATA_MAX || len != sizeof(*ch) + ch->data_len
        || ch->offset > b->expected_size || ch->data_len > b->expected_size - ch->offset) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: invalid CHUNK block=%u bytes=%zu offset=%u length=%u expected=%u",
                   MDL_ID, ch->chunk, len, ch->offset, ch->data_len, b->expected_size);
        md_dl_reject_block(dl, (int)ch->chunk); return;
    }
    uint32_t end = ch->offset + ch->data_len;
    if (end <= b->bytes_received) return; // Полный дубль не продлевает таймаут и не публикует байты заново.
    if (ch->offset != b->bytes_received) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: CHUNK gap/overlap block=%u offset=%u prefix=%u length=%u",
                   MDL_ID, ch->chunk, ch->offset, b->bytes_received, ch->data_len);
        md_dl_reject_block(dl, (int)ch->chunk); return;
    }
    FILE* file = b->source && !b->source->failed ? file_io_open_update(b->source->storage.temporary) : NULL;
    int failed = !file;
    if (file) {
        if (fseeko(file, (off_t)ch->offset, SEEK_SET) || fwrite(data + sizeof(*ch), 1, ch->data_len, file) != ch->data_len)
            failed = 1;
        if (fclose(file)) failed = 1;
    }
    if (failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: CHUNK local write failed block=%u offset=%u length=%u errno=%d",
                    MDL_ID, ch->chunk, ch->offset, ch->data_len, errno);
        md_dl_finish(dl, -1); return;
    }
    b->bytes_received = end; b->state = MD_BLK_RECV; b->last_progress_tb = get_time_tb();
    md_block_publish(b->source, end, 0);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: CHUNK committed block=%u prefix=%u/%u attempt=%02x%02x",
                MDL_ID, ch->chunk, end, b->expected_size, b->transfer_id[0], b->transfer_id[1]);
}

/* Регистрирует скачанные блоки в media_files (location = собранный файл), чтобы узел
 * мог отдавать их как держатель. Вызывается после успешной сборки и проверки хеша. */
struct md_index_sql {
    struct media_index_result value;
    uint8_t self[32], author_ed[32];
    char channel[65], path[MEDIA_PATH_MAX], base[MEDIA_BASE_MAX];
    struct media_pkt_query_resp_entry* local_holders;
};
struct md_index_job { struct media_download* dl; struct md_index_sql sql; uint8_t ids[]; };
/* Публикация родителя проверяется до первого QUERY/relay; полного локального файла ещё может не быть. */
static int md_dl_track_write(sqlite3* db, void* arg) {
    struct md_index_sql* data = arg; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM attachment_public_objects WHERE namespace=?1 AND id=?2"
        " AND hash=?3 AND size=?4 AND author=?5", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, data->channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, data->value.media_id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, data->value.content_hash, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 4, data->value.file_size);
    if (!rc) rc = sqlite3_bind_blob(st, 5, data->author_ed, 32, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_ROW) rc = SQLITE_OK; else if (rc == SQLITE_DONE) rc = SQLITE_AUTH; }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: download parent not published/mismatched ch=%s media=%02x%02x rc=%d", MDL_ID,
            data->channel, data->value.media_id[0], data->value.media_id[1], rc); return rc;
    }
    rc = media_index_path_access_read(db, data->path, data->base, &data->value);
    if (!rc) rc = media_index_track_preparation_write(db, data->channel, data->path, data->base);
    if (rc || !data->local_holders) return rc;
    for (int n = 0; n < data->value.num_blocks; n++) {
        struct media_pkt_query_resp_entry* entry = &data->local_holders[n];
        if (md_query_holders_read(db, data->self, data->channel, data->self, data->value.media_id, entry)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: local catalog query failed ch=%s block=%d; continue network lookup", MDL_ID, data->channel, n);
            entry->num_holders = 0; break;
        }
    }
    return SQLITE_OK;
}
static int md_dl_sql_current(void* arg) {
    struct md_index_job* job = (void*)((char*)arg - offsetof(struct md_index_job, sql)); struct media_download* dl = job->dl;
    return dl->active && dl->inst->md.initialized && !dl->inst->destroying &&
        dl->inst->group_generation == dl->db_generation && topo_group_chat_key_allowed(dl->inst, dl->group_id, dl->inst->my_ed25519_pubkey);
}
static int md_dl_index_write(sqlite3* db, void* arg) {
    struct md_index_sql* sql = arg;
    return media_index_commit_write(db, &sql->value, sql->self, NULL, sql->channel, sql->path, sql->base);
}
static void md_dl_index_done(void* arg, const struct db_async_result* result) {
    struct md_index_job* job = (void*)((char*)arg - offsetof(struct md_index_job, sql)); struct media_download* dl = job->dl;
    int current = md_dl_sql_current(arg);
    if (result->status || !current) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: index completion status=%d rc=%d current=%d: %s",
        MDL_ID, result->status, result->sqlite_code, current, result->message);
    if (dl->active) md_dl_finish(dl, !result->status && current ? 0 : -1);
}
static void md_dl_index_cleanup(void* arg) {
    struct md_index_job* job = (void*)((char*)arg - offsetof(struct md_index_job, sql));
    md_dl_release(job->dl); u_free(job);
}
/* 1=асинхронное завершение,0/-1 — legacy результат. Внешний обработчик уже удерживает dl. */
static int md_dl_register_async(struct media_download* dl) {
    size_t size = sizeof(struct md_index_job) + (size_t)dl->num_blocks * 16;
    if (size > UINT32_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: oversized index task bytes=%zu", MDL_ID, size); return -1; }
    struct md_index_job* job = u_calloc(1, size);
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: index task allocation failed", MDL_ID); return -1; }
    job->dl = dl; memcpy(job->sql.self, dl->inst->my_ed25519_pubkey, 32);
    struct media_index_result* value = &job->sql.value;
    value->file_size = dl->file_size; value->block_size = dl->block_size; value->num_blocks = dl->num_blocks; value->block_ids = job->ids;
    memcpy(value->media_id, dl->media_id, 16); memcpy(value->content_hash, dl->content_hash, 32);
    for (int n = 0; n < dl->num_blocks; n++) memcpy(job->ids + (size_t)n * 16, dl->blocks[n].block_id, 16);
    strcpy(job->sql.channel, topo_groups_channel(dl->inst->topo_groups, dl->group_id));
    snprintf(job->sql.path, sizeof(job->sql.path), "%s", dl->dest_path); snprintf(job->sql.base, sizeof(job->sql.base), "%s", dl->media_base);
    if (dl->pump_timer) { uasync_call_soon_cancel(dl->inst->ua, dl->pump_timer); dl->pump_timer = NULL; }
    if (dl->watchdog_timer) { uasync_cancel_timeout(dl->inst->ua, dl->watchdog_timer); dl->watchdog_timer = NULL; }
    dl->refs++;
    db_async_run(dl->inst->db_worker, &(struct db_async_task){.name="download index", .mode=DB_ASYNC_WRITE, .data=&job->sql,
        .guard=md_dl_sql_current, .work=md_dl_index_write, .done=md_dl_index_done, .cleanup=md_dl_index_cleanup});
    return 1;
}
static int md_dl_register_servable(struct media_download* dl) {
    struct UTUN_INSTANCE* inst = dl->inst;
    if (db_async_can_write(inst->db_worker)) return md_dl_register_async(dl);
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL;
    if (!db) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: register_servable without DB", MDL_ID); return -1; }
    const char* chat_id = topo_groups_channel(dl->inst->topo_groups, dl->group_id);
    uint8_t* ids = u_malloc((uint32_t)dl->num_blocks * 16);
    if (!ids) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: register_servable allocation failed", MDL_ID); return -1; }
    for (int n = 0; n < dl->num_blocks; n++) memcpy(ids + (size_t)n * 16, dl->blocks[n].block_id, 16);
    struct media_index_result result = { .file_size = dl->file_size, .block_size = dl->block_size,
        .num_blocks = dl->num_blocks, .block_ids = ids };
    memcpy(result.media_id, dl->media_id, 16); memcpy(result.content_hash, dl->content_hash, 32);
    int rc = media_index_commit(db, &result, inst->my_ed25519_pubkey, NULL, chat_id, dl->dest_path, dl->media_base);
    u_free(ids); return rc;
}

/* Запись назначения начинается только после проверки блоков. До publish старое имя сохраняет прежние байты. */
static int md_dl_assemble(struct media_download* dl) {
    struct file_io_temp output = {0};
    int failed = file_io_temp_open(&output, dl->dest_path, FILE_IO_DURABLE);
    uint64_t total = 0;
    uint8_t buffer[65536];
    for (int bi = 0; bi < dl->num_blocks && !failed; bi++) {
        struct media_download_block* b = &dl->blocks[bi];
        struct file_io_info info;
        if (!b->source || !b->source->verified || file_io_inspect(b->source->storage.temporary, 0, &info)
            || info.type != FILE_IO_REGULAR || info.size != b->expected_size) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: assembly source invalid block=%d", MDL_ID, bi); failed = 1; break;
        }
        FILE* file = file_io_open_read(b->source->storage.temporary);
        if (!file) { failed = 1; break; }
        uint32_t left = b->expected_size;
        while (left && !failed) {
            size_t count = left < sizeof(buffer) ? left : sizeof(buffer);
            if (fread(buffer, 1, count, file) != count || fwrite(buffer, 1, count, output.file) != count) failed = 1;
            else { left -= (uint32_t)count; total += count; }
        }
        if (fclose(file)) failed = 1;
    }
    if (!failed && total != (uint64_t)dl->file_size) failed = 1;
    if (output.file && file_io_temp_finish(&output)) failed = 1;
    uint8_t hash[32];
    if (!failed && (ma_sha256_file(output.temporary, hash) || memcmp(hash, dl->content_hash, 32))) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: assembled content hash mismatch/read error path=%s", MDL_ID, dl->dest_path);
        failed = 1;
    }
    if (!failed && file_io_temp_publish(&output, output.temporary, dl->dest_path)) failed = 1;
    file_io_temp_discard(&output);
    if (failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: assembly failed bytes=%llu/%lld path=%s errno=%d",
                    MDL_ID, (unsigned long long)total, (long long)dl->file_size, dl->dest_path, errno); return -1;
    }
    dl->assembled = 1;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: verified file published path=%s bytes=%llu",
               MDL_ID, dl->dest_path, (unsigned long long)total);
    return md_dl_register_servable(dl);
}

void media_download_handle_done(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t from_ed[32],
                                const uint8_t* data, size_t len) {
    if (!data || len != MEDIA_BLOCK_DONE_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: malformed DONE bytes=%zu", MDL_ID, len); return;
    }
    const struct media_pkt_block_done* done = (const struct media_pkt_block_done*)data;
    struct media_download* dl = md_dl_match(inst, group_id, from_ed, done->media_id, done->block_id,
                                           done->transfer_id, done->chunk);
    if (!dl) return;
    int bi = (int)done->chunk;
    struct media_download_block* b = &dl->blocks[bi];
    if (done->status != MEDIA_BLOCK_OK || done->total_size != b->expected_size || b->bytes_received != b->expected_size) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: incomplete DONE block=%d status=%u wire=%u prefix=%u expected=%u",
                   MDL_ID, bi, done->status, done->total_size, b->bytes_received, b->expected_size);
        md_dl_reject_block(dl, bi); return;
    }
    struct file_io_info info;
    if (!b->source || b->source->failed || file_io_inspect(b->source->storage.temporary, 0, &info)
        || info.type != FILE_IO_REGULAR || info.size != b->expected_size) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: DONE local file invalid block=%d", MDL_ID, bi); md_dl_finish(dl, -1); return;
    }
    uint8_t* buffer = u_malloc(b->expected_size);
    FILE* file = buffer ? file_io_open_read(b->source->storage.temporary) : NULL;
    int failed = !file;
    if (file) {
        if (fread(buffer, 1, b->expected_size, file) != b->expected_size) failed = 1;
        if (fclose(file)) failed = 1;
    }
    if (failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: DONE local read/allocation failed block=%d size=%u errno=%d",
                    MDL_ID, bi, b->expected_size, errno);
        u_free(buffer); md_dl_finish(dl, -1); return;
    }
    int signature = sc_ed25519_verify(dl->author_pubkey, buffer, b->expected_size, dl->block_sigs + (size_t)bi * 64);
    u_free(buffer);
    if (signature != SC_OK) {
        if (signature != SC_ERR_AUTH_FAILED) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: local signature verification failed block=%d rc=%d", MDL_ID, bi, signature);
            md_dl_finish(dl, -1); return;
        }
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: DONE author signature invalid block=%d rc=%d", MDL_ID, bi, signature);
        md_dl_reject_block(dl, bi); return;
    }
    b->state = MD_BLK_DONE; dl->inflight--; dl->blocks_validated++;
    md_block_publish(b->source, b->expected_size, 1);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: block verified block=%d progress=%d/%d", MDL_ID, bi, dl->blocks_validated, dl->num_blocks);
    dl->refs++;
    dl->notifying = 1;
    for (struct md_download_listener* listener = dl->listeners; dl->active && listener; listener = listener->next)
        if (!listener->cancelled && listener->progress) listener->progress(listener->progress_arg, dl->blocks_validated, dl->num_blocks);
    dl->notifying = 0;
    md_dl_notify_cancelled(dl);
    if (dl->active) {
        md_dl_send_have_block(inst, dl, bi);
        if (dl->blocks_validated == dl->num_blocks) {
            int result = md_dl_assemble(dl); if (result <= 0) md_dl_finish(dl, result);
        }
        else if (md_dl_pump(dl) < 0) md_dl_finish(dl, -1);
    }
    md_dl_release(dl);
}

void media_download_handle_overloaded(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t from_ed[32],
                                      const uint8_t* data, size_t len) {
    if (!data || len != sizeof(struct media_pkt_block_overloaded)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: malformed OVERLOADED bytes=%zu", MDL_ID, len); return;
    }
    const struct media_pkt_block_overloaded* ov = (const struct media_pkt_block_overloaded*)data;
    struct media_download* dl = md_dl_match(inst, group_id, from_ed, ov->media_id, ov->block_id, ov->transfer_id, ov->chunk);
    if (!dl) return;
    struct media_download_block* b = &dl->blocks[ov->chunk];
    uint32_t delay = ov->retry_after_ms ? ov->retry_after_ms : 2000;
    if (delay > 30000) delay = 30000;
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: OVERLOADED block=%u peer=%s delay=%u",
               MDL_ID, ov->chunk, sc_public_key_hex(from_ed, (char[65]){0}), delay);
    md_dl_end_attempt(dl, (int)ov->chunk);
    dl->inflight--; b->bytes_received = 0;
    uint64_t now = get_time_tb();
    md_dl_backoff_holder(dl, b, now + (uint64_t)delay * 10);
    b->holder_idx = md_dl_select_holder(b, b->holder_idx + 1, now);
    b->state = MD_BLK_IDLE;
    if (md_dl_pump(dl) < 0 || md_dl_query_pump(dl, now) < 0 || md_dl_arm_watchdog(dl) < 0) md_dl_finish(dl, -1);
}

void media_download_handle_relay_full(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t from_ed[32],
                                      const uint8_t* data, size_t len) {
    if (!data || len < MEDIA_RELAY_FULL_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: truncated RELAY_FULL bytes=%zu", MDL_ID, len); return;
    }
    const struct media_pkt_block_relay_full* full = (const struct media_pkt_block_relay_full*)data;
    unsigned count = full->num_relay_nodes;
    if (!count || count > MD_MAX_RELAY_DOWNSTREAM || len != sizeof(*full) + count * 32) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: malformed RELAY_FULL bytes=%zu nodes=%u", MDL_ID, len, count); return;
    }
    struct media_download* dl = md_dl_match(inst, group_id, from_ed, full->media_id, full->block_id,
                                           full->transfer_id, full->chunk);
    if (!dl) return;
    uint8_t nodes[MD_MAX_RELAY_DOWNSTREAM][32];
    memcpy(nodes, data + sizeof(*full), count * sizeof(nodes[0]));
    for (unsigned i = 0; i < count; i++) {
        uint8_t x[32];
        if (!memcmp(nodes[i], inst->my_ed25519_pubkey, 32) || !memcmp(nodes[i], from_ed, 32) ||
            sc_identity_public_x("media relay holder", nodes[i], x) != SC_OK) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: RELAY_FULL invalid node index=%u", MDL_ID, i); return;
        }
        for (unsigned j = 0; j < i; j++) if (!memcmp(nodes[i], nodes[j], 32)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: RELAY_FULL duplicate node index=%u", MDL_ID, i); return;
        }
    }
    struct media_download_block* b = &dl->blocks[full->chunk];
    md_dl_end_attempt(dl, (int)full->chunk);
    dl->inflight--; b->bytes_received = 0;
    uint64_t now = get_time_tb();
    md_dl_backoff_holder(dl, b, now + md_dl_stall_tb(inst));
    for (unsigned i = 0; i < count; i++) md_dl_add_holder(b, nodes[i]);
    b->holder_idx = md_dl_select_holder(b, b->holder_idx + 1, now);
    b->state = MD_BLK_IDLE;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: RELAY_FULL block=%u holders=%d", MDL_ID, full->chunk, b->num_holders);
    if (md_dl_pump(dl) < 0 || md_dl_query_pump(dl, now) < 0 || md_dl_arm_watchdog(dl) < 0) md_dl_finish(dl, -1);
}

/* ── public API ── */

static void md_dl_begin(struct media_download* dl) {
    if (md_mkdir_parent(dl->dest_path)) { md_dl_finish(dl, -1); return; }
    md_dl_collect_supernodes(dl->inst, dl);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: download started media=%02x%02x blocks=%d sources=%d after durable preparation", MDL_ID,
        dl->media_id[0], dl->media_id[1], dl->num_blocks, dl->super_count);
    if (md_dl_query_pump(dl, get_time_tb()) < 0 || md_dl_pump(dl) < 0 || md_dl_arm_watchdog(dl) < 0) md_dl_finish(dl, -1);
}

static void md_dl_add_local_holders(struct media_download* dl, int block, const struct media_pkt_query_resp_entry* entry) {
    for (unsigned n = 0; n < entry->num_holders; n++)
        if (topo_group_chat_key_allowed(dl->inst, dl->group_id, entry->holders[n])) md_dl_add_holder(&dl->blocks[block], entry->holders[n]);
    if (dl->blocks[block].num_holders) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: local catalog media=%02x%02x block=%d holders=%d",
        MDL_ID, dl->media_id[0], dl->media_id[1], block, dl->blocks[block].num_holders);
}

static void md_dl_tracked(void* arg, const struct db_async_result* result) {
    struct md_index_job* job = (void*)((char*)arg - offsetof(struct md_index_job, sql)); struct media_download* dl = job->dl;
    int current = dl->active && dl->inst->md.initialized && !dl->inst->destroying &&
        dl->inst->group_generation == dl->db_generation && topo_group_chat_key_allowed(dl->inst, dl->group_id, dl->inst->my_ed25519_pubkey);
    if (result->status || !current) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: preparation completion status=%d rc=%d current=%d: %s", MDL_ID,
            result->status, result->sqlite_code, current, result->message);
        if (dl->active) md_dl_finish(dl, -1);
    } else {
        for (int n = 0; job->sql.local_holders && n < dl->num_blocks; n++) md_dl_add_local_holders(dl, n, &job->sql.local_holders[n]);
        md_dl_begin(dl);
    }
}

static int md_dl_subscribe(struct media_download* dl, void (*done)(void*, int), void* arg,
    void (*progress)(void*, int, int), void* progress_arg) {
    struct md_download_listener* listener = u_calloc(1, sizeof(*listener));
    if (!listener) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: cannot allocate download listener", MDL_ID); return -1; }
    listener->done = done; listener->arg = arg; listener->progress = progress; listener->progress_arg = progress_arg;
    listener->next = dl->listeners; dl->listeners = listener;
    return 0;
}

int media_download_start(struct UTUN_INSTANCE* inst, uint64_t group_id,
                         const struct media_index_result* result,
                         const char* dest_path, const char* media_base,
                         const uint8_t author_pubkey[32],
                         void (*done_cb)(void* arg, int err), void* done_arg,
                         void (*progress_cb)(void* arg, int blocks_done, int num_blocks), void* progress_arg) {
    if (!inst || !result || !dest_path || !media_base || !author_pubkey || !done_cb || strlen(media_base) >= MEDIA_BASE_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: start with NULL args inst=%p result=%p dest=%p base=%p cb=%p",
                    MDL_ID, (void*)inst, (void*)result, (void*)dest_path, (void*)media_base, (void*)done_cb);
        return -1;
    }
    struct media_delivery_ctx* md = &inst->md;
    int rw = db_async_can_write(inst->db_worker);
    if (!md->initialized) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: start before media_delivery init", MDL_ID); return -1; }
    if (!topo_group_chat_key_allowed(inst, group_id, inst->my_ed25519_pubkey)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: start denied outside active CHAT membership group=%llx", MDL_ID,
                   (unsigned long long)group_id); return -1;
    }

    char location[MEDIA_PATH_MAX], normalized[MEDIA_PATH_MAX];
    const char* channel = topo_groups_channel(inst->topo_groups, group_id);
    if (result->file_size <= 0 || result->block_size <= 0 || result->block_size > MEDIA_BLOCK_SIZE_MAX
        || result->num_blocks <= 0 || (uint64_t)result->num_blocks > UINT32_MAX / sizeof(struct media_download_block)
        || (uint64_t)result->num_blocks > UINT32_MAX / 64
        || (result->file_size - 1) / result->block_size + 1 != result->num_blocks
        || !result->block_ids || !result->block_sigs || media_index_location(dest_path, media_base, location)
        || (!rw && media_index_set_base(inst->topo_sqlite_db, media_base)) || media_index_path(media_base, location, normalized)
        || !group_policy_member_verified(inst, channel, author_pubkey)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: invalid file geometry/path/base before download", MDL_ID); return -1;
    }
    struct media_download* existing = md_dl_find(inst, group_id, result->media_id);
    if (existing) {
        int same = !strcmp(existing->dest_path, normalized) && !memcmp(existing->author_pubkey, author_pubkey, 32) &&
            existing->file_size == result->file_size && existing->block_size == result->block_size && existing->num_blocks == result->num_blocks &&
            !memcmp(existing->content_hash, result->content_hash, 32) && !memcmp(existing->block_sigs, result->block_sigs, (size_t)result->num_blocks * 64);
        for (int n = 0; same && n < result->num_blocks; n++) same = !memcmp(existing->blocks[n].block_id, result->block_ids + (size_t)n * 16, 16);
        if (!same) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: conflicting active download ch=%s path=%s", MDL_ID, channel, normalized); return -1;
        }
        if (md_dl_subscribe(existing, done_cb, done_arg, progress_cb, progress_arg)) return -1;
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: joined active download ch=%s media=%02x%02x", MDL_ID, channel, result->media_id[0], result->media_id[1]);
        return 0;
    }
    struct md_index_sql tracking = {0}; memcpy(tracking.author_ed, author_pubkey, 32);
    memcpy(tracking.self, inst->my_ed25519_pubkey, 32);
    memcpy(tracking.value.media_id, result->media_id, 16); memcpy(tracking.value.content_hash, result->content_hash, 32);
    tracking.value.file_size = result->file_size;
    snprintf(tracking.channel, sizeof(tracking.channel), "%s", channel);
    snprintf(tracking.path, sizeof(tracking.path), "%s", normalized); snprintf(tracking.base, sizeof(tracking.base), "%s", media_base);
    if (!rw) {
        sqlite3* db = inst->topo_sqlite_db;
        int rc = !sqlite3_get_autocommit(db) ? SQLITE_MISUSE : sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
        int owns = !rc; if (!rc) rc = md_dl_track_write(db, &tracking);
        if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        if (rc) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: download preparation rejected group=%llx rc=%d", MDL_ID, (unsigned long long)group_id, rc);
            if (owns && !sqlite3_get_autocommit(db)) {
                int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
                if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: download preparation rollback rc=%d", MDL_ID, rollback);
                if (!sqlite3_get_autocommit(db)) utun_instance_stop(inst);
            }
            return -1;
        }
    }

    struct media_download* dl = u_calloc(1, sizeof(*dl));
    if (!dl) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: u_calloc for download failed", MDL_ID); return -1; }

    memcpy(dl->media_id, result->media_id, 16);
    dl->group_id = group_id;
    snprintf(dl->dest_path, sizeof(dl->dest_path), "%s", normalized);
    if (rw) snprintf(dl->media_base, sizeof(dl->media_base), "%s", media_base);
    else if (media_index_get_base(inst->topo_sqlite_db, dl->media_base)) { u_free(dl); return -1; }
    memcpy(dl->author_pubkey, author_pubkey, 32);
    dl->num_blocks = result->num_blocks;
    dl->file_size = result->file_size;
    dl->block_size = result->block_size;
    memcpy(dl->content_hash, result->content_hash, 32);
    dl->active = 1;
    dl->inst = inst;
    dl->db_generation = inst->group_generation;

    dl->blocks = u_calloc((size_t)dl->num_blocks, sizeof(struct media_download_block));
    dl->block_sigs = u_malloc((size_t)dl->num_blocks * 64);
    if (!dl->blocks || !dl->block_sigs) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: alloc blocks/sigs failed", MDL_ID);
        u_free(dl->blocks); u_free(dl->block_sigs); u_free(dl); return -1;
    }
    for (int i = 0; i < dl->num_blocks; i++) {
        memcpy(dl->blocks[i].block_id, result->block_ids + (size_t)i * 16, 16);
        dl->blocks[i].state = MD_BLK_IDLE;
        dl->blocks[i].holder_idx = -1;
        int64_t start = (int64_t)i * dl->block_size;
        int64_t rem = dl->file_size > 0 ? dl->file_size - start : 0;
        dl->blocks[i].expected_size = (uint32_t)((rem > 0 && rem < dl->block_size) ? rem : dl->block_size);
    }
    memcpy(dl->block_sigs, result->block_sigs, (size_t)dl->num_blocks * 64);

    /* регистрация в очереди */
    if (!md->downloads) {
        md->downloads = queue_new(inst->ua, 256, offsetof(struct media_download, group_id), MEDIA_DOWNLOAD_KEY_SIZE, "md_dl");
        if (!md->downloads) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: queue_new(downloads) failed", MDL_ID); u_free(dl->blocks); u_free(dl->block_sigs); u_free(dl); return -1; }
    }
    struct ll_entry* qe = queue_entry_new(sizeof(struct media_download));
    if (!qe) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: queue_entry_new(download) failed", MDL_ID); u_free(dl->blocks); u_free(dl->block_sigs); u_free(dl); return -1; }
    memcpy(qe->data, dl, sizeof(*dl));
    u_free(dl);
    dl = (struct media_download*)qe->data;
    dl->entry = qe;
    dl->refs = 1;
    dl->destination = media_delivery_reserve_base(inst, dl->dest_path, dl->media_base);
    if (!dl->destination) {
        media_delivery_release(inst, dl->destination);
        u_free(dl->blocks); u_free(dl->block_sigs); queue_entry_free(qe); return -1;
    }
    if (md_dl_subscribe(dl, done_cb, done_arg, progress_cb, progress_arg)) {
        media_delivery_release(inst, dl->destination);
        u_free(dl->blocks); u_free(dl->block_sigs); queue_entry_free(qe); return -1;
    }
    struct md_download_listener* listener = dl->listeners;
    struct media_destination* destination = dl->destination;
    struct media_download_block* blocks = dl->blocks; uint8_t* signatures = dl->block_sigs;
    if (queue_data_put_with_index(md->downloads, qe)) {
        media_delivery_release(inst, destination); u_free(blocks); u_free(signatures); u_free(listener); return -1;
    }
    int local_catalog = group_policy_member_role_key(inst, channel, inst->my_ed25519_pubkey, GROUP_MEMBER_SUPERNODE) == 1;
    if (rw) {
        size_t bytes = local_catalog ? (size_t)dl->num_blocks * sizeof(struct media_pkt_query_resp_entry) : 0;
        struct md_index_job* job = u_calloc(1, sizeof(*job) + bytes);
        if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: download preparation allocation failed", MDL_ID); md_dl_finish(dl, -1); return 0; }
        job->dl = dl; job->sql = tracking;
        if (local_catalog) {
            job->sql.value.num_blocks = dl->num_blocks;
            job->sql.local_holders = (void*)job->ids;
            for (int n = 0; n < dl->num_blocks; n++) memcpy(job->sql.local_holders[n].block_id, dl->blocks[n].block_id, 16);
        }
        dl->refs++;
        db_async_run(inst->db_worker, &(struct db_async_task){.name="download preparation", .mode=DB_ASYNC_WRITE, .data=&job->sql,
            .guard=md_dl_sql_current, .work=md_dl_track_write, .done=md_dl_tracked, .cleanup=md_dl_index_cleanup});
        return 0;
    }
    for (int n = 0; local_catalog && n < dl->num_blocks; n++) {
        struct media_pkt_query_resp_entry entry;
        memcpy(entry.block_id, dl->blocks[n].block_id, 16);
        if (md_query_holders_read(inst->topo_sqlite_db, tracking.self, channel, tracking.self, dl->media_id, &entry)) break;
        md_dl_add_local_holders(dl, n, &entry);
    }
    md_dl_begin(dl);
    return 0;
}

void media_download_peer_closed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32]) {
    for (struct ll_entry* e = inst->md.downloads ? inst->md.downloads->head : NULL; e; e = e->next) {
        struct media_download* dl = (struct media_download*)e->data;
        if (dl->group_id != group || dl->assembled) continue;
        int affected = dl->query_deadline_tb && !memcmp(dl->query_peer, peer, sizeof(dl->query_peer));
        if (affected) dl->query_deadline_tb = 0;
        for (int i = 0; i < dl->num_peers; i++) {
            struct media_download_peer* p = &dl->peers[i];
            if (memcmp(p->ed, peer, sizeof(p->ed))) continue;
            if (p->cm_handle) { conn_mgr_close(p->cm_handle); p->cm_handle = NULL; }
            p->connected = 0; p->pending_event = CONN_EVENT_DOWN + 1; affected = 1;
        }
        if (!affected) continue;
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: router closed group=%llx peer=%s; defer download failover", MDL_ID,
                   (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}));
        if (!dl->pump_timer) dl->pump_timer = uasync_call_soon(inst->ua, dl, md_dl_pump_cb);
        if (!dl->pump_timer) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: cannot schedule closed peer", MDL_ID); dl->err = -1; }
    }
}

int media_download_cancel_group_object(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t media_id[16]) {
    struct media_download* dl = md_dl_find(inst, group, media_id);
    if (!dl) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: cancel — download not found", MDL_ID); return -1; }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: download cancelled", MDL_ID);
    md_dl_finish(dl, -2);
    return 0;
}

int media_download_cancel(struct UTUN_INSTANCE* inst, const uint8_t* media_id, const uint8_t* block_id, uint32_t chunk) {
    (void)block_id; (void)chunk;
    return media_download_cancel_group_object(inst, 0, media_id);
}

int media_download_unsubscribe(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t media_id[16], void* arg) {
    struct media_download* dl = md_dl_find(inst, group, media_id);
    if (!dl) return -1;
    struct md_download_listener* listener = dl->listeners;
    while (listener && (listener->cancelled || listener->arg != arg)) listener = listener->next;
    if (!listener) return -1;
    listener->cancelled = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: cancelled request group=%llx media=%02x%02x", MDL_ID,
        (unsigned long long)group, media_id[0], media_id[1]);
    dl->refs++;
    md_dl_notify_cancelled(dl);
    md_dl_release(dl);
    return 0;
}

/* ── дамп состояния (для отладки/тестов) ── */

void md_dl_dump(struct media_download* dl) {
    if (!dl) return;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s: dump media=%02x%02x... blocks=%d validated=%d inflight=%d active=%d attempts_budget=%d",
               MDL_ID, dl->media_id[0], dl->media_id[1], dl->num_blocks, dl->blocks_validated, dl->inflight, dl->active,
               dl->inst ? md_dl_max_attempts(dl->inst) : 0);
    for (int bi = 0; bi < dl->num_blocks; bi++) {
        struct media_download_block* b = &dl->blocks[bi];
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "%s:   blk=%d state=%d holders=%d hidx=%d attempts=%d bytes=%u/%u",
                   MDL_ID, bi, b->state, b->num_holders, b->holder_idx, b->attempts, b->bytes_received, b->expected_size);
    }
}
