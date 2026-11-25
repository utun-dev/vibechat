// test_chat_join_e2e.c — E2E тест join-протокола через реальные chat_core/chat_sync.
//
// Много-процессный: chat_core/chat_sync — глобальные синглтоны на процесс, поэтому
// роли A(инвайтер)/C(connection)/J(джойнер) разнесены по fork-процессам.
// Статические данные (ключи канала, node_id, pubkey) заполняются родителем ДО fork и
// наследуются детьми через память; динамический обмен — invite-ссылка через файл.
// Координация результата — коды возврата (0=OK, 1=FAIL).
//
// Сценарии:
//   happy    A↔B↔C по BGP, C↔J по ETCP. Состав J получает от A до репликации на C.
//   degraded A==C: J входит напрямую в A (ключ локально, форварда нет).
//   invalid  неверный ключ: J отклонён, мембера J нет.
//   recovery A подтверждает вступление, J теряет READY и перезапускается после отзыва ссылки.

#include "chat_core.h"
#include "chat_core_priv.h"
#include "chat_sync.h"
#include "chat_sync_sql.h"
#include "chat_join.h"
#include "chat_headless_control.h"
#include "../lib/socket_compat.h"
#include "../routing_layer/etcp_router.h"
#include "../transport_layer/etcp_api.h"
#include "invite_link.h"
#include "invite_build.h"
#include "member_sync.h"
#include "chat_event.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/node_conn_direct.h"
#include "../transport_layer/secure_channel.h"
#include "../ntp_time.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "test_net.h"
#include "test_membership.h"
#include "test_join_wire.h"
#include "test_utils.h"
#include <errno.h>

#include <sqlite3.h>
#define OPENSSL_API_COMPAT 0x10100000L
#include <openssl/evp.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif

#ifdef _WIN32

int main(void) {
    printf("[SKIP] test_chat_join_e2e — fork() not available on Windows\n");
    return 0;
}

#else

#define POLL_MS 5
#define CH_NAME "test-channel"

#define IDX_A 0
#define IDX_C 1
#define IDX_J 2
#define IDX_B 3

/* общие данные (заполняются родителем, наследуются детьми через fork) */
struct e2e_shared {
    char     ch_id[65];
    uint8_t  ch_x_pub[32], ch_x_priv[32], ch_ed_pub[32], ch_ed_priv[32], ch_sig[64];
    uint64_t nid[4];
    uint8_t  x_pub[4][32], seed[4][32], ed_pub[4][32];
    int      degraded, lose_ready;
};

static struct e2e_shared g_sh;
static int early_bgp_membership;

/* Проверяем границу доступа на каждом шаге реального event loop. */
static void poll_checked(struct UTUN_INSTANCE* inst) {
    uasync_poll(inst->ua, POLL_MS);
    if (inst->node_id == g_sh.nid[IDX_J]) return;
    if (topo_node_sqlite_member_in_channel(inst->topo_sqlite_db, g_sh.ch_id, g_sh.nid[IDX_J])) return;
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, topo_groups_channel_slot(inst->topo_groups, g_sh.ch_id));
    for (struct ll_entry* e = group && group->senders_list ? group->senders_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        if (item->conn && item->conn->peer_node_id == g_sh.nid[IDX_J]) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "test: J entered BGP before member commit group=%s", g_sh.ch_id);
            early_bgp_membership = 1;
        }
    }
}

struct invite_result { int calls, result; char link[1024]; };
static void invite_ready(void* arg, int result, const char* link) {
    struct invite_result* r = arg;
    r->calls++; r->result = result;
    if (link) snprintf(r->link, sizeof(r->link), "%s", link);
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
    /* wait_file реагирует на появление имени: публикуем только полностью записанный файл. */
    char temporary[1024];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", p) >= (int)sizeof(temporary)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN fixture: publication path too long"); return -1;
    }
    FILE* fp = fopen(temporary, "w");
    if (!fp) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN fixture: publication open failed path=%s: %s", p, strerror(errno)); return -1; }
    va_list ap; va_start(ap, f); int written = vfprintf(fp, f, ap); va_end(ap);
    int closed = fclose(fp);
    if (written < 0 || closed || rename(temporary, p)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN fixture: publication failed path=%s written=%d close=%d: %s",
            p, written, closed, strerror(errno)); remove(temporary); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "JOIN fixture: file published path=%s bytes=%d", p, written); return 0;
}

/* ── генерация данных сценария + конфигов (родитель) ── */

static void fill_shared(int degraded) {
    memset(&g_sh, 0, sizeof(g_sh));
    g_sh.degraded = degraded;

    for (int i = 0; i < 4; i++) {
        uint8_t transport_secret[32];
        assert(!sc_identity_generate(g_sh.x_pub[i], transport_secret, g_sh.ed_pub[i], g_sh.seed[i]));
        g_sh.nid[i] = sc_derive_node_id_from_pubkey(g_sh.x_pub[i]);
    }

    assert(!chat_channel_generate_keys(g_sh.ch_x_pub, g_sh.ch_x_priv, g_sh.ch_ed_pub, g_sh.ch_ed_priv));
    sc_public_key_hex(g_sh.ch_ed_pub, g_sh.ch_id);

    {
        uint8_t msg[CHAT_CHANNEL_DESCRIPTION_MAX];
        int size = chat_channel_description_data(g_sh.ch_id, CH_NAME, g_sh.ed_pub[IDX_A], g_sh.ch_x_pub, g_sh.ch_ed_pub, msg, sizeof(msg));
        assert(size > 0 && !sc_ed25519_sign(g_sh.ch_ed_priv, msg, (size_t)size, g_sh.ch_sig));
    }
}

