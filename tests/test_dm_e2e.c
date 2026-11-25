// test_dm_e2e.c — интеграционный тест прямого чата (DM): прямая отправка + offline-хранение.
//
// Однопоточный: 3 узла (A, B, C) в одном процессе, один UASYNC, без fork.
// Топология — цепочка B→C→A (A владелец канала, C storage, B target).
// Сценарий управляется одной master state-machine (таймер dm_tick).
//
// Сценарий:
//   фаза 1 (прямая): A,B,C online. A dm_start(B) → dm_send("hello1") → B получает напрямую.
//   фаза 2 (offline): B1 завершается (A теряет B). A dm_send("hello2") → недоступен →
//                     кладёт в mailbox на C (storage=yes). C видит блоб в dm_mail.
//   фаза 3 (reconnect): B2 переподключается к C → dm_on_conn_status → PULL mailbox → получает "hello2".

#include "chat_core.h"
#include "chat_sync.h"
#include "member_sync.h"
#include "../dm/dm_core.h"
#include "../dm/dm_mailbox.h"
#include "../dm/dm_crypto.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/etcp_api.h"
#include "../routing_layer/etcp_router.h"
#include "../transport_layer/secure_channel.h"
#include "../ntp_time.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_clock.h"
#include "test_net.h"

#include <sqlite3.h>
#define OPENSSL_API_COMPAT 0x10100000L
#include "test_membership.h"
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#define GROUP_SLOT UINT64_C(4242424242424242)
#define CH_ID "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951" /* Ed25519 для seed {94}. */

#define IDX_A 0
#define IDX_B 1
#define IDX_C 2

#define TICK_TB     100      /* 10 ms на такт state machine */
#define MAX_TICKS   3000     /* глобальный таймаут ≈ 30 c */
#define OFFLINE_BATCH 40     /* Больше лимита одной порции durable pump. */

/* фазы master state-machine */
enum dm_phase {
    P_WAIT_CONN,       /* ждём подъёма всех соединений */
    P_SETUP,           /* однократная настройка канала/мемберов на A,B,C */
    P_WAIT_BGP,        /* ждём сходимости BGP CHAT-группы */
    P_SEND1,           /* A: dm_start + hello1 */
    P_WAIT_MSG1,       /* ждём hello1 у B */
    P_B_OFFLINE,       /* destroy B1 */
    P_WAIT_B_OFFLINE,  /* ждём ухода B из BGP у A */
    P_SEND2,           /* A: hello2 → mailbox */
    P_WAIT_MAIL,       /* ждём блоб в dm_mail у C */
    P_RECONNECT,       /* create B2 */
    P_WAIT_B2,         /* ждём hello2 у B2 и опустошение dm_mail у C */
    P_DONE,
};

struct dm_ctx {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst[3];
    enum dm_phase phase;
    int   result;        /* 0=running, 1=ok, 2=fail */
    int   ticks;
    void* timer;
};

/* общие данные (ключи узлов и канала) */
struct dm_shared {
    uint8_t  ch_x_pub[32], ch_ed_pub[32], ch_ed_priv[32], ch_sig[64];
    uint64_t nid[3];
    uint8_t  x_pub[3][32], seed[3][32], ed_pub[3][32];
    int      port[3];
    char     conv_id[64];
};

static struct dm_shared g_sh;

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

/* ── генерация данных сценария + конфигов ── */

static void fill_shared(void) {
    memset(&g_sh, 0, sizeof(g_sh));

    for (int i = 0; i < 3; i++) {
        uint8_t transport_secret[32];
        if (sc_identity_generate(g_sh.x_pub[i], transport_secret, g_sh.ed_pub[i], g_sh.seed[i])) abort();
        g_sh.nid[i] = sc_derive_node_id_from_pubkey(g_sh.x_pub[i]);
    }

    uint8_t x_private[32];
    g_sh.ch_ed_priv[0] = 94;
    if (sc_identity_from_seed(g_sh.ch_ed_priv, g_sh.ch_x_pub, x_private, g_sh.ch_ed_pub) ||
        test_group_signature(CH_ID, "dm-test", g_sh.ed_pub[IDX_A], g_sh.ch_x_pub, g_sh.ch_ed_pub, g_sh.ch_ed_priv, g_sh.ch_sig)) abort();

    uint64_t conv = dm_derive_conv_id(g_sh.ed_pub[IDX_A], g_sh.ed_pub[IDX_B]);
    snprintf(g_sh.conv_id, sizeof(g_sh.conv_id), "%llu", (unsigned long long)conv);
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
        if (i == IDX_C) {
            /* C → A (клиент к A) */
            char apub[65]; to_hex(g_sh.x_pub[IDX_A], 32, apub);
            snprintf(client, sizeof(client),
                "[client: to_a]\npeer_public_key=%s\nlink=s1:127.0.0.1:%d\n",
                apub, g_sh.port[IDX_A]);
        } else if (i == IDX_B) {
            /* B → C (клиент к C) */
            char cpub[65]; to_hex(g_sh.x_pub[IDX_C], 32, cpub);
            snprintf(client, sizeof(client),
                "[client: to_c]\npeer_public_key=%s\nlink=s1:127.0.0.1:%d\n",
                cpub, g_sh.port[IDX_C]);
        }

        wf(path,
            "[global]\ntun_ip=10.97.%d.1/24\ntun_ifname=tun%d0\ndb_path=%s/db%s\n"
            "my_private_key=%s\nmy_public_key=%s\n"
            "[server: s1]\naddr=127.0.0.1:%d\ntype=public\n"
            "%s"
            "[chatserver]\nstorage_autoload=0\n[allowed_keys]\nallow_all=1\n",
            i, i, dir, i == IDX_A ? "a" : i == IDX_B ? "b" : "c",
            priv, pub, g_sh.port[i], client);
    }
}

