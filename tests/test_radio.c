#include "../src/chat/chat_devgroup.h"
#include "../src/call/call_priv.h"
#include "../src/call/call_proto.h"
// test_radio.c — интеграционный тест рации (PTT мультикаст голоса).
//
// Однопоточный: 3 узла (A, B, C) в одном процессе, один UASYNC, без fork.
// Топология — цепочка A←B←C (A сервер, B клиент к A + сервер, C клиент к B).
// Все три в одной CHAT-группе. Master state-machine на таймере.
//
// Сценарий:
//   1. A,B,C radio_on → флаг TOPO_FLAG_RADIO распространяется по BGP (node.radio==1).
//   2. A: radio_talk_begin + 5 кадров + radio_talk_end → B и C (через relay B) получают
//      кадры в порядке seq 0..4 и FIN.
//   3. C radio_off (обрезание) → A снова говорит → B получает, C не получает.
//   4. Без FIN: C снимает индикацию при radio_off, B — по таймауту источника.
//   До передачи: вызовы через B, deadline 2с/60с, потери RESULT/ACK, отказ/отмена/занятость,
//   отсутствие живого BGP-пути, некорректный срок INVITE и отсутствие таймеров рации в покое.

#include "chat_core.h"
#include "chat_sync.h"
#include "chat_event.h"
#include "chat_member.h"
#include "member_sync.h"
#include "../radio/radio.h"
#include "../radio/radio_call.h"
#include "../call/call.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/secure_channel.h"
#include "../ntp_time.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/db_async.h"
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_clock.h"
#include "test_net.h"
#include "test_membership.h"

#include <sqlite3.h>
#include <assert.h>
#define OPENSSL_API_COMPAT 0x10100000L
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

static char CH_ID[65];

#define IDX_A 0
#define IDX_B 1
#define IDX_C 2

#define TICK_TB     100      /* 10 ms на такт */
#define MAX_TICKS   24000    /* включает минутные deadline звонка и выбора продолжения */
#define N_FRAMES    5

enum rad_phase {
    P_WAIT_CONN,
    P_SETUP,
    P_WAIT_BGP,
    P_RADIO_CALLS,
    P_RADIO_ON,
    P_WAIT_FLAGS,
    P_TALK1,
    P_WAIT_MEMBER_LISTS,
    P_WAIT_RX1,
    P_WAIT_MUTED_RELAY,
    P_RADIO_OFF_C,
    P_WAIT_OFF_C,
    P_TALK2,
    P_WAIT_RX2,
    P_REENABLE_C,
    P_WAIT_REENABLE_C,
    P_TALK_STALLED,
    P_WAIT_STALLED_START,
    P_WAIT_STALLED_STOP,
    P_WAIT_RESTORED_C,
    P_WITHDRAW_C,
    P_WAIT_WITHDRAW_EVENT,
    P_DONE,
};

struct rad_ctx {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst[3];
    enum rad_phase phase;
    int   result;        /* 0=running, 1=ok, 2=fail */
    int   ticks;
    void* timer;
    int call_step;
    uint64_t call_started;
    struct db_async_tx* start_gate; /* Только fixture: задержать первый READ настроек инициатора до FINISHED. */
    int gate_ready, gate_done, gate_released;
};

/* Задержка через handle оставляет uasync и сеть работающими, не зависит от скорости диска/потоков. */
static void start_gate_ready(void* arg, const struct db_async_result* result) {
    struct rad_ctx* t = arg; assert(!result->status); t->gate_ready++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "radio fixture: caller DB held until accepted invitation completes");
}
static void start_gate_done(void* arg, const struct db_async_result* result) {
    struct rad_ctx* t = arg; t->start_gate = NULL; t->gate_done++;
    if (result->status && !t->result) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio fixture: DB gate failed status=%d sqlite=%d: %s",
            result->status, result->sqlite_code, result->message);
        t->result = 2; uasync_stop(t->ua);
    }
}

/* приёмник кадров (per-узел): проверка порядка seq и FIN */
struct rx_evt {
    int      active;
    uint16_t stream;
    uint16_t expect_seq;
    int      frames;       /* число data-кадров (fin=0) текущего потока */
    int      fin_seen;
    int      seq_ok;
};

struct rad_shared {
    uint8_t  ch_x_pub[32], ch_ed_pub[32], ch_ed_priv[32], ch_sig[64];
    uint64_t nid[3];
    uint8_t  x_pub[3][32], seed[3][32], ed_pub[3][32], ed_private[3][32];
};
static struct rad_shared g_sh;
static struct rx_evt     g_rx[3];
static int g_member_radio[3][3];
static int g_member_lists[3];
static int g_member_event_bad;
static int g_talk_on[3], g_talk_starts[3], g_talk_stops[3];
static struct { int state, reason, finished, incoming; uint64_t finished_tb; } g_calls[20][3];
static int g_call_ready, g_drop_result, g_drop_ack, g_drop_force;
static int device_targets_pending, device_targets_count = -1;
static void device_targets_ready(void* arg, int count, const uint8_t (*nodes)[32]) {
    (void)arg; (void)nodes; device_targets_pending = 0; device_targets_count = count;
}
static int g_member_phone[3][3];
static etcp_recv_fn g_call_recv[3];

/* Потеря приложения после router ACK: восстановить результат обязан сервис, не транспорт. */
static void call_packet_loss(struct ETCP_CONN* conn, struct ll_entry* entry) {
    int idx = -1;
    for (int i = 0; i < 3; ++i) if (conn->instance->node_id == g_sh.nid[i]) idx = i;
    if (idx < 0 || !entry) abort();
    if (entry->len <= ROUTER_SVC_PAYLOAD_OFF) { g_call_recv[idx](conn, entry); return; }
    int cmd = entry->dgram[ROUTER_SVC_PAYLOAD_OFF];
    if ((idx == IDX_A && cmd == 3 && g_drop_result-- > 0) || (idx == IDX_C && cmd == 4 && g_drop_ack-- > 0) ||
        (idx == IDX_C && cmd == 6 && g_drop_force-- > 0)) {
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "test drops radio signaling dst=%d command=%d", idx, cmd);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    g_call_recv[idx](conn, entry);
}

static void (*device_signal_recv[3])(struct ETCP_CONN*, struct ll_entry*);
static int device_drop_select = 1, device_drop_selected = 1;
static void device_signal_loss(struct ETCP_CONN* conn, struct ll_entry* entry) {
    int idx = -1;
    for (int i = 0; i < 3; ++i) if (conn->instance->node_id == g_sh.nid[i]) idx = i;
    if (idx < 0) abort();
    if (entry->len >= ROUTER_SVC_PAYLOAD_OFF + 9) {
        const uint8_t* p = entry->dgram + ROUTER_SVC_PAYLOAD_OFF;
        uint64_t id; memcpy(&id, p + 1, 8);
        if (id == 20001 && ((p[0] == CALL_SUBCMD_SELECT && device_drop_select-- > 0) ||
            (p[0] == CALL_SUBCMD_SELECTED && device_drop_selected-- > 0))) {
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "test drops selection id=%llx command=%u", (unsigned long long)id, p[0]);
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
    }
    device_signal_recv[idx](conn, entry);
}

static void radio_action(struct UTUN_INSTANCE* inst, uint64_t id, int action, const uint8_t peer[32]) {
    struct radio_call_request req = {.inst = inst, .id = id, .action = action};
    if (action == RADIO_CALL_START) memcpy(req.group, g_sh.ch_ed_pub, sizeof(req.group));
    if (peer) memcpy(req.peer, peer, sizeof(req.peer));
    radio_call_execute(&req);
}

static int active_timer_count(struct UASYNC* ua) {
    int count = 0;
    for (size_t i = 0; i < ua->timeout_heap->size; ++i) {
        TimeoutEntry* entry = &ua->timeout_heap->heap[i];
        if (!entry->deleted) ++count;
    }
    return count;
}

