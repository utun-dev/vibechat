// test_radio_headless.c — e2e-тест рации через headless API (control + audio сокеты).
//
// Два узла (A, B) в одном процессе, один UASYNC. Тест выступает «headless-клиентом»
// обоих узлов: control-сокеты (JSON: radio_on/off/subscribers) и audio-сокеты
// (бинарные PCM-фреймы рации) каждого узла.
//
// Сценарий (через реальные TCP-сокеты):
//   A radio_on, B radio_on → A: PTT_START + FRAME(ненулевой PCM) + PTT_STOP
//   → B получает микшированный ненулевой PCM → radio_subscribers(A) == 1.

#include "chat_core.h"
#include "chat_sync.h"
#include "chat_event.h"
#include "member_sync.h"
#include "chat_headless_control.h"
#include "../radio/radio.h"
#include "../radio/radio_headless.h"
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
#include "../lib/socket_compat.h"
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
#include <errno.h>

static char CH_ID[65];

#define IDX_A 0
#define IDX_B 1

#define TICK_TB     100      /* 10 ms на такт */
#define MAX_TICKS   4000     /* глобальный таймаут ≈ 40 c */

enum hr_phase {
    P_WAIT_CONN,
    P_SETUP,
    P_WAIT_BGP,
    P_CONNECT_CLIENTS,
    P_RADIO_ON,
    P_WAIT_RADIO_ON,
    P_BIND_AUDIO,
    P_TALK,
    P_WAIT_MEDIA,
    P_CHECK_SUBSCRIBERS,
    P_WAIT_SUBSCRIBERS,
    P_DONE,
};

struct hr_conn {
    socket_t fd;
    void*    socket_id;
    char     buf[65536];
    int      len;
};

struct hr_ctx {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst[2];
    enum hr_phase phase;
    int   result;
    int   ticks;
    void* timer;
    struct hr_conn ctl[2];
    struct hr_conn aud[2];
};

struct hr_shared {
    uint8_t  ch_x_pub[32], ch_ed_pub[32], ch_ed_priv[32], ch_sig[64];
    uint64_t nid[2];
    uint8_t  x_pub[2][32], seed[2][32], ed_pub[2][32];
};
static struct hr_shared g_sh;

static int G_PASSED = 0, G_FAILED = 0, G_TOTAL = 0;
#define TEST(n)  do { G_TOTAL++; printf("  %-58s", n); fflush(stdout); } while(0)
#define OK()     do { G_PASSED++; printf("OK\n"); } while(0)
#define FAIL(f,...) do { G_FAILED++; printf("FAIL: " f "\n", ##__VA_ARGS__); } while(0)

static void to_hex(const uint8_t* bin, size_t n, char* out) {
    for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", bin[i]);
    out[n * 2] = '\0';
}

static int wf(const char* p, const char* f, ...) {
    va_list ap; FILE* fp = fopen(p, "w"); if (!fp) return -1;
    va_start(ap, f); vfprintf(fp, f, ap); va_end(ap); fclose(fp); return 0;
}

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
    if (test_group_signature(CH_ID, "hl-radio", g_sh.ed_pub[IDX_A], g_sh.ch_x_pub, g_sh.ch_ed_pub, g_sh.ch_ed_priv, g_sh.ch_sig)) abort();
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
            char apub[65]; to_hex(g_sh.x_pub[IDX_A], 32, apub);
            snprintf(client, sizeof(client), "[client: to_a]\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n", apub);
        }
        wf(path,
            "[global]\ntun_ip=10.99.%d.1/24\ntun_ifname=tun%d0\ndb_path=%s/db%s\n"
            "my_private_key=%s\nmy_public_key=%s\n"
            "[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
            "%s"
            "[chatserver]\nstorage_autoload=0\n[allowed_keys]\nallow_all=1\n",
            i, i, dir, i == IDX_A ? "a" : "b",
            priv, pub, client);
    }
}

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

