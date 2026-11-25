/* Настоящие CHAT/BGP/ETCP/CM из существующего fixture; отдельный процесс и собственная state machine. */
#define main call_reference_main
#include "test_join_id.h"
#include "test_call.c"
#undef main
#include "test_clock.h"
#include "../src/radio/radio_call.h"

#define ADMISSION_FIRST UINT64_C(0x200000001)
#define ADMISSION_SECOND UINT64_C(0x200000002)
#define ADMISSION_THIRD UINT64_C(0x200000003)
#define ADMISSION_RADIO UINT64_C(0x300000001)

struct admission_events {
    unsigned count, ended, errors, invites, cm_open, radio_finished;
    uint64_t ended_id;
    uint8_t ended_reason, radio_reason;
};
struct admission_test {
    struct UASYNC *ua;
    struct UTUN_INSTANCE *inst[2];
    struct admission_events events[2];
    struct ETCP_CONN *transport;
    void *timer;
    char dm[2][64];
    unsigned phase, ticks, checks, refusals, duplicates, dropped_acks;
    uint64_t next_id;
    int hold_acks, result;
};
static struct admission_test *admission_current;
struct target_reply { int calls, count; uint8_t node[32]; };
static void target_ready(void* arg, int count, const uint8_t (*nodes)[32]) {
    struct target_reply* reply = arg; reply->calls++; reply->count = count; if (count == 1) memcpy(reply->node, nodes[0], 32);
}

#define ADMISSION_CHECK(t, expression) do { \
    (t)->checks++; \
    if (!(expression)) { \
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call admission assertion line=%d phase=%u: %s", __LINE__, (t)->phase, #expression); \
        return 0; \
    } \
} while (0)

static int admission_index(struct UTUN_INSTANCE *inst) {
    if (!admission_current) return -1;
    for (int i = 0; i < 2; i++) if (admission_current->inst[i] == inst) return i;
    return -1;
}

int __real_etcp_route_send_key(struct UTUN_INSTANCE *, uint64_t, const uint8_t[32], struct ll_entry *, int, int);
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE *inst, uint64_t group, const uint8_t peer[32], struct ll_entry *e, int force, int mode) {
    int index = admission_index(inst);
    if (index >= 0 && e && e->dgram && e->len >= 2 && e->dgram[0] == ETCP_RT_ID_CALL) {
        if (e->dgram[1] == CALL_SUBCMD_INVITE) admission_current->events[index].invites++;
        if (e->dgram[1] == CALL_SUBCMD_END_ACK && admission_current->hold_acks) {
            admission_current->dropped_acks++;
            queue_dgram_free(e); queue_entry_free(e); return 0;
        }
    }
    return __real_etcp_route_send_key(inst, group, peer, e, force, mode);
}

int __real_conn_mgr_open(struct UTUN_INSTANCE *, uint64_t, const uint8_t*, conn_mgr_cb_t, void *, struct CONN_MGR_HANDLE **);
int __wrap_conn_mgr_open(struct UTUN_INSTANCE *inst, uint64_t group, const uint8_t peer[32], conn_mgr_cb_t cb, void *arg,
                         struct CONN_MGR_HANDLE **out) {
    int index = admission_index(inst);
    if (index >= 0) admission_current->events[index].cm_open++;
    return __real_conn_mgr_open(inst, group, peer, cb, arg, out);
}