/* Публичный lifecycle: idle destroy/init не должен удалять или создавать ни одного таймера. */
static int check_radio_idle_timers(struct rad_ctx* t) {
    int before = active_timer_count(t->ua);
    for (int i = 0; i < 3; ++i) radio_call_destroy(t->inst[i]);
    int stopped = active_timer_count(t->ua);
    for (int i = 0; i < 3; ++i) if (radio_call_init(t->inst[i])) return 0;
    int restarted = active_timer_count(t->ua);
    if (before == stopped && stopped == restarted) return 1;
    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test idle radio timers before=%d stopped=%d restarted=%d", before, stopped, restarted);
    return 0;
}

/* Проверяем именно UI-события: self, прямой пир и пир за посредником. */
static void member_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    if (type == CHAT_EVT_RADIO_CALL) {
        uint64_t id;
        if (!data || len != CHAT_RADIO_CALL_EVENT_SIZE) { g_member_event_bad = 1; return; }
        memcpy(&id, data, 8);
        struct TOPO_GROUP* route = topo_groups_find_wire(inst->topo_groups, data + 8);
        uint64_t group = route ? route->group_id : 0;
        int idx = -1;
        for (int i = 0; i < 3; ++i) if (inst->node_id == g_sh.nid[i]) idx = i;
        if (idx < 0 || id < 100 || id > 119 || !group || memcmp(data + 8, g_sh.ch_ed_pub, 32)) { g_member_event_bad = 1; return; }
        g_calls[id - 100][idx].state = data[CHAT_RADIO_CALL_STATE]; g_calls[id - 100][idx].reason = data[CHAT_RADIO_CALL_STATE + 1];
        if (data[CHAT_RADIO_CALL_STATE] == RADIO_CALL_INCOMING) {
            ++g_calls[id - 100][idx].incoming;
            if (g_call_ready) radio_action(inst, id, RADIO_CALL_READY, 0);
        }
        if (data[CHAT_RADIO_CALL_STATE] == RADIO_CALL_SELECTED && id != 118) {
            radio_set_active(inst, group, 1);
            radio_action(inst, id, RADIO_CALL_ACCEPT, 0);
        }
        if (data[CHAT_RADIO_CALL_STATE] == RADIO_CALL_FINISHED) {
            ++g_calls[id - 100][idx].finished; g_calls[id - 100][idx].finished_tb = get_time_tb();
            if (data[CHAT_RADIO_CALL_STATE + 1] == RADIO_CALL_ACCEPTED) radio_set_active(inst, group, 1);
        }
        return;
    }
    if (type == CHAT_EVT_RADIO_TALK) {
        if (!data || len != 67 || data[66] > 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: invalid talk event bytes=%d", len); g_member_event_bad = 1; return;
        }
        const uint8_t* source = data + 32;
        int dst = -1;
        for (int i = 0; i < 3; ++i) if (inst->node_id == g_sh.nid[i]) dst = i;
        if (dst < 0 || memcmp(data, g_sh.ch_ed_pub, 32) || memcmp(source, g_sh.ed_pub[IDX_A], 32)) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: unexpected talk dst=%d group=%s source=%s",
                        dst, sc_public_key_hex(data, (char[65]){0}), sc_public_key_hex(source, (char[65]){0}));
            g_member_event_bad = 1; return;
        }
        g_talk_on[dst] = data[66];
        if (data[66]) ++g_talk_starts[dst];
        else ++g_talk_stops[dst];
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "test: talk event dst=%d on=%d starts=%d stops=%d",
                    dst, data[66], g_talk_starts[dst], g_talk_stops[dst]);
        return;
    }
    if (type != CHAT_EVT_MEMBER_UPDATED && type != CHAT_EVT_MEMBER_LIST) return;
    if (!data || len < 3) { g_member_event_bad = 1; return; }
    int cl = data[0];
    if (cl != (int)strlen(CH_ID) || len < cl + 3 || memcmp(data + 1, CH_ID, cl)) return;
    int count = data[cl + 1];
    int off = cl + 2;
    if (type == CHAT_EVT_MEMBER_LIST) { count |= data[cl + 2] << 8; off++; }
    if (len != off + count * CHAT_MEMBER_DISPLAY_SIZE) { g_member_event_bad = 1; return; }
    int dst = -1;
    for (int i = 0; i < 3; i++) if (inst->node_id == g_sh.nid[i]) dst = i;
    if (dst < 0) { g_member_event_bad = 1; return; }
    if (type == CHAT_EVT_MEMBER_LIST) g_member_lists[dst]++;
    for (int i = 0; i < count; i++) {
        struct chat_member_display member;
        if (chat_core_deserialize_member(data + off + i * CHAT_MEMBER_DISPLAY_SIZE, &member) < 0) {
            g_member_event_bad = 1; return;
        }
        for (int j = 0; j < 3; j++) if (!memcmp(member.ed25519, g_sh.ed_pub[j], 32)) {
            g_member_radio[dst][j] = !!(member.flags & CHAT_MEMBER_FLAG_RADIO);
            g_member_phone[dst][j] = member.flags & (CHAT_MEMBER_FLAG_PHONE_KNOWN | CHAT_MEMBER_FLAG_PHONE_BUSY);
        }
    }
}

/* Полный список, одиночная строка и текущие события должны совпадать. */
static int check_member_radios(struct UTUN_INSTANCE* inst, int dst, int c_radio) {
    uint8_t* wire = NULL; int count = 0;
    if (chat_core_get_member_list(inst, CH_ID, &wire, &count) < 0 || count != 3 || g_member_event_bad) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: invalid member list dst=%d count=%d event_bad=%d", dst, count, g_member_event_bad);
        u_free(wire); return 0;
    }
    int seen = 0;
    for (int i = 0; i < count; i++) {
        struct chat_member_display member;
        chat_core_deserialize_member(wire + i * CHAT_MEMBER_DISPLAY_SIZE, &member);
        for (int j = 0; j < 3; j++) {
            if (memcmp(member.ed25519, g_sh.ed_pub[j], 32)) continue;
            uint8_t single[CHAT_MEMBER_DISPLAY_SIZE];
            int expected = j == IDX_C ? c_radio : 1;
            int radio = !!(member.flags & CHAT_MEMBER_FLAG_RADIO);
            if (radio != expected || g_member_radio[dst][j] != expected
                || chat_core_get_single_member(inst, CH_ID, member.ed25519, single) < 0
                || !!(single[CHAT_MEMBER_FLAGS_OFF] & CHAT_MEMBER_FLAG_RADIO) != expected
                || memcmp(single, wire + i * CHAT_MEMBER_DISPLAY_SIZE, CHAT_MEMBER_DISPLAY_SIZE)) {
                DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: member radio dst=%d src=%d list=%d event=%d expected=%d",
                            dst, j, radio, g_member_radio[dst][j], expected);
                u_free(wire); return 0;
            }
            seen |= 1 << j;
        }
    }
    u_free(wire);
    return seen == 7;
}

static int member_radio_events_ready(int c_radio) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        if (g_member_radio[i][j] != (j == IDX_C ? c_radio : 1)) return 0;
    return 1;
}

static int G_PASSED = 0, G_FAILED = 0, G_TOTAL = 0;
#define TEST(n)  do { G_TOTAL++; printf("  %-58s", n); fflush(stdout); } while(0)
#define OK()     do { G_PASSED++; printf("OK\n"); } while(0)
#define FAIL(f,...) do { G_FAILED++; printf("FAIL: " f "\n", ##__VA_ARGS__); } while(0)

/* ── крипто-хелперы ── */

static void to_hex(const uint8_t* bin, size_t n, char* out) {
    for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", bin[i]);
    out[n * 2] = '\0';
}

