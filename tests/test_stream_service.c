/* Живые CHAT-ядра и Merkle: атомарные опции, локальные владельцы, BGP-кандидаты и teardown. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <openssl/sha.h>

#include "../src/stream/stream.h"
#include "../src/stream/stream_member.h"
#include "../src/stream/stream_priv.h"
#include "../src/stream/stream_viewer.h"
#include "../src/chat/chat_event.h"
#include "../src/radio/radio.h"
#include "../src/chat/chat_core.h"
#include "../src/group/group_protocol.h"
#include "../src/chat/merkle_tree.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/routing_layer/route_crypto.h"
#include "../src/transport_layer/etcp.h"
#include "../src/transport_layer/etcp_keepalive.h"
#include "../src/transport_layer/node_conn_direct.h"
#include "../src/utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/json_flat.h"
#include "../lib/mem.h"
#include "test_clock.h"
#include "test_utils.h"
#include "test_membership.h"
#include "stream_address_fixture.h"
/* Private-состояние нужно только для воспроизведения границы INFO_RESP/idle без случайных задержек сети. */
#include "../src/stream/stream_network.c"
#include "../src/stream/stream_audio_meter.h"

#define GROUP1 UINT64_C(710001)
#define GROUP2 UINT64_C(710002)
#define CHANNEL1 "2152f8d19b791d24453242e15f2eab6cb7cffa7b6a5ed30097960e069881db12"
#define CHANNEL2 "d759793bbc13a2819a827c76adb6fba8a49aee007f49f2d0992d99b825ad2c48"
#define SINGLE_INPUT_CAPACITY (STREAM_RS_DEFAULT_SIZE - RS_HEADER_SIZE - STREAM_BATCH_OVERHEAD)
#define PRESSURE_INPUT_CAPACITY (6 * (STREAM_RS_DEFAULT_SIZE - RS_HEADER_SIZE) - STREAM_BATCH_OVERHEAD)
#define STREAM_TEST_CLOCK_STEP_TB 100
#define CHECK(expr) do { if (!(expr)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "test_stream_service failed line=%d: %s", __LINE__, #expr); exit(1); \
} } while (0)

static struct UTUN_INSTANCE *nodes[5];
struct catalogue_reply { int done, result; char* json; };
static void catalogue_ready(void* arg, int result, char* json) {
    struct catalogue_reply* reply = arg; CHECK(!reply->done++); reply->result = result; reply->json = json;
}
/* Только тест: продолжаем обрабатывать uasync, пока общий READ не вернул собственный JSON. */
static int read_catalogue(struct UTUN_INSTANCE* inst, uint64_t group, char** out) {
    struct catalogue_reply reply = {0}; CHECK(out && !*out); uint64_t deadline = get_time_tb() + 100000;
    stream_sources_read(inst, group, catalogue_ready, &reply);
    while (!reply.done && get_time_tb() < deadline) test_clock_poll(inst->ua, 100);
    CHECK(reply.done); *out = reply.json; return reply.result;
}
static struct {
    uint64_t last_ms[4][4];
    unsigned lines, tx, rx, idle;
} reports;

/* Проверяем живые сводки: один peer, все субпотоки вместе; таймер продолжает писать при полном простое. */
static void capture_stream_report(int level, const char *category, const char *message) {
    const char *line = strstr(message, "stream NET self=");
    if (!line || strcmp(category, "video")) return;
    CHECK(level == DEBUG_LEVEL_DEBUG);
    char self[65], peer[65]; unsigned long long dt, blocks, bytes;
    CHECK(sscanf(line, "stream NET self=%64s peer=%64s dt_ms=%llu", self, peer, &dt) == 3 && dt >= 500);
    unsigned a = 0, b = 0;
    while (a < 4 && strcmp(sc_public_key_hex(nodes[a]->my_ed25519_pubkey, (char[65]){0}), self)) a++;
    while (b < 4 && strcmp(sc_public_key_hex(nodes[b]->my_ed25519_pubkey, (char[65]){0}), peer)) b++;
    CHECK(a < 4 && b < 4);
    uint64_t now = get_time_tb() / 10;
    CHECK(!reports.last_ms[a][b] || now - reports.last_ms[a][b] >= 500);
    reports.last_ms[a][b] = now; reports.lines++;
    const char *sent = strstr(line, " sent="), *received = strstr(line, " received=");
    CHECK(sent && received && strstr(line, "errors=send/recv/crypto"));
    CHECK(strstr(line, "CONTROL{sent=") && strstr(line, "TX_window=") && strstr(line, "RX_window=")
        && strstr(line, "BUDGET{peer_flows=") && strstr(line, "closing=") && strstr(line, "cpu_pct="));
    CHECK(strstr(line, " global_pressure=") && strstr(line, " loop_late_ms="));
    unsigned regular, regular_limit, temporary, temporary_limit, background, background_limit;
    const char *budget = strstr(line, " regular="); CHECK(budget);
    CHECK(sscanf(budget, " regular=%u/%u temporary=%u/%u background=%u/%u", &regular, &regular_limit,
        &temporary, &temporary_limit, &background, &background_limit) == 6);
    CHECK(regular_limit == 192 && temporary_limit == 64 && background_limit == 16);
    CHECK(regular <= regular_limit && temporary <= temporary_limit && background <= background_limit && background <= temporary);
    CHECK(sscanf(sent, " sent=%llu/%llu", &blocks, &bytes) == 2);
    reports.tx += blocks != 0;
    int no_tx = blocks == 0;
    CHECK(sscanf(received, " received=%llu/%llu", &blocks, &bytes) == 2);
    reports.rx += blocks != 0; reports.idle += no_tx && blocks == 0;
}
static struct UASYNC *ua;
static struct test_group_keys group_keys[2];

static int stream_fixture_parse(const uint8_t* wire, size_t size, struct stream_message* message) {
    int rc = stream_wire_parse(wire, size, message);
    if (rc != STREAM_OK) return rc;
    for (unsigned i = 0; i < 2; i++) {
        if (memcmp(message->descriptor.group_key, group_keys[i].ed, SC_PUBKEY_SIZE)) continue;
        message->descriptor.group_id = i ? GROUP2 : GROUP1; return STREAM_OK;
    }
    return STREAM_ERR_FORMAT;
}
static uint8_t expected[1000];
static struct STREAM_HANDLE *reader1, *reader2;
static unsigned data_events[4], end_events[4], closed_events[4];
static int close_readers_in_callback;
static unsigned network_data[4], network_end[4], accepts, reverse_requests, dropped_renew;
static unsigned network_gaps[4];
static unsigned parallel_data;
static uint64_t network_next[4] = {0, 1, 1, 0};
static uint8_t first_request[STREAM_WIRE_HEADER + 8];
static etcp_recv_fn saved_raw[4], saved_route;
static int drop_renew, gap_batch, drop_telemetry;
static unsigned pressure_data, overload_seen, selects_seen;
static int ignore_select;
static uint8_t pressure_payload[PRESSURE_INPUT_CAPACITY], stale_select[9 + STREAM_OVERLOAD_MAX * 8 + STREAM_WIRE_HEADER];
static size_t stale_select_length;
static etcp_recv_fn pressure_handlers[4];
/* Один ускоренный uasync обслуживает все узлы стенда. CPU задаётся явно, физическая нагрузка — отдельный стенд. */
static int simulated_cpu = 30;
static unsigned global_data[4];
static unsigned automatic_data[4], automatic_gaps[4], automatic_end[4], automatic_probes[4], automatic_accepts;
static uint64_t automatic_next[4];
static int automatic_drop_relay;
static int automatic_drop_tail;
static etcp_recv_fn automatic_handlers[4];
static uint8_t automatic_request[STREAM_WIRE_HEADER + 1];
static etcp_recv_fn relay_raw_handlers[4], relay_route_handler, relay_status_handler;
static unsigned relay_data[3], relay_ends[3], relay_requests_seen, relay_statuses_seen, relay_cancels_seen;
static int relay_drop_source;
static uint8_t relay_request_wire[STREAM_WIRE_HEADER];
static struct {
    int hold_info, hold_request, hold_ready, check_starts, watch_provider;
    uint8_t info[STREAM_WIRE_HEADER + STREAM_INFO_SIZE + STREAM_PATH_MAX * 8];
    size_t info_length;
    unsigned source_subscribes, helper_subscribes, accepted, ready_held;
    uint64_t request_tb, start, epoch, provider_tb, provider_info_tb;
} relay_race;
static struct {
    unsigned data, closed, reinits, responses;
    int hold_info;
    const char *phase;
    uint64_t source_epoch;
    etcp_recv_fn receive;
    uint8_t first[STREAM_WIRE_HEADER + STREAM_INFO_SIZE + STREAM_PATH_MAX * 8];
    uint8_t latest[STREAM_WIRE_HEADER + STREAM_INFO_SIZE + STREAM_PATH_MAX * 8];
    size_t first_length, latest_length;
} bootstrap;
static void run_until(unsigned *value, unsigned expected_value);
/* Отказ по capacity проверяем в вызывающем потоке: общий счётчик меняют DB и crypto workers. */
static __thread int capacity_alloc_watch;
static __thread unsigned capacity_allocations;
static void capacity_allocation(const char *location) {
    if (!capacity_alloc_watch) return;
    capacity_allocations++;
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "capacity rejection allocated memory: %s", location);
}
void *__real_u_malloc_impl(uint32_t size, const char *location);
void *__wrap_u_malloc_impl(uint32_t size, const char *location) {
    capacity_allocation(location); return __real_u_malloc_impl(size, location);
}
void *__real_u_calloc_impl(uint32_t count, uint32_t size, const char *location);
void *__wrap_u_calloc_impl(uint32_t count, uint32_t size, const char *location) {
    capacity_allocation(location); return __real_u_calloc_impl(count, size, location);
}
void *__real_u_realloc_impl(void *ptr, uint32_t size, const char *location);
void *__wrap_u_realloc_impl(void *ptr, uint32_t size, const char *location) {
    capacity_allocation(location); return __real_u_realloc_impl(ptr, size, location);
}
char *__real_u_strdup_impl(const char *text, const char *location);
char *__wrap_u_strdup_impl(const char *text, const char *location) {
    capacity_allocation(location); return __real_u_strdup_impl(text, location);
}
err_t __real_uasync_get_load(struct UASYNC *async, struct uasync_load *load);
err_t __wrap_uasync_get_load(struct UASYNC *async, struct uasync_load *load) {
    err_t result = __real_uasync_get_load(async, load);
    if (simulated_cpu && result == ERR_OK) { load->average_percent = load->current_percent = simulated_cpu; load->valid = 1; }
    return result;
}

static void on_global_pressure(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)arg; unsigned node = inst == nodes[1] ? 1 : 2;
    CHECK(stream_address_equal(event->descriptor.source, stream_address_read(nodes[3]->my_ed25519_pubkey)) && event->descriptor.group_id == GROUP2);
    if (event->type == STREAM_EVENT_DATA) { CHECK(event->length == SINGLE_INPUT_CAPACITY); global_data[node]++; }
    else CHECK(event->type == STREAM_EVENT_GAP || event->type == STREAM_EVENT_CLOSED || event->type == STREAM_EVENT_END);
}

static void automatic_raw(struct ETCP_CONN *conn, struct ll_entry *entry) {
    unsigned node = 0; while (node < 4 && nodes[node] != conn->instance) node++; CHECK(node < 4);
    if (entry->len >= STREAM_WIRE_HEADER) {
        uint8_t command = entry->dgram[2];
        if (command == STREAM_CMD_INFO_REQ) automatic_probes[node]++;
        if (command == STREAM_CMD_ACCEPT) automatic_accepts++;
        if (node == 3 && conn->peer_node_id == nodes[2]->node_id && command == STREAM_CMD_SUBSCRIBE) {
            memcpy(automatic_request, entry->dgram, STREAM_WIRE_HEADER);
            automatic_request[2] = STREAM_CMD_UNSUBSCRIBE; automatic_request[STREAM_WIRE_HEADER] = STREAM_REASON_TRANSPORT;
        }
        if (automatic_drop_relay && node == 2 && conn->peer_node_id == nodes[1]->node_id && command == STREAM_CMD_DATA) {
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
        if (automatic_drop_tail && node == 2 && entry->dgram[3] && (command == STREAM_CMD_DATA || command == STREAM_CMD_END)) {
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
    }
    automatic_handlers[node](conn, entry);
}

static void on_automatic(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)arg; unsigned node = inst == nodes[1] ? 1 : 2;
    CHECK(stream_address_equal(event->descriptor.source, stream_address_read(nodes[3]->my_ed25519_pubkey)) && event->descriptor.group_id == GROUP2);
    if (event->type == STREAM_EVENT_DATA) {
        CHECK(event->length == stream_input_size(&event->descriptor) && !memcmp(event->data, pressure_payload, event->length));
        CHECK(event->first_seq == automatic_next[node]++); automatic_data[node]++;
    } else if (event->type == STREAM_EVENT_GAP) {
        CHECK(event->first_seq == automatic_next[node] && event->last_seq >= event->first_seq);
        automatic_next[node] = event->last_seq + 1; automatic_gaps[node]++;
    } else if (event->type == STREAM_EVENT_END) automatic_end[node]++;
    else CHECK(event->type == STREAM_EVENT_CLOSED);
}

struct restart_events { uint64_t next; unsigned data, gaps; };
static void on_restart(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)inst; struct restart_events *events = arg;
    if (event->type == STREAM_EVENT_DATA) {
        CHECK(event->first_seq == events->next++ && event->length == sizeof(expected));
        CHECK(!memcmp(event->data, expected, sizeof(expected))); events->data++;
    } else if (event->type == STREAM_EVENT_GAP) {
        CHECK(event->first_seq == events->next && event->last_seq >= event->first_seq);
        events->next = event->last_seq + 1; events->gaps++;
    } else CHECK(event->type == STREAM_EVENT_CLOSED);
}

/* Два restart до отложенного GAP сохраняют ещё не сообщённую приложению границу. */
static void test_restart_bound(struct STREAM_HANDLE *subscriber, const struct stream_descriptor *descriptor) {
    struct restart_events events = {.next = 50};
    struct stream_encoder *encoder = NULL; const uint8_t *blocks[RS_MAX_BLOCKS];
    size_t size = 0;
    CHECK(stream_encoder_create(&encoder, descriptor, nodes[0]->my_ed25519_privkey) == STREAM_OK);
    stream_set_event_callback(nodes[3], on_restart, &events);
    CHECK(stream_encoder_encode(encoder, 50, 1, 0, expected, sizeof(expected), blocks, &size) == STREAM_OK);
    for (unsigned sub = 0; sub < 6; sub++) CHECK(stream_ingest_block(nodes[3], blocks[sub], size) == STREAM_OK);
    run_until(&events.data, 1);
    CHECK(stream_receive_restart(subscriber) == STREAM_OK);
    CHECK(stream_encoder_encode(encoder, 60, 2, 0, expected, sizeof(expected), blocks, &size) == STREAM_OK);
    CHECK(stream_ingest_block(nodes[3], blocks[0], size) == STREAM_OK);
    CHECK(stream_receive_restart(subscriber) == STREAM_OK);
    CHECK(stream_ingest_block(nodes[3], blocks[0], size) == STREAM_DUPLICATE);
    for (unsigned sub = 1; sub < 6; sub++) CHECK(stream_ingest_block(nodes[3], blocks[sub], size) == STREAM_OK);
    run_until(&events.data, 2);
    CHECK(events.gaps == 1 && events.next == 61);
    CHECK(stream_receive_restart(subscriber) == STREAM_OK);
    CHECK(stream_receive_restart(subscriber) == STREAM_OK);
    CHECK(stream_ingest_block(nodes[3], blocks[0], size) == STREAM_DUPLICATE);
    test_clock_run_for(ua, 1000);
    struct stream_local local; CHECK(stream_local_get(nodes[3], descriptor, &local) == STREAM_OK);
    CHECK(local.anchored && local.start == 61 && local.next == 61 && events.data == 2 && events.gaps == 1);
    /* Неверная подпись пачки даёт ровно один GAP и сохраняет owner/приём следующего индекса. */
    CHECK(stream_encoder_encode(encoder, 61, 3, 0, expected, sizeof(expected), blocks, &size) == STREAM_OK);
    uint8_t bad[STREAM_BLOCK_MAX_SIZE]; memcpy(bad, blocks[0], size); bad[STREAM_BLOCK_RS_OFF + RS_HEADER_SIZE] ^= 1;
    CHECK(stream_ingest_block(nodes[3], bad, size) == STREAM_OK);
    for (unsigned sub = 1; sub < 6; sub++) CHECK(stream_ingest_block(nodes[3], blocks[sub], size) == STREAM_OK);
    run_until(&events.gaps, 2);
    CHECK(events.data == 2 && events.next == 62 && stream_handle_active(subscriber));
    CHECK(stream_encoder_encode(encoder, 62, 4, 0, expected, sizeof(expected), blocks, &size) == STREAM_OK);
    for (unsigned sub = 0; sub < 6; sub++) CHECK(stream_ingest_block(nodes[3], blocks[sub], size) == STREAM_OK);
    run_until(&events.data, 3);
    CHECK(events.gaps == 2 && events.next == 63 && stream_handle_active(subscriber));
    struct stream_info info;
    CHECK(stream_get_info(subscriber, &info) == STREAM_OK && info.buffer.rejected_batches == 1);
    stream_set_event_callback(nodes[3], NULL, NULL); stream_encoder_destroy(encoder);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] restart and rejected batch: exact GAP, unchanged owner, following authenticated DATA");
}

static void pressure_raw(struct ETCP_CONN *conn, struct ll_entry *entry) {
    unsigned node = 0; while (node < 4 && nodes[node] != conn->instance) node++; CHECK(node < 4);
    if (entry->len >= STREAM_WIRE_HEADER) {
        if (entry->dgram[2] == STREAM_CMD_OVERLOAD) overload_seen++;
        if (entry->dgram[2] == STREAM_CMD_OVERLOAD_SELECT) {
            selects_seen++; CHECK(entry->len <= sizeof(stale_select));
            memcpy(stale_select, entry->dgram, entry->len); stale_select_length = entry->len;
            if (ignore_select) { queue_dgram_free(entry); queue_entry_free(entry); return; }
        }
    }
    pressure_handlers[node](conn, entry);
}

static void on_pressure(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)arg; CHECK(inst == nodes[1] && stream_address_equal(event->descriptor.source, stream_address_read(nodes[3]->my_ed25519_pubkey)) && event->descriptor.group_id == GROUP2);
    if (event->type == STREAM_EVENT_DATA) {
        CHECK(event->length == sizeof(pressure_payload) && !memcmp(event->data, pressure_payload, sizeof(pressure_payload))); pressure_data++;
    } else CHECK(event->type == STREAM_EVENT_GAP || event->type == STREAM_EVENT_CLOSED || event->type == STREAM_EVENT_END);
}

static struct stream_peer_load_info source_load(void) {
    struct stream_peer_load_info info; CHECK(stream_peer_get_load(nodes[3], stream_address_read(nodes[1]->my_ed25519_pubkey), &info) == STREAM_OK);
    CHECK(info.load.arrivals.bytes == info.load.sent.bytes + info.load.removed.bytes + info.load.queued.bytes);
    CHECK(info.load.arrivals.packets == info.load.sent.packets + info.load.removed.packets + info.load.queued.packets);
    return info;
}

/* Протокол проходит настоящие ETCP/router обработчики; перехват только считает команды и имитирует потерю RENEW. */
static void network_raw(struct ETCP_CONN *conn, struct ll_entry *entry) {
    unsigned node = 0; while (node < 4 && nodes[node] != conn->instance) node++; CHECK(node < 4);
    if (entry->len >= STREAM_WIRE_HEADER) {
        uint8_t command = entry->dgram[2];
        if (node == 1 && command == STREAM_CMD_TELEMETRY && drop_telemetry) {
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
        if (node == 3 && command == STREAM_CMD_SUBSCRIBE && entry->dgram[3] == 0 && !first_request[0]) {
            CHECK(entry->len == sizeof(first_request)); memcpy(first_request, entry->dgram, sizeof(first_request));
        }
        if (node == 1 && command == STREAM_CMD_ACCEPT) accepts++;
        /* Дыра поставщика внутри его окна: заменить DATA пачки 2 на честный GAP 2->3, сохранив сессию/handle. */
        if (node == 1 && command == STREAM_CMD_DATA && gap_batch) {
            struct stream_message message; struct stream_path path; size_t used;
            CHECK(stream_fixture_parse(entry->dgram, entry->len, &message) == STREAM_OK);
            CHECK(stream_path_parse(message.payload, message.length, &path, &used) == STREAM_OK);
            if (stream_read_be(message.payload + used + STREAM_DATA_HEAD_SIZE + STREAM_FRAME_TIMING_SIZE + STREAM_BLOCK_SEQ_OFF, 8) == 2) {
                entry->dgram[2] = STREAM_CMD_GAP; entry->len = STREAM_WIRE_HEADER + 16;
                stream_write_be(entry->dgram + STREAM_WIRE_HEADER, 2, 8);
                stream_write_be(entry->dgram + STREAM_WIRE_HEADER + 8, 3, 8);
            }
        }
        if (node == 3 && command == STREAM_CMD_RENEW && drop_renew && !memcmp(entry->dgram + STREAM_WIRE_GROUP_OFF, group_keys[1].ed, SC_PUBKEY_SIZE)) {
            dropped_renew++; queue_dgram_free(entry); queue_entry_free(entry); return;
        }
    }
    saved_raw[node](conn, entry);
}

static void network_routed(struct ETCP_CONN *conn, struct ll_entry *entry) {
    if (entry->len >= ROUTER_SVC_PAYLOAD_OFF + STREAM_WIRE_HEADER
        && entry->dgram[ROUTER_SVC_PAYLOAD_OFF + 2] == STREAM_CMD_REVERSE_REQ) reverse_requests++;
    saved_route(conn, entry);
}

static void on_network(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)arg; unsigned node = inst == nodes[1] ? 1 : 2;
    CHECK(stream_address_equal(event->descriptor.source, stream_address_read(nodes[3]->my_ed25519_pubkey)));
    if (event->descriptor.group_id == GROUP1) {
        if (event->type == STREAM_EVENT_DATA) {
            CHECK(node == 1 && event->first_seq == parallel_data && event->length == sizeof(expected));
            CHECK(!memcmp(event->data, expected, sizeof(expected))); parallel_data++;
        } else CHECK(event->type == STREAM_EVENT_CLOSED);
        return;
    }
    CHECK(event->descriptor.group_id == GROUP2);
    if (event->type == STREAM_EVENT_DATA) {
        CHECK(event->length == sizeof(expected) && !memcmp(event->data, expected, sizeof(expected)));
        CHECK(event->first_seq == network_next[node]++); network_data[node]++;
    } else if (event->type == STREAM_EVENT_END) network_end[node]++;
    else if (event->type == STREAM_EVENT_GAP) {
        CHECK(event->first_seq == network_next[node] && event->last_seq == 2);
        network_next[node] = event->last_seq + 1; network_gaps[node]++;
    }
    else CHECK(event->type == STREAM_EVENT_CLOSED);
}

static void hex(const uint8_t *input, char *out) {
    for (unsigned i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", input[i]);
}

/* Проверяем заимствованные байты и после закрытия последнего локального владельца внутри callback. */
static void on_stream(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)arg;
    unsigned node = 0;
    while (node < 4 && nodes[node] != inst) node++;
    CHECK(node < 4 && (event->descriptor.group_id == GROUP1 || event->type == STREAM_EVENT_CLOSED));
    if (event->type == STREAM_EVENT_DATA) {
        CHECK(event->length == sizeof(expected) && !memcmp(event->data, expected, sizeof(expected)));
        data_events[node]++;
        if (node == 1 && close_readers_in_callback) {
            close_readers_in_callback = 0;
            stream_close(reader1); reader1 = NULL;
            stream_close(reader2); reader2 = NULL;
            CHECK(!memcmp(event->data, expected, sizeof(expected)));
        }
    } else if (event->type == STREAM_EVENT_END) end_events[node]++;
    else if (event->type == STREAM_EVENT_CLOSED) closed_events[node]++;
    else CHECK(0);
}

/* Реальные удостоверения участников: join/update/tree подписи, одинаковые на всех узлах. */
static void put_member(unsigned destination, unsigned member, uint64_t group) {
    const char* channel = group == GROUP2 ? CHANNEL2 : CHANNEL1;
    char userinfo[256], name[32];
    snprintf(name, sizeof(name), "node%u", member);
    snprintf(userinfo, sizeof(userinfo), "{\"name\":\"%s\",\"pad\":\"retained\",\"avatar_server\":\"0011223344556677001122334455667700112233445566770011223344556677\","
             "\"avatar_file\":\"retained-avatar\"}", name);
    uint8_t join[64], tree[64], update[64], body[MS_USERINFO_MAX + 112];
    const struct test_group_keys* keys = &group_keys[group == GROUP2];
    int size = member_sync_build_join_msg(keys->x.public_key, keys->ed, nodes[member]->my_ed25519_pubkey,
                                        nodes[member]->my_keys.public_key, body, sizeof(body));
    CHECK(size > 0 && sc_ed25519_sign(nodes[member]->my_ed25519_privkey, body, (size_t)size, join) == SC_OK);
    CHECK(member_sync_sign_join(keys->ed_private, channel, NULL, nodes[member]->my_keys.public_key, nodes[member]->my_ed25519_pubkey,
        TEST_JOIN_ID(keys->x.public_key, keys->ed, nodes[member]->my_ed25519_pubkey, nodes[member]->my_keys.public_key), 123, tree) == 0);
    size = member_sync_build_update_msg(TEST_JOIN_ID(keys->x.public_key, keys->ed, nodes[member]->my_ed25519_pubkey, nodes[member]->my_keys.public_key), 124, userinfo, body, sizeof(body));
    CHECK(size > 0 && sc_ed25519_sign(nodes[member]->my_ed25519_privkey, body, (size_t)size, update) == SC_OK);
    CHECK(member_sync_put(nodes[destination], channel, nodes[member]->my_keys.public_key,
          nodes[member]->my_ed25519_pubkey, join, 123, update, 124, userinfo, NULL, NULL, 0, 0, tree) >= 0);
}

static void run_until(unsigned *value, unsigned expected_value) {
    uint64_t deadline = get_time_tb() + 100000;
    while (*value < expected_value && get_time_tb() < deadline) test_clock_poll(ua, 100);
    if (*value != expected_value) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream test wait expected=%u actual=%u data=%u/%u end=%u/%u closed=%u/%u",
        expected_value, *value, data_events[1], data_events[2], end_events[1], end_events[2], closed_events[1], closed_events[2]);
    CHECK(*value == expected_value);
}

/* Дожидаемся именно Merkle-профиля, не копируем stream-опции вручную между ядрами. */
static void wait_state(unsigned receiver, unsigned member, uint64_t group, uint64_t epoch, int subscribed) {
    uint64_t deadline = get_time_tb() + 150000;
    while (get_time_tb() < deadline) {
        struct stream_member_state state;
        int result = stream_member_load(nodes[receiver], group, stream_address_read(nodes[member]->my_ed25519_pubkey), &state, NULL);
        if (result == STREAM_OK && (subscribed ? stream_member_has_subscription(&state, stream_address_read(nodes[0]->my_ed25519_pubkey), epoch)
                                             : state.published && state.source.source_epoch == epoch)) return;
        test_clock_poll(ua, 100);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Merkle stream state timeout receiver=%u member=%u group=%llu epoch=%llx",
                receiver, member, (unsigned long long)group, (unsigned long long)epoch);
    for (unsigned i = 0; i < 4; i++) {
        char query[160]; sqlite3_stmt* st = NULL;
        snprintf(query, sizeof(query), "SELECT update_ts,userinfo FROM peers_%s WHERE node_id=?", group == GROUP2 ? CHANNEL2 : CHANNEL1);
        int rc = sqlite3_prepare_v2(nodes[i]->topo_sqlite_db, query, -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)nodes[member]->node_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Merkle profile node=%u self=%016llx member=%u sql=%d version=%lld userinfo=%s",
            i, (unsigned long long)nodes[i]->node_id, member, rc,
            rc == SQLITE_ROW ? (long long)sqlite3_column_int64(st, 0) : -1,
            rc == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 1) : "unavailable");
        sqlite3_finalize(st);
        for (unsigned j = 0; j < 4; j++) if (i != j) {
            struct ETCP_CONN* conn = instance_find_conn(nodes[i], nodes[j]->node_id);
            if (conn) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Merkle link self=%u peer=%u up=%d sleep=%d READY=%d",
                i, j, conn->links_up, conn->peer_sleep_phase,
                topo_group_peer_ready(topo_groups_find(nodes[i]->topo_groups, group), nodes[j]->node_id));
        }
    }
    CHECK(0);
}

static void profile(unsigned node, uint64_t group, char json[MS_USERINFO_MAX + 1], uint8_t signature[64], uint64_t *version) {
    const char* channel = group == GROUP2 ? CHANNEL2 : CHANNEL1;
    CHECK(chat_core_get_my_profile(nodes[node], channel, json, MS_USERINFO_MAX + 1, signature, version) == 0);
}

static void root_hash(unsigned node, uint64_t group, uint8_t hash[32]) {
    const char* channel = group == GROUP2 ? CHANNEL2 : CHANNEL1;
    CHECK(merkle_sync_read_hash(nodes[node], MT_DATA_MEMBERS, channel, 0, 0, hash) == 0);
}

static void network_write(struct STREAM_HANDLE *publisher, unsigned seq) {
    size_t accepted = 0;
    CHECK(stream_publish_write(publisher, expected, sizeof(expected), &accepted) == STREAM_OK && accepted == sizeof(expected));
    uint64_t deadline = get_time_tb() + 100000;
    struct stream_info info;
    do {
        CHECK(stream_get_info(publisher, &info) == STREAM_OK);
        if (info.buffer.blocks >= (seq + 1) * 8) return;
        test_clock_poll(ua, 100);
    } while (get_time_tb() < deadline);
    CHECK(0);
}

/* Считаем только подтверждённые настоящим ETCP-handshake сбросы проверяемого соединения. */
static void bootstrap_status(struct ETCP_CONN *conn, int status, void *arg) {
    (void)arg;
    if (conn->peer_node_id == nodes[3]->node_id && status == ETCP_CONN_STATUS_REINIT) {
        bootstrap.reinits++;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "bootstrap phase=%s REINIT peer=%llx ordinal=%u local=%llx remote=%llx",
                   bootstrap.phase, (unsigned long long)conn->peer_node_id, bootstrap.reinits,
                   (unsigned long long)conn->reset_id, (unsigned long long)conn->peer_reset_id);
    }
}

/* Перехват удерживает только INFO_RESP проверяемого источника; остальной протокол проходит штатно. */
static void bootstrap_raw(struct ETCP_CONN *conn, struct ll_entry *entry) {
    if (bootstrap.hold_info && conn->peer_node_id == nodes[3]->node_id && entry->len >= STREAM_WIRE_HEADER
        && entry->dgram[2] == STREAM_CMD_INFO_RESP) {
        struct stream_message message;
        CHECK(stream_fixture_parse(entry->dgram, entry->len, &message) == STREAM_OK);
        if (message.descriptor.group_id != GROUP1 || !stream_address_equal(message.descriptor.source, stream_address_read(nodes[3]->my_ed25519_pubkey))
            || message.descriptor.source_epoch != bootstrap.source_epoch) {
            bootstrap.receive(conn, entry); return;
        }
        CHECK(message.length >= 8 && entry->len <= sizeof(bootstrap.first));
        if (!bootstrap.first_length) {
            memcpy(bootstrap.first, entry->dgram, entry->len); bootstrap.first_length = entry->len;
        }
        memcpy(bootstrap.latest, entry->dgram, entry->len); bootstrap.latest_length = entry->len; bootstrap.responses++;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "bootstrap phase=%s holds INFO_RESP ordinal=%u sender=%llx receiver=%llx nonce=%llx",
                   bootstrap.phase, bootstrap.responses, (unsigned long long)message.sender_epoch,
                   (unsigned long long)message.receiver_epoch, (unsigned long long)stream_read_be(message.payload, 8));
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    bootstrap.receive(conn, entry);
}

/* Первый ответ либо ответ нового поколения после подтверждённого REINIT; дубликаты не продвигают фазу. */
static void bootstrap_wait_response(struct ETCP_CONN *conn, unsigned reinits) {
    uint64_t deadline = get_time_tb() + 50000;
    struct stream_message first = {0}, latest = {0};
    if (conn) CHECK(stream_fixture_parse(bootstrap.first, bootstrap.first_length, &first) == STREAM_OK && first.length >= 8);
    do {
        if (bootstrap.latest_length) {
            CHECK(stream_fixture_parse(bootstrap.latest, bootstrap.latest_length, &latest) == STREAM_OK && latest.length >= 8);
            if (!conn || (bootstrap.reinits > reinits && conn->initialized && !conn->session_required && !conn->reinit_pending
                && latest.receiver_epoch != first.receiver_epoch && stream_read_be(latest.payload, 8) != stream_read_be(first.payload, 8))) {
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "bootstrap phase=%s response ready renewed=%d REINIT=%u responses=%u",
                           bootstrap.phase, conn != NULL, bootstrap.reinits, bootstrap.responses);
                return;
            }
        }
        test_clock_poll(ua, 100);
    } while (get_time_tb() < deadline);
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "bootstrap phase=%s response timeout renewed=%d REINIT=%u/%u responses=%u"
                " receiver=%llx/%llx nonce=%llx/%llx initialized=%d session_required=%d reinit_pending=%d",
                bootstrap.phase, conn != NULL, bootstrap.reinits, reinits, bootstrap.responses,
                (unsigned long long)first.receiver_epoch, (unsigned long long)latest.receiver_epoch,
                (unsigned long long)(first.length >= 8 ? stream_read_be(first.payload, 8) : 0),
                (unsigned long long)(latest.length >= 8 ? stream_read_be(latest.payload, 8) : 0),
                conn ? conn->initialized : 0, conn ? conn->session_required : 0, conn ? conn->reinit_pending : 0);
    CHECK(0);
}

static void bootstrap_event(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    if (!stream_address_equal(event->descriptor.source, stream_address_read(nodes[3]->my_ed25519_pubkey))) { on_stream(inst, event, arg); return; }
    CHECK(inst == nodes[1] && event->descriptor.group_id == GROUP1 && event->descriptor.source_epoch == bootstrap.source_epoch);
    if (event->type == STREAM_EVENT_CLOSED) {
        bootstrap.closed++;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "bootstrap phase=%s local reader closed reason=%d ordinal=%u",
                   bootstrap.phase, event->reason, bootstrap.closed); return;
    }
    CHECK(event->descriptor.group_id == GROUP1 && event->type == STREAM_EVENT_DATA && event->length == sizeof(expected));
    CHECK(!memcmp(event->data, expected, sizeof(expected))); bootstrap.data++;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "bootstrap phase=%s DATA ordinal=%u seq=%llu closed=%u",
               bootstrap.phase, bootstrap.data, (unsigned long long)event->first_seq, bootstrap.closed);
}

