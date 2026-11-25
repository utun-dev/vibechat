// test_radio_mesh.c — интеграционный тест рации: дедупликация дублей в mesh (diamond).
//
// Однопоточный: 4 узла (A, B, D, C) в одном процессе, один UASYNC, без fork.
// Топология — diamond: A↔B, A↔D, B↔C, D↔C (A сервер, B и D — клиенты к A +
// серверы, C — клиент к B и к D). Все четыре в одной CHAT-группе.
// Master state-machine на таймере.
//
// Сценарий:
//   1. A,B,D,C radio_on → флаг TOPO_FLAG_RADIO распространяется по BGP.
//   2. A: N кадров + FIN; B/D/C получают ровно N кадров по порядку и один FIN.
//      Дедупликация (src,stream,seq) может сработать на B, D или C.
//   3. Unix: повторяем burst с удержанием прямого A→B, затем A→D до прихода
//      обходной копии через C. Настоящие ETCP и radio_recv проверяют оба порядка.
//      Сценарий завершается после обработки всех отправленных RADIO-пакетов.

#include "chat_core.h"
#include "chat_sync.h"
#include "chat_event.h"
#include "member_sync.h"
#include "../radio/radio.h"
#include "../radio/radio_proto.h"
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
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"

#include <sqlite3.h>
#define OPENSSL_API_COMPAT 0x10100000L
#include "test_membership.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#define GROUP_SLOT UINT64_C(4242424242424242)
#define CH_ID "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951" /* Ed25519 для seed {94}. */

#define IDX_A 0
#define IDX_B 1
#define IDX_D 2
#define IDX_C 3

#define TICK_TB     100      /* 10 ms на такт */
#define MAX_TICKS   4000     /* глобальный таймаут ≈ 40 c */
#define N_FRAMES    5
#define N_PACKETS   3 /* default burst4: DATA4 + DATA1 + FIN */

enum mesh_phase {
    P_WAIT_CONN,
    P_SETUP,
    P_WAIT_BGP,
    P_RADIO_ON,
    P_WAIT_FLAGS,
    P_TALK1,
    P_WAIT_RX1,
    P_DONE,
};

struct mesh_ctx {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst[4];
    enum mesh_phase phase;
    int   result;        /* 0=running, 1=ok, 2=fail */
    int   ticks;
    void* timer;
    int scenario;        /* 0 = обычная доставка; Unix: 1/2 = обходная копия сначала на B/D. */
    uint32_t dup_start[4]; /* Счётчики до burst: предыдущий сценарий не даёт ложный PASS. */
};

/* приёмник кадров (per-узел): проверка порядка seq и FIN */
struct rx_evt {
    int      active;
    uint16_t stream;
    uint16_t expect_seq;
    int      frames;       /* число data-кадров (fin=0) текущего потока */
    int      fin_seen;
    int      seq_ok;
};

struct mesh_shared {
    uint8_t  ch_x_pub[32], ch_ed_pub[32], ch_ed_priv[32], ch_sig[64];
    uint64_t nid[4];
    uint8_t  x_pub[4][32], seed[4][32], ed_pub[4][32];
};
static struct mesh_shared g_sh;
static struct rx_evt       g_rx[4];
static const char* g_names[4] = {"A", "B", "D", "C"};

static int mesh_node_index(uint64_t id) {
    for (int i = 0; i < 4; ++i) if (g_sh.nid[i] == id) return i;
    return -1;
}

#ifdef UTUN_TEST_MESH_OBSERVE
/* Наблюдение raw ETCP и отменяемое удержание прямой копии до обходной доставки. */
static etcp_recv_fn g_radio_recv;
static unsigned int g_wire_tx[4][4], g_wire_rx[4][4];
static struct { struct ETCP_CONN* conn; struct ll_entry* entry; } g_held[N_FRAMES + 1];
static unsigned int g_held_count;
static int g_hold_node = -1, g_released;
extern int __real_etcp_bind(struct UTUN_INSTANCE* inst, uint8_t id, etcp_recv_fn callback);
extern int __real_etcp_send(struct ETCP_CONN* conn, struct ll_entry* entry);