static int wf(const char* p, const char* f, ...) {
    va_list ap; FILE* fp = fopen(p, "w"); if (!fp) return -1;
    va_start(ap, f); vfprintf(fp, f, ap); va_end(ap); fclose(fp); return 0;
}

/* ── данные сценария + конфиги (цепочка A←B←C) ── */

static void fill_shared(void) {
    memset(&g_sh, 0, sizeof(g_sh));
    for (int i = 0; i < 3; i++) {
        uint8_t transport_secret[32];
        if (sc_identity_generate(g_sh.x_pub[i], transport_secret, g_sh.ed_pub[i], g_sh.seed[i])) abort();
        g_sh.nid[i] = sc_derive_node_id_from_pubkey(g_sh.x_pub[i]);
    }
    uint8_t x_private[32];
    if (chat_channel_generate_keys(g_sh.ch_x_pub, x_private, g_sh.ch_ed_pub, g_sh.ch_ed_priv)) abort();
    sc_public_key_hex(g_sh.ch_ed_pub, CH_ID);
    if (test_group_signature(CH_ID, "radio-test", g_sh.ed_pub[IDX_A], g_sh.ch_x_pub, g_sh.ch_ed_pub, g_sh.ch_ed_priv, g_sh.ch_sig)) abort();
}

static void write_configs(const char* dir) {
    for (int i = 0; i < 3; i++) {
        char priv[65], pub[65], path[512], dbd[512];
        to_hex(g_sh.seed[i], 32, priv);
        to_hex(g_sh.x_pub[i], 32, pub);
        snprintf(path, sizeof(path), "%s/%s.conf", dir, i == IDX_A ? "a" : i == IDX_B ? "b" : "c");
        snprintf(dbd, sizeof(dbd), "%s/db%s", dir, i == IDX_A ? "a" : i == IDX_B ? "b" : "c");
        utun_mkdir(dbd, 0755);

        char client[512] = "";
        if (i == IDX_B) {   /* B → A */
            char apub[65]; to_hex(g_sh.x_pub[IDX_A], 32, apub);
            snprintf(client, sizeof(client), "[client: to_a]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", apub);
        } else if (i == IDX_C) {   /* C → B */
            char bpub[65]; to_hex(g_sh.x_pub[IDX_B], 32, bpub);
            snprintf(client, sizeof(client), "[client: to_b]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", bpub);
        }

        wf(path,
            "[global]\ntun_ip=10.98.%d.1/24\ntun_ifname=tun%d0\ndb_path=%s/db%s\n"
            "my_private_key=%s\nmy_public_key=%s\n"
            "[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
            "%s"
            "[chatserver]\nstorage_autoload=0\n[allowed_keys]\nallow_all=1\n",
            i, i, dir, i == IDX_A ? "a" : i == IDX_B ? "b" : "c",
            priv, pub, client);
    }
}

/* ── проверки ── */

static int conn_up(struct UTUN_INSTANCE* inst, uint64_t nid) {
    struct ETCP_CONN* c = instance_find_conn(inst, nid);
    return c && c->initialized && c->links_up;
}

static int bgp_has(struct UTUN_INSTANCE* inst, uint64_t nid) {
    uint64_t gid = topo_groups_channel_slot(inst->topo_groups, CH_ID);
    struct TOPO_GROUP* g = topo_groups_find(inst->topo_groups, gid);
    if (!g) return 0;
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(g, nid);
    return nq && nq->paths && nq->paths->head;
}

static int node_radio(struct UTUN_INSTANCE* inst, uint64_t nid) {
    uint64_t gid = topo_groups_channel_slot(inst->topo_groups, CH_ID);
    struct TOPO_GROUP* g = topo_groups_find(inst->topo_groups, gid);
    if (!g) return 0;
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(g, nid);
    return nq ? nq->radio : 0;
}

/* ── frame cb (per-узел) ── */

static void rx_frame_cb(struct UTUN_INSTANCE* inst, uint64_t group_id,
                        const uint8_t ed[32], uint16_t stream, uint16_t seq,
                        uint8_t fin, const uint8_t* opus, int len, void* arg) {
    (void)opus; (void)len;
    if (radio_member_silenced(inst, group_id, ed)) return; /* локальный аудио-потребитель */
    struct rx_evt* rx = (struct rx_evt*)arg;
    if (fin) {
        if (rx->active && stream == rx->stream) rx->fin_seen = 1;
        return;
    }
    if (!rx->active || stream != rx->stream) {
        rx->active = 1;
        rx->stream = stream;
        rx->expect_seq = 0;
        rx->frames = 0;
        rx->fin_seen = 0;
        rx->seq_ok = 1;
    }
    if (seq != rx->expect_seq) rx->seq_ok = 0;
    rx->expect_seq = (uint16_t)(seq + 1);
    rx->frames++;
}

/* ── настройка канала и мемберов ── */

static void channel_put_shared(struct UTUN_INSTANCE* inst, int has_priv) {
    topo_node_sqlite_channel_put(inst->topo_sqlite_db, CH_ID, "radio-test", g_sh.ed_pub[IDX_A],
        g_sh.ch_x_pub, NULL, g_sh.ch_ed_pub, has_priv ? g_sh.ch_ed_priv : NULL, g_sh.ch_sig);
    chat_core_ensure_channel_ready(inst, CH_ID);
}

static void member_put_self(struct UTUN_INSTANCE* inst, const char* name) {
    uint64_t join_ts = (uint64_t)ntp_time_get_seconds(inst);
    uint8_t jm[MEMBER_JOIN_MSG_SIZE];
    int jl = member_sync_build_join_msg(g_sh.ch_x_pub, g_sh.ch_ed_pub, inst->my_ed25519_pubkey,
                                        inst->my_keys.public_key, jm, (int)sizeof(jm));
    uint8_t join_sig[64]; sc_ed25519_sign(inst->my_ed25519_privkey, jm, (size_t)jl, join_sig);
    uint8_t tree_sig[64]; member_sync_sign_join(g_sh.ch_ed_priv, CH_ID, NULL, inst->my_keys.public_key, inst->my_ed25519_pubkey,
        TEST_JOIN_ID(g_sh.ch_x_pub, g_sh.ch_ed_pub, inst->my_ed25519_pubkey, inst->my_keys.public_key), join_ts, tree_sig);
    char userinfo[256]; snprintf(userinfo, sizeof(userinfo), "{\"name\":\"%s\"}", name);
    uint8_t update[64]; assert(!test_member_profile(inst->my_ed25519_privkey,
        TEST_JOIN_ID(g_sh.ch_x_pub, g_sh.ch_ed_pub, inst->my_ed25519_pubkey, inst->my_keys.public_key), join_ts, userinfo, update));
    member_sync_put(inst, CH_ID, inst->my_keys.public_key, inst->my_ed25519_pubkey,
                    join_sig, join_ts, update, join_ts, userinfo, NULL, NULL, 0, 0, tree_sig);
}

static void member_put_known(struct UTUN_INSTANCE* inst, int idx, const char* name) {
    uint8_t body[MEMBER_JOIN_MSG_SIZE], join[64], tree[64];
    int len = member_sync_build_join_msg(g_sh.ch_x_pub, g_sh.ch_ed_pub, g_sh.ed_pub[idx], g_sh.x_pub[idx], body, sizeof(body));
    if (len < 0 || sc_ed25519_sign(g_sh.ed_private[idx], body, len, join) != SC_OK ||
        member_sync_sign_join(g_sh.ch_ed_priv, CH_ID, NULL, g_sh.x_pub[idx], g_sh.ed_pub[idx],
            TEST_JOIN_ID(g_sh.ch_x_pub, g_sh.ch_ed_pub, g_sh.ed_pub[idx], g_sh.x_pub[idx]), 1, tree)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: known member certificate unavailable idx=%d", idx); return;
    }
    char userinfo[256]; snprintf(userinfo, sizeof(userinfo), "{\"name\":\"%s\"}", name);
    uint8_t update[64]; assert(!test_member_profile(g_sh.ed_private[idx],
        TEST_JOIN_ID(g_sh.ch_x_pub, g_sh.ch_ed_pub, g_sh.ed_pub[idx], g_sh.x_pub[idx]), 1, userinfo, update));
    member_sync_put(inst, CH_ID, g_sh.x_pub[idx], g_sh.ed_pub[idx],
                    join, 1, update, 1, userinfo, NULL, NULL, 0, 0, tree);
}