/* Холодное подключение, управляемый REINIT автоматической пробы, старые ответы и исходный срок ручной пробы. */
static void test_bootstrap(void) {
    debug_level_t transport_level = g_debug_config.category_levels[DEBUG_CATEGORY_ETCP];
    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_INFO);
    struct STREAM_HANDLE *publisher = NULL, *subscriber = NULL;
    struct stream_parameters parameters = {.n = 1, .r = 0, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[3], GROUP1, &parameters, &publisher) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    bootstrap.phase = "cold"; bootstrap.source_epoch = origin.descriptor.source_epoch;
    wait_state(1, 3, GROUP1, origin.descriptor.source_epoch, 0);
    CHECK(!instance_find_conn(nodes[1], nodes[3]->node_id));
    bootstrap.receive = nodes[1]->api_bindings.callbacks[ETCP_ID_STREAM];
    CHECK(etcp_bind(nodes[1], ETCP_ID_STREAM, bootstrap_raw) == 0);
    etcp_add_conn_status_cbk(nodes[1], bootstrap_status, NULL);
    stream_set_event_callback(nodes[1], bootstrap_event, NULL);
    for (unsigned i = 0; i < sizeof(expected); i++) expected[i] = (uint8_t)(i * 71 + 3);
    size_t accepted = 0;
    CHECK(stream_publish_write(publisher, expected, sizeof(expected), &accepted) == STREAM_OK && accepted == sizeof(expected));
    uint64_t started = get_time_tb();
    CHECK(stream_subscribe_open(nodes[1], GROUP1, stream_address_read(nodes[3]->my_ed25519_pubkey), &subscriber) == STREAM_OK);
    run_until(&bootstrap.data, 1);
    struct stream_selection_info selected; CHECK(stream_selection_get_info(subscriber, &selected) == STREAM_OK);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "bootstrap result REINIT=%u probes=%llu subflows=%u failures=%llu",
               bootstrap.reinits, (unsigned long long)selected.probes_started, selected.subflows,
               (unsigned long long)selected.failures);
    CHECK(selected.probes_started == 1 && selected.subflows == 1 && !selected.failures && !bootstrap.closed);
    CHECK(get_time_tb() - started < 100000);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] cold automatic subscription: elapsed_ms=%llu probes=%llu",
               (unsigned long long)((get_time_tb() - started) / 10), (unsigned long long)selected.probes_started);
    stream_close(subscriber); subscriber = NULL;
    run_until(&bootstrap.closed, 1);

    /* Новый локальный буфер исключает получение прежней пачки из кеша первого читателя. */
    bootstrap.phase = "automatic pending REINIT"; bootstrap.hold_info = 1;
    bootstrap.responses = 0; bootstrap.first_length = bootstrap.latest_length = 0;
    CHECK(stream_subscribe_open(nodes[1], GROUP1, stream_address_read(nodes[3]->my_ed25519_pubkey), &subscriber) == STREAM_OK);
    struct stream_info local; CHECK(stream_get_info(subscriber, &local) == STREAM_OK && !local.buffer.blocks);
    bootstrap_wait_response(NULL, 0);
    CHECK(stream_selection_get_info(subscriber, &selected) == STREAM_OK);
    CHECK(selected.probes_started == 1 && selected.probes_active == 1 && !selected.subflows && !selected.failures);
    CHECK(bootstrap.data == 1 && bootstrap.closed == 1);
    struct ETCP_CONN *conn = instance_find_conn(nodes[1], nodes[3]->node_id);
    CHECK(conn && conn->links_up && conn->initialized && !conn->session_required && !conn->reinit_pending);
    unsigned reinits = bootstrap.reinits;
    uint64_t reset_id = conn->reset_id;
    etcp_conn_reinit(conn, "stream automatic pending INFO test");
    bootstrap_wait_response(conn, reinits);
    CHECK(instance_find_conn(nodes[1], nodes[3]->node_id) == conn && conn->reset_id != reset_id);
    CHECK(bootstrap.reinits == reinits + 1 && bootstrap.data == 1 && bootstrap.closed == 1);
    CHECK(stream_selection_get_info(subscriber, &selected) == STREAM_OK);
    CHECK(selected.probes_started == 1 && selected.probes_active == 1 && !selected.subflows && !selected.failures);
    struct ll_entry *entry = ll_alloc_lldgram((uint16_t)bootstrap.latest_length); CHECK(entry);
    memcpy(entry->dgram, bootstrap.latest, bootstrap.latest_length); entry->len = bootstrap.latest_length;
    bootstrap.hold_info = 0; bootstrap.receive(conn, entry);
    run_until(&bootstrap.data, 2);
    CHECK(stream_selection_get_info(subscriber, &selected) == STREAM_OK);
    CHECK(selected.probes_started == 1 && selected.subflows == 1 && !selected.failures && bootstrap.closed == 1);
    CHECK(bootstrap.reinits == reinits + 1);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] automatic pending probe survives confirmed REINIT: probes=%llu subflows=%u failures=%llu",
               (unsigned long long)selected.probes_started, selected.subflows, (unsigned long long)selected.failures);
    stream_close(subscriber); subscriber = NULL;
    run_until(&bootstrap.closed, 2);

    bootstrap.phase = "manual stale response"; bootstrap.hold_info = 1;
    bootstrap.responses = 0; bootstrap.first_length = bootstrap.latest_length = 0;
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[3]->my_ed25519_pubkey), &subscriber) == STREAM_OK);
    struct STREAM_RECEIVE *probe = NULL;
    CHECK(stream_probe_open(subscriber, stream_address_read(nodes[3]->my_ed25519_pubkey), 0, UINT64_MAX, &probe) == STREAM_OK);
    bootstrap_wait_response(NULL, 0);
    CHECK(instance_find_conn(nodes[1], nodes[3]->node_id) == conn && conn->links_up);
    reinits = bootstrap.reinits;
    etcp_conn_reinit(conn, "stream pending INFO test");
    bootstrap_wait_response(conn, reinits);
    CHECK(bootstrap.reinits == reinits + 1 && bootstrap.closed == 2 && bootstrap.data == 2);
    struct stream_message old, current;
    CHECK(stream_fixture_parse(bootstrap.first, bootstrap.first_length, &old) == STREAM_OK);
    CHECK(stream_fixture_parse(bootstrap.latest, bootstrap.latest_length, &current) == STREAM_OK);
    CHECK(old.receiver_epoch != current.receiver_epoch && stream_read_be(old.payload, 8) != stream_read_be(current.payload, 8));
    struct stream_receive_info info;
    CHECK(stream_receive_get_info(probe, &info) == STREAM_OK && info.state == STREAM_RECEIVE_INFO && !info.info_ready);
    for (unsigned replay = 0; replay < 2; replay++) {
        struct ll_entry *entry = ll_alloc_lldgram((uint16_t)bootstrap.first_length); CHECK(entry);
        memcpy(entry->dgram, bootstrap.first, bootstrap.first_length); entry->len = bootstrap.first_length;
        if (replay) {
            old.sender_epoch = current.sender_epoch; old.receiver_epoch = current.receiver_epoch;
            stream_wire_header(entry->dgram, &old); /* Отдельно проверяем nonce внутри уже актуальной сессии. */
        }
        bootstrap.receive(conn, entry);
        CHECK(stream_receive_get_info(probe, &info) == STREAM_OK && info.state == STREAM_RECEIVE_INFO && !info.info_ready);
    }
    entry = ll_alloc_lldgram((uint16_t)bootstrap.latest_length); CHECK(entry);
    memcpy(entry->dgram, bootstrap.latest, bootstrap.latest_length); entry->len = bootstrap.latest_length;
    bootstrap.receive(conn, entry);
    CHECK(stream_receive_get_info(probe, &info) == STREAM_OK && info.info_ready && !info.handle && !info.blocks);
    stream_receive_close(probe); probe = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] pending INFO survives REINIT; old epoch/nonce rejected; fresh response accepted");

    bootstrap.phase = "manual original deadline";
    bootstrap.responses = 0; bootstrap.first_length = bootstrap.latest_length = 0;
    started = get_time_tb();
    CHECK(stream_probe_open(subscriber, stream_address_read(nodes[3]->my_ed25519_pubkey), 0, UINT64_MAX, &probe) == STREAM_OK);
    bootstrap_wait_response(NULL, 0);
    for (unsigned reset = 0; reset < 2; reset++) {
        /* Сроки от открытия пробы: ожидание handshake первого сброса не сдвигает второй к истечению deadline. */
        uint64_t reset_at = started + (1 + 2 * reset) * 10000;
        while (get_time_tb() < reset_at) test_clock_poll(ua, 100);
        CHECK(stream_receive_get_info(probe, &info) == STREAM_OK && info.state != STREAM_RECEIVE_CLOSED && !info.info_ready);
        reinits = bootstrap.reinits;
        etcp_conn_reinit(conn, "stream pending deadline test");
        bootstrap_wait_response(conn, reinits);
        CHECK(bootstrap.reinits == reinits + 1);
        memcpy(bootstrap.first, bootstrap.latest, bootstrap.latest_length); bootstrap.first_length = bootstrap.latest_length;
    }
    do { test_clock_poll(ua, 100); CHECK(stream_receive_get_info(probe, &info) == STREAM_OK); }
    while (info.state != STREAM_RECEIVE_CLOSED && get_time_tb() - started < 60000);
    CHECK(info.state == STREAM_RECEIVE_CLOSED && info.reason == STREAM_REASON_TRANSPORT && get_time_tb() - started < 60000);
    entry = ll_alloc_lldgram((uint16_t)bootstrap.latest_length); CHECK(entry);
    memcpy(entry->dgram, bootstrap.latest, bootstrap.latest_length); entry->len = bootstrap.latest_length;
    bootstrap.receive(conn, entry); // Поздний удержанный ответ возвращает место окна, но не оживляет просроченную пробу.
    CHECK(stream_receive_get_info(probe, &info) == STREAM_OK && info.state == STREAM_RECEIVE_CLOSED && !info.info_ready);
    struct stream_peer *pending_peer = peer_find(stream_network_get(nodes[1]), stream_address_read(nodes[3]->my_ed25519_pubkey));
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "late INFO_RESP peer=%s outstanding=%u", pending_peer ? "retained" : "released",
        pending_peer ? pending_peer->request_count : 0);
    CHECK(!pending_peer || !pending_peer->request_count);
    stream_receive_close(probe); probe = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] repeated REINIT keeps original pending deadline: elapsed_ms=%llu",
               (unsigned long long)((get_time_tb() - started) / 10));

    stream_close(subscriber); stream_close(publisher);
    run_until(&bootstrap.closed, 3);
    CHECK(etcp_bind(nodes[1], ETCP_ID_STREAM, bootstrap.receive) == 0);
    etcp_remove_conn_status_cbk(nodes[1], bootstrap_status, NULL);
    stream_set_event_callback(nodes[1], on_stream, NULL);
    debug_set_category_level(DEBUG_CATEGORY_ETCP, transport_level);
}

/* Разрушающий транспорт сценарий выполняется после проверок, которым нужен готовый общий стенд. */
static void test_pending_down(void) {
    struct STREAM_HANDLE *publisher = NULL, *subscriber = NULL;
    CHECK(stream_publish_open(nodes[3], GROUP1, NULL, &publisher) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    wait_state(1, 3, GROUP1, origin.descriptor.source_epoch, 0);
    bootstrap.phase = "pending DOWN/DELETE"; bootstrap.source_epoch = origin.descriptor.source_epoch;
    unsigned closed = bootstrap.closed, data = bootstrap.data;
    stream_set_event_callback(nodes[1], bootstrap_event, NULL);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[3]->my_ed25519_pubkey), &subscriber) == STREAM_OK);
    struct STREAM_RECEIVE *probe = NULL;
    CHECK(stream_probe_open(subscriber, stream_address_read(nodes[3]->my_ed25519_pubkey), 0, UINT64_MAX, &probe) == STREAM_OK);
    struct ETCP_CONN *conn = instance_find_conn(nodes[1], nodes[3]->node_id); CHECK(conn && conn->links_up);
    etcp_connection_close(conn); /* Реальный DOWN и DELETE проходят через NCD. */
    struct stream_receive_info info;
    CHECK(stream_receive_get_info(probe, &info) == STREAM_OK && info.state == STREAM_RECEIVE_CLOSED
          && info.reason == STREAM_REASON_TRANSPORT);
    stream_receive_close(probe); stream_close(subscriber); stream_close(publisher);
    run_until(&bootstrap.closed, closed + 1);
    CHECK(bootstrap.data == data);
    stream_set_event_callback(nodes[1], on_stream, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] transport DOWN/DELETE still cancels pending request");
}

/* Источник -> relay -> получатель; данные передаются через raw ETCP, никаких прямых stream_ingest в этой проверке. */
static void test_network(void) {
    debug_level_t previous_video = g_debug_config.category_levels[DEBUG_CATEGORY_VIDEO];
    debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_DEBUG); debug_set_log_hook(capture_stream_report);
    for (unsigned i = 0; i < sizeof(expected); i++) expected[i] = (uint8_t)(i * 71 + 3);
    struct TOPO_PEER_REQUEST *connections[2] = {0};
    struct TOPO_GROUP *root = topo_groups_find(nodes[3]->topo_groups, GROUP2);
    for (unsigned i = 1; i < 3; i++) CHECK(topo_group_peer_open(root, nodes[i]->node_id, &connections[i - 1]) == 0);
    uint64_t deadline = get_time_tb() + 150000;
    while (get_time_tb() < deadline
           && (topo_group_peer_phase(connections[0]) != TOPO_PEER_READY || topo_group_peer_phase(connections[1]) != TOPO_PEER_READY))
        test_clock_poll(ua, 100);
    for (unsigned i = 1; i < 3; i++) {
        CHECK(topo_group_peer_phase(connections[i - 1]) == TOPO_PEER_READY);
        CHECK(member_sync_start(nodes[3], instance_find_conn_by_key(nodes[3], nodes[i]->my_keys.public_key), CHANNEL2, NULL, NULL) == 0);
        CHECK(member_sync_start(nodes[i], instance_find_conn_by_key(nodes[i], nodes[3]->my_keys.public_key), CHANNEL2, NULL, NULL) == 0);
    }
    struct STREAM_HANDLE *publisher = NULL, *subscribers[3] = {0};
    struct stream_parameters fec_parameters = {.n = 8, .r = 2, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[3], GROUP2, &fec_parameters, &publisher) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    /* Подписанный END отдельно отвергает подмену подписи, индекса и группы. */
    uint8_t end[STREAM_END_SIZE]; uint64_t last;
    CHECK(stream_end_sign(&origin.descriptor, UINT64_MAX - 1, nodes[3]->my_ed25519_privkey, end) == STREAM_OK);
    CHECK(stream_end_verify(&origin.descriptor, nodes[3]->my_ed25519_pubkey, end, sizeof(end), &last) == STREAM_OK && last == UINT64_MAX - 1);
    end[105] ^= 1; CHECK(stream_end_verify(&origin.descriptor, nodes[3]->my_ed25519_pubkey, end, sizeof(end), &last) == STREAM_ERR_SIGNATURE);
    end[105] ^= 1; end[41] ^= 1;
    CHECK(stream_end_verify(&origin.descriptor, nodes[3]->my_ed25519_pubkey, end, sizeof(end), &last) == STREAM_ERR_SIGNATURE);
    for (unsigned i = 1; i < 3; i++) wait_state(i, 3, GROUP2, origin.descriptor.source_epoch, 0);
    for (unsigned i = 1; i < 3; i++) {
        CHECK(stream_subscribe_manual_open(nodes[i], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &subscribers[i]) == STREAM_OK);
        stream_set_event_callback(nodes[i], on_network, NULL);
    }
    network_write(publisher, 0); network_write(publisher, 1);
    struct stream_network *supplier = stream_network_get(nodes[3]);
    struct stream_peer *waiting = peer_open(supplier, stream_address_read(nodes[1]->my_ed25519_pubkey)); CHECK(waiting && !waiting->flows);
    waiting->idle = get_time_tb() - STREAM_LEASE_TB;
    uint64_t retained_epoch = waiting->own_epoch;
    uint8_t info_wire[STREAM_WIRE_HEADER + STREAM_INFO_REQUEST_SIZE] = {0};
    struct stream_message info_request = {.descriptor = origin.descriptor, .command = STREAM_CMD_INFO_REQ,
        .sender_epoch = waiting->other_epoch ? waiting->other_epoch : 123};
    stream_wire_header(info_wire, &info_request); stream_write_be(info_wire + STREAM_WIRE_HEADER, 1, 8);
    stream_write_be(info_wire + STREAM_WIRE_HEADER + 9, UINT64_MAX, 8);
    dispatch(supplier, stream_address_read(nodes[1]->my_ed25519_pubkey), info_wire, sizeof(info_wire), 0);
    CHECK(waiting->control->count);
    struct ll_entry *offered;
    while ((offered = queue_data_get(waiting->control))) {
        waiting->reply_count--; queue_dgram_free(offered); queue_entry_free(offered); queue_resume_callback(waiting->control);
    }
    if (supplier->timer) uasync_cancel_timeout(ua, supplier->timer);
    supplier->timer = NULL; network_tick(supplier);
    CHECK(peer_find(supplier, stream_address_read(nodes[1]->my_ed25519_pubkey)) == waiting && waiting->own_epoch == retained_epoch);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] INFO_RESP refreshes expired idle session before SUBSCRIBE; epoch retained");
    for (unsigned i = 1; i < 4; i++) {
        saved_raw[i] = nodes[i]->api_bindings.callbacks[ETCP_ID_STREAM]; CHECK(saved_raw[i] != NULL);
        CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, network_raw) == 0);
    }
    saved_route = nodes[3]->router_bindings.callbacks[ETCP_RT_ID_STREAM]; CHECK(saved_route != NULL);
    CHECK(etcp_router_bind(nodes[3], ETCP_RT_ID_STREAM, network_routed) == 0);
    struct STREAM_RECEIVE *up[8] = {0}, *down[8] = {0};
    for (unsigned i = 0; i < 8; i++) CHECK(stream_receive_open(subscribers[1], stream_address_read(nodes[3]->my_ed25519_pubkey), i, UINT64_MAX, &up[i]) == STREAM_OK);
    run_until(&network_data[1], 1);
    for (unsigned i = 0; i < 8; i++) {
        struct stream_receive_info info; CHECK(stream_receive_get_info(up[i], &info) == STREAM_OK);
        if (!info.blocks) { deadline = get_time_tb() + 100000;
            do { test_clock_poll(ua, 100); CHECK(stream_receive_get_info(up[i], &info) == STREAM_OK); }
            while (!info.blocks && get_time_tb() < deadline);
        }
        CHECK(info.blocks == 1 && info.start == 1 && info.next == 2 && info.handle);
    }
    struct stream_buffer_positions positions;
    CHECK(stream_network_buffer_positions(nodes[3], &origin.descriptor, &positions) == STREAM_OK);
    CHECK(positions.first == 0 && positions.head == 2 && positions.count == 1 && positions.total == 1);
    CHECK(stream_address_equal(positions.peers[0].peer, stream_address_read(nodes[1]->my_ed25519_pubkey)) && positions.peers[0].subflows == 8);
    CHECK(positions.peers[0].worst == 2 && positions.peers[0].best == 2);
    CHECK(positions.peers[0].worst_us == 0 && positions.peers[0].best_us == 0);
    /* Время считается по содержимому пачек, а не по доле seq в кольце. */
    struct stream_peer *buffer_peer = peer_find(supplier, stream_address_read(nodes[1]->my_ed25519_pubkey)); CHECK(buffer_peer && buffer_peer->flows);
    struct STREAM_RECEIVE *delayed = buffer_peer->flows; CHECK(!delayed->outgoing && delayed->info.next == 2);
    delayed->info.next = 0;
    CHECK(stream_network_buffer_positions(nodes[3], &origin.descriptor, &positions) == STREAM_OK);
    CHECK(positions.peers[0].worst_us == UINT64_MAX && positions.peers[0].best_us == UINT64_MAX);
    struct stream_local timed; CHECK(stream_local_get(nodes[3], &origin.descriptor, &timed) == STREAM_OK);
    struct stream_frame_timing timing = {.valid = 1, .audio_us = 100000};
    CHECK(stream_buffer_set_timing(timed.buffer, 0, &timing) == STREAM_OK);
    timing.audio_us = 300000;
    CHECK(stream_buffer_set_timing(timed.buffer, 1, &timing) == STREAM_OK);
    CHECK(stream_network_buffer_positions(nodes[3], &origin.descriptor, &positions) == STREAM_OK);
    CHECK(positions.peers[0].worst_us == 400000 && positions.peers[0].best_us == 0);
    delayed->info.next = 1;
    CHECK(stream_network_buffer_positions(nodes[3], &origin.descriptor, &positions) == STREAM_OK);
    CHECK(positions.peers[0].worst_us == 300000 && positions.peers[0].best_us == 0);
    delayed->info.next = 2;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] send buffer time: unknown timing, 400/300 ms backlog and live subflows");
    /* Меняется только локальная stream generation; физический ETCP и поколение источника сохраняются. */
    struct stream_receive_info previous_generation; CHECK(stream_receive_get_info(up[0], &previous_generation) == STREAM_OK);
    struct ETCP_CONN *stable = instance_find_conn(nodes[1], nodes[3]->node_id); CHECK(stable && stable->links_up);
    int notified = 0;
    for (struct etcp_status_cbk_entry *cb = nodes[1]->conn_status_cbks; cb; cb = cb->next)
        if (cb->arg == stream_network_get(nodes[1])) { cb->fn(stable, ETCP_CONN_STATUS_REINIT, cb->arg); notified = 1; break; }
    CHECK(notified && stable->links_up);
    /* Старый DUPLICATE устанавливает общий старт, но не повторяет уже выданную приложению пачку. */
    CHECK(stream_receive_restart(subscribers[1]) == STREAM_OK);
    first_request[0] = 0;
    for (unsigned i = 0; i < 8; i++) {
        struct stream_receive_info info; CHECK(stream_receive_get_info(up[i], &info) == STREAM_OK && info.state == STREAM_RECEIVE_CLOSED);
        stream_receive_close(up[i]); up[i] = NULL;
        CHECK(stream_receive_open(subscribers[1], stream_address_read(nodes[3]->my_ed25519_pubkey), i, UINT64_MAX, &up[i]) == STREAM_OK);
    }
    struct stream_receive_info current_generation; CHECK(stream_receive_get_info(up[0], &current_generation) == STREAM_OK);
    deadline = get_time_tb() + 100000;
    do { test_clock_poll(ua, 100); CHECK(stream_receive_get_info(up[0], &current_generation) == STREAM_OK); }
    while (!current_generation.blocks && get_time_tb() < deadline);
    test_clock_run_for(ua, 1000);
    struct stream_local restarted; CHECK(stream_local_get(nodes[1], &origin.descriptor, &restarted) == STREAM_OK);
    CHECK(current_generation.blocks == 1 && restarted.anchored && restarted.start == 2 && restarted.next == 2);
    CHECK(network_data[1] == 1 && network_next[1] == 2);
    CHECK(current_generation.local_epoch != previous_generation.local_epoch && current_generation.peer_epoch == previous_generation.peer_epoch);
    CHECK(stable == instance_find_conn(nodes[1], nodes[3]->node_id) && stable->links_up);
    /* Node2 видит подписку relay через обычный Merkle-обмен, а не копирование из его SQLite. */
    deadline = get_time_tb() + 150000;
    int ready = 0;
    while (!ready && get_time_tb() < deadline) {
        struct stream_member_state member;
        ready = stream_member_load(nodes[2], GROUP2, stream_address_read(nodes[1]->my_ed25519_pubkey), &member, NULL) == STREAM_OK
                && stream_member_has_subscription(&member, stream_address_read(nodes[3]->my_ed25519_pubkey), origin.descriptor.source_epoch);
        if (!ready) test_clock_poll(ua, 100);
    }
    CHECK(ready);
    for (unsigned i = 0; i < 8; i++) CHECK(stream_receive_open(subscribers[2], stream_address_read(nodes[1]->my_ed25519_pubkey), i, UINT64_MAX, &down[i]) == STREAM_OK);
    run_until(&network_data[2], 1);
    struct stream_local retained; CHECK(stream_local_get(nodes[2], &origin.descriptor, &retained) == STREAM_OK);
    struct stream_path path; CHECK(stream_buffer_path(retained.buffer, 1, 0, &path) == STREAM_OK);
    CHECK(path.count == 3 && stream_address_equal(path.nodes[0], stream_address_read(nodes[3]->my_ed25519_pubkey)) && stream_address_equal(path.nodes[1], stream_address_read(nodes[1]->my_ed25519_pubkey)) && stream_address_equal(path.nodes[2], stream_address_read(nodes[2]->my_ed25519_pubkey)));
    struct stream_telemetry telemetry;
    deadline = get_time_tb() + 20000;
    do {
        test_clock_poll(ua, 100);
        CHECK(stream_network_telemetry(nodes[2], &origin.descriptor, &telemetry) == STREAM_OK);
    } while ((telemetry.count != 2 || !telemetry.rows[0].sample || !telemetry.rows[1].sample) && get_time_tb() < deadline);
    CHECK(telemetry.count == 2 && stream_address_equal(telemetry.rows[0].node, stream_address_read(nodes[3]->my_ed25519_pubkey)) && stream_address_equal(telemetry.rows[1].node, stream_address_read(nodes[1]->my_ed25519_pubkey)));
    CHECK(telemetry.rows[0].sample && telemetry.rows[1].sample);
    drop_telemetry = 1; test_clock_run_for(ua, 7000);
    CHECK(stream_network_telemetry(nodes[2], &origin.descriptor, &telemetry) == STREAM_OK);
    CHECK((telemetry.rows[0].flags & STREAM_TELEMETRY_STALE) && !(telemetry.rows[1].flags & STREAM_TELEMETRY_STALE));
    drop_telemetry = 0; test_clock_run_for(ua, 5000);
    CHECK(stream_network_telemetry(nodes[2], &origin.descriptor, &telemetry) == STREAM_OK);
    CHECK(!(telemetry.rows[0].flags & STREAM_TELEMETRY_STALE) && !(telemetry.rows[1].flags & STREAM_TELEMETRY_STALE));
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] telemetry through relay: 200ms updates, stale upstream after 400ms, fresh local and recovery");
    /* Замкнуть сохранённую цепочку обратно в node1 нельзя, даже при актуальной Merkle-подписке. */
    struct STREAM_RECEIVE *loop = NULL;
    CHECK(stream_receive_open(subscribers[1], stream_address_read(nodes[2]->my_ed25519_pubkey), 0, 1, &loop) == STREAM_OK);
    struct stream_receive_info loop_info;
    deadline = get_time_tb() + 100000;
    do { test_clock_poll(ua, 100); CHECK(stream_receive_get_info(loop, &loop_info) == STREAM_OK); }
    while (loop_info.state != STREAM_RECEIVE_CLOSED && get_time_tb() < deadline);
    CHECK(loop_info.state == STREAM_RECEIVE_CLOSED && loop_info.reason == STREAM_REASON_LOOP); stream_receive_close(loop);
    /* Повтор SUBSCRIBE сохраняет handle и текущий курсор, не отправляет старый DATA заново. */
    CHECK(first_request[0]); unsigned before = accepts;
    struct ETCP_CONN *conn = instance_find_conn(nodes[1], nodes[3]->node_id); CHECK(conn && conn->links_up);
    while (conn->send_input_q->count) test_clock_poll(ua, 100);
    struct ll_entry *duplicate = ll_alloc_lldgram(sizeof(first_request)); CHECK(duplicate != NULL);
    memcpy(duplicate->dgram, first_request, sizeof(first_request)); duplicate->len = sizeof(first_request); CHECK(etcp_send(conn, duplicate) == 0);
    run_until(&accepts, before + 1);
    struct stream_receive_info info; CHECK(stream_receive_get_info(up[0], &info) == STREAM_OK && info.blocks == 1 && info.next == 2);
    /* Вторая группа на том же peer/NCD/ETCP использует иной FEC и сохраняется после закрытия выдач GROUP2. */
    struct STREAM_HANDLE *parallel_publisher = NULL, *parallel_subscriber = NULL;
    struct STREAM_RECEIVE *parallel = NULL; struct stream_parameters parameters = {.n = 1, .r = 0, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[3], GROUP1, &parameters, &parallel_publisher) == STREAM_OK);
    struct stream_info parallel_info; CHECK(stream_get_info(parallel_publisher, &parallel_info) == STREAM_OK);
    wait_state(1, 3, GROUP1, parallel_info.descriptor.source_epoch, 0);
    /* Две независимые публикации одного канала видны через общий каталог; подписчики не становятся источниками. */
    char *catalog = NULL, origin_id[80], parallel_id[80], subscriber_id[80];
    CHECK(read_catalogue(nodes[1], GROUP1, &catalog) == STREAM_OK);
    snprintf(origin_id, sizeof(origin_id), "\"id\":\"%s\"", sc_public_key_hex(nodes[0]->my_ed25519_pubkey, (char[65]){0}));
    snprintf(parallel_id, sizeof(parallel_id), "\"id\":\"%s\"", sc_public_key_hex(nodes[3]->my_ed25519_pubkey, (char[65]){0}));
    snprintf(subscriber_id, sizeof(subscriber_id), "\"id\":\"%s\"", sc_public_key_hex(nodes[1]->my_ed25519_pubkey, (char[65]){0}));
    CHECK(strstr(catalog, origin_id) && strstr(catalog, parallel_id) && !strstr(catalog, subscriber_id)); u_free(catalog);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] shared catalogue exposes two live publishers in the same channel");
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[3]->my_ed25519_pubkey), &parallel_subscriber) == STREAM_OK);
    size_t accepted = 0;
    CHECK(stream_publish_write(parallel_publisher, expected, sizeof(expected), &accepted) == STREAM_OK && accepted == sizeof(expected));
    deadline = get_time_tb() + 100000;
    do { test_clock_poll(ua, 100); CHECK(stream_get_info(parallel_publisher, &parallel_info) == STREAM_OK); }
    while (!parallel_info.buffer.blocks && get_time_tb() < deadline);
    CHECK(parallel_info.buffer.blocks == 1);
    CHECK(stream_receive_open(parallel_subscriber, stream_address_read(nodes[3]->my_ed25519_pubkey), 0, UINT64_MAX, &parallel) == STREAM_OK);
    run_until(&parallel_data, 1);
    /* Одна и та же routed reverse-попытка не меняет сессию/действующие handles. */
    struct stream_message reverse; CHECK(stream_fixture_parse(first_request, sizeof(first_request), &reverse) == STREAM_OK);
    reverse.command = STREAM_CMD_REVERSE_REQ; reverse.receiver_epoch = reverse.handle = 0;
    for (unsigned i = 0; i < 2; i++) {
        struct ll_entry *entry = ll_alloc_lldgram(1 + STREAM_WIRE_HEADER); CHECK(entry != NULL);
        entry->dgram[0] = ETCP_RT_ID_STREAM; stream_wire_header(entry->dgram + 1, &reverse); entry->len = 1 + STREAM_WIRE_HEADER;
        CHECK(etcp_route_send(nodes[1], GROUP2, nodes[3]->node_id, entry, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT) == 0);
    }
    run_until(&reverse_requests, 2);
    gap_batch = 1; network_write(publisher, 2); run_until(&network_gaps[1], 1); gap_batch = 0;
    /* Снять два лишних проверочных субпотока: шесть оставшихся восстанавливают новые пачки. */
    for (unsigned i = 6; i < 8; i++) { stream_receive_close(up[i]); up[i] = NULL; stream_receive_close(down[i]); down[i] = NULL; }
    network_write(publisher, 3); run_until(&network_data[1], 2); run_until(&network_data[2], 2); run_until(&network_gaps[2], 1);
    /* Relay пересылает первую часть, пока у него и downstream ещё нет k частей для проверки общей подписи. */
    for (unsigned sub = 1; sub < 6; sub++) { stream_receive_close(up[sub]); up[sub] = NULL; }
    CHECK(stream_receive_get_info(down[0], &info) == STREAM_OK && info.blocks == 2);
    network_write(publisher, 4); deadline = get_time_tb() + 100000;
    do { test_clock_poll(ua, 100); CHECK(stream_receive_get_info(down[0], &info) == STREAM_OK); }
    while (info.blocks < 3 && get_time_tb() < deadline);
    CHECK(info.blocks == 3 && network_data[1] == 2 && network_data[2] == 2);
    uint8_t pending_output[sizeof(expected)]; size_t pending_length = 0;
    for (unsigned node = 1; node < 3; node++) {
        struct stream_local pending; CHECK(stream_local_get(nodes[node], &origin.descriptor, &pending) == STREAM_OK);
        CHECK(pending.next == 4 && stream_buffer_decode(pending.buffer, 4, pending_output, sizeof(pending_output), &pending_length) == STREAM_WAIT);
    }
    for (unsigned sub = 1; sub < 6; sub++) CHECK(stream_receive_open(subscribers[1], stream_address_read(nodes[3]->my_ed25519_pubkey), sub, 4, &up[sub]) == STREAM_OK);
    run_until(&network_data[1], 3); run_until(&network_data[2], 3);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] live relay forwards before batch decode; consumers wait for k parts and source signature");
    CHECK(stream_publish_finish(publisher) == STREAM_OK);
    run_until(&network_end[1], 1); run_until(&network_end[2], 1);
    /* RENEW каждые 2с удерживает даже окончившуюся выдачу; затем имитируем отсутствие продлений. */
    test_clock_run_for(ua, 71000);
    CHECK(stream_receive_get_info(up[0], &info) == STREAM_OK && info.state == STREAM_RECEIVE_ENDED);
    drop_renew = 1; test_clock_run_for(ua, 71000); drop_renew = 0;
    CHECK(dropped_renew && stream_receive_get_info(up[0], &info) == STREAM_OK
          && info.state == STREAM_RECEIVE_CLOSED && info.reason == STREAM_REASON_LEASE);
    CHECK(stream_receive_get_info(parallel, &info) == STREAM_OK && info.state == STREAM_RECEIVE_ACTIVE);
    CHECK(stream_publish_write(parallel_publisher, expected, sizeof(expected), &accepted) == STREAM_OK && accepted == sizeof(expected));
    run_until(&parallel_data, 2);
    CHECK(stream_handle_active(reader1) && stream_handle_active(reader2));
    /* REINIT не обязан разрушать ETCP_CONN, но старые stream handles становятся недействительны. */
    conn = instance_find_conn(nodes[2], nodes[1]->node_id); CHECK(conn != NULL);
    etcp_conn_reinit(conn, "stream network test");
    deadline = get_time_tb() + 100000;
    do { test_clock_poll(ua, 100); CHECK(stream_receive_get_info(down[0], &info) == STREAM_OK); }
    while (info.state != STREAM_RECEIVE_CLOSED && get_time_tb() < deadline);
    CHECK(stream_receive_get_info(down[0], &info) == STREAM_OK && info.state == STREAM_RECEIVE_CLOSED
          && info.reason == STREAM_REASON_TRANSPORT);
    for (unsigned i = 0; i < 8; i++) { stream_receive_close(up[i]); stream_receive_close(down[i]); }
    stream_receive_close(parallel); stream_close(parallel_subscriber); stream_close(parallel_publisher);
    for (unsigned i = 1; i < 4; i++) CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, saved_raw[i]) == 0);
    CHECK(etcp_router_bind(nodes[3], ETCP_RT_ID_STREAM, saved_route) == 0);
    for (unsigned i = 1; i < 3; i++) stream_close(subscribers[i]); stream_close(publisher);
    test_clock_run_for(ua, 1000);
    stream_set_event_callback(nodes[1], on_stream, NULL); stream_set_event_callback(nodes[2], NULL, NULL);
    for (unsigned i = 0; i < 2; i++) topo_group_peer_close(connections[i]);
    debug_set_log_hook(NULL); debug_set_category_level(DEBUG_CATEGORY_VIDEO, previous_video);
    CHECK(reports.lines && reports.tx && reports.rx && reports.idle);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] live ETCP chain: common index/restart, FEC, paths/LOOP, GAP, duplicate/reverse, END, leases, REINIT");
}