/* У exit останавливаем обычный MEMBERS, чтобы до успеха J его локальный состав оставался прежним. */
static etcp_recv_fn member_receive;
static void hold_members(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn;
    queue_dgram_free(entry);
    queue_entry_free(entry);
}

static etcp_recv_fn join_receive;
static int ready_dropped;
static void drop_join_ready(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (test_join_open(conn->instance, entry->dgram, entry->len, NULL, NULL) == CS_MSG_JOIN_READY) {
        ready_dropped++;
        queue_dgram_free(entry); queue_entry_free(entry);
        return;
    }
    join_receive(conn, entry);
}

static void write_configs(const char* dir) {
    for (int i = 0; i < 4; i++) {
        char priv[65], pub[65], path[512], dbd[512];
        to_hex(g_sh.seed[i], 32, priv);
        to_hex(g_sh.x_pub[i], 32, pub);
        snprintf(path, sizeof(path), "%s/%s.conf", dir, i == IDX_A ? "a" : i == IDX_C ? "c" : i == IDX_B ? "b" : "j");
        snprintf(dbd, sizeof(dbd), "%s/db%s", dir, i == IDX_A ? "a" : i == IDX_C ? "c" : i == IDX_B ? "b" : "j");
        utun_mkdir(dbd, 0755);

        wf(path,
            "[global]\ntun_ip=10.98.%d.1/24\ntun_ifname=tun%d0\ndb_path=%s/db%s\n"
            "my_private_key=%s\nmy_public_key=%s\n"
            "[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n[allowed_keys]\nallow_all=1\n",
            i, i, dir, i == IDX_A ? "a" : i == IDX_C ? "c" : i == IDX_B ? "b" : "j",
            priv, pub);
    }
}

/* ── poll-хелперы (дети) ── */

// Родитель не выбирает порты заранее: A публикует endpoint своего уже открытого слушателя.
static int publish_bootstrap_port(struct UTUN_INSTANCE* inst, const char* dir, const char* role) {
    char path[768], temporary[768];
    snprintf(path, sizeof(path), "%s/%s.port", dir, role); snprintf(temporary, sizeof(temporary), "%s/%s.port.tmp", dir, role);
    uint16_t port = ss_get_port(&test_etcp_bound_socket(inst, "s1")->local_addr);
    FILE* f = fopen(temporary, "w");
    if (!f) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "JOIN fixture: endpoint file open failed: %s", strerror(errno)); return 0; }
    int ok = fprintf(f, "%u\n", port) > 0;
    if (fclose(f)) ok = 0;
    if (!ok || rename(temporary, path)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "JOIN fixture: endpoint publication failed: %s", strerror(errno)); return 0;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "JOIN fixture: bootstrap listener port=%u published", port);
    return 1;
}

static uint16_t wait_bootstrap_port(struct UTUN_INSTANCE* inst, const char* dir, const char* role) {
    char path[768]; snprintf(path, sizeof(path), "%s/%s.port", dir, role);
    uint64_t deadline = get_time_tb() + 50000;
    while (get_time_tb() < deadline) {
        FILE* f = fopen(path, "r");
        if (f) {
            unsigned port = 0; int count = fscanf(f, "%u", &port), error = fclose(f);
            if (count != 1 || !port || port > UINT16_MAX || error) {
                DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "JOIN fixture: invalid bootstrap endpoint count=%d port=%u error=%d", count, port, error);
                return 0;
            }
            DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "JOIN fixture: bootstrap listener port=%u received", port);
            return (uint16_t)port;
        }
        if (errno != ENOENT) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "JOIN fixture: endpoint read failed: %s", strerror(errno)); return 0; }
        poll_checked(inst);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "JOIN fixture: bootstrap endpoint publication timeout"); return 0;
}

static int wait_conn(struct UTUN_INSTANCE* inst, uint64_t nid, int max_iter) {
    for (int a = 0; a < max_iter; a++) {
        struct ETCP_CONN* c = instance_find_conn(inst, nid);
        if (c && c->initialized && c->links_up) return 1;
        poll_checked(inst);
    }
    {
        struct ETCP_CONN* c = instance_find_conn(inst, nid);
        fprintf(stderr, "  [conn-fail] self=0x%016llx target=0x%016llx conn=%p init=%d links_up=%d\n",
            (unsigned long long)inst->node_id, (unsigned long long)nid, (void*)c,
            c ? c->initialized : -1, c ? c->links_up : -1);
    }
    return 0;
}

static int wait_bgp(struct UTUN_INSTANCE* inst, uint64_t nid, int max_iter) {
    uint64_t gid = topo_groups_channel_slot(inst->topo_groups, g_sh.ch_id);
    for (int a = 0; a < max_iter; a++) {
        struct TOPO_GROUP* g = topo_groups_find(inst->topo_groups, gid);
        if (g) {
            struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(g, nid);
            if (topo_node_best_path(nq)) return 1;
        }
        poll_checked(inst);
    }
    {
        struct TOPO_GROUP* g = topo_groups_find(inst->topo_groups, gid);
        struct TOPO_GROUP_NODE* nq = g ? topo_node_find_by_id(g, nid) : NULL;
        fprintf(stderr, "  [bgp-fail] gid=%016llx group=%p nq=%p paths_head=%p self=0x%016llx target=0x%016llx\n",
            (unsigned long long)gid, (void*)g, (void*)nq,
            (void*)(nq && nq->paths ? nq->paths->head : NULL),
            (unsigned long long)inst->node_id, (unsigned long long)nid);
    }
    return 0;
}

