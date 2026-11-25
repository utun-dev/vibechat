// test_call.c — интеграционный тест P2P аудио-звонка (сигналинг).
//
// Однопоточный: 2 узла (A=caller, B=callee) в одном процессе, один UASYNC.
// Прямое соединение A←B, оба в CHAT-группе. Master state-machine на таймере.
//
// Сценарий:
//   1. PM BGP: живой/закрывающийся/исчезнувший путь, выбор общей группы и повторная проверка.
//   2. happy path из PM: A call_start → B INCOMING → B accept → обе ACTIVE (ACCEPTED)
//      → B hangup с ожидающими видеокомандами → обе ENDED, поздние команды отменены.
//   3. Новый звонок: поздний close старого порта не закрывает новый, detach отменяет post.
//   4. decline из PM: A call_start → B INCOMING → B decline → обе ENDED(DECLINE).
//   HANGUP переживает смену router epoch и потерю END_ACK; повторы не затрагивают новый звонок.
//   Одновременное завершение и deadline без ACK освобождают записи и не дублируют ENDED.

#include "chat_core.h"
#include "chat_sync.h"
#include "chat_event.h"
#include "chat_setting.h"
#include "member_sync.h"
#include "chat_member.h"
#include "../call/call.h"
#include "../dm/dm_core.h"
#include "../dm/dm_crypto.h"
#include "../call/call_proto.h"
#include "../call/call_priv.h"
#include "../call/call_video_transport.h"
#include "../call/call_video_port.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../routing_layer/conn_mgr_priv.h"
#include "../routing_layer/route_crypto.h"
#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/etcp_connections.h"
#include "../transport_layer/secure_channel.h"
#include "../ntp_time.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"
#include "test_clock.h"

#include <sqlite3.h>
#define OPENSSL_API_COMPAT 0x10100000L
#include "test_membership.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

static char CH_ID[65];

#define IDX_A 0
#define IDX_B 1

#define TICK_TB     100      /* 10 ms на такт */
#define MAX_TICKS   6000     /* включает ограниченное удержание tombstone и серию политик входящего вызова */

enum call_phase {
    P_WAIT_CONN,
    P_SETUP,
    P_WAIT_BGP,
    P_STATUS_IDLE, P_STATUS_HIDE, P_STATUS_RESTORE, P_STATUS_RESTORED, P_STATUS_ENDED,
    P_CALL1_START,       /* A: call_start */
    P_WAIT_INCOMING1,    /* B: INCOMING */
    P_ACCEPT,            /* B: call_accept */
    P_WAIT_ACCEPTED,     /* обе ACTIVE */
    P_SEND_MEDIA,        /* A: call_send_media (тестовый Opus-кадр) */
    P_WAIT_MEDIA,        /* B: media_recv_cb получил кадр */
    P_BATCH_SEND, P_BATCH_WAIT, P_PARTIAL_WAIT,
    P_VIDEO_CONFIG, P_VIDEO_SEND, P_VIDEO_BUSY, P_VIDEO_RESUME, P_VIDEO_RANGE_WAIT, P_VIDEO_SWITCH, P_VIDEO_SWITCH_WAIT,
    P_VIDEO_SHRINK_WAIT, P_VIDEO_PORT_OPEN, P_VIDEO_PORT_RECEIVE, P_VIDEO_PORT_SEND, P_VIDEO_PORT_ACK,
    P_VIDEO_DECODE_OK, P_VIDEO_DECODE_REJECT,
    P_VIDEO_PORT_CANCELLED, P_VIDEO_PORT_REOPENED,
    P_WAIT_ENDED1,       /* обе ENDED */
    P_CALL2_START,       /* A: call_start (2-й звонок) */
    P_WAIT_INCOMING2,    /* B: INCOMING */
    P_VIDEO_NEW_CALL, P_VIDEO_NEW_PORT, P_STALE_END,
    P_DECLINE,           /* B: call_decline */
    P_WAIT_ENDED2,       /* обе ENDED + A DECLINED */
    P_TIMEOUT_START, P_TIMEOUT_INCOMING, P_TIMEOUT_ACTIVE, P_TIMEOUT_ENDED,
    P_POLICY_START, P_POLICY_WAIT, P_POLICY_RING, P_POLICY_END,
    P_DONE,
};

struct call_test_ctx {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst[2];
    enum call_phase phase;
    int   result;        /* 0=running, 1=ok, 2=fail */
    int   ticks;
    int phase_ticks;
    void* timer;
    uint64_t call_id;    /* текущий call_id */
    uint64_t previous_call_id;
    int video_restart_tested;
    int policy_index;
};

/* записи событий CHAT_EVT_CALL_* на узел */
struct call_evt {
    int incoming, ringing, accepted, ended, declined, errors, phone_busy;
    uint8_t route_peer[32];
    uint64_t route_group;
    uint64_t incoming_call_id;
    uint64_t accepted_call_id;
    uint64_t ended_call_id;
    uint8_t  ended_reason;
    uint8_t  peer_phone_flags; /* Последний online-снимок собеседника из MEMBER_UPDATED. */
};

static struct call_evt g_ev[2];  /* [A], [B] */
static etcp_recv_fn g_signal_recv[2];
static int g_drop_acks[2], g_dropped_acks[2];

/* Имитируем потерю именно END_ACK, сохраняя доставку нового вызова и транспортных ACK. */
static void signal_recv_loss(struct ETCP_CONN* conn, struct ll_entry* entry) {
    int idx = conn->instance->test_user_ptr == &g_ev[IDX_A] ? IDX_A : IDX_B;
    if (entry && entry->dgram && entry->len > ROUTER_SVC_PAYLOAD_OFF
        && entry->dgram[ROUTER_SVC_PAYLOAD_OFF] == CALL_SUBCMD_END_ACK && g_drop_acks[idx]) {
        if (g_drop_acks[idx] > 0) --g_drop_acks[idx];
        ++g_dropped_acks[idx];
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "test drops END_ACK node=%d count=%d", idx, g_dropped_acks[idx]);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    g_signal_recv[idx](conn, entry);
}

/* записи принятых медиа-кадров (media_recv_cb) */
struct media_evt {
    int      frames;
    uint64_t last_call_id;
    int      last_len;
    uint8_t  last[64];
};
static struct media_evt g_media[2];
static struct call_capture_time g_capture_timing;
static unsigned g_audio_packets;
static const struct call_capture_time sent_timing = {1234567890123ll, 19, 312};

static void audio_recv_cb(struct UTUN_INSTANCE* inst, uint64_t call_id, uint16_t seq, uint32_t ts,
                          const uint8_t* opus, int len, const struct call_capture_time* timing, int packet_start) {
    (void)inst; (void)call_id; (void)seq; (void)ts; (void)opus; (void)len; (void)packet_start;
    g_capture_timing = *timing;
    g_audio_packets += packet_start != 0;
}
static const struct call_audio_ops audio_ops = {.on_media = audio_recv_cb};