/* Дожидаемся освобождения выдач и управляющего окна. reset_peers отмечает REINIT от последнего узла к остальным. */
static void wait_stream_teardown(const struct stream_descriptor *descriptor, unsigned node_count, unsigned reset_peers) {
    uint64_t start = get_time_tb();
    /* Согласование эпохи ETCP повторяется каждые 500 мс и может занять до 10 с. */
    const uint64_t timeout_tb = reset_peers ? 100000 : 10000;
    do {
        uint64_t elapsed = get_time_tb() - start;
        unsigned flows = 0, requests = 0, resets = 0;
        for (unsigned i = 0; i < node_count; i++) {
            struct stream_network *net = stream_network_get(nodes[i]);
            for (struct stream_peer *peer = net->peers; peer; peer = peer->next) {
                for (struct STREAM_RECEIVE *f = peer->flows; f; f = f->next)
                    flows += same_stream(&f->descriptor, descriptor);
                for (struct stream_pending_request *r = peer->requests; r; r = r->next) {
                    if (!same_stream(&r->descriptor, descriptor)) continue;
                    requests++;
                    if (elapsed >= timeout_tb)
                        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream teardown pending request node=%u peer=%s command=%u id=%llx sub=%u",
                            i, STREAM_ADDRESS_TEXT(peer->address), r->command, (unsigned long long)r->id, r->subblock);
                }
            }
        }
        for (unsigned i = 0; i + 1 < node_count; i++) if (reset_peers & (1u << i)) {
            struct ETCP_CONN *conn = instance_find_conn(nodes[node_count - 1], nodes[i]->node_id);
            if (!conn || !conn->reinit_pending) continue;
            resets++;
            if (elapsed >= timeout_tb)
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream teardown ETCP REINIT pending peer=%u links=%u reset_id=%llx",
                    i, conn->links_up, (unsigned long long)conn->reset_id);
        }
        if (!flows && !requests && !resets) {
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream teardown complete epoch=%llx elapsed_tb=%llu",
                (unsigned long long)descriptor->source_epoch, (unsigned long long)elapsed);
            return;
        }
        if (elapsed >= timeout_tb) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream teardown deadline epoch=%llx elapsed_tb=%llu flows=%u requests=%u resets=%u",
                (unsigned long long)descriptor->source_epoch, (unsigned long long)elapsed, flows, requests, resets);
            CHECK(0);
        }
        test_clock_poll(ua, 100);
    } while (1);
}

struct recovery_pending {
    struct recovery_pending *next;
    struct ETCP_CONN *conn;
    struct ll_entry *entry;
    uint64_t due_ms;
};
struct recovery_test {
    struct STREAM_HANDLE *publisher, *viewer;
    struct stream_descriptor descriptor;
    struct recovery_pending *head, *tail;
    etcp_recv_fn receive;
    uint64_t origin_us, next_frame_us, sent, frames, last_pts, last_frame_ms, maximum_gap_ms, maximum_lag_ms;
    struct ll_queue *pump_queue;
    queue_callback_fn pump_callback;
    void *pump_arg;
    uint64_t pump_due_us, common_start;
    unsigned pump_Bps, accepted, other_parts;
    unsigned delay_ms, overloads, overload_closed, media_gaps, expired_rejections, expired_offers, accepts;
    int paused, check_start, hold_part, hold_info;
};
static struct recovery_test *recovery;

/* Реальные raw ETCP-пакеты сохраняют порядок и владение; задержка имитирует путь, а не ответы selector. */
static void recovery_receive(struct ETCP_CONN *conn, struct ll_entry *entry) {
    struct stream_message message;
    int target = stream_fixture_parse(entry->dgram, entry->len, &message) == STREAM_OK
        && message.descriptor.source_epoch == recovery->descriptor.source_epoch;
    if (target) {
        if (message.command == STREAM_CMD_ACCEPT) recovery->accepts++;
        if (message.command == STREAM_CMD_OVERLOAD) recovery->overloads++;
        if (message.command == STREAM_CMD_REJECT && message.length && message.payload[0] == STREAM_REASON_EXPIRED)
            recovery->expired_rejections++;
        if (message.command == STREAM_CMD_INFO_RESP) {
            uint64_t seq = stream_read_be(message.payload + STREAM_INFO_POSITION_OFF, 8);
            uint64_t first = stream_read_be(message.payload + 16, 8);
            if (seq != UINT64_MAX && first != UINT64_MAX && seq < first && !message.payload[STREAM_INFO_POSITION_OFF + 24])
                recovery->expired_offers++;
        }
        if (message.command == STREAM_CMD_CLOSED && message.length && message.payload[0] == STREAM_REASON_OVERLOAD)
            recovery->overload_closed++;
        if (message.command == STREAM_CMD_ACCEPT && recovery->check_start) {
            CHECK(stream_read_be(message.payload + 8, 8) == recovery->common_start);
            if (++recovery->accepted == recovery->descriptor.n) recovery->check_start = 0;
        }
        if (message.command == STREAM_CMD_DATA && message.subblock) recovery->other_parts++;
    }
    int hold = target && ((recovery->hold_part && message.command == STREAM_CMD_DATA && !message.subblock)
        || (recovery->hold_info && message.command == STREAM_CMD_INFO_RESP));
    if ((recovery->hold_part || recovery->hold_info) && !hold) { recovery->receive(conn, entry); return; }
    if (!hold && !recovery->delay_ms && !recovery->head) { recovery->receive(conn, entry); return; }
    struct recovery_pending *pending = u_calloc(1, sizeof(*pending)); CHECK(pending && etcp_conn_ref_take(conn) == 0);
    pending->conn = conn; pending->entry = entry; pending->due_ms = get_time_tb() / 10 + recovery->delay_ms;
    if (hold) pending->due_ms = UINT64_MAX;
    if (recovery->tail && pending->due_ms < recovery->tail->due_ms) pending->due_ms = recovery->tail->due_ms;
    if (recovery->tail) recovery->tail->next = pending; else recovery->head = pending;
    recovery->tail = pending;
}

/* Полный access unit разбивается как в publisher: KEY 55КБ занимает несколько последовательных RS-пачек. */
static void recovery_publish(struct recovery_test *r, uint64_t now_us) {
    uint8_t video[55000] = {0}, wire[16384]; size_t length = 0;
    unsigned frame_size = r->sent % 50 ? 6000 : sizeof(video);
    memset(video + 4, (uint8_t)r->sent, frame_size - 4);
    struct stream_media_block block = {.format = {320, 180, 0, STREAM_MEDIA_H264, 0, 1}, .video_count = 1};
    size_t fragment = stream_publish_capacity(r->publisher) - STREAM_MEDIA_HEADER_SIZE - STREAM_MEDIA_VIDEO_SIZE;
    for (unsigned offset = 0; offset < frame_size;) {
        unsigned take = frame_size - offset < fragment ? frame_size - offset : fragment;
        block.video[0] = (struct stream_media_video){r->sent + 1, now_us - r->origin_us, frame_size, offset,
            take, r->sent % 50 ? 0 : STREAM_MEDIA_KEY, video + offset};
        CHECK(stream_media_encode(&block, wire, sizeof(wire), &length) == STREAM_OK);
        CHECK(stream_publish_block(r->publisher, wire, length) == STREAM_OK); offset += take;
    }
    r->sent++; r->next_frame_us = now_us + 20000;
}

/* В паузе автора ускоряем только пустое ожидание, учитывая отправку из очереди и выпуск задержанных пакетов. */
static void recovery_poll(struct recovery_test *r, uint64_t until_us) {
    if (!r->paused) { test_clock_poll(ua, 50); return; }
    uint64_t wake_us = get_time_us() + 40000;
    if (until_us < wake_us) wake_us = until_us;
    if (r->pump_queue && r->pump_queue->head && r->pump_due_us < wake_us) wake_us = r->pump_due_us;
    if (r->head && r->head->due_ms != UINT64_MAX) {
        uint64_t due_us = r->head->due_ms * 1000;
        if (due_us < wake_us) wake_us = due_us;
    }
    test_clock_poll_until(ua, 50, wake_us);
}

/* 50 кадров/с, KEY 55КБ на нескольких RS-пачках раз в секунду, n=8/r=0; payload проверяется после настоящего media cursor. */
static void recovery_run(struct recovery_test *r, unsigned duration_ms) {
    uint64_t until = get_time_tb() / 10 + duration_ms;
    while (get_time_tb() / 10 < until) {
        uint64_t now_us = get_time_us(), now_ms = get_time_tb() / 10;
        if (r->pump_queue && r->pump_queue->head && now_us >= r->pump_due_us) {
            r->pump_due_us = now_us + (uint64_t)r->pump_queue->head->len * 1000000 / r->pump_Bps;
            r->pump_callback(r->pump_queue, r->pump_arg);
        }
        while (r->head && now_ms >= r->head->due_ms) {
            struct recovery_pending *pending = r->head; r->head = pending->next;
            if (!r->head) r->tail = NULL;
            r->receive(pending->conn, pending->entry); etcp_conn_ref_free(pending->conn); u_free(pending);
        }
        if (!r->paused && now_us >= r->next_frame_us) {
            recovery_publish(r, now_us);
        }
        if (!r->viewer) { recovery_poll(r, until * 1000); continue; }
        for (unsigned work = 0; work < 32; work++) {
            struct stream_media_packet packet;
            int result = stream_media_read(r->viewer, -1, 1, -1, &packet);
            if (result == STREAM_WAIT || result == STREAM_MORE) break;
            if (result == STREAM_GAP || result == STREAM_EXPIRED) {
                r->media_gaps++; CHECK(stream_media_resync(r->viewer) == STREAM_OK); continue;
            }
            CHECK(result == STREAM_OK && packet.kind == STREAM_MEDIA_READ_VIDEO);
            CHECK(packet.video.length == ((packet.video.frame_id - 1) % 50 ? 6000 : 55000));
            CHECK(!r->frames || packet.video.pts_us > r->last_pts);
            for (unsigned i = 4; i < packet.video.length; i++) CHECK(packet.video.data[i] == (uint8_t)(packet.video.frame_id - 1));
            uint64_t gap = r->last_frame_ms ? now_ms - r->last_frame_ms : 0;
            uint64_t lag = (now_us - r->origin_us - packet.video.pts_us) / 1000;
            if (gap > r->maximum_gap_ms) r->maximum_gap_ms = gap;
            if (lag > r->maximum_lag_ms) r->maximum_lag_ms = lag;
            r->last_frame_ms = now_ms; r->last_pts = packet.video.pts_us; r->frames++;
        }
        if (r->last_frame_ms && now_ms - r->last_frame_ms > r->maximum_gap_ms)
            r->maximum_gap_ms = now_ms - r->last_frame_ms;
        recovery_poll(r, until * 1000);
    }
}

/* Сначала дожидаемся индекса всей опубликованной истории; fixed sleep зависит от нагрузки общего test runner. */
static struct stream_media_recovery recovery_position(struct recovery_test *r) {
    struct stream_local local; struct stream_media_recovery position; struct stream_buffer_stats stats;
    CHECK(stream_local_get(nodes[0], &r->descriptor, &local) == STREAM_OK && local.media);
    CHECK(stream_buffer_get_stats(local.buffer, &stats) == STREAM_OK && stats.blocks);
    uint64_t deadline = get_time_tb() + 100000;
    do {
        CHECK(stream_media_store_recovery(local.media, &position) == STREAM_OK);
        if (position.last_seq == stats.highest_seq) break;
        CHECK(get_time_tb() < deadline); test_clock_poll(ua, 100);
    } while (1);
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recovery fixture indexed head=%llu key=%llu..%llu valid=%d frames=%llu",
        (unsigned long long)position.last_seq, (unsigned long long)position.key_seq,
        (unsigned long long)position.key_last_seq, position.key_valid, (unsigned long long)r->sent);
    CHECK(position.key_valid); return position;
}

/* Заполняем настоящее кольцо тем же видео до вытеснения нужной позиции. Шаг в 50 кадров сохраняет позицию внутри GOP. */
static void recovery_expire(struct recovery_test *r, uint64_t seq) {
    struct stream_info info;
    unsigned frames = 0;
    do {
        for (unsigned frame = 0; frame < 50; frame++, frames++) {
            recovery_publish(r, get_time_us());
            if (!(frames % 16)) test_clock_poll(ua, 0);
        }
        CHECK(stream_get_info(r->publisher, &info) == STREAM_OK);
    } while (info.buffer.expired_before <= seq && frames < 6000);
    if (info.buffer.expired_before <= seq)
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "recovery source position still retained seq=%llu expired_before=%llu after %u frames",
            (unsigned long long)seq, (unsigned long long)info.buffer.expired_before, frames);
    CHECK(info.buffer.expired_before > seq);
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recovery source position evicted seq=%llu expired_before=%llu after %u frames",
        (unsigned long long)seq, (unsigned long long)info.buffer.expired_before, frames);
}

static void test_recovery_scenarios(void) {
    debug_level_t previous_video = g_debug_config.category_levels[DEBUG_CATEGORY_VIDEO];
    debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_DEBUG);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "recovery fixture: idle clock step up to 40ms during source pauses, bounded by packet and timer deadlines");
    /* Для пакетной публикации фиксируем нагрузку CPU. Пороги admission проверяются отдельным сценарием. */
    int previous_cpu = simulated_cpu; simulated_cpu = 30;
    unsigned failed = 0;
    for (unsigned scenario = 0; scenario < 9; scenario++) {
        int wifi = scenario == 1;
        struct recovery_test r = {0}; recovery = &r;
        CHECK(stream_publish_open(nodes[0], GROUP1, NULL, &r.publisher) == STREAM_OK);
        CHECK(stream_media_enable(r.publisher, 0) == STREAM_OK);
        struct stream_info info; CHECK(stream_get_info(r.publisher, &info) == STREAM_OK); r.descriptor = info.descriptor;
        CHECK(r.descriptor.n == 8 && !r.descriptor.r);
        wait_state(1, 0, GROUP1, r.descriptor.source_epoch, 0);
        r.origin_us = get_time_us(); r.next_frame_us = r.origin_us;
        if (scenario == 5 || scenario == 8) {
            recovery_run(&r, 2500);
            /* Явный P после KEY: 2,5с под нагрузкой могут закончиться ровно на KEY. */
            do { recovery_publish(&r, get_time_us()); } while ((r.sent - 1) % 50 == 0);
            r.paused = 1;
            struct stream_media_recovery position = recovery_position(&r);
            CHECK(position.key_last_seq > position.key_seq && position.last_seq > position.key_last_seq);
            r.check_start = scenario == 5; r.common_start = position.key_seq; r.hold_info = scenario == 8;
        }
        CHECK(stream_media_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), r.descriptor.source_epoch,
            STREAM_MEDIA_TRACK_VIDEO, &r.viewer) == STREAM_OK);
        r.receive = nodes[1]->api_bindings.callbacks[ETCP_ID_STREAM];
        CHECK(etcp_bind(nodes[1], ETCP_ID_STREAM, recovery_receive) == 0);
        if (scenario == 8) {
            recovery_run(&r, 100); CHECK(r.head && !r.frames);
            recovery_expire(&r, r.common_start);
            struct stream_media_recovery position = recovery_position(&r);
            CHECK(position.key_seq > r.common_start);
            r.common_start = position.key_seq; r.check_start = 1; r.hold_info = 0;
            for (struct recovery_pending *pending = r.head; pending; pending = pending->next) pending->due_ms = get_time_tb() / 10;
            recovery_run(&r, 6500);
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "recovery expired KEY accepted=%u frames=%llu expired_INFO=%u rejected_SUBSCRIBE=%u",
                r.accepted, (unsigned long long)r.frames, r.expired_offers, r.expired_rejections);
            CHECK(r.accepted == 8 && r.frames && r.expired_offers >= 8 && !r.expired_rejections);
        }
        r.delay_ms = wifi ? 100 : 0;
        recovery_run(&r, 3000); CHECK(r.frames >= (scenario == 5 || scenario == 8 ? 1 : 50));
        if (scenario == 5) CHECK(r.accepted == 8 && !r.media_gaps);
        r.maximum_gap_ms = r.maximum_lag_ms = 0;
        struct ETCP_CONN *conn = instance_find_conn(nodes[0], nodes[1]->node_id); CHECK(conn && conn->links_up);
        struct ll_queue *queue = conn->send_input_q;
        queue_callback_fn callback = queue->callback; void *arg = queue->callback_arg; CHECK(callback);
        if (wifi) {
            etcp_conn_reinit(conn, "test Wi-Fi network switch");
            recovery_run(&r, 3500);
        } else if (scenario == 3) {
            queue_set_callback(queue, NULL, NULL);
            r.pump_queue = queue; r.pump_callback = callback; r.pump_arg = arg; r.pump_Bps = 60000;
            recovery_run(&r, 10000); r.pump_queue = NULL;
            struct stream_selection_info selection; CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK);
            CHECK(!r.media_gaps && !selection.restarts && r.maximum_gap_ms < 3000);
            queue_set_callback(queue, callback, arg); queue_resume_callback(queue); recovery_run(&r, 3500);
        } else if (scenario == 4) {
            queue_set_callback(queue, NULL, NULL); recovery_run(&r, 4900);
            struct stream_selection_info selection; CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK);
            CHECK(!selection.restarts && !r.media_gaps);
            recovery_run(&r, 700);
            CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK && selection.restarts == 1);
            queue_set_callback(queue, callback, arg); queue_resume_callback(queue); recovery_run(&r, 3500);
        } else if (scenario == 7) {
            unsigned before = r.other_parts;
            r.hold_part = 1; recovery_run(&r, 4900);
            struct stream_selection_info selection; CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK);
            CHECK(!selection.restarts && !r.media_gaps && r.other_parts > before + 100);
            recovery_run(&r, 700);
            CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK && selection.restarts == 1);
            r.hold_part = 0;
            for (struct recovery_pending *pending = r.head; pending; pending = pending->next) pending->due_ms = get_time_tb() / 10;
            recovery_run(&r, 3500);
        } else if (scenario == 6) {
            queue_set_callback(queue, NULL, NULL);
            /* Реальное вытеснение 32-МБ source-кольца; транспорт не доставляет старую позицию. */
            CHECK(stream_get_info(r.publisher, &info) == STREAM_OK && info.buffer.blocks);
            recovery_expire(&r, info.buffer.highest_seq);
            r.paused = 1; recovery_run(&r, 6000);
            struct stream_media_recovery position = recovery_position(&r);
            r.common_start = position.key_seq; r.check_start = 1;
            queue_set_callback(queue, callback, arg); queue_resume_callback(queue); recovery_run(&r, 3500);
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "recovery expired history accepted=%u gaps=%u common_start=%llu frames=%u",
                r.accepted, r.media_gaps, (unsigned long long)r.common_start, r.frames);
            CHECK(r.accepted == 8 && r.media_gaps);
        } else if (scenario != 5 && scenario != 8) {
            /* Очередь растёт 850мс: как SM Q=165KB/671мс, но реальный транспорт остаётся UP. */
            queue_set_callback(queue, NULL, NULL); recovery_run(&r, scenario == 2 ? 2600 : 850);
            queue_set_callback(queue, callback, arg); queue_resume_callback(queue);
            recovery_run(&r, 3500); CHECK(r.overloads);
        }
        struct stream_selection_info selection; CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK);
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO,
            "recovery regression scenario=%s sent=%llu frames=%llu freeze_ms=%llu lag_ms=%llu overload=%u closed=%u gaps=%u restarts=%llu",
            wifi ? "wifi-switch-control-delay100ms" : scenario == 2 ? "stalled-wifi-2600ms" : scenario == 3 ? "sustained-60KBps"
                : scenario == 4 ? "stalled-wifi-5600ms" : scenario == 5 ? "late-viewer-complete-KEY"
                : scenario == 6 ? "expired-source-KEY" : scenario == 7 ? "one-part-stalled-5600ms"
                : scenario == 8 ? "KEY-evicted-after-INFO" : "slow-wifi-850ms",
            (unsigned long long)r.sent, (unsigned long long)r.frames,
            (unsigned long long)r.maximum_gap_ms, (unsigned long long)r.maximum_lag_ms,
            r.overloads, r.overload_closed, r.media_gaps, (unsigned long long)selection.restarts);
        failed += r.overload_closed != 0 || r.maximum_gap_ms > (wifi ? 1800 : scenario == 2 ? 4000 : scenario == 4 || scenario == 7 ? 8000
            : scenario == 5 || scenario == 8 ? 5000 : scenario == 6 ? 16000 : scenario == 3 ? 3000 : 1800)
            || (scenario != 5 && scenario != 6 && scenario != 8 && get_time_tb() / 10 - r.last_frame_ms >= 300);
        CHECK(scenario == 6 ? r.media_gaps > 0 : r.media_gaps == 0);
        CHECK(scenario == 6 || scenario == 8 ? selection.restarts >= 1 : selection.restarts == (scenario == 4 || scenario == 7 ? 1 : 0));
        /* Пауза автора не является поломкой пути и не должна перезапускать приёмник. */
        if (scenario == 8) {
            r.paused = 0; recovery_run(&r, 500);
            CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK);
        }
        uint64_t restarts = selection.restarts;
        r.paused = 1; recovery_run(&r, scenario >= 5 ? 500 : 2000);
        CHECK(stream_selection_get_info(r.viewer, &selection) == STREAM_OK && selection.restarts == restarts);
        r.delay_ms = 0;
        while (r.head) recovery_run(&r, 10);
        CHECK(etcp_bind(nodes[1], ETCP_ID_STREAM, r.receive) == 0);
        stream_close(r.viewer); stream_close(r.publisher); wait_stream_teardown(&r.descriptor, 2, 0);
        recovery = NULL;
    }
    debug_set_category_level(DEBUG_CATEGORY_VIDEO, previous_video);
    simulated_cpu = previous_cpu;
    CHECK(!failed);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM,
        "[PASS] recovery: large KEY, common start, transient/sustained congestion and exact 5s re-request without media reset");
}

/* Настоящий send_input_q намеренно держим непустым: короткий глитч не режет, устойчивый рост вызывает OVERLOAD. */
static void test_pressure(void) {
    debug_level_t previous_video = g_debug_config.category_levels[DEBUG_CATEGORY_VIDEO];
    debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_INFO);
    memset(pressure_payload, 42, sizeof(pressure_payload));
    struct STREAM_HANDLE *publisher = NULL, *subscriber = NULL;
    struct stream_parameters parameters = {.n = 8, .r = 2, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[3], GROUP2, &parameters, &publisher) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    wait_state(1, 3, GROUP2, origin.descriptor.source_epoch, 0);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &subscriber) == STREAM_OK);
    stream_set_event_callback(nodes[1], on_pressure, NULL);
    for (unsigned i = 1; i <= 3; i += 2) {
        pressure_handlers[i] = nodes[i]->api_bindings.callbacks[ETCP_ID_STREAM]; CHECK(pressure_handlers[i] != NULL);
        CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, pressure_raw) == 0);
    }
    size_t accepted = 0;
    CHECK(stream_publish_write(publisher, pressure_payload, sizeof(pressure_payload), &accepted) == STREAM_OK && accepted == sizeof(pressure_payload));
    struct STREAM_RECEIVE *receives[8] = {0};
    for (unsigned i = 0; i < 8; i++) CHECK(stream_receive_open(subscriber, stream_address_read(nodes[3]->my_ed25519_pubkey), i, UINT64_MAX, &receives[i]) == STREAM_OK);
    run_until(&pressure_data, 1);
    uint64_t deadline = get_time_tb() + 100000;
    while (get_time_tb() < deadline) {
        int ready = 1;
        for (unsigned i = 0; i < 8; i++) {
            struct stream_receive_info info; CHECK(stream_receive_get_info(receives[i], &info) == STREAM_OK);
            if (!info.blocks) ready = 0;
            CHECK(info.rtt_valid && info.supplier_metrics_valid);
        }
        if (ready) break; test_clock_poll(ua, 100);
    }
    struct ETCP_CONN *conn = instance_find_conn(nodes[3], nodes[1]->node_id); CHECK(conn && conn->links_up);
    struct ll_queue *queue = conn->send_input_q;
    while (queue->count) test_clock_poll(ua, 100);
    queue_callback_fn callback = queue->callback; void *callback_arg = queue->callback_arg;
    CHECK(callback != NULL); queue_set_callback(queue, NULL, NULL);
    for (unsigned i = 0; i < 4; i++) CHECK(stream_publish_write(publisher, pressure_payload, sizeof(pressure_payload), &accepted) == STREAM_OK);
    test_clock_run_for(ua, 3000);
    struct stream_peer_load_info info = source_load(); CHECK(info.load.queued.bytes && !info.request_id && !overload_seen);
    queue_set_callback(queue, callback, callback_arg); queue_resume_callback(queue);
    run_until(&pressure_data, 5);
    test_clock_run_for(ua, 1000); info = source_load(); CHECK(!info.load.queued.bytes && !info.request_id);
    /* Вход 8 пакетов/интервал, выход 7: число parity-закрытий зависит от measured rate; должно сохраниться k=6. */
    while (queue->count) test_clock_poll(ua, 100);
    queue_set_callback(queue, NULL, NULL);
    for (unsigned round = 0; round < 60; round++) {
        CHECK(stream_publish_write(publisher, pressure_payload, sizeof(pressure_payload), &accepted) == STREAM_OK);
        uint64_t target = source_load().load.sent.packets + 7;
        deadline = get_time_tb() + 100000;
        while (source_load().load.sent.packets < target && !source_load().released_count && get_time_tb() < deadline) {
            if (queue->count) callback(queue, callback_arg);
            test_clock_poll(ua, 100);
        }
        info = source_load();
        if (info.released_count) break;
        CHECK(info.load.sent.packets >= target);
        test_clock_run_for(ua, 2000);
    }
    info = source_load();
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "pressure fixture parity request=%llu requested=%llu/%u released=%llu/%u selects=%u",
        (unsigned long long)info.request_id, (unsigned long long)info.required_rate, info.required_count,
        (unsigned long long)info.released_rate, info.released_count, selects_seen);
    CHECK(info.released_count >= 1 && info.released_count <= 2 && selects_seen);
    /* Короткое окно измерения может запросить больше двух частей: manual-получатель отдаёт весь безопасный
     * избыток, но не закрывает необходимые k. Полный перенос при наличии резервов проверяет test_stream_overload. */
    if (info.required_count <= 2) CHECK(info.released_rate >= info.required_rate);
    else CHECK(info.released_count == 2 && info.released_rate < info.required_rate);
    unsigned parity_closed = info.released_count;
    queue_set_callback(queue, callback, callback_arg); queue_resume_callback(queue);
    deadline = get_time_tb() + 100000;
    for (;;) {
        unsigned received = 0;
        for (unsigned i = 6; i < 8; i++) {
            struct stream_receive_info state; CHECK(stream_receive_get_info(receives[i], &state) == STREAM_OK);
            if (state.state == STREAM_RECEIVE_CLOSED) received++;
        }
        if (received == parity_closed || get_time_tb() >= deadline) break;
        test_clock_poll(ua, 100);
    }
    for (unsigned i = 0; i < 8; i++) {
        struct stream_receive_info receive; CHECK(stream_receive_get_info(receives[i], &receive) == STREAM_OK);
        if (receive.state == STREAM_RECEIVE_CLOSED) CHECK(i >= 6 && receive.reason == STREAM_REASON_OVERLOAD);
        else CHECK(receive.state == STREAM_RECEIVE_ACTIVE);
    }
    test_clock_run_for(ua, 10000); CHECK(!source_load().load.queued.bytes && conn->links_up);
    unsigned delivered = pressure_data;
    CHECK(stream_publish_write(publisher, pressure_payload, sizeof(pressure_payload), &accepted) == STREAM_OK);
    run_until(&pressure_data, delivered + 1);
    CHECK(stream_receive_restart(subscriber) == STREAM_OK);
    for (unsigned i = 0; i < 8; i++) {
        stream_receive_close(receives[i]); receives[i] = NULL;
        CHECK(stream_receive_open(subscriber, stream_address_read(nodes[3]->my_ed25519_pubkey), i, UINT64_MAX, &receives[i]) == STREAM_OK);
    }
    test_clock_run_for(ua, 1000); CHECK(pressure_data == delivered + 1);
    CHECK(stream_publish_write(publisher, pressure_payload, sizeof(pressure_payload), &accepted) == STREAM_OK);
    run_until(&pressure_data, delivered + 2); test_clock_run_for(ua, 1000);
    uint64_t previous_request = source_load().request_id;
    overload_seen = selects_seen = 0; stale_select_length = 0;
    /* Тот же транспорт остаётся UP. Теперь держим очередь дольше подтверждения и возвращаем управление получателю. */
    while (queue->count) test_clock_poll(ua, 100);
    queue_set_callback(queue, NULL, NULL);
    for (unsigned i = 0; i < 24; i++) CHECK(stream_publish_write(publisher, pressure_payload, sizeof(pressure_payload), &accepted) == STREAM_OK);
    deadline = get_time_tb() + 15000;
    do { test_clock_poll(ua, 100); info = source_load(); } while ((!info.request_active || info.request_id == previous_request) && get_time_tb() < deadline);
    CHECK(info.request_active && info.required_count > 1 && info.required_rate && !info.released_count);
    uint64_t request = info.request_id, expires = info.deadline_ms;
    test_clock_run_for(ua, 3000); info = source_load(); CHECK(info.request_id == request && info.deadline_ms == expires);
    ignore_select = 1; queue_set_callback(queue, callback, callback_arg); queue_resume_callback(queue);
    deadline = get_time_tb() + 100000;
    while (!overload_seen && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(overload_seen);
    /* При паузе все части могут временно считаться остановленными. Тогда получатель вправе не выбирать ни одной. */
    while (get_time_tb() / 10 < expires + 100) test_clock_poll(ua, 100);
    info = source_load(); CHECK(!info.request_active && !info.released_count && !info.load.queued.bytes);
    unsigned closed = 0;
    for (unsigned i = 0; i < 8; i++) {
        struct stream_receive_info receive; CHECK(stream_receive_get_info(receives[i], &receive) == STREAM_OK);
        if (receive.state == STREAM_RECEIVE_CLOSED) { CHECK(receive.reason == STREAM_REASON_OVERLOAD); closed++; }
    }
    CHECK(!closed);
    /* Поздний выбор старого id не закрывает дополнительную выдачу. */
    ignore_select = 0;
    struct ETCP_CONN *back = instance_find_conn(nodes[1], nodes[3]->node_id); CHECK(back != NULL);
    while (back->send_input_q->count) test_clock_poll(ua, 100);
    if (!stale_select_length) {
        struct stream_peer* p = peer_find(stream_network_get(nodes[1]), stream_address_read(nodes[3]->my_ed25519_pubkey)); CHECK(p);
        struct stream_receive_info parity; CHECK(stream_receive_get_info(receives[6], &parity) == STREAM_OK && parity.handle);
        struct stream_message delayed = {.descriptor = receives[6]->descriptor, .command = STREAM_CMD_OVERLOAD_SELECT,
            .sender_epoch = p->own_epoch, .receiver_epoch = p->other_epoch};
        CHECK(stream_packet_header(nodes[1], stale_select, &delayed) == STREAM_OK);
        stream_write_be(stale_select + STREAM_WIRE_HEADER, request, 8);
        stale_select[STREAM_WIRE_HEADER + 8] = 1;
        stream_write_be(stale_select + STREAM_WIRE_HEADER + 9, parity.handle, 8);
        stale_select_length = STREAM_WIRE_HEADER + 17;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "pressure fixture: test late SELECT id=%llu after all offered parts had stopped",
            (unsigned long long)request);
    }
    struct ll_entry *late = ll_alloc_lldgram((uint16_t)stale_select_length); CHECK(late != NULL);
    memcpy(late->dgram, stale_select, stale_select_length); late->len = (uint16_t)stale_select_length; CHECK(etcp_send(back, late) == 0);
    test_clock_run_for(ua, 1000); CHECK(source_load().released_count == closed && conn->links_up);
    for (unsigned i = 0; i < 8; i++) stream_receive_close(receives[i]);
    stream_close(subscriber); stream_close(publisher); test_clock_run_for(ua, 1000);
    for (unsigned i = 1; i <= 3; i += 2) CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, pressure_handlers[i]) == 0);
    stream_set_event_callback(nodes[1], on_stream, NULL);
    CHECK(stream_handle_active(reader1) && stream_handle_active(reader2));
    debug_set_category_level(DEBUG_CATEGORY_VIDEO, previous_video);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] live overload: glitch, safe parity cooperation, fixed deadline, lost/stale SELECT preserves essential parts");
}

/* CPU>80 закрывает допуск сразу; восстановление <=70 за500мс, неизвестный CPU и поздний tick его не подменяют. */
static void test_cpu_admission(void) {
    struct stream_network net = {.inst = nodes[0], .callback_late_ms = 1000};
    stream_load_init(&net.load, 1000);
    net.cpu.valid = 1; net.cpu.average_percent = 30;
    pressure_tick(&net, 1000); pressure_tick(&net, 1700); CHECK(!net.global_pressure);
    net.cpu.average_percent = 80; pressure_tick(&net, 2000); CHECK(!net.global_pressure);
    net.cpu.average_percent = 80.1; pressure_tick(&net, 2001); CHECK(net.global_pressure);
    net.cpu.average_percent = 75;
    pressure_tick(&net, 2600); pressure_tick(&net, 4000); CHECK(net.global_pressure);
    net.cpu.valid = 0; pressure_tick(&net, 4100); CHECK(net.global_pressure);
    net.cpu.valid = 1; net.cpu.average_percent = 70;
    pressure_tick(&net, 4200); pressure_tick(&net, 4699); CHECK(net.global_pressure);
    pressure_tick(&net, 4700); CHECK(!net.global_pressure);
    net.cpu.average_percent = 81; pressure_tick(&net, 4800); CHECK(net.global_pressure);
    net.cpu.average_percent = 69; pressure_tick(&net, 4900); pressure_tick(&net, 5200); CHECK(net.global_pressure);
    net.cpu.average_percent = 71; pressure_tick(&net, 5300);
    net.cpu.average_percent = 70; pressure_tick(&net, 5400); pressure_tick(&net, 5899); CHECK(net.global_pressure);
    pressure_tick(&net, 5900); CHECK(!net.global_pressure);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] CPU admission: immediate >80, <=70 for500ms, recovery reset, unknown CPU and late loop");
}