/* Ждать подпись нужного пригласившего и проверить её на сохранённом JOIN этого мембера. */
static int wait_signed(struct UTUN_INSTANCE* inst, int person, const uint8_t* want_sb, int max_iter) {
    uint64_t nid = g_sh.nid[person];
    for (int a = 0; a < max_iter; a++) {
        uint8_t sb[32] = {0}; uint8_t sig[64];
        if (!topo_node_sqlite_member_get_sign(inst->topo_sqlite_db, g_sh.ch_id, g_sh.ed_pub[person], sb, sig) && !memcmp(sb, want_sb, 32)) {
            uint8_t join[64]; struct ms_member_rec rec = {.node_id=nid, .x25519=g_sh.x_pub[person], .ed25519=g_sh.ed_pub[person],
                .signature=sig, .signed_by=member_sync_signer(sb), .join_sig=join};
            int rc = topo_node_sqlite_member_get_join(inst->topo_sqlite_db, g_sh.ch_id, g_sh.ed_pub[person], join, &rec.join_ts);
            if (!rc && member_sync_verify_signature(inst->topo_sqlite_db, g_sh.ch_id, &rec) == 1) return 1;
            fprintf(stderr, "  invalid invitation proof node=%016llx signer=%s JOIN read rc=%d\n",
                (unsigned long long)nid, sc_public_key_hex(sb, (char[65]){0}), rc);
            return 0;
        }
        poll_checked(inst);
    }
    {
        uint8_t sb[32] = {0}; uint8_t sig[64];
        int gr = topo_node_sqlite_member_get_sign(inst->topo_sqlite_db, g_sh.ch_id, g_sh.ed_pub[person], sb, sig);
        fprintf(stderr, "  [signed-fail] nid=0x%016llx want_sb=%s get_sign_rc=%d cur_sb=%s\n",
            (unsigned long long)nid, sc_public_key_hex(want_sb, (char[65]){0}), gr, sc_public_key_hex(sb, (char[65]){0}));
    }
    return 0;
}

static int wait_not_member(struct UTUN_INSTANCE* inst, uint64_t nid, int max_iter) {
    for (int a = 0; a < max_iter; a++) {
        if (topo_node_sqlite_member_in_channel(inst->topo_sqlite_db, g_sh.ch_id, nid)) return 0;
        poll_checked(inst);
    }
    return 1;
}

static int wait_file(struct UTUN_INSTANCE* inst, const char* path, int max_iter) {
    for (int a = 0; a < max_iter; a++) {
        if (access(path, F_OK) == 0) return 1;
        poll_checked(inst);
    }
    return 0;
}

/* CHAT_EVT_CONNECT_RESULT с успехом приходит после публикации локального допуска. */
static int g_j_join_ok = 0;
static int g_j_join_finished, g_j_join_result;

static void join_event_handler(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    if (type == CHAT_EVT_CONNECT_RESULT && len == CHAT_CONNECT_RESULT_SIZE) {
        int r = 0; memcpy(&r, data + CHAT_CONNECT_RESULT_CODE_OFF, 4);
        g_j_join_finished++; g_j_join_result = r;
        if (r == 0) {
            char id[65]; sc_public_key_hex(data + CHAT_CONNECT_RESULT_GROUP_OFF, id);
            uint64_t group = topo_groups_channel_slot(inst->topo_groups, id);
            assert(topo_group_chat_key_allowed(inst, group, inst->my_ed25519_pubkey));
            g_j_join_ok = 1;
        }
    }
}

static int wait_join_result(struct UTUN_INSTANCE* inst, int max_iter) {
    for (int a = 0; a < max_iter && !g_j_join_ok; a++) poll_checked(inst);
    return g_j_join_ok;
}

/* ── настройка канала и мемберов ── */

static void channel_put_shared(struct UTUN_INSTANCE* inst, int has_priv) {
    topo_node_sqlite_channel_put(inst->topo_sqlite_db, g_sh.ch_id, CH_NAME, g_sh.ed_pub[IDX_A],
        g_sh.ch_x_pub, has_priv ? g_sh.ch_x_priv : NULL,
        g_sh.ch_ed_pub, has_priv ? g_sh.ch_ed_priv : NULL, g_sh.ch_sig);
    chat_core_ensure_channel_ready(inst, g_sh.ch_id);
}

static void member_put_self(struct UTUN_INSTANCE* inst, const char* name) {
    uint64_t join_ts = (uint64_t)ntp_time_get_seconds(inst);
    uint8_t jm[MEMBER_JOIN_MSG_SIZE];
    int jl = member_sync_build_join_msg(g_sh.ch_x_pub, g_sh.ch_ed_pub, inst->my_ed25519_pubkey,
                                        inst->my_keys.public_key, jm, (int)sizeof(jm));
    uint8_t join_sig[64]; sc_ed25519_sign(inst->my_ed25519_privkey, jm, (size_t)jl, join_sig);
    uint8_t root_sig[64];
    member_sync_sign_join(g_sh.ch_ed_priv, g_sh.ch_id, NULL, inst->my_keys.public_key, inst->my_ed25519_pubkey,
        TEST_JOIN_ID(g_sh.ch_x_pub, g_sh.ch_ed_pub, inst->my_ed25519_pubkey, inst->my_keys.public_key), join_ts, root_sig);
    char userinfo[256]; snprintf(userinfo, sizeof(userinfo), "{\"name\":\"%s\"}", name);
    uint8_t update[64]; assert(!test_member_profile(inst->my_ed25519_privkey,
        TEST_JOIN_ID(g_sh.ch_x_pub, g_sh.ch_ed_pub, inst->my_ed25519_pubkey, inst->my_keys.public_key), join_ts, userinfo, update));
    member_sync_put(inst, g_sh.ch_id, inst->my_keys.public_key, inst->my_ed25519_pubkey,
                    join_sig, join_ts, update, join_ts, userinfo, NULL, NULL, 0, 0, root_sig);
}