/* ── чистые проверки (без опроса) ── */

static int conn_up(struct UTUN_INSTANCE* inst, uint64_t nid) {
    if (!inst) return 0;
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

/* есть ли расшифрованный текст needle в беседе (одна проверка) */
static int dm_msg_has(struct UTUN_INSTANCE* inst, const char* needle) {
    char buf[8192]; size_t len = 0;
    if (dm_list_messages_json(inst, g_sh.conv_id, 100, 0, buf, sizeof(buf), &len) != 0) return 0;
    return strstr(buf, needle) != NULL;
}

/* количество блобов в dm_mail для recipient=B от sender=A (одна проверка) */
static int dm_mail_count(struct UTUN_INSTANCE* inst) {
    sqlite3_stmt* s = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(inst->topo_sqlite_db,
            "SELECT COUNT(*) FROM dm_pending_messages WHERE recipient=? AND sender=?",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(s, 1, g_sh.ed_pub[IDX_B], 32, SQLITE_STATIC);
        sqlite3_bind_blob(s, 2, g_sh.ed_pub[IDX_A], 32, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    }
    if (s) sqlite3_finalize(s);
    return n;
}

/* Точная проверка durable исходящей очереди; ошибка SQL не маскируется нулём. */
static int outbox_count(struct UTUN_INSTANCE* inst) {
    sqlite3_stmt* st = NULL;
    int count = -1;
    if (sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT COUNT(*) FROM dm_outbox", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return count;
}

/* Все 40 последовательных сообщений сохранены; максимум seq сам по себе этого не доказывает. */
static int offline_batch_received(struct UTUN_INSTANCE* inst) {
    sqlite3_stmt* st = NULL;
    int count = -1;
    if (sqlite3_prepare_v2(inst->topo_sqlite_db,
            "SELECT COUNT(*) FROM dm_messages WHERE conv_id=? AND dir=0 AND seq>=2 AND seq<?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, g_sh.conv_id, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 2, OFFLINE_BATCH + 2);
        if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return count == OFFLINE_BATCH;
}

/* Каноническое сообщение реального автора для проверок порядка, подписи и commit. */
static size_t test_body(struct UTUN_INSTANCE* author, uint64_t seq, const char* text, uint8_t body[512]) {
    uint64_t conv = strtoull(g_sh.conv_id, NULL, 10), ts = 1;
    uint8_t key[32], nonce[DM_NONCE_SIZE];
    if (dm_derive_content_key(author->my_keys.private_key, g_sh.x_pub[IDX_B], key) != 0) return 0;
    dm_build_nonce(conv, author->my_ed25519_pubkey, seq, nonce);
    memcpy(body, &conv, 8);
    memcpy(body + 8, &seq, 8);
    memcpy(body + 16, &ts, 8);
    memcpy(body + DM_MSG_AUTHOR_OFF, author->my_ed25519_pubkey, 32);
    body[DM_MSG_CT_LEN_OFF] = 4;
    memcpy(body + DM_MSG_FIXED_HDR, "text", 4);
    size_t enc_len = 0;
    if (media_ccm_encrypt(key, nonce, (const uint8_t*)text, strlen(text), body + DM_MSG_FIXED_HDR + 6, &enc_len) != 0) return 0;
    uint16_t n = (uint16_t)enc_len;
    memcpy(body + DM_MSG_FIXED_HDR + 4, &n, 2);
    if (sc_ed25519_sign(author->my_ed25519_privkey, body, DM_MSG_FIXED_HDR + 6 + enc_len, body + DM_MSG_FIXED_HDR + 6 + enc_len) != SC_OK) return 0;
    return DM_MSG_FIXED_HDR + 6 + enc_len + 64;
}

/* Вход сервиса после аутентификации router; отдельно от реального UDP-сценария ниже. */
static int mailbox_packet(struct UTUN_INSTANCE* dst, uint64_t source, uint8_t cmd, const uint8_t* body, size_t len) {
    struct ETCP_CONN* conn = instance_find_conn(dst, dst->node_id == g_sh.nid[IDX_C] ? g_sh.nid[IDX_A] : g_sh.nid[IDX_C]);
    if (!conn || !dst->router_bindings.callbacks[ETCP_RT_ID_DM_MAILBOX]) return -1;
    struct ll_entry* e = queue_entry_new(0);
    if (!e) return -1;
    e->len = (uint16_t)(ROUTER_SVC_PAYLOAD_OFF + 1 + len);
    e->dgram = u_calloc(1, e->len);
    if (!e->dgram) { queue_entry_free(e); return -1; }
    uint64_t gid = GROUP_SLOT;
    e->dgram[0] = ETCP_RT_ID_DM_MAILBOX;
    for (int i = 0; i < 3; i++) if (g_sh.nid[i] == source) memcpy(e->dgram + ROUTER_SVC_SRC_ED_OFF, g_sh.ed_pub[i], 32);
    memcpy(e->dgram + ROUTER_SVC_GROUP_OFF, &gid, 8);
    e->dgram[ROUTER_SVC_FLAGS_OFF] = ROUTER_FLAG_SIGNED;
    e->dgram[ROUTER_SVC_PAYLOAD_OFF] = cmd;
    memcpy(e->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, body, len);
    dst->router_bindings.callbacks[ETCP_RT_ID_DM_MAILBOX](conn, e);
    return 0;
}

/* Ошибочный commit, tamper, точные повторы и ACK чужого тела не теряют pending. */
static int protocol_checks(struct UTUN_INSTANCE* A, struct UTUN_INSTANCE* B, struct UTUN_INSTANCE* C) {
    uint8_t body[512], receipt[DM_RECEIPT_SIZE], again[DM_RECEIPT_SIZE];
    size_t len = test_body(A, 100, "higher", body);
    if (!len || dm_accept_message(B, body, len, receipt) != 0 ||
        dm_accept_message(B, body, len, again) != 0 || memcmp(receipt, again, sizeof(receipt))) return -1;
    body[len - 1] ^= 1;
    if (dm_accept_message(B, body, len, again) == 0) return -1;
    body[len - 1] ^= 1;
    if (dm_accept_message(B, body, len - 1, again) == 0 ||
        dm_verify_message(A, g_sh.nid[IDX_C], body, len) == 0) return -1;
    len = test_body(A, 99, "lower", body);
    if (!len || dm_accept_message(B, body, len, receipt) != 0 || !dm_msg_has(B, "lower")) return -1;
    len = test_body(A, 99, "conflicting", body);
    if (!len || dm_accept_message(B, body, len, again) == 0) return -1;
    if (sqlite3_exec(B->topo_sqlite_db, "CREATE TEMP TRIGGER dm_fail BEFORE INSERT ON dm_messages"
                     " WHEN NEW.seq=98 BEGIN SELECT RAISE(ABORT,'test commit failure'); END", NULL, NULL, NULL) != SQLITE_OK) return -1;
    len = test_body(A, 98, "retry-after-db-error", body);
    if (!len || dm_accept_message(B, body, len, again) == 0) return -1;
    if (sqlite3_exec(B->topo_sqlite_db, "DROP TRIGGER dm_fail", NULL, NULL, NULL) != SQLITE_OK ||
        dm_accept_message(B, body, len, again) != 0) return -1;
    /* Правильная подпись B, правильные conv/seq, но хеш другого тела. */
    uint64_t seq = 1;
    memcpy(receipt + 16, &seq, 8);
    if (sc_ed25519_sign(B->my_ed25519_privkey, receipt, DM_ACK_SIG_OFF, receipt + DM_ACK_SIG_OFF) != SC_OK ||
        dm_accept_receipt(A, receipt) == 0 || outbox_count(A) != 1) return -1;
    receipt[DM_RECEIPT_SIZE - 1] ^= 1;
    if (dm_accept_receipt(A, receipt) == 0 || outbox_count(A) != 1) return -1;
    /* Ошибка durable outbox откатывает одновременно историю и seq отправителя. */
    if (sqlite3_exec(A->topo_sqlite_db, "CREATE TEMP TRIGGER dm_outbox_fail BEFORE INSERT ON dm_outbox"
                     " BEGIN SELECT RAISE(ABORT,'test outbox failure'); END", NULL, NULL, NULL) != SQLITE_OK) return -1;
    if (dm_send(A, g_sh.conv_id, "text", (const uint8_t*)"must-not-commit", 15) == 0 || outbox_count(A) != 1) return -1;
    if (sqlite3_exec(A->topo_sqlite_db, "DROP TRIGGER dm_outbox_fail", NULL, NULL, NULL) != SQLITE_OK ||
        dm_msg_has(A, "must-not-commit")) return -1;
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(A->topo_sqlite_db, "SELECT last_out_seq FROM dm_conversations WHERE conv_id=?", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, g_sh.conv_id, -1, SQLITE_STATIC);
    int seq_unchanged = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == 1;
    sqlite3_finalize(st);
    if (!seq_unchanged) return -1;
    /* Суперузел хранит только проверенные тела и удаляет только точный подтверждённый экземпляр. */
    len = test_body(A, 97, "mailbox-exact-receipt", body);
    if (!len) return -1;
    uint8_t put[MB_RECIPIENT_SIZE + 512];
    memcpy(put, B->my_ed25519_pubkey, MB_RECIPIENT_SIZE);
    memcpy(put + MB_RECIPIENT_SIZE, body, len);
    if (mailbox_packet(A, A->node_id, 1, put, len + MB_RECIPIENT_SIZE) != 0 || dm_mail_count(A) != 0) return -1; /* A без роли. */
    put[MB_RECIPIENT_SIZE + len - 1] ^= 1;
    if (mailbox_packet(C, A->node_id, 1, put, len + MB_RECIPIENT_SIZE) != 0 || dm_mail_count(C) != 0) return -1;
    put[MB_RECIPIENT_SIZE + len - 1] ^= 1;
    if (mailbox_packet(C, A->node_id, 1, put, len + MB_RECIPIENT_SIZE) != 0 || dm_mail_count(C) != 1) return -1;
    if (dm_accept_message(B, body, len, receipt) != 0) return -1;
    memcpy(again, receipt, sizeof(again));
    again[40] ^= 1;
    if (sc_ed25519_sign(B->my_ed25519_privkey, again, 72, again + 72) != SC_OK) return -1;
    if (mailbox_packet(C, B->node_id, 3, again, sizeof(again)) != 0 || dm_mail_count(C) != 1) return -1;
    if (mailbox_packet(C, C->node_id, 3, receipt, sizeof(receipt)) != 0 || dm_mail_count(C) != 1) return -1;
    if (mailbox_packet(C, B->node_id, 3, receipt, sizeof(receipt)) != 0 || dm_mail_count(C) != 0) return -1;
    if (mailbox_packet(C, A->node_id, 1, put, len + MB_RECIPIENT_SIZE) != 0 || dm_mail_count(C) != 0) return -1; /* Поздний PUT. */
    /* STORED относится к конкретному принятому транспортом PUT и точному телу, а не только conv/seq. */
    if (sqlite3_prepare_v2(A->topo_sqlite_db, "SELECT body FROM dm_outbox", -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_bytes(st, 0) > (int)sizeof(body)) { sqlite3_finalize(st); return -1; }
    len = (size_t)sqlite3_column_bytes(st, 0); memcpy(body, sqlite3_column_blob(st, 0), len); sqlite3_finalize(st);
    uint8_t stored[MB_STORED_SIZE]; memcpy(stored, body, 16);
    memcpy(stored + MB_STORED_RECIPIENT_OFF, B->my_ed25519_pubkey, MB_RECIPIENT_SIZE);
    SHA256(body, len, stored + MB_STORED_HASH_OFF);
    if (mailbox_packet(A, C->node_id, 4, stored, sizeof(stored))) return -1;
    if (sqlite3_prepare_v2(A->topo_sqlite_db, "SELECT supernode FROM dm_delivery WHERE receipt IS NULL", -1, &st, NULL) != SQLITE_OK) return -1;
    int untouched = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_NULL; sqlite3_finalize(st);
    if (!untouched || dm_mailbox_put(A, B->my_ed25519_pubkey, body, len)) return -1;
    stored[MB_STORED_HASH_OFF] ^= 1; if (mailbox_packet(A, C->node_id, 4, stored, sizeof(stored))) return -1;
    if (sqlite3_prepare_v2(A->topo_sqlite_db, "SELECT supernode FROM dm_delivery WHERE receipt IS NULL", -1, &st, NULL) != SQLITE_OK) return -1;
    untouched = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_NULL; sqlite3_finalize(st);
    if (!untouched) return -1;
    stored[MB_STORED_HASH_OFF] ^= 1; if (mailbox_packet(A, C->node_id, 4, stored, sizeof(stored))) return -1;
    if (sqlite3_prepare_v2(A->topo_sqlite_db, "SELECT supernode FROM dm_delivery WHERE receipt IS NULL", -1, &st, NULL) != SQLITE_OK) return -1;
    int confirmed = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32 &&
        !memcmp(sqlite3_column_blob(st, 0), C->my_ed25519_pubkey, 32); sqlite3_finalize(st);
    if (!confirmed || outbox_count(A) != 1) return -1;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "STORED checks passed: unsolicited/wrong hash rejected; exact submitted PUT confirmed; outbox retained");
    DEBUG_INFO(DEBUG_CATEGORY_DM, "protocol checks passed: reorder, dedup, tamper, DB rollback, exact receipt");
    return 0;
}

/* ── настройка канала и мемберов (однократно) ── */

static void channel_put_shared(struct UTUN_INSTANCE* inst, int has_priv) {
    topo_node_sqlite_channel_put(inst->topo_sqlite_db, CH_ID, "dm-test", g_sh.ed_pub[IDX_A],
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

/* мембер C со флагом storage=yes (adm_tags подписан канальным ключом) */
static void member_put_storage(struct UTUN_INSTANCE* inst, int idx, const char* name) {
    char userinfo[256]; snprintf(userinfo, sizeof(userinfo), "{\"name\":\"%s\"}", name);
    char adm_tags[80]; snprintf(adm_tags, sizeof(adm_tags), "{\"storage\":\"yes\",\"supernode\":\"yes\",\"ver\":\"1\"}");
    uint8_t ats[64];
    uint8_t amsg[256]; int size = member_sync_build_owner_msg(CH_ID, g_sh.ed_pub[idx], adm_tags, amsg, sizeof(amsg));
    if (size < 0 || sc_ed25519_sign(g_sh.ch_ed_priv, amsg, (size_t)size, ats)) abort();
    const uint8_t* private = g_sh.seed[idx];
    if (test_member_seed(inst, CH_ID, g_sh.x_pub[idx], g_sh.ed_pub[idx], private,
        g_sh.ch_ed_priv, userinfo, adm_tags, ats) < 0) abort();
}

/* однократная настройка роли */
static void do_setup(struct UTUN_INSTANCE* inst, int role) {
    uint64_t nc = g_sh.nid[IDX_C];
    switch (role) {
    case IDX_A:
        channel_put_shared(inst, 1);
        member_put_self(inst, "A");
        member_put_placeholder(inst, IDX_B, "B");
        member_put_storage(inst, IDX_C, "C");
        member_sync_start(inst, instance_find_conn(inst, nc), CH_ID, NULL, NULL);
        break;
    case IDX_B:
        channel_put_shared(inst, 0);
        member_put_self(inst, "B");
        member_put_placeholder(inst, IDX_A, "A");
        member_put_storage(inst, IDX_C, "C");
        member_sync_start(inst, instance_find_conn(inst, nc), CH_ID, NULL, NULL);
        break;
    case IDX_C:
        channel_put_shared(inst, 0);
        member_put_self(inst, "C");
        member_put_storage(inst, IDX_C, "C");
        member_put_placeholder(inst, IDX_A, "A");
        member_put_placeholder(inst, IDX_B, "B");
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_A]), CH_ID, NULL, NULL);
        member_sync_start(inst, instance_find_conn(inst, g_sh.nid[IDX_B]), CH_ID, NULL, NULL);
        break;
    }
}

/* ── master state-machine (движется таймером) ── */

static void dm_tick(void* arg) {
    struct dm_ctx* t = arg;
    t->timer = NULL;
    if (t->result) return;

    struct UTUN_INSTANCE* A = t->inst[IDX_A];
    struct UTUN_INSTANCE* B = t->inst[IDX_B];
    struct UTUN_INSTANCE* C = t->inst[IDX_C];
    uint64_t na = g_sh.nid[IDX_A], nb = g_sh.nid[IDX_B], nc = g_sh.nid[IDX_C];

    switch (t->phase) {
    case P_WAIT_CONN:
        /* C→A и B→C должны быть up */
        if (conn_up(A, nc) && conn_up(C, na) && conn_up(B, nc) && conn_up(C, nb))
            t->phase = P_SETUP;
        break;
    case P_SETUP:
        do_setup(A, IDX_A);
        do_setup(B, IDX_B);
        do_setup(C, IDX_C);
        t->phase = P_WAIT_BGP;
        break;
    case P_WAIT_BGP:
        /* A видит C и B; C видит A и B; B видит A (и C) */
        if (bgp_has(A, nc) && bgp_has(A, nb) && bgp_has(C, na) && bgp_has(C, nb) && bgp_has(B, na))
            t->phase = P_SEND1;
        break;
    case P_SEND1:
        if (dm_start(A, nb, g_sh.x_pub[IDX_B], g_sh.ed_pub[IDX_B], "B", CH_ID, DM_MODE_DEVICE, NULL) != 0
            || dm_send(A, g_sh.conv_id, "text", (const uint8_t*)"hello1", 6) != 0 || protocol_checks(A, B, C) != 0) {
            fprintf(stderr, "A: dm_start/send hello1 failed\n");
            t->result = 2;
        } else {
            t->phase = P_WAIT_MSG1;
        }
        break;
    case P_WAIT_MSG1:
        if (dm_msg_has(B, "hello1") && outbox_count(A) == 0 && dm_mail_count(C) == 0) t->phase = P_B_OFFLINE;
        break;
    case P_B_OFFLINE:
        utun_instance_destroy(B);
        t->inst[IDX_B] = NULL;
        t->phase = P_WAIT_B_OFFLINE;
        break;
    case P_WAIT_B_OFFLINE:
        if (!bgp_has(A, nb)) t->phase = P_SEND2;
        break;
    case P_SEND2:
        for (int i = 0; i < OFFLINE_BATCH; i++) {
            char text[32];
            snprintf(text, sizeof(text), "offline-%02d", i);
            if (dm_send(A, g_sh.conv_id, "text", (const uint8_t*)text, (uint32_t)strlen(text)) != 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_DM, "offline batch send failed index=%d", i);
                t->result = 2;
                uasync_stop(t->ua);
                return;
            }
        }
        t->phase = P_WAIT_MAIL;
        break;
    case P_WAIT_MAIL:
        if (dm_mail_count(C) == OFFLINE_BATCH) {
            utun_instance_destroy(A);
            t->inst[IDX_A] = NULL;
            char cfg[512];
            snprintf(cfg, sizeof(cfg), "%s/a.conf", getenv("UTUN_TEST_DIR"));
            t->inst[IDX_A] = utun_instance_create(t->ua, cfg);
            if (!t->inst[IDX_A] || utun_instance_init(t->inst[IDX_A]) != 0 || outbox_count(t->inst[IDX_A]) != OFFLINE_BATCH) {
                DEBUG_ERROR(DEBUG_CATEGORY_DM, "outbox restart failed");
                t->result = 2;
                uasync_stop(t->ua);
                return;
            }
            t->phase = P_RECONNECT;
        }
        break;
    case P_RECONNECT: {
        char cfg[512];
        snprintf(cfg, sizeof(cfg), "%s/b.conf", getenv("UTUN_TEST_DIR") ? getenv("UTUN_TEST_DIR") : ".");
        struct UTUN_INSTANCE* b2 = utun_instance_create(t->ua, cfg);
        if (!b2) { fprintf(stderr, "B2 create failed\n"); t->result = 2; break; }
        utun_instance_init(b2);
        t->inst[IDX_B] = b2;
        t->phase = P_WAIT_B2;
        break;
    }
    case P_WAIT_B2:
        if (offline_batch_received(B) && dm_mail_count(C) == 0 && outbox_count(A) == 0) t->phase = P_DONE;
        break;
    case P_DONE:
        t->result = 1;
        uasync_stop(t->ua);
        return;
    }

    if (t->result) { uasync_stop(t->ua); return; }
    if (t->phase == P_DONE) { t->result = 1; uasync_stop(t->ua); return; }

    if (++t->ticks > MAX_TICKS) {
        fprintf(stderr, "test: timeout in phase %d (A_conn=%d C_conn=%d B_conn=%d)\n",
                (int)t->phase,
                conn_up(t->inst[IDX_A], nc), conn_up(t->inst[IDX_C], na), conn_up(t->inst[IDX_B], nc));
        t->result = 2;
        uasync_stop(t->ua);
        return;
    }
    t->timer = uasync_set_timeout(t->ua, TICK_TB, t, dm_tick, "dm_tick");
}

/* ── сценарий 2: первое сообщение в offline (у получателя нет беседы) ──
 * A,B,C online → B уходит offline → A шлёт ПЕРВОЕ сообщение "first" → mailbox →
 * B2 переподключается (без беседы) → тянет mailbox → получает "first". */
enum dm2_phase {
    S2_WAIT_CONN,
    S2_SETUP,
    S2_WAIT_BGP,
    S2_B_OFFLINE,
    S2_WAIT_B_OFFLINE,
    S2_SEND,          /* A: dm_start + dm_send "first" → mailbox (B offline, первое) */
    S2_WAIT_MAIL,     /* C: блоб в dm_mail */
    S2_RECONNECT,     /* create B2 (та же БД B, но без беседы) */
    S2_WAIT_B2,       /* B2: получил "first" */
    S2_DONE,
};

struct dm2_ctx {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst[3];
    enum dm2_phase phase;
    int   result;
    int   ticks;
    void* timer;
};

static void dm2_tick(void* arg) {
    struct dm2_ctx* t = arg;
    t->timer = NULL;
    if (t->result) return;

    struct UTUN_INSTANCE* A = t->inst[IDX_A];
    struct UTUN_INSTANCE* B = t->inst[IDX_B];
    struct UTUN_INSTANCE* C = t->inst[IDX_C];
    uint64_t na = g_sh.nid[IDX_A], nb = g_sh.nid[IDX_B], nc = g_sh.nid[IDX_C];

    switch (t->phase) {
    case S2_WAIT_CONN:
        if (conn_up(A, nc) && conn_up(C, na) && conn_up(B, nc) && conn_up(C, nb))
            t->phase = S2_SETUP;
        break;
    case S2_SETUP:
        do_setup(A, IDX_A);
        do_setup(B, IDX_B);
        do_setup(C, IDX_C);
        t->phase = S2_WAIT_BGP;
        break;
    case S2_WAIT_BGP:
        if (bgp_has(A, nc) && bgp_has(A, nb) && bgp_has(C, na) && bgp_has(C, nb) && bgp_has(B, na))
            t->phase = S2_B_OFFLINE;
        break;
    case S2_B_OFFLINE:
        utun_instance_destroy(B);
        t->inst[IDX_B] = NULL;
        t->phase = S2_WAIT_B_OFFLINE;
        break;
    case S2_WAIT_B_OFFLINE:
        if (!bgp_has(A, nb)) t->phase = S2_SEND;
        break;
    case S2_SEND:
        if (dm_start(A, nb, g_sh.x_pub[IDX_B], g_sh.ed_pub[IDX_B], "B", CH_ID, DM_MODE_DEVICE, NULL) != 0
            || dm_send(A, g_sh.conv_id, "text", (const uint8_t*)"first", 5) != 0) {
            fprintf(stderr, "A: dm_start/send first failed\n");
            t->result = 2;
        } else {
            t->phase = S2_WAIT_MAIL;
        }
        break;
    case S2_WAIT_MAIL:
        if (dm_mail_count(C) > 0) {
            /* У отправителя больше нет процесса; очередь суперузла также проходит restart. */
            utun_instance_destroy(A);
            t->inst[IDX_A] = NULL;
            utun_instance_destroy(C);
            t->inst[IDX_C] = NULL;
            char cfg[512];
            snprintf(cfg, sizeof(cfg), "%s/c.conf", getenv("UTUN_TEST_DIR"));
            t->inst[IDX_C] = utun_instance_create(t->ua, cfg);
            if (!t->inst[IDX_C] || utun_instance_init(t->inst[IDX_C]) != 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_DM, "s2: supernode restart failed");
                t->result = 2;
                uasync_stop(t->ua);
                return;
            }
            t->phase = S2_RECONNECT;
        }
        break;
    case S2_RECONNECT: {
        char cfg[512];
        snprintf(cfg, sizeof(cfg), "%s/b.conf", getenv("UTUN_TEST_DIR") ? getenv("UTUN_TEST_DIR") : ".");
        struct UTUN_INSTANCE* b2 = utun_instance_create(t->ua, cfg);
        if (!b2) { fprintf(stderr, "B2 create failed\n"); t->result = 2; break; }
        utun_instance_init(b2);
        t->inst[IDX_B] = b2;
        t->phase = S2_WAIT_B2;
        break;
    }
    case S2_WAIT_B2:
        if (dm_msg_has(B, "first") && dm_mail_count(C) == 0) t->phase = S2_DONE;
        break;
    case S2_DONE:
        t->result = 1;
        uasync_stop(t->ua);
        return;
    }

    if (t->result) { uasync_stop(t->ua); return; }
    if (t->phase == S2_DONE) { t->result = 1; uasync_stop(t->ua); return; }

    if (++t->ticks > MAX_TICKS) {
        fprintf(stderr, "s2: timeout in phase %d (A_conn=%d C_conn=%d B_conn=%d)\n",
                (int)t->phase,
                conn_up(t->inst[IDX_A], nc), conn_up(t->inst[IDX_C], na), conn_up(t->inst[IDX_B], nc));
        t->result = 2;
        uasync_stop(t->ua);
        return;
    }
    t->timer = uasync_set_timeout(t->ua, TICK_TB, t, dm2_tick, "dm2_tick");
}

/* ── main ── */

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    test_clock_init(1000); test_clock_sync_wall_time();
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level_by_name("dm", "trace");
        debug_set_category_level_by_name("chat_sync", "info");
        debug_set_category_level_by_name("member_sync", "info");
        debug_set_category_level_by_name("bgp", "info");
        debug_set_category_level_by_name("etcp_route", "info");
        debug_set_category_level_by_name("crypto", "error");
    }
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));

    char dir[512]; snprintf(dir, sizeof(dir), "/tmp/utun_dm_e2e_XXXXXX");
    if (test_mkdtemp(dir) != 0) { fprintf(stderr, "mkdtemp failed\n"); return 1; }
    test_setenv("UTUN_TEST_DIR", dir, 1);

    fill_shared();
    write_configs(dir);

    printf("=== test_dm_e2e ===\n"); fflush(stdout);

    TEST("single-thread: direct send + offline mailbox + reconnect pull"); {
        struct UASYNC* ua = uasync_create();
        if (!ua) { FAIL("uasync_create failed"); return 1; }

        struct dm_ctx t;
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
            FAIL("instance create failed A=%p B=%p C=%p",
                 (void*)t.inst[IDX_A], (void*)t.inst[IDX_B], (void*)t.inst[IDX_C]);
            uasync_destroy(ua, 0);
            return 1;
        }
        for (int i = 0; i < 3; i++) g_sh.port[i] = ss_get_port(&test_etcp_bound_socket(t.inst[i], "s1")->local_addr);
        write_configs(dir); // Сохраняем назначенные endpoint для всех restart в этом сценарии.
        test_etcp_set_peer(t.inst[IDX_C], "to_a", t.inst[IDX_A], "s1");
        test_etcp_set_peer(t.inst[IDX_B], "to_c", t.inst[IDX_C], "s1");
        utun_instance_init(t.inst[IDX_A]);
        utun_instance_init(t.inst[IDX_C]);
        /* B стартует после C, чтобы B→C пришлось на уже поднятый C (как в сценарии) */
        utun_instance_init(t.inst[IDX_B]);

        t.timer = uasync_set_timeout(ua, TICK_TB, &t, dm_tick, "dm_tick");
        test_clock_mainloop(ua);

        int ok = (t.result == 1);

        if (t.inst[IDX_A]) utun_instance_destroy(t.inst[IDX_A]);
        if (t.inst[IDX_B]) utun_instance_destroy(t.inst[IDX_B]);
        if (t.inst[IDX_C]) utun_instance_destroy(t.inst[IDX_C]);
        uasync_destroy(ua, 0);

        if (ok) OK(); else FAIL("result=%d phase=%d", t.result, (int)t.phase);
    }

    char cmd[512]; snprintf(cmd, sizeof(cmd), "rm -rf %s", dir); (void)!system(cmd);

    /* сценарий 2: отдельная топология (свежие ключи/БД) */
    char dir2[512]; snprintf(dir2, sizeof(dir2), "/tmp/utun_dm_e2e2_XXXXXX");
    if (test_mkdtemp(dir2) != 0) { fprintf(stderr, "mkdtemp dir2 failed\n"); return 1; }
    test_setenv("UTUN_TEST_DIR", dir2, 1);
    fill_shared();
    write_configs(dir2);

    TEST("first offline DM: supernode restart, sender remains offline"); {
        struct UASYNC* ua = uasync_create();
        if (!ua) { FAIL("uasync_create failed"); return 1; }

        struct dm2_ctx t;
        memset(&t, 0, sizeof(t));
        t.ua = ua;
        t.phase = S2_WAIT_CONN;

        char cfgA[512], cfgB[512], cfgC[512];
        snprintf(cfgA, sizeof(cfgA), "%s/a.conf", dir2);
        snprintf(cfgB, sizeof(cfgB), "%s/b.conf", dir2);
        snprintf(cfgC, sizeof(cfgC), "%s/c.conf", dir2);

        t.inst[IDX_A] = utun_instance_create(ua, cfgA);
        t.inst[IDX_B] = utun_instance_create(ua, cfgB);
        t.inst[IDX_C] = utun_instance_create(ua, cfgC);

        if (!t.inst[IDX_A] || !t.inst[IDX_B] || !t.inst[IDX_C]) {
            FAIL("instance create failed A=%p B=%p C=%p",
                 (void*)t.inst[IDX_A], (void*)t.inst[IDX_B], (void*)t.inst[IDX_C]);
            uasync_destroy(ua, 0);
            return 1;
        }
        for (int i = 0; i < 3; i++) g_sh.port[i] = ss_get_port(&test_etcp_bound_socket(t.inst[i], "s1")->local_addr);
        write_configs(dir2);
        test_etcp_set_peer(t.inst[IDX_C], "to_a", t.inst[IDX_A], "s1");
        test_etcp_set_peer(t.inst[IDX_B], "to_c", t.inst[IDX_C], "s1");
        utun_instance_init(t.inst[IDX_A]);
        utun_instance_init(t.inst[IDX_C]);
        utun_instance_init(t.inst[IDX_B]);

        t.timer = uasync_set_timeout(ua, TICK_TB, &t, dm2_tick, "dm2_tick");
        test_clock_mainloop(ua);

        int ok = (t.result == 1);

        if (t.inst[IDX_A]) utun_instance_destroy(t.inst[IDX_A]);
        if (t.inst[IDX_B]) utun_instance_destroy(t.inst[IDX_B]);
        if (t.inst[IDX_C]) utun_instance_destroy(t.inst[IDX_C]);
        uasync_destroy(ua, 0);

        if (ok) OK(); else FAIL("result=%d phase=%d", t.result, (int)t.phase);
    }

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir2); (void)!system(cmd);

    printf("\n%d/%d passed, %d failed\n", G_PASSED, G_TOTAL, G_FAILED);
    test_clock_finish();
    return G_FAILED > 0 ? 1 : 0;
}
