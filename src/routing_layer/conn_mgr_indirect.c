/* conn_mgr_indirect.c — согласование и проверка пути через посредников. */
#include "conn_mgr_priv.h"
#include "etcp.h"
#include <string.h>
#include "../lib/debug_config.h"
#include "../lib/u_async.h"
#include "utun_instance.h"
#include "topo_group.h"

/* Оба плеча посредника должны быть прямыми READY-сессиями. RTT=0/FFFF — нет замера. */
static uint16_t cm_valid_candidate_rtt(struct CONN_MGR* mgr, const uint8_t* target,
                                      const struct CONN_MGR_CANDIDATE* candidate) {
    if (!candidate->rtt || candidate->rtt == UINT16_MAX ||
        !memcmp(candidate->ed, mgr->instance->my_ed25519_pubkey, SC_PUBKEY_SIZE) ||
        (target && !memcmp(candidate->ed, target, SC_PUBKEY_SIZE))) return UINT16_MAX;
    return cm_candidate_rtt(mgr, candidate->ed);
}

static int cm_has_candidate(const struct CONN_MGR_CANDIDATE* candidates, uint8_t count, const uint8_t* ed) {
    for (uint8_t i = 0; i < count; i++) if (!memcmp(candidates[i].ed, ed, SC_PUBKEY_SIZE)) return 1;
    return 0;
}

static uint8_t cm_collect_candidates(struct CONN_MGR* mgr, const uint8_t* target,
                                     const struct CONN_MGR_CANDIDATE* candidates, uint8_t count,
                                     struct CONN_MGR_CANDIDATE* out) {
    uint8_t used = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint16_t rtt = cm_valid_candidate_rtt(mgr, target, &candidates[i]);
        if (rtt == UINT16_MAX || cm_has_candidate(out, used, candidates[i].ed)) continue;
        out[used] = candidates[i];
        out[used++].rtt = rtt;
    }
    return used;
}

void cm_start_phase_indirect(struct CONN_MGR_ENTRY* entry) {
    struct CONN_MGR* mgr = entry->mgr;
    if (!entry->indirect_started) {
        entry->indirect_started = 1; entry->exchange_request_id = ++mgr->next_request_id;
        if (!entry->exchange_request_id) entry->exchange_request_id = ++mgr->next_request_id;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: INDIRECT start peer=%s elapsed=%llums candidates=%u",
                   sc_public_key_hex(entry->ed, (char[65]){0}), (unsigned long long)((get_time_tb() - entry->start_tb) / 10),
                   mgr->best_candidate_count);
    }
    entry->indirect_next_tb = get_time_tb() + CONN_MGR_EXCHANGE_RETRANSMIT_MS * 10;
    if (entry->selection.count) {
        cm_send_control(mgr, entry->ed, &entry->selection, sizeof(entry->selection));
        return;
    }
    struct CM_EXCHANGE_REQ req = {0};
    req.cmd = ETCP_RT_ID_CONN_MGR; req.subcmd = CM_SUBCMD_INTERM_EXCHANGE_REQ; req.request_id = entry->exchange_request_id;
    const uint8_t* target = entry->ed;
    req.candidate_count = cm_collect_candidates(mgr, target, mgr->best_candidates, mgr->best_candidate_count, req.candidates);
    // Даже пустой запрос позволяет получить кандидатов другой стороны. Повторы подхватят новые замеры.
    cm_send_control(mgr, entry->ed, &req, sizeof(req));
}

void cm_finalize_indirect(struct CONN_MGR_ENTRY* entry) {
    if (!entry->intermediariy_count) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: empty intermediary selection peer=%s", sc_public_key_hex(entry->ed, (char[65]){0}));
        return;
    }
    entry->indirect_confirmed = 1;
    uint8_t old_type = entry->conn_type;
    cm_refresh_path(entry); // Готовый прямой путь имеет приоритет; повторного UP при улучшении нет.
    if (entry->state != CONN_MGR_STATE_DISCONNECTED && entry->conn_type == old_type) cm_update_nodeinfo(entry);
    if (entry->state != CONN_MGR_STATE_DISCONNECTED) cm_arm_timer(entry);
}