static void member_put_fixture(struct UTUN_INSTANCE* inst, int idx, const char* name) {
    /* A/C уже члены канала: подписать полную запись известными родителю ключами.
     * J этим путём не добавляется: он проходит настоящий протокол приглашения. */
    uint64_t ts = (uint64_t)ntp_time_get_seconds(inst);
    uint8_t message[MEMBER_JOIN_MSG_SIZE], join_sig[64], root_sig[64], update[64];
    const uint8_t *private_key = g_sh.seed[idx], *public_key = g_sh.ed_pub[idx];
    int size = member_sync_build_join_msg(g_sh.ch_x_pub, g_sh.ch_ed_pub, g_sh.ed_pub[idx], g_sh.x_pub[idx],
                                           message, sizeof(message));
    char userinfo[128]; snprintf(userinfo, sizeof(userinfo), "{\"name\":\"%s\"}", name);
    if (size <= 0 || sc_ed25519_sign(private_key, message, (size_t)size, join_sig) != SC_OK ||
        member_sync_sign_join(g_sh.ch_ed_priv, g_sh.ch_id, NULL, g_sh.x_pub[idx], public_key,
            TEST_JOIN_ID(g_sh.ch_x_pub, g_sh.ch_ed_pub, g_sh.ed_pub[idx], g_sh.x_pub[idx]), ts, root_sig) != 0 ||
        test_member_profile(private_key, TEST_JOIN_ID(g_sh.ch_x_pub, g_sh.ch_ed_pub, g_sh.ed_pub[idx], g_sh.x_pub[idx]), ts, userinfo, update) ||
        member_sync_put(inst, g_sh.ch_id, g_sh.x_pub[idx], public_key,
                        join_sig, ts, update, ts, userinfo, NULL, NULL, 0, 0, root_sig) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "test: signed member fixture failed node=%016llx", (unsigned long long)g_sh.nid[idx]);
        abort();
    }
}

static int gui_results, gui_result_valid;
static void gui_invite_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    if (type != CHAT_EVT_INVITE_LINK_READY) return;
    gui_results++;
    if (len < 11 || len < 1 + data[0] + 10) return;
    uint64_t request_id; uint16_t link_len;
    memcpy(&request_id, data + 1 + data[0], 8);
    memcpy(&link_len, data + 1 + data[0] + 8, 2);
    if (len != 1 + data[0] + 10 + link_len || request_id != 102 || !link_len) return;
    struct InviteData invite; char error[128];
    gui_result_valid = invite_link_decode((const char*)data + 1 + data[0] + 10, link_len, &invite, error, sizeof(error)) == 0
        && !memcmp(invite.inviter_pubkey, inst->my_ed25519_pubkey, 32)
        && chat_join_key_matches(inst, chat_join_key_group(inst, invite.join_key), invite.join_key, inst->my_ed25519_pubkey);
}

static int check_gui_superseded(struct UTUN_INSTANCE* inst) {
    gui_results = gui_result_valid = 0;
    chat_event_set_handler(inst, gui_invite_event);
    for (uint64_t id = 101; id <= 102; id++) {
        struct chat_invite_build_req* req = u_calloc(1, sizeof(*req));
        if (!req) abort();
        req->inst = inst; memcpy(req->target, inst->my_ed25519_pubkey, 32); req->request_id = id;
        snprintf(req->ch_id, sizeof(req->ch_id), "%s", g_sh.ch_id);
        chat_invite_build_link_trampoline(req);
    }
    int deferred = gui_results == 0;
    uint64_t deadline = get_time_tb() + 10000;
    while (!gui_results && get_time_tb() < deadline) poll_checked(inst);
    chat_event_set_handler(inst, join_event_handler);
    return deferred && gui_results == 1 && gui_result_valid && !inst->invite_gui_request;
}