static void do_setup(struct UTUN_INSTANCE* inst, int role) {
    switch (role) {
    case IDX_A:
        channel_put_shared(inst, 1);
        member_put_self(inst, "A");
        member_put_known(inst, IDX_B, "B");
        member_put_known(inst, IDX_C, "C");
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_B]), CH_ID, NULL, NULL);
        break;
    case IDX_B:
        channel_put_shared(inst, 0);
        member_put_self(inst, "B");
        member_put_known(inst, IDX_A, "A");
        member_put_known(inst, IDX_C, "C");
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_A]), CH_ID, NULL, NULL);
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_C]), CH_ID, NULL, NULL);
        break;
    case IDX_C:
        channel_put_shared(inst, 0);
        member_put_self(inst, "C");
        member_put_known(inst, IDX_A, "A");
        member_put_known(inst, IDX_B, "B");
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_B]), CH_ID, NULL, NULL);
        break;
    }
}

/* ── talk: 5 кадров + FIN от A ── */

static void do_talk(struct UTUN_INSTANCE* A) {
    uint64_t gid = topo_groups_channel_slot(A->topo_groups, CH_ID);
    static const uint8_t opus[16] = { 0xf8, 0xff, 0xfe, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d };
    radio_talk_begin(A, gid);
    for (int i = 0; i < N_FRAMES; i++) radio_talk_send(A, gid, opus, sizeof(opus));
    radio_talk_end(A, gid);
}

/* ── master state-machine ── */