/* При свободном CPU несколько очередей получают отдельные OVERLOAD. Высокий CPU запрещает только новые выдачи. */
static void test_global_pressure(int cpu) {
    debug_level_t previous_video = g_debug_config.category_levels[DEBUG_CATEGORY_VIDEO];
    debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_DEBUG);
    uint64_t ready_deadline = get_time_tb() + 10000;
    while (!source_load().accepting && get_time_tb() < ready_deadline) test_clock_poll(ua, 100);
    CHECK(source_load().accepting);
    memset(global_data, 0, sizeof(global_data));
    struct STREAM_HANDLE *publisher = NULL, *subscribers[3] = {0}; struct STREAM_RECEIVE *receives[3][2] = {{0}};
    struct stream_parameters parameters = {.n = 2, .r = 1, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[3], GROUP2, &parameters, &publisher) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    for (unsigned i = 1; i < 3; i++) {
        wait_state(i, 3, GROUP2, origin.descriptor.source_epoch, 0);
        CHECK(stream_subscribe_manual_open(nodes[i], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &subscribers[i]) == STREAM_OK);
        stream_set_event_callback(nodes[i], on_global_pressure, NULL);
    }
    uint8_t payload[SINGLE_INPUT_CAPACITY]; memset(payload, 3, sizeof(payload)); size_t accepted;
    CHECK(stream_publish_write(publisher, payload, sizeof(payload), &accepted) == STREAM_OK && accepted == sizeof(payload));
    for (unsigned i = 1; i < 3; i++) for (unsigned sub = 0; sub < 2; sub++)
        CHECK(stream_receive_open(subscribers[i], stream_address_read(nodes[3]->my_ed25519_pubkey), sub, UINT64_MAX, &receives[i][sub]) == STREAM_OK);
    for (unsigned i = 1; i < 3; i++) run_until(&global_data[i], 1);
    uint64_t previous_requests[3] = {0};
    for (unsigned i = 1; i < 3; i++) {
        struct stream_peer_load_info before; CHECK(stream_peer_get_load(nodes[3], stream_address_read(nodes[i]->my_ed25519_pubkey), &before) == STREAM_OK);
        previous_requests[i] = before.request_id;
    }
    simulated_cpu = cpu; test_clock_run_for(ua, 7000);
    struct stream_peer_load_info info[3];
    for (unsigned i = 1; i < 3; i++) {
        CHECK(stream_peer_get_load(nodes[3], stream_address_read(nodes[i]->my_ed25519_pubkey), &info[i]) == STREAM_OK);
        CHECK(info[i].cpu_percent == cpu && info[i].request_id == previous_requests[i] && !info[i].load.queued.bytes);
        CHECK(info[i].global_pressure == (cpu > 80) && info[i].accepting == (cpu <= 80));
    }
    struct STREAM_RECEIVE *probe = NULL;
    CHECK(stream_probe_open(subscribers[1], stream_address_read(nodes[3]->my_ed25519_pubkey), 0, UINT64_MAX, &probe) == STREAM_OK);
    struct stream_receive_info offer; uint64_t probe_deadline = get_time_tb() + 10000;
    do { test_clock_poll(ua, 100); CHECK(stream_receive_get_info(probe, &offer) == STREAM_OK); }
        while (!offer.info_ready && offer.state != STREAM_RECEIVE_CLOSED && get_time_tb() < probe_deadline);
    CHECK(offer.info_ready && offer.accepting == (cpu <= 80)); stream_receive_close(probe);
    if (cpu > 80) {
        simulated_cpu = 75; test_clock_run_for(ua, 7000); CHECK(source_load().global_pressure);
        simulated_cpu = 70; test_clock_run_for(ua, 3000); CHECK(source_load().global_pressure);
        test_clock_run_for(ua, 4000); CHECK(!source_load().global_pressure && source_load().accepting);
        simulated_cpu = cpu; test_clock_run_for(ua, 7000); CHECK(source_load().global_pressure && !source_load().accepting);
    }
    /* Даже при закрытом допуске действующие выдачи продолжают доставку. */
    unsigned before_data[3] = {0, global_data[1], global_data[2]};
    CHECK(stream_publish_write(publisher, payload, sizeof(payload), &accepted) == STREAM_OK);
    for (unsigned i = 1; i < 3; i++) run_until(&global_data[i], before_data[i] + 1);
    struct ll_queue *queues[3] = {0}; queue_callback_fn callbacks[3] = {0}; void *args[3] = {0};
    for (unsigned i = 1; i < 3; i++) {
        struct ETCP_CONN *conn = instance_find_conn(nodes[3], nodes[i]->node_id); CHECK(conn != NULL);
        queues[i] = conn->send_input_q;
        while (queues[i]->count) test_clock_poll(ua, 100);
        callbacks[i] = queues[i]->callback; args[i] = queues[i]->callback_arg; CHECK(callbacks[i] != NULL);
        queue_set_callback(queues[i], NULL, NULL);
    }
    for (unsigned i = 0; i < 48; i++) CHECK(stream_publish_write(publisher, payload, sizeof(payload), &accepted) == STREAM_OK);
    uint64_t deadline = get_time_tb() + 20000;
    for (;;) {
        int ready = 1;
        for (unsigned i = 1; i < 3; i++) {
            CHECK(stream_peer_get_load(nodes[3], stream_address_read(nodes[i]->my_ed25519_pubkey), &info[i]) == STREAM_OK);
            if (info[i].global_pressure != (cpu > 80) || !info[i].request_active) ready = 0;
        }
        if (ready || get_time_tb() >= deadline) break;
        test_clock_poll(ua, 100);
    }
    CHECK(info[1].global_pressure == (cpu > 80) && info[2].global_pressure == (cpu > 80));
    CHECK(info[1].request_active && info[2].request_active);
    for (unsigned i = 1; i < 3; i++) { queue_set_callback(queues[i], callbacks[i], args[i]); queue_resume_callback(queues[i]); }
    deadline = get_time_tb() + 100000;
    for (;;) {
        int done = 1;
        for (unsigned i = 1; i < 3; i++) {
            CHECK(stream_peer_get_load(nodes[3], stream_address_read(nodes[i]->my_ed25519_pubkey), &info[i]) == STREAM_OK);
            if (info[i].request_active) done = 0;
            CHECK(info[i].load.arrivals.bytes == info[i].load.sent.bytes + info[i].load.removed.bytes + info[i].load.queued.bytes);
        }
        if (done || get_time_tb() >= deadline) break;
        test_clock_poll(ua, 100);
    }
    for (unsigned i = 1; i < 3; i++) {
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "global overload drained cpu=%d peer=%u queued=%llu last_request=%llu released=%u",
            cpu, i, (unsigned long long)info[i].load.queued.bytes, (unsigned long long)info[i].request_id, info[i].released_count);
        CHECK(!info[i].load.queued.bytes && info[i].released_count <= 1);
        /* CLOSED должен дойти до получателя, не только освободить локальный flow отправителя. */
        unsigned closed = 0;
        do {
            struct stream_receive_info receive; CHECK(stream_receive_get_info(receives[i][1], &receive) == STREAM_OK);
            closed = receive.state == STREAM_RECEIVE_CLOSED && receive.reason == STREAM_REASON_OVERLOAD;
            if (!closed) test_clock_poll(ua, 100);
        } while (!closed && get_time_tb() < deadline);
        CHECK(closed);
        unsigned active = 0;
        for (unsigned sub = 0; sub < 2; sub++) {
            struct stream_receive_info receive; CHECK(stream_receive_get_info(receives[i][sub], &receive) == STREAM_OK);
            active += receive.state == STREAM_RECEIVE_ACTIVE;
        }
        CHECK(active == 1 && global_data[i] >= 1);
        struct ETCP_CONN *conn = instance_find_conn(nodes[3], nodes[i]->node_id); CHECK(conn && conn->links_up);
    }
    simulated_cpu = 30;
    for (unsigned i = 1; i < 3; i++) { for (unsigned sub = 0; sub < 2; sub++) stream_receive_close(receives[i][sub]); stream_close(subscribers[i]); }
    stream_close(publisher); test_clock_run_for(ua, 7000);
    stream_set_event_callback(nodes[1], on_stream, NULL); stream_set_event_callback(nodes[2], NULL, NULL);
    debug_set_category_level(DEBUG_CATEGORY_VIDEO, previous_video);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] multi-peer overload CPU=%d: admission, two parity CLOSED, essential DATA and ETCP preserved", cpu);
}

/* INFO accepting=0 не замораживает старт на30с; повтор делит обычный бюджет и не создаёт DATA до допуска. */
static void test_admission_retry(void) {
    int previous_cpu = simulated_cpu; simulated_cpu = 30;
    test_clock_run_for(ua, 7000); CHECK(source_load().accepting);
    struct STREAM_HANDLE *publisher = NULL, *viewer = NULL;
    struct stream_parameters parameters = {.n = 1, .r = 0, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[3], GROUP2, &parameters, &publisher) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    wait_state(1, 3, GROUP2, origin.descriptor.source_epoch, 0);
    uint8_t payload[SINGLE_INPUT_CAPACITY]; memset(payload, 3, sizeof(payload)); size_t accepted;
    CHECK(stream_publish_write(publisher, payload, sizeof(payload), &accepted) == STREAM_OK && accepted == sizeof(payload));
    global_data[1] = 0; stream_set_event_callback(nodes[1], on_global_pressure, NULL);
    simulated_cpu = 81; test_clock_run_for(ua, 7000); CHECK(!source_load().accepting);
    CHECK(stream_subscribe_open(nodes[1], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &viewer) == STREAM_OK);
    test_clock_run_for(ua, 24000);
    struct stream_selection_info before, after; CHECK(stream_selection_get_info(viewer, &before) == STREAM_OK);
    CHECK(!global_data[1] && !before.subflows && before.probes_started >= 2 && before.probes_started <= 3 && before.cycles == 1);
    uint64_t started = get_time_tb(); simulated_cpu = 30;
    while (!global_data[1] && get_time_tb() - started < 30000) test_clock_poll(ua, 100);
    CHECK(source_load().accepting && global_data[1] == 1);
    CHECK(stream_selection_get_info(viewer, &after) == STREAM_OK);
    CHECK(after.probes_started > before.probes_started && after.cycles == 1 && !after.restarts && after.subflows == 1);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] admission INFO retry: DATA after CPU recovery elapsed_ms=%llu probes=%llu->%llu cycle=%llu",
        (unsigned long long)((get_time_tb() - started) / 10), (unsigned long long)before.probes_started,
        (unsigned long long)after.probes_started, (unsigned long long)after.cycles);
    stream_close(viewer); stream_close(publisher); test_clock_run_for(ua, 7000);
    stream_set_event_callback(nodes[1], on_stream, NULL); simulated_cpu = previous_cpu;
}

static unsigned budget_data[4];
static void on_budget_data(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    unsigned node = stream_address_equal(event->descriptor.source, stream_address_read(nodes[2]->my_ed25519_pubkey)) ? 2 : 3;
    if (!stream_address_equal(event->descriptor.source, stream_address_read(nodes[node]->my_ed25519_pubkey))) { on_stream(inst, event, arg); return; }
    CHECK(inst == nodes[1]);
    if (event->descriptor.group_id == GROUP2) {
        CHECK(node == 3 && event->type == STREAM_EVENT_CLOSED); return;
    }
    CHECK(event->descriptor.group_id == GROUP1);
    if (event->type == STREAM_EVENT_DATA) {
        CHECK(event->first_seq == budget_data[node] && event->length == SINGLE_INPUT_CAPACITY); budget_data[node]++;
    }
}

/* Две трансляции делят фоновые слоты. Ожидание местного ресурса не превращается в30-секундный backoff поставщика. */
static void test_shared_probe_budget(void) {
    struct TOPO_PEER_REQUEST *connection = NULL;
    CHECK(topo_group_peer_open(topo_groups_find(nodes[1]->topo_groups, GROUP2), nodes[3]->node_id, &connection) == 0);
    uint64_t deadline = get_time_tb() + 150000;
    while (topo_group_peer_phase(connection) != TOPO_PEER_READY && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(topo_group_peer_phase(connection) == TOPO_PEER_READY);
    struct STREAM_HANDLE *background_source = NULL, *background_reader = NULL, *publishers[4] = {0}, *viewers[4] = {0};
    struct stream_parameters parameters = {.n = 16, .r = 15, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[3], GROUP2, &parameters, &background_source) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(background_source, &origin) == STREAM_OK);
    wait_state(1, 3, GROUP2, origin.descriptor.source_epoch, 0);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &background_reader) == STREAM_OK);
    struct STREAM_RECEIVE *probes[16] = {0};
    for (unsigned i = 0; i < 16; i++)
        CHECK(stream_probe_open(background_reader, stream_address_read(nodes[3]->my_ed25519_pubkey), (uint8_t)i, UINT64_MAX, &probes[i]) == STREAM_OK);
    struct stream_network *net = stream_network_get(nodes[1]); CHECK(net->background_used == 16);
    parameters.n = 1; parameters.r = 0;
    uint8_t payload[SINGLE_INPUT_CAPACITY] = {0}; size_t accepted;
    stream_set_event_callback(nodes[1], on_budget_data, NULL); memset(budget_data, 0, sizeof(budget_data));
    for (unsigned node = 2; node < 4; node++) {
        CHECK(stream_publish_open(nodes[node], GROUP1, &parameters, &publishers[node]) == STREAM_OK);
        CHECK(stream_get_info(publishers[node], &origin) == STREAM_OK); wait_state(1, node, GROUP1, origin.descriptor.source_epoch, 0);
        CHECK(stream_publish_write(publishers[node], payload, sizeof(payload), &accepted) == STREAM_OK && accepted == sizeof(payload));
        CHECK(stream_subscribe_open(nodes[1], GROUP1, stream_address_read(nodes[node]->my_ed25519_pubkey), &viewers[node]) == STREAM_OK);
    }
    test_clock_run_for(ua, 2500);
    struct stream_selection_info before, after;
    for (unsigned node = 2; node < 4; node++) {
        CHECK(stream_selection_get_info(viewers[node], &before) == STREAM_OK);
        CHECK(!budget_data[node] && !before.probes_started && !before.failures && net->background_used == 16);
    }
    uint64_t started = get_time_tb(); stream_receive_close(probes[0]); probes[0] = NULL;
    while ((!budget_data[2] || !budget_data[3]) && get_time_tb() - started < 10000) test_clock_poll(ua, 100);
    uint64_t elapsed = (get_time_tb() - started) / 10;
    for (unsigned node = 2; node < 4; node++) {
        CHECK(budget_data[node] == 1); CHECK(stream_selection_get_info(viewers[node], &after) == STREAM_OK);
        CHECK(after.probes_started == 1 && !after.failures && !after.restarts && after.subflows == 1);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] shared probe budget: source=%u DATA after release_ms=%llu probes=%llu failures=%llu",
            node, (unsigned long long)elapsed, (unsigned long long)after.probes_started, (unsigned long long)after.failures);
        stream_close(viewers[node]); stream_close(publishers[node]);
    }
    for (unsigned i = 0; i < 16; i++) stream_receive_close(probes[i]);
    stream_close(background_reader); stream_close(background_source); test_clock_run_for(ua, 2000);
    CHECK(!net->temporary_used && !net->background_used);
    stream_set_event_callback(nodes[1], on_stream, NULL);
    topo_group_peer_close(connection);
}

/* Default API сам выбирает выдачи: пустой источник, общий старт, цикл 30 с, overlap, stall и реальный CLOSED. */
static void test_automatic(void) {
    test_clock_run_for(ua, 10000);
    struct STREAM_HANDLE *publisher = NULL, *subscribers[3] = {0}, *second = NULL, *manual = NULL;
    struct stream_parameters parameters = {.n = 2, .r = 1, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[3], GROUP2, &parameters, &publisher) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    for (unsigned i = 1; i < 3; i++) wait_state(i, 3, GROUP2, origin.descriptor.source_epoch, 0);
    stream_set_event_callback(nodes[1], on_automatic, NULL); stream_set_event_callback(nodes[2], on_automatic, NULL);
    for (unsigned i = 1; i < 4; i++) {
        automatic_handlers[i] = nodes[i]->api_bindings.callbacks[ETCP_ID_STREAM];
        CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, automatic_raw) == 0);
    }
    CHECK(node_conn_direct_compatible(nodes[1], nodes[3]->node_id));
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &manual) == STREAM_OK);
    struct STREAM_RECEIVE *probe = NULL;
    CHECK(stream_probe_open(manual, stream_address_read(nodes[3]->my_ed25519_pubkey), 0, UINT64_MAX, &probe) == STREAM_OK);
    struct stream_receive_info probe_info; uint64_t deadline = get_time_tb() + 100000;
    do { test_clock_poll(ua, 100); CHECK(stream_receive_get_info(probe, &probe_info) == STREAM_OK); }
    while (!probe_info.info_ready && get_time_tb() < deadline);
    CHECK(probe_info.info_ready && probe_info.probe_only && !probe_info.handle && !probe_info.blocks && !automatic_accepts);
    CHECK(probe_info.first_available == UINT64_MAX && probe_info.last_available == UINT64_MAX);
    CHECK(probe_info.path.count == 1 && stream_address_equal(probe_info.path.nodes[0], stream_address_read(nodes[3]->my_ed25519_pubkey)));
    CHECK(probe_info.accepting && probe_info.free_slots && probe_info.path_rtt_valid && probe_info.rtt_valid);
    CHECK(stream_probe_next(probe, 1, UINT64_MAX) == STREAM_OK); deadline = get_time_tb() + 100000;
    do { test_clock_poll(ua, 100); CHECK(stream_receive_get_info(probe, &probe_info) == STREAM_OK); }
    while (!probe_info.info_ready && get_time_tb() < deadline);
    CHECK(probe_info.info_ready && probe_info.subblock == 1 && !automatic_accepts);
    stream_receive_close(probe); stream_close(manual); manual = NULL;
    CHECK(stream_subscribe_open(nodes[1], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &subscribers[1]) == STREAM_OK);
    CHECK(stream_subscribe_open(nodes[1], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &second) == STREAM_OK);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &manual) == STREAM_ERR_STATE && !manual);
    struct STREAM_RECEIVE *foreign = NULL;
    CHECK(stream_receive_open(subscribers[1], stream_address_read(nodes[3]->my_ed25519_pubkey), 0, UINT64_MAX, &foreign) == STREAM_ERR_ARGUMENT && !foreign);
    CHECK(stream_receive_restart(subscribers[1]) == STREAM_ERR_STATE);
    deadline = get_time_tb() + 150000;
    int relay_announced = 0;
    do {
        struct stream_member_state member;
        relay_announced = stream_member_load(nodes[2], GROUP2, stream_address_read(nodes[1]->my_ed25519_pubkey), &member, NULL) == STREAM_OK
            && stream_member_has_subscription(&member, stream_address_read(nodes[3]->my_ed25519_pubkey), origin.descriptor.source_epoch);
        if (!relay_announced) test_clock_poll(ua, 100);
    } while (!relay_announced && get_time_tb() < deadline);
    CHECK(relay_announced);
    CHECK(stream_subscribe_open(nodes[2], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &subscribers[2]) == STREAM_OK);
    CHECK(stream_service_needs_activity(nodes[2]) && nodes[2]->client_activity == CLIENT_ACTIVITY_ACTIVE);
    CHECK(utun_set_client_activity(nodes[2], 0) == 0 && nodes[2]->client_activity == CLIENT_ACTIVITY_ACTIVE);
    CHECK(nodes[2]->client_activity_requested == CLIENT_ACTIVITY_STANDBY);
    struct stream_selection_settings settings = {200, 1000, 100};
    for (unsigned i = 1; i < 3; i++) {
        CHECK(stream_selection_configure(subscribers[i], &settings) == STREAM_OK);
        stream_set_event_callback(nodes[i], on_automatic, NULL);
    }
    test_clock_run_for(ua, 3000);
    CHECK(!automatic_data[1] && !automatic_data[2]);
    size_t capacity = stream_publish_capacity(publisher);
    CHECK(capacity == SINGLE_INPUT_CAPACITY && capacity <= sizeof(pressure_payload));
    memset(pressure_payload, 0xa9, capacity); size_t accepted;
    /* Шаг до 25 мс сокращает пустые опросы, сохраняя сетевые таймеры и границу следующей пачки через 120 мс. */
    test_clock_init(250);
    uint64_t initial_probes[3] = {0}; struct stream_selection_info state[3];
    for (unsigned frame = 0; frame < 350; frame++) {
        CHECK(stream_publish_write(publisher, pressure_payload, capacity, &accepted) == STREAM_OK && accepted == capacity);
        test_clock_run_for(ua, 1200);
        if (frame == 80 || frame == 180) for (unsigned i = 1; i < 3; i++) {
            CHECK(stream_selection_get_info(subscribers[i], &state[i]) == STREAM_OK);
            CHECK(state[i].automatic && state[i].subflows == 2 && state[i].cycles == 1);
            if (frame == 80) initial_probes[i] = state[i].probes_started;
            else CHECK(state[i].probes_started >= initial_probes[i] && state[i].probes_started - initial_probes[i] <= 12);
        }
    }
    for (unsigned i = 1; i < 3; i++) {
        CHECK(stream_selection_get_info(subscribers[i], &state[i]) == STREAM_OK);
        CHECK(state[i].cycles == 2 && state[i].probes_started > initial_probes[i] && state[i].subflows == 2);
        CHECK(automatic_data[i] == 350 && !automatic_gaps[i]);
    }
    CHECK(state[2].migrations && state[2].providers == 2 && state[2].protected_by_fec);
    CHECK(!automatic_probes[2]); /* Mobile relay не пробуется ни одним планировщиком. */
    struct stream_selection_info same; CHECK(stream_selection_get_info(second, &same) == STREAM_OK);
    CHECK(same.probes_started == state[1].probes_started); stream_close(second); second = NULL;
    uint64_t migrations = state[2].migrations; automatic_drop_relay = 1;
    for (unsigned frame = 0; frame < 30; frame++) {
        CHECK(stream_publish_write(publisher, pressure_payload, capacity, &accepted) == STREAM_OK);
        test_clock_run_for(ua, 1200);
    }
    CHECK(stream_selection_get_info(subscribers[2], &state[2]) == STREAM_OK);
    CHECK(state[2].migrations > migrations && state[2].providers == 1 && state[2].subflows == 2);
    CHECK(automatic_data[2] == 380 && !automatic_gaps[2]); automatic_drop_relay = 0;
    CHECK(automatic_request[0]); uint64_t failures = state[2].failures;
    struct ETCP_CONN *source_connection = instance_find_conn(nodes[3], nodes[2]->node_id); CHECK(source_connection && source_connection->links_up);
    struct ll_entry *cancel = ll_alloc_lldgram(sizeof(automatic_request)); CHECK(cancel);
    memcpy(cancel->dgram, automatic_request, sizeof(automatic_request)); cancel->len = sizeof(automatic_request);
    automatic_handlers[3](source_connection, cancel);
    for (unsigned frame = 0; frame < 20; frame++) {
        CHECK(stream_publish_write(publisher, pressure_payload, capacity, &accepted) == STREAM_OK);
        test_clock_run_for(ua, 1200);
    }
    CHECK(stream_selection_get_info(subscribers[2], &state[2]) == STREAM_OK);
    CHECK(state[2].failures > failures && state[2].providers == 2 && state[2].subflows == 2);
    CHECK(automatic_data[2] == 400 && !automatic_gaps[2] && source_connection->links_up);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "automatic paced delivery interval_ms=120 relay_batches=%u viewer_batches=%u gaps=%u/%u",
        automatic_data[1], automatic_data[2], automatic_gaps[1], automatic_gaps[2]);
    test_clock_init(STREAM_TEST_CLOCK_STEP_TB);
    CHECK(stream_publish_finish(publisher) == STREAM_OK); run_until(&automatic_end[1], 1); run_until(&automatic_end[2], 1);
    for (unsigned i = 1; i < 3; i++) stream_close(subscribers[i]); stream_close(publisher);
    publisher = subscribers[1] = subscribers[2] = NULL;
    CHECK(stream_publish_open(nodes[3], GROUP2, &parameters, &publisher) == STREAM_OK);
    CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    wait_state(2, 3, GROUP2, origin.descriptor.source_epoch, 0);
    CHECK(stream_subscribe_open(nodes[2], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &subscribers[2]) == STREAM_OK);
    test_clock_run_for(ua, 5000);
    CHECK(stream_publish_finish(publisher) == STREAM_OK); run_until(&automatic_end[2], 2);
    CHECK(automatic_data[2] == 400); stream_close(subscribers[2]); stream_close(publisher);
    subscribers[2] = publisher = NULL;
    test_clock_run_for(ua, 1000);
    /* END первого субпотока не выключает выбор недостающих: для 2+1 нужны ещё данные второй выдачи. */
    parameters.n = 3;
    CHECK(stream_publish_open(nodes[3], GROUP2, &parameters, &publisher) == STREAM_OK);
    CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    for (unsigned i = 1; i < 3; i++) {
        wait_state(i, 3, GROUP2, origin.descriptor.source_epoch, 0);
        CHECK(stream_subscribe_open(nodes[i], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &subscribers[i]) == STREAM_OK);
        CHECK(stream_selection_configure(subscribers[i], &settings) == STREAM_OK);
        automatic_next[i] = 0;
    }
    automatic_drop_tail = 1;
    test_clock_run_for(ua, 10000);
    size_t input_size = stream_input_size(&origin.descriptor);
    memset(pressure_payload, 0xa9, input_size);
    CHECK(stream_publish_write(publisher, pressure_payload, input_size, &accepted) == STREAM_OK && accepted == input_size);
    CHECK(stream_publish_finish(publisher) == STREAM_OK);
    struct stream_local terminal;
    deadline = get_time_tb() + 100000;
    do { test_clock_poll(ua, 100); CHECK(stream_local_get(nodes[2], &origin.descriptor, &terminal) == STREAM_OK); }
    while (!terminal.finished && get_time_tb() < deadline);
    CHECK(terminal.finished && terminal.anchored && !terminal.next && automatic_data[2] == 400 && automatic_end[2] == 2);
    automatic_drop_tail = 0;
    run_until(&automatic_data[2], 401); run_until(&automatic_end[2], 3);
    CHECK(automatic_next[2] == 1 && !automatic_gaps[2]);
    for (unsigned i = 1; i < 3; i++) stream_close(subscribers[i]); stream_close(publisher);
    test_clock_run_for(ua, 1000);
    for (unsigned i = 1; i < 4; i++) CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, automatic_handlers[i]) == 0);
    stream_set_event_callback(nodes[1], on_stream, NULL); stream_set_event_callback(nodes[2], NULL, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] automatic streams: INFO-only, shared owners, 30s cycle, FEC spread, overlap, stall, CLOSED and early END drain");
}

/* Ожидаем настоящий INFO_RESP; закрытие вместо ответа немедленно показывает состояние/причину. */
static void wait_probe_info(struct STREAM_RECEIVE *probe) {
    struct stream_receive_info info;
    uint64_t deadline = get_time_tb() + 50000;
    do {
        CHECK(stream_receive_get_info(probe, &info) == STREAM_OK);
        if (info.info_ready) return;
        if (info.state == STREAM_RECEIVE_CLOSED) break;
        test_clock_poll(ua, 100);
    } while (get_time_tb() < deadline);
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "INFO probe wait failed state=%d reason=%d peer=%llx sub=%u",
        info.state, info.reason, STREAM_ADDRESS_TEXT(info.peer), info.subblock);
    CHECK(0);
}

/* Переданный INFO может истечь в заблокированном ETCP; ещё не отправленный ждёт локально без штрафа поставщику. */
static void test_control_expiry(void) {
    for (int media = 0; media < 2; media++) {
        struct STREAM_HANDLE *publisher = NULL, *subscriber = NULL;
        struct stream_parameters parameters = {.n = 2, .r = 0, .block_size = STREAM_RS_DEFAULT_SIZE};
        CHECK(stream_publish_open(nodes[0], GROUP1, &parameters, &publisher) == STREAM_OK);
        struct stream_info publication; CHECK(stream_get_info(publisher, &publication) == STREAM_OK);
        wait_state(1, 0, GROUP1, publication.descriptor.source_epoch, 0);
        CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &subscriber) == STREAM_OK);
        if (media) CHECK(stream_media_enable(subscriber, 0) == STREAM_OK);
        /* Сначала завершаем NCD/INFO, чтобы connect deadline не тратился на подготовку стенда. */
        struct STREAM_RECEIVE *warmup = NULL;
        CHECK(stream_probe_open(subscriber, stream_address_read(nodes[0]->my_ed25519_pubkey), 0, UINT64_MAX, &warmup) == STREAM_OK);
        wait_probe_info(warmup); stream_receive_close(warmup);
        struct ETCP_CONN *conn = instance_find_conn(nodes[1], nodes[0]->node_id); CHECK(conn && conn->links_up);
        struct ll_queue *queue = conn->send_input_q;
        while (queue->count) test_clock_poll(ua, 100);
        queue_callback_fn callback = queue->callback; void *callback_arg = queue->callback_arg; CHECK(callback);
        queue_set_callback(queue, NULL, NULL);
        struct STREAM_RECEIVE *probes[2] = {0};
        for (unsigned sub = 0; sub < 2; sub++) CHECK(stream_probe_open(subscriber, stream_address_read(nodes[0]->my_ed25519_pubkey), sub, UINT64_MAX, &probes[sub]) == STREAM_OK);
        struct stream_receive_info first, second;
        uint64_t started = get_time_tb(), deadline = started + 5000;
        do {
            CHECK(stream_receive_get_info(probes[0], &first) == STREAM_OK);
            CHECK(stream_receive_get_info(probes[1], &second) == STREAM_OK);
            if (queue->count && ((first.state == STREAM_RECEIVE_INFO && second.state == STREAM_RECEIVE_CONNECTING)
                || (second.state == STREAM_RECEIVE_INFO && first.state == STREAM_RECEIVE_CONNECTING))) break;
            if (queue->count) callback(queue, callback_arg);
            test_clock_poll(ua, 100);
        } while (get_time_tb() < deadline);
        CHECK(queue->count && ((first.state == STREAM_RECEIVE_INFO && second.state == STREAM_RECEIVE_CONNECTING)
            || (second.state == STREAM_RECEIVE_INFO && first.state == STREAM_RECEIVE_CONNECTING)));
        struct stream_message held;
        CHECK(stream_fixture_parse(queue->head->dgram, queue->head->len, &held) == STREAM_OK);
        CHECK(held.command == STREAM_CMD_INFO_REQ && held.length == STREAM_INFO_REQUEST_SIZE && held.payload[8] == media && held.subblock < 2);
        unsigned held_sub = held.subblock, waiting_sub = 1 - held_sub;
        test_clock_run_for(ua, 62000); /* Позже сетевого срока5с; локально ожидающий INFO не просрочен. */
        CHECK(stream_receive_get_info(probes[held_sub], &first) == STREAM_OK);
        CHECK(stream_receive_get_info(probes[waiting_sub], &second) == STREAM_OK);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM,
            "control backpressure media=%d sent_sub=%u state=%d reason=%d local_wait_sub=%u state=%d elapsed_ms=%llu queued=%d",
            media, held_sub, first.state, first.reason, waiting_sub, second.state,
            (unsigned long long)((get_time_tb() - started) / 10), queue->count);
        CHECK(first.state == STREAM_RECEIVE_CLOSED && first.reason == STREAM_REASON_TRANSPORT);
        CHECK(second.state == STREAM_RECEIVE_CONNECTING && !second.info_ready && queue->count);
        CHECK(stream_handle_active(subscriber) && stream_handle_active(publisher));
        CHECK(instance_find_conn(nodes[1], nodes[0]->node_id) == conn && conn->initialized && conn->links_up);
        queue_set_callback(queue, callback, callback_arg); queue_resume_callback(queue);
        wait_probe_info(probes[waiting_sub]);
        for (unsigned sub = 0; sub < 2; sub++) stream_receive_close(probes[sub]);
        struct STREAM_RECEIVE *reopened = NULL;
        CHECK(stream_probe_open(subscriber, stream_address_read(nodes[0]->my_ed25519_pubkey), held_sub, UINT64_MAX, &reopened) == STREAM_OK);
        wait_probe_info(reopened); stream_receive_close(reopened);
        CHECK(instance_find_conn(nodes[1], nodes[0]->node_id) == conn && conn->initialized && conn->links_up);
        stream_close(subscriber); stream_close(publisher);
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] control backpressure: INFO media0/1, sent timeout, local wait retained, late reply and recovery");
}

/* Ошибка размера/SQLite не меняет подпись, userinfo, версию и производное Merkle-дерево. */
static void test_profile_atomicity(void) {
    char before[MS_USERINFO_MAX + 1], after[MS_USERINFO_MAX + 1], huge[MS_USERINFO_MAX + 1];
    uint8_t before_signature[64], after_signature[64], before_hash[32], after_hash[32];
    uint64_t before_version, after_version;
    profile(0, GROUP1, before, before_signature, &before_version); root_hash(0, GROUP1, before_hash);
    memset(huge, 'p', MS_USERINFO_MAX); huge[MS_USERINFO_MAX] = 0;
    struct chat_profile_option options[] = {{"stream_source", "0000000000000001:8:2:1189:0"}, {"stream_subscriptions", huge}};
    CHECK(chat_core_set_my_profile_options(nodes[0], CHANNEL1, options, 2) < 0);
    profile(0, GROUP1, after, after_signature, &after_version); root_hash(0, GROUP1, after_hash);
    CHECK(!strcmp(before, after) && before_version == after_version && !memcmp(before_signature, after_signature, 64));
    CHECK(!memcmp(before_hash, after_hash, 32));
    CHECK(sqlite3_exec(nodes[0]->topo_sqlite_db, "CREATE TRIGGER reject_stream BEFORE UPDATE OF userinfo ON peers_" CHANNEL1 " "
          "BEGIN SELECT RAISE(ABORT,'injected stream profile failure'); END", NULL, NULL, NULL) == SQLITE_OK);
    struct STREAM_HANDLE *failed = NULL;
    CHECK(stream_publish_open(nodes[0], GROUP1, NULL, &failed) < 0 && !failed);
    CHECK(sqlite3_exec(nodes[0]->topo_sqlite_db, "DROP TRIGGER reject_stream", NULL, NULL, NULL) == SQLITE_OK);
    profile(0, GROUP1, after, after_signature, &after_version); root_hash(0, GROUP1, after_hash);
    CHECK(!strcmp(before, after) && before_version == after_version && !memcmp(before_hash, after_hash, 32));
    CHECK(!memcmp(before_signature, after_signature, 64));
    options[0] = (struct chat_profile_option){"pad", "retained"}; options[1] = (struct chat_profile_option){"extra", "value"};
    CHECK(chat_core_set_my_profile_options(nodes[0], CHANNEL1, options, 2) == 0);
    profile(0, GROUP1, after, after_signature, &after_version);
    char value[65];
    CHECK(json_flat_get(after, "avatar_file", value, sizeof(value)) == 0 && !strcmp(value, "retained-avatar"));
    CHECK(json_flat_get(after, "avatar_server", value, sizeof(value)) == 0 && !strcmp(value, "0011223344556677001122334455667700112233445566770011223344556677"));
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] profile updates: retained avatar/name/options and atomic size/DB rollback");
}

/* Блок владельца действительно подписан канальным ключом и проходит member_sync, профиль A сохраняется. */
static void relay_role(unsigned member, int enabled, unsigned version) {
    char tags[128]; uint8_t signature[64], body[256];
    snprintf(tags, sizeof(tags), "{\"ver\":\"%u\",\"supernode\":\"%s\"}", version, enabled ? "yes" : "no");
    int length = member_sync_build_owner_msg(CHANNEL2, nodes[member]->my_ed25519_pubkey, tags, body, sizeof(body)); CHECK(length > 0);
    CHECK(sc_ed25519_sign(group_keys[1].ed_private, body, (size_t)length, signature) == SC_OK);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(member_sync_put(nodes[i], CHANNEL2, nodes[member]->my_keys.public_key,
              nodes[member]->my_ed25519_pubkey, NULL, 0, NULL, 0, NULL, tags, signature, 0, 0, NULL) >= 0);
        CHECK(stream_member_supernode(nodes[i], GROUP2, stream_address_read(nodes[member]->my_ed25519_pubkey)) == enabled);
    }
}

/* Роль немедленно следует за подписанным блоком канала; deleted и чужой snapshot проверяются отдельно. */
static void test_supernode_lookup(void) {
    uint64_t member = nodes[1]->node_id;
    CHECK(stream_member_supernode(nodes[0], GROUP2, stream_address_read(nodes[1]->my_ed25519_pubkey)) == 0);
    relay_role(1, 1, 90);
    sqlite3* legacy = nodes[0]->topo_sqlite_db; nodes[0]->topo_sqlite_db = NULL;
    int cached_role = stream_member_supernode(nodes[0], GROUP2, stream_address_read(nodes[1]->my_ed25519_pubkey));
    nodes[0]->topo_sqlite_db = legacy; CHECK(cached_role == 1);
    CHECK(stream_member_supernode(nodes[0], GROUP1, stream_address_read(nodes[1]->my_ed25519_pubkey)) == 0);
    CHECK(stream_member_supernode(nodes[0], GROUP2, stream_address_read(group_keys[1].ed)) == 0);
    sqlite3 *db = nodes[0]->topo_sqlite_db; sqlite3_stmt *statement = NULL;
    CHECK(sqlite3_exec(db, "SAVEPOINT role_lookup", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "UPDATE peers_" CHANNEL2 " SET deleted=1 WHERE node_id=?", -1, &statement, NULL) == SQLITE_OK);
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)member);
    CHECK(sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(db) == 1); sqlite3_finalize(statement);
    CHECK(stream_member_supernode(nodes[0], GROUP2, stream_address_read(nodes[1]->my_ed25519_pubkey)) == 1); // незавершённая транзакция не меняет опубликованную роль
    CHECK(sqlite3_exec(db, "ROLLBACK TO role_lookup", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(stream_member_supernode(nodes[0], GROUP2, stream_address_read(nodes[1]->my_ed25519_pubkey)) == 1);
    CHECK(sqlite3_prepare_v2(db, "UPDATE node_snapshots SET record=x'00' WHERE node_id=?", -1, &statement, NULL) == SQLITE_OK);
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)nodes[2]->node_id);
    CHECK(sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(db) == 1); sqlite3_finalize(statement);
    CHECK(stream_member_supernode(nodes[0], GROUP2, stream_address_read(nodes[1]->my_ed25519_pubkey)) == 1);
    CHECK(sqlite3_exec(db, "ROLLBACK TO role_lookup; RELEASE role_lookup", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "UPDATE peers_" CHANNEL2 " SET deleted=? WHERE node_id=?", -1, &statement, NULL) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(statement, 2, (sqlite3_int64)member) == SQLITE_OK);
    for (int deleted = 1; deleted >= 0; deleted--) {
        CHECK(sqlite3_bind_int(statement, 1, deleted) == SQLITE_OK && sqlite3_step(statement) == SQLITE_DONE);
        CHECK(sqlite3_reset(statement) == SQLITE_OK && test_member_publish_access(nodes[0], CHANNEL2) == 0);
        CHECK(stream_member_supernode(nodes[0], GROUP2, stream_address_read(nodes[1]->my_ed25519_pubkey)) == !deleted);
    }
    CHECK(sqlite3_finalize(statement) == SQLITE_OK);
    relay_role(1, 0, 91);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] supernode lookup: signed grant/revoke, channel isolation, absent/deleted member, unrelated snapshot");
}