/* Реальный JSON control socket: параллельные invite, quit закрывает клиента и отбрасывает хвост команд. */
static int check_headless_invites(struct UTUN_INSTANCE* inst) {
    int ok = 0, used = 0, lines = 0, eof = 0; char reply[4096] = {0}; const char* stage = "listener";
    socket_t fd = SOCKET_INVALID;
    if (chat_headless_control_init(inst->ua, inst, "127.0.0.1", 0) != 0) return 0;
    uint16_t port = chat_headless_control_get_port(inst);
    if (!port) return 0;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCKET_INVALID) goto out;
    struct sockaddr_in addr = {0}; addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) goto out;
    socket_set_nonblocking(fd);
    char command[1024]; stage = "parallel invites";
    int n = snprintf(command, sizeof(command),
        "{\"id\":71,\"cmd\":\"invite\",\"ch\":\"%s\",\"ed25519\":\"%s\"}\n"
        "{\"id\":72,\"cmd\":\"invite\",\"ch\":\"%s\",\"ed25519\":\"%s\"}\n",
        g_sh.ch_id, sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}), g_sh.ch_id, sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}));
    if (send(fd, command, n, 0) != n) goto out;
    uint64_t deadline = get_time_tb() + 10000;
    while (lines < 2 && get_time_tb() < deadline) {
        poll_checked(inst);
        ssize_t r = recv(fd, reply + used, sizeof(reply) - 1 - used, 0);
        if (r > 0) {
            for (int i = 0; i < r; i++) if (reply[used + i] == '\n') lines++;
            used += (int)r; reply[used] = 0;
        }
    }
    if (lines != 2 || !strstr(reply, "\"id\":71,\"ok\":true") || !strstr(reply, "\"id\":72,\"ok\":true")) goto out;
    uint64_t last_key = 0; stage = "invite decode";
    char* pos = reply;
    for (int i = 0; i < 2; i++) {
        pos = strstr(pos, "utun://"); if (!pos) goto out;
        char* end = strchr(pos, '"'); if (!end) goto out;
        struct InviteData data; char error[128];
        if (invite_link_decode(pos, end - pos, &data, error, sizeof(error)) != 0
            || data.join_key == last_key || memcmp(data.pubkey, inst->my_keys.public_key, 32)
            || memcmp(data.inviter_pubkey, inst->my_ed25519_pubkey, 32)
            || !chat_join_key_matches(inst, chat_join_key_group(inst, data.join_key), data.join_key, inst->my_ed25519_pubkey)) goto out;
        last_key = data.join_key; pos = end;
    }
    stage = "invite/quit"; n = snprintf(command, sizeof(command),
        "{\"id\":73,\"cmd\":\"invite\",\"ch\":\"%s\",\"ed25519\":\"%s\"}\n"
        "{\"id\":74,\"cmd\":\"quit\"}\n"
        "{\"id\":75,\"cmd\":\"invite\",\"ch\":\"%s\",\"ed25519\":\"%s\"}\n",
        g_sh.ch_id, sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}), g_sh.ch_id, sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}));
    if (send(fd, command, n, 0) != n) goto out;
    used = lines = 0; reply[0] = 0; deadline = get_time_tb() + 10000;
    while (get_time_tb() < deadline) {
        poll_checked(inst);
        ssize_t r = recv(fd, reply + used, sizeof(reply) - 1 - used, 0);
        if (r == 0) { eof = 1; break; }
        if (r > 0) {
            for (int i = 0; i < r; i++) lines += reply[used + i] == '\n';
            used += (int)r; reply[used] = 0;
        }
    }
    /* READ следующей команды даёт локальному invite время завершиться до quit: такой ответ уже не отменяется. */
    const char *bye = strstr(reply, "\"id\":74,\"ok\":true"), *invite = strstr(reply, "\"id\":73");
    ok = eof && bye && (lines == 1 || lines == 2) && (!invite ||
        (invite < bye && invite == strstr(reply, "\"id\":73,\"ok\":true"))) &&
        !strstr(reply, "\"id\":75");
out:
    if (!ok) DEBUG_ERROR(DEBUG_CATEGORY_DEBUG,
        "headless invite regression: stage=%s bytes=%d lines=%d eof=%d reply=%s", stage, used, lines, eof, reply);
    if (fd != SOCKET_INVALID) socket_close_wrapper(fd);
    chat_headless_control_destroy(inst);
    chat_event_set_handler(inst, join_event_handler);
    if (!ok) fprintf(stderr, "headless: concurrent invite/quit regression failed\n");
    return ok;
}

/* ── child_main ── */

/* Проверяем выбор на реальных мемберах канала; изменения производных полей откатываем до следующего poll. */
static int check_invite_priority(struct UTUN_INSTANCE* inst) {
    uint64_t channel_id = topo_groups_channel_slot(inst->topo_groups, g_sh.ch_id); uint8_t selected[32] = {0};
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, channel_id);
    struct TOPO_GROUP_NODE* peer = topo_node_find_by_id(group, g_sh.nid[IDX_C]);
    struct TOPO_GROUP_NODE* other = topo_node_find_by_id(group, g_sh.nid[IDX_B]);
    if (!topo_node_best_path(peer)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "test invite priority: no live path to C group=%s", g_sh.ch_id); return 0;
    }
    char table[80], sql[256];
    peers_table_name(g_sh.ch_id, table, sizeof(table));
    sqlite3* db = inst->topo_sqlite_db;
    if (sqlite3_exec(db, "SAVEPOINT invite_priority", NULL, NULL, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "test invite priority: savepoint failed: %s", sqlite3_errmsg(db)); return 0;
    }
    int ok = 0;
    struct ll_queue* paths = peer->paths, *other_paths = other ? other->paths : NULL;
    if (other) other->paths = NULL;
    snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET node_type=1", table);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto out;
    if (chat_invite_best_node(inst, channel_id, selected) || memcmp(selected, g_sh.ed_pub[IDX_C], 32)) goto out;
    snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET node_type=4 WHERE node_id=%lld", table, (long long)inst->node_id);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto out;
    if (chat_invite_best_node(inst, channel_id, selected) || memcmp(selected, inst->my_ed25519_pubkey, 32)) goto out;
    snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET node_type=1 WHERE node_id=%lld", table, (long long)inst->node_id);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto out;
    snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET node_type=4 WHERE node_id=%lld", table, (long long)g_sh.nid[IDX_C]);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) goto out;
    if (chat_invite_best_node(inst, channel_id, selected) || memcmp(selected, g_sh.ed_pub[IDX_C], 32)) goto out;
    peer->paths = NULL;
    if (chat_invite_best_node(inst, channel_id, selected) || memcmp(selected, inst->my_ed25519_pubkey, 32)) goto out;
    ok = 1;