static void admission_event(struct UTUN_INSTANCE *inst, int type, const uint8_t *data, int length) {
    int index = admission_index(inst);
    if (index < 0) return;
    struct admission_events *ev = &admission_current->events[index];
    switch (type) {
    case CHAT_EVT_CALL_ENDED:
        if (length >= 9) { memcpy(&ev->ended_id, data, 8); ev->ended_reason = data[8]; ev->ended++; }
        /* fall through */
    case CHAT_EVT_CALL_INCOMING: case CHAT_EVT_CALL_RINGING: case CHAT_EVT_CALL_ACCEPTED:
    case CHAT_EVT_CALL_DECLINED: case CHAT_EVT_CALL_PHONE_BUSY: case CHAT_EVT_CALL_PATH:
    case CHAT_EVT_CALL_CONNECTION: case CHAT_EVT_CALL_STATS:
        ev->count++; break;
    case CHAT_EVT_CALL_ERROR: ev->count++; ev->errors++; break;
    case CHAT_EVT_RADIO_CALL:
        if (length == CHAT_RADIO_CALL_EVENT_SIZE && data[CHAT_RADIO_CALL_STATE] == RADIO_CALL_FINISHED) { ev->radio_finished++; ev->radio_reason = data[CHAT_RADIO_CALL_STATE + 1]; }
        break;
    default: break;
    }
    call_evt_handler(inst, type, data, length);
}

static unsigned admission_live(struct UTUN_INSTANCE *inst) {
    unsigned count = 0;
    for (struct ll_entry *e = inst->call->sessions->head; e; e = e->next)
        count += ((struct call_session *)e->data)->state != CALL_ST_ENDED;
    return count;
}

/* Рация требует проверенного членства: дополняем старый call fixture join/update/tree подписями. */
static int admission_members(struct admission_test *t) {
    uint64_t version = (uint64_t)ntp_time_get_seconds(t->inst[0]) + 1;
    for (int member = 0; member < 2; member++) {
        struct UTUN_INSTANCE *inst = t->inst[member];
        uint8_t body[MS_USERINFO_MAX + 112], join[64], tree[64], update[64];
        const char *userinfo = member ? "{\"name\":\"B\"}" : "{\"name\":\"A\"}";
        int size = member_sync_build_join_msg(g_sh.ch_x_pub, g_sh.ch_ed_pub, inst->my_ed25519_pubkey,
                                              inst->my_keys.public_key, body, sizeof(body));
        ADMISSION_CHECK(t, size > 0 && sc_ed25519_sign(inst->my_ed25519_privkey, body, (size_t)size, join) == SC_OK);
        ADMISSION_CHECK(t, member_sync_sign_join(g_sh.ch_ed_priv, CH_ID, NULL, inst->my_keys.public_key, inst->my_ed25519_pubkey,
            TEST_JOIN_ID(g_sh.ch_x_pub, g_sh.ch_ed_pub, inst->my_ed25519_pubkey, inst->my_keys.public_key), version, tree) == 0);
        size = member_sync_build_update_msg(TEST_JOIN_ID(g_sh.ch_x_pub, g_sh.ch_ed_pub, inst->my_ed25519_pubkey, inst->my_keys.public_key), version + 1, userinfo, body, sizeof(body));
        ADMISSION_CHECK(t, size > 0 && sc_ed25519_sign(inst->my_ed25519_privkey, body, (size_t)size, update) == SC_OK);
        for (int destination = 0; destination < 2; destination++)
            ADMISSION_CHECK(t, member_sync_put(t->inst[destination], CH_ID, inst->my_keys.public_key,
                inst->my_ed25519_pubkey, join, version, update, version + 1, userinfo, NULL, NULL, 0, 0, tree) >= 0);
    }
    return 1;
}