/* Прямой stream-вход источника блокируем только у зрителей; Merkle/BGP и получение суперузла остаются настоящими. */
static void relay_test_raw(struct ETCP_CONN *conn, struct ll_entry *entry) {
    unsigned index = 0; while (index < 4 && conn->instance != nodes[index]) index++; CHECK(index < 4);
    if (entry->len >= STREAM_WIRE_HEADER) {
        unsigned command = entry->dgram[2];
        if (relay_race.watch_provider && index == 2 && conn->peer_node_id == nodes[1]->node_id
            && command == STREAM_CMD_INFO_RESP && !relay_race.provider_info_tb) relay_race.provider_info_tb = get_time_tb();
        if (command == STREAM_CMD_SUBSCRIBE && conn->peer_node_id == nodes[2]->node_id) {
            if (index == 3) relay_race.source_subscribes++;
            if (index == 1) relay_race.helper_subscribes++;
        }
        if (index == 2 && conn->peer_node_id == nodes[3]->node_id && command == STREAM_CMD_INFO_RESP && relay_race.hold_info) {
            if (!relay_race.info_length) {
                CHECK(entry->len <= sizeof(relay_race.info)); relay_race.info_length = entry->len;
                memcpy(relay_race.info, entry->dgram, entry->len);
            }
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
        if (relay_race.check_starts && index == 2 && conn->peer_node_id == nodes[1]->node_id && command == STREAM_CMD_ACCEPT) {
            struct stream_message m; CHECK(stream_fixture_parse(entry->dgram, entry->len, &m) == STREAM_OK && m.length >= 16);
            uint64_t start = stream_read_be(m.payload + 8, 8);
            if (!relay_race.accepted) relay_race.start = start;
            CHECK(start == relay_race.start && m.subblock < 8); relay_race.accepted |= 1u << m.subblock;
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "bootstrap test helper ACCEPT sub=%u start=%llu mask=%02x",
                        m.subblock, (unsigned long long)start, relay_race.accepted);
        }
    }
    if (relay_drop_source && index != 1 && conn->peer_node_id == nodes[3]->node_id) {
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    relay_raw_handlers[index](conn, entry);
}

/* Сохраняем реальный подписанный routed-запрос для проверки позднего повтора после освобождения буфера. */
static void relay_test_routed(struct ETCP_CONN *conn, struct ll_entry *entry) {
    if (entry->len >= ROUTER_SVC_PAYLOAD_OFF + STREAM_WIRE_HEADER
        && entry->dgram[ROUTER_SVC_PAYLOAD_OFF + 2] == STREAM_CMD_RELAY_CANCEL) relay_cancels_seen++;
    if (entry->len >= ROUTER_SVC_PAYLOAD_OFF + STREAM_WIRE_HEADER
        && entry->dgram[ROUTER_SVC_PAYLOAD_OFF + 2] == STREAM_CMD_RELAY_REQUEST) {
        memcpy(relay_request_wire, entry->dgram + ROUTER_SVC_PAYLOAD_OFF, STREAM_WIRE_HEADER); relay_requests_seen++;
        if (!relay_race.request_tb) relay_race.request_tb = get_time_tb();
        if (relay_race.hold_request) { queue_dgram_free(entry); queue_entry_free(entry); return; }
    }
    relay_route_handler(conn, entry);
}

/* Счётчик STATUS выявляет ответы на дубликаты, не меняя подписанное управление и его порядок. */
static void relay_test_status(struct ETCP_CONN *conn, struct ll_entry *entry) {
    if (entry->len >= ROUTER_SVC_PAYLOAD_OFF + STREAM_WIRE_HEADER
        && entry->dgram[ROUTER_SVC_PAYLOAD_OFF + 2] == STREAM_CMD_RELAY_STATUS) {
        relay_statuses_seen++;
        size_t payload = ROUTER_SVC_PAYLOAD_OFF + STREAM_WIRE_HEADER;
        if (relay_race.hold_ready && entry->len > payload
            && (entry->dgram[payload] == STREAM_RELAY_READY || entry->dgram[payload] == STREAM_RELAY_BOUND)) {
            relay_race.ready_held++;
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "bootstrap test holds availability status=%u ordinal=%u",
                        entry->dgram[payload], relay_race.ready_held);
            queue_dgram_free(entry); queue_entry_free(entry); return;
        }
    }
    relay_status_handler(conn, entry);
}

/* Фиксируем принятую Merkle/BGP-опцию перед stream callback; PROFILE без нужной подписки не является поставщиком. */
static void relay_test_provider(const uint8_t ed[32], const char *channel, void *arg) {
    (void)arg;
    if (!relay_race.watch_provider || relay_race.provider_tb || memcmp(ed, nodes[1]->my_ed25519_pubkey, 32)
        || !channel || strcmp(channel, CHANNEL2)) return;
    struct stream_member_state member;
    if (stream_member_load(nodes[2], GROUP2, stream_address_read(nodes[1]->my_ed25519_pubkey), &member, NULL) == STREAM_OK
        && stream_member_online(nodes[2], GROUP2, stream_address_read(nodes[1]->my_ed25519_pubkey))
        && stream_member_has_subscription(&member, stream_address_read(nodes[3]->my_ed25519_pubkey), relay_race.epoch)) relay_race.provider_tb = get_time_tb();
}

/* Внутренний приём не вызывает GUI callback; публичный viewer на том же экземпляре вызывает его. */
static void relay_test_event(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)arg; unsigned index = 0; while (index < 3 && inst != nodes[index]) index++; CHECK(index < 3);
    CHECK(stream_address_equal(event->descriptor.source, stream_address_read(nodes[3]->my_ed25519_pubkey)) && event->descriptor.group_id == GROUP2);
    if (event->type == STREAM_EVENT_DATA) { CHECK(event->length == sizeof(expected)); relay_data[index]++; }
    if (event->type == STREAM_EVENT_END) relay_ends[index]++;
}

/* Повторяем управление через штатный подписанный router, сохраняя nonce/эпохи исходной попытки. */
static void relay_test_control(const struct stream_message *message) {
    struct ll_entry *entry = ll_alloc_lldgram(1 + STREAM_WIRE_HEADER); CHECK(entry);
    entry->dgram[0] = ETCP_RT_ID_STREAM; stream_wire_header(entry->dgram + 1, message);
    entry->len = 1 + STREAM_WIRE_HEADER;
    CHECK(etcp_route_send(nodes[2], GROUP2, nodes[1]->node_id, entry, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT) == 0);
}

/* Запоздалый настоящий INFO подаётся тому же raw dispatcher; старый probe nonce не должен менять победителя. */
static void relay_release_info(void) {
    CHECK(relay_race.info_length);
    struct ETCP_CONN *conn = instance_find_conn(nodes[2], nodes[3]->node_id); CHECK(conn && conn->links_up);
    struct ll_entry *entry = ll_alloc_lldgram((uint16_t)relay_race.info_length); CHECK(entry);
    memcpy(entry->dgram, relay_race.info, relay_race.info_length); entry->len = relay_race.info_length;
    relay_raw_handlers[2](conn, entry); relay_race.info_length = 0; relay_race.hold_info = 0;
}

/* Все проверки помощи используют подписанное управление и настоящий raw ETCP, не прямой ingest. */
static void test_supernode_relay(void) {
    int route_level = g_debug_config.category_levels[DEBUG_CATEGORY_ETCPROUTE];
    debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, DEBUG_LEVEL_INFO);
    /* Гонка поставщиков требует живых узлов; сон/пробуждение проверяются отдельным сценарием. */
    int activity[4];
    for (unsigned i = 0; i < 4; i++) {
        activity[i] = nodes[i]->client_activity_requested;
        CHECK(utun_set_client_activity(nodes[i], CLIENT_ACTIVITY_ACTIVE) == 0);
    }
    struct TOPO_PEER_REQUEST *connections[3] = {0};
    uint64_t deadline = get_time_tb() + 150000;
    for (unsigned i = 0; i < 3; i++) {
        CHECK(topo_group_peer_open(topo_groups_find(nodes[3]->topo_groups, GROUP2), nodes[i]->node_id, &connections[i]) == 0);
        while (topo_group_peer_phase(connections[i]) != TOPO_PEER_READY && get_time_tb() < deadline) test_clock_poll(ua, 100);
        CHECK(topo_group_peer_phase(connections[i]) == TOPO_PEER_READY);
        CHECK(member_sync_start(nodes[3], instance_find_conn_by_key(nodes[3], nodes[i]->my_keys.public_key), CHANNEL2, NULL, NULL) == 0);
        CHECK(member_sync_start(nodes[i], instance_find_conn_by_key(nodes[i], nodes[3]->my_keys.public_key), CHANNEL2, NULL, NULL) == 0);
    }
    relay_role(1, 1, 100);
    for (unsigned i = 0; i < 4; i++) {
        relay_raw_handlers[i] = nodes[i]->api_bindings.callbacks[ETCP_ID_STREAM];
        CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, relay_test_raw) == 0);
        if (i < 3) stream_set_event_callback(nodes[i], relay_test_event, NULL);
    }
    relay_route_handler = nodes[1]->router_bindings.callbacks[ETCP_RT_ID_STREAM];
    CHECK(etcp_router_bind(nodes[1], ETCP_RT_ID_STREAM, relay_test_routed) == 0);
    relay_status_handler = nodes[2]->router_bindings.callbacks[ETCP_RT_ID_STREAM];
    CHECK(etcp_router_bind(nodes[2], ETCP_RT_ID_STREAM, relay_test_status) == 0);
    CHECK(member_sync_add_props_cbk(nodes[2], relay_test_provider, NULL) == 0);
    struct stream_parameters parameters = {.n = 8, .r = 2, .block_size = STREAM_RS_DEFAULT_SIZE};
    struct STREAM_HANDLE *publisher = NULL, *viewers[3] = {0};
    CHECK(stream_publish_open(nodes[3], GROUP2, &parameters, &publisher) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    for (unsigned i = 0; i < 3; i++) wait_state(i, 3, GROUP2, origin.descriptor.source_epoch, 0);
    CHECK(stream_subscribe_open(nodes[2], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &viewers[2]) == STREAM_OK);
    test_clock_run_for(ua, 10000);
    struct stream_local local;
    CHECK(!relay_requests_seen && !relay_data[2]);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_WAIT);
    stream_close(viewers[2]); viewers[2] = NULL; test_clock_run_for(ua, 1000);
    CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
    CHECK(stream_subscribe_open(nodes[2], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &viewers[2]) == STREAM_OK);
    run_until(&relay_data[2], 1); test_clock_run_for(ua, 60000);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_WAIT && !relay_requests_seen);
    stream_close(viewers[2]); viewers[2] = NULL;
    test_clock_run_for(ua, 10000);
    /* Помощь начинается через 100 мс, но прямой INFO выигрывает до ответа помощника и отменяет его запрос. */
    relay_race.hold_info = relay_race.hold_request = 1;
    uint64_t started = get_time_tb();
    CHECK(stream_subscribe_open(nodes[2], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &viewers[2]) == STREAM_OK);
    test_clock_run_for(ua, 900); CHECK(!relay_requests_seen);
    deadline = get_time_tb() + 10000;
    while (!relay_requests_seen && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(relay_requests_seen && relay_race.info_length);
    /* Измеряем доставку через ETCP, включая другие callbacks и проверку MEMBERS, а не только 100-мс таймер.
     * Запрос не должен уйти раньше срока или ждать следующего секундного обновления списка поставщиков. */
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "bootstrap helper delivery elapsed_tb=%llu expected_tb=990..4999",
        (unsigned long long)(relay_race.request_tb - started));
    CHECK(relay_race.request_tb - started >= 990 && relay_race.request_tb - started < 5000);
    relay_release_info(); run_until(&relay_data[2], 2); test_clock_run_for(ua, 2000);
    CHECK(relay_race.source_subscribes && !relay_race.helper_subscribes);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_WAIT);
    struct stream_message cancelled; CHECK(stream_fixture_parse(relay_request_wire, sizeof(relay_request_wire), &cancelled) == STREAM_OK);
    relay_race.hold_request = 0; relay_test_control(&cancelled); test_clock_run_for(ua, 2000);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_WAIT);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] bootstrap direct wins after helper request: request_delay_ms=%llu, late REQUEST cancelled",
               (unsigned long long)((relay_race.request_tb - started) / 10));
    stream_close(viewers[2]); viewers[2] = NULL; test_clock_run_for(ua, 1000);
    /* Закрытие reader до дедлайна отменяет и таймер, и объединённую работу сети. */
    unsigned requests_before = relay_requests_seen;
    relay_race.hold_info = 1;
    CHECK(stream_subscribe_open(nodes[2], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &viewers[2]) == STREAM_OK);
    test_clock_run_for(ua, 500); stream_close(viewers[2]); viewers[2] = NULL;
    test_clock_run_for(ua, 2000); CHECK(relay_requests_seen == requests_before);
    /* Merkle объявляет буфер до STATUS READY/BOUND: выбранная выдача должна удержать STARTING-запрос. */
    memset(&relay_race, 0, sizeof(relay_race)); relay_race.hold_ready = relay_race.watch_provider = 1;
    relay_race.epoch = origin.descriptor.source_epoch;
    relay_drop_source = 1;
    unsigned cancellations_before = relay_cancels_seen;
    unsigned expected_data = relay_data[2] + 1;
    CHECK(stream_subscribe_open(nodes[2], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &viewers[2]) == STREAM_OK);
    deadline = get_time_tb() + 40000;
    while (relay_data[2] < expected_data && relay_cancels_seen == cancellations_before && get_time_tb() < deadline)
        test_clock_poll(ua, 100);
    CHECK(relay_race.ready_held && relay_cancels_seen == cancellations_before);
    CHECK(relay_data[2] == expected_data && relay_race.helper_subscribes);
    CHECK(relay_race.provider_tb && relay_race.provider_info_tb >= relay_race.provider_tb);
    CHECK(relay_race.provider_info_tb - relay_race.provider_tb < 2000);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] provider callback starts INFO without 1s roster wait: elapsed_ms=%llu",
               (unsigned long long)((relay_race.provider_info_tb - relay_race.provider_tb) / 10));
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] helper INFO before delayed READY/BOUND keeps selected pending issuance; no premature CANCEL");
    /* Проверяем отсутствие проб из-за повторных properties при живой DATA, отдельно от аварийного таймера паузы. */
    CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
    test_clock_run_for(ua, 2000);
    struct stream_selection_info stable_before, stable_after;
    CHECK(stream_selection_get_info(viewers[2], &stable_before) == STREAM_OK);
    for (unsigned version = 101; version <= 103; version++) {
        relay_role(1, 1, version);
        CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
        test_clock_run_for(ua, 1000);
    }
    test_clock_run_for(ua, 2000);
    CHECK(stream_selection_get_info(viewers[2], &stable_after) == STREAM_OK);
    CHECK(stable_after.probes_started == stable_before.probes_started);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] repeated provider property callbacks preserve probes and their cycle");
    /* У получателя исчезает BGP к помощнику, но прямые stream-выдачи и его подписанная роль остаются. */
    struct TOPO_GROUP *viewer_group = topo_groups_find(nodes[2]->topo_groups, GROUP2);
    struct TOPO_GROUP *helper_group = topo_groups_find(nodes[1]->topo_groups, GROUP2);
    struct TOPO_NODEPATH *helper_path = topo_node_best_path(topo_node_find_by_id(viewer_group, nodes[1]->node_id)); CHECK(helper_path);
    struct ETCP_CONN *via = helper_path->conn;
    uint8_t helper_announce[sizeof(struct TOPOMSG_HEADER) + TOPO_NODE_WIRE_MAX_SIZE] = {0};
    memcpy(((struct TOPOMSG_HEADER*)helper_announce)->group_key, topo_group_wire_id(helper_group), 32);
    int helper_bytes = topo_node_serialize(topo_node_registry_find(nodes[1]->topo_groups, nodes[1]->node_id), helper_group->local_node, 0, helper_announce + sizeof(struct TOPOMSG_HEADER), sizeof(helper_announce) - sizeof(struct TOPOMSG_HEADER), 0);
    CHECK(helper_bytes > 0);
    struct TOPOMSG_WITHDRAW_PKT helper_withdraw = {0};
    memcpy(helper_withdraw.ed25519, nodes[1]->my_ed25519_pubkey, sizeof(helper_withdraw.ed25519));
    unsigned withdrawn_paths = 0;
    while ((helper_path = topo_node_best_path(topo_node_find_by_id(viewer_group, nodes[1]->node_id)))) {
        CHECK(++withdrawn_paths <= 4);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "helper WITHDRAW ordinal=%u via=%llx hops=%u", withdrawn_paths,
            (unsigned long long)helper_path->conn->peer_node_id, helper_path->hop_count);
        CHECK(topo_group_process_withdraw(viewer_group, helper_path->conn, (const uint8_t *)&helper_withdraw, sizeof(helper_withdraw)) == 0);
    }
    CHECK(!stream_member_online(nodes[2], GROUP2, stream_address_read(nodes[1]->my_ed25519_pubkey)));
    for (unsigned i = 0; i < 10; i++) {
        unsigned before = relay_data[2];
        CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK); test_clock_run_for(ua, 2000);
        CHECK(relay_data[2] == before + 1 && relay_cancels_seen == cancellations_before);
        CHECK(relay_flows(stream_network_get(nodes[2]), &origin.descriptor, stream_address_read(nodes[1]->my_ed25519_pubkey), 1, 0));
    }
    CHECK(topo_group_process_nodeinfo(viewer_group, via, helper_announce, sizeof(struct TOPOMSG_HEADER) + (size_t)helper_bytes) == 0);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] intermediate helper BGP loss preserves bound relay request and direct DATA");
    stream_close(viewers[2]); viewers[2] = NULL; test_clock_run_for(ua, 40000);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_WAIT);
    memset(&relay_race, 0, sizeof(relay_race)); relay_race.hold_info = relay_race.check_starts = 1;
    started = get_time_tb();
    expected_data = relay_data[2] + 1;
    CHECK(stream_subscribe_open(nodes[2], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &viewers[2]) == STREAM_OK);
    deadline = get_time_tb() + 300000;
    while (relay_data[2] < expected_data && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(relay_data[2] == expected_data && relay_requests_seen && !relay_data[1]);
    CHECK(get_time_tb() - started < 10000 && relay_race.request_tb - started >= 990);
    uint64_t connected_ms = (get_time_tb() - started) / 10;
    /* Первый FEC готов после k ACCEPT; пустой ранний субпоток повторяется через 100ms плюс фазу network tick. */
    deadline = get_time_tb() + 5000;
    while (relay_race.accepted != 255 && get_time_tb() < deadline) test_clock_poll(ua, 100);
    if (relay_race.accepted != 255)
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "bootstrap test incomplete helper ACCEPT mask=%02x after first FEC", relay_race.accepted);
    CHECK(relay_race.accepted == 255);
    CHECK(relay_race.helper_subscribes && !relay_race.source_subscribes);
    relay_release_info(); test_clock_run_for(ua, 1000);
    CHECK(!relay_race.source_subscribes && relay_data[2] == expected_data);
    struct stream_message duplicate; CHECK(stream_fixture_parse(relay_request_wire, sizeof(relay_request_wire), &duplicate) == STREAM_OK);
    CHECK(duplicate.receiver_epoch);
    struct stream_network *relay_net = stream_network_get(nodes[1]);
    struct stream_peer *relay_peer = peer_find(relay_net, stream_address_read(nodes[2]->my_ed25519_pubkey)); CHECK(relay_peer);
    struct stream_relay_request *granted = relay_request_find(relay_net, &origin.descriptor, stream_address_read(nodes[2]->my_ed25519_pubkey), duplicate.handle, 0);
    CHECK(granted && granted->granted && granted->status < STREAM_RELAY_REJECTED);
    int previous_pressure = relay_net->global_pressure, previous_cooling = relay_peer->cooling;
    struct stream_message refused = duplicate; refused.receiver_epoch = 0;
    unsigned previous_slots = relay_net->regular_used;
    for (unsigned limit = 0; limit < 3; limit++) {
        refused.handle = refused.handle == UINT64_MAX ? 1 : refused.handle + 1;
        relay_net->global_pressure = limit == 0; relay_peer->cooling = limit == 1;
        if (limit == 2) relay_net->regular_used = STREAM_REGULAR_LIMIT;
        relay_test_control(&refused); test_clock_run_for(ua, 300);
        struct stream_relay_request *rejected = relay_request_find(relay_net, &origin.descriptor, stream_address_read(nodes[2]->my_ed25519_pubkey), refused.handle, 0);
        CHECK(rejected && !rejected->granted && rejected->status == STREAM_RELAY_REJECTED);
        CHECK(rejected->reason == (limit == 2 ? STREAM_REASON_LIMIT : STREAM_REASON_OVERLOAD));
        if (limit == 2) { CHECK(relay_net->regular_used == STREAM_REGULAR_LIMIT); relay_net->regular_used = previous_slots; }
    }
    relay_net->global_pressure = 1; relay_peer->cooling = 1;
    unsigned statuses_before = relay_statuses_seen;
    for (unsigned i = 0; i < 3; i++) { relay_test_control(&duplicate); test_clock_run_for(ua, 300); }
    CHECK(granted->status < STREAM_RELAY_REJECTED && relay_flows(relay_net, &origin.descriptor, stream_address_read(nodes[2]->my_ed25519_pubkey), 0, 0));
    relay_net->global_pressure = previous_pressure; relay_net->global_candidate = 0; relay_peer->cooling = previous_cooling;
    relay_peer->clear_since_ms = 0;
    CHECK(relay_statuses_seen == statuses_before);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM,
        "[PASS] relay admission: CPU/peer pressure and full slots reject new requests; repeated granted REQUEST preserves downstream");
    relay_race.check_starts = 0;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] bootstrap helper wins: elapsed_ms=%llu, n=8 r=2, common_start=%llu, stale direct INFO ignored",
               (unsigned long long)connected_ms, (unsigned long long)relay_race.start);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_OK && local.relay_only);
    /* Поздний обычный подписчик также удерживает раздачу, даже без собственного запроса помощи. */
    CHECK(stream_subscribe_open(nodes[0], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &viewers[0]) == STREAM_OK);
    run_until(&relay_data[0], 1);
    stream_close(viewers[2]); viewers[2] = NULL;
    test_clock_run_for(ua, 80000);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_OK);
    /* Локальный viewer меняет :r на обычную подписку и делит существующий буфер. */
    CHECK(sqlite3_exec(nodes[1]->topo_sqlite_db, "CREATE TRIGGER reject_relay_viewer BEFORE UPDATE OF userinfo ON peers_" CHANNEL2 " "
          "BEGIN SELECT RAISE(ABORT,'injected relay viewer failure'); END", NULL, NULL, NULL) == SQLITE_OK);
    struct STREAM_HANDLE *failed = NULL;
    CHECK(stream_subscribe_open(nodes[1], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &failed) < 0 && !failed);
    CHECK(sqlite3_exec(nodes[1]->topo_sqlite_db, "DROP TRIGGER reject_relay_viewer", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_OK && local.relay_only);
    CHECK(stream_subscribe_open(nodes[1], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &viewers[1]) == STREAM_OK);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_OK && !local.relay_only);
    unsigned before = relay_data[1];
    CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
    run_until(&relay_data[0], 2); run_until(&relay_data[1], before + 1);
    stream_close(viewers[0]); viewers[0] = NULL;
    test_clock_run_for(ua, 100000);
    CHECK(stream_handle_active(viewers[1]));
    CHECK(stream_get_info(viewers[1], &origin) == STREAM_OK && origin.owners == 1);
    stream_close(viewers[1]); viewers[1] = NULL;
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_WAIT);
    /* Поздний повтор завершённого запроса не подписывает суперузел заново. */
    struct stream_message replay; CHECK(stream_fixture_parse(relay_request_wire, sizeof(relay_request_wire), &replay) == STREAM_OK);
    CHECK(replay.receiver_epoch); relay_test_control(&replay);
    test_clock_run_for(ua, 50000);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_WAIT);
    /* Даже после удаления tombstone старое подтверждение поколения не может снова выделить owner. */
    test_clock_run_for_local_io(ua, 700000); relay_test_control(&replay); test_clock_run_for(ua, 20000);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_WAIT);
    replay.handle ^= UINT64_C(0x123456789abcdef0); replay.sender_epoch ^= UINT64_C(0x9876543210fedcba);
    replay.receiver_epoch = 0; replay.command = STREAM_CMD_RELAY_CANCEL; relay_test_control(&replay);
    replay.command = STREAM_CMD_RELAY_REQUEST; relay_test_control(&replay);
    test_clock_run_for(ua, 20000);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_WAIT);
    /* Два relay-only экземпляра не выбирают друг друга, даже если имеют буферы и живые BGP-пути. */
    struct STREAM_HANDLE *helpers[2] = {0};
    relay_role(0, 1, 100);
    CHECK(stream_relay_open(nodes[1], &origin.descriptor, &helpers[0]) == STREAM_OK);
    CHECK(stream_relay_open(nodes[0], &origin.descriptor, &helpers[1]) == STREAM_OK);
    test_clock_run_for(ua, 30000);
    for (unsigned i = 0; i < 2; i++) {
        struct stream_candidate *candidates = NULL; size_t count = 0;
        CHECK(stream_get_candidates(helpers[i], &candidates, &count) == STREAM_OK);
        for (size_t c = 0; c < count; c++) CHECK(!stream_address_equal(candidates[c].address, stream_address_read(nodes[i ? 1 : 0]->my_ed25519_pubkey)));
        u_free(candidates); stream_close(helpers[i]);
    }
    relay_role(0, 0, 101);
    stream_close(publisher); publisher = NULL; test_clock_run_for(ua, 10000);
    /* READY без SUBSCRIBE истекает; публичный reader сохраняется, внутренний приём останавливается. */
    CHECK(stream_publish_open(nodes[3], GROUP2, &parameters, &publisher) == STREAM_OK);
    CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    for (unsigned i = 0; i < 3; i++) wait_state(i, 3, GROUP2, origin.descriptor.source_epoch, 0);
    CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
    CHECK(stream_subscribe_manual_open(nodes[2], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &viewers[2]) == STREAM_OK);
    stream_relay_demand(nodes[2], &origin.descriptor, 1, get_time_tb() / 10);
    deadline = get_time_tb() + 100000;
    while (!stream_relay_confirmed(nodes[2], &origin.descriptor, stream_address_read(nodes[1]->my_ed25519_pubkey)) && get_time_tb() < deadline)
        test_clock_poll(ua, 100);
    CHECK(stream_relay_confirmed(nodes[2], &origin.descriptor, stream_address_read(nodes[1]->my_ed25519_pubkey)));
    test_clock_run_for(ua, 100000);
    CHECK(!stream_relay_confirmed(nodes[2], &origin.descriptor, stream_address_read(nodes[1]->my_ed25519_pubkey)));
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_WAIT && stream_handle_active(viewers[2]));
    stream_close(viewers[2]); viewers[2] = NULL; stream_close(publisher); publisher = NULL;
    test_clock_run_for(ua, 10000);
    /* Пустой END проходит помощь до READY/DATA; буфер освобождается, хотя источник handle ещё жив. */
    CHECK(stream_publish_open(nodes[3], GROUP2, &parameters, &publisher) == STREAM_OK);
    CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    for (unsigned i = 0; i < 3; i++) wait_state(i, 3, GROUP2, origin.descriptor.source_epoch, 0);
    CHECK(stream_subscribe_manual_open(nodes[2], GROUP2, stream_address_read(nodes[3]->my_ed25519_pubkey), &viewers[2]) == STREAM_OK);
    stream_relay_demand(nodes[2], &origin.descriptor, 1, get_time_tb() / 10);
    deadline = get_time_tb() + 100000;
    while (stream_local_get(nodes[1], &origin.descriptor, &local) != STREAM_OK && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_OK);
    test_clock_run_for(ua, 20000); CHECK(stream_publish_finish(publisher) == STREAM_OK);
    run_until(&relay_ends[2], 1); CHECK(!relay_ends[1]);
    test_clock_run_for(ua, 50000);
    CHECK(stream_local_get(nodes[1], &origin.descriptor, &local) == STREAM_WAIT);
    stream_close(viewers[2]); stream_close(publisher);
    relay_drop_source = 0; relay_role(1, 0, 104);
    for (unsigned i = 0; i < 4; i++) CHECK(utun_set_client_activity(nodes[i], activity[i]) == 0);
    CHECK(etcp_router_bind(nodes[1], ETCP_RT_ID_STREAM, relay_route_handler) == 0);
    CHECK(etcp_router_bind(nodes[2], ETCP_RT_ID_STREAM, relay_status_handler) == 0);
    member_sync_remove_props_cbk(nodes[2], relay_test_provider, NULL);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(etcp_bind(nodes[i], ETCP_ID_STREAM, relay_raw_handlers[i]) == 0);
        if (i < 3) stream_set_event_callback(nodes[i], i < 2 ? on_stream : NULL, NULL);
    }
    for (unsigned i = 0; i < 3; i++) topo_group_peer_close(connections[i]);
    debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, route_level);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] supernode assistance: direct success, fallback, shared viewers, idle release, replay, relay-only exclusion, empty END");
}

static unsigned member_data, member_closed;

/* После запрета DATA не выдаются приложению; старый reader завершается ровно одним CLOSED. */
static void member_event(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)arg; CHECK(inst == nodes[1]);
    if (event->descriptor.group_id != GROUP1) return;
    if (event->type == STREAM_EVENT_DATA) {
        CHECK(event->length == sizeof(expected) && !memcmp(event->data, expected, sizeof(expected)));
        member_data++;
    }
    if (event->type == STREAM_EVENT_CLOSED) { CHECK(event->reason == STREAM_CLOSE_MEMBER); member_closed++; }
}

/* Ошибка загрузки не публикует ни частичное состояние, ни ключ. */
static void check_member_load(unsigned destination, unsigned member, int expected_result) {
    struct stream_member_state state, saved; uint8_t key[32], previous[32];
    memset(&state, 0x5a, sizeof(state)); memcpy(&saved, &state, sizeof(state));
    memset(key, 0xa5, sizeof(key)); memcpy(previous, key, sizeof(key));
    sqlite3* legacy = nodes[destination]->topo_sqlite_db; nodes[destination]->topo_sqlite_db = NULL;
    int result = stream_member_load(nodes[destination], GROUP1, stream_address_read(nodes[member]->my_ed25519_pubkey), &state, key);
    nodes[destination]->topo_sqlite_db = legacy;
    CHECK(result == expected_result);
    if (result != STREAM_OK) CHECK(!memcmp(&state, &saved, sizeof(state)) && !memcmp(key, previous, sizeof(key)));
}

/* Запрет/разрешение поступает только через подписанный блок владельца и настоящий commit member_sync. */
static int member_owner_update(unsigned destination, unsigned member, unsigned version, int deleted, int corrupt) {
    char tags[128]; uint8_t signature[64], body[256];
    snprintf(tags, sizeof(tags), "{\"ver\":\"%u\",\"deleted\":\"%s\"}", version, deleted ? "yes" : "no");
    int length = member_sync_build_owner_msg(CHANNEL1, nodes[member]->my_ed25519_pubkey, tags, body, sizeof(body)); CHECK(length > 0);
    CHECK(sc_ed25519_sign(group_keys[0].ed_private, body, (size_t)length, signature) == SC_OK);
    if (corrupt) signature[0] ^= 1;
    int result = member_sync_put(nodes[destination], CHANNEL1, nodes[member]->my_keys.public_key,
        nodes[member]->my_ed25519_pubkey, NULL, 0, NULL, 0, NULL, tags, signature, 0, 0, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "member owner destination=%u member=%u ver=%u deleted=%d corrupt=%d result=%d",
               destination, member, version, deleted, corrupt, result);
    return result;
}

/* Ждём согласованную выдачу, не заменяя готовность фиксированной задержкой. */
static void wait_member_flow(struct STREAM_RECEIVE *flow) {
    struct stream_receive_info info;
    uint64_t deadline = get_time_tb() + 100000;
    do {
        CHECK(stream_receive_get_info(flow, &info) == STREAM_OK);
        if (info.state == STREAM_RECEIVE_ACTIVE) return;
        CHECK(info.state != STREAM_RECEIVE_CLOSED);
        test_clock_poll(ua, 100);
    } while (get_time_tb() < deadline);
    CHECK(info.state == STREAM_RECEIVE_ACTIVE);
}

/* Внешний handle остаётся доступен для диагностики и close после отсоединения. */
static void check_member_flow_closed(struct STREAM_RECEIVE *flow) {
    struct stream_receive_info info;
    CHECK(stream_receive_get_info(flow, &info) == STREAM_OK);
    CHECK(info.state == STREAM_RECEIVE_CLOSED && info.reason == STREAM_REASON_MEMBER);
}