/* Объединение кандидатов обеих сторон без дублей, выбор по сумме двух действительных RTT. */
static void cm_compute_intermediaries(struct CONN_MGR_ENTRY* entry, const struct CM_EXCHANGE_RESP* resp) {
    struct scored_candidate { struct CONN_MGR_CANDIDATE candidate; uint32_t sum_rtt; };
    struct scored_candidate all[2 * CONN_MGR_MAX_CANDIDATES]; uint8_t count = 0;
    const uint8_t* target = entry->ed;
    for (int side = 0; side < 2; side++) {
        uint8_t n = side ? resp->your_count : resp->my_count;
        const struct CONN_MGR_CANDIDATE* candidates = side ? resp->your_candidates : resp->my_candidates;
        for (uint8_t i = 0; i < n; i++) {
            uint16_t rtt = cm_valid_candidate_rtt(entry->mgr, target, &candidates[i]);
            if (rtt == UINT16_MAX) continue;
            int duplicate = 0;
            for (uint8_t j = 0; j < count; j++)
                if (!memcmp(all[j].candidate.ed, candidates[i].ed, SC_PUBKEY_SIZE)) duplicate = 1;
            if (duplicate) continue;
            all[count].candidate = candidates[i];
            all[count].candidate.rtt = rtt;
            all[count++].sum_rtt = (uint32_t)rtt + candidates[i].rtt;
        }
    }
    for (uint8_t i = 0; i < count; i++) for (uint8_t j = i + 1; j < count; j++) {
        if (all[j].sum_rtt < all[i].sum_rtt || (all[j].sum_rtt == all[i].sum_rtt &&
            memcmp(all[j].candidate.ed, all[i].candidate.ed, SC_PUBKEY_SIZE) < 0)) {
            struct scored_candidate tmp = all[i]; all[i] = all[j]; all[j] = tmp;
        }
    }
    if (!count) {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: no common measured READY intermediary peer=%s; retry pending",
                    sc_public_key_hex(entry->ed, (char[65]){0}));
        return;
    }
    struct CM_INTERM_SEL* sel = &entry->selection; memset(sel, 0, sizeof(*sel));
    sel->cmd = ETCP_RT_ID_CONN_MGR; sel->subcmd = CM_SUBCMD_INTERM_SELECTED; sel->request_id = entry->exchange_request_id;
    sel->count = count > CONN_MGR_MAX_INTERMEDIARIES ? CONN_MGR_MAX_INTERMEDIARIES : count;
    for (uint8_t i = 0; i < sel->count; i++) {
        sel->selected[i] = all[i].candidate;
        char hex[65];
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: intermediary peer=%s via=%s sum_rtt=%u",
                    sc_public_key_hex(entry->ed, (char[65]){0}), sc_public_key_hex(all[i].candidate.ed, hex), all[i].sum_rtt);
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: INDIRECT selected peer=%s count=%u; verifying delivery",
               sc_public_key_hex(entry->ed, (char[65]){0}), sel->count);
    cm_send_control(entry->mgr, entry->ed, sel, sizeof(*sel));
}

void cm_handle_interm_exchange_resp(struct CONN_MGR* mgr, const uint8_t src[32], const uint8_t* data, size_t len) {
    if (len != sizeof(struct CM_EXCHANGE_RESP)) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: malformed EXCHANGE_RESP peer=%s len=%zu", sc_public_key_hex(src, (char[65]){0}), len);
        return;
    }
    struct CM_EXCHANGE_RESP resp; memcpy(&resp, data, sizeof(resp));
    struct CONN_MGR_ENTRY* entry = cm_find_entry(mgr, src);
    if (!entry || !entry->attempt_active || !entry->indirect_started || entry->indirect_confirmed || entry->selection.count ||
        entry->exchange_request_id != resp.request_id || get_time_tb() >= entry->deadline_tb) {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: stale EXCHANGE_RESP peer=%s request=%u", sc_public_key_hex(src, (char[65]){0}), resp.request_id);
        return;
    }
    if (resp.my_count > CONN_MGR_MAX_CANDIDATES || resp.your_count > CONN_MGR_MAX_CANDIDATES) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: invalid candidate counts peer=%s", sc_public_key_hex(src, (char[65]){0})); return;
    }
    cm_compute_intermediaries(entry, &resp);
}

void cm_handle_interm_selected(struct CONN_MGR* mgr, const uint8_t src[32], const uint8_t* data, size_t len) {
    if (len != sizeof(struct CM_INTERM_SEL)) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: malformed INTERM_SELECTED peer=%s len=%zu", sc_public_key_hex(src, (char[65]){0}), len);
        return;
    }
    struct CM_INTERM_SEL sel; memcpy(&sel, data, sizeof(sel));
    if (!sel.count || sel.count > CONN_MGR_MAX_INTERMEDIARIES) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: invalid intermediary count=%u peer=%s", sel.count, sc_public_key_hex(src, (char[65]){0})); return;
    }
    struct CONN_MGR_ENTRY* entry = cm_find_entry(mgr, src);
    if (!entry) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: selection without exchange peer=%s", sc_public_key_hex(src, (char[65]){0})); return; }
    if (entry->peer_exchange_request_id != sel.request_id) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: stale selection peer=%s request=%u", sc_public_key_hex(src, (char[65]){0}), sel.request_id);
        return;
    }
    if (get_time_tb() >= entry->peer_exchange_deadline_tb) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: selection after deadline peer=%s", sc_public_key_hex(src, (char[65]){0}));
        return;
    }
    struct CM_INTERM_SEL ack = {0}; ack.cmd = ETCP_RT_ID_CONN_MGR; ack.subcmd = CM_SUBCMD_INTERM_ACK; ack.request_id = sel.request_id;
    ack.count = cm_collect_candidates(mgr, src, sel.selected, sel.count, ack.selected);
    if (!ack.count) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: selected intermediaries unavailable peer=%s", sc_public_key_hex(src, (char[65]){0}));
        cm_send_control(mgr, src, &ack, sizeof(ack)); return;
    }
    for (uint8_t i = 0; i < ack.count; i++) memcpy(entry->intermediaries[i], ack.selected[i].ed, SC_PUBKEY_SIZE);
    entry->intermediariy_count = ack.count;
    // Публикуем путь до ACK, чтобы ответ прошёл через выбранного посредника.
    cm_finalize_indirect(entry);
    cm_send_control(mgr, src, &ack, sizeof(ack));
}