static void mesh_wire(const char* direction, struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!conn || !entry || entry->len < 1 + RADIO_HDR_SIZE || entry->dgram[0] != ETCP_ID_RADIO) return;
    const uint8_t* p = entry->dgram + 1;
    uint16_t stream, seq;
    memcpy(&stream, p + RADIO_OFF_STREAM, 2); memcpy(&seq, p + RADIO_OFF_SEQ, 2);
    int self = mesh_node_index(conn->instance->node_id), peer = mesh_node_index(conn->peer_node_id);
    if (self >= 0 && peer >= 0) {
        if (!strcmp(direction, "TX")) ++g_wire_tx[self][peer]; else ++g_wire_rx[self][peer];
    }
    DEBUG_TRACE(DEBUG_CATEGORY_DEBUG,
        "mesh wire %s self=%s peer=%s group=%s src=%s stream=%u seq=%u fin=%d hops=%u",
        direction, self < 0 ? "?" : g_names[self], peer < 0 ? "?" : g_names[peer],
        sc_public_key_hex(p + RADIO_OFF_GROUP, (char[65]){0}), sc_public_key_hex(p + RADIO_OFF_SRC, (char[65]){0}), stream, seq, !!(p[RADIO_OFF_FLAGS] & RADIO_FLAG_FIN),
        p[RADIO_OFF_HOPS]);
}

static void mesh_radio_recv(struct ETCP_CONN* conn, struct ll_entry* entry) {
    int self = mesh_node_index(conn->instance->node_id), peer = mesh_node_index(conn->peer_node_id);
    mesh_wire("RX", conn, entry);
    g_radio_recv(conn, entry);
    if (g_hold_node >= 0 && self == g_hold_node && peer == IDX_C
        && g_rx[self].frames == N_FRAMES && g_rx[self].fin_seen && !g_released) {
        if (g_held_count != N_PACKETS) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "mesh gate: incomplete held burst node=%s held=%u", g_names[self], g_held_count);
            abort();
        }
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "mesh gate: release A->%s after C->%s delivered %d frames and FIN; held=%u",
                   g_names[self], g_names[self], N_FRAMES, g_held_count);
        g_released = 1;
        for (unsigned int i = 0; i < g_held_count; ++i) {
            if (__real_etcp_send(g_held[i].conn, g_held[i].entry) != 0) {
                queue_dgram_free(g_held[i].entry); queue_entry_free(g_held[i].entry);
                DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "mesh gate: real ETCP send failed node=%s frame=%u", g_names[self], i);
                abort();
            }
        }
        g_held_count = 0;
    }
}

int __wrap_etcp_bind(struct UTUN_INSTANCE* inst, uint8_t id, etcp_recv_fn callback) {
    if (id == ETCP_ID_RADIO) {
        if (g_radio_recv && g_radio_recv != callback) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "mesh: unexpected different radio callback"); abort();
        }
        g_radio_recv = callback;
        return __real_etcp_bind(inst, id, mesh_radio_recv);
    }
    return __real_etcp_bind(inst, id, callback);
}

int __wrap_etcp_send(struct ETCP_CONN* conn, struct ll_entry* entry) {
    mesh_wire("TX", conn, entry);
    if (g_hold_node >= 0 && conn && entry && entry->len >= 1 + RADIO_HDR_SIZE && entry->dgram[0] == ETCP_ID_RADIO
        && conn->instance->node_id == g_sh.nid[IDX_A] && mesh_node_index(conn->peer_node_id) == g_hold_node && !g_released) {
        if (g_held_count == N_FRAMES + 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "mesh gate: held burst overflow node=%s", g_names[g_hold_node]); abort();
        }
        g_held[g_held_count].conn = conn; g_held[g_held_count++].entry = entry;
        return 0; /* Владение принимает gate; передаёт реальному ETCP только после обходной доставки. */
    }
    return __real_etcp_send(conn, entry);
}
#endif