static void channel_put_shared(struct UTUN_INSTANCE* inst, int has_priv) {
    topo_node_sqlite_channel_put(inst->topo_sqlite_db, CH_ID, "hl-radio", g_sh.ed_pub[IDX_A],
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

/* ── клиентские сокеты ── */

static void hr_recv_cb(socket_t fd, void* arg) {
    struct hr_conn* c = (struct hr_conn*)arg;
    (void)fd;
    char tmp[4096];
    ssize_t r = recv(fd, tmp, sizeof(tmp) - 1, 0);
    if (r <= 0) return;
    if (c->len + (int)r < (int)sizeof(c->buf)) {
        memcpy(c->buf + c->len, tmp, (size_t)r);
        c->len += (int)r;
    }
}

static int hr_connect(struct hr_ctx* t, struct hr_conn* c, int port) {
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd == SOCKET_INVALID) return -1;
    struct sockaddr_in sin; memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET; sin.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    if (connect(c->fd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        socket_close_wrapper(c->fd); c->fd = SOCKET_INVALID; return -1;
    }
    socket_set_nonblocking(c->fd);
    c->socket_id = uasync_add_socket_t(t->ua, c->fd, hr_recv_cb, NULL, NULL, "test_radio", c);
    if (!c->socket_id) { socket_close_wrapper(c->fd); c->fd = SOCKET_INVALID; return -1; }
    return 0;
}

static void hr_send(struct hr_conn* c, const void* data, size_t len) {
    if (!c || c->fd == SOCKET_INVALID) return;
    ssize_t off = 0;
    while (off < (ssize_t)len) {
        ssize_t r = send(c->fd, (const char*)data + off, len - (size_t)off, 0);
        if (r < 0 && (socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK)) { continue; }
        if (r <= 0) break;
        off += r;
    }
}

static int hr_has(struct hr_conn* c, const char* substr) {
    return c && memmem(c->buf, (size_t)c->len, substr, strlen(substr)) != NULL;
}

/* есть ли в audio-буфере FRAME с ненулевым PCM (данные рации) */
static int hr_has_nonzero_pcm(struct hr_conn* c, const uint8_t group_key[32]) {
    if (!c) return 0;
    const char* buf = c->buf;
    int len = c->len;
    for (int i = 0; i + RADIO_AUDIO_HDR_SIZE <= len; i++) {
        if ((uint8_t)buf[i] != RADIO_AUDIO_FRAME || memcmp(buf + i + 1, group_key, 32)) continue;
        uint16_t flen = (uint16_t)(((uint8_t)buf[i + RADIO_AUDIO_LEN_OFF] << 8) | (uint8_t)buf[i + RADIO_AUDIO_LEN_OFF + 1]);
        if (flen != RADIO_AUDIO_PCM_FRAME_BYTES) continue;
        if (i + RADIO_AUDIO_HDR_SIZE + flen > len) break;
        int16_t pcm[960]; memcpy(pcm, buf + i + RADIO_AUDIO_HDR_SIZE, sizeof(pcm));
        for (int k = 0; k < 960; k++) if (pcm[k] != 0) return 1;
    }
    return 0;
}

/* ── master state-machine ── */

static void hr_tick(void* arg) {
    struct hr_ctx* t = arg;
    t->timer = NULL;
    if (t->result) return;

    struct UTUN_INSTANCE* A = t->inst[IDX_A];
    struct UTUN_INSTANCE* B = t->inst[IDX_B];
    uint64_t na = g_sh.nid[IDX_A], nb = g_sh.nid[IDX_B];
    uint64_t gid = topo_groups_channel_slot(A->topo_groups, CH_ID);
    struct TOPO_GROUP* group = topo_groups_find(A->topo_groups, gid);
    const uint8_t* group_key = group ? topo_group_wire_id(group) : NULL;

    switch (t->phase) {
    case P_WAIT_CONN:
        if (conn_up(A, nb) && conn_up(B, na)) t->phase = P_SETUP;
        break;
    case P_SETUP:
        do_setup(A, IDX_A);
        do_setup(B, IDX_B);
        if (chat_headless_control_init(A->ua, A, "127.0.0.1", 0) != 0
            || chat_headless_control_init(B->ua, B, "127.0.0.1", 0) != 0) {
            fprintf(stderr, "headless control init failed\n"); t->result = 2; break;
        }
        if (radio_headless_init(A->ua, A, "127.0.0.1", 0) != 0
            || radio_headless_init(B->ua, B, "127.0.0.1", 0) != 0) {
            fprintf(stderr, "radio headless audio init failed\n"); t->result = 2; break;
        }
        t->phase = P_WAIT_BGP;
        break;
    case P_WAIT_BGP:
        if (bgp_has(A, nb) && bgp_has(B, na)) t->phase = P_CONNECT_CLIENTS;
        break;
    case P_CONNECT_CLIENTS:
        if (hr_connect(t, &t->ctl[IDX_A], chat_headless_control_get_port(A)) != 0
            || hr_connect(t, &t->ctl[IDX_B], chat_headless_control_get_port(B)) != 0
            || hr_connect(t, &t->aud[IDX_A], radio_headless_get_port(A)) != 0
            || hr_connect(t, &t->aud[IDX_B], radio_headless_get_port(B)) != 0) {
            fprintf(stderr, "client connect failed\n"); t->result = 2; break;
        }
        t->phase = P_RADIO_ON;
        break;
    case P_RADIO_ON: {
        char json[256];
        snprintf(json, sizeof(json), "{\"id\":1,\"cmd\":\"radio_on\",\"ch\":\"%s\"}\n", CH_ID);
        hr_send(&t->ctl[IDX_A], json, strlen(json));
        hr_send(&t->ctl[IDX_B], json, strlen(json));
        t->phase = P_WAIT_RADIO_ON;
        break;
    }
    case P_WAIT_RADIO_ON:
        /* оба подтвердили radio_on и A видит флаг B через BGP */
        if (group_key && hr_has(&t->ctl[IDX_A], "\"radio\":true") && hr_has(&t->ctl[IDX_B], "\"radio\":true") &&
            hr_has(&t->ctl[IDX_A], sc_public_key_hex(group_key, (char[65]){0})) &&
            hr_has(&t->ctl[IDX_B], sc_public_key_hex(group_key, (char[65]){0})) && node_radio(A, nb) && node_radio(B, na))
            t->phase = P_BIND_AUDIO;
        break;
    case P_BIND_AUDIO: {
        uint8_t hdr[RADIO_AUDIO_HDR_SIZE] = {0};
        hdr[0] = RADIO_AUDIO_HELLO;
        memcpy(hdr + 1, group_key, 32);
        hr_send(&t->aud[IDX_A], hdr, sizeof(hdr));
        hr_send(&t->aud[IDX_B], hdr, sizeof(hdr));
        t->phase = P_TALK;
        break;
    }
    case P_TALK: {
        uint8_t ptt[RADIO_AUDIO_HDR_SIZE] = {0};
        ptt[0] = RADIO_AUDIO_PTT_START; memcpy(ptt + 1, group_key, 32);
        hr_send(&t->aud[IDX_A], ptt, sizeof(ptt));

        uint8_t frm[RADIO_AUDIO_HDR_SIZE + RADIO_AUDIO_PCM_FRAME_BYTES];
        frm[0] = RADIO_AUDIO_FRAME; memcpy(frm + 1, group_key, 32);
        frm[RADIO_AUDIO_LEN_OFF] = (uint8_t)(RADIO_AUDIO_PCM_FRAME_BYTES >> 8);
        frm[RADIO_AUDIO_LEN_OFF + 1] = (uint8_t)RADIO_AUDIO_PCM_FRAME_BYTES;
        int16_t pcm[960]; for (int k = 0; k < 960; k++) pcm[k] = 1000;
        memcpy(frm + RADIO_AUDIO_HDR_SIZE, pcm, sizeof(pcm));
        hr_send(&t->aud[IDX_A], frm, sizeof(frm));

        ptt[0] = RADIO_AUDIO_PTT_STOP;
        hr_send(&t->aud[IDX_A], ptt, sizeof(ptt));
        t->phase = P_WAIT_MEDIA;
        break;
    }
    case P_WAIT_MEDIA:
        if (group_key && hr_has_nonzero_pcm(&t->aud[IDX_B], group_key)) t->phase = P_CHECK_SUBSCRIBERS;
        break;
    case P_CHECK_SUBSCRIBERS: {
        char json[256];
        snprintf(json, sizeof(json), "{\"id\":2,\"cmd\":\"radio_subscribers\",\"ch\":\"%s\"}\n", CH_ID);
        hr_send(&t->ctl[IDX_A], json, strlen(json));
        t->phase = P_WAIT_SUBSCRIBERS;
        break;
    }
    case P_WAIT_SUBSCRIBERS:
        if (hr_has(&t->ctl[IDX_A], "\"subscribers\":1")) t->phase = P_DONE;
        break;
    case P_DONE:
        t->result = 1;
        uasync_stop(t->ua);
        return;
    }

    if (t->phase == P_DONE) { t->result = 1; uasync_stop(t->ua); return; }

    if (++t->ticks > MAX_TICKS) {
        fprintf(stderr, "test: timeout in phase %d (A_conn=%d B_conn=%d radio_flags A(B)=%d B(A)=%d)\n",
                (int)t->phase, conn_up(A, nb), conn_up(B, na), node_radio(A, nb), node_radio(B, na));
        t->result = 2;
        uasync_stop(t->ua);
        return;
    }
    t->timer = uasync_set_timeout(t->ua, TICK_TB, t, hr_tick, "hr_tick");
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
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));

    char dir[512]; snprintf(dir, sizeof(dir), "/tmp/utun_radio_hl_XXXXXX");
    if (test_mkdtemp(dir) != 0) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
    test_setenv("UTUN_TEST_DIR", dir, 1);

    fill_shared();
    write_configs(dir);

    printf("=== test_radio_headless ===\n"); fflush(stdout);

    TEST("headless: radio_on + PTT PCM-поток + radio_subscribers через сокеты"); {
        struct UASYNC* ua = uasync_create();
        if (!ua) { FAIL("uasync_create failed"); return 1; }

        struct hr_ctx t;
        memset(&t, 0, sizeof(t));
        t.ua = ua;
        t.phase = P_WAIT_CONN;
        for (int i = 0; i < 2; i++) { t.ctl[i].fd = SOCKET_INVALID; t.aud[i].fd = SOCKET_INVALID; }

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
        utun_instance_init(t.inst[IDX_A]);
        utun_instance_init(t.inst[IDX_B]);

        t.timer = uasync_set_timeout(ua, TICK_TB, &t, hr_tick, "hr_tick");
        uasync_mainloop(ua);

        int ok = (t.result == 1);

        for (int i = 0; i < 2; i++) {
            if (t.ctl[i].fd != SOCKET_INVALID) { uasync_remove_socket_t(ua, t.ctl[i].fd); socket_close_wrapper(t.ctl[i].fd); }
            if (t.aud[i].fd != SOCKET_INVALID) { uasync_remove_socket_t(ua, t.aud[i].fd); socket_close_wrapper(t.aud[i].fd); }
        }
        if (t.inst[IDX_A]) utun_instance_destroy(t.inst[IDX_A]);
        if (t.inst[IDX_B]) utun_instance_destroy(t.inst[IDX_B]);
        uasync_destroy(ua, 0);

        if (ok) OK(); else FAIL("result=%d phase=%d", t.result, (int)t.phase);
    }

    char cmd[512]; snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); (void)!system(cmd);

    printf("\n%d/%d passed, %d failed\n", G_PASSED, G_TOTAL, G_FAILED);
    return G_FAILED > 0 ? 1 : 0;
}