void cm_handle_interm_ack(struct CONN_MGR* mgr, const uint8_t src[32], const uint8_t* data, size_t len) {
    if (len != sizeof(struct CM_INTERM_SEL)) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: malformed INTERM_ACK peer=%s len=%zu", sc_public_key_hex(src, (char[65]){0}), len); return;
    }
    struct CM_INTERM_SEL ack; memcpy(&ack, data, sizeof(ack));
    struct CONN_MGR_ENTRY* entry = cm_find_entry(mgr, src);
    if (!entry || !entry->attempt_active || entry->exchange_request_id != ack.request_id ||
        !entry->selection.count || entry->indirect_confirmed ||
        get_time_tb() >= entry->deadline_tb) {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: stale INTERM_ACK peer=%s request=%u", sc_public_key_hex(src, (char[65]){0}), ack.request_id);
        return;
    }
    if (ack.count > CONN_MGR_MAX_INTERMEDIARIES) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: invalid ACK count=%u peer=%s", ack.count, sc_public_key_hex(src, (char[65]){0})); return;
    }
    struct CONN_MGR_CANDIDATE confirmed[CONN_MGR_MAX_INTERMEDIARIES];
    uint8_t count = cm_collect_candidates(mgr, src, ack.selected, ack.count, confirmed);
    uint8_t accepted = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (cm_has_candidate(entry->selection.selected, entry->selection.count, confirmed[i].ed))
            memcpy(entry->intermediaries[accepted++], confirmed[i].ed, SC_PUBKEY_SIZE);
    }
    if (!accepted) {
        memset(&entry->selection, 0, sizeof(entry->selection));
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: intermediary verification failed peer=%s; repeating exchange", sc_public_key_hex(src, (char[65]){0}));
        return;
    }
    entry->intermediariy_count = accepted;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: INDIRECT confirmed peer=%s count=%u elapsed=%llums",
               sc_public_key_hex(src, (char[65]){0}), entry->intermediariy_count, (unsigned long long)((get_time_tb() - entry->start_tb) / 10));
    cm_finalize_indirect(entry);
}

void cm_handle_interm_exchange_req(const uint8_t src[32], struct CONN_MGR* mgr, struct CM_EXCHANGE_REQ* req) {
    if (!mgr) return;
    if (!req->request_id || !src || !memcmp(src, mgr->instance->my_ed25519_pubkey, 32) || req->candidate_count > CONN_MGR_MAX_CANDIDATES ||
        !cm_peer_snapshot(mgr, src)) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: invalid EXCHANGE_REQ peer=%s count=%u", sc_public_key_hex(src, (char[65]){0}), req->candidate_count);
        return;
    }
    struct CONN_MGR_ENTRY* entry = cm_ensure_entry(mgr, src);
    if (!entry) return;
    uint64_t now = get_time_tb();
    if (entry->peer_exchange_request_id != req->request_id || !entry->peer_exchange_deadline_tb) {
        entry->peer_exchange_request_id = req->request_id;
        entry->peer_exchange_deadline_tb = now + CONN_MGR_INTERM_EXCHANGE_TIMEOUT_MS * 10;
        if (!entry->handles) entry->deadline_tb = entry->peer_exchange_deadline_tb;
    }
    if (now >= entry->peer_exchange_deadline_tb) {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: expired EXCHANGE_REQ peer=%s request=%u",
                    sc_public_key_hex(src, (char[65]){0}), req->request_id);
        return;
    }
    if (entry->state == CONN_MGR_STATE_DISCONNECTED) {
        entry->state = CONN_MGR_STATE_CONNECTING; entry->start_tb = now;
    }
    cm_arm_timer(entry);
    struct CM_EXCHANGE_RESP resp = {0};
    resp.cmd = ETCP_RT_ID_CONN_MGR; resp.subcmd = CM_SUBCMD_INTERM_EXCHANGE_RESP; resp.request_id = req->request_id;
    const uint8_t* target = src;
    resp.my_count = cm_collect_candidates(mgr, target, mgr->best_candidates, mgr->best_candidate_count, resp.my_candidates);
    resp.your_count = cm_collect_candidates(mgr, target, req->candidates, req->candidate_count, resp.your_candidates);
    cm_send_control(mgr, src, &resp, sizeof(resp));
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: EXCHANGE_RESP peer=%s request=%u my=%u your=%u",
                sc_public_key_hex(src, (char[65]){0}), req->request_id, resp.my_count, resp.your_count);
}