/* Отказ не меняет существующий звонок, CM, таймеры, очередь и счётчики. */
static int admission_attempt(struct admission_test *t, int index, uint64_t original, const char *ch, const uint8_t peer[32],
                              uint64_t requested, int busy_event) {
    struct UTUN_INSTANCE *inst = t->inst[index];
    struct call_session *s = call_session_find(inst->call, original);
    struct call_session saved = {0};
    if (s) saved = *s;
    struct admission_events before = t->events[index];
    uint32_t started = inst->call->c_calls_started;
    int records = queue_entry_count(inst->call->sessions);
    unsigned live = admission_live(inst);
    call_start(inst, ch, peer, requested);
    struct admission_events *after = &t->events[index];
    ADMISSION_CHECK(t, after->cm_open == before.cm_open && after->invites == before.invites);
    ADMISSION_CHECK(t, inst->call->c_calls_started == started && queue_entry_count(inst->call->sessions) == records);
    ADMISSION_CHECK(t, admission_live(inst) == live && after->errors == before.errors);
    if (s) {
        ADMISSION_CHECK(t, call_session_find(inst->call, original) == s);
        ADMISSION_CHECK(t, s->state == saved.state && s->role == saved.role && s->reason == saved.reason);
        ADMISSION_CHECK(t, s->group_id == saved.group_id && s->peer_node_id == saved.peer_node_id && s->force == saved.force);
        ADMISSION_CHECK(t, s->cm_handle == saved.cm_handle && s->tx_q == saved.tx_q && s->started_tb == saved.started_tb);
        ADMISSION_CHECK(t, s->ring_timer == saved.ring_timer && s->invite_timer == saved.invite_timer);
        ADMISSION_CHECK(t, s->traffic_timer == saved.traffic_timer && s->teardown_timer == saved.teardown_timer);
        ADMISSION_CHECK(t, s->stats_timer == saved.stats_timer && s->tx_media_seq == saved.tx_media_seq);
    }
    if (busy_event) {
        ADMISSION_CHECK(t, !call_session_find(inst->call, requested));
        ADMISSION_CHECK(t, after->count == before.count + 1 && after->ended == before.ended + 1);
        ADMISSION_CHECK(t, after->ended_id == requested && after->ended_reason == CALL_REASON_BUSY);
        t->refusals++;
    } else {
        ADMISSION_CHECK(t, after->count == before.count && after->ended == before.ended);
        t->duplicates++;
    }
    return 1;
}

static int admission_busy_cases(struct admission_test *t, int index, uint64_t original) {
    struct UTUN_INSTANCE *inst = t->inst[index];
    const uint8_t* peer = t->inst[1 - index]->my_ed25519_pubkey;
    uint8_t other[32]; memcpy(other, peer, sizeof(other)); other[0] ^= 1;
    ADMISSION_CHECK(t, call_busy(inst) && admission_live(inst) == 1);
    ADMISSION_CHECK(t, admission_attempt(t, index, original, CH_ID, peer, original, 0));
    /* Даже другой tuple с тем же ID не должен завершать/дублировать текущую запись. */
    ADMISSION_CHECK(t, admission_attempt(t, index, original, "4242424242424243", other, original, 0));
    ADMISSION_CHECK(t, admission_attempt(t, index, original, CH_ID, peer, ++t->next_id, 1));
    ADMISSION_CHECK(t, admission_attempt(t, index, original, t->dm[index], peer, ++t->next_id, 1));
    /* Занятость instance проверяется до поиска другой группы/доступности другого пира. */
    ADMISSION_CHECK(t, admission_attempt(t, index, original, "4242424242424243", other, ++t->next_id, 1));
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call admission protected node=%d id=%llx state=%u role=%u live=1 no_new_CM_INVITE=1",
               index, (unsigned long long)original, call_session_find(inst->call, original)->state,
               call_session_find(inst->call, original)->role);
    return 1;
}