out:
    peer->paths = paths;
    if (other) other->paths = other_paths;
    if (sqlite3_exec(db, "ROLLBACK TO invite_priority; RELEASE invite_priority", NULL, NULL, NULL) != SQLITE_OK) ok = 0;
    if (!ok) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "test invite priority failed: selected=%s err=%s",
                         sc_public_key_hex(selected, (char[65]){0}), sqlite3_errmsg(db));
    else DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "test invite priority: regular, self-supernode, supernode, unreachable PASS");
    return ok;
}

static int child_main(const char* role, const char* dir, int invalid_key) {
    struct UASYNC* ua = uasync_create();
    if (!ua) { fprintf(stderr, "%s: uasync_create failed\n", role); return 1; }

    int resume = !strcmp(role, "j-resume");
    char cfg[512]; snprintf(cfg, sizeof(cfg), "%s/%s.conf", dir, resume ? "j" : role);
    struct UTUN_INSTANCE* inst = utun_instance_create(ua, cfg);
    if (!inst) { fprintf(stderr, "%s: create failed\n", role); uasync_destroy(ua, 0); return 1; }
    if (utun_core_start(inst) < 0 || chat_service_start(inst) < 0) {
        utun_instance_destroy(inst); uasync_destroy(ua, 0); return 1;
    }
    chat_event_set_handler(inst, join_event_handler);
    if ((!strcmp(role, "a") || !strcmp(role, "b")) && !publish_bootstrap_port(inst, dir, role)) return 1;
    /* Настроенный bootstrap ядра соединяет участников и CHAT без создания UTUN-группы. */
    if ((!strcmp(role, "b") || !strcmp(role, "c")) && !g_sh.degraded) {
        int upstream = !strcmp(role, "b") ? IDX_A : IDX_B;
        uint16_t port = wait_bootstrap_port(inst, dir, upstream == IDX_A ? "a" : "b");
        if (!port) return 1;
        struct CFG_CLIENT_LINK link = {0};
        struct sockaddr_in* address = (struct sockaddr_in*)&link.remote_addr;
        address->sin_family = AF_INET; address->sin_addr.s_addr = htonl(INADDR_LOOPBACK); address->sin_port = htons(port);
        struct CFG_CLIENT client = { .name = "bootstrap", .links = &link };
        to_hex(g_sh.x_pub[upstream], 32, client.peer_public_key_hex);
        if (init_client_connections(inst, &client) < 0) return 1;
        assert(!inst->utun_started && !topo_groups_get_default(inst->topo_groups));
    }
    int rc = 1;

    if (strcmp(role, "a") == 0) {
        if (!g_sh.degraded && !wait_conn(inst, g_sh.nid[IDX_B], 4000)) {
            fprintf(stderr, "A: no conn to B\n"); goto out;
        }
        channel_put_shared(inst, 1);
        member_put_self(inst, "A");
        if (!g_sh.degraded) {
            member_put_fixture(inst, IDX_B, "B");
            member_put_fixture(inst, IDX_C, "C");
        }

        if (!g_sh.degraded && !wait_bgp(inst, g_sh.nid[IDX_C], 8000)) {
            fprintf(stderr, "A: no BGP to C\n"); goto out;
        }
        if (!g_sh.degraded && !invalid_key && !check_invite_priority(inst)) goto out;

        if (!check_gui_superseded(inst)) { fprintf(stderr, "GUI: stale invite result\n"); goto out; }
        if (!check_headless_invites(inst)) goto out;
        const uint8_t* target = g_sh.ed_pub[g_sh.degraded ? IDX_A : IDX_C];
        struct invite_result result = {0};
        struct chat_invite_build* pending = NULL;
        /* conn_presence может отставать от paths; повторяем только ошибку запуска. */
        for (int a = 0; a < 1000 && !pending; a++) {
            pending = chat_invite_build_link(inst, topo_groups_channel_slot(inst->topo_groups, g_sh.ch_id), target, invite_ready, &result);
            if (!pending) poll_checked(inst);
        }
        uint64_t deadline = get_time_tb() + 60000;
        while (pending && !result.calls && get_time_tb() < deadline) poll_checked(inst);
        if (!result.calls && pending) chat_invite_build_cancel(pending);
        if (result.calls != 1 || result.result != CHAT_JOIN_OK || !result.link[0]) {
            fprintf(stderr, "A: invite build failed calls=%d result=%d\n", result.calls, result.result); goto out;
        }
        struct InviteData built;
        if (invite_link_decode(result.link, strlen(result.link), &built, NULL, 0) ||
            chat_join_key_group(inst, built.join_key) != topo_groups_channel_slot(inst->topo_groups, g_sh.ch_id)) goto out;
        if (g_sh.lose_ready) {
            chat_join_revoke_window_key(inst, 0);
            built.join_key = built.join_key == 1 ? 2 : built.join_key ^ UINT64_C(1);
            assert(!chat_join_store_window_key(inst, topo_groups_channel_slot(inst->topo_groups, g_sh.ch_id), built.join_key, 123));
            assert(invite_link_encode(&built, result.link, sizeof(result.link)) > 0);
        }
        char lf[512];
        snprintf(lf, sizeof(lf), "%s/link.txt", dir);
        if (wf(lf, "%s", result.link)) goto out;

        if (invalid_key) {
            /* J с неверным ключом — мембером стать не должен */
            if (!wait_not_member(inst, g_sh.nid[IDX_J], 1500)) {
                fprintf(stderr, "A: J unexpectedly became member\n"); goto out;
            }
        } else {
            if (!wait_signed(inst, IDX_J, g_sh.ed_pub[IDX_A], 8000)) {
                fprintf(stderr, "A: J not signed by me\n"); goto out;
            }
            if (g_sh.lose_ready) {
                chat_join_revoke_window_key(inst, 123);
                assert(!chat_join_key_group(inst, built.join_key));
                char revoked[512]; snprintf(revoked, sizeof(revoked), "%s/revoked", dir);
                if (wf(revoked, "revoked")) goto out;
            }
        }
        rc = 0;
    } else if (strcmp(role, "b") == 0) {
        channel_put_shared(inst, 0);
        member_put_fixture(inst, IDX_A, "A");
        member_put_fixture(inst, IDX_B, "B");
        member_put_fixture(inst, IDX_C, "C");
        if (!wait_bgp(inst, g_sh.nid[IDX_A], 8000) || !wait_bgp(inst, g_sh.nid[IDX_C], 8000)) goto out;
        if (invalid_key && !wait_not_member(inst, g_sh.nid[IDX_J], 1500)) goto out;
        rc = 0;
    } else if (strcmp(role, "c") == 0) {
        if (!wait_conn(inst, g_sh.nid[IDX_B], 4000)) {
            fprintf(stderr, "C: no conn to B\n"); goto out;
        }
        channel_put_shared(inst, 0);
        member_put_fixture(inst, IDX_C, "C");
        member_put_fixture(inst, IDX_B, "B");
        member_put_fixture(inst, IDX_A, "A");
        if (!wait_bgp(inst, g_sh.nid[IDX_A], 8000)) {
            fprintf(stderr, "C: no BGP to A\n"); goto out;
        }

        struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, g_sh.ch_id);
        const struct TOPO_NODEPATH* path = topo_node_best_path(topo_node_find_by_ed(group, g_sh.ed_pub[IDX_A]));
        assert(path && path->conn && path->conn->peer_node_id == g_sh.nid[IDX_B]);
        member_receive = inst->api_bindings.callbacks[ETCP_ID_MEMBER_SYNC];
        assert(member_receive && !etcp_bind(inst, ETCP_ID_MEMBER_SYNC, hold_members));
        if (invalid_key) {
            if (!wait_not_member(inst, g_sh.nid[IDX_J], 1500)) goto out;
        } else {
            char ready[512]; snprintf(ready, sizeof(ready), "%s/join_ready", dir);
            if (!wait_file(inst, ready, 8000)) { fprintf(stderr, "C: J did not complete JOIN\n"); goto out; }
            if (topo_node_sqlite_member_in_channel(inst->topo_sqlite_db, g_sh.ch_id, g_sh.nid[IDX_J])) {
                fprintf(stderr, "C: J unexpectedly entered the exit membership table before regular sync resumed\n"); goto out;
            }
            assert(!inst->join_keys && !etcp_bind(inst, ETCP_ID_MEMBER_SYNC, member_receive));
        }
        rc = 0;
    } else if (resume) {
        for (unsigned i = 0; i < 8000; i++) {
            uint64_t group = topo_groups_channel_slot(inst->topo_groups, g_sh.ch_id);
            if (topo_group_chat_key_allowed(inst, group, inst->my_ed25519_pubkey)) {
                char ready[512]; snprintf(ready, sizeof(ready), "%s/group_ready", dir);
                rc = wf(ready, "ready") != 0;
                goto out;
            }
            poll_checked(inst);
        }
        fprintf(stderr, "J: persisted JOIN did not recover after restart\n");
    } else {
        char lf[512]; snprintf(lf, sizeof(lf), "%s/link.txt", dir);
        if (!wait_file(inst, lf, 8000)) { fprintf(stderr, "J: no link file\n"); goto out; }

        char link[1024];
        { FILE* f = fopen(lf, "r"); if (!f) { fprintf(stderr, "J: link open failed\n"); goto out; }
          size_t n = fread(link, 1, sizeof(link) - 1, f); fclose(f); link[n] = '\0'; }

        struct InviteData d; char err[256];
        if (invite_link_decode(link, strlen(link), &d, err, sizeof(err)) < 0) {
            fprintf(stderr, "J: decode failed bytes=%zu prefix=%.7s: %s\n", strlen(link), link, err); goto out;
        }
        uint8_t addrs_buf[2048];
        int addrs_len = invite_serialize_addrs(&d, addrs_buf, sizeof(addrs_buf));
        if (addrs_len <= 0) { fprintf(stderr, "J: serialize addrs failed\n"); goto out; }

        uint64_t key = invalid_key ? (d.join_key ^ 0xDEADBEEFCAFEBABEULL) : d.join_key;
        d.join_key = key;
        if (g_sh.lose_ready) {
            join_receive = inst->api_bindings.callbacks[ETCP_ID_CHAT_SYNC];
            assert(join_receive && !etcp_bind(inst, ETCP_ID_CHAT_SYNC, drop_join_ready));
        }
        if (invite_link_encode(&d, link, sizeof(link)) < 0 || chat_sync_join_link(inst, 1, link, strlen(link))) goto out;

        if (g_sh.lose_ready) {
            char revoked[512]; snprintf(revoked, sizeof(revoked), "%s/revoked", dir);
            for (unsigned i = 0; i < 8000 && !ready_dropped; i++) poll_checked(inst);
            if (!ready_dropped || !wait_file(inst, revoked, 8000) || g_j_join_finished) goto out;
            assert(!topo_group_chat_key_allowed(inst, topo_groups_channel_slot(inst->topo_groups, g_sh.ch_id), inst->my_ed25519_pubkey));
            utun_instance_destroy(inst); uasync_destroy(ua, 0);
            _exit(0);
        } else if (invalid_key) {
            for (unsigned i = 0; i < 8000 && !g_j_join_finished; i++) poll_checked(inst);
            rc = g_j_join_finished == 1 && g_j_join_result != 0 && !g_j_join_ok
                && topo_node_sqlite_channel_get(inst->topo_sqlite_db, g_sh.ch_id, NULL, 0, NULL, NULL, NULL, NULL) != 0
                && wait_not_member(inst, g_sh.nid[IDX_J], 200) ? 0 : 1;
            if (rc) fprintf(stderr, "J: wrong key failed to deny description/membership: finished=%d result=%d\n",
                            g_j_join_finished, g_j_join_result);
        } else {
            /* J получил результат JOIN после локального допуска. */
            if (!wait_join_result(inst, 8000)) {
                fprintf(stderr, "J: local admission was not published\n"); goto out;
            }
            char ready[512]; snprintf(ready, sizeof(ready), "%s/join_ready", dir);
            wf(ready, "ready");
            uint64_t connection_node = g_sh.nid[g_sh.degraded ? IDX_A : IDX_C];
            if (g_sh.degraded && !wait_bgp(inst, connection_node, 8000)) {
                fprintf(stderr, "J: BGP did not recover after local admission\n"); goto out;
            }
            snprintf(ready, sizeof(ready), "%s/group_ready", dir);
            wf(ready, "ready");
            rc = 0;
        }
    }

    /* Участники обслуживают обмен до публикации допуска на J. */
    if (rc == 0 && !invalid_key && strcmp(role, "j") != 0) {
        char ready[512]; snprintf(ready, sizeof(ready), "%s/group_ready", dir);
        if (!wait_file(inst, ready, 8000)) { fprintf(stderr, "%s: join delivery not confirmed\n", role); rc = 1; }
    }