static void mesh_snapshot(struct mesh_ctx* t, const char* reason) {
    uint64_t gid = GROUP_SLOT;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "mesh snapshot reason=%s scenario=%d phase=%d tick=%d tb=%llu",
               reason, t->scenario, t->phase, t->ticks,
               (unsigned long long)get_time_tb());
    for (int i = 0; i < 4; ++i) {
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "mesh node=%s id=%016llx frames=%d fin=%d seq_ok=%d next_seq=%u dup=%u",
                   g_names[i], (unsigned long long)g_sh.nid[i], g_rx[i].frames, g_rx[i].fin_seen,
                   g_rx[i].seq_ok, g_rx[i].expect_seq, radio_dup_dropped(t->inst[i], gid));
#ifdef UTUN_TEST_MESH_OBSERVE
        for (int j = 0; j < 4; ++j) if (g_wire_tx[i][j] || g_wire_rx[i][j])
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "mesh edge self=%s peer=%s tx=%u rx=%u",
                       g_names[i], g_names[j], g_wire_tx[i][j], g_wire_rx[i][j]);
#endif
    }
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

/* ── данные сценария + конфиги (diamond A↔B, A↔D, B↔C, D↔C) ── */

static void fill_shared(void) {
    memset(&g_sh, 0, sizeof(g_sh));
    for (int i = 0; i < 4; i++) {
        uint8_t transport_secret[32];
        if (sc_identity_generate(g_sh.x_pub[i], transport_secret, g_sh.ed_pub[i], g_sh.seed[i])) abort();
        g_sh.nid[i] = sc_derive_node_id_from_pubkey(g_sh.x_pub[i]);
    }
    uint8_t x_private[32];
    g_sh.ch_ed_priv[0] = 94;
    if (sc_identity_from_seed(g_sh.ch_ed_priv, g_sh.ch_x_pub, x_private, g_sh.ch_ed_pub) ||
        test_group_signature(CH_ID, "radio-mesh", g_sh.ed_pub[IDX_A], g_sh.ch_x_pub, g_sh.ch_ed_pub, g_sh.ch_ed_priv, g_sh.ch_sig)) abort();
}

static void write_configs(const char* dir) {
    for (int i = 0; i < 4; i++) {
        char priv[65], pub[65], path[512], dbd[512];
        to_hex(g_sh.seed[i], 32, priv);
        to_hex(g_sh.x_pub[i], 32, pub);
        snprintf(path, sizeof(path), "%s/%s.conf", dir,
                 i == IDX_A ? "a" : i == IDX_B ? "b" : i == IDX_D ? "d" : "c");
        snprintf(dbd, sizeof(dbd), "%s/db%s", dir,
                 i == IDX_A ? "a" : i == IDX_B ? "b" : i == IDX_D ? "d" : "c");
        utun_mkdir(dbd, 0755);

        char client[1024] = "";
        if (i == IDX_B) {   /* B → A */
            char apub[65]; to_hex(g_sh.x_pub[IDX_A], 32, apub);
            snprintf(client, sizeof(client), "[client: to_a]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", apub);
        } else if (i == IDX_D) {   /* D → A */
            char apub[65]; to_hex(g_sh.x_pub[IDX_A], 32, apub);
            snprintf(client, sizeof(client), "[client: to_a]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", apub);
        } else if (i == IDX_C) {   /* C → B и C → D */
            char bpub[65], dpub[65];
            to_hex(g_sh.x_pub[IDX_B], 32, bpub);
            to_hex(g_sh.x_pub[IDX_D], 32, dpub);
            snprintf(client, sizeof(client),
                     "[client: to_b]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n"
                     "[client: to_d]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", bpub, dpub);
        }

        wf(path,
            "[global]\ntun_ip=10.98.%d.1/24\ntun_ifname=tun%d0\ndb_path=%s/db%s\n"
            "my_private_key=%s\nmy_public_key=%s\n"
            "[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
            "%s"
            "[chatserver]\nstorage_autoload=0\n[allowed_keys]\nallow_all=1\n",
            i, i, dir, i == IDX_A ? "a" : i == IDX_B ? "b" : i == IDX_D ? "d" : "c",
            priv, pub, client);
    }
}