static struct call_video_caps g_video_peer[2];
static uint8_t g_video_data[9001];
static int g_video_frames, g_video_ready, g_video_error;
static unsigned g_video_preroll;
static uint64_t g_rate_generation, g_rate_request;
static uint32_t g_rate_bps;
static unsigned g_rate_commands;
static unsigned g_video_keys[2];
static int g_port_frame;
static void port_received(void *arg, const struct call_video_frame *f) {
    (void)arg;
    if (g_port_frame >= 0 && f->frame_id == (uint64_t)g_port_frame + 1 && f->format.generation == 1
        && f->size == sizeof(g_video_data) && !memcmp(f->data, g_video_data, sizeof(g_video_data))) ++g_port_frame;
    else g_port_frame = -1;
}
static void video_caps(void *arg, const struct call_video_caps *caps) { g_video_peer[(size_t)arg] = *caps; }
static void video_need_key(void *arg) { g_video_keys[(size_t)arg]++; }
static int video_ready(void *arg, uint32_t bytes) { (void)arg; return bytes <= sizeof(g_video_data) && g_video_ready; }
static void video_frame(void *arg, const struct call_video_frame *f) {
    (void)arg;
    if (f->format.generation == 1 && f->frame_id >= 2 && f->frame_id <= 7) {
        if (f->frame_id != g_video_preroll + 2 || f->pts_us != sent_timing.pts_us + (int64_t)f->frame_id * 30000
            || f->flags || f->size != sizeof(g_video_data) || memcmp(f->data, g_video_data, sizeof(g_video_data))) g_video_error = 1;
        g_video_preroll++; return;
    }
    g_video_frames++;
    if (f->size != sizeof(g_video_data) || memcmp(f->data, g_video_data, sizeof(g_video_data))
        || f->format.generation != (uint64_t)g_video_frames || f->format.codec != (g_video_frames == 1 ? CALL_VIDEO_H265 : CALL_VIDEO_H264))
        g_video_error = 1;
}
static void video_bitrate(void *arg, uint64_t generation, uint64_t request, uint32_t bps) {
    (void)arg; g_rate_generation = generation; g_rate_request = request; g_rate_bps = bps; g_rate_commands++;
}
static const struct call_video_ops video_ops = {
    .capabilities = video_caps, .receive_ready = video_ready, .frame = video_frame, .bitrate = video_bitrate, .need_key = video_need_key
};
static struct call_video_frame video_test_frame(uint64_t id, unsigned generation) {
    struct call_video_frame f = {0};
    f.call_id = id; f.frame_id = generation == 1 ? 1 : generation + 6;
    f.pts_us = sent_timing.pts_us + (int64_t)f.frame_id * 30000;
    f.format = (struct call_video_format){generation, 640, 480, 30, 0,
        generation == 1 ? CALL_VIDEO_H265 : CALL_VIDEO_H264, generation == 1 ? 93 : 31, 4000000, 1};
    f.flags = CALL_VIDEO_KEY; f.size = f.length = sizeof(g_video_data); f.data = g_video_data;
    return f;
}

static void media_recv_cb(struct UTUN_INSTANCE* inst, uint64_t call_id,
                          const uint8_t* opus, int len, void* arg) {
    (void)inst;
    struct media_evt* m = (struct media_evt*)arg;
    if (!m) return;
    m->frames++;
    m->last_call_id = call_id;
    m->last_len = len;
    if (len > 0) memcpy(m->last, opus, (size_t)(len < 64 ? len : 64));
}

struct call_shared {
    uint8_t  ch_x_pub[32], ch_ed_pub[32], ch_ed_priv[32], ch_sig[64];
    uint64_t nid[2];
    uint8_t  x_pub[2][32], seed[2][32], ed_pub[2][32];
};
static struct call_shared g_sh;

static int G_PASSED = 0, G_FAILED = 0, G_TOTAL = 0;
#define TEST(n)  do { G_TOTAL++; printf("  %-58s", n); fflush(stdout); } while(0)
#define OK()     do { G_PASSED++; printf("OK\n"); } while(0)
#define FAIL(f,...) do { G_FAILED++; printf("FAIL: " f "\n", ##__VA_ARGS__); } while(0)

/* Тип определяется по живым линкам, даже когда NCD остаётся UP при смене направления. */
static int test_transport_direction(void) {
    struct ETCP_CONN* conn = u_calloc(1, sizeof(*conn));
    struct ETCP_LINK incoming = {0}, outgoing = {0};
    if (!conn) return 0;
    conn->peer_node_id = 42; conn->peer_ed25519_pubkey[0] = 42; conn->links_up = 1; conn->links = &incoming;
    incoming.initialized = incoming.link_status = incoming.is_server = 1;
    outgoing.initialized = outgoing.link_status = 1; incoming.next = &outgoing;
    int ok = conn_mgr_connection_type(conn, conn->peer_ed25519_pubkey) == CONN_TYPE_DIRECT;
    outgoing.link_status = 0;
    ok &= conn_mgr_connection_type(conn, conn->peer_ed25519_pubkey) == CONN_TYPE_REVERSE;
    incoming.link_status = 0;
    ok &= conn_mgr_connection_type(conn, conn->peer_ed25519_pubkey) == CONN_TYPE_NONE;
    incoming.link_status = 1;
    ok &= conn_mgr_connection_type(conn, (const uint8_t[32]){43}) == CONN_TYPE_INDIRECT;
    conn->close_requested = 1;
    ok &= conn_mgr_connection_type(conn, (const uint8_t[32]){43}) == CONN_TYPE_NONE;
    u_free(conn); return ok;
}

/* ── крипто-хелперы ── */

static void to_hex(const uint8_t* bin, size_t n, char* out) {
    for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", bin[i]);
    out[n * 2] = '\0';
}

static int wf(const char* p, const char* f, ...) {
    va_list ap; FILE* fp = fopen(p, "w"); if (!fp) return -1;
    va_start(ap, f); vfprintf(fp, f, ap); va_end(ap); fclose(fp); return 0;
}

/* ── генерация данных сценария + конфигов ── */

static void fill_shared(void) {
    memset(&g_sh, 0, sizeof(g_sh));

    for (int i = 0; i < 2; i++) {
        uint8_t transport_secret[32];
        if (sc_identity_generate(g_sh.x_pub[i], transport_secret, g_sh.ed_pub[i], g_sh.seed[i])) abort();
        g_sh.nid[i] = sc_derive_node_id_from_pubkey(g_sh.x_pub[i]);
    }

    uint8_t x_private[32];
    if (chat_channel_generate_keys(g_sh.ch_x_pub, x_private, g_sh.ch_ed_pub, g_sh.ch_ed_priv)) abort();
    sc_public_key_hex(g_sh.ch_ed_pub, CH_ID);
    if (test_group_signature(CH_ID, "call-test", g_sh.ed_pub[IDX_A], g_sh.ch_x_pub, g_sh.ch_ed_pub, g_sh.ch_ed_priv, g_sh.ch_sig)) abort();
}

static void write_configs(const char* dir) {
    for (int i = 0; i < 2; i++) {
        char priv[65], pub[65], path[512], dbd[512];
        to_hex(g_sh.seed[i], 32, priv);
        to_hex(g_sh.x_pub[i], 32, pub);
        snprintf(path, sizeof(path), "%s/%s.conf", dir, i == IDX_A ? "a" : "b");
        snprintf(dbd, sizeof(dbd), "%s/db%s", dir, i == IDX_A ? "a" : "b");
        utun_mkdir(dbd, 0755);

        char client[512] = "";
        if (i == IDX_B) {
            /* B → A (клиент к A) */
            char apub[65]; to_hex(g_sh.x_pub[IDX_A], 32, apub);
            snprintf(client, sizeof(client),
                "[client: to_a]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", apub);
        }

        wf(path,
            "[global]\ntun_ip=10.97.%d.1/24\ntun_ifname=tun%d0\ndb_path=%s/db%s\n"
            "my_private_key=%s\nmy_public_key=%s\n"
            "[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
            "%s"
            "[chatserver]\nstorage_autoload=0\n[allowed_keys]\nallow_all=1\n",
            i, i, dir, i == IDX_A ? "a" : "b",
            priv, pub, client);
    }
}

/* ── событийный обработчик ── */