/* Отдельный процесс test_stream_member разделяет fixture, но не состояния остальных stream-сценариев. */
static void test_member_lifecycle(void) {
    CHECK(!utun_set_client_activity(nodes[0], CLIENT_ACTIVITY_ACTIVE));
    CHECK(!utun_set_client_activity(nodes[2], CLIENT_ACTIVITY_ACTIVE));
    check_member_load(1, 0, STREAM_OK);
    sqlite3 *db = nodes[1]->topo_sqlite_db;
    CHECK(sqlite3_exec(db, "SAVEPOINT member_incomplete", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt *statement = NULL;
    CHECK(sqlite3_prepare_v2(db, "UPDATE peers_" CHANNEL1 " SET source=1 WHERE node_id=?", -1, &statement, NULL) == SQLITE_OK);
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)nodes[0]->node_id);
    CHECK(sqlite3_step(statement) == SQLITE_DONE); sqlite3_finalize(statement);
    check_member_load(1, 0, STREAM_OK); // неопубликованный source не меняет подтверждённый профиль
    CHECK(sqlite3_exec(db, "ROLLBACK TO member_incomplete; RELEASE member_incomplete", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "UPDATE peers_" CHANNEL1 " SET source=? WHERE node_id=?", -1, &statement, NULL) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(statement, 2, (sqlite3_int64)nodes[0]->node_id) == SQLITE_OK);
    for (int source = 1; source >= 0; source--) {
        CHECK(sqlite3_bind_int(statement, 1, source) == SQLITE_OK && sqlite3_step(statement) == SQLITE_DONE);
        CHECK(sqlite3_reset(statement) == SQLITE_OK && test_member_publish_access(nodes[1], CHANNEL1) == 0);
        check_member_load(1, 0, source ? STREAM_WAIT : STREAM_OK);
    }
    CHECK(sqlite3_finalize(statement) == SQLITE_OK);
    CHECK(member_owner_update(1, 0, 1000, 1, 1) < 0); check_member_load(1, 0, STREAM_OK);
    CHECK(member_owner_update(1, 0, 1000, 1, 0) >= 0); check_member_load(1, 0, STREAM_ERR_NOT_MEMBER);
    CHECK(member_owner_update(1, 0, 999, 0, 0) >= 0); check_member_load(1, 0, STREAM_ERR_NOT_MEMBER);
    CHECK(member_owner_update(1, 0, 1001, 0, 0) >= 0); check_member_load(1, 0, STREAM_OK);
    struct stream_member_state missing, saved;
    memset(&missing, 0x5a, sizeof(missing)); memcpy(&saved, &missing, sizeof(missing));
    CHECK(stream_member_load(nodes[1], GROUP1, stream_address_read(group_keys[0].ed), &missing, NULL) == STREAM_ERR_NOT_MEMBER);
    CHECK(!memcmp(&missing, &saved, sizeof(missing)));
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] member load: WAIT/NOT_MEMBER, unchanged outputs, signed/stale/invalid owner updates");

    struct STREAM_HANDLE *publisher = NULL, *other = NULL, *reader = NULL, *relay = NULL, *denied = NULL;
    struct STREAM_RECEIVE *receive = NULL, *relay_receive = NULL;
    struct stream_parameters parameters = {.n = 1, .r = 0, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[0], GROUP1, &parameters, &publisher) == STREAM_OK);
    CHECK(stream_publish_open(nodes[1], GROUP2, NULL, &other) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    wait_state(1, 0, GROUP1, origin.descriptor.source_epoch, 0);
    wait_state(2, 0, GROUP1, origin.descriptor.source_epoch, 0);
    stream_set_event_callback(nodes[1], member_event, NULL);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &reader) == STREAM_OK);
    CHECK(stream_receive_open(reader, stream_address_read(nodes[0]->my_ed25519_pubkey), 0, UINT64_MAX, &receive) == STREAM_OK);
    wait_member_flow(receive);
    memset(expected, 0x47, sizeof(expected));
    CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
    run_until(&member_data, 1);
    struct NODE_CONN_DIRECT *keep = NULL;
    CHECK(node_conn_direct_open(nodes[1], nodes[0]->node_id, NULL, NULL, &keep, NULL) >= 0 && keep);
    struct ETCP_CONN *transport = node_conn_direct_get_conn(keep); CHECK(transport && transport->links_up);

    CHECK(member_owner_update(1, 0, 1002, 1, 0) >= 0);
    check_member_load(1, 0, STREAM_ERR_NOT_MEMBER);
    CHECK(!stream_handle_active(reader) && stream_handle_reason(reader) == STREAM_CLOSE_MEMBER);
    check_member_flow_closed(receive);
    CHECK(stream_handle_active(other) && stream_handle_active(publisher) && transport->links_up);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &denied) == STREAM_ERR_NOT_MEMBER && !denied);
    CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
    uint64_t deadline = get_time_tb() + 100000;
    while (!member_closed && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(member_data == 1 && member_closed == 1 && transport->links_up);
    /* Исключённому источнику больше не выдаётся состав группы; отдельно проверяем локальный отзыв. */
    CHECK(member_owner_update(0, 0, 1002, 1, 0) >= 0);
    CHECK(!stream_handle_active(publisher) && stream_handle_reason(publisher) == STREAM_CLOSE_MEMBER);
    for (unsigned i = 0; i < 4; i++) CHECK(member_owner_update(i, 0, 1003, 0, 0) >= 0);
    CHECK(!stream_handle_active(reader));
    stream_receive_close(receive); receive = NULL; stream_close(reader); reader = NULL;
    stream_close(publisher); publisher = NULL;
    CHECK(stream_publish_open(nodes[0], GROUP1, &parameters, &publisher) == STREAM_OK);
    struct stream_info restarted; CHECK(stream_get_info(publisher, &restarted) == STREAM_OK);
    CHECK(restarted.descriptor.source_epoch != origin.descriptor.source_epoch); origin = restarted;
    wait_state(1, 0, GROUP1, origin.descriptor.source_epoch, 0);
    wait_state(2, 0, GROUP1, origin.descriptor.source_epoch, 0);
    CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &reader) == STREAM_OK);
    CHECK(stream_subscribe_manual_open(nodes[2], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &relay) == STREAM_OK);
    CHECK(stream_receive_open(relay, stream_address_read(nodes[0]->my_ed25519_pubkey), 0, UINT64_MAX, &relay_receive) == STREAM_OK);
    wait_member_flow(relay_receive);
    wait_state(1, 2, GROUP1, origin.descriptor.source_epoch, 1);
    struct stream_local local; deadline = get_time_tb() + 100000;
    do {
        test_clock_poll(ua, 100);
        CHECK(stream_local_get(nodes[2], &origin.descriptor, &local) == STREAM_OK);
    } while (!local.decoded && get_time_tb() < deadline);
    CHECK(local.decoded);
    CHECK(stream_receive_open(reader, stream_address_read(nodes[2]->my_ed25519_pubkey), 0, UINT64_MAX, &receive) == STREAM_OK);
    wait_member_flow(receive);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] source removal: handles/flows/publisher stop, other channel/NCD survive, new epoch after restore");

    CHECK(member_owner_update(1, 2, 1000, 1, 0) >= 0);
    check_member_flow_closed(receive);
    CHECK(stream_handle_active(reader) && stream_handle_active(relay) && stream_handle_active(other));
    struct stream_receive_info relay_info;
    CHECK(stream_receive_get_info(relay_receive, &relay_info) == STREAM_OK && relay_info.state == STREAM_RECEIVE_ACTIVE);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] supplier removal: only its issuance closes, source session/other upstream/channel survive");
    stream_receive_close(receive); receive = NULL;

    CHECK(member_owner_update(1, 1, 1000, 1, 0) >= 0);
    CHECK(!stream_handle_active(reader) && stream_handle_reason(reader) == STREAM_CLOSE_MEMBER);
    CHECK(stream_handle_active(other) && stream_handle_active(publisher) && stream_handle_active(relay));
    CHECK(stream_publish_open(nodes[1], GROUP1, NULL, &denied) == STREAM_ERR_NOT_MEMBER && !denied);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &denied) == STREAM_ERR_NOT_MEMBER && !denied);
    CHECK(stream_publish_block(other, expected, sizeof(expected)) == STREAM_OK);
    CHECK(stream_get_info(other, &restarted) == STREAM_OK && restarted.buffer.blocks > 0);
    CHECK(member_owner_update(1, 1, 1001, 0, 0) >= 0);
    CHECK(!stream_handle_active(reader));
    uint64_t ready_deadline = get_time_tb() + 100000;
    struct TOPO_GROUP* restored_group = topo_groups_find(nodes[1]->topo_groups, GROUP1);
    while (!topo_group_peer_ready(restored_group, nodes[0]->node_id) && get_time_tb() < ready_deadline) test_clock_poll(ua, 100);
    CHECK(topo_group_peer_ready(restored_group, nodes[0]->node_id));
    stream_close(reader); reader = NULL;
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &reader) == STREAM_OK);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] own removal: channel closes/new open denied, other channel writes, "
               "higher owner version restores new open");
    stream_set_event_callback(nodes[1], NULL, NULL);
    /* Ремонт повреждённого JOIN сохраняет блок владельца и локальный mute. */
    CHECK(radio_set_member_muted(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey, 1) == 0);
    CHECK(sqlite3_prepare_v2(db, "UPDATE peers_" CHANNEL1 " SET join_sig=randomblob(64) WHERE node_id=?",
                            -1, &statement, NULL) == SQLITE_OK);
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)nodes[0]->node_id);
    CHECK(sqlite3_step(statement) == SQLITE_DONE); sqlite3_finalize(statement);
    struct TOPO_GROUP *purge_group = topo_groups_find(nodes[1]->topo_groups, GROUP1);
    CHECK(topo_group_peer_ready(purge_group, nodes[0]->node_id));
    uint8_t purge_before[32], purge_after[32];
    CHECK(merkle_sync_read_hash(nodes[1], MT_DATA_MEMBERS, CHANNEL1, 0, 0, purge_before) == 0);
    for (int fault = 0; fault < 2; fault++) {
        const char* trigger = fault == 0
            ? "CREATE TRIGGER reject_member_purge BEFORE UPDATE OF join_sig ON peers_" CHANNEL1 " BEGIN SELECT RAISE(ABORT,'member JOIN repair'); END"
            : "CREATE TRIGGER reject_member_purge BEFORE DELETE ON merkle_tree_hash BEGIN SELECT RAISE(ABORT,'member purge index'); END";
        CHECK(sqlite3_exec(db, trigger, NULL, NULL, NULL) == SQLITE_OK);
        CHECK(member_sync_verify_and_purge(nodes[1], CHANNEL1) < 0);
        CHECK(radio_member_muted(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey) && stream_handle_active(reader));
        CHECK(stream_handle_active(other) && transport->links_up && node_conn_direct_get_conn(keep) == transport);
        CHECK(topo_group_peer_ready(purge_group, nodes[0]->node_id));
        CHECK(merkle_sync_read_hash(nodes[1], MT_DATA_MEMBERS, CHANNEL1, 0, 0, purge_after) == 0);
        CHECK(!memcmp(purge_before, purge_after, sizeof(purge_before)));
        CHECK(sqlite3_exec(db, "DROP TRIGGER reject_member_purge", NULL, NULL, NULL) == SQLITE_OK);
    }
    CHECK(member_sync_verify_and_purge(nodes[1], CHANNEL1) == 0);
    CHECK(radio_member_muted(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey) && !stream_handle_active(reader));
    CHECK(!topo_group_peer_ready(purge_group, nodes[0]->node_id));
    CHECK(stream_handle_active(other) && transport->links_up && node_conn_direct_get_conn(keep) == transport);
    struct stream_member_state other_member;
    CHECK(stream_member_load(nodes[1], GROUP2, stream_address_read(nodes[0]->my_ed25519_pubkey), &other_member, NULL) == STREAM_OK);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] JOIN repair keeps mute, closes the reader in this group and preserves NCD/other channels");
    stream_receive_close(relay_receive); stream_close(reader); stream_close(relay); stream_close(other); stream_close(publisher);
    node_conn_direct_close(keep);
}

/* Канонический список исключает неоднозначные IDs, дубли и обрезание значений. */
static void test_member_format(void) {
    struct stream_member_state state = {.source = {.group_id = GROUP1, .source = STREAM_TEST_ADDR(1), .source_epoch = 2,
        .n = 8, .r = 2, .block_size = STREAM_RS_DEFAULT_SIZE}, .published = 1, .audio_enabled = 1, .count = 2,
        .subscriptions = {{STREAM_TEST_ADDR(1), 2}, {STREAM_TEST_ADDR(UINT64_MAX), UINT64_MAX, 1}}};
    char source[MS_USERINFO_MAX + 1], subscriptions[MS_USERINFO_MAX + 1], json[2 * MS_USERINFO_MAX + 64];
    CHECK(stream_member_format(&state, source, subscriptions) == STREAM_OK);
    snprintf(json, sizeof(json), "{\"stream_source\":\"%s\",\"stream_subscriptions\":\"%s\"}", source, subscriptions);
    struct stream_member_state parsed;
    CHECK(stream_member_parse(GROUP1, STREAM_TEST_ADDR(1), json, &parsed) == STREAM_OK);
    CHECK(parsed.published && parsed.audio_enabled && parsed.source.source_epoch == 2 && parsed.count == 2);
    CHECK(stream_address_equal(parsed.subscriptions[1].source, STREAM_TEST_ADDR(UINT64_MAX)) && parsed.subscriptions[1].source_epoch == UINT64_MAX);
    CHECK(!parsed.subscriptions[0].relay_only && parsed.subscriptions[1].relay_only);
    const char *bad[] = {
        "{\"stream_subscriptions\":\"1:2\"}",
        "{\"stream_subscriptions\":\"0000000000000001:0000000000000002,\"}",
        "{\"stream_subscriptions\":\"0000000000000001:0000000000000002,0000000000000001:0000000000000002\"}",
        "{\"stream_source\":\"0000000000000002:256:2:1189:1\"}",
        "{\"stream_source\":\"0000000000000002:8:2:1189:2\",\"stream_subscriptions\":\"0000000000000001:0000000000000002\"}",
        "{\"stream_source\":\"0000000000000002:8:2:1189\"}",
        "{\"stream_source\":\"\",\"stream_source\":\"\"}"
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(stream_member_parse(GROUP1, STREAM_TEST_ADDR(1), bad[i], &parsed) == STREAM_ERR_FORMAT);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] member format: full 64-bit IDs, canonical lists, duplicate/overflow rejection");
}

static void wait_source_audio(uint64_t epoch, int enabled) {
    uint64_t deadline = get_time_tb() + 100000;
    for (;;) {
        struct stream_member_state state;
        if (stream_member_load(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &state, NULL) == STREAM_OK
            && state.published && state.source.source_epoch == epoch && state.audio_enabled == enabled) return;
        CHECK(get_time_tb() < deadline); test_clock_poll(ua, 100);
    }
}

static void test_viewer_radio_mute(void) {
    struct STREAM_HANDLE *publisher = NULL, *raw = NULL, *relay = NULL, *first = NULL, *second = NULL;
    struct stream_info info;
    struct stream_address source = stream_address_read(nodes[0]->my_ed25519_pubkey);
    CHECK(stream_publish_open(nodes[0], topo_groups_channel_slot(nodes[0]->topo_groups, CHANNEL1), NULL, &publisher) == STREAM_OK);
    CHECK(stream_publish_set_audio_enabled(publisher, 1) == STREAM_OK);
    CHECK(stream_get_info(publisher, &info) == STREAM_OK);
    uint64_t epoch = info.descriptor.source_epoch;
    wait_source_audio(epoch, 1);
    CHECK(!radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    struct stream_descriptor relay_key = info.descriptor; relay_key.group_id = GROUP1;
    CHECK(stream_relay_open(nodes[1], &relay_key, &relay) == STREAM_OK);
    CHECK(stream_subscribe_open(nodes[1], GROUP1, source, &raw) == STREAM_OK);
    CHECK(!radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    CHECK(stream_media_open(nodes[1], GROUP1, source, epoch, STREAM_MEDIA_TRACK_VIDEO, &first) == STREAM_OK);
    CHECK(stream_media_open(nodes[1], GROUP1, source, epoch, STREAM_MEDIA_TRACK_VIDEO | STREAM_MEDIA_TRACK_AUDIO, &second) == STREAM_OK);
    uint8_t video[8] = {0}, audio[1] = {1}, wire[512]; size_t length = 0;
    struct stream_media_block media = {.format = {64, 64, 0, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, 1},
        .video_count = 1, .audio_count = 1};
    media.video[0] = (struct stream_media_video){1, 0, sizeof(video), 0, sizeof(video), STREAM_MEDIA_KEY, video};
    media.audio[0] = (struct stream_media_audio){0, sizeof(audio), 960, audio};
    CHECK(stream_media_encode(&media, wire, sizeof(wire), &length) == STREAM_OK);
    CHECK(stream_publish_block(publisher, wire, length) == STREAM_OK);
    struct stream_media_status status;
    uint64_t media_deadline = get_time_tb() + 100000;
    do {
        CHECK(get_time_tb() < media_deadline); test_clock_poll(ua, 100);
        CHECK(stream_media_get_status(second, &status) == STREAM_OK);
    } while (!status.initialized);
    CHECK(status.audio_enabled);
    CHECK(radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey) && !radio_member_muted(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    CHECK(!radio_member_silenced(nodes[1], GROUP2, nodes[0]->my_ed25519_pubkey));
    CHECK(!radio_member_silenced(nodes[1], GROUP1, nodes[2]->my_ed25519_pubkey));
    stream_close(first); first = NULL;
    CHECK(radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    CHECK(radio_set_member_muted(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey, 1) == 0);
    CHECK(stream_publish_set_audio_enabled(publisher, 0) == STREAM_OK); wait_source_audio(epoch, 0);
    media.audio_count = 0; media.video[0].frame_id = 2; media.video[0].pts_us = 40000;
    CHECK(stream_media_encode(&media, wire, sizeof(wire), &length) == STREAM_OK);
    CHECK(stream_publish_block(publisher, wire, length) == STREAM_OK);
    media_deadline = get_time_tb() + 100000;
    do {
        CHECK(get_time_tb() < media_deadline); test_clock_poll(ua, 100);
        CHECK(stream_media_get_status(second, &status) == STREAM_OK);
    } while (status.video_us != 40000);
    CHECK(!status.audio_enabled && status.ready_us == 40000 && status.audio_us == 20000);
    CHECK(!stream_radio_muted(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey) && radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    CHECK(radio_set_member_muted(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey, 0) == 0);
    CHECK(!radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    CHECK(stream_publish_set_audio_enabled(publisher, 1) == STREAM_OK); wait_source_audio(epoch, 1);
    CHECK(stream_media_get_status(second, &status) == STREAM_OK && status.audio_enabled);
    CHECK(radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    stream_close(second); second = NULL;
    CHECK(!radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    CHECK(stream_media_open(nodes[1], GROUP1, source, epoch, STREAM_MEDIA_TRACK_VIDEO, &first) == STREAM_OK);
    CHECK(radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    stream_close(publisher); publisher = NULL;
    CHECK(stream_publish_open(nodes[0], topo_groups_channel_slot(nodes[0]->topo_groups, CHANNEL1), NULL, &publisher) == STREAM_OK);
    CHECK(stream_publish_set_audio_enabled(publisher, 1) == STREAM_OK);
    CHECK(stream_get_info(publisher, &info) == STREAM_OK && info.descriptor.source_epoch != epoch);
    wait_source_audio(info.descriptor.source_epoch, 1);
    CHECK(!stream_handle_active(first) && !radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    stream_close(first); stream_close(raw); stream_close(relay); first = NULL;
    CHECK(stream_media_open(nodes[1], GROUP1, source, info.descriptor.source_epoch, STREAM_MEDIA_TRACK_VIDEO, &first) == STREAM_OK);
    CHECK(radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    stream_cancel_group(nodes[1], GROUP1);
    CHECK(!radio_member_silenced(nodes[1], GROUP1, nodes[0]->my_ed25519_pubkey));
    stream_close(first); stream_close(publisher);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] viewer radio mute: source audio, manual mute, group isolation, owners, relay, epoch, close");
}

/* Пауза player не останавливает live ingest/selector; FIN сохраняет источник до дочитывания локального owner. */
static void test_delayed_media_reader(void) {
    struct STREAM_HANDLE *publisher = NULL, *remote = NULL, *preview = NULL;
    CHECK(stream_publish_open(nodes[0], topo_groups_channel_slot(nodes[0]->topo_groups, CHANNEL1), NULL, &publisher) == STREAM_OK);
    struct stream_info info; CHECK(stream_get_info(publisher, &info) == STREAM_OK);
    wait_state(1, 0, GROUP1, info.descriptor.source_epoch, 0);
    CHECK(stream_media_enable(publisher, 0) == STREAM_OK);
    CHECK(stream_subscribe_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &remote) == STREAM_OK);
    CHECK(stream_subscribe_open(nodes[0], topo_groups_channel_slot(nodes[0]->topo_groups, CHANNEL1), stream_address_read(nodes[0]->my_ed25519_pubkey), &preview) == STREAM_OK);
    CHECK(stream_media_enable(remote, STREAM_MEDIA_TRACK_VIDEO) == STREAM_OK);
    CHECK(stream_media_enable(preview, STREAM_MEDIA_TRACK_VIDEO) == STREAM_OK);
    uint8_t video[128] = {1}, wire[512]; size_t length = 0;
    struct stream_media_block media = {.format = {64, 64, 0, STREAM_MEDIA_H264, 0, 1}, .video_count = 1};
    media.video[0] = (struct stream_media_video){1, 0, sizeof(video), 0, sizeof(video), STREAM_MEDIA_KEY, video};
    CHECK(stream_media_encode(&media, wire, sizeof(wire), &length) == STREAM_OK);
    CHECK(stream_publish_block(publisher, wire, length) == STREAM_OK);
    struct stream_media_packet packet; int result = STREAM_WAIT;
    uint64_t deadline = get_time_tb() + 100000;
    while (result == STREAM_WAIT || result == STREAM_MORE) {
        CHECK(get_time_tb() < deadline); test_clock_poll(ua, 100);
        result = stream_media_read(remote, -1, 1, -1, &packet);
    }
    CHECK(result == STREAM_OK && packet.kind == STREAM_MEDIA_READ_VIDEO && packet.video.pts_us == 0);
    CHECK(stream_media_read(preview, -1, 1, -1, &packet) == STREAM_OK && packet.video.pts_us == 0);
    struct stream_selection_info before, after;
    CHECK(stream_selection_get_info(remote, &before) == STREAM_OK);
    for (unsigned frame = 1; frame <= 150; frame++) {
        media.video[0].frame_id = frame + 1; media.video[0].pts_us = (uint64_t)frame * 40000;
        media.video[0].flags = frame % 25 ? 0 : STREAM_MEDIA_KEY;
        CHECK(stream_media_encode(&media, wire, sizeof(wire), &length) == STREAM_OK);
        CHECK(stream_publish_block(publisher, wire, length) == STREAM_OK);
        test_clock_run_for(ua, 400);
    }
    struct stream_descriptor remote_key = info.descriptor; remote_key.group_id = GROUP1;
    struct stream_local live; CHECK(stream_local_get(nodes[1], &remote_key, &live) == STREAM_OK);
    CHECK(live.next == 151);
    CHECK(stream_selection_get_info(remote, &after) == STREAM_OK && after.restarts == before.restarts);
    struct stream_media_status status;
    CHECK(stream_media_get_status(remote, &status) == STREAM_OK && status.start_us == 0 && status.ready_us == 6000000);
    CHECK(stream_publish_finish(publisher) == STREAM_OK);
    stream_close(publisher); publisher = NULL;
    CHECK(stream_handle_active(preview));
    unsigned counts[2] = {0}; struct STREAM_HANDLE *readers[2] = {remote, preview};
    for (unsigned reader = 0; reader < 2; reader++) {
        deadline = get_time_tb() + 100000;
        for (;;) {
            result = stream_media_read(readers[reader], 10000000, 1, -1, &packet);
            if (result == STREAM_WAIT || result == STREAM_MORE) {
                CHECK(get_time_tb() < deadline); test_clock_poll(ua, 100); continue;
            }
            CHECK(result == STREAM_OK);
            if (packet.kind == STREAM_MEDIA_READ_END) break;
            CHECK(packet.kind == STREAM_MEDIA_READ_VIDEO && packet.video.pts_us == (uint64_t)(counts[reader] + 1) * 40000);
            CHECK(packet.video.length == sizeof(video) && !memcmp(packet.video.data, video, sizeof(video))); counts[reader]++;
        }
        CHECK(counts[reader] == 150); stream_close(readers[reader]);
    }
    test_clock_run_for(ua, 1000);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] delayed media readers: 6s pause, live progress without restart, exact remote/local FIN drain");
}

static struct {
    etcp_recv_fn viewer_receive, source_receive;
    struct ll_entry *held_info;
    struct ETCP_CONN *held_conn;
    unsigned data, ends, renews;
    int hold_source;
} lifecycle;

/* Первый INFO источника задержан до DATA неполного ретранслятора; остальные пакеты не теряются. */
static void lifecycle_view_raw(struct ETCP_CONN *conn, struct ll_entry *entry) {
    if (lifecycle.hold_source && conn->peer_node_id == nodes[0]->node_id
        && entry->len >= STREAM_WIRE_HEADER && entry->dgram[2] == STREAM_CMD_INFO_RESP) {
        CHECK(!lifecycle.held_info);
        lifecycle.held_info = entry; lifecycle.held_conn = conn; return;
    }
    lifecycle.viewer_receive(conn, entry);
}

static void lifecycle_source_raw(struct ETCP_CONN *conn, struct ll_entry *entry) {
    if (conn->peer_node_id == nodes[2]->node_id && entry->len >= STREAM_WIRE_HEADER
        && entry->dgram[2] == STREAM_CMD_RENEW) lifecycle.renews++;
    lifecycle.source_receive(conn, entry);
}

static void lifecycle_event(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)inst; (void)arg;
    if (event->type == STREAM_EVENT_DATA) {
        CHECK(event->first_seq == 0 && event->length == sizeof(expected));
        CHECK(!memcmp(event->data, expected, sizeof(expected))); lifecycle.data++;
    } else if (event->type == STREAM_EVENT_END) lifecycle.ends++;
    else CHECK(event->type == STREAM_EVENT_CLOSED);
}

/* Первый поставщик имеет лишь 3 части пачки; пауза источника не оправдывает зависший bootstrap. */
static void test_partial_bootstrap_and_end(void) {
    /* После проверки forced-active получатель уже в standby; этот сценарий требует обмена каталогом. */
    CHECK(utun_set_client_activity(nodes[2], CLIENT_ACTIVITY_ACTIVE) == 0);
    struct STREAM_HANDLE *publisher = NULL, *relay = NULL, *viewer = NULL;
    CHECK(stream_publish_open(nodes[0], topo_groups_channel_slot(nodes[0]->topo_groups, CHANNEL1), NULL, &publisher) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    struct stream_descriptor remote_key = origin.descriptor; remote_key.group_id = GROUP1;
    wait_state(1, 0, GROUP1, origin.descriptor.source_epoch, 0);
    wait_state(2, 0, GROUP1, origin.descriptor.source_epoch, 0);
    /* Подписанный профиль может прийти через соседа до восстановления маршрута к источнику. */
    uint64_t route_deadline = get_time_tb() + 150000;
    while (!stream_member_online(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey)) && get_time_tb() < route_deadline) test_clock_poll(ua, 100);
    CHECK(stream_member_online(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey)));
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &relay) == STREAM_OK);
    wait_state(2, 1, GROUP1, origin.descriptor.source_epoch, 1);
    memset(expected, 0x37, sizeof(expected));
    CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
    struct stream_local source, received;
    CHECK(stream_local_get(nodes[0], &origin.descriptor, &source) == STREAM_OK);
    for (unsigned sub = 0; sub < 3; sub++) {
        const uint8_t *part; size_t length; uint64_t actual;
        CHECK(stream_buffer_read(source.buffer, 0, sub, &part, &length, &actual) == STREAM_OK);
        CHECK(stream_ingest_block(nodes[1], part, length) == STREAM_OK);
    }
    lifecycle.viewer_receive = nodes[2]->api_bindings.callbacks[ETCP_ID_STREAM];
    lifecycle.source_receive = nodes[0]->api_bindings.callbacks[ETCP_ID_STREAM];
    CHECK(etcp_bind(nodes[2], ETCP_ID_STREAM, lifecycle_view_raw) == 0);
    CHECK(etcp_bind(nodes[0], ETCP_ID_STREAM, lifecycle_source_raw) == 0);
    stream_set_event_callback(nodes[2], lifecycle_event, NULL); lifecycle.hold_source = 1;
    CHECK(stream_subscribe_open(nodes[2], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &viewer) == STREAM_OK);
    uint64_t deadline = get_time_tb() + 40000;
    do {
        test_clock_poll(ua, 100);
        CHECK(stream_local_get(nodes[2], &remote_key, &received) == STREAM_OK);
    } while (!received.anchored && get_time_tb() < deadline);
    CHECK(received.anchored && !received.decoded && lifecycle.held_info);
    struct stream_selection_info selected; CHECK(stream_selection_get_info(viewer, &selected) == STREAM_OK);
    CHECK(selected.phase == STREAM_DELIVERY_BOOTSTRAP);
    lifecycle.hold_source = 0;
    struct ll_entry *held = lifecycle.held_info; lifecycle.held_info = NULL;
    lifecycle.viewer_receive(lifecycle.held_conn, held);
    run_until(&lifecycle.data, 1);
    CHECK(stream_local_get(nodes[2], &remote_key, &received) == STREAM_OK);
    CHECK(received.start == 0 && received.next == 1 && received.decoded);
    test_clock_run_for(ua, 1000); CHECK(stream_selection_get_info(viewer, &selected) == STREAM_OK);
    CHECK(!selected.restarts && selected.providers == 2);
    CHECK(selected.phase == STREAM_DELIVERY_LIVE);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] partial bootstrap: complementary source parts decoded original batch without restart");
    /* END не зависит от существования relay; оставляем только прямую выдачу, чтобы измерять её RENEW. */
    stream_close(viewer); stream_close(relay); viewer = NULL;
    test_clock_run_for(ua, 10000);
    CHECK(stream_subscribe_open(nodes[2], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &viewer) == STREAM_OK);
    run_until(&lifecycle.data, 2); test_clock_run_for(ua, 20000);
    CHECK(stream_publish_finish(publisher) == STREAM_OK); run_until(&lifecycle.ends, 1);
    test_clock_run_for(ua, 2000); lifecycle.renews = 0;
    test_clock_run_for(ua, 100000);
    CHECK(stream_selection_get_info(viewer, &selected) == STREAM_OK);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "finished reader retained: renews=%u flows=%u overlaps=%u probes=%u providers=%u",
               lifecycle.renews, selected.subflows, selected.overlaps, selected.probes_active, selected.providers);
    CHECK(!lifecycle.renews && !selected.subflows && !selected.overlaps && !selected.probes_active && !selected.providers);
    CHECK(selected.phase == STREAM_DELIVERY_FINISHED);
    CHECK(stream_handle_active(viewer));
    CHECK(stream_local_get(nodes[2], &remote_key, &received) == STREAM_OK && received.next == 1 && received.finished);
    struct ETCP_CONN *shared = instance_find_conn(nodes[2], nodes[0]->node_id); CHECK(shared && shared->links_up);
    stream_close(viewer); stream_close(publisher); test_clock_run_for(ua, 1000);
    CHECK(etcp_bind(nodes[2], ETCP_ID_STREAM, lifecycle.viewer_receive) == 0);
    CHECK(etcp_bind(nodes[0], ETCP_ID_STREAM, lifecycle.source_receive) == 0);
    stream_set_event_callback(nodes[2], NULL, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] completed receive releases upstream while retaining reader and shared transport");
}

static struct { unsigned data, gaps, ends; } tail_delivery;
static void tail_event(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)inst; (void)arg;
    if (event->type == STREAM_EVENT_DATA) {
        CHECK(event->first_seq == tail_delivery.data && event->length == 256);
        for (size_t i = 0; i < event->length; i++) CHECK(event->data[i] == 0x40 + event->first_seq);
        tail_delivery.data++;
    } else if (event->type == STREAM_EVENT_GAP) tail_delivery.gaps++;
    else if (event->type == STREAM_EVENT_END) tail_delivery.ends++;
}

/* Подписанный END быстрой части не доказывает отсутствие хвоста другой части у relay. */
static void test_delayed_relay_tail(int missing) {
    struct STREAM_HANDLE *publisher = NULL, *relay = NULL, *viewer = NULL;
    struct stream_parameters params = {.n = 2, .r = 0, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[0], topo_groups_channel_slot(nodes[0]->topo_groups, CHANNEL1), &params, &publisher) == STREAM_OK);
    struct stream_info origin; CHECK(stream_get_info(publisher, &origin) == STREAM_OK);
    struct stream_descriptor remote_key = origin.descriptor; remote_key.group_id = GROUP1;
    wait_state(1, 0, GROUP1, origin.descriptor.source_epoch, 0);
    wait_state(2, 0, GROUP1, origin.descriptor.source_epoch, 0);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &relay) == STREAM_OK);
    CHECK(stream_subscribe_manual_open(nodes[2], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &viewer) == STREAM_OK);
    wait_state(2, 1, GROUP1, origin.descriptor.source_epoch, 1);
    uint8_t payload[256];
    for (unsigned seq = 0; seq < 2; seq++) {
        memset(payload, 0x40 + seq, sizeof(payload)); CHECK(stream_publish_block(publisher, payload, sizeof(payload)) == STREAM_OK);
    }
    CHECK(stream_publish_finish(publisher) == STREAM_OK);
    struct stream_local local; CHECK(stream_local_get(nodes[0], &origin.descriptor, &local) == STREAM_OK);
    const uint8_t *part; size_t length; uint64_t actual;
    for (unsigned seq = 0; seq < 2; seq++) for (unsigned sub = 0; sub < 2; sub++) {
        if (seq == 1 && sub == 1) continue;
        CHECK(stream_buffer_read(local.buffer, seq, sub, &part, &length, &actual) == STREAM_OK);
        CHECK(stream_ingest_block(nodes[1], part, length) == STREAM_OK);
    }
    CHECK(stream_ingest_end(nodes[1], &remote_key, local.end, STREAM_END_SIZE) == STREAM_OK);
    memset(&tail_delivery, 0, sizeof(tail_delivery)); stream_set_event_callback(nodes[2], tail_event, NULL);
    struct STREAM_RECEIVE *receives[2] = {0};
    for (unsigned sub = 0; sub < 2; sub++) CHECK(stream_receive_open(viewer, stream_address_read(nodes[1]->my_ed25519_pubkey), sub, 0, &receives[sub]) == STREAM_OK);
    run_until(&tail_delivery.data, 1); test_clock_run_for(ua, 2000);
    struct stream_receive_info slow; CHECK(stream_receive_get_info(receives[1], &slow) == STREAM_OK);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "relay delayed tail: data=%u gaps=%u ends=%u slow_state=%d next=%llu before_missing_DATA",
        tail_delivery.data, tail_delivery.gaps, tail_delivery.ends, slow.state, (unsigned long long)slow.next);
    CHECK(tail_delivery.data == 1 && !tail_delivery.gaps && !tail_delivery.ends && slow.state == STREAM_RECEIVE_ACTIVE && slow.next == 1);
    if (missing) stream_local_progress(nodes[1], &remote_key, 2); /* Все upstream подтвердили пропуск хвоста. */
    else {
        CHECK(stream_buffer_read(local.buffer, 1, 1, &part, &length, &actual) == STREAM_OK);
        CHECK(stream_ingest_block(nodes[1], part, length) == STREAM_OK);
        run_until(&tail_delivery.data, 2);
    }
    run_until(&tail_delivery.ends, 1); CHECK(tail_delivery.gaps == (unsigned)missing && tail_delivery.data == (missing ? 1u : 2u));
    for (unsigned sub = 0; sub < 2; sub++) stream_receive_close(receives[sub]);
    stream_close(viewer); stream_close(relay); stream_close(publisher); test_clock_run_for(ua, 1000);
    stream_set_event_callback(nodes[2], NULL, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] early signed END waits for relay tail: confirmed_missing=%d data=%u gaps=%u end=%u",
        missing, tail_delivery.data, tail_delivery.gaps, tail_delivery.ends);
}