/* 1 — продолжить, 2 — завершить; 0 — ошибка. Состояния меняются только штатным сигналингом. */
static int admission_step(struct admission_test *t) {
    struct UTUN_INSTANCE *a = t->inst[0], *b = t->inst[1];
    struct call_session *sa = call_session_find(a->call, t->phase < 6 ? ADMISSION_FIRST : ADMISSION_SECOND);
    struct call_session *sb = call_session_find(b->call, t->phase < 6 ? ADMISSION_FIRST : ADMISSION_SECOND);
    switch (t->phase) {
    case 0:
        if (!conn_up(a, b->node_id) || !conn_up(b, a->node_id)) return 1;
        do_setup(a, IDX_A); do_setup(b, IDX_B);
        ADMISSION_CHECK(t, admission_members(t));
        t->phase++; break;
    case 1:
        if (!bgp_has(a, b->node_id) || !bgp_has(b, a->node_id)) return 1;
        {
            sqlite3* legacy = a->topo_sqlite_db; struct target_reply reply = {0};
            a->topo_sqlite_db = NULL;
            call_targets_read(a, topo_groups_channel_slot(a->topo_groups, CH_ID), b->my_ed25519_pubkey, 0, 1, target_ready, &reply);
            a->topo_sqlite_db = legacy;
            ADMISSION_CHECK(t, reply.calls == 1 && reply.count == 1 && !memcmp(reply.node, b->my_ed25519_pubkey, 32));
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "call target resolved from committed keys and live route without SQLite handle");
        }
        for (int i = 0; i < 2; i++) {
            uint64_t peer = t->inst[1 - i]->node_id;
            ADMISSION_CHECK(t, dm_start(t->inst[i], peer, g_sh.x_pub[1 - i], g_sh.ed_pub[1 - i],
                "peer", CH_ID, DM_MODE_DEVICE, NULL) == 0);
            snprintf(t->dm[i], sizeof(t->dm[i]), "dm:%llu", (unsigned long long)dm_derive_conv_id(t->inst[i]->my_ed25519_pubkey, g_sh.ed_pub[1 - i]));
        }
        t->transport = instance_find_conn(a, b->node_id);
        ADMISSION_CHECK(t, admission_attempt(t, 0, 0, CH_ID, b->my_ed25519_pubkey, 0, 0));
        ADMISSION_CHECK(t, admission_attempt(t, 0, 0, CH_ID, NULL, ++t->next_id, 0));
        call_start(a, CH_ID, b->my_ed25519_pubkey, ADMISSION_FIRST);
        sa = call_session_find(a->call, ADMISSION_FIRST);
        ADMISSION_CHECK(t, sa && sa->state == CALL_ST_OUTGOING);
        ADMISSION_CHECK(t, admission_busy_cases(t, 0, ADMISSION_FIRST));
        t->phase++; break;
    case 2:
        if (!sa || !sb || sa->state != CALL_ST_RINGING || sb->state != CALL_ST_RINGING) return 1;
        ADMISSION_CHECK(t, admission_busy_cases(t, 0, ADMISSION_FIRST));
        ADMISSION_CHECK(t, admission_busy_cases(t, 1, ADMISSION_FIRST));
        call_accept(b, ADMISSION_FIRST); t->phase++; break;
    case 3:
        if (!sa || !sb || sa->state != CALL_ST_ACTIVE || sb->state != CALL_ST_ACTIVE) return 1;
        ADMISSION_CHECK(t, sa->cm_handle && sb->cm_handle);
        ADMISSION_CHECK(t, admission_busy_cases(t, 0, ADMISSION_FIRST));
        ADMISSION_CHECK(t, admission_busy_cases(t, 1, ADMISSION_FIRST));
        {
            unsigned before = t->events[0].radio_finished;
            struct radio_call_request req = {.inst = a, .id = ADMISSION_RADIO, .action = RADIO_CALL_START};
            memcpy(req.group, g_sh.ch_ed_pub, 32);
            memcpy(req.peer, b->my_ed25519_pubkey, sizeof(req.peer));
            radio_call_execute(&req);
            ADMISSION_CHECK(t, !radio_call_busy(a) && call_busy(a));
            ADMISSION_CHECK(t, t->events[0].radio_finished == before + 1 && t->events[0].radio_reason == RADIO_CALL_BUSY);
        }
        {
            const uint8_t opus[] = {0xf8, 0xff, 0xfe};
            for (unsigned i = 0; i < 4; ++i) {
                struct call_capture_time timing = sent_timing; timing.pts_us += i * CALL_OPUS_FRAME_MS * 1000;
                ADMISSION_CHECK(t, call_send_media(a, ADMISSION_FIRST, opus, sizeof(opus), &timing) == 0);
                ADMISSION_CHECK(t, call_send_media(b, ADMISSION_FIRST, opus, sizeof(opus), &timing) == 0);
            }
        }
        t->phase++; break;
    case 4:
        if (g_media[0].frames < 4 || g_media[1].frames < 4) return 1;
        ADMISSION_CHECK(t, g_media[0].frames == 4 && g_media[1].frames == 4);
        ADMISSION_CHECK(t, g_media[0].last_call_id == ADMISSION_FIRST && g_media[1].last_call_id == ADMISSION_FIRST);
        ADMISSION_CHECK(t, g_media[0].last_len == 3 && !memcmp(g_media[0].last, g_media[1].last, 3));
        ADMISSION_CHECK(t, instance_find_conn(a, b->node_id) == t->transport && conn_up(a, b->node_id));
        t->hold_acks = 1; call_hangup(a, ADMISSION_FIRST); t->phase++; break;
    case 5:
        if (!sa || !sb || sa->state != CALL_ST_ENDED || sb->state != CALL_ST_ENDED) return 1;
        ADMISSION_CHECK(t, t->dropped_acks && !call_busy(a) && !call_busy(b));
        ADMISSION_CHECK(t, admission_attempt(t, 0, ADMISSION_FIRST, CH_ID, b->my_ed25519_pubkey, ADMISSION_FIRST, 0));
        ADMISSION_CHECK(t, admission_attempt(t, 1, ADMISSION_FIRST, t->dm[1], a->my_ed25519_pubkey, ADMISSION_FIRST, 0));
        call_set_phone_state(b, 2, CALL_POLICY_WARN_PHONE);
        call_start(a, t->dm[0], b->my_ed25519_pubkey, ADMISSION_SECOND);
        ADMISSION_CHECK(t, call_session_find(a->call, ADMISSION_FIRST) == sa && sa->state == CALL_ST_ENDED);
        sa = call_session_find(a->call, ADMISSION_SECOND);
        ADMISSION_CHECK(t, sa && sa->state == CALL_ST_OUTGOING && admission_live(a) == 1);
        t->phase++; break;
    case 6:
        if (!sa || !sb || sa->state != CALL_ST_WAIT_DECISION || sb->state != CALL_ST_WAIT_DECISION) return 1;
        ADMISSION_CHECK(t, admission_busy_cases(t, 0, ADMISSION_SECOND));
        ADMISSION_CHECK(t, admission_busy_cases(t, 1, ADMISSION_SECOND));
        call_continue(a, ADMISSION_SECOND); t->phase++; break;
    case 7:
        if (!sa || !sb || sa->state != CALL_ST_RINGING || sb->state != CALL_ST_RINGING) return 1;
        ADMISSION_CHECK(t, sa->force && sb->force);
        call_hangup(a, ADMISSION_SECOND); t->hold_acks = 0; t->phase++; break;
    case 8:
        if (queue_entry_count(a->call->sessions) || queue_entry_count(b->call->sessions)) return 1;
        call_set_phone_state(b, 0, CALL_POLICY_PUBLISH_PHONE);
        {
            struct radio_call_request req = {.inst = a, .id = ADMISSION_RADIO + 1, .action = RADIO_CALL_START};
            memcpy(req.group, g_sh.ch_ed_pub, 32);
            memcpy(req.peer, b->my_ed25519_pubkey, sizeof(req.peer));
            radio_call_execute(&req);
            ADMISSION_CHECK(t, radio_call_busy(a) && !call_busy(a));
            ADMISSION_CHECK(t, admission_attempt(t, 0, 0, CH_ID, b->my_ed25519_pubkey, ++t->next_id, 1));
            req.action = RADIO_CALL_CANCEL; radio_call_execute(&req);
        }
        t->phase++; break;
    case 9:
        if (radio_call_busy(a) || radio_call_busy(b)) return 1;
        call_start(a, CH_ID, b->my_ed25519_pubkey, ADMISSION_THIRD); t->phase++; break;
    case 10:
        sa = call_session_find(a->call, ADMISSION_THIRD); sb = call_session_find(b->call, ADMISSION_THIRD);
        if (!sa || !sb || sa->state != CALL_ST_RINGING || sb->state != CALL_ST_RINGING) return 1;
        call_decline(b, ADMISSION_THIRD); t->phase++; break;
    case 11:
        if (queue_entry_count(a->call->sessions) || queue_entry_count(b->call->sessions)) return 1;
        ADMISSION_CHECK(t, !call_busy(a) && !call_busy(b));
        ADMISSION_CHECK(t, instance_find_conn(a, b->node_id) == t->transport && conn_up(a, b->node_id));
        ADMISSION_CHECK(t, t->refusals == 22 && t->duplicates == 18);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call admission PASS checks=%u BUSY=%u no_event=%u original_audio_RX=%d/%d END_ACK_drops=%u",
                   t->checks, t->refusals, t->duplicates, g_media[0].frames, g_media[1].frames, t->dropped_acks);
        return 2;
    }
    return 1;
}

