/* TTL и callbacks broadcast: реальные очереди/таймеры, управляемая шкала времени. */
#define _GNU_SOURCE
#include "utun_instance.h"
#include "topo_group.h"
#include "broadcast.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #c); exit(1); } } while (0)

/* Вход приёма, используемый ETCP-диспетчером внутри broadcast.c. */
void broadcast_recv(struct TOPO_GROUP*, struct ETCP_CONN*, const uint8_t*, size_t);

static uint64_t now_us = 1000000;
static int fail_timer, calls, order[16], order_count;

int __real_clock_gettime(clockid_t id, struct timespec* ts);
int __wrap_clock_gettime(clockid_t id, struct timespec* ts) {
    if (id != CLOCK_BOOTTIME) return __real_clock_gettime(id, ts);
    ts->tv_sec = now_us / 1000000; ts->tv_nsec = now_us % 1000000 * 1000;
    return 0;
}

void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int tb, void* arg, timeout_callback_t cb, const char* name) {
    if (fail_timer && name && strcmp(name, "broadcast_cleanup_timer") == 0) return NULL;
    return __real_uasync_set_timeout(ua, tb, arg, cb, name);
}

static void received(const uint8_t* uuid, const uint8_t* data, uint16_t len, void* arg) {
    CHECK(uuid && len == 3 && data[0] == 1 && data[1] == 0 && data[2] == 2);
    CHECK(order_count < (int)(sizeof(order) / sizeof(order[0])));
    order[order_count++] = (int)(intptr_t)arg;
    calls++;
}

static void receive_id(struct TOPO_GROUP* group, uint8_t id) {
    uint8_t payload[BROADCAST_UUID_SIZE + 2 + 3] = {0};
    payload[0] = id; payload[BROADCAST_UUID_SIZE] = 3;
    payload[BROADCAST_UUID_SIZE + 2] = 1; payload[BROADCAST_UUID_SIZE + 4] = 2;
    order_count = 0;
    broadcast_recv(group, NULL, payload, sizeof(payload));
}

static void cleanup_tick(struct UASYNC* ua) {
    now_us += (uint64_t)BROADCAST_CLEANUP_INTERVAL_TB * 100;
    uasync_poll(ua, 0);
}

static void test_ttl(struct UASYNC* ua, struct TOPO_GROUP* group) {
    calls = 0;
    CHECK(broadcast_init(group) == 0);
    broadcast_add_cbk(group, received, (void*)(intptr_t)1);
    receive_id(group, 1); CHECK(calls == 1);
    receive_id(group, 1); CHECK(calls == 1);
    cleanup_tick(ua); /* 60с: исходный UUID ещё свежий. */
    receive_id(group, 1); CHECK(calls == 1);
    receive_id(group, 2); CHECK(calls == 2);
    cleanup_tick(ua); /* 120с: равенство TTL ещё не удаляет запись. */
    receive_id(group, 1); CHECK(calls == 2);
    receive_id(group, 2); CHECK(calls == 2);
    cleanup_tick(ua); /* 180с: первый UUID устарел, второму ровно TTL. */
    receive_id(group, 2); CHECK(calls == 2);
    receive_id(group, 1); CHECK(calls == 3);
    cleanup_tick(ua);
    receive_id(group, 1); CHECK(calls == 3);
    receive_id(group, 2); CHECK(calls == 4);
    broadcast_destroy(group); CHECK(!group->broadcast);
    puts("PASS broadcast TTL: fresh, exact boundary, expired, mixed ages");
}

static void test_callbacks(struct TOPO_GROUP* group) {
    calls = 0;
    CHECK(broadcast_init(group) == 0);
    CHECK(broadcast_init(group) == -1);
    broadcast_add_cbk(group, received, (void*)(intptr_t)1);
    broadcast_add_cbk(group, received, (void*)(intptr_t)2);
    broadcast_add_cbk(group, received, (void*)(intptr_t)2);
    receive_id(group, 3);
    CHECK(calls == 3 && order_count == 3 && order[0] == 2 && order[1] == 2 && order[2] == 1);
    broadcast_remove_cbk(group, received, (void*)(intptr_t)2);
    receive_id(group, 4);
    CHECK(calls == 5 && order_count == 2 && order[0] == 2 && order[1] == 1);
    broadcast_remove_cbk(group, received, (void*)(intptr_t)2);
    broadcast_remove_cbk(group, received, (void*)(intptr_t)2);
    receive_id(group, 5); CHECK(calls == 6 && order_count == 1 && order[0] == 1);
    const uint8_t data[] = {1, 0, 2};
    CHECK(broadcast_send(group, data, sizeof(data)) == 0 && calls == 6);
    CHECK(broadcast_send(group, data, 0) == 0 && calls == 6);
    CHECK(broadcast_send(group, NULL, 0) == -1);
    CHECK(broadcast_send(group, data, BROADCAST_MAX_DATA + 1) == -1);
    broadcast_destroy(group); broadcast_destroy(group);
    puts("PASS broadcast callbacks: duplicates, order, removal, no local echo");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    size_t baseline = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct UTUN_INSTANCE inst = {.ua = ua};
    struct TOPO_GROUP group = {.instance = &inst, .group_id = 42, .group_type = TOPO_GROUP_TYPE_UTUN};
    size_t before = u_get_allocated_count();
    fail_timer = 1;
    CHECK(broadcast_init(&group) == -1 && !group.broadcast);
    CHECK(u_get_allocated_count() == before);
    fail_timer = 0;
    test_ttl(ua, &group);
    test_callbacks(&group);
    CHECK(broadcast_init(&group) == 0);
    fail_timer = 1; cleanup_tick(ua);
    broadcast_destroy(&group);
    uasync_destroy(ua, 0);
    CHECK(u_get_allocated_count() == baseline);
    puts("PASS broadcast timer failures and resource cleanup");
    return 0;
}
