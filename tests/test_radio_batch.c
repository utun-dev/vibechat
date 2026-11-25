#define GROUP_KEY "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"

/* Настоящие radio TX/RX без сокетов: размеры1..10, хвост/FIN, malformed, дедупликация и таймеры. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../src/radio/radio.h"
#include "../src/chat/chat_setting.h"
#include "../src/utun_instance.h"
#include "../src/routing_layer/topo_group.h"
#include "../src/routing_layer/topo_node.h"
#include "../src/transport_layer/etcp.h"
#include "../src/transport_layer/etcp_api.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <openssl/sha.h>

static unsigned received, fins, expected_seq;
static uint8_t expected_src[32];
static uint64_t peer_timestamp = 1000;
static uint8_t peer_private[32], peer_public[32];

/* Независимая сборка подписываемых полей: путь соседей в подпись не входит. */
static size_t signed_data(const uint8_t* wire, size_t len, uint8_t* out) {
    const uint8_t* p = wire + 1;
    size_t body = 1 + RADIO_HDR_SIZE + (size_t)p[RADIO_OFF_HOPS] * RADIO_KEY_SIZE;
    out[0] = ETCP_ID_RADIO; memcpy(out + 1, p, RADIO_OFF_HOPS); out[1 + RADIO_OFF_HOPS] = p[RADIO_OFF_COUNT];
    memcpy(out + RADIO_HDR_SIZE, wire + body, len - body - RADIO_SIGNATURE_SIZE);
    return RADIO_HDR_SIZE + len - body - RADIO_SIGNATURE_SIZE;
}

static void sign_peer(uint8_t* wire, size_t len) {
    peer_timestamp++; memcpy(wire + 1 + RADIO_OFF_TIMESTAMP, &peer_timestamp, 8);
    uint8_t body[RADIO_HDR_SIZE + RADIO_MAX_BODY]; size_t bytes = signed_data(wire, len, body);
    assert(sc_ed25519_sign(peer_private, body, bytes, wire + len - RADIO_SIGNATURE_SIZE) == SC_OK);
}
static void receive_frame(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint16_t stream,
                          uint16_t seq, uint8_t fin, const uint8_t* opus, int len, void* arg) {
    (void)inst; (void)stream; (void)arg;
    assert(group == 77 && !memcmp(ed, expected_src, 32) && seq == expected_seq++);
    if (fin) { assert(!len && !opus); fins++; return; }
    assert(len == RADIO_MAX_OPUS);
    for (int i = 0; i < len; i++) assert(opus[i] == (uint8_t)seq);
    received++;
}

/* Скопировать исходящий пакет как входящий от другого узла; ownership передаётся dispatcher. */
static void inject(struct UTUN_INSTANCE* inst, struct ETCP_CONN* conn, const uint8_t* wire, size_t len) {
    struct ll_entry* e = ll_alloc_lldgram((uint16_t)len); assert(e);
    memcpy(e->dgram, wire, len); e->len = len;
    inst->api_bindings.callbacks[ETCP_ID_RADIO](conn, e);
}