out:
    if (early_bgp_membership) rc = 1;
    fprintf(stderr, "%s: %s\n", role, rc == 0 ? "OK" : "FAIL");
    fflush(stdout); fflush(stderr);
    _exit(rc);
}

/* ── parent ── */

static int run_scenario(const char* name, int degraded, int invalid_key, int lose_ready) {
    char dir[512]; snprintf(dir, sizeof(dir), "/tmp/utun_e2e_%s_XXXXXX", name);
    if (test_mkdtemp(dir)) { perror("join scenario temporary directory"); return 0; }

    fill_shared(degraded);
    g_sh.lose_ready = lose_ready;
    write_configs(dir);

    const char* direct[] = {"a", "j", NULL};
    const char* routed[] = {"a", "b", "c", "j", NULL};
    const char** roles = degraded ? direct : routed;
    pid_t pids[4]; int n = 0;
    for (int i = 0; roles[i]; i++) {
        pid_t p = fork();
        if (p == 0) {
            int r = child_main(roles[i], dir, invalid_key);
            _exit(r);
        }
        if (p > 0) pids[n++] = p;
    }

    int ok = n == (degraded ? 2 : 4);
    if (lose_ready && n == 2) {
        int st;
        if (waitpid(pids[1], &st, 0) != pids[1] || !WIFEXITED(st) || WEXITSTATUS(st)) { ok = 0; n--; }
        else {
            pid_t guest = fork();
            if (guest == 0) _exit(child_main("j-resume", dir, 0));
            if (guest < 0) { ok = 0; n--; }
            else pids[1] = guest;
        }
    }
    for (int i = 0; i < n; i++) {
        int st = 0; waitpid(pids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) ok = 0;
    }

    char cmd[512]; snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); (void)!system(cmd);
    return ok;
}