/* ── проверки ── */

static int conn_up(struct UTUN_INSTANCE* inst, uint64_t nid) {
    struct ETCP_CONN* c = instance_find_conn(inst, nid);
    return c && c->initialized && c->links_up;
}

static int bgp_has(struct UTUN_INSTANCE* inst, uint64_t nid) {
    uint64_t gid = GROUP_SLOT;
    struct TOPO_GROUP* g = topo_groups_find(inst->topo_groups, gid);
    if (!g) return 0;
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(g, nid);
    return nq && nq->paths && nq->paths->head;
}

static int node_radio(struct UTUN_INSTANCE* inst, uint64_t nid) {
    uint64_t gid = GROUP_SLOT;
    struct TOPO_GROUP* g = topo_groups_find(inst->topo_groups, gid);
    if (!g) return 0;
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(g, nid);
    return nq ? nq->radio : 0;
}

/* все 8 направлений diamond подняты */
static int all_conn_up(struct mesh_ctx* t) {
    return conn_up(t->inst[IDX_A], g_sh.nid[IDX_B]) && conn_up(t->inst[IDX_B], g_sh.nid[IDX_A])
        && conn_up(t->inst[IDX_A], g_sh.nid[IDX_D]) && conn_up(t->inst[IDX_D], g_sh.nid[IDX_A])
        && conn_up(t->inst[IDX_B], g_sh.nid[IDX_C]) && conn_up(t->inst[IDX_C], g_sh.nid[IDX_B])
        && conn_up(t->inst[IDX_D], g_sh.nid[IDX_C]) && conn_up(t->inst[IDX_C], g_sh.nid[IDX_D]);
}

/* каждый узел видит 3 остальных в BGP */
static int all_bgp(struct mesh_ctx* t) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (i != j && !bgp_has(t->inst[i], g_sh.nid[j])) return 0;
    return 1;
}

/* каждый узел видит radio=1 у 3 остальных */
static int all_radio(struct mesh_ctx* t) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (i != j && !node_radio(t->inst[i], g_sh.nid[j])) return 0;
    return 1;
}

/* ── frame cb (per-узел) ── */

static void rx_frame_cb(struct UTUN_INSTANCE* inst, uint64_t group_id,
                        const uint8_t ed[32], uint16_t stream, uint16_t seq,
                        uint8_t fin, const uint8_t* opus, int len, void* arg) {
    (void)group_id; (void)opus; (void)len;
    struct rx_evt* rx = (struct rx_evt*)arg;
    if (memcmp(ed, g_sh.ed_pub[IDX_A], 32) || (rx->active && stream != rx->stream)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "mesh RX: wrong source/stream self=%016llx src=%s stream=%u expected=%u",
                    (unsigned long long)inst->node_id, sc_public_key_hex(ed, (char[65]){0}), stream, rx->stream);
        rx->seq_ok = 0; return;
    }
    if (fin) {
        if (!rx->active || rx->fin_seen || rx->frames != N_FRAMES || seq != N_FRAMES) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "mesh RX: invalid FIN self=%016llx stream=%u seq=%u frames=%d previous_fin=%d",
                        (unsigned long long)inst->node_id, stream, seq, rx->frames, rx->fin_seen);
            rx->seq_ok = 0;
        }
        rx->fin_seen = 1;
        return;
    }
    if (!rx->active) {
        rx->active = 1;
        rx->stream = stream;
        rx->expect_seq = 0;
        rx->frames = 0;
        rx->fin_seen = 0;
        rx->seq_ok = 1;
    }
    if (seq != rx->expect_seq || rx->fin_seen) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "mesh RX: wrong data order self=%016llx seq=%u expected=%u fin=%d",
                    (unsigned long long)inst->node_id, seq, rx->expect_seq, rx->fin_seen);
        rx->seq_ok = 0;
    }
    rx->expect_seq = (uint16_t)(seq + 1);
    rx->frames++;
}