static void poll_for(struct UASYNC* ua, unsigned duration_tb) {
    uint64_t until = get_time_tb() + duration_tb;
    while (get_time_tb() < until) uasync_poll(ua, 10);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    size_t baseline = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE inst = {.ua = ua};
    struct SC_MYKEYS peer_keys;
    assert(sc_identity_generate(inst.my_keys.public_key, inst.my_keys.private_key, inst.my_ed25519_pubkey, inst.my_ed25519_privkey) == SC_OK);
    assert(sc_identity_generate(peer_keys.public_key, peer_keys.private_key, peer_public, peer_private) == SC_OK);
    struct SC_MYKEYS relay_keys, listener_keys;
    uint8_t relay_ed[32], relay_seed[32], listener_ed[32], listener_seed[32];
    assert(!sc_identity_generate(relay_keys.public_key, relay_keys.private_key, relay_ed, relay_seed));
    assert(!sc_identity_generate(listener_keys.public_key, listener_keys.private_key, listener_ed, listener_seed));
    inst.node_id = sc_derive_node_id_from_pubkey(inst.my_keys.public_key);
    const uint64_t source = sc_derive_node_id_from_pubkey(peer_keys.public_key);
    const uint64_t relay_id = sc_derive_node_id_from_pubkey(relay_keys.public_key);
    const uint64_t listener_id = sc_derive_node_id_from_pubkey(listener_keys.public_key);
    memcpy(expected_src, peer_public, 32);
    /* Готовый результат проверки MEMBERS; подписи дерева проверяются отдельными сетевыми тестами. */
    assert(sqlite3_open(":memory:", &inst.topo_sqlite_db) == SQLITE_OK);
    assert(sqlite3_exec(inst.topo_sqlite_db, "CREATE TABLE peers_" GROUP_KEY "(node_id INTEGER PRIMARY KEY,fixture_slot INTEGER,record_state INTEGER DEFAULT 0,"
        "source INTEGER DEFAULT 0,deleted INTEGER DEFAULT 0,admitted INTEGER DEFAULT 1,identity_verified INTEGER DEFAULT 1,"
        "x25519_pubkey BLOB DEFAULT(zeroblob(32)),ed25519_pubkey BLOB)", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt* member = NULL;
    assert(sqlite3_prepare_v2(inst.topo_sqlite_db, "INSERT INTO peers_" GROUP_KEY "(fixture_slot,node_id,x25519_pubkey,ed25519_pubkey) VALUES(?,?,?,?)", -1, &member, NULL) == SQLITE_OK);
    const uint8_t* xkeys[] = {peer_keys.public_key, inst.my_keys.public_key, relay_keys.public_key, listener_keys.public_key};
    const uint8_t* signing_keys[] = {peer_public, inst.my_ed25519_pubkey, relay_ed, listener_ed};
    for (int id = 1; id <= 4; id++) {
        assert(sqlite3_bind_int(member, 1, id) == SQLITE_OK);
        assert(sqlite3_bind_int64(member, 2, (sqlite3_int64)sc_derive_node_id_from_pubkey(xkeys[id - 1])) == SQLITE_OK);
        assert(sqlite3_bind_blob(member, 3, xkeys[id - 1], 32, SQLITE_STATIC) == SQLITE_OK);
        assert(sqlite3_bind_blob(member, 4, signing_keys[id - 1], 32, SQLITE_STATIC) == SQLITE_OK);
        assert(sqlite3_step(member) == SQLITE_DONE && sqlite3_reset(member) == SQLITE_OK);
    }
    assert(sqlite3_finalize(member) == SQLITE_OK);
    struct TOPO_GROUPS groups = {.instance = &inst}; inst.topo_groups = &groups;
    groups.group_list = queue_new(ua, 16, 0, 8, "radio_test_groups"); assert(groups.group_list);
    struct TOPO_GROUP* group = (void*)queue_entry_new(sizeof(*group) - sizeof(struct ll_entry)); assert(group);
    group->instance = &inst; group->group_id = 77; group->radio_active = 1;
    group->group_type = TOPO_GROUP_TYPE_CHAT; strcpy(group->channel_id, GROUP_KEY);
    struct SC_MYKEYS group_x; uint8_t group_seed[32];
    assert(!sc_identity_generate(group_x.public_key, group_x.private_key, group->wire_id, group_seed));
    assert(!queue_data_put_with_index(groups.group_list, &group->ll));
    group->nodes = queue_new(ua, 0, 0, 0, "radio_test_nodes");
    group->senders_list = queue_new(ua, 0, 0, 0, "radio_test_senders");
    struct ETCP_CONN conn = {.instance = &inst, .peer_node_id = source, .links_up = 1};
    memcpy(conn.crypto_ctx.peer_public_key, peer_keys.public_key, 32);
    memcpy(conn.peer_ed25519_pubkey, peer_public, 32);
    conn.send_input_q = queue_new(ua, 0, 0, 0, "radio_test_wire");
    assert(group->nodes && group->senders_list && conn.send_input_q);
    struct TOPO_GROUP_NODE* node = (void*)queue_entry_new(sizeof(*node) - sizeof(struct ll_entry)); assert(node);
    node->node_id = source; memcpy(node->ed25519, peer_public, RADIO_KEY_SIZE); node->radio = 1; node->paths = queue_new(ua, 0, 0, 0, "radio_test_paths"); assert(node->paths);
    struct TOPO_NODEPATH* path = (void*)queue_entry_new(sizeof(*path) - sizeof(struct ll_entry)); assert(path);
    path->conn = &conn; assert(!queue_data_put(node->paths, &path->ll)); assert(!queue_data_put(group->nodes, &node->ll));
    struct ll_entry* sender = queue_entry_new(sizeof(struct TOPO_GROUP_CONN_ITEM)); assert(sender);
    struct TOPO_GROUP_CONN_ITEM* session = (void*)sender->data;
    session->conn = &conn; session->node_id = source; session->accepted = 1;
    assert(!queue_data_put(group->senders_list, sender));
    assert(!radio_init(group) && !radio_init_instance(&inst));
    radio_set_frame_cb(&inst, receive_frame, NULL);
    uint8_t opus[RADIO_MAX_OPUS], sample[RADIO_MAX_PACKET_SIZE]; size_t sample_len = 0;
    uint64_t last_tx_timestamp = 0;
    for (unsigned target = 1; target <= 10; target++) {
        /* NTP может вернуть часы назад; timestamp DATA/FIN продолжает расти между передачами. */
        inst.ntp.synced = 1; inst.ntp.offset_us = target == 2 ? 10000000 : target == 3 ? -10000000 : 0;
        char value[8]; snprintf(value, sizeof(value), "%u", target);
        assert(!chat_setting_set(&inst, "radio_burst_frames", value));
        assert(!radio_talk_begin(&inst, 77));
        /* Смена настройки не меняет уже открытую передачу. */
        assert(!chat_setting_set(&inst, "radio_burst_frames", "1"));
        unsigned total = target + 1;
        for (unsigned i = 0; i < total; i++) {
            memset(opus, (int)i, sizeof(opus)); assert(!radio_talk_send(&inst, 77, opus, sizeof(opus)));
            assert(conn.send_input_q->count == (int)((i + 1) / target));
        }
        assert(!radio_talk_end(&inst, 77));
        assert(conn.send_input_q->count == (int)((total + target - 1) / target + 1));
        received = fins = expected_seq = 0;
        struct ll_entry* e;
        while ((e = queue_data_get(conn.send_input_q))) {
            uint8_t* p = e->dgram + 1;
            assert(e->dgram[0] == ETCP_ID_RADIO && p[RADIO_OFF_HOPS] == 1);
            uint64_t timestamp; memcpy(&timestamp, p + RADIO_OFF_TIMESTAMP, 8);
            assert(timestamp > last_tx_timestamp); last_tx_timestamp = timestamp;
            uint8_t proof[RADIO_HDR_SIZE + RADIO_MAX_BODY]; size_t proof_len = signed_data(e->dgram, e->len, proof);
            assert(sc_ed25519_verify(inst.my_ed25519_pubkey, proof, proof_len, e->dgram + e->len - RADIO_SIGNATURE_SIZE) == SC_OK);
            memcpy(p + RADIO_OFF_SRC, peer_public, RADIO_KEY_SIZE); memcpy(p + RADIO_OFF_HOPLIST, peer_public, RADIO_KEY_SIZE);
            sign_peer(e->dgram, e->len);
            unsigned before = received, before_fins = fins;
            inject(&inst, &conn, e->dgram, e->len);
            assert(received == before + p[RADIO_OFF_COUNT]);
            assert(fins == before_fins + !!p[RADIO_OFF_FLAGS]);
            inject(&inst, &conn, e->dgram, e->len); // Дубль пачки не выдаёт кадры повторно.
            assert(received == before + p[RADIO_OFF_COUNT] && fins == before_fins + !!p[RADIO_OFF_FLAGS]);
            if (target == 10 && p[RADIO_OFF_COUNT] == 10) {
                sample_len = e->len; memcpy(sample, e->dgram, sample_len);
            }
            queue_dgram_free(e); queue_entry_free(e); queue_resume_callback(conn.send_input_q);
        }
        assert(received == total && fins == 1);
    }
    /* Повреждённый последний кадр не должен выдать даже корректный префикс. */
    assert(sample_len); uint16_t stream = 60000;
    memcpy(sample + 1 + RADIO_OFF_STREAM, &stream, 2);
    sign_peer(sample, sample_len);
    unsigned before = received;
    for (size_t len = 0; len < sample_len; len++) inject(&inst, &conn, sample, len);
    assert(received == before);
    uint8_t invalid[sizeof(sample) + 1]; memcpy(invalid, sample, sample_len); invalid[sample_len] = 0;
    inject(&inst, &conn, invalid, sample_len + 1);
    for (unsigned count = 0; count <= 11; count += 11) {
        invalid[1 + RADIO_OFF_COUNT] = count; inject(&inst, &conn, invalid, sample_len);
    }
    memcpy(invalid, sample, sample_len); invalid[1 + RADIO_OFF_FLAGS] = RADIO_FLAG_FIN;
    inject(&inst, &conn, invalid, sample_len);
    invalid[1 + RADIO_OFF_FLAGS] = 2; inject(&inst, &conn, invalid, sample_len);
    memcpy(invalid, sample, sample_len); memset(invalid + 1 + RADIO_HDR_SIZE + RADIO_KEY_SIZE, 0, 2);
    inject(&inst, &conn, invalid, sample_len);
    assert(received == before);
    /* Корректные границы, но подделанные содержимое/подпись/поток не отравляют дедупликацию. */
    const size_t changed[] = {1 + RADIO_OFF_STREAM, 1 + RADIO_OFF_SEQ, 1 + RADIO_OFF_TIMESTAMP + 7,
                              1 + RADIO_HDR_SIZE + RADIO_KEY_SIZE + 2, sample_len - 1};
    for (unsigned i = 0; i < sizeof(changed) / sizeof(changed[0]); i++) {
        memcpy(invalid, sample, sample_len); invalid[changed[i]] ^= 1;
        inject(&inst, &conn, invalid, sample_len); assert(received == before);
    }
    received = fins = expected_seq = 0; inject(&inst, &conn, sample, sample_len); assert(received == 10 && !fins);
    /* Путь дописывает посредник 3, подпись остаётся автора 1. Отзыв автора не равен отзыву соседа. */
    memcpy(invalid, sample, 1 + RADIO_HDR_SIZE + RADIO_KEY_SIZE);
    memcpy(invalid + 1 + RADIO_HDR_SIZE + RADIO_KEY_SIZE, relay_ed, RADIO_KEY_SIZE);
    memcpy(invalid + 1 + RADIO_HDR_SIZE + 2 * RADIO_KEY_SIZE, sample + 1 + RADIO_HDR_SIZE + RADIO_KEY_SIZE, sample_len - 1 - RADIO_HDR_SIZE - RADIO_KEY_SIZE);
    invalid[1 + RADIO_OFF_HOPS] = 2; stream++; memcpy(invalid + 1 + RADIO_OFF_STREAM, &stream, 2);
    sign_peer(invalid, sample_len + RADIO_KEY_SIZE); conn.peer_node_id = session->node_id = relay_id;
    memcpy(conn.crypto_ctx.peer_public_key, relay_keys.public_key, 32);
    memcpy(conn.peer_ed25519_pubkey, relay_ed, 32);
    assert(sqlite3_exec(inst.topo_sqlite_db, "UPDATE peers_" GROUP_KEY " SET admitted=0 WHERE fixture_slot=1", NULL, NULL, NULL) == SQLITE_OK);
    before = received; inject(&inst, &conn, invalid, sample_len + RADIO_KEY_SIZE); assert(received == before);
    assert(sqlite3_exec(inst.topo_sqlite_db, "UPDATE peers_" GROUP_KEY " SET admitted=1 WHERE fixture_slot=1", NULL, NULL, NULL) == SQLITE_OK);
    received = fins = expected_seq = 0; inject(&inst, &conn, invalid, sample_len + RADIO_KEY_SIZE); assert(received == 10 && !fins);
    conn.peer_node_id = session->node_id = source; memcpy(conn.crypto_ctx.peer_public_key, peer_keys.public_key, 32);
    memcpy(conn.peer_ed25519_pubkey, peer_public, 32);
    /* FIN с корректной формой тоже обязан иметь подпись; отказ не поглощает настоящий FIN. */
    uint8_t fin[1 + RADIO_HDR_SIZE + RADIO_KEY_SIZE + RADIO_SIGNATURE_SIZE] = {ETCP_ID_RADIO};
    memcpy(fin + 1, invalid + 1, RADIO_HDR_SIZE);
    fin[1 + RADIO_OFF_HOPS] = 1; fin[1 + RADIO_OFF_COUNT] = 0; fin[1 + RADIO_OFF_FLAGS] = RADIO_FLAG_FIN;
    uint16_t final_seq = 10; memcpy(fin + 1 + RADIO_OFF_SEQ, &final_seq, 2);
    memcpy(fin + 1 + RADIO_OFF_HOPLIST, peer_public, RADIO_KEY_SIZE);
    inject(&inst, &conn, fin, sizeof(fin)); assert(!fins && expected_seq == final_seq);
    sign_peer(fin, sizeof(fin)); inject(&inst, &conn, fin, sizeof(fin)); assert(fins == 1 && expected_seq == final_seq + 1);
    /* Чистый транзит 3->2->4 не требует допуска автора 1, но сохраняет его подпись побайтно. */
    struct ETCP_CONN relay_conn = {.instance = &inst, .peer_node_id = relay_id, .links_up = 1};
    memcpy(relay_conn.crypto_ctx.peer_public_key, relay_keys.public_key, 32);
    memcpy(relay_conn.peer_ed25519_pubkey, relay_ed, 32);
    struct ll_entry* relay_entry = queue_entry_new(sizeof(struct TOPO_GROUP_CONN_ITEM)); assert(relay_entry);
    struct TOPO_GROUP_CONN_ITEM* relay_session = (void*)relay_entry->data;
    relay_session->conn = &relay_conn; relay_session->node_id = relay_id; relay_session->accepted = 1;
    assert(!queue_data_put(group->senders_list, relay_entry));
    memcpy(node->ed25519, listener_ed, RADIO_KEY_SIZE);
    conn.peer_node_id = session->node_id = node->node_id = listener_id; memcpy(conn.crypto_ctx.peer_public_key, listener_keys.public_key, 32);
    memcpy(conn.peer_ed25519_pubkey, listener_ed, 32); group->radio_active = 0;
    stream++; memcpy(invalid + 1 + RADIO_OFF_STREAM, &stream, 2); sign_peer(invalid, sample_len + RADIO_KEY_SIZE);
    assert(sqlite3_exec(inst.topo_sqlite_db, "UPDATE peers_" GROUP_KEY " SET admitted=0 WHERE fixture_slot=1", NULL, NULL, NULL) == SQLITE_OK);
    before = received; inject(&inst, &relay_conn, invalid, sample_len + RADIO_KEY_SIZE);
    assert(received == before && conn.send_input_q->count == 1);
    struct ll_entry* forwarded = queue_data_get(conn.send_input_q);
    assert(forwarded->len == sample_len + 2 * RADIO_KEY_SIZE && forwarded->dgram[1 + RADIO_OFF_HOPS] == 3);
    assert(!memcmp(forwarded->dgram + forwarded->len - RADIO_SIGNATURE_SIZE,
                   invalid + sample_len + RADIO_KEY_SIZE - RADIO_SIGNATURE_SIZE, RADIO_SIGNATURE_SIZE));
    uint8_t proof[RADIO_HDR_SIZE + RADIO_MAX_BODY]; size_t proof_len = signed_data(forwarded->dgram, forwarded->len, proof);
    assert(sc_ed25519_verify(peer_public, proof, proof_len, forwarded->dgram + forwarded->len - RADIO_SIGNATURE_SIZE) == SC_OK);
    queue_dgram_free(forwarded); queue_entry_free(forwarded); queue_resume_callback(conn.send_input_q);
    assert(!queue_remove_data(group->senders_list, relay_entry)); queue_entry_free(relay_entry);
    assert(sqlite3_exec(inst.topo_sqlite_db, "UPDATE peers_" GROUP_KEY " SET admitted=1 WHERE fixture_slot=1", NULL, NULL, NULL) == SQLITE_OK);
    memcpy(node->ed25519, peer_public, RADIO_KEY_SIZE);
    conn.peer_node_id = session->node_id = node->node_id = source; memcpy(conn.crypto_ctx.peer_public_key, peer_keys.public_key, 32);
    memcpy(conn.peer_ed25519_pubkey, peer_public, 32); group->radio_active = 1;
    /* FIN/пауза дольше транзитного кэша не разрешают повтор прежней пачки автора. */
    poll_for(ua, 21000); before = received;
    inject(&inst, &conn, sample, sample_len); inject(&inst, &conn, fin, sizeof(fin)); assert(received == before && fins == 1);
    /* Другой автор со своим меньшим timestamp принимается; watermark первого автора сохраняется. */
    memcpy(invalid, sample, sample_len); uint64_t other = relay_id, other_timestamp = 1;
    memcpy(invalid + 1 + RADIO_OFF_SRC, relay_ed, RADIO_KEY_SIZE);
    memcpy(invalid + 1 + RADIO_OFF_HOPLIST, relay_ed, RADIO_KEY_SIZE);
    memcpy(invalid + 1 + RADIO_OFF_TIMESTAMP, &other_timestamp, 8);
    proof_len = signed_data(invalid, sample_len, proof);
    assert(sc_ed25519_sign(relay_seed, proof, proof_len, invalid + sample_len - RADIO_SIGNATURE_SIZE) == SC_OK);
    conn.peer_node_id = session->node_id = other; memcpy(expected_src, relay_ed, 32); memcpy(conn.crypto_ctx.peer_public_key, relay_keys.public_key, 32);
    memcpy(conn.peer_ed25519_pubkey, relay_ed, 32);
    received = fins = expected_seq = 0; inject(&inst, &conn, invalid, sample_len); assert(received == 10);
    inject(&inst, &conn, invalid, sample_len); assert(received == 10);
    conn.peer_node_id = session->node_id = source; memcpy(expected_src, peer_public, 32); memcpy(conn.crypto_ctx.peer_public_key, peer_keys.public_key, 32);
    memcpy(conn.peer_ed25519_pubkey, peer_public, 32);
    inject(&inst, &conn, sample, sample_len); assert(received == 10);
    sign_peer(sample, sample_len); received = fins = expected_seq = 0;
    inject(&inst, &conn, sample, sample_len); assert(received == 10);
    /* Пауза захвата: частичная пачка уйдёт по deadline без END. */
    assert(!chat_setting_set(&inst, "radio_burst_frames", "10"));
    assert(!radio_talk_begin(&inst, 77)); memset(opus, 0, sizeof(opus));
    assert(!radio_talk_send(&inst, 77, opus, sizeof(opus))); assert(!conn.send_input_q->count);
    poll_for(ua, 2500); assert(conn.send_input_q->count == 1);
    struct ll_entry* e = queue_data_get(conn.send_input_q); assert(e->dgram[1 + RADIO_OFF_COUNT] == 1);
    queue_dgram_free(e); queue_entry_free(e); queue_resume_callback(conn.send_input_q);
    /* Destroy отменяет timer, не отправляет данные уже закрытой группы. */
    assert(!radio_talk_send(&inst, 77, opus, sizeof(opus)));
    radio_destroy(group); poll_for(ua, 2500); assert(!conn.send_input_q->count);
    radio_destroy_instance(&inst);
    queue_entry_free(queue_data_get(node->paths)); queue_free(node->paths);
    queue_entry_free(queue_data_get(group->nodes)); queue_free(group->nodes);
    queue_entry_free(queue_data_get(group->senders_list)); queue_free(group->senders_list);
    queue_entry_free(queue_data_get(groups.group_list)); queue_free(groups.group_list);
    assert(sqlite3_close(inst.topo_sqlite_db) == SQLITE_OK);
    queue_free(conn.send_input_q); uasync_destroy(ua, 0);
    if (u_get_allocated_count() != baseline) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio test retained allocations before=%zu after=%zu",
                                                       baseline, u_get_allocated_count());
    assert(u_get_allocated_count() == baseline);
    debug_set_level(DEBUG_LEVEL_INFO);
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PASS radio batches:1..10/max Opus, immutable target, tail/FIN, dedup, malformed, deadline, teardown, no leaks");
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PASS radio authentication: tampering/forged FIN rejected, revoked author blocked, pure transit preserves signature");
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PASS radio timestamps: independent authors, replay after FIN/pause rejected, sender clock rollback preserves order");
    return 0;
}