static int parent_main(void) {
    printf("=== test_chat_join_e2e ===\n"); fflush(stdout);

    TEST("A!=C: initial JOIN succeeds before exit learns J"); {
        if (run_scenario("happy", 0, 0, 0)) OK(); else FAIL();
    }
    TEST("degraded A==C: J joins directly via A"); {
        if (run_scenario("deg", 1, 0, 0)) OK(); else FAIL();
    }
    TEST("invalid key: J rejected (no member)"); {
        if (run_scenario("inv", 0, 1, 0)) OK(); else FAIL();
    }
    TEST("lost READY: J recovers after restart and link revocation"); {
        if (run_scenario("lost_ready", 1, 0, 1)) OK(); else FAIL();
    }

    printf("\n%d/%d passed, %d failed\n", G_PASSED, G_TOTAL, G_FAILED);
    return G_FAILED > 0 ? 1 : 0;
}

int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level_by_name("chat", "debug");
        debug_set_category_level_by_name("chat_sync", "info");
        debug_set_category_level_by_name("member_sync", "trace");
        debug_set_category_level_by_name("etcp_route", "info");
        debug_set_category_level_by_name("bgp", "trace");
    }
    if (getenv("UTUN_TEST_JOIN_LOG")) {
        debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_INFO);
        debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_INFO);
    }
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));

    if (argc >= 3) {
        int invalid = (argc >= 4 && strcmp(argv[3], "invalid") == 0);
        return child_main(argv[1], argv[2], invalid);
    }
    return parent_main();
}

#endif /* !_WIN32 */