static void admission_tick(void *arg) {
    struct admission_test *t = arg; t->timer = NULL;
    int result = admission_step(t);
    if (++t->ticks > 3000) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call admission timeout phase=%u", t->phase); result = 0;
    }
    if (result == 1) {
        t->timer = uasync_set_timeout(t->ua, TICK_TB, t, admission_tick, "call_admission");
        if (t->timer) return;
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call admission timer allocation failed"); result = 0;
    }
    t->result = result == 2 ? 0 : 1; uasync_stop(t->ua);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_DEBUG);
    utun_instance_set_tun_init_enabled(0); test_clock_init(TICK_TB); test_clock_sync_wall_time();
    size_t allocated = u_get_allocated_count();
    char dir[512] = "/tmp/utun_call_admission_XXXXXX";
    if (test_mkdtemp(dir) || test_setenv("UTUN_TEST_DIR", dir, 1)) return 1;
    srand(1); fill_shared(); write_configs(dir);
    struct admission_test t = {.next_id = UINT64_C(0x400000000), .result = 1};
    admission_current = &t;
    t.ua = uasync_create();
    for (int i = 0; t.ua && i < 2; i++) {
        char path[600]; snprintf(path, sizeof(path), "%s/%s.conf", dir, i ? "b" : "a");
        t.inst[i] = utun_instance_create(t.ua, path);
    }
    if (t.inst[0] && t.inst[1]) {
        test_etcp_set_peer(t.inst[1], "to_a", t.inst[0], "s1");
        for (int i = 0; i < 2; i++) {
            utun_instance_init(t.inst[i]); t.inst[i]->test_user_ptr = &g_ev[i];
            chat_event_set_handler(t.inst[i], admission_event);
            call_set_media_recv_cb(t.inst[i], media_recv_cb, &g_media[i]);
        }
        t.timer = uasync_set_timeout(t.ua, TICK_TB, &t, admission_tick, "call_admission");
        if (t.timer) test_clock_mainloop(t.ua);
    }
    if (t.timer) uasync_cancel_timeout(t.ua, t.timer);
    for (int i = 0; i < 2; i++) if (t.inst[i]) utun_instance_destroy(t.inst[i]);
    if (t.ua) { uasync_poll(t.ua, 0); uasync_destroy(t.ua, 0); }
    admission_current = NULL; test_clock_finish();
    char command[600]; snprintf(command, sizeof(command), "rm -rf %s", dir); (void)!system(command);
    size_t remaining = u_get_allocated_count();
    if (remaining != allocated) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call admission leaked allocations before=%zu after=%zu", allocated, remaining); t.result = 1;
    }
    printf("call admission: %s, checks=%u BUSY=%u no_event=%u allocations=%zu->%zu\n",
           t.result ? "FAIL" : "PASS", t.checks, t.refusals, t.duplicates, allocated, remaining);
    return t.result;
}