static void rad_tick(void* arg) {
    struct rad_ctx* t = arg;
    t->timer = NULL;
    if (t->result) return;

    struct UTUN_INSTANCE* A = t->inst[IDX_A];
    struct UTUN_INSTANCE* B = t->inst[IDX_B];
    struct UTUN_INSTANCE* C = t->inst[IDX_C];
    uint64_t na = g_sh.nid[IDX_A], nb = g_sh.nid[IDX_B], nc = g_sh.nid[IDX_C];
    uint64_t gid = topo_groups_channel_slot(A->topo_groups, CH_ID);

    switch (t->phase) {
    case P_WAIT_CONN:
        if (conn_up(A, nb) && conn_up(B, na) && conn_up(B, nc) && conn_up(C, nb))
            t->phase = P_SETUP;
        break;
    case P_SETUP:
        do_setup(A, IDX_A);
        do_setup(B, IDX_B);
        do_setup(C, IDX_C);
        t->phase = P_WAIT_BGP;
        break;
    case P_WAIT_BGP:
        if (bgp_has(A, nb) && bgp_has(A, nc) && bgp_has(B, na) && bgp_has(B, nc) && bgp_has(C, na) && bgp_has(C, nb))
            t->phase = P_RADIO_CALLS;
        break;
    case P_RADIO_CALLS: {
        uint64_t now = get_time_tb();
        if (g_member_event_bad) { t->result = 2; uasync_stop(t->ua); return; }
        switch (t->call_step) {
        case 0: {
            uint8_t key[32];
            if (member_sync_verified_ed25519(A, CH_ID, nc, key) || member_sync_verified_ed25519(C, CH_ID, na, key)) break;
            if (!check_radio_idle_timers(t)) {
                DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test idle radio invitation service has timers");
                t->result = 2; uasync_stop(t->ua); return;
            }
            /* Уже расшифрованный пакет от проверенного пира: даже INT64_MIN обязан безопасно отбрасываться. */
            struct ll_entry* expired = ll_alloc_lldgram(ROUTER_SVC_HDR_SIZE + 18);
            if (!expired) abort();
            expired->len = ROUTER_SVC_PAYLOAD_OFF + 18;
            memset(expired->dgram, 0, expired->len);
            expired->dgram[0] = ETCP_RT_ID_RADIO_CALL;
            memcpy(expired->dgram + ROUTER_SVC_SRC_ED_OFF, C->my_ed25519_pubkey, 32);
            memcpy(expired->dgram + ROUTER_SVC_DST_ED_OFF, A->my_ed25519_pubkey, 32);
            memcpy(expired->dgram + ROUTER_SVC_GROUP_OFF, &gid, 8);
            expired->dgram[ROUTER_SVC_FLAGS_OFF] = ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED;
            expired->dgram[ROUTER_SVC_PAYLOAD_OFF] = 1;
            uint64_t expired_id = 108; int64_t expired_at = INT64_MIN;
            memcpy(expired->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, &expired_id, 8);
            memcpy(expired->dgram + ROUTER_SVC_PAYLOAD_OFF + 9, &expired_at, 8);
            struct ETCP_CONN source = {.instance = A};
            A->router_bindings.callbacks[ETCP_RT_ID_RADIO_CALL](&source, expired);
            if (radio_call_busy(A) || g_member_event_bad) {
                DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test malformed invite created a call"); t->result = 2; uasync_stop(t->ua); return;
            }
            struct TOPO_GROUP* cg = topo_groups_find(C->topo_groups, gid);
            struct ETCP_CONN* path = topo_group_find_conn_for_node(cg, na);
            if (!path) break;
            int saved_up = path->links_up;
            path->links_up = 0;
            radio_action(C, 106, RADIO_CALL_START, A->my_ed25519_pubkey);
            path->links_up = saved_up;
            radio_action(C, 107, RADIO_CALL_START, C->my_ed25519_pubkey);
            if (g_calls[6][IDX_C].reason != RADIO_CALL_UNAVAILABLE || g_calls[7][IDX_C].reason != RADIO_CALL_UNAVAILABLE ||
                radio_call_busy(C)) {
                DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test BGP without live path/self-call must fail before signaling");
                t->result = 2; uasync_stop(t->ua); return;
            }
            for (int i = 0; i < 3; ++i) {
                g_call_recv[i] = t->inst[i]->router_bindings.callbacks[ETCP_RT_ID_RADIO_CALL];
                t->inst[i]->router_bindings.callbacks[ETCP_RT_ID_RADIO_CALL] = call_packet_loss;
            }
            g_call_ready = 0; t->call_started = now;
            radio_action(A, 100, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step;
            break;
        }
        case 1:
            if (g_calls[0][IDX_A].finished && g_calls[0][IDX_C].finished) {
                uint64_t elapsed = g_calls[0][IDX_A].finished_tb - t->call_started;
                if (g_calls[0][IDX_A].reason != RADIO_CALL_UNAVAILABLE || elapsed < RADIO_CALL_RESPONSE_TB ||
                    elapsed > RADIO_CALL_RESPONSE_TB + 1000 || g_calls[0][IDX_C].incoming != 1) {
                    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test first response deadline failed reason=%d elapsed_tb=%llu incoming=%d",
                        g_calls[0][IDX_A].reason, (unsigned long long)elapsed, g_calls[0][IDX_C].incoming);
                    t->result = 2; uasync_stop(t->ua); return;
                }
                g_call_ready = 1; g_drop_result = 1; g_drop_ack = 1;
                radio_action(A, 101, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step;
            }
            break;
        case 2:
            if (g_calls[1][IDX_A].state == RADIO_CALL_RINGING) {
                t->start_gate = db_tx_begin(A->db_worker, "radio-start-fixture", DB_ASYNC_READ, start_gate_ready, start_gate_done, t);
                if (!t->start_gate) {
                    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio fixture: caller DB gate was refused");
                    t->result = 2; uasync_stop(t->ua); return;
                }
                radio_set_active(C, gid, 1);
                radio_action(C, 101, RADIO_CALL_ANSWER, 0); ++t->call_step;
            }
            break;
        case 3:
            if (g_calls[1][IDX_A].finished && g_calls[1][IDX_C].finished) {
                if (!t->gate_ready) break;
                if (g_calls[1][IDX_A].reason == RADIO_CALL_ACCEPTED && g_calls[1][IDX_C].reason == RADIO_CALL_ACCEPTED) {
                    if (!t->gate_released) {
                        if (radio_is_active(A, gid) || !radio_start_pending(A, gid)) {
                            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio fixture: held DB did not delay caller activation active=%d pending=%d",
                                radio_is_active(A, gid), radio_start_pending(A, gid));
                            t->result = 2; uasync_stop(t->ua); return;
                        }
                        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "radio fixture: FINISHED accepted while caller waits for settings; releasing DB");
                        t->gate_released = 1; t->call_started = now; db_tx_commit(t->start_gate);
                    }
                    /* FINISHED завершает приглашение; локальное включение ещё ждёт своего READ настроек. */
                    if (((!radio_is_active(A, gid) && radio_start_pending(A, gid)) ||
                         (!radio_is_active(C, gid) && radio_start_pending(C, gid))) && now - t->call_started < RADIO_CALL_RESPONSE_TB) break;
                }
                if (g_calls[1][IDX_A].reason != RADIO_CALL_ACCEPTED || g_calls[1][IDX_C].reason != RADIO_CALL_ACCEPTED ||
                    !radio_is_active(A, gid) || !radio_is_active(C, gid) || t->gate_done != 1) {
                    DEBUG_ERROR(DEBUG_CATEGORY_RADIO,
                        "test radio acceptance failed reasons=%d/%d states=%d/%d active=%d/%d pending=%d/%d",
                        g_calls[1][IDX_A].reason, g_calls[1][IDX_C].reason, g_calls[1][IDX_A].state, g_calls[1][IDX_C].state,
                        radio_is_active(A, gid), radio_is_active(C, gid), radio_start_pending(A, gid), radio_start_pending(C, gid));
                    t->result = 2; uasync_stop(t->ua); return;
                }
                DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "radio fixture: both subscriptions active after asynchronous settings READ");
                radio_set_active(A, gid, 0); radio_set_active(C, gid, 0);
                radio_action(A, 102, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step;
            }
            break;
        case 4:
            if (g_calls[2][IDX_A].state == RADIO_CALL_RINGING) { radio_action(C, 102, RADIO_CALL_DECLINE, 0); ++t->call_step; }
            break;
        case 5:
            if (g_calls[2][IDX_A].finished) {
                if (g_calls[2][IDX_A].reason != RADIO_CALL_DECLINED || radio_is_active(A, gid) || radio_is_active(C, gid)) {
                    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test radio decline failed"); t->result = 2; uasync_stop(t->ua); return;
                }
                radio_action(A, 103, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step;
            }
            break;
        case 6:
            if (g_calls[3][IDX_A].state == RADIO_CALL_RINGING) {
                radio_action(C, 104, RADIO_CALL_START, B->my_ed25519_pubkey);
                radio_action(A, 103, RADIO_CALL_CANCEL, 0); ++t->call_step;
            }
            break;
        case 7:
            if (g_calls[3][IDX_C].finished && g_calls[4][IDX_C].finished) {
                if (g_calls[3][IDX_C].reason != RADIO_CALL_CANCELLED || g_calls[4][IDX_C].reason != RADIO_CALL_BUSY) {
                    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test radio cancel/busy failed"); t->result = 2; uasync_stop(t->ua); return;
                }
                t->call_started = now;
                radio_action(A, 105, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step;
            }
            break;
        case 8:
            if (g_calls[5][IDX_A].finished && g_calls[5][IDX_C].finished) {
                uint64_t elapsed = g_calls[5][IDX_A].finished_tb - t->call_started;
                if (g_calls[5][IDX_A].reason != RADIO_CALL_NO_ANSWER || elapsed < RADIO_CALL_RING_TB || elapsed > RADIO_CALL_RING_TB + 1000) {
                    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test one minute deadline failed reason=%d elapsed_tb=%llu",
                        g_calls[5][IDX_A].reason, (unsigned long long)elapsed); t->result = 2; uasync_stop(t->ua); return;
                }
                for (int n = 0; n < 6; ++n) for (int i = 0; i < 3; ++i) {
                    if (g_calls[n][i].finished > 1 || g_calls[n][i].incoming > 1) {
                        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test duplicated radio call event id=%d node=%d", n + 100, i);
                        t->result = 2; uasync_stop(t->ua); return;
                    }
                }
                t->call_started = now; ++t->call_step;
            }
            break;
        case 9:
            if (now - t->call_started >= 5000) {
                if (radio_call_busy(A) || radio_call_busy(B) || radio_call_busy(C) || !check_radio_idle_timers(t)) {
                    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test finished invitations retain timers or busy state");
                    t->result = 2; uasync_stop(t->ua); return;
                }
                DEBUG_INFO(DEBUG_CATEGORY_RADIO, "test radio calls passed: 2s/60s, accept/decline/cancel/busy, lost result/ACK, idle has no timer");
                // Проверка idle пересоздала сервис и его binding; снова включаем потерю пакетов приложения.
                for (int i = 0; i < 3; ++i) t->inst[i]->router_bindings.callbacks[ETCP_RT_ID_RADIO_CALL] = call_packet_loss;
                call_set_phone_state(C, 2, CALL_POLICY_WARN_PHONE);
                radio_action(A, 108, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step;
            }
            break;
        case 10:
            if (g_calls[8][IDX_A].state != RADIO_CALL_WAIT_DECISION) break;
            if (g_calls[8][IDX_C].incoming || g_member_phone[IDX_A][IDX_C]) goto phone_failed;
            call_set_phone_state(C, 1, CALL_POLICY_WARN_PHONE);
            t->call_started = now; ++t->call_step; break;
        case 11:
            if (now - t->call_started < 10000) break;
            if (g_calls[8][IDX_C].incoming) goto phone_failed; // Освободившийся телефон сам не начинает вызов.
            g_drop_force = 2;
            radio_action(A, 108, RADIO_CALL_CONTINUE, 0); ++t->call_step; break;
        case 12:
            if (g_calls[8][IDX_A].state != RADIO_CALL_RINGING) break;
            if (g_calls[8][IDX_C].incoming != 1 || g_drop_force > 0) goto phone_failed;
            radio_action(C, 108, RADIO_CALL_DECLINE, 0); ++t->call_step; break;
        case 13:
            if (!g_calls[8][IDX_A].finished) break;
            radio_action(A, 108, RADIO_CALL_CONTINUE, 0); // Запоздалое UI-подтверждение не оживляет попытку.
            call_set_phone_state(C, 2, CALL_POLICY_PUBLISH_PHONE);
            radio_action(A, 109, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step; break;
        case 14:
            if (g_calls[9][IDX_A].state != RADIO_CALL_RINGING || g_member_phone[IDX_A][IDX_C] != 0xC0) break;
            if (g_calls[9][IDX_C].incoming != 1 || g_calls[8][IDX_C].incoming != 1) goto phone_failed;
            {
                uint8_t member[CHAT_MEMBER_DISPLAY_SIZE];
                if (chat_core_get_single_member(A, CH_ID, C->my_ed25519_pubkey, member) || (member[CHAT_MEMBER_FLAGS_OFF] & 0xC0) != 0xC0) goto phone_failed;
            }
            radio_action(A, 109, RADIO_CALL_CANCEL, 0); ++t->call_step; break;
        case 15:
            if (!g_calls[9][IDX_C].finished) break;
            call_set_phone_state(C, 2, CALL_POLICY_WARN_PHONE | CALL_POLICY_REJECT_PHONE);
            radio_action(A, 110, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step; break;
        case 16:
            if (!g_calls[10][IDX_A].finished || g_member_phone[IDX_A][IDX_C]) break;
            if (g_calls[10][IDX_A].reason != RADIO_CALL_PHONE_BUSY || g_calls[10][IDX_C].incoming) goto phone_failed;
            call_set_phone_state(C, 0, CALL_POLICY_BLOCK_CALLS);
            radio_action(A, 111, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step; break;
        case 17:
            if (g_calls[11][IDX_A].state != RADIO_CALL_RINGING) break;
            radio_action(A, 111, RADIO_CALL_CANCEL, 0); ++t->call_step; break;
        case 18:
            if (!g_calls[11][IDX_C].finished) break;
            call_set_phone_state(C, 2, CALL_POLICY_BLOCK_RADIO | CALL_POLICY_WARN_PHONE);
            radio_action(A, 112, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step; break;
        case 19:
            if (!g_calls[12][IDX_A].finished) break;
            if (g_calls[12][IDX_A].reason != RADIO_CALL_POLICY || g_calls[12][IDX_C].incoming) goto phone_failed;
            call_set_phone_state(C, 2, CALL_POLICY_WARN_PHONE);
            t->call_started = now;
            radio_action(A, 113, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step; break;
        case 20:
            if (!g_calls[13][IDX_A].finished) break;
            if (g_calls[13][IDX_A].reason != RADIO_CALL_NO_ANSWER || g_calls[13][IDX_C].incoming ||
                now - t->call_started < RADIO_CALL_RING_TB || now - t->call_started > RADIO_CALL_RING_TB + 1000) goto phone_failed;
            call_set_phone_state(C, 0, 0);
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "phone policy passed: hidden/published BGP, warn, override with loss, reject, independent blocks, 60s choice");
            radio_set_active(C, gid, 1);
            call_set_phone_state(C, 0, 0);
            g_drop_result = 1; g_drop_ack = 1;
            radio_action(A, 114, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step; break;
        case 21:
            if (g_calls[14][IDX_A].state != RADIO_CALL_RINGING) break;
            if (g_calls[14][IDX_C].incoming != 1 || !radio_is_active(C, gid)) goto phone_failed;
            radio_action(C, 114, RADIO_CALL_ANSWER, 0); ++t->call_step; break;
        case 22:
            if (!g_calls[14][IDX_A].finished) break;
            if (g_calls[14][IDX_A].reason != RADIO_CALL_ACCEPTED || !radio_is_active(C, gid)) goto phone_failed;
            radio_set_active(A, gid, 0);
            /* Настоящий импорт сохраняет принадлежность при последующих NODEINFO. */
            {
                struct devgroup_record record; uint8_t ep[32], xp[32], receipt[16] = {1};
                if (chat_devgroup_rename(B, "Devices") || devgroup_load_local(B, &record, ep, xp) != 1 ||
                    chat_devgroup_import(C, &record, ep, xp, receipt, B->my_ed25519_pubkey)) goto phone_failed;
                OPENSSL_cleanse(ep, sizeof(ep)); OPENSSL_cleanse(xp, sizeof(xp));
            }
            t->call_step = 35; break;
        case 35: {
            if (device_targets_count != 2) {
                if (!device_targets_pending) {
                    device_targets_pending = 1; call_targets_read(A, gid, C->my_ed25519_pubkey, 1, 3, device_targets_ready, NULL);
                }
                break;
            }
            struct radio_call_request req = {.inst = A, .id = 115, .action = RADIO_CALL_START, .devices = 1};
            memcpy(req.group, g_sh.ch_ed_pub, 32);
            memcpy(req.peer, C->my_ed25519_pubkey, sizeof(req.peer));
            radio_call_execute(&req); t->call_step = 23; break;
        }
        case 23:
            if (!g_calls[15][IDX_B].incoming || !g_calls[15][IDX_C].incoming) break;
            radio_action(B, 115, RADIO_CALL_ANSWER, 0); radio_action(C, 115, RADIO_CALL_ANSWER, 0);
            ++t->call_step; break;
        case 24:
            if (!g_calls[15][IDX_A].finished || !g_calls[15][IDX_B].finished || !g_calls[15][IDX_C].finished) break;
            if (g_calls[15][IDX_A].reason != RADIO_CALL_ACCEPTED ||
                (g_calls[15][IDX_B].reason == RADIO_CALL_ACCEPTED) + (g_calls[15][IDX_C].reason == RADIO_CALL_ACCEPTED) != 1 ||
                !radio_is_active(C, gid)) goto phone_failed;
            {
                struct radio_call_request req = {.inst = A, .id = 116, .action = RADIO_CALL_START, .devices = 1};
            memcpy(req.group, g_sh.ch_ed_pub, 32);
                memcpy(req.peer, C->my_ed25519_pubkey, sizeof(req.peer));
                radio_call_execute(&req);
            }
            ++t->call_step; break;
        case 25:
            if (!g_calls[16][IDX_B].incoming || !g_calls[16][IDX_C].incoming) break;
            radio_action(C, 116, RADIO_CALL_DECLINE, 0); ++t->call_step; break;
        case 26:
            if (!g_calls[16][IDX_A].finished || !g_calls[16][IDX_B].finished || !g_calls[16][IDX_C].finished) break;
            if (g_calls[16][IDX_A].reason != RADIO_CALL_DECLINED || g_calls[16][IDX_B].reason != RADIO_CALL_CANCELLED ||
                !radio_is_active(C, gid)) goto phone_failed;
            for (int i = 0; i < 3; ++i) {
                device_signal_recv[i] = t->inst[i]->router_bindings.callbacks[ETCP_RT_ID_CALL];
                t->inst[i]->router_bindings.callbacks[ETCP_RT_ID_CALL] = device_signal_loss;
            }
            call_start_devices(A, CH_ID, C->my_ed25519_pubkey, 20001); ++t->call_step; break;
        case 27: {
            struct call_session* b = call_session_find(B->call, 20001), *c = call_session_find(C->call, 20001);
            if (!b || !c) break;
            if (b->state != CALL_ST_RINGING || c->state != CALL_ST_RINGING) goto phone_failed;
            call_accept(B, 20001); call_accept(C, 20001);
            if (b->state != CALL_ST_ANSWERING || c->state != CALL_ST_ANSWERING) goto phone_failed;
            ++t->call_step; break;
        }
        case 28: {
            struct call_session* a = call_session_find(A->call, 20001);
            if (!a || a->state != CALL_ST_ACTIVE) break;
            struct call_session* b = call_session_find(B->call, 20001), *c = call_session_find(C->call, 20001);
            if ((b && b->state == CALL_ST_ACTIVE) + (c && c->state == CALL_ST_ACTIVE) != 1 || !a->selected) goto phone_failed;
            if (device_drop_select > 0 || device_drop_selected > 0) goto phone_failed;
            call_hangup(A, 20001); ++t->call_step; break;
        }
        case 29:
            if (call_busy(A) || call_busy(B) || call_busy(C)) break;
            call_start_devices(A, CH_ID, C->my_ed25519_pubkey, 20002); ++t->call_step; break;
        case 30:
            if (!call_session_find(B->call, 20002) || !call_session_find(C->call, 20002)) break;
            call_decline(C, 20002); ++t->call_step; break;
        case 31:
            if (call_busy(A) || call_busy(B) || call_busy(C)) break;
            radio_action(A, 118, RADIO_CALL_START, C->my_ed25519_pubkey); ++t->call_step; break;
        case 32:
            if (g_calls[18][IDX_A].state != RADIO_CALL_RINGING) break;
            radio_action(C, 118, RADIO_CALL_ANSWER, 0); ++t->call_step; break;
        case 33:
            if (g_calls[18][IDX_C].state != RADIO_CALL_SELECTED) break;
            radio_action(A, 118, RADIO_CALL_CANCEL, 0); ++t->call_step; break;
        case 34:
            if (!g_calls[18][IDX_C].finished) break;
            radio_action(C, 118, RADIO_CALL_ACCEPT, 0); // Запоздалое завершение GUI не оживляет приглашение.
            if (!radio_is_active(C, gid) || radio_call_busy(A) || radio_call_busy(C)) goto phone_failed;
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "device calls passed: listening ring, simultaneous answers, group decline, cancel during selection");
            t->phase = P_RADIO_ON; break;
        phone_failed:
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "phone policy failed step=%d force_drops_left=%d incoming108=%d state108=%d reason108=%d",
                t->call_step, g_drop_force, g_calls[8][IDX_C].incoming, g_calls[8][IDX_A].state, g_calls[8][IDX_A].reason);
            t->result = 2; uasync_stop(t->ua); return;
        }
        break;
    }
    case P_RADIO_ON:
        radio_set_active(A, gid, 1);
        radio_set_active(B, gid, 1);
        radio_set_active(C, gid, 1);
        t->phase = P_WAIT_FLAGS;
        break;
    case P_WAIT_FLAGS:
        if (node_radio(A, nb) && node_radio(A, nc) && node_radio(B, na) && node_radio(B, nc) && node_radio(C, na) &&
            node_radio(C, nb) && member_radio_events_ready(1))
            t->phase = P_TALK1;
        break;
    case P_TALK1:
        for (int i = 0; i < 3; i++) {
            if (!check_member_radios(t->inst[i], i, 1)) { t->result = 2; uasync_stop(t->ua); return; }
            struct chat_member_list_req* req = u_calloc(1, sizeof(*req));
            if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: member request OOM"); t->result = 2; uasync_stop(t->ua); return; }
            req->inst = t->inst[i]; strcpy(req->ch_id, CH_ID);
            chat_core_request_member_list_trampoline(req);
        }
        t->phase = P_WAIT_MEMBER_LISTS;
        break;
    case P_WAIT_MEMBER_LISTS:
        if (!g_member_lists[0] || !g_member_lists[1] || !g_member_lists[2]) break;
        for (int i = 0; i < 3; i++) if (g_member_lists[i] != 1 || !check_member_radios(t->inst[i], i, 1)) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: asynchronous member list invalid dst=%d events=%d", i, g_member_lists[i]);
            t->result = 2; uasync_stop(t->ua); return;
        }
        memset(g_rx, 0, sizeof(g_rx));
        do_talk(A);
        t->phase = P_WAIT_RX1;
        break;
    case P_WAIT_RX1:
        if (g_rx[IDX_B].frames == N_FRAMES && g_rx[IDX_B].fin_seen && g_rx[IDX_B].seq_ok
            && g_rx[IDX_C].frames == N_FRAMES && g_rx[IDX_C].fin_seen && g_rx[IDX_C].seq_ok) {
            if (chat_core_toggle_member_radio_mute(B, CH_ID, A->my_ed25519_pubkey) < 0 || !radio_member_muted(B, gid, A->my_ed25519_pubkey) ||
                radio_member_muted(C, gid, A->my_ed25519_pubkey)) { t->result = 2; uasync_stop(t->ua); return; }
            memset(g_rx, 0, sizeof(g_rx)); do_talk(A);
            t->phase = P_WAIT_MUTED_RELAY;
        }
        break;
    case P_WAIT_MUTED_RELAY:
        if (g_rx[IDX_C].frames == N_FRAMES && g_rx[IDX_C].fin_seen && g_rx[IDX_C].seq_ok) {
            if (g_rx[IDX_B].frames || g_rx[IDX_B].fin_seen || !radio_is_active(B, gid) ||
                chat_core_toggle_member_radio_mute(B, CH_ID, A->my_ed25519_pubkey) < 0 || radio_member_muted(B, gid, A->my_ed25519_pubkey)) {
                DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: locally muted relay blocked downstream or local mute failed");
                t->result = 2; uasync_stop(t->ua); return;
            }
            t->phase = P_RADIO_OFF_C;
        }
        break;
    case P_RADIO_OFF_C:
        radio_set_active(C, gid, 0);
        t->phase = P_WAIT_OFF_C;
        break;
    case P_WAIT_OFF_C:
        /* ждём, пока A и B увидят radio=0 у C (обрезание ветви C) */
        if (!node_radio(A, nc) && !node_radio(B, nc) && member_radio_events_ready(0)) {
            memset(g_rx, 0, sizeof(g_rx));
            t->phase = P_TALK2;
        }
        break;
    case P_TALK2:
        for (int i = 0; i < 3; i++)
            if (!check_member_radios(t->inst[i], i, 0)) { t->result = 2; uasync_stop(t->ua); return; }
        do_talk(A);
        t->phase = P_WAIT_RX2;
        break;
    case P_WAIT_RX2:
        if (g_rx[IDX_B].frames == N_FRAMES && g_rx[IDX_B].fin_seen && g_rx[IDX_B].seq_ok
            && g_rx[IDX_C].frames == 0 && !g_rx[IDX_C].fin_seen)
            t->phase = P_REENABLE_C;
        break;
    case P_REENABLE_C:
        radio_set_active(C, gid, 1);
        t->phase = P_WAIT_REENABLE_C;
        break;
    case P_WAIT_REENABLE_C:
        if (node_radio(A, nc) && node_radio(B, nc)) t->phase = P_TALK_STALLED;
        break;
    case P_TALK_STALLED: {
        const uint8_t opus[] = {0xf8, 0xff, 0xfe};
        memset(g_rx, 0, sizeof(g_rx));
        if (radio_talk_begin(A, gid) < 0 || radio_talk_send(A, gid, opus, sizeof(opus)) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: cannot start burst without FIN");
            t->result = 2; uasync_stop(t->ua); return;
        }
        t->phase = P_WAIT_STALLED_START;
        break;
    }
    case P_WAIT_STALLED_START:
        if (g_rx[IDX_B].frames && g_rx[IDX_C].frames) {
            if (!g_talk_on[IDX_B] || !g_talk_on[IDX_C] || g_member_event_bad) {
                DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: missing talk START B=%d C=%d bad=%d",
                            g_talk_on[IDX_B], g_talk_on[IDX_C], g_member_event_bad);
                t->result = 2; uasync_stop(t->ua); return;
            }
            radio_set_active(C, gid, 0);
            if (g_talk_on[IDX_C] || !g_talk_on[IDX_B]) {
                DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: subscription stop did not clear only C: B=%d C=%d",
                            g_talk_on[IDX_B], g_talk_on[IDX_C]);
                t->result = 2; uasync_stop(t->ua); return;
            }
            t->phase = P_WAIT_STALLED_STOP;
        }
        break;
    case P_WAIT_STALLED_STOP:
        if (!g_talk_on[IDX_B]) {
            if (g_rx[IDX_B].fin_seen || g_rx[IDX_C].fin_seen || g_member_event_bad) {
                DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: stopped indication without FIN check failed B_fin=%d C_fin=%d bad=%d",
                            g_rx[IDX_B].fin_seen, g_rx[IDX_C].fin_seen, g_member_event_bad);
                t->result = 2; uasync_stop(t->ua); return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "test: talk STOP without FIN verified (C unsubscribe, B timeout)");
            radio_talk_end(A, gid);
            radio_set_active(C, gid, 1);
            t->phase = P_WAIT_RESTORED_C;
        }
        break;
    case P_WAIT_RESTORED_C:
        if (node_radio(A, nc) && node_radio(B, nc)) t->phase = P_WITHDRAW_C;
        break;
    case P_WITHDRAW_C: {
        struct TOPO_GROUP* group = topo_groups_find(A->topo_groups, gid);
        struct TOPO_GROUP_NODE* node = topo_node_find_by_id(group, nc);
        while (node && node->paths) {
            struct ETCP_CONN* from = ((struct TOPO_NODEPATH*)node->paths->head)->conn;
            struct TOPOMSG_WITHDRAW_PKT withdraw = {0};
            memcpy(withdraw.ed25519, C->my_ed25519_pubkey, sizeof(withdraw.ed25519));
            if (topo_group_process_withdraw(group, from, (const uint8_t*)&withdraw, sizeof(withdraw)) < 0) {
                t->result = 2; uasync_stop(t->ua); return;
            }
            node = topo_node_find_by_id(group, nc);
        }
        if (!radio_is_active(C, gid)) {
            t->result = 2; uasync_stop(t->ua); return;
        }
        t->phase = P_WAIT_WITHDRAW_EVENT;
        break;
    }
    case P_WAIT_WITHDRAW_EVENT:
        if (g_member_radio[IDX_A][IDX_C]) break;
        if (!check_member_radios(A, IDX_A, 0)) { t->result = 2; uasync_stop(t->ua); return; }
        t->phase = P_DONE;
        break;
    case P_DONE:
        if (g_member_event_bad || g_talk_starts[IDX_B] != 4 || g_talk_stops[IDX_B] != 4 ||
            g_talk_starts[IDX_C] != 3 || g_talk_stops[IDX_C] != 3 || g_talk_on[IDX_B] || g_talk_on[IDX_C]) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "test: talk lifecycle mismatch B=%d/%d C=%d/%d on=%d/%d bad=%d",
                        g_talk_starts[IDX_B], g_talk_stops[IDX_B], g_talk_starts[IDX_C], g_talk_stops[IDX_C],
                        g_talk_on[IDX_B], g_talk_on[IDX_C], g_member_event_bad);
            t->result = 2; uasync_stop(t->ua); return;
        }
        t->result = 1;
        uasync_stop(t->ua);
        return;
    }

    if (++t->ticks > MAX_TICKS) {
        fprintf(stderr, "test: timeout in phase %d (B_rx=%d/%d fin=%d seqok=%d C_rx=%d/%d fin=%d seqok=%d radio_flags: A(B=%d C=%d) B(A=%d C=%d) C(A=%d B=%d))\n",
                (int)t->phase,
                g_rx[IDX_B].frames, N_FRAMES, g_rx[IDX_B].fin_seen, g_rx[IDX_B].seq_ok,
                g_rx[IDX_C].frames, N_FRAMES, g_rx[IDX_C].fin_seen, g_rx[IDX_C].seq_ok,
                node_radio(A, nb), node_radio(A, nc), node_radio(B, na), node_radio(B, nc),
                node_radio(C, na), node_radio(C, nb));
        t->result = 2;
        uasync_stop(t->ua);
        return;
    }
    t->timer = uasync_set_timeout(t->ua, TICK_TB, t, rad_tick, "rad_tick");
}