static void call_evt_handler(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    struct call_evt* ev = (struct call_evt*)inst->test_user_ptr;
    if (!ev) return;
    switch (type) {
    case CHAT_EVT_MEMBER_UPDATED: {
        struct chat_member_display member;
        if (len != (int)strlen(CH_ID) + 2 + CHAT_MEMBER_DISPLAY_SIZE || data[0] != strlen(CH_ID)
            || memcmp(data + 1, CH_ID, data[0]) || data[1 + data[0]] != 1) break;
        if (!chat_core_deserialize_member(data + 2 + data[0], &member) && !member.is_self && member.online)
            ev->peer_phone_flags = member.flags & (CHAT_MEMBER_FLAG_PHONE_KNOWN | CHAT_MEMBER_FLAG_PHONE_BUSY);
        break;
    }
    case CHAT_EVT_CALL_PHONE_BUSY:
        ev->phone_busy++;
        break;
    case CHAT_EVT_CALL_ERROR:
        ev->errors++;
        break;
    case CHAT_EVT_DM_CALL_ROUTE:
        if (len >= 66 && len == 65 + data[0]) {
            memcpy(ev->route_peer, data + 1 + data[0], 32);
            const struct TOPO_GROUP* group = topo_groups_find_wire(inst->topo_groups, data + 33 + data[0]);
            ev->route_group = group ? group->group_id : 0;
        }
        break;
    case CHAT_EVT_CALL_INCOMING: {
        if (len < 1 + 32 + 8) return;
        int ch_len = data[0];
        if (len != 1 + ch_len + 32 + 8) return;
        ev->incoming = 1;
        memcpy(&ev->incoming_call_id, data + 1 + ch_len + 32, 8);
        break;
    }
    case CHAT_EVT_CALL_RINGING:
        ev->ringing = 1;
        break;
    case CHAT_EVT_CALL_ACCEPTED:
        ev->accepted = 1;
        if (len >= 8) memcpy(&ev->accepted_call_id, data, 8);
        break;
    case CHAT_EVT_CALL_ENDED:
        ev->ended++;
        if (len >= 9) { memcpy(&ev->ended_call_id, data, 8); ev->ended_reason = data[8]; }
        break;
    case CHAT_EVT_CALL_DECLINED:
        ev->declined = 1;
        break;
    default:
        break;
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

/* ── настройка канала и мемберов ── */

static void channel_put_shared(struct UTUN_INSTANCE* inst, int has_priv) {
    topo_node_sqlite_channel_put(inst->topo_sqlite_db, CH_ID, "call-test", g_sh.ed_pub[IDX_A],
        g_sh.ch_x_pub, NULL, g_sh.ch_ed_pub, has_priv ? g_sh.ch_ed_priv : NULL, g_sh.ch_sig);
    chat_core_ensure_channel_ready(inst, CH_ID);
}

static void member_put_self(struct UTUN_INSTANCE* inst, const char* name) {
    char userinfo[256]; snprintf(userinfo, sizeof(userinfo), "{\"name\":\"%s\"}", name);
    if (test_member_seed(inst, CH_ID, inst->my_keys.public_key, inst->my_ed25519_pubkey,
        inst->my_ed25519_privkey, g_sh.ch_ed_priv, userinfo, NULL, NULL) < 0) abort();
}

static void member_put_placeholder(struct UTUN_INSTANCE* inst, int idx, const char* name) {
    char userinfo[256]; snprintf(userinfo, sizeof(userinfo), "{\"name\":\"%s\"}", name);
    const uint8_t* private = g_sh.seed[idx];
    if (test_member_seed(inst, CH_ID, g_sh.x_pub[idx], g_sh.ed_pub[idx], private,
        g_sh.ch_ed_priv, userinfo, NULL, NULL) < 0) abort();
}

static void do_setup(struct UTUN_INSTANCE* inst, int role) {
    switch (role) {
    case IDX_A:
        channel_put_shared(inst, 1);
        member_put_self(inst, "A");
        member_put_placeholder(inst, IDX_B, "B");
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_B]), CH_ID, NULL, NULL);
        break;
    case IDX_B:
        channel_put_shared(inst, 0);
        member_put_self(inst, "B");
        member_put_placeholder(inst, IDX_A, "A");
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_A]), CH_ID, NULL, NULL);
        break;
    }
}

static void pm_route_result(void* arg, const struct dm_call_route_result* result) {
    *(struct dm_call_route_result*)arg = *result;
}

/* Кнопка и запуск PM одинаково реагируют на потерю BGP, даже при живом ETCP. */
static int test_pm_route(struct UTUN_INSTANCE* inst, uint64_t peer, char target[64]) {
    char conv[24];
    snprintf(conv, sizeof(conv), "%llu", (unsigned long long)dm_derive_conv_id(inst->my_ed25519_pubkey, g_sh.ed_pub[IDX_B]));
    snprintf(target, 64, "dm:%s", conv);
    if (dm_start(inst, peer, g_sh.x_pub[IDX_B], g_sh.ed_pub[IDX_B], "B", CH_ID, DM_MODE_DEVICE, NULL)) return 0;
    uint64_t expected = topo_groups_channel_slot(inst->topo_groups, CH_ID);
    struct dm_call_route_result route = {0};
    dm_call_route_submit(inst, conv, 0, pm_route_result, &route);
    if (!route.available || memcmp(route.peer_ed, g_sh.ed_pub[IDX_B], 32) || route.group != expected) return 0;
    struct TOPO_GROUP* g = topo_groups_find(inst->topo_groups, expected);
    struct TOPO_GROUP_NODE* node = topo_node_find_by_id(g, peer);
    if (!node || !node->paths) return 0;
    struct ETCP_CONN* route_conn = topo_node_best_path(node)->conn;
    route_conn->close_requested = 1;
    dm_call_route_submit(inst, conv, 0, pm_route_result, &route);
    int closed_unavailable = !route.available && route.group == 0;
    route_conn->close_requested = 0;
    if (!closed_unavailable) return 0;
    struct ll_queue* paths = node->paths;
    node->paths = NULL;
    dm_call_route_submit(inst, conv, 0, pm_route_result, &route);
    int ok = !route.available && !memcmp(route.peer_ed, g_sh.ed_pub[IDX_B], 32) && route.group == 0;
    struct dm_messages_req* req = u_calloc(1, sizeof(*req));
    if (req) {
        req->inst = inst; snprintf(req->conv_id, sizeof(req->conv_id), "%s", conv);
        dm_call_route_trampoline(req);
        ok &= !memcmp(g_ev[IDX_A].route_peer, g_sh.ed_pub[IDX_B], 32) && g_ev[IDX_A].route_group == 0;
    } else ok = 0;
    int before = g_ev[IDX_A].errors;
    call_start(inst, target, g_sh.ed_pub[IDX_B], 123);
    ok &= g_ev[IDX_A].errors == before + 1;
    node->paths = paths;
    dm_call_route_submit(inst, conv, 0, pm_route_result, &route);
    ok &= route.available && route.group == expected;
    /* Сохранённая область недоступна: выбирается другой общий канал с живым BGP. */
    char sql[180];
    snprintf(sql, sizeof(sql), "UPDATE dm_conversations SET group_id='%s' WHERE conv_id='%s'", "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16", conv);
    ok &= sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK;
    dm_call_route_submit(inst, conv, 0, pm_route_result, &route);
    ok &= route.available && route.group == expected;
    before = g_ev[IDX_A].errors;
    call_start(inst, "dm:missing", g_sh.ed_pub[IDX_B], 124);
    /* Подмена ключа не должна позволять позвонить из чужой беседы. */
    uint8_t other[32]; memcpy(other, g_sh.ed_pub[IDX_B], sizeof(other)); other[0] ^= 1;
    call_start(inst, target, other, 126);
    ok &= g_ev[IDX_A].errors == before + 2;
    return ok;
}

/* ── master state-machine ── */

/* Проверяем вычисленный статус, локальный NODEINFO и доставленную BGP-копию у другого узла. */
static int published_status_matches(struct UTUN_INSTANCE* sender, struct UTUN_INSTANCE* receiver, uint8_t expected) {
    struct TOPO_GROUP* local = topo_groups_find_channel(sender->topo_groups, CH_ID);
    struct TOPO_GROUP* remote = topo_groups_find_channel(receiver->topo_groups, CH_ID);
    struct TOPO_GROUP_NODE* node = remote ? topo_node_find_by_id(remote, sender->node_id) : NULL;
    return call_phone_status(sender) == expected && local && local->local_node && local->local_node->phone_status == expected
        && node && node->phone_status == expected;
}