/* ── настройка канала и мемберов ── */

static void channel_put_shared(struct UTUN_INSTANCE* inst, int has_priv) {
    topo_node_sqlite_channel_put(inst->topo_sqlite_db, CH_ID, "radio-mesh", g_sh.ed_pub[IDX_A],
        g_sh.ch_x_pub, NULL, g_sh.ch_ed_pub, has_priv ? g_sh.ch_ed_priv : NULL, g_sh.ch_sig);
    if (!topo_groups_create_group(inst->topo_groups, GROUP_SLOT, TOPO_GROUP_TYPE_CHAT, CH_ID) || chat_core_ensure_channel_ready(inst, CH_ID)) abort();
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
        member_put_placeholder(inst, IDX_D, "D");
        member_put_placeholder(inst, IDX_C, "C");
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_B]), CH_ID, NULL, NULL);
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_D]), CH_ID, NULL, NULL);
        break;
    case IDX_B:
        channel_put_shared(inst, 0);
        member_put_self(inst, "B");
        member_put_placeholder(inst, IDX_A, "A");
        member_put_placeholder(inst, IDX_D, "D");
        member_put_placeholder(inst, IDX_C, "C");
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_A]), CH_ID, NULL, NULL);
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_C]), CH_ID, NULL, NULL);
        break;
    case IDX_D:
        channel_put_shared(inst, 0);
        member_put_self(inst, "D");
        member_put_placeholder(inst, IDX_A, "A");
        member_put_placeholder(inst, IDX_B, "B");
        member_put_placeholder(inst, IDX_C, "C");
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_A]), CH_ID, NULL, NULL);
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_C]), CH_ID, NULL, NULL);
        break;
    case IDX_C:
        channel_put_shared(inst, 0);
        member_put_self(inst, "C");
        member_put_placeholder(inst, IDX_A, "A");
        member_put_placeholder(inst, IDX_B, "B");
        member_put_placeholder(inst, IDX_D, "D");
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_B]), CH_ID, NULL, NULL);
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_D]), CH_ID, NULL, NULL);
        break;
    }
}

/* ── talk: N кадров + FIN от A ── */

static void do_talk(struct UTUN_INSTANCE* A) {
    uint64_t gid = GROUP_SLOT;
    static const uint8_t opus[16] = { 0xf8, 0xff, 0xfe, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d };
    radio_talk_begin(A, gid);
    for (int i = 0; i < N_FRAMES; i++) radio_talk_send(A, gid, opus, sizeof(opus));
    radio_talk_end(A, gid);
}

/* ── master state-machine ── */

/* 1 = полная доставка и дедупликация; 0 = ждём сеть; -1 = проверяемое нарушение. */
static int mesh_burst_result(struct mesh_ctx* t) {
    uint64_t gid = GROUP_SLOT;
    uint32_t dup[4];
    for (int i = 0; i < 4; ++i) {
        dup[i] = radio_dup_dropped(t->inst[i], gid) - t->dup_start[i];
        if ((g_rx[i].active && !g_rx[i].seq_ok) || g_rx[i].frames > N_FRAMES || (i == IDX_A && g_rx[i].active)) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "mesh: invalid delivery scenario=%d node=%s frames=%d seq_ok=%d",
                        t->scenario, g_names[i], g_rx[i].frames, g_rx[i].seq_ok);
            return -1;
        }
    }
    for (int i = 1; i < 4; ++i)
        if (g_rx[i].frames != N_FRAMES || !g_rx[i].fin_seen || !g_rx[i].seq_ok) return 0;
#ifdef UTUN_TEST_MESH_OBSERVE
    if (g_held_count) return 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) if (g_wire_tx[i][j] != g_wire_rx[j][i]) return 0;
    if (g_hold_node >= 0 && (!g_released || dup[g_hold_node] != N_PACKETS || dup[IDX_C]
        || g_wire_tx[g_hold_node][IDX_C] || g_wire_rx[IDX_C][g_hold_node])) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "mesh: forced order failed hold=%s released=%d dupB=%u dupD=%u dupC=%u",
                    g_names[g_hold_node], g_released, dup[IDX_B], dup[IDX_D], dup[IDX_C]);
        return -1;
    }