/* ── main ── */

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level_by_name("radio", "debug");
        debug_set_category_level_by_name("bgp", "info");
        debug_set_category_level_by_name("member_sync", "info");
        debug_set_category_level_by_name("chat_sync", "info");
    }
    test_clock_init(TICK_TB);
    test_clock_sync_wall_time();
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));

    char dir[512]; snprintf(dir, sizeof(dir), "/tmp/utun_radio_XXXXXX");
    if (test_mkdtemp(dir) != 0) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
    test_setenv("UTUN_TEST_DIR", dir, 1);

    fill_shared();
    write_configs(dir);

    printf("=== test_radio ===\n"); fflush(stdout);

    TEST("member snapshot: short names have zero padding"); {
        const char* names[] = {"", "0x06B9", "Без имени", "123456789012345678901234567890123456789012345678901234567890123"};
        int valid = 1;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            struct chat_member_display member = {0};
            member.ed25519[0] = 1;
            memset(member.name, 0xFF, sizeof(member.name));
            snprintf(member.name, sizeof(member.name), "%s", names[i]);
            uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE];
            chat_core_serialize_member(&member, wire);
            char expected[64] = {0};
            snprintf(expected, sizeof(expected), "%s", names[i]);
            if (memcmp(wire + 7, expected, sizeof(expected))) { valid = 0; break; }
        }
        if (valid) OK(); else FAIL("name field contains bytes after NUL");
    }

    TEST("radio: members + mute relay + speaker FIN/timeout/off + withdraw"); {
        struct UASYNC* ua = uasync_create();
        if (!ua) { FAIL("uasync_create failed"); return 1; }

        struct rad_ctx t;
        memset(&t, 0, sizeof(t));
        t.ua = ua;
        t.phase = P_WAIT_CONN;

        char cfgA[512], cfgB[512], cfgC[512];
        snprintf(cfgA, sizeof(cfgA), "%s/a.conf", dir);
        snprintf(cfgB, sizeof(cfgB), "%s/b.conf", dir);
        snprintf(cfgC, sizeof(cfgC), "%s/c.conf", dir);

        t.inst[IDX_A] = utun_instance_create(ua, cfgA);
        t.inst[IDX_B] = utun_instance_create(ua, cfgB);
        t.inst[IDX_C] = utun_instance_create(ua, cfgC);
        if (!t.inst[IDX_A] || !t.inst[IDX_B] || !t.inst[IDX_C]) {
            FAIL("instance create failed A=%p B=%p C=%p", (void*)t.inst[IDX_A], (void*)t.inst[IDX_B], (void*)t.inst[IDX_C]);
            uasync_destroy(ua, 0);
            return 1;
        }
        test_etcp_set_peer(t.inst[IDX_B], "to_a", t.inst[IDX_A], "s1");
        test_etcp_set_peer(t.inst[IDX_C], "to_b", t.inst[IDX_B], "s1");
        utun_instance_init(t.inst[IDX_A]);
        utun_instance_init(t.inst[IDX_B]);
        utun_instance_init(t.inst[IDX_C]);
        for (int i = 0; i < 3; i++) memcpy(g_sh.ed_private[i], t.inst[i]->my_ed25519_privkey, 32);
        memset(g_member_radio, -1, sizeof(g_member_radio));
        for (int i = 0; i < 3; i++) chat_event_set_handler(t.inst[i], member_event);

        radio_set_frame_cb(t.inst[IDX_A], rx_frame_cb, &g_rx[IDX_A]);
        radio_set_frame_cb(t.inst[IDX_B], rx_frame_cb, &g_rx[IDX_B]);
        radio_set_frame_cb(t.inst[IDX_C], rx_frame_cb, &g_rx[IDX_C]);

        t.timer = uasync_set_timeout(ua, TICK_TB, &t, rad_tick, "rad_tick");
        test_clock_mainloop(ua);

        int ok = (t.result == 1);

        if (t.inst[IDX_A]) utun_instance_destroy(t.inst[IDX_A]);
        if (t.inst[IDX_B]) utun_instance_destroy(t.inst[IDX_B]);
        if (t.inst[IDX_C]) utun_instance_destroy(t.inst[IDX_C]);
        uasync_destroy(ua, 0);

        if (ok) OK(); else FAIL("result=%d phase=%d", t.result, (int)t.phase);
    }

    char cmd[512]; snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); (void)!system(cmd);

    printf("\n%d/%d passed, %d failed\n", G_PASSED, G_TOTAL, G_FAILED);
    test_clock_finish();
    return G_FAILED > 0 ? 1 : 0;
}