static void call_tick(void* arg) {
    struct call_test_ctx* t = arg;
    t->timer = NULL;
    if (t->result) return;

    struct UTUN_INSTANCE* A = t->inst[IDX_A];
    struct UTUN_INSTANCE* B = t->inst[IDX_B];
    uint64_t na = g_sh.nid[IDX_A], nb = g_sh.nid[IDX_B];
    struct call_evt* ea = &g_ev[IDX_A];
    struct call_evt* eb = &g_ev[IDX_B];

    switch (t->phase) {
    case P_WAIT_CONN:
        if (conn_up(A, nb) && conn_up(B, na)) t->phase = P_SETUP;
        break;
    case P_SETUP:
        do_setup(A, IDX_A);
        do_setup(B, IDX_B);
        t->phase = P_WAIT_BGP;
        break;
    case P_WAIT_BGP:
        if (bgp_has(A, nb) && bgp_has(B, na)) t->phase = P_STATUS_IDLE;
        break;
    case P_STATUS_IDLE:
        if (!published_status_matches(A, B, TOPO_FLAG_PHONE_KNOWN)) break;
        if (A->phone_state || call_phone_status(B)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "desktop idle status changed OS state or unknown phone status"); t->result = 2; break;
        }
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "desktop call status: idle published by default without OS phone state");
        t->phase = P_CALL1_START;
        break;
    case P_CALL1_START: {
        memset(ea, 0, sizeof(*ea));
        memset(eb, 0, sizeof(*eb));
        char target[64];
        if (!test_pm_route(A, nb, target)) {
            fprintf(stderr, "PM route availability/recheck failed\n"); t->result = 2; uasync_stop(t->ua); return;
        }
        t->call_id = ((uint64_t)1 << 40) | (uint64_t)(rand() & 0xFFFFF);
        call_start(A, target, B->my_ed25519_pubkey, t->call_id);
        t->phase = P_WAIT_INCOMING1;
        break;
    }
    case P_WAIT_INCOMING1:
        if (eb->incoming && eb->incoming_call_id == t->call_id) t->phase = P_ACCEPT;
        break;
    case P_ACCEPT:
        if (!published_status_matches(A, B, TOPO_FLAG_PHONE_KNOWN | TOPO_FLAG_PHONE_BUSY)
            || !published_status_matches(B, A, TOPO_FLAG_PHONE_KNOWN | TOPO_FLAG_PHONE_BUSY)) break;
        call_accept(B, t->call_id);
        t->phase = P_WAIT_ACCEPTED;
        break;
    case P_WAIT_ACCEPTED:
        if (ea->accepted && eb->accepted && ea->accepted_call_id == t->call_id) t->phase = P_STATUS_HIDE;
        break;
    case P_STATUS_HIDE:
        if (!published_status_matches(A, B, TOPO_FLAG_PHONE_KNOWN | TOPO_FLAG_PHONE_BUSY)) break;
        if (chat_setting_set(A, "incoming_call_policy", "16")) { t->result = 2; break; }
        t->phase = P_STATUS_RESTORE;
        break;
    case P_STATUS_RESTORE:
        if (!published_status_matches(A, B, 0)) break;
        if (!call_busy(A) || A->phone_state || A->incoming_call_policy != CALL_POLICY_BLOCK_RADIO) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "desktop status disable changed call/OS state or unrelated policy"); t->result = 2; break;
        }
        if (chat_setting_set(A, "incoming_call_policy", "17")) { t->result = 2; break; }
        t->phase = P_STATUS_RESTORED;
        break;
    case P_STATUS_RESTORED:
        if (!published_status_matches(A, B, TOPO_FLAG_PHONE_KNOWN | TOPO_FLAG_PHONE_BUSY)) break;
        if (ea->peer_phone_flags != (CHAT_MEMBER_FLAG_PHONE_KNOWN | CHAT_MEMBER_FLAG_PHONE_BUSY)
            || eb->peer_phone_flags != ea->peer_phone_flags) break;
        if (A->phone_state || A->incoming_call_policy != (CALL_POLICY_BLOCK_RADIO | CALL_POLICY_PUBLISH_PHONE)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "desktop status enable changed OS state or unrelated policy"); t->result = 2; break;
        }
        if (chat_setting_set(A, "incoming_call_policy", "1")) { t->result = 2; break; }
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "desktop call status: outgoing/incoming busy, runtime hide/restore delivered over BGP");
        t->phase = P_SEND_MEDIA;
        break;
    case P_SEND_MEDIA: {
        // Потеря CM-пути не завершает звонок; роутер продолжает доставку.
        struct TOPO_GROUP* g = topo_groups_find_channel(A->topo_groups, CH_ID);
        struct CONN_MGR_ENTRY* ce = cm_find_entry(g->conn_mgr, B->my_ed25519_pubkey);
        if (ce) {
            cm_deliver_event(ce, CONN_EVENT_DOWN);
            cm_deliver_event(ce, CONN_EVENT_TIMEOUT);
        }
        struct ETCP_CONN* conn = instance_find_conn(A, nb);
        if (conn) {
            etcp_fire_conn_status(conn, ETCP_CONN_STATUS_DOWN);
            etcp_fire_conn_status(conn, ETCP_CONN_STATUS_UP);
            /* Синтетический ETCP callback не переводит NCD через DOWN/UP: восстановить CHAT-сессию явно. */
            if (topo_group_new_conn(g, conn)) { t->result = 2; break; }
        }
        if (ea->ended || eb->ended) { fprintf(stderr, "temporary path loss ended call\n"); t->result = 2; break; }

        /* тестовый «Opus-кадр» — релей произвольных байт, не валидируем кодек */
        for (int i = 0; i < 2; i++) {
            struct call_session *cs = call_session_find((i ? B : A)->call, t->call_id);
            if (cs->audio_batch_timer) uasync_cancel_timeout(t->ua, cs->audio_batch_timer);
            cs->audio_batch_timer = NULL; cs->audio_batch.target = 1;
        }
        static const uint8_t opus[16] = { 0xf8, 0xff, 0xfe, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d };
        struct call_capture_time invalid = sent_timing;
        invalid.delay_samples = 48001;
        if (call_send_media(A, t->call_id, opus, sizeof(opus), &invalid) == 0) {
            fprintf(stderr, "A: invalid codec delay accepted\n"); t->result = 2; break;
        }
        if (call_send_media(A, t->call_id, opus, sizeof(opus), &sent_timing) != 0) {
            fprintf(stderr, "A: call_send_media failed\n");
            t->result = 2;
        } else {
            t->phase = P_WAIT_MEDIA;
        }
        break;
    }
    case P_WAIT_MEDIA: {
        /* Инъекция callbacks не создаёт нового NCD UP. После штатного backoff доводим запрос до READY. */
        struct TOPO_GROUP* group = topo_groups_find_channel(A->topo_groups, CH_ID);
        struct ETCP_CONN* conn = instance_find_conn(A, nb);
        if (conn && !topo_group_peer_ready(group, nb)) topo_group_new_conn(group, conn);
        if (g_media[IDX_B].frames > 0 && g_media[IDX_B].last_call_id == t->call_id
            && g_media[IDX_B].last_len == 16) {
            if (g_media[IDX_B].frames != 1 || g_capture_timing.pts_us != sent_timing.pts_us
                || g_capture_timing.epoch != sent_timing.epoch || g_capture_timing.delay_samples != sent_timing.delay_samples) {
                fprintf(stderr, "B: capture timing changed in transit\n"); t->result = 2; break;
            }
            t->phase = P_BATCH_SEND;
        }
        break;
    }
    case P_BATCH_SEND: {
        struct call_session *cs = call_session_find(A->call, t->call_id);
        cs->audio_batch.target = 10;
        struct ETCP_CONN *transport = instance_find_conn(A, nb);
        if (!transport) { t->result = 2; break; }
        queue_set_threshold(transport->send_input_q, -1, 0);
        t->phase_ticks = 0;
        uint8_t opus[16] = {0};
        for (unsigned i = 1; i <= 10; i++) {
            struct call_capture_time timing = sent_timing; timing.pts_us += i * 20000;
            if (call_send_media(A, t->call_id, opus, sizeof(opus), &timing)) { t->result = 2; break; }
        }
        if (cs->tx_batches != 1 || cs->c_tx_frames != 1 || !cs->audio_send.conn || cs->tx_q->count != 10
            || cs->audio_send.conn->send_q->count) {
            fprintf(stderr, "audio packed before transport readiness\n"); t->result = 2;
        }
        t->phase = P_BATCH_WAIT; break;
    }
    case P_BATCH_WAIT:
        if (++t->phase_ticks <= 10) {
            struct call_session *cs = call_session_find(A->call, t->call_id);
            if (cs->tx_q->count != 10 || cs->tx_batches != 1 || !cs->audio_send.conn
                || cs->audio_send.conn->send_q->count) { t->result = 2; break; }
            if (t->phase_ticks == 10) queue_set_threshold(instance_find_conn(A, nb)->send_input_q, 0, 0);
            break;
        }
        if (g_media[IDX_B].frames >= 11) {
            if (g_media[IDX_B].frames != 11 || g_audio_packets != 2 || g_capture_timing.pts_us != sent_timing.pts_us + 200000) {
                fprintf(stderr, "batch frames/timing not preserved\n"); t->result = 2; break;
            }
            struct call_session *cs = call_session_find(A->call, t->call_id);
            cs->audio_batch.target = 4; t->phase_ticks = 0;
            uint8_t opus[16] = {0};
            for (unsigned i = 11; i <= 13; i++) {
                struct call_capture_time timing = sent_timing; timing.pts_us += i * 20000;
                if (call_send_media(A, t->call_id, opus, sizeof(opus), &timing)) { t->result = 2; break; }
            }
            if (cs->tx_batches != 2 || cs->audio_send.conn || cs->tx_q->count != 3) {
                fprintf(stderr, "partial batch requested transport before target\n"); t->result = 2;
            }
            t->phase = P_PARTIAL_WAIT;
        }
        break;
    case P_PARTIAL_WAIT:
        if (++t->phase_ticks <= 10) {
            struct call_session *cs = call_session_find(A->call, t->call_id);
            if (cs->tx_batches != 2 || cs->tx_q->count != 3 || cs->audio_send.conn || g_media[IDX_B].frames != 11) {
                fprintf(stderr, "partial batch escaped while transport was free\n"); t->result = 2; break;
            }
            if (t->phase_ticks == 10) {
                uint8_t opus[16] = {0};
                struct call_capture_time timing = sent_timing; timing.pts_us += 280000;
                if (call_send_media(A, t->call_id, opus, sizeof(opus), &timing) || !cs->audio_send.conn) t->result = 2;
            }
            break;
        }
        if (g_media[IDX_B].frames >= 15) {
            struct call_session *cs = call_session_find(A->call, t->call_id);
            if (g_media[IDX_B].frames != 15 || g_audio_packets != 3 || cs->tx_batches != 3 || cs->audio_send.conn
                || g_capture_timing.pts_us != sent_timing.pts_us + 280000) {
                fprintf(stderr, "full batch ready delivery failed\n"); t->result = 2; break;
            }
            cs->audio_batch.target = 1;
            t->phase = P_VIDEO_CONFIG;
        }
        break;
    case P_VIDEO_CONFIG: {
        if (strcmp(chat_setting_get(A, "call_video_bitrate_range_kbps"), "300:4000")
            || chat_setting_set(A, "call_video_bitrate_range_kbps", "4000:300") != -2
            || strcmp(chat_setting_get(A, "call_video_bitrate_range_kbps"), "300:4000")) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video range default/atomic validation failed"); t->result = 2; break;
        }
        for (unsigned i = 4; i < sizeof(g_video_data); i++) g_video_data[i] = (uint8_t)i;
        struct call_video_caps caps = {0};
        caps.encode[0] = caps.encode[1] = caps.decode[0] = caps.decode[1]
            = (struct call_video_limit){1280, 720, 30, 1280 * 720 * 30, 4000000, 1, 31};
        caps.encode[1].level = caps.decode[1].level = 93;
        if (call_video_configure(A, t->call_id, &caps, &video_ops, (void*)0)
            || call_video_configure(B, t->call_id, &caps, &video_ops, (void*)1)) { t->result = 2; break; }
        g_video_ready = 0; t->phase = P_VIDEO_SEND; break;
    }
    case P_VIDEO_SEND: {
        if (g_video_peer[0].decode[1].width != 1280 || g_video_peer[1].decode[1].width != 1280) break;
        struct call_video_frame f = video_test_frame(t->call_id, 1);
        if (call_video_configure_rate(A, t->call_id, &f.format, 800000, 400000)) { t->result = 2; break; }
        struct call_video_frame invalid = f; invalid.format.profile = 2;
        if (call_video_send_frame(A, &invalid) != -1
            || call_video_configure_rate(A, t->call_id, &invalid.format, 800000, 400000) != -1) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video accepted unsupported profile"); t->result = 2; break;
        }
        int result = call_video_send_frame(A, &f);
        if (result < 0) { fprintf(stderr, "video send failed\n"); t->result = 2; break; }
        if (!result) break;
        for (unsigned i = 2; i <= 7; i++) {
            f.frame_id = i; f.pts_us = sent_timing.pts_us + i * 30000; f.flags = 0;
            if (call_video_send_frame(A, &f) != 1) {
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video could not queue dependent frame %u", i); t->result = 2; break;
            }
        }
        g_video_keys[0] = 0;
        call_video_request_key(B, t->call_id);
        t->phase_ticks = 0; t->phase = P_VIDEO_BUSY; break;
    }
    case P_VIDEO_BUSY:
        if (g_video_frames || g_video_preroll) { fprintf(stderr, "video bypassed receive backpressure\n"); t->result = 2; break; }
        if (++t->phase_ticks < 250 || !g_video_keys[0]) break;
        g_video_ready = 1; call_video_resume_receive(B, t->call_id); t->phase = P_VIDEO_RESUME; break;
    case P_VIDEO_RESUME:
        if (!g_video_frames || g_video_preroll != 6 || !g_rate_commands) break;
        if (g_video_error || g_media[IDX_B].frames != 15) { t->result = 2; break; }
        // Настройка меняется при незавершённой команде; старый ACK остаётся действительным.
        if (chat_setting_set(A, "call_video_bitrate_range_kbps", "300:350")) { t->result = 2; break; }
        if (g_rate_commands != 1 || g_rate_generation != 1 || g_rate_bps != 400000
            || call_video_rate_applied(A, t->call_id, 2, g_rate_request, g_rate_bps) != -1
            || call_video_rate_applied(A, t->call_id, 1, g_rate_request + 1, g_rate_bps) != -1
            || call_video_rate_applied(A, t->call_id, 1, g_rate_request, g_rate_bps)) {
            fprintf(stderr, "video rate command/completion mismatch\n"); t->result = 2; break;
        }
        t->phase = P_VIDEO_RANGE_WAIT; break;
    case P_VIDEO_RANGE_WAIT: {
        if (g_rate_commands < 2) break;
        if (g_rate_commands != 2 || g_rate_generation != 1 || g_rate_bps != 350000
            || call_video_rate_applied(A, t->call_id, 1, g_rate_request, g_rate_bps)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video live range change did not converge to 350kbps"); t->result = 2; break;
        }
        struct call_video_frame next = video_test_frame(t->call_id, 2);
        if (chat_setting_set(A, "call_video_bitrate_range_kbps", "500:1000")
            || call_video_configure_rate(A, t->call_id, &next.format, 350000, 400000) != -1
            || chat_setting_set(A, "call_video_bitrate_range_kbps", "300:350")) { t->result = 2; break; }
        t->phase = P_VIDEO_SWITCH; break;
    }
    case P_VIDEO_SWITCH: {
        struct call_video_frame f = video_test_frame(t->call_id, 2);
        if (call_video_configure_rate(A, t->call_id, &f.format, 350000, 400000)
            || call_video_rate_applied(A, t->call_id, 1, g_rate_request, g_rate_bps) != -1) { t->result = 2; break; }
        if (call_video_send_frame(A, &f) != 1) { t->result = 2; break; }
        t->phase = P_VIDEO_SWITCH_WAIT; break;
    }
    case P_VIDEO_SWITCH_WAIT:
        if (g_video_frames < 2) break;
        if (g_video_error) { t->result = 2; break; }
        // Завершение во время незавершённой сборки/паузы приёма не оставляет callbacks.
        g_video_ready = 0;
        {
            struct call_video_frame f = video_test_frame(t->call_id, 3);
            if (call_video_configure_rate(A, t->call_id, &f.format, 350000, 400000)) { t->result = 2; break; }
            if (call_video_send_frame(A, &f) != 1) { t->result = 2; break; }
            struct call_video_caps caps = g_video_peer[0]; caps.decode[0].maximum_bps = 3000000;
            if (call_video_configure(B, t->call_id, &caps, &video_ops, (void*)1)) { t->result = 2; break; }
        }
        t->phase = P_VIDEO_SHRINK_WAIT; break;
    case P_VIDEO_SHRINK_WAIT: {
        if (g_video_peer[0].decode[0].maximum_bps != 3000000) break;
        if (g_video_frames != 2) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video escaped changed peer envelope"); t->result = 2; break; }
        struct call_video_frame f = video_test_frame(t->call_id, 3); f.format.maximum_bps = 3000000;
        if (call_video_configure_rate(A, t->call_id, &f.format, 350000, 400000) != -1) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video envelope changed without new generation"); t->result = 2; break;
        }
        f = video_test_frame(t->call_id, 4); f.format.maximum_bps = 3000000;
        if (call_video_configure_rate(A, t->call_id, &f.format, 350000, 400000) || call_video_send_frame(A, &f) != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video failed to replace withdrawn envelope"); t->result = 2; break;
        }
        if (call_video_port_attach(B) || call_video_port_open(t->call_id, &g_video_peer[0])) { t->result = 2; break; }
        t->phase = P_VIDEO_PORT_OPEN; break;
    }
    case P_VIDEO_PORT_OPEN: {
        struct call_video_port_status status;
        if (call_video_port_status(t->call_id, &status) || !status.have_peer) break;
        t->phase = P_VIDEO_PORT_RECEIVE; break;
    }
    case P_VIDEO_PORT_RECEIVE: {
        struct call_video_frame received;
        if (!call_video_port_pull(t->call_id, &received)) break;
        int valid = received.frame_id == 10 && received.format.maximum_bps == 3000000
            && received.size == sizeof(g_video_data) && !memcmp(received.data, g_video_data, sizeof(g_video_data));
        u_free((void *)received.data);
        if (!valid) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port RX ownership/metadata mismatch"); t->result = 2; break; }
        struct call_video_ops callbacks = video_ops; callbacks.frame = port_received;
        if (call_video_configure(A, t->call_id, &g_video_peer[1], &callbacks, (void *)0)) { t->result = 2; break; }
        struct call_video_frame f = video_test_frame(t->call_id, 1);
        if (call_video_port_range(t->call_id, 300000, 500000) || call_video_port_format(t->call_id, &f.format, 500000)) {
            t->result = 2; break;
        }
        g_video_ready = 1; t->phase = P_VIDEO_PORT_SEND; break;
    }
    case P_VIDEO_PORT_SEND: {
        struct call_video_port_status status;
        if (call_video_port_status(t->call_id, &status) || status.configured_generation != 1) break;
        struct call_video_frame f = video_test_frame(t->call_id, 1);
        struct call_video_frame batch[3] = {f, f, f};
        for (unsigned i = 1; i < 3; ++i) { batch[i].frame_id += i; batch[i].pts_us += i * 33333; batch[i].flags = 0; }
        ++batch[1].call_id;
        if (call_video_port_send(batch, 3) != -1) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port accepted mixed-call batch"); t->result = 2; break;
        }
        --batch[1].call_id;
        if (status.minimum_bps != 300000 || status.maximum_bps != 500000
            || call_video_port_send(batch, 3) != 1 || call_video_port_send(batch, 3) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port TX mailbox/range mismatch"); t->result = 2; break;
        }
        // B ещё не запускал аудиопередачу. Видео проходит только по своей готовности.
        t->phase = P_VIDEO_PORT_ACK; break;
    }
    case P_VIDEO_PORT_ACK: {
        if (!g_port_frame) break;
        if (g_port_frame < 0) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port TX payload mismatch"); t->result = 2; break; }
        if (g_port_frame != 3) break;
        if (call_video_decode_result(A, t->call_id, 1, 1)
            || call_video_decode_result(A, t->call_id, 2, 0) != -1) { t->result = 2; break; }
        t->phase = P_VIDEO_DECODE_OK; break;
    }
    case P_VIDEO_DECODE_OK: {
        struct call_video_port_status status;
        if (call_video_port_status(t->call_id, &status) || status.decoded_generation != 1) break;
        if (!status.decoded_accepted || call_video_decode_result(A, t->call_id, 1, 0)) { t->result = 2; break; }
        t->phase_ticks = 0; t->phase = P_VIDEO_DECODE_REJECT; break;
    }
    case P_VIDEO_DECODE_REJECT: {
        struct call_video_port_status status;
        if (call_video_port_status(t->call_id, &status) || status.decoded_generation != 1 || status.decoded_accepted) break;
        if (++t->phase_ticks < 10) break; // Регулятор не должен отменять запрет decoder на следующем tick.
        struct call_video_frame f = video_test_frame(t->call_id, 1); f.frame_id = 4; f.pts_us += 99999;
        if (call_video_send_frame(B, &f) != -1) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video ignored decoder rejection"); t->result = 2; break; }
        // Все команды попадут в uasync после синхронного ENDED; TX ещё принадлежит mailbox.
        struct call_video_frame closing[2] = {f, f}; ++closing[1].frame_id; closing[1].pts_us += 33333;
        if (call_video_port_format(t->call_id, &f.format, 500000)
            || call_video_port_applied(t->call_id, 1, 1, 500000)
            || call_video_port_decoded(t->call_id, 1, 1) || call_video_port_send(closing, 2) != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video teardown commands were not queued"); t->result = 2; break;
        }
        call_video_port_request_key(t->call_id);
        t->previous_call_id = t->call_id;
        g_drop_acks[IDX_B] = 2;
        struct call_session *pending_audio = call_session_find(B->call, t->call_id);
        uint8_t unfinished_opus[2] = {1, 2};
        if (call_send_media(B, t->call_id, unfinished_opus, sizeof(unfinished_opus), &sent_timing)
            || !pending_audio->audio_send.conn) { t->result = 2; break; }
        call_hangup(B, t->call_id);
        struct call_session* ended = call_session_find(B->call, t->call_id);
        if (!ended || ended->tx_q || ended->video || ended->audio_send.conn || ended->audio_batch_timer
            || !ended->end_subcmd || !ended->teardown_timer) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "ended call retained media or lost final command"); t->result = 2; break;
        }
        t->phase = P_VIDEO_PORT_CANCELLED; break;
    }
    case P_VIDEO_PORT_CANCELLED: {
        struct call_video_port_status status;
        struct call_video_frame f = video_test_frame(t->call_id, 1);
        if (call_video_port_status(t->call_id, &status) != -1 || call_video_port_send(&f, 1) != -1 || g_port_frame != 3) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "ended video port retained commands or accepted a late frame"); t->result = 2; break;
        }
        call_video_port_close(t->call_id); // Повторное закрытие после отмены команд.
        if (call_video_port_open(t->call_id, &g_video_peer[0])) { t->result = 2; break; }
        t->phase = P_VIDEO_PORT_REOPENED; break;
    }
    case P_VIDEO_PORT_REOPENED: {
        struct call_video_port_status status;
        if (call_video_port_status(t->call_id, &status) != -1) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "late open resurrected an ended video session"); t->result = 2; break;
        }
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "video teardown regression: queued TX/configuration and late open cancelled");
        t->phase = P_WAIT_ENDED1; break;
    }
    case P_WAIT_ENDED1:
        if (ea->ended && eb->ended && ea->ended_call_id == t->call_id) {
            if (ea->ended != 1 || eb->ended != 1 || ea->ended_reason != CALL_REASON_REMOTE_HANGUP
                || eb->ended_reason != CALL_REASON_LOCAL_HANGUP) {
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video teardown: HANGUP delivery failed caller_reason=%u callee_reason=%u",
                            ea->ended_reason, eb->ended_reason);
                t->result = 2; break;
            }
            t->phase = P_STATUS_ENDED;
        }
        break;
    case P_STATUS_ENDED:
        if (!published_status_matches(A, B, TOPO_FLAG_PHONE_KNOWN) || !published_status_matches(B, A, 0)) break;
        if (ea->peer_phone_flags || eb->peer_phone_flags != CHAT_MEMBER_FLAG_PHONE_KNOWN) break;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "hangup: member events delivered free desktop and unknown phone to the two peers");
        t->phase = P_CALL2_START;
        break;
    case P_CALL2_START: {
        memset(ea, 0, sizeof(*ea));
        memset(eb, 0, sizeof(*eb));
        char target[64];
        if (!test_pm_route(A, nb, target)) {
            fprintf(stderr, "PM route availability/recheck failed\n"); t->result = 2; uasync_stop(t->ua); return;
        }
        ++t->call_id;
        call_start(A, target, B->my_ed25519_pubkey, t->call_id);
        t->phase = P_WAIT_INCOMING2;
        break;
    }
    case P_WAIT_INCOMING2:
        if (eb->incoming && eb->incoming_call_id == t->call_id) {
            if (t->video_restart_tested) t->phase = P_DECLINE;
            else { call_accept(B, t->call_id); t->phase = P_VIDEO_NEW_CALL; }
        }
        break;
    case P_VIDEO_NEW_CALL: {
        if (!ea->accepted || !eb->accepted) break;
        if (call_video_configure(A, t->call_id, &g_video_peer[1], &video_ops, (void *)0)
            || call_video_port_open(t->call_id, &g_video_peer[0])) { t->result = 2; break; }
        call_video_port_close(t->previous_call_id);
        t->phase = P_VIDEO_NEW_PORT; break;
    }
    case P_VIDEO_NEW_PORT: {
        struct call_video_port_status status;
        if (call_video_port_status(t->call_id, &status) || status.error) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "late close affected the new video port"); t->result = 2; break;
        }
        if (!status.have_peer) break;
        // Новый звонок работает, пока старый повторяет HANGUP после двух потерянных END_ACK.
        if (call_session_find(A->call, t->previous_call_id) || call_session_find(B->call, t->previous_call_id)) break;
        if (g_dropped_acks[IDX_B] != 2 || ea->ended || eb->ended) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "lost END_ACK affected the new call"); t->result = 2; break;
        }
        struct call_pkt_hangup stale = {CALL_SUBCMD_HANGUP, t->previous_call_id, CALL_REASON_LOCAL_HANGUP};
        struct ETCP_ROUTER_CONN* rc = etcp_router_conn_get(B, topo_groups_channel_slot(B->topo_groups, CH_ID), na, ETCP_RT_ID_CALL);
        if (!rc || etcp_router_conn_send(rc, (const uint8_t*)&stale, sizeof(stale), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "cannot send stale HANGUP"); t->result = 2; break;
        }
        t->phase_ticks = 0; t->phase = P_STALE_END; break;
    }
    case P_STALE_END: {
        if (++t->phase_ticks < 30) break;
        struct call_video_port_status status;
        if (ea->ended || eb->ended || call_video_port_status(t->call_id, &status) || status.error) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "stale HANGUP affected the new call"); t->result = 2; break;
        }
        call_video_port_request_key(t->call_id);
        call_video_port_close(t->call_id);
        call_video_port_detach(B); // Отменяет уже поставленный post до destroy ядра.
        if (call_video_port_status(t->call_id, &status) != -1) { t->result = 2; break; }
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "video teardown regression: new call survives stale close; detach cancels post");
        t->video_restart_tested = 1;
        call_hangup(A, t->call_id); call_hangup(B, t->call_id);
        t->phase = P_WAIT_ENDED2; break;
    }
    case P_DECLINE:
        call_decline(B, t->call_id);
        t->phase = P_WAIT_ENDED2;
        break;
    case P_WAIT_ENDED2:
        if (!ea->ended || !eb->ended || call_session_find(A->call, t->call_id) || call_session_find(B->call, t->call_id)) break;
        if (ea->ended != 1 || eb->ended != 1
            || ea->ended_reason != (ea->declined ? CALL_REASON_DECLINE : CALL_REASON_LOCAL_HANGUP)
            || eb->ended_reason != ea->ended_reason) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "duplicate end or wrong reason after simultaneous hangup/decline"); t->result = 2; break;
        }
        t->phase = ea->declined ? P_TIMEOUT_START : P_CALL2_START;
        break;
    case P_TIMEOUT_START:
        memset(ea, 0, sizeof(*ea)); memset(eb, 0, sizeof(*eb));
        call_start(A, CH_ID, B->my_ed25519_pubkey, ++t->call_id);
        t->phase = P_TIMEOUT_INCOMING;
        break;
    case P_TIMEOUT_INCOMING:
        if (!eb->incoming) break;
        call_accept(B, t->call_id); t->phase = P_TIMEOUT_ACTIVE;
        break;
    case P_TIMEOUT_ACTIVE: {
        if (!ea->accepted || !eb->accepted) break;
        call_set_phone_state(B, 2, CALL_POLICY_PUBLISH_PHONE); /* Системный звонок продолжается после отбоя A. */
        g_drop_acks[IDX_A] = -1;
        call_hangup(A, t->call_id);
        struct call_session* s = call_session_find(A->call, t->call_id);
        if (!s || !s->end_subcmd) { t->result = 2; break; }
        // Сдвигаем только начало ожидания: проверяем deadline без двадцатисекундного sleep.
        s->end_started_tb -= CALL_END_TIMEOUT_TB;
        t->phase = P_TIMEOUT_ENDED; break;
    }
    case P_TIMEOUT_ENDED:
        if (call_session_find(A->call, t->call_id) || call_session_find(B->call, t->call_id)) break;
        if (ea->ended != 1 || eb->ended != 1 || !g_dropped_acks[IDX_A]) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "unconfirmed end did not release sessions cleanly"); t->result = 2; break;
        }
        if (eb->peer_phone_flags != CHAT_MEMBER_FLAG_PHONE_KNOWN
            || ea->peer_phone_flags != (CHAT_MEMBER_FLAG_PHONE_KNOWN | CHAT_MEMBER_FLAG_PHONE_BUSY)) break;
        call_set_phone_state(B, 0, CALL_POLICY_PUBLISH_PHONE);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call end regression: peer is free after caller hangup; ongoing OS call remains busy");
        t->phase = P_POLICY_START;
        break;
    case P_POLICY_START: {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "phone policy starting scenario=%d", t->policy_index);
        g_drop_acks[IDX_A] = g_drop_acks[IDX_B] = 0;
        static const struct { int state; unsigned policy; } policies[] = {
            {0, CALL_POLICY_WARN_PHONE}, {2, CALL_POLICY_PUBLISH_PHONE}, {2, CALL_POLICY_WARN_PHONE},
            {2, CALL_POLICY_WARN_PHONE}, {2, CALL_POLICY_WARN_PHONE | CALL_POLICY_REJECT_PHONE},
            {0, CALL_POLICY_BLOCK_CALLS}, {2, CALL_POLICY_BLOCK_CALLS | CALL_POLICY_WARN_PHONE},
            {2, CALL_POLICY_BLOCK_RADIO}, {2, CALL_POLICY_WARN_PHONE}, {2, CALL_POLICY_WARN_PHONE}
        };
        if (t->policy_index == (int)(sizeof(policies) / sizeof(policies[0]))) {
            call_set_phone_state(B, 0, CALL_POLICY_PUBLISH_PHONE);
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "phone policy passed: independent flags, no incoming UI before choice, override, cancel, rejection, deadline");
            t->phase = P_DONE; break;
        }
        memset(ea, 0, sizeof(*ea)); memset(eb, 0, sizeof(*eb));
        call_set_phone_state(B, policies[t->policy_index].state, policies[t->policy_index].policy);
        ++t->call_id;
        call_start(A, CH_ID, B->my_ed25519_pubkey, t->call_id);
        t->phase = P_POLICY_WAIT; break;
    }
    case P_POLICY_WAIT:
        if (t->policy_index == 4 || t->policy_index == 5 || t->policy_index == 6) {
            if (!ea->ended) break;
            int expected = t->policy_index == 4 ? CALL_REASON_PHONE_BUSY : CALL_REASON_POLICY;
            if (eb->incoming || ea->phone_busy || ea->ended_reason != expected) goto policy_failed;
            t->phase = P_POLICY_END; break;
        }
        if (t->policy_index == 2 || t->policy_index == 3 || t->policy_index >= 8) {
            if (!ea->phone_busy) break;
            if (eb->incoming || ea->ringing || ea->phone_busy != 1) goto policy_failed;
            if (t->policy_index == 2) {
                call_hangup(A, t->call_id); t->phase = P_POLICY_END;
            } else if (t->policy_index == 8) {
                struct call_session* s = call_session_find(A->call, t->call_id);
                s->started_tb -= CALL_RING_TIMEOUT_TB;
                call_continue(A, t->call_id);
                if (ea->ended_reason != CALL_REASON_RING_TIMEOUT) goto policy_failed;
                t->phase = P_POLICY_END;
            } else {
                if (t->policy_index == 9) call_set_phone_state(B, 2, CALL_POLICY_BLOCK_CALLS | CALL_POLICY_WARN_PHONE);
                call_continue(A, t->call_id);
                t->phase = t->policy_index == 9 ? P_POLICY_END : P_POLICY_RING;
            }
        } else t->phase = P_POLICY_RING;
        break;
    case P_POLICY_RING:
        if (!eb->incoming) break;
        if (eb->incoming_call_id != t->call_id || (t->policy_index != 3 && ea->phone_busy)) goto policy_failed;
        call_decline(B, t->call_id); t->phase = P_POLICY_END; break;
    case P_POLICY_END:
        if (!ea->ended || !eb->ended || call_session_find(A->call, t->call_id) || call_session_find(B->call, t->call_id)) break;
        if (ea->ended != 1 || eb->ended != 1 || (t->policy_index == 9 && (eb->incoming || ea->ended_reason != CALL_REASON_POLICY)))
            goto policy_failed;
        call_continue(A, t->call_id); // Поздняя команда не должна создавать новую сессию.
        ++t->policy_index; t->phase = P_POLICY_START; break;
    policy_failed:
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "phone policy failed scenario=%d incoming=%d busy_events=%d reason=%u",
                    t->policy_index, eb->incoming, ea->phone_busy, ea->ended_reason);
        t->result = 2; break;
    case P_DONE:
        t->result = 1;
        uasync_stop(t->ua);
        return;
    }

    if (t->phase == P_DONE) { t->result = 1; uasync_stop(t->ua); return; }
    if (t->result == 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call test failed in phase=%d", t->phase); uasync_stop(t->ua); return;
    }

    if (++t->ticks > MAX_TICKS) {
        fprintf(stderr, "test: timeout in phase %d (A_conn=%d B_conn=%d evA:inc=%d acc=%d end=%d dec=%d evB:inc=%d acc=%d end=%d dec=%d)\n",
                (int)t->phase, conn_up(A, nb), conn_up(B, na),
                ea->incoming, ea->accepted, ea->ended, ea->declined,
                eb->incoming, eb->accepted, eb->ended, eb->declined);
        t->result = 2;
        uasync_stop(t->ua);
        return;
    }
    t->timer = uasync_set_timeout(t->ua, TICK_TB, t, call_tick, "call_tick");
}