/* Объявление источника ждёт спящего пира; foreground возобновляет настоящий Merkle-обмен. */
static void test_sources_after_wake(void) {
    debug_level_t level = g_debug_config.category_levels[DEBUG_CATEGORY_KEEPALIVE];
    debug_set_category_level(DEBUG_CATEGORY_KEEPALIVE, DEBUG_LEVEL_INFO);
    /* Предыдущий recovery оставляет центральный mobile-узел в standby; здесь спит только проверяемый получатель. */
    CHECK(utun_set_client_activity(nodes[0], CLIENT_ACTIVITY_ACTIVE) == 0);
    CHECK(utun_set_client_activity(nodes[2], 1) == 0);
    /* Каналы стенда внесены напрямую в БД после init; ждём первого обновления кеша chat_sync (5 с). */
    test_clock_run_for(ua, 51000);
    CHECK(utun_set_client_activity(nodes[2], 0) == 0);
    struct ETCP_CONN *conn = instance_find_conn(nodes[0], nodes[2]->node_id); CHECK(conn && conn->links_up);
    uint64_t deadline = get_time_tb() + 10000;
    while (!conn->peer_sleep_phase && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(conn->peer_sleep_phase);
    /* Linux-стенд без Android duty-cycle: задаём полученный от мобильного пира тайминг 120 с. */
    struct ETCP_LINK *link = conn->links;
    while (link && (!link->initialized || !link->ka_peer_sleeping)) link = link->next;
    CHECK(link);
    struct ETCP_DGRAM *pkt = memory_pool_alloc(nodes[0]->pkt_pool); CHECK(pkt);
    memset(pkt, 0, sizeof(*pkt) + 8); pkt->link = link; pkt->data_len = 8;
    pkt->data[0] = ETCP_KEEPALIVE; pkt->data[1] = KA_FLAG_SLEEP;
    pkt->data[2] = 0x30; pkt->data[3] = 0x75; // period=30000 мс
    pkt->data[6] = 0xb0; pkt->data[7] = 4; // sleep=1200 * 0.1 с
    CHECK(etcp_keepalive_on_recv(pkt) == 1 && conn->peer_phase_timer);
    struct STREAM_HANDLE *publisher = NULL;
    CHECK(stream_publish_open(nodes[1], GROUP1, NULL, &publisher) == STREAM_OK);
    struct stream_info info; CHECK(stream_get_info(publisher, &info) == STREAM_OK);
    test_clock_run_for(ua, 5000);
    struct stream_member_state state;
    CHECK(stream_member_load(nodes[2], GROUP1, stream_address_read(nodes[1]->my_ed25519_pubkey), &state, NULL) == STREAM_OK && !state.published);
    CHECK(conn->peer_sleep_phase && conn->peer_phase_timer);
    uint64_t started = get_time_tb();
    CHECK(utun_set_client_activity(nodes[2], 1) == 0);
    wait_state(2, 1, GROUP1, info.descriptor.source_epoch, 0);
    CHECK(!conn->peer_sleep_phase && !conn->peer_phase_timer && get_time_tb() - started < 30000);
    char *catalog = NULL, id[80];
    CHECK(read_catalogue(nodes[2], GROUP1, &catalog) == STREAM_OK);
    snprintf(id, sizeof(id), "\"id\":\"%s\"", sc_public_key_hex(nodes[1]->my_ed25519_pubkey, (char[65]){0}));
    CHECK(strstr(catalog, id)); u_free(catalog);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] source catalogue after foreground: elapsed_ms=%llu, cancelled sleep=120000ms",
               (unsigned long long)((get_time_tb() - started) / 10));
    stream_close(publisher);
    deadline = get_time_tb() + 150000;
    do {
        test_clock_poll(ua, 100);
        CHECK(stream_member_load(nodes[2], GROUP1, stream_address_read(nodes[1]->my_ed25519_pubkey), &state, NULL) == STREAM_OK);
    } while (state.published && get_time_tb() < deadline);
    CHECK(!state.published);
    debug_set_category_level(DEBUG_CATEGORY_KEEPALIVE, level);
}

static unsigned offline_snapshots;
static int offline_listed;
static unsigned offline_data;
static unsigned offline_unavailable;

/* Удержанный offline-поставщик не должен попадать в фоновые пробы и получать штраф за известное отсутствие BGP. */
static void capture_offline_probe(int level, const char *category, const char *message) {
    (void)level; (void)category;
    const char *line = strstr(message, "stream receive supplier unavailable peer="); char peer[65];
    if (line && sscanf(line, "stream receive supplier unavailable peer=%64s", peer) == 1 && !strcmp(peer, sc_public_key_hex(nodes[0]->my_ed25519_pubkey, (char[65]){0})))
        offline_unavailable++;
}

/* После восстановления выдаём строго следующую пачку; повторы и незаявленные GAP недопустимы. */
static void on_offline_data(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)arg; CHECK(inst == nodes[1]);
    if (event->type == STREAM_EVENT_CLOSED) return;
    CHECK(event->descriptor.group_id == GROUP1 && stream_address_equal(event->descriptor.source, stream_address_read(nodes[0]->my_ed25519_pubkey)));
    CHECK(event->type == STREAM_EVENT_DATA && event->first_seq == offline_data && event->last_seq == offline_data);
    CHECK(event->length == sizeof(expected) && !memcmp(event->data, expected, sizeof(expected))); offline_data++;
}

/* Ожидание сохраняет сессию, но не может удерживать ни flow, ни постоянный резерв нужной эпохи. */
static void offline_check_released(const struct stream_descriptor *key) {
    struct stream_network *net = stream_network_get(nodes[1]);
    for (struct stream_receive_slot *slot = net->slots; slot; slot = slot->next) CHECK(!same_stream(&slot->descriptor, key));
    for (struct stream_peer *p = net->peers; p; p = p->next)
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
            CHECK(!same_stream(&f->descriptor, key) || f->info.state == STREAM_RECEIVE_CLOSED);
}

static void offline_catalog_wait(unsigned previous) {
    uint64_t deadline = get_time_tb() + 10000;
    while (offline_snapshots <= previous && get_time_tb() < deadline) test_clock_poll(ua, 10);
    CHECK(offline_snapshots > previous);
}

/* Проверяем доставку каталога по BGP без нового подписанного статуса публикации. */
static void offline_catalog_event(struct UTUN_INSTANCE *inst, int type, const uint8_t *data, int length) {
    if (type != CHAT_EVT_STREAM_SOURCES) return;
    CHECK(inst == nodes[1] && length >= 33 && !memcmp(data, topo_group_wire_id(topo_groups_find(inst->topo_groups, GROUP1)), 32) && data[32]);
    char json[2048], id[80]; CHECK(length - 33 < (int)sizeof(json));
    memcpy(json, data + 33, (size_t)length - 33); json[length - 33] = 0;
    snprintf(id, sizeof(id), "\"id\":\"%s\"", sc_public_key_hex(nodes[0]->my_ed25519_pubkey, (char[65]){0}));
    offline_listed = strstr(json, id) != NULL; offline_snapshots++;
}

/* Управляющие records не требуют demand; CLOSED должен вытеснить старые данные и сохранить причину. */
static void offline_viewer_record(struct stream_viewers *viewers, uint64_t id, int kind, int reason) {
    struct stream_viewer_record *record = NULL;
    uint64_t deadline = get_time_tb() + 10000;
    while (!(record = stream_viewer_pull(viewers, id)) && get_time_tb() < deadline) test_clock_poll(ua, 1);
    CHECK(record && record->kind == kind && !record->next);
    if (kind == STREAM_VIEWER_CLOSED) CHECK(record->reason == reason);
    stream_viewer_records_free(record);
}

/* k ограничен постоянными слотами, n по-прежнему допускает255. Отказ не меняет подписанный профиль или старый handle. */
static void test_capacity_limits(void) {
    int activity = nodes[0]->client_activity_requested;
    CHECK(utun_set_client_activity(nodes[0], CLIENT_ACTIVITY_ACTIVE) == 0);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "capacity fixture: source stays active while announcing a synthetic unsupported profile");
    const struct stream_parameters parameters[] = {
        {192, 0, STREAM_RS_DEFAULT_SIZE}, {193, 0, STREAM_RS_DEFAULT_SIZE},
        {255, 63, STREAM_RS_DEFAULT_SIZE}, {255, 62, STREAM_RS_DEFAULT_SIZE}
    };
    struct STREAM_HANDLE *publisher = NULL, *refused = NULL;
    for (unsigned i = 0; i < sizeof(parameters) / sizeof(parameters[0]); i++) {
        char before[MS_USERINFO_MAX + 1], after[MS_USERINFO_MAX + 1];
        uint8_t sig_before[64], sig_after[64]; uint64_t version_before, version_after;
        if (!(i & 1)) {
            CHECK(stream_publish_open(nodes[0], GROUP1, &parameters[i], &publisher) == STREAM_OK);
            struct stream_info info; CHECK(stream_get_info(publisher, &info) == STREAM_OK);
            CHECK(info.descriptor.n == parameters[i].n && info.descriptor.r == parameters[i].r);
            continue;
        }
        profile(0, GROUP1, before, sig_before, &version_before);
        size_t allocations = u_get_allocated_count();
        capacity_allocations = 0; capacity_alloc_watch = 1;
        CHECK(stream_publish_open(nodes[0], GROUP1, &parameters[i], &refused) == STREAM_ERR_CAPACITY && !refused);
        capacity_alloc_watch = 0;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "capacity publish rejection: caller_allocations=%u global_blocks=%zu->%zu",
            capacity_allocations, allocations, u_get_allocated_count());
        CHECK(!capacity_allocations && stream_handle_active(publisher));
        profile(0, GROUP1, after, sig_after, &version_after);
        CHECK(!strcmp(before, after) && version_before == version_after && !memcmp(sig_before, sig_after, sizeof(sig_before)));
        stream_close(publisher); publisher = NULL;
        struct stream_member_state state = {.published = 1, .count = 1,
            .source = {.group_id = GROUP1, .source = stream_address_read(nodes[0]->my_ed25519_pubkey), .source_epoch = 9000 + i,
                .n = parameters[i].n, .r = parameters[i].r, .block_size = STREAM_RS_DEFAULT_SIZE},
            .subscriptions = {{stream_address_read(nodes[0]->my_ed25519_pubkey), 9000 + i}}};
        char source[MS_USERINFO_MAX + 1], subscriptions[MS_USERINFO_MAX + 1];
        CHECK(stream_member_format(&state, source, subscriptions) == STREAM_OK);
        struct chat_profile_option options[] = {{"stream_source", source}, {"stream_subscriptions", subscriptions}};
        CHECK(chat_core_set_my_profile_options(nodes[0], CHANNEL1, options, 2) == 0);
        wait_state(1, 0, GROUP1, state.source.source_epoch, 0);
        allocations = u_get_allocated_count();
        capacity_allocations = 0; capacity_alloc_watch = 1;
        CHECK(stream_subscribe_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &refused) == STREAM_ERR_CAPACITY && !refused);
        CHECK(stream_relay_open(nodes[1], &state.source, &refused) == STREAM_ERR_CAPACITY && !refused);
        capacity_alloc_watch = 0;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "capacity subscribe/relay rejection: caller_allocations=%u global_blocks=%zu->%zu",
            capacity_allocations, allocations, u_get_allocated_count());
        CHECK(!capacity_allocations);
        struct stream_viewers *viewers = NULL; CHECK(stream_viewers_create(&viewers, nodes[1]) == 0);
        CHECK(stream_viewer_open(viewers, 991, stream_address_read(topo_group_wire_id(topo_groups_find(nodes[1]->topo_groups, GROUP1))), stream_address_read(nodes[0]->my_ed25519_pubkey), state.source.source_epoch, 3) == 0);
        offline_viewer_record(viewers, 991, STREAM_VIEWER_CLOSED, STREAM_CLOSE_CAPACITY);
        stream_viewers_destroy(viewers);
        /* Низкоуровневый ручной приём одной части не требует восстановления всех k частей. */
        CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &refused) == STREAM_OK);
        stream_close(refused); refused = NULL;
        options[0].value = ""; options[1].value = "";
        CHECK(chat_core_set_my_profile_options(nodes[0], CHANNEL1, options, 2) == 0);
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] capacity k192 accepted n192/255; k193 rejected before allocation/profile; viewer CLOSED_CAPACITY");
    CHECK(utun_set_client_activity(nodes[0], activity) == 0);
}

/* Первый зритель получает все k частей от одного автора, включая границы прежних peer64/regular192. */
static void test_single_supplier_budget(void) {
    const unsigned required[] = {80, 192, 255, 255};
    const char *supplier_window[] = {"128", "8", "128", "4096"};
    const char *viewer_window[] = {"128", "128", "8", "4096"};
    for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        const char *limit = required[i] > 192 ? "256" : "192";
        CHECK(chat_setting_set(nodes[0], "stream_regular_limit", limit) == 0);
        CHECK(chat_setting_set(nodes[1], "stream_regular_limit", limit) == 0);
        CHECK(chat_setting_set(nodes[0], "stream_control_limit", supplier_window[i]) == 0);
        CHECK(chat_setting_set(nodes[1], "stream_control_limit", viewer_window[i]) == 0);
        struct STREAM_HANDLE *publisher = NULL, *viewer = NULL;
        struct stream_parameters parameters = {required[i], 0, STREAM_RS_DEFAULT_SIZE};
        CHECK(stream_publish_open(nodes[0], GROUP1, &parameters, &publisher) == STREAM_OK);
        struct stream_info info; CHECK(stream_get_info(publisher, &info) == STREAM_OK);
        wait_state(1, 0, GROUP1, info.descriptor.source_epoch, 0);
        stream_set_event_callback(nodes[1], on_offline_data, NULL); offline_data = 0;
        CHECK(stream_subscribe_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &viewer) == STREAM_OK);
        struct stream_network *net = stream_network_get(nodes[1]); unsigned previous = net->regular_used;
        if (!i) net->regular_used = 113; // 79 свободных недостаточно для атомарного k80.
        CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
        if (!i) {
            test_clock_run_for(ua, 3000); CHECK(!offline_data && !net->reservations && net->regular_used == 113);
            net->regular_used = previous;
        }
        run_until(&offline_data, 1);
        CHECK(net->reservations && net->reservations->minimum == required[i]);
        struct stream_peer *peer = peer_find(net, stream_address_read(nodes[0]->my_ed25519_pubkey)); CHECK(peer);
        unsigned received = 0;
        for (struct STREAM_RECEIVE *f = peer->flows; f; f = f->next)
            if (f->outgoing && !f->info.probe_only && f->info.state == STREAM_RECEIVE_ACTIVE && f->info.blocks) received++;
        CHECK(received == required[i]);
        if (!i) {
            CHECK(chat_setting_set(nodes[1], "stream_regular_limit", "79") == 0);
            struct STREAM_HANDLE *second = NULL;
            CHECK(stream_subscribe_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &second) == STREAM_OK);
            CHECK(net->regular_used == 80 && stream_handle_active(viewer)); stream_close(second);
            CHECK(chat_setting_set(nodes[1], "stream_regular_limit", limit) == 0);
        }
        CHECK(peer->reply_count <= peer->local_window);
        struct stream_peer *sending = peer_find(stream_network_get(nodes[0]), stream_address_read(nodes[1]->my_ed25519_pubkey)); CHECK(sending);
        CHECK(sending->reply_count <= sending->local_window);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] single supplier decodes k=%u regular=%s DATA=%u reserve=%u control=%s/%s",
            required[i], limit, received, net->regular_used, supplier_window[i], viewer_window[i]);
        stream_close(viewer); stream_close(publisher); stream_set_event_callback(nodes[1], NULL, NULL);
        CHECK(!net->reservations && net->regular_used == previous);
        uint64_t closed_before = get_time_tb() + 50000;
        while ((peer->flows || peer->requests || peer->reply_count || sending->flows || sending->requests || sending->reply_count)
            && get_time_tb() < closed_before) test_clock_poll(ua, 100);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "close drain k=%u viewer=%d/%u/%u supplier=%d/%u/%u",
            required[i], peer->flows != NULL, peer->request_count, peer->reply_count,
            sending->flows != NULL, sending->request_count, sending->reply_count);
        CHECK(!peer->flows && !peer->requests && !peer->reply_count);
        CHECK(!sending->flows && !sending->requests && !sending->reply_count);
    }
    CHECK(chat_setting_set(nodes[0], "stream_regular_limit", "192") == 0);
    CHECK(chat_setting_set(nodes[1], "stream_regular_limit", "192") == 0);
    CHECK(chat_setting_set(nodes[0], "stream_control_limit", "128") == 0);
    CHECK(chat_setting_set(nodes[1], "stream_control_limit", "128") == 0);
}

static unsigned window_data[2];

/* Два одновременных автора проверяют обе стороны общей peer-сессии; GAP и повтор DATA недопустимы. */
static void on_window_data(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)arg; unsigned node = inst == nodes[0] ? 0 : 1; CHECK(inst == nodes[node]);
    if (event->type == STREAM_EVENT_CLOSED || stream_address_equal(event->descriptor.source, stream_address_read(inst->my_ed25519_pubkey))) return;
    CHECK(stream_address_equal(event->descriptor.source, stream_address_read(nodes[1 - node]->my_ed25519_pubkey)) && event->type == STREAM_EVENT_DATA);
    CHECK(event->first_seq == window_data[node] && event->last_seq == window_data[node]);
    CHECK(event->length == sizeof(expected) && !memcmp(event->data, expected, sizeof(expected))); window_data[node]++;
}

/* Полный транспорт в обе стороны, живые DATA и смена окна8/128→4096/8; запущенные INFO завершаются без потерь. */
static void test_control_windows(void) {
    struct STREAM_HANDLE *publishers[2] = {0}, *viewers[2] = {0};
    struct STREAM_RECEIVE *receives[2][255] = {{0}};
    struct stream_info sources[2]; struct stream_parameters parameters = {255, 0, STREAM_RS_DEFAULT_SIZE};
    memset(window_data, 0, sizeof(window_data));
    for (unsigned i = 0; i < 2; i++) {
        CHECK(chat_setting_set(nodes[i], "stream_regular_limit", "512") == 0);
        CHECK(chat_setting_set(nodes[i], "stream_control_limit", i ? "128" : "8") == 0);
        CHECK(stream_publish_open(nodes[i], GROUP1, &parameters, &publishers[i]) == STREAM_OK);
        CHECK(stream_get_info(publishers[i], &sources[i]) == STREAM_OK);
        wait_state(1 - i, i, GROUP1, sources[i].descriptor.source_epoch, 0);
    }
    for (unsigned i = 0; i < 2; i++) {
        stream_set_event_callback(nodes[i], on_window_data, NULL);
        CHECK(stream_subscribe_manual_open(nodes[i], GROUP1, stream_address_read(nodes[1 - i]->my_ed25519_pubkey), &viewers[i]) == STREAM_OK);
        for (unsigned sub = 0; sub < 255; sub++)
            CHECK(stream_receive_open(viewers[i], stream_address_read(nodes[1 - i]->my_ed25519_pubkey), sub, 0, &receives[i][sub]) == STREAM_OK);
        CHECK(stream_publish_block(publishers[i], expected, sizeof(expected)) == STREAM_OK);
    }
    for (unsigned i = 0; i < 2; i++) run_until(&window_data[i], 1);
    struct stream_peer *peers[2]; struct ll_queue *queues[2]; queue_callback_fn callbacks[2]; void *args[2];
    for (unsigned i = 0; i < 2; i++) {
        peers[i] = peer_find(stream_network_get(nodes[i]), stream_address_read(nodes[1 - i]->my_ed25519_pubkey)); CHECK(peers[i]);
        struct ETCP_CONN *conn = instance_find_conn(nodes[i], nodes[1 - i]->node_id); CHECK(conn && conn->initialized);
        queues[i] = conn->send_input_q;
        uint64_t deadline = get_time_tb() + 50000;
        while (queues[i]->count && get_time_tb() < deadline) test_clock_poll(ua, 100);
        CHECK(!queues[i]->count);
        callbacks[i] = queues[i]->callback; args[i] = queues[i]->callback_arg; CHECK(callbacks[i]);
        queue_set_callback(queues[i], NULL, NULL);
    }
    struct STREAM_RECEIVE *probes[2][16] = {{0}};
    for (unsigned i = 0; i < 2; i++) for (unsigned sub = 0; sub < 16; sub++)
        CHECK(stream_probe_open(viewers[i], stream_address_read(nodes[1 - i]->my_ed25519_pubkey), sub, 0, &probes[i][sub]) == STREAM_OK);
    test_clock_run_for(ua, 500);
    CHECK(queues[0]->count && queues[1]->count);
    CHECK(chat_setting_set(nodes[0], "stream_control_limit", "4096") == 0);
    CHECK(chat_setting_set(nodes[1], "stream_control_limit", "8") == 0);
    test_clock_run_for(ua, 500);
    for (unsigned i = 0; i < 2; i++) {
        queue_set_callback(queues[i], callbacks[i], args[i]); queue_resume_callback(queues[i]);
    }
    for (unsigned i = 0; i < 2; i++) for (unsigned sub = 0; sub < 16; sub++) {
        wait_probe_info(probes[i][sub]); stream_receive_close(probes[i][sub]);
    }
    uint64_t deadline = get_time_tb() + 50000;
    while ((peers[0]->window_current != 4096 || peers[1]->window_current != 8
        || peers[0]->window_pending || peers[1]->window_pending) && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(peers[0]->window_current == 4096 && peers[0]->remote_window == 8 && !peers[0]->window_pending);
    CHECK(peers[1]->window_current == 8 && peers[1]->remote_window == 4096 && !peers[1]->window_pending);
    for (unsigned i = 0; i < 2; i++) {
        CHECK(peers[i]->reply_count <= peers[i]->local_window && !peers[i]->request_count);
        CHECK(stream_publish_block(publishers[i], expected, sizeof(expected)) == STREAM_OK);
    }
    for (unsigned i = 0; i < 2; i++) run_until(&window_data[i], 2);
    test_clock_run_for(ua, 25000); // Продления работают после изменения окна и завершения проб.
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned sub = 0; sub < 255; sub++) stream_receive_close(receives[i][sub]);
        CHECK(stream_handle_active(viewers[i])); stream_close(viewers[i]); stream_close(publishers[i]);
        stream_set_event_callback(nodes[i], NULL, NULL);
    }
    deadline = get_time_tb() + 50000;
    while ((peers[0]->flows || peers[0]->requests || peers[0]->reply_count || peers[1]->flows || peers[1]->requests || peers[1]->reply_count)
        && get_time_tb() < deadline) test_clock_poll(ua, 100);
    for (unsigned i = 0; i < 2; i++) {
        CHECK(!peers[i]->flows && !peers[i]->requests && !peers[i]->reply_count);
        CHECK(!peers[i]->network->regular_used && !peers[i]->network->temporary_used);
        CHECK(chat_setting_set(nodes[i], "stream_control_limit", "128") == 0);
        CHECK(chat_setting_set(nodes[i], "stream_regular_limit", "192") == 0);
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] control windows: duplex k255, blocked transport both ways, hot8/128->4096/8,32 INFO,renew,close");
}

/* Настоящие BGP WITHDRAW/NODEINFO: запасной путь, отложенная сверка, offline и восстановление той же эпохи. */
static void test_source_offline(void) {
    struct TOPO_GROUP *group = topo_groups_find(nodes[1]->topo_groups, GROUP1);
    struct TOPO_GROUP *origin = topo_groups_find(nodes[0]->topo_groups, GROUP1);
    struct TOPO_PEER_REQUEST *alternate = NULL, *other = NULL;
    CHECK(topo_group_peer_open(group, nodes[2]->node_id, &alternate) == 0);
    CHECK(topo_group_peer_open(topo_groups_find(nodes[1]->topo_groups, GROUP2), nodes[0]->node_id, &other) == 0);
    uint64_t deadline = get_time_tb() + 150000;
    struct TOPO_GROUP_NODE *node = NULL;
    do {
        test_clock_poll(ua, 10); node = topo_node_find_by_id(group, nodes[0]->node_id);
    } while ((topo_group_peer_phase(alternate) != TOPO_PEER_READY || topo_group_peer_phase(other) != TOPO_PEER_READY
              || !node || !node->paths || queue_entry_count(node->paths) < 2) && get_time_tb() < deadline);
    CHECK(topo_group_peer_phase(alternate) == TOPO_PEER_READY && topo_group_peer_phase(other) == TOPO_PEER_READY);
    CHECK(node && node->paths && queue_entry_count(node->paths) == 2);
    struct ETCP_CONN *direct = instance_find_conn(nodes[1], nodes[0]->node_id);
    struct ETCP_CONN *indirect = instance_find_conn(nodes[1], nodes[2]->node_id); CHECK(direct && indirect);
    uint8_t announce[sizeof(struct TOPOMSG_HEADER) + TOPO_NODE_WIRE_MAX_SIZE] = {0};
    memcpy(((struct TOPOMSG_HEADER*)announce)->group_key, topo_group_wire_id(origin), 32);
    int bytes = topo_node_serialize(topo_node_registry_find(nodes[0]->topo_groups, nodes[0]->node_id), origin->local_node, 0, announce + sizeof(struct TOPOMSG_HEADER), sizeof(announce) - sizeof(struct TOPOMSG_HEADER), 0);
    CHECK(bytes > 0); size_t announce_size = sizeof(struct TOPOMSG_HEADER) + (size_t)bytes;
    struct TOPOMSG_WITHDRAW_PKT withdraw = {0};
    memcpy(withdraw.ed25519, nodes[0]->my_ed25519_pubkey, sizeof(withdraw.ed25519));
    struct STREAM_HANDLE *publisher = NULL, *second = NULL, *reader = NULL, *relay = NULL, *other_reader = NULL, *refused = NULL;
    struct stream_parameters parameters = {.n = 1, .r = 0, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[0], GROUP1, &parameters, &publisher) == STREAM_OK);
    CHECK(stream_publish_open(nodes[0], GROUP2, NULL, &second) == STREAM_OK);
    struct stream_info info, second_info; CHECK(stream_get_info(publisher, &info) == STREAM_OK);
    CHECK(stream_get_info(second, &second_info) == STREAM_OK);
    wait_state(1, 0, GROUP1, info.descriptor.source_epoch, 0); wait_state(1, 0, GROUP2, second_info.descriptor.source_epoch, 0);
    CHECK(stream_subscribe_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &reader) == STREAM_OK);
    CHECK(stream_relay_open(nodes[1], &info.descriptor, &relay) == STREAM_OK);
    CHECK(stream_subscribe_open(nodes[1], GROUP2, stream_address_read(nodes[0]->my_ed25519_pubkey), &other_reader) == STREAM_OK);
    struct stream_viewers *viewers = NULL; CHECK(stream_viewers_create(&viewers, nodes[1]) == 0);
    CHECK(stream_viewer_open(viewers, 901, stream_address_read(topo_group_wire_id(topo_groups_find(nodes[1]->topo_groups, GROUP1))), stream_address_read(nodes[0]->my_ed25519_pubkey), info.descriptor.source_epoch, 3) == 0);
    offline_viewer_record(viewers, 901, STREAM_VIEWER_OPENED, 0);
    chat_event_set_handler(nodes[1], offline_catalog_event); offline_snapshots = 0;
    CHECK(stream_watch_sources(nodes[1], GROUP1, 901) == STREAM_OK);
    offline_catalog_wait(0); CHECK(offline_listed);
    CHECK(topo_group_process_withdraw(group, direct, (const uint8_t *)&withdraw, sizeof(withdraw)) == 0);
    test_clock_poll(ua, 0); CHECK(stream_member_online(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey)));
    CHECK(stream_handle_active(reader) && stream_handle_active(relay) && offline_listed);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] source remains active via alternate BGP path after direct WITHDRAW");
    CHECK(topo_group_process_withdraw(group, indirect, (const uint8_t *)&withdraw, sizeof(withdraw)) == 0);
    CHECK(topo_group_process_nodeinfo(group, direct, announce, announce_size) == 0);
    test_clock_poll(ua, 0); CHECK(stream_handle_active(reader) && stream_handle_active(relay) && offline_listed);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] BGP path restored before deferred reconciliation preserves stream owners");
    unsigned snapshots = offline_snapshots;
    CHECK(topo_group_process_withdraw(group, direct, (const uint8_t *)&withdraw, sizeof(withdraw)) == 0);
    CHECK(!stream_member_online(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey)));
    CHECK(stream_subscribe_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &refused) == STREAM_WAIT && !refused);
    CHECK(stream_relay_open(nodes[1], &info.descriptor, &refused) == STREAM_WAIT && !refused);
    CHECK(stream_media_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), info.descriptor.source_epoch, 3, &refused) == STREAM_WAIT && !refused);
    test_clock_run_for_local_io(ua, 290000);
    struct stream_local local; CHECK(stream_local_get(nodes[1], &info.descriptor, &local) == STREAM_OK && !local.waiting);
    CHECK(stream_handle_active(reader) && stream_handle_active(relay));
    CHECK(!stream_viewer_pull(viewers, 901));
    test_clock_run_for(ua, 20000);
    CHECK(stream_local_get(nodes[1], &info.descriptor, &local) == STREAM_OK && local.waiting);
    offline_check_released(&info.descriptor);
    CHECK(stream_handle_active(reader) && stream_handle_active(relay) && !stream_viewer_pull(viewers, 901));
    offline_catalog_wait(snapshots);
    CHECK(!offline_listed && stream_handle_active(other_reader) && stream_handle_active(publisher));
    struct stream_member_state state;
    CHECK(stream_member_load(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &state, NULL) == STREAM_OK && state.published);
    CHECK(state.source.source_epoch == info.descriptor.source_epoch);
    CHECK(stream_viewer_open(viewers, 902, stream_address_read(topo_group_wire_id(topo_groups_find(nodes[1]->topo_groups, GROUP1))), stream_address_read(nodes[0]->my_ed25519_pubkey), info.descriptor.source_epoch, 3) == 0);
    offline_viewer_record(viewers, 902, STREAM_VIEWER_CLOSED, STREAM_CLOSE_OFFLINE);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] no BGP/DATA: resources retained at29s, released at31s, viewers/owners/buffer and other group preserved");
    snapshots = offline_snapshots;
    CHECK(topo_group_process_nodeinfo(group, direct, announce, announce_size) == 0);
    offline_catalog_wait(snapshots); CHECK(offline_listed);
    CHECK(stream_local_get(nodes[1], &info.descriptor, &local) == STREAM_OK && !local.waiting);
    CHECK(stream_handle_active(reader) && stream_handle_active(relay) && !stream_viewer_pull(viewers, 901));
    CHECK(stream_viewer_open(viewers, 903, stream_address_read(topo_group_wire_id(topo_groups_find(nodes[1]->topo_groups, GROUP1))), stream_address_read(nodes[0]->my_ed25519_pubkey), info.descriptor.source_epoch, 3) == 0);
    offline_viewer_record(viewers, 903, STREAM_VIEWER_OPENED, 0);
    CHECK(stream_member_load(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &state, NULL) == STREAM_OK && state.published);
    CHECK(state.source.source_epoch == info.descriptor.source_epoch);
    CHECK(stream_watch_sources(nodes[1], 0, 901) == STREAM_OK); chat_event_set_handler(nodes[1], NULL);
    stream_viewers_destroy(viewers); stream_close(reader); reader = NULL; stream_close(relay); relay = NULL;
    /* Медиа-INFO создал индекс и у автора; новый raw-эфир проверяет byte-exact DATA на том же живом транспорте. */
    stream_close(publisher); publisher = NULL;
    CHECK(stream_publish_open(nodes[0], GROUP1, &parameters, &publisher) == STREAM_OK);
    CHECK(stream_get_info(publisher, &info) == STREAM_OK);
    wait_state(1, 0, GROUP1, info.descriptor.source_epoch, 0);
    stream_set_event_callback(nodes[1], on_offline_data, NULL); offline_data = 0;
    CHECK(stream_subscribe_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &reader) == STREAM_OK);
    CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK); run_until(&offline_data, 1);
    struct stream_peer *peer = peer_find(stream_network_get(nodes[1]), stream_address_read(nodes[0]->my_ed25519_pubkey)); CHECK(peer);
    struct STREAM_RECEIVE *original = NULL;
    for (struct STREAM_RECEIVE *f = peer->flows; f; f = f->next)
        if (f->outgoing && !f->info.probe_only && same_stream(&f->descriptor, &info.descriptor)) original = f;
    CHECK(original && original->info.state == STREAM_RECEIVE_ACTIVE);
    uint64_t original_handle = original->info.handle;
    offline_unavailable = 0; debug_set_log_hook(capture_offline_probe);
    CHECK(topo_group_process_withdraw(group, direct, (const uint8_t *)&withdraw, sizeof(withdraw)) == 0);
    CHECK(!stream_member_online(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey)));
    for (unsigned seq = 1; seq <= 155; seq++) {
        CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
        test_clock_run_for(ua, 2000); CHECK(offline_data == seq + 1);
        CHECK(stream_local_get(nodes[1], &info.descriptor, &local) == STREAM_OK && !local.waiting);
        CHECK(!stream_member_online(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey)));
        peer = peer_find(stream_network_get(nodes[1]), stream_address_read(nodes[0]->my_ed25519_pubkey)); CHECK(peer);
        original = NULL;
        for (struct STREAM_RECEIVE *f = peer->flows; f; f = f->next)
            if (f->outgoing && !f->info.probe_only && same_stream(&f->descriptor, &info.descriptor)) original = f;
        CHECK(original && original->info.state == STREAM_RECEIVE_ACTIVE && original->info.handle == original_handle);
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] direct DATA survives31s without BGP on original issuance, ordered_batches=%u", offline_data);
    test_clock_run_for_local_io(ua, 290000);
    CHECK(stream_local_get(nodes[1], &info.descriptor, &local) == STREAM_OK && !local.waiting);
    const uint8_t *duplicate; size_t duplicate_size; uint64_t actual;
    CHECK(stream_buffer_read(local.buffer, 155, 0, &duplicate, &duplicate_size, &actual) == STREAM_OK);
    CHECK(stream_ingest_block(nodes[1], duplicate, duplicate_size) == STREAM_DUPLICATE);
    test_clock_run_for(ua, 20000);
    CHECK(stream_local_get(nodes[1], &info.descriptor, &local) == STREAM_OK && local.waiting && local.next == 156);
    offline_check_released(&info.descriptor); CHECK(stream_handle_active(reader));
    struct stream_selection_info phase; CHECK(stream_selection_get_info(reader, &phase) == STREAM_OK);
    CHECK(phase.phase == STREAM_DELIVERY_WAITING);
    debug_set_log_hook(NULL); CHECK(!offline_unavailable);
    CHECK(stream_publish_block(publisher, expected, sizeof(expected)) == STREAM_OK);
    CHECK(topo_group_process_nodeinfo(group, direct, announce, announce_size) == 0);
    run_until(&offline_data, 157);
    CHECK(stream_local_get(nodes[1], &info.descriptor, &local) == STREAM_OK && !local.waiting && local.next == 157);
    test_clock_run_for(ua, 1000); CHECK(stream_selection_get_info(reader, &phase) == STREAM_OK);
    CHECK(phase.phase == STREAM_DELIVERY_LIVE);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] duplicate does not extend grace; same reader resumes automatically from156 without replay");
    CHECK(topo_group_process_withdraw(group, direct, (const uint8_t *)&withdraw, sizeof(withdraw)) == 0);
    test_clock_run_for_local_io(ua, 310000);
    CHECK(stream_local_get(nodes[1], &info.descriptor, &local) == STREAM_OK && local.waiting);
    uint64_t previous_epoch = info.descriptor.source_epoch;
    stream_close(publisher); publisher = NULL;
    CHECK(stream_publish_open(nodes[0], GROUP1, &parameters, &publisher) == STREAM_OK);
    CHECK(stream_get_info(publisher, &info) == STREAM_OK && info.descriptor.source_epoch != previous_epoch);
    wait_state(1, 0, GROUP1, info.descriptor.source_epoch, 0);
    CHECK(!stream_handle_active(reader) && stream_handle_reason(reader) == STREAM_CLOSE_SOURCE_CHANGED);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] signed new source epoch closes waiting old reader without BGP recovery");
    stream_close(reader); reader = NULL;
    CHECK(topo_group_process_nodeinfo(group, direct, announce, announce_size) == 0);
    CHECK(stream_subscribe_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &reader) == STREAM_OK);
    CHECK(topo_group_process_withdraw(group, direct, (const uint8_t *)&withdraw, sizeof(withdraw)) == 0);
    test_clock_run_for_local_io(ua, 310000);
    CHECK(stream_local_get(nodes[1], &info.descriptor, &local) == STREAM_OK && local.waiting);
    CHECK(member_owner_update(1, 0, 2000, 1, 0) >= 0);
    CHECK(!stream_handle_active(reader) && stream_handle_reason(reader) == STREAM_CLOSE_MEMBER);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] signed membership revocation immediately closes waiting reader without route");
    CHECK(topo_group_process_nodeinfo(group, direct, announce, announce_size) == 0);
    stream_close(reader); stream_close(other_reader); stream_set_event_callback(nodes[1], NULL, NULL);
    stream_close(publisher); stream_close(second); topo_group_peer_close(alternate); topo_group_peer_close(other);
    for (unsigned i = 0; i < 4; i++) CHECK(member_owner_update(i, 0, 2001, 0, 0) >= 0);
    test_clock_poll(ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] source reappears and opens after BGP recovery with unchanged signed publication/epoch");
}