#endif
    if ((uint64_t)dup[IDX_B] + dup[IDX_D] + dup[IDX_C] < N_PACKETS) return 0;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "mesh scenario=%d PASS frames=5/5/5 FIN=1/1/1 dupB=%u dupD=%u dupC=%u",
               t->scenario, dup[IDX_B], dup[IDX_D], dup[IDX_C]);
    return 1;
}

static void mesh_tick(void* arg) {
    struct mesh_ctx* t = arg;
    t->timer = NULL;
    if (t->result) return;

    struct UTUN_INSTANCE* A = t->inst[IDX_A];
    struct UTUN_INSTANCE* B = t->inst[IDX_B];
    struct UTUN_INSTANCE* D = t->inst[IDX_D];
    struct UTUN_INSTANCE* C = t->inst[IDX_C];
    uint64_t gid = GROUP_SLOT;
    enum mesh_phase previous = t->phase;

    switch (t->phase) {
    case P_WAIT_CONN:
        if (all_conn_up(t)) t->phase = P_SETUP;
        break;
    case P_SETUP:
        do_setup(A, IDX_A);
        do_setup(B, IDX_B);
        do_setup(D, IDX_D);
        do_setup(C, IDX_C);
        t->phase = P_WAIT_BGP;
        break;
    case P_WAIT_BGP:
        if (all_bgp(t)) t->phase = P_RADIO_ON;
        break;
    case P_RADIO_ON:
        radio_set_active(A, gid, 1);
        radio_set_active(B, gid, 1);
        radio_set_active(D, gid, 1);
        radio_set_active(C, gid, 1);
        t->phase = P_WAIT_FLAGS;
        break;
    case P_WAIT_FLAGS:
        if (all_radio(t)) t->phase = P_TALK1;
        break;
    case P_TALK1:
        memset(g_rx, 0, sizeof(g_rx));
        for (int i = 0; i < 4; ++i) t->dup_start[i] = radio_dup_dropped(t->inst[i], gid);
#ifdef UTUN_TEST_MESH_OBSERVE
        memset(g_wire_tx, 0, sizeof(g_wire_tx)); memset(g_wire_rx, 0, sizeof(g_wire_rx));
        g_hold_node = t->scenario == 1 ? IDX_B : t->scenario == 2 ? IDX_D : -1;
        g_released = 0;
#endif
        do_talk(A);
        t->phase = P_WAIT_RX1;
        break;
    case P_WAIT_RX1:
        {
            int result = mesh_burst_result(t);
            if (result < 0) { mesh_snapshot(t, "invalid_burst"); t->result = 2; uasync_stop(t->ua); return; }
            if (result > 0) {
#ifdef UTUN_TEST_MESH_OBSERVE
                t->phase = ++t->scenario < 3 ? P_TALK1 : P_DONE;
#else
                t->phase = P_DONE;
#endif
            }
        }
        break;
    case P_DONE:
        mesh_snapshot(t, "complete");
        t->result = 1;
        uasync_stop(t->ua);
        return;
    }

    if (previous != t->phase) mesh_snapshot(t, "phase_change");

    if (++t->ticks > MAX_TICKS) {
        mesh_snapshot(t, "timeout");
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "mesh: timeout scenario=%d phase=%d ticks=%d", t->scenario, t->phase, t->ticks);
        fprintf(stderr, "test: timeout in phase %d (B_rx=%d/%d fin=%d seqok=%d D_rx=%d/%d fin=%d seqok=%d C_rx=%d/%d fin=%d seqok=%d dupC=%u)\n",
                (int)t->phase,
                g_rx[IDX_B].frames, N_FRAMES, g_rx[IDX_B].fin_seen, g_rx[IDX_B].seq_ok,
                g_rx[IDX_D].frames, N_FRAMES, g_rx[IDX_D].fin_seen, g_rx[IDX_D].seq_ok,
                g_rx[IDX_C].frames, N_FRAMES, g_rx[IDX_C].fin_seen, g_rx[IDX_C].seq_ok,
                radio_dup_dropped(C, gid));
        t->result = 2;
        uasync_stop(t->ua);
        return;
    }
    t->timer = uasync_set_timeout(t->ua, TICK_TB, t, mesh_tick, "mesh_tick");
}