/* ── main ── */

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    test_clock_init(TICK_TB);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level_by_name("call", "trace");
        debug_set_category_level_by_name("bgp", "info");
        debug_set_category_level_by_name("etcp_route", "trace");
        debug_set_category_level_by_name("member_sync", "info");
        debug_set_category_level_by_name("chat_sync", "info");
    }
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));

    char dir[512]; snprintf(dir, sizeof(dir), "/tmp/utun_call_XXXXXX");
    if (test_mkdtemp(dir) != 0) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
    test_setenv("UTUN_TEST_DIR", dir, 1);

    fill_shared();
    write_configs(dir);

    printf("=== test_call ===\n"); fflush(stdout);

    TEST("live transport: mixed links, reverse, none, intermediary");
    if (test_transport_direction()) OK(); else FAIL("incorrect transport direction");

    TEST("PM BGP availability/recheck + audio call + decline"); {
        struct UASYNC* ua = uasync_create();
        if (!ua) { FAIL("uasync_create failed"); return 1; }

        struct call_test_ctx t;
        memset(&t, 0, sizeof(t));
        t.ua = ua;
        t.phase = P_WAIT_CONN;

        char cfgA[512], cfgB[512];
        snprintf(cfgA, sizeof(cfgA), "%s/a.conf", dir);
        snprintf(cfgB, sizeof(cfgB), "%s/b.conf", dir);

        t.inst[IDX_A] = utun_instance_create(ua, cfgA);
        t.inst[IDX_B] = utun_instance_create(ua, cfgB);
        if (!t.inst[IDX_A] || !t.inst[IDX_B]) {
            FAIL("instance create failed A=%p B=%p", (void*)t.inst[IDX_A], (void*)t.inst[IDX_B]);
            uasync_destroy(ua, 0);
            return 1;
        }
        test_etcp_set_peer(t.inst[IDX_B], "to_a", t.inst[IDX_A], "s1");
        t.inst[IDX_A]->config->global.client_type = CLIENT_TYPE_DESKTOP;
        utun_instance_init(t.inst[IDX_A]);
        utun_instance_init(t.inst[IDX_B]);

        t.inst[IDX_A]->test_user_ptr = &g_ev[IDX_A];
        t.inst[IDX_B]->test_user_ptr = &g_ev[IDX_B];
        for (int i = 0; i < 2; ++i) {
            g_signal_recv[i] = t.inst[i]->router_bindings.callbacks[ETCP_RT_ID_CALL];
            etcp_router_unbind(t.inst[i], ETCP_RT_ID_CALL);
            etcp_router_bind(t.inst[i], ETCP_RT_ID_CALL, signal_recv_loss);
            if (etcp_router_require_crypto(t.inst[i], ETCP_RT_ID_CALL, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)) return 1;
        }
        chat_event_set_handler(t.inst[IDX_A], call_evt_handler);
        chat_event_set_handler(t.inst[IDX_B], call_evt_handler);
        call_set_media_recv_cb(t.inst[IDX_A], media_recv_cb, &g_media[IDX_A]);
        call_set_media_recv_cb(t.inst[IDX_B], media_recv_cb, &g_media[IDX_B]);
        call_set_audio_ops(t.inst[IDX_B], &audio_ops);

        t.timer = uasync_set_timeout(ua, TICK_TB, &t, call_tick, "call_tick");
        test_clock_mainloop(ua);

        int ok = (t.result == 1);

        if (t.inst[IDX_A]) utun_instance_destroy(t.inst[IDX_A]);
        if (t.inst[IDX_B]) { call_video_port_detach(t.inst[IDX_B]); utun_instance_destroy(t.inst[IDX_B]); }
        uasync_poll(ua, 0);
        uasync_destroy(ua, 0);

        if (ok) OK(); else FAIL("result=%d phase=%d", t.result, (int)t.phase);
    }

    char cmd[512]; snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); (void)!system(cmd);

    printf("\n%d/%d passed, %d failed\n", G_PASSED, G_TOTAL, G_FAILED);
    test_clock_finish();
    return G_FAILED > 0 ? 1 : 0;
}