#include "test_stream_failover.h"
#include "test_stream_overload.h"
#include "test_stream_paths.h"
#include "test_stream_live.h"

static void test_statistics(void) {
    struct UASYNC *ua = nodes[0]->ua;
    struct STREAM_HANDLE *publisher = NULL, *viewer = NULL, *second = NULL;
    struct stream_parameters parameters = {.n=1, .r=0, .block_size=STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[0], GROUP1, &parameters, &publisher) == STREAM_OK);
    struct stream_info info; CHECK(stream_get_info(publisher, &info) == STREAM_OK);
    CHECK(stream_publish_set_audio_enabled(publisher, 1) == STREAM_OK);
    wait_source_audio(info.descriptor.source_epoch, 1);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &viewer) == STREAM_OK);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &second) == STREAM_OK);
    CHECK(stream_media_enable(second, STREAM_MEDIA_TRACK_VIDEO | STREAM_MEDIA_TRACK_AUDIO) == STREAM_OK);
    struct stream_audio_meter *meter = stream_media_audio_meter(second); CHECK(meter);
    struct STREAM_RECEIVE *receive = NULL;
    CHECK(stream_receive_open(viewer, stream_address_read(nodes[0]->my_ed25519_pubkey), 0, UINT64_MAX, &receive) == STREAM_OK);
    test_clock_run_for(ua, 50000);
    uint8_t video[8] = {0}, audio[1] = {1}, wire_media[512]; size_t length = 0;
    struct stream_media_block media = {.format = {64, 64, 0, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, 1},
        .video_count = 1, .audio_count = 1};
    media.video[0] = (struct stream_media_video){1, 0, sizeof(video), 0, sizeof(video), STREAM_MEDIA_KEY, video};
    media.audio[0] = (struct stream_media_audio){0, sizeof(audio), 960, audio};
    CHECK(stream_media_encode(&media, wire_media, sizeof(wire_media), &length) == STREAM_OK);
    CHECK(stream_publish_block(publisher, wire_media, length) == STREAM_OK);
    struct stream_media_status status;
    uint64_t deadline = get_time_tb() + 100000;
    do {
        CHECK(get_time_tb() < deadline); test_clock_poll(ua, 100);
        CHECK(stream_media_get_status(second, &status) == STREAM_OK);
    } while (!status.initialized);
    CHECK(status.audio_enabled && status.audio_us == 20000);
    /* Перерыв в DATA не создаёт глитч. Затем consumer передаёт ровно 10 мс тишины. */
    stream_audio_meter_output(meter, status.audio_us, 960, 0, 0);
    test_clock_run_for(ua, 40000);
    stream_audio_meter_output(meter, status.audio_us, 0, 480, 1);
    test_clock_run_for(ua, 40000);
    stream_audio_meter_release(meter);
    struct stream_stat_node *history = NULL; size_t count = 0;
    CHECK(stream_stats_read(nodes[0], GROUP1, info.descriptor.source_epoch, &history, &count) == STREAM_OK);
    CHECK(count == 1 && stream_address_equal(history[0].node, stream_address_read(nodes[1]->my_ed25519_pubkey)) && history[0].count >= 2);
    unsigned samples = history[0].count; int startup = 0, healthy = 0, glitch = 0;
    for (unsigned i=0; i<samples; i++) {
        const struct stream_stat_sample *sample = &history[0].samples[i];
        CHECK(sample->v[ST_DT] >= 3000 && sample->v[ST_PAUSE_MS] <= sample->v[ST_AUDIO_OBSERVED]);
        if (!sample->v[ST_AUDIO_OBSERVED]) {
            CHECK(sample->v[ST_PAUSE_MAX] == UINT64_MAX && !sample->v[ST_PAUSES] && !sample->v[ST_PAUSED]); startup = 1;
        } else if (sample->v[ST_AUDIO_OBSERVED] == 20) {
            CHECK(!sample->v[ST_PAUSE_MAX] && !sample->v[ST_PAUSED]); healthy = 1;
        } else {
            CHECK(sample->v[ST_PAUSE_MAX] == 10 && sample->v[ST_PAUSE_MS] == 10);
            CHECK(sample->v[ST_PAUSES] == 1 && sample->v[ST_PAUSED]); glitch = 1;
        }
    }
    CHECK(startup && healthy && glitch); u_free(history); history = NULL;
    CHECK(stream_stats_viewer_count(nodes[0], GROUP1, info.descriptor.source_epoch) == 1);
    CHECK(stream_stats_viewer_count(nodes[0], GROUP2, info.descriptor.source_epoch) == 0);
    CHECK(stream_stats_viewer_count(nodes[0], GROUP1, info.descriptor.source_epoch + 1) == 0);
    /* Один отчёт на сессию, несмотря на два handles; повтор не добавляет историю. */
    struct stream_stat_sample sample = {.sequence = UINT64_C(100000), .generation = 1};
    sample.v[ST_DT] = 3000; sample.v[ST_LAG] = UINT64_MAX; sample.v[ST_NAT] = 3;
    uint8_t wire[STREAM_WIRE_HEADER + STREAM_STAT_FIELDS * 8];
    struct stream_message message = {.descriptor=info.descriptor, .command=STREAM_CMD_STATS, .sender_epoch=1, .handle=sample.sequence};
    stream_wire_header(wire,&message);
    for(unsigned i=0;i<STREAM_STAT_FIELDS;i++)stream_write_be(wire+STREAM_WIRE_HEADER+i*8,sample.v[i],8);
    dispatch(stream_network_get(nodes[0]), stream_address_read(nodes[1]->my_ed25519_pubkey), wire, sizeof(wire), 1);
    dispatch(stream_network_get(nodes[0]), stream_address_read(nodes[1]->my_ed25519_pubkey), wire, sizeof(wire), 1);
    CHECK(stream_stats_read(nodes[0], GROUP1, info.descriptor.source_epoch, &history, &count) == STREAM_OK);
    CHECK(count==1 && history[0].count==samples+1 && history[0].samples[samples].v[ST_NAT]==3);
    u_free(history); history=NULL;
    /* Счётчик не включает relay, завершённые и отчёты возрастом от 9с. */
    stream_write_be(wire + STREAM_WIRE_HEADER + ST_RELAY * 8, 1, 8);
    message.handle++; stream_wire_header(wire, &message);
    dispatch(stream_network_get(nodes[0]), stream_address_read(nodes[1]->my_ed25519_pubkey), wire, sizeof(wire), 1);
    CHECK(stream_stats_viewer_count(nodes[0], GROUP1, info.descriptor.source_epoch) == 0);
    stream_write_be(wire + STREAM_WIRE_HEADER + ST_RELAY * 8, 0, 8);
    stream_write_be(wire + STREAM_WIRE_HEADER + ST_FINISHED * 8, 1, 8);
    message.handle++; stream_wire_header(wire, &message);
    dispatch(stream_network_get(nodes[0]), stream_address_read(nodes[1]->my_ed25519_pubkey), wire, sizeof(wire), 1);
    CHECK(stream_stats_viewer_count(nodes[0], GROUP1, info.descriptor.source_epoch) == 0);
    stream_write_be(wire + STREAM_WIRE_HEADER + ST_FINISHED * 8, 0, 8);
    message.handle++; stream_wire_header(wire, &message);
    dispatch(stream_network_get(nodes[0]), stream_address_read(nodes[1]->my_ed25519_pubkey), wire, sizeof(wire), 1);
    CHECK(stream_stats_viewer_count(nodes[0], GROUP1, info.descriptor.source_epoch) == 1);
    struct stream_stats_entry *entry = stream_network_get(nodes[0])->statistics;
    CHECK(entry && stream_address_equal(entry->data.node, stream_address_read(nodes[1]->my_ed25519_pubkey)) && entry->data.count);
    struct stream_stat_sample *latest = &entry->data.samples[entry->data.count - 1];
    uint64_t received_ms = latest->at_ms;
    latest->at_ms = get_time_tb() / 10 - STREAM_STATS_STALE_MS;
    CHECK(stream_stats_viewer_count(nodes[0], GROUP1, info.descriptor.source_epoch) == 0);
    latest->at_ms++; CHECK(stream_stats_viewer_count(nodes[0], GROUP1, info.descriptor.source_epoch) == 1);
    latest->at_ms = received_ms;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] viewer count: unique nodes, exact epoch, relay/finished exclusion, 9s expiry");
    /* Классификация использует опубликованные адреса, а не сам факт public IP. */
    struct TOPO_SOCKMETA4 direct={.id=1,.address_profile={ADDRESS_MAP_REAL_IP,ADDRESS_IN_OPEN,ADDRESS_SOURCE_ETCP}};
    struct TOPO_SOCKMETA4 eim={.id=2,.address_profile={ADDRESS_MAP_EIM,ADDRESS_IN_CONTACT,ADDRESS_SOURCE_ETCP}};
    struct TOPO_SOCKMETA4 strict={.id=3,.address_profile={ADDRESS_MAP_DEPENDENT,ADDRESS_IN_CONTACT,ADDRESS_SOURCE_ETCP}};
    direct.next=&eim;eim.next=&strict;
    struct TOPO_ADDR4 a={.addr={8,8,8,8},.port=1234,.socket_id=1,.protocol=TOPO_PROTO_UDP};
    struct TOPO_ADDR4 b=a,c=a; a.next=&b;b.next=&c;c.socket_id=3;b.socket_id=2;
    struct TOPO_NODE node={.v4_sock_meta=&direct,.v4_addrs=&a};
    CHECK(topo_node_best_confirmed_type(&node)==1);
    node.v4_addrs=&b;CHECK(topo_node_best_confirmed_type(&node)==2);
    eim.address_profile.inbound=ADDRESS_IN_UNKNOWN;CHECK(topo_node_best_confirmed_type(&node)==3);
    node.v4_addrs=&b;b.next=NULL;CHECK(topo_node_best_confirmed_type(&node)==0);
    stream_receive_close(receive);stream_close(second);stream_close(viewer);stream_close(publisher);
    test_clock_run_for(ua,10000);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM,"[PASS] statistics: routed reports, one per session, healthy PCM and 10ms audio starvation, dedup, best NAT");
}

int main(int argc, char **argv) {
    const char *mode = argc == 2 ? argv[1] : NULL;
#ifdef STREAM_MEMBER_TEST_ONLY
    mode = "--member-only";
#elif defined(STREAM_RELAY_TEST_ONLY)
    mode = "--relay-only";
#elif defined(STREAM_FAILOVER_TEST_ONLY)
    mode = "--failover-only";
#elif defined(STREAM_OFFLINE_TEST_ONLY)
    mode = "--offline-only";
#elif defined(STREAM_OVERLOAD_TEST_ONLY)
    mode = "--overload-only";
#elif defined(STREAM_PATHS_TEST_ONLY)
    mode = "--paths-only";
#elif defined(STREAM_LIVE_TEST_ONLY)
    mode = "--live-only";
#endif
    unsigned node_count = mode && (!strcmp(mode, "--failover-only") || !strcmp(mode, "--overload-only") || !strcmp(mode, "--paths-only")) ? 5 : 4;
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_INFO);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_MEMBER_SYNC, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_CONNECTION, DEBUG_LEVEL_INFO);
        debug_set_category_level(DEBUG_CATEGORY_KEEPALIVE, DEBUG_LEVEL_INFO);
    }
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_WARN);
#ifdef SIGPIPE
    CHECK(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "TCP stream test ignores SIGPIPE; closed socket errors are handled by transport");
#endif
    test_member_format();
    size_t allocations = u_get_allocated_count();
    test_clock_init(STREAM_TEST_CLOCK_STEP_TB); utun_instance_set_tun_init_enabled(0);
    ua = uasync_create(); CHECK(ua != NULL);
    for (unsigned g = 0; g < 2; g++) {
        struct test_group_keys* keys = &group_keys[g];
        memset(keys->ed_private, g ? 68 : 66, 32);
        CHECK(!sc_identity_from_seed(keys->ed_private, keys->x.public_key, keys->x.private_key, keys->ed));
        CHECK(!strcmp(sc_public_key_hex(keys->ed, (char[65]){0}), g ? CHANNEL2 : CHANNEL1));
    }
    char directory[512] = "/tmp/utun-stream-service-XXXXXX"; CHECK(test_mkdtemp(directory) == 0);
    for (unsigned i = 0; i < node_count; i++) {
        uint8_t device_x[32], device_seed[32]; CHECK(sc_identity_generate_config(device_x, device_seed) == SC_OK);
        char priv[65], pub[65], config[1600]; hex(device_seed, priv); hex(device_x, pub);
        unsigned type = node_count == 4 && (i == 0 || i == 2) ? CLIENT_TYPE_MOBILE : CLIENT_TYPE_DESKTOP;
        snprintf(config, sizeof(config), "[global]\nmy_private_key=%s\nmy_public_key=%s\ndb_path=%s/node%u\nclient_type=%u\n"
            "[server:direct]\naddr=127.0.0.1:0\ntype=public\ntransport=%s\n"
            "%s[allowed_keys]\nallow_all=1\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=0\n",
            priv, pub, directory, i, type, i == 3 ? "tcp" : "udp",
            i == 3 ? "" : "[server:tcp]\naddr=127.0.0.1:0\ntype=public\ntransport=tcp\n");
        nodes[i] = utun_instance_create_from_str(ua, config); CHECK(nodes[i] != NULL);
        snprintf(nodes[i]->name, sizeof(nodes[i]->name), "node%u", i);
        CHECK(utun_core_start(nodes[i]) == 0 && chat_service_start(nodes[i]) == 0 && nodes[i]->stream);
        CHECK(nodes[i]->router_bindings.callbacks[ETCP_RT_ID_STREAM]
              && nodes[i]->router_bindings.callbacks[ETCP_RT_ID_STREAM] != nodes[i]->router_bindings.callbacks[ETCP_RT_ID_MEMBER_FILES]);
        for (unsigned g = 0; g < 2; g++) {
            const char* id = g ? CHANNEL2 : CHANNEL1;
            uint64_t group = g ? GROUP2 : GROUP1;
            uint8_t signature[64] = {0};
            const struct test_group_keys* keys = &group_keys[g];
            CHECK(!test_group_signature(id, "stream", nodes[0]->my_ed25519_pubkey, keys->x.public_key, keys->ed, keys->ed_private, signature));
            CHECK(topo_node_sqlite_channel_put(nodes[i]->topo_sqlite_db, id, "stream", nodes[0]->my_ed25519_pubkey,
                keys->x.public_key, NULL, keys->ed, i ? NULL : keys->ed_private, signature) == 0);
            CHECK(topo_groups_create_group(nodes[i]->topo_groups, group, TOPO_GROUP_TYPE_CHAT, id));
            CHECK(!chat_core_ensure_channel_ready(nodes[i], id));
        }
    }
    for (unsigned destination = 0; destination < node_count; destination++) {
        for (unsigned member = 0; member < node_count; member++) {
            put_member(destination, member, GROUP1); put_member(destination, member, GROUP2);
            struct TOPO_NODE *identity = topo_node_registry_find(nodes[member]->topo_groups, nodes[member]->node_id);
            CHECK(identity && topo_node_sqlite_snapshot_put(nodes[destination]->topo_sqlite_db, identity) >= 0);
        }
    }
    test_supernode_lookup();
    struct TOPO_PEER_REQUEST *connections[4] = {0};
    for (unsigned i = 1; i < node_count; i++)
        CHECK(topo_group_peer_open(topo_groups_find(nodes[0]->topo_groups, GROUP1), nodes[i]->node_id, &connections[i - 1]) == 0);
    uint64_t deadline = get_time_tb() + 150000;
    while (get_time_tb() < deadline) {
        int ready = 1;
        for (unsigned i = 0; i + 1 < node_count; i++) if (topo_group_peer_phase(connections[i]) != TOPO_PEER_READY) ready = 0;
        if (ready) break;
        test_clock_poll(ua, 100);
    }
    for (unsigned i = 0; i + 1 < node_count; i++) CHECK(topo_group_peer_phase(connections[i]) == TOPO_PEER_READY);
    /* Каналы/мемберы стенда добавлены после chat_sync_init: первый обмен запускается явно. */
    for (unsigned i = 1; i < node_count; i++) {
        CHECK(member_sync_start(nodes[0], instance_find_conn_by_key(nodes[0], nodes[i]->my_keys.public_key), CHANNEL1, NULL, NULL) == 0);
        CHECK(member_sync_start(nodes[i], instance_find_conn_by_key(nodes[i], nodes[0]->my_keys.public_key), CHANNEL1, NULL, NULL) == 0);
    }
    if (mode && (!strcmp(mode, "--relay-only") || !strcmp(mode, "--lifecycle-only") || !strcmp(mode, "--wake-only")
        || !strcmp(mode, "--bootstrap-only") || !strcmp(mode, "--recovery-only") || !strcmp(mode, "--member-only")
        || !strcmp(mode, "--control-expiry-only") || !strcmp(mode, "--offline-only") || !strcmp(mode, "--failover-only")
        || !strcmp(mode, "--budget-only") || !strcmp(mode, "--overload-only") || !strcmp(mode, "--paths-only")
        || !strcmp(mode, "--live-only") || !strcmp(mode, "--statistics-only") || !strcmp(mode, "--control-window-only"))) {
        if (!strcmp(mode, "--statistics-only")) test_statistics();
        else if (!strcmp(mode, "--member-only")) test_member_lifecycle();
        else if (!strcmp(mode, "--control-expiry-only")) test_control_expiry();
        else if (!strcmp(mode, "--control-window-only")) test_control_windows();
        else if (!strcmp(mode, "--relay-only")) test_supernode_relay();
        else if (!strcmp(mode, "--lifecycle-only")) {
            test_capacity_limits(); test_single_supplier_budget(); test_control_windows();
            test_partial_bootstrap_and_end(); test_delayed_relay_tail(0); test_delayed_relay_tail(1);
        }
        else if (!strcmp(mode, "--bootstrap-only")) test_bootstrap();
        else if (!strcmp(mode, "--recovery-only")) test_recovery_scenarios();
        else if (!strcmp(mode, "--failover-only")) test_failover();
        else if (!strcmp(mode, "--overload-only")) test_overload_migration();
        else if (!strcmp(mode, "--paths-only")) test_paths();
        else if (!strcmp(mode, "--live-only")) test_live();
        else if (!strcmp(mode, "--budget-only")) test_shared_probe_budget();
        else if (!strcmp(mode, "--offline-only")) test_source_offline();
        else test_sources_after_wake();
        for (unsigned i = 0; i + 1 < node_count; i++) topo_group_peer_close(connections[i]);
        for (unsigned i = 0; i < node_count; i++) { stream_set_event_callback(nodes[i], NULL, NULL); utun_instance_destroy(nodes[i]); }
        uasync_destroy(ua, 1); test_clock_finish();
        if (u_get_allocated_count() != allocations) {
            debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "relay tracked allocations before=%zu after=%zu", allocations, u_get_allocated_count());
            u_report_unfreed_blocks();
        }
        CHECK(u_get_allocated_count() == allocations);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] isolated stream checks complete, no tracked allocations remain");
        return 0;
    }
    test_statistics();
    test_recovery_scenarios();
    test_sources_after_wake();
    test_profile_atomicity();
    test_capacity_limits(); test_single_supplier_budget(); test_control_windows();
    test_control_expiry();
    stream_set_event_callback(nodes[0], on_stream, NULL); stream_set_event_callback(nodes[1], on_stream, NULL);
    struct STREAM_HANDLE *publisher = NULL, *publisher2 = NULL, *other_group = NULL, *mobile = NULL, *desktop = NULL;
    struct stream_parameters fec_parameters = {.n = 8, .r = 2, .block_size = STREAM_RS_DEFAULT_SIZE};
    CHECK(stream_publish_open(nodes[0], GROUP1, &fec_parameters, &publisher) == STREAM_OK);
    CHECK(stream_publish_open(nodes[0], GROUP2, NULL, &publisher2) == STREAM_OK);
    struct stream_info info; CHECK(stream_get_info(publisher, &info) == STREAM_OK);
    uint64_t epoch = info.descriptor.source_epoch;
    for (unsigned i = 1; i < 4; i++) wait_state(i, 0, GROUP1, epoch, 0);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &reader1) == STREAM_OK);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &reader2) == STREAM_OK);
    CHECK(stream_get_info(reader1, &info) == STREAM_OK && info.owners == 2 && info.buffer.blocks == 0);
    CHECK(stream_subscribe_manual_open(nodes[2], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &mobile) == STREAM_OK);
    CHECK(stream_subscribe_manual_open(nodes[3], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &desktop) == STREAM_OK);
    struct stream_member_state state;
    CHECK(stream_member_load(nodes[1], GROUP1, stream_address_read(nodes[1]->my_ed25519_pubkey), &state, NULL) == STREAM_OK && state.count == 1);
    for (unsigned i = 2; i < 4; i++) wait_state(1, i, GROUP1, epoch, 1);
    struct stream_candidate *candidates = NULL; size_t count = 0;
    CHECK(stream_get_candidates(reader1, &candidates, &count) == STREAM_OK);
    int source_found = 0, desktop_found = 0;
    for (size_t i = 0; i < count; i++) {
        CHECK(!stream_address_equal(candidates[i].address, stream_address_read(nodes[2]->my_ed25519_pubkey)));
        if (stream_address_equal(candidates[i].address, stream_address_read(nodes[0]->my_ed25519_pubkey))) { CHECK(candidates[i].source); source_found = 1; }
        if (stream_address_equal(candidates[i].address, stream_address_read(nodes[3]->my_ed25519_pubkey))) desktop_found = 1;
    }
    CHECK(source_found && desktop_found); u_free(candidates); candidates = NULL;
    test_bootstrap();
    struct TOPO_GROUP *group = topo_groups_find(nodes[1]->topo_groups, GROUP1);
    struct TOPO_GROUP_NODE *peer = topo_node_find_by_id(group, nodes[3]->node_id); CHECK(peer != NULL);
    topo_nodeq_remove_node(group, peer); /* BGP withdraw не удаляет сохранённую опцию мембера. */
    CHECK(stream_member_load(nodes[1], GROUP1, stream_address_read(nodes[3]->my_ed25519_pubkey), &state, NULL) == STREAM_OK);
    CHECK(stream_member_has_subscription(&state, stream_address_read(nodes[0]->my_ed25519_pubkey), epoch));
    CHECK(stream_get_candidates(reader1, &candidates, &count) == STREAM_OK);
    for (size_t i = 0; i < count; i++) CHECK(!stream_address_equal(candidates[i].address, stream_address_read(nodes[3]->my_ed25519_pubkey)));
    u_free(candidates);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] Merkle subscriptions and BGP gating; mobile source allowed, mobile relay excluded");
    test_network();
    test_pressure();
    test_cpu_admission();
    test_global_pressure(30);
    test_global_pressure(81);
    test_admission_retry();
    test_shared_probe_budget();
    test_automatic();
    test_pending_down();

    /* Независимая GROUP2-подписка требует собственного BGP-пути, а не только записи профиля в общей БД. */
    struct TOPO_PEER_REQUEST *other_connection = NULL;
    CHECK(topo_group_peer_open(topo_groups_find(nodes[1]->topo_groups, GROUP2), nodes[0]->node_id, &other_connection) == 0);
    deadline = get_time_tb() + 150000;
    while (topo_group_peer_phase(other_connection) != TOPO_PEER_READY && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(topo_group_peer_phase(other_connection) == TOPO_PEER_READY && stream_member_online(nodes[1], GROUP2, stream_address_read(nodes[0]->my_ed25519_pubkey)));
    struct stream_info second; CHECK(stream_get_info(publisher2, &second) == STREAM_OK);
    char source_value[MS_USERINFO_MAX + 1], subs_value[MS_USERINFO_MAX + 1];
    CHECK(stream_member_load(nodes[0], GROUP2, stream_address_read(nodes[0]->my_ed25519_pubkey), &state, NULL) == STREAM_OK);
    CHECK(stream_member_format(&state, source_value, subs_value) == STREAM_OK);
    char profile2[MS_USERINFO_MAX + 1]; uint8_t sig2[64], join2[64]; uint64_t version2, join_ts;
    profile(0, GROUP2, profile2, sig2, &version2);
    CHECK(topo_node_sqlite_member_get_join(nodes[0]->topo_sqlite_db, CHANNEL2, nodes[0]->my_ed25519_pubkey, join2, &join_ts) == 0);
    uint8_t tree2[64]; uint8_t parent[32];
    CHECK(topo_node_sqlite_member_get_sign(nodes[0]->topo_sqlite_db, CHANNEL2, nodes[0]->my_ed25519_pubkey, parent, tree2) == 0);
    CHECK(member_sync_put(nodes[1], CHANNEL2, nodes[0]->my_keys.public_key, nodes[0]->my_ed25519_pubkey,
        join2, join_ts, sig2, version2, profile2, NULL, NULL, 0, member_sync_signer(parent), tree2) >= 0);
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP2, stream_address_read(nodes[0]->my_ed25519_pubkey), &other_group) == STREAM_OK);

    for (unsigned i = 0; i < sizeof(expected); i++) expected[i] = (uint8_t)(i * 71 + 3);
    test_restart_bound(desktop, &info.descriptor);
    struct stream_encoder *encoder = NULL;
    CHECK(stream_encoder_create(&encoder, &info.descriptor, nodes[0]->my_ed25519_privkey) == STREAM_OK);
    const uint8_t *blocks[RS_MAX_BLOCKS];
    size_t size = 0;
    CHECK(stream_encoder_encode(encoder, 100, 65530, 0, expected, sizeof(expected), blocks, &size) == STREAM_OK);
    close_readers_in_callback = 1;
    for (unsigned i = 0; i < 8; i++) if (i != 1 && i != 5) CHECK(stream_ingest_block(nodes[1], blocks[i], size) == STREAM_OK);
    run_until(&data_events[1], 1); run_until(&closed_events[1], 1);
    CHECK(!reader1 && !reader2 && stream_handle_active(other_group));
    uint8_t invalid[STREAM_BLOCK_MAX_SIZE]; memcpy(invalid, blocks[0], size); invalid[size - 1] ^= 1;
    CHECK(stream_ingest_block(nodes[1], invalid, size) == STREAM_WAIT); /* Нет подписки — нет decode/crypto. */
    stream_encoder_destroy(encoder);
    size_t accepted = 0;
    CHECK(stream_publish_write(publisher, expected, sizeof(expected), &accepted) == STREAM_OK && accepted == sizeof(expected));
    CHECK(stream_publish_finish(publisher) == STREAM_OK);
    run_until(&data_events[0], 1); run_until(&end_events[0], 1);
    CHECK(stream_member_load(nodes[0], GROUP1, stream_address_read(nodes[0]->my_ed25519_pubkey), &state, NULL) == STREAM_OK && !state.published && state.count == 1);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] opaque data/FEC delivery, close inside callback, independent group and DATA before END");

    stream_close(publisher); publisher = NULL;
    CHECK(stream_publish_open(nodes[0], GROUP1, NULL, &publisher) == STREAM_OK);
    CHECK(stream_get_info(publisher, &info) == STREAM_OK && info.descriptor.source_epoch != epoch);
    deadline = get_time_tb() + 150000;
    while ((stream_handle_active(mobile) || stream_handle_active(desktop)) && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(!stream_handle_active(mobile) && !stream_handle_active(desktop));
    CHECK(stream_handle_reason(mobile) == STREAM_CLOSE_SOURCE_CHANGED && stream_handle_reason(desktop) == STREAM_CLOSE_SOURCE_CHANGED);
    CHECK(!stream_service_needs_activity(nodes[2]) && nodes[2]->client_activity == CLIENT_ACTIVITY_STANDBY);
    stream_close(mobile); stream_close(desktop);
    /* Ремонт собственного JOIN отзывает допуск и закрывает буфер. */
    sqlite3_stmt *broken = NULL; uint8_t bad_signature[64]; memset(bad_signature, 1, sizeof(bad_signature));
    CHECK(sqlite3_prepare_v2(nodes[1]->topo_sqlite_db, "UPDATE peers_" CHANNEL2 " SET join_sig=? WHERE node_id=?",
                            -1, &broken, NULL) == SQLITE_OK);
    sqlite3_bind_blob(broken, 1, bad_signature, sizeof(bad_signature), SQLITE_STATIC);
    sqlite3_bind_int64(broken, 2, (sqlite3_int64)nodes[1]->node_id);
    CHECK(sqlite3_step(broken) == SQLITE_DONE); sqlite3_finalize(broken);
    uint8_t purge_root_before[32], purge_root_after[32];
    CHECK(merkle_sync_read_hash(nodes[1], MT_DATA_MEMBERS, CHANNEL2, 0, 0, purge_root_before) == 0);
    int purge_count_before = member_sync_count(nodes[1], CHANNEL2);
    for (int fault = 0; fault < 2; fault++) {
        const char* trigger = fault == 0
            ? "CREATE TRIGGER reject_purge BEFORE UPDATE OF join_sig ON peers_" CHANNEL2 " BEGIN SELECT RAISE(ABORT,'stream JOIN repair'); END"
            : "CREATE TRIGGER reject_purge BEFORE DELETE ON merkle_tree_hash BEGIN SELECT RAISE(ABORT,'stream purge index'); END";
        CHECK(sqlite3_exec(nodes[1]->topo_sqlite_db, trigger, NULL, NULL, NULL) == SQLITE_OK);
        CHECK(member_sync_verify_and_purge(nodes[1], CHANNEL2) < 0);
        CHECK(stream_handle_active(other_group) && member_sync_count(nodes[1], CHANNEL2) == purge_count_before);
        CHECK(merkle_sync_read_hash(nodes[1], MT_DATA_MEMBERS, CHANNEL2, 0, 0, purge_root_after) == 0);
        CHECK(!memcmp(purge_root_before, purge_root_after, sizeof(purge_root_before)));
        CHECK(sqlite3_exec(nodes[1]->topo_sqlite_db, "DROP TRIGGER reject_purge", NULL, NULL, NULL) == SQLITE_OK);
    }
    CHECK(member_sync_verify_and_purge(nodes[1], CHANNEL2) == 0);
    CHECK(!stream_handle_active(other_group) && stream_handle_reason(other_group) == STREAM_CLOSE_MEMBER);
    run_until(&closed_events[1], 2); stream_close(other_group); other_group = NULL;
    CHECK(topo_group_peer_phase(other_connection) == TOPO_PEER_FAILED);
    topo_group_peer_close(other_connection); other_connection = NULL;
    put_member(1, 1, GROUP2);
    CHECK(topo_group_peer_open(topo_groups_find(nodes[1]->topo_groups, GROUP2), nodes[0]->node_id, &other_connection) == 0);
    /* Восстановление допуска повторно согласует групповую сессию; транспорт UP ещё не даёт маршрута. */
    deadline = get_time_tb() + 150000;
    while ((topo_group_peer_phase(other_connection) != TOPO_PEER_READY ||
        !stream_member_online(nodes[1], GROUP2, stream_address_read(nodes[0]->my_ed25519_pubkey))) && get_time_tb() < deadline) test_clock_poll(ua, 100);
    if (!stream_member_online(nodes[1], GROUP2, stream_address_read(nodes[0]->my_ed25519_pubkey))) DEBUG_ERROR(DEBUG_CATEGORY_STREAM,
        "restored member route not ready phase=%d group=%llu source=%llx", topo_group_peer_phase(other_connection),
        (unsigned long long)GROUP2, (unsigned long long)nodes[0]->node_id);
    CHECK(topo_group_peer_phase(other_connection) == TOPO_PEER_READY && stream_member_online(nodes[1], GROUP2, stream_address_read(nodes[0]->my_ed25519_pubkey)));
    CHECK(stream_subscribe_manual_open(nodes[1], GROUP2, stream_address_read(nodes[0]->my_ed25519_pubkey), &other_group) == STREAM_OK);
    chat_core_delete_channel(nodes[1], CHANNEL2);
    CHECK(!stream_handle_active(other_group) && stream_handle_reason(other_group) == STREAM_CLOSE_GROUP);
    stream_close(other_group);
    topo_group_peer_close(other_connection);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] source epoch, lost own membership and channel deletion detach local handles");

    char stale[MS_USERINFO_MAX + 1]; uint8_t stale_signature[64]; uint64_t stale_version;
    profile(0, GROUP1, stale, stale_signature, &stale_version);
    CHECK(utun_service_start(nodes[0]) == 0);
    sqlite3 *db = nodes[0]->topo_sqlite_db; struct ETCP_SOCKET *sockets = nodes[0]->etcp_sockets;
    CHECK(stream_publish_write(publisher2, expected, 17, &accepted) == STREAM_OK);
    chat_service_stop(nodes[0]);
    CHECK(!nodes[0]->stream && nodes[0]->utun_started && nodes[0]->topo_sqlite_db == db && nodes[0]->etcp_sockets == sockets);
    CHECK(!stream_handle_active(publisher) && !stream_handle_active(publisher2));
    stream_close(publisher); stream_close(publisher2);
    group_service_stop(nodes[0]); /* Перезапуск общей группы сбрасывает runtime-кеш подписанного профиля. */
    sqlite3_stmt *restore = NULL;
    CHECK(sqlite3_prepare_v2(db, "UPDATE peers_" CHANNEL1 " SET userinfo=?,update_sig=?,update_ts=? WHERE node_id=?", -1, &restore, NULL) == SQLITE_OK);
    sqlite3_bind_text(restore, 1, stale, -1, SQLITE_STATIC); sqlite3_bind_blob(restore, 2, stale_signature, 64, SQLITE_STATIC);
    sqlite3_bind_int64(restore, 3, (sqlite3_int64)stale_version); sqlite3_bind_int64(restore, 4, (sqlite3_int64)nodes[0]->node_id);
    CHECK(sqlite3_step(restore) == SQLITE_DONE); sqlite3_finalize(restore);
    CHECK(chat_service_start(nodes[0]) == 0 && nodes[0]->stream);
    uint64_t restored_group = topo_groups_channel_slot(nodes[0]->topo_groups, CHANNEL1); CHECK(restored_group);
    CHECK(stream_member_load(nodes[0], restored_group, stream_address_read(nodes[0]->my_ed25519_pubkey), &state, NULL) == STREAM_OK && !state.published && !state.count);
    CHECK(data_events[0] == 1); /* Pending 100ms batch не выполнилась после stop. */
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] CHAT stop keeps UTUN/core; source timer cancelled; stale signed options cleared on restart");
    for (unsigned i = 0; i < 4; i++) stream_set_event_callback(nodes[i], NULL, NULL);
    test_partial_bootstrap_and_end();
    test_delayed_relay_tail(0); test_delayed_relay_tail(1);
    test_delayed_media_reader();
    test_viewer_radio_mute();
    for (unsigned i = 0; i < 3; i++) topo_group_peer_close(connections[i]);
    for (unsigned i = 0; i < 4; i++) utun_instance_destroy(nodes[i]);
    uasync_destroy(ua, 1); test_clock_finish();
    if (u_get_allocated_count() != allocations) {
        debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "tracked allocations before=%zu after=%zu", allocations, u_get_allocated_count());
        u_report_unfreed_blocks();
    }
    CHECK(u_get_allocated_count() == allocations);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] stream service checks complete, no tracked allocations remain");
    return 0;
}