/* ── main ── */

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_TRACE);
        debug_set_category_level_by_name("radio", "debug");
        debug_set_category_level_by_name("bgp", "info");
        debug_set_category_level_by_name("member_sync", "info");
        debug_set_category_level_by_name("chat_sync", "info");
    }
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));

    char dir[512]; snprintf(dir, sizeof(dir), "/tmp/utun_radio_mesh_XXXXXX");
    if (test_mkdtemp(dir) != 0) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
    test_setenv("UTUN_TEST_DIR", dir, 1);

    fill_shared();
    write_configs(dir);

    printf("=== test_radio_mesh ===\n"); fflush(stdout);

    TEST("diamond: complete delivery and deduplication at B/D/C"); {
        size_t allocated = u_get_allocated_count();
        struct UASYNC* ua = uasync_create();
        if (!ua) { FAIL("uasync_create failed"); return 1; }

        struct mesh_ctx t;
        memset(&t, 0, sizeof(t));
        t.ua = ua;
        t.phase = P_WAIT_CONN;

        char cfg[4][512];
        const char* names[4] = { "a.conf", "b.conf", "d.conf", "c.conf" };
        for (int i = 0; i < 4; i++) snprintf(cfg[i], sizeof(cfg[i]), "%s/%s", dir, names[i]);

        for (int i = 0; i < 4; i++) {
            t.inst[i] = utun_instance_create(ua, cfg[i]);
            if (!t.inst[i]) { FAIL("instance create failed i=%d", i); uasync_destroy(ua, 0); return 1; }
        }
        test_etcp_set_peer(t.inst[IDX_B], "to_a", t.inst[IDX_A], "s1");
        test_etcp_set_peer(t.inst[IDX_D], "to_a", t.inst[IDX_A], "s1");
        test_etcp_set_peer(t.inst[IDX_C], "to_b", t.inst[IDX_B], "s1");
        test_etcp_set_peer(t.inst[IDX_C], "to_d", t.inst[IDX_D], "s1");
        for (int i = 0; i < 4; i++) utun_instance_init(t.inst[i]);

        for (int i = 0; i < 4; i++) radio_set_frame_cb(t.inst[i], rx_frame_cb, &g_rx[i]);

        t.timer = uasync_set_timeout(ua, TICK_TB, &t, mesh_tick, "mesh_tick");
        uasync_mainloop(ua);

        int ok = (t.result == 1);
#ifdef UTUN_TEST_MESH_OBSERVE
        for (unsigned int i = 0; i < g_held_count; ++i) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "mesh gate: discard unsent frame=%u after failure", i);
            queue_dgram_free(g_held[i].entry); queue_entry_free(g_held[i].entry);
        }
        g_held_count = 0;
#endif

        for (int i = 0; i < 4; i++) if (t.inst[i]) utun_instance_destroy(t.inst[i]);
        uasync_destroy(ua, 0);

        size_t remaining = u_get_allocated_count();
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "mesh cleanup allocations=%zu->%zu scenarios=%d", allocated, remaining, t.scenario);
        if (ok && remaining == allocated) OK();
        else FAIL("result=%d phase=%d scenario=%d allocations=%zu->%zu", t.result, (int)t.phase, t.scenario, allocated, remaining);
    }

    char cmd[512]; snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); (void)!system(cmd);

    printf("\n%d/%d passed, %d failed\n", G_PASSED, G_TOTAL, G_FAILED);
    return G_FAILED > 0 ? 1 : 0;
}
