/**
 * @file test_dummynet.c
 * @brief Комплексный тест модуля dummynet (строго однопоточный, только uasync)
 *
 * Архитектура:
 * - Все операции через uasync (таймеры и сокеты)
 * - Без usleep/delay - только uasync_poll()
 * - Все callback в одном контексте
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/time.h>

#include "../lib/u_async.h"
#include "../lib/socket_compat.h"
#include "../lib/debug_config.h"
#include "dummynet.h"

#define LISTEN_PORT             18000
#define PKT_COUNT               1000
#define MAX_PKT_SIZE            1500
#define MIN_PKT_SIZE            500
#define SEND_INTERVAL_US        100     /* 0.1ms между пакетами = 10 pkt/ms */

/* Формат пакета */
struct test_pkt {
    uint32_t seq_num;
    uint64_t send_time_us;
    uint16_t data_len;
} __attribute__((packed));

/* Состояние теста */
struct test_state {
    struct UASYNC* ua;
    struct dummynet* dn;
    socket_t sock_a;           /* Порт listen_port-1 */
    socket_t sock_b;           /* Порт listen_port+1 */
    
    /* Отправка */
    uint32_t next_seq;
    uint32_t pkts_to_send;
    void* send_timer;
    
    /* Статистика приема */
    uint32_t recv_a;           /* Получено на A (от B) */
    uint32_t recv_b;           /* Получено на B (от A) */
    uint32_t lost_a;
    uint32_t lost_b;
    uint64_t latency_sum_a;
    uint64_t latency_sum_b;
    uint32_t latency_count_a;
    uint32_t latency_count_b;
    
    /* Флаги завершения */
    int done;
    int passed;
};

static inline uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

/* Функция отправки одного пакета */
static void send_one_packet(struct test_state* st, uint32_t seq) {
    uint8_t buf[MAX_PKT_SIZE];
    struct test_pkt* pkt = (struct test_pkt*)buf;
    /* Максимальный размер данных: MAX_PKT_SIZE - sizeof(*pkt) */
    uint16_t max_data_len = MAX_PKT_SIZE - sizeof(struct test_pkt);
    uint16_t data_len = MIN_PKT_SIZE + (rand() % (max_data_len - MIN_PKT_SIZE + 1));
    
    pkt->seq_num = seq;
    pkt->send_time_us = now_us();
    pkt->data_len = data_len;
    
    /* Заполняем данные */
    for (int i = 0; i < data_len; i++) {
        buf[sizeof(*pkt) + i] = (uint8_t)((seq + i) & 0xFF);
    }
    
    /* A -> dummynet (forward) */
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr("127.0.0.1");
    dest.sin_port = htons(LISTEN_PORT);
    
    socket_sendto(st->sock_a, buf, sizeof(*pkt) + data_len, 
                  (struct sockaddr*)&dest, sizeof(dest));
    
    /* B -> dummynet (backward) */
    pkt->seq_num = seq + 10000; /* Отличаем пакеты от B */
    pkt->send_time_us = now_us();
    socket_sendto(st->sock_b, buf, sizeof(*pkt) + data_len,
                  (struct sockaddr*)&dest, sizeof(dest));
}

/* Callback отправки пакета (для интервальной отправки) */
static void send_timer_callback(void* arg) {
    struct test_state* st = (struct test_state*)arg;
    
    if (st->next_seq >= st->pkts_to_send) {
        /* Все отправили, больше не запускаем таймер */
        st->send_timer = NULL;
        return;
    }
    
    send_one_packet(st, st->next_seq);
    st->next_seq++;
    
    /* Перезапускаем таймер на следующую отправку (10 timebase = 1ms) */
    st->send_timer = uasync_set_timeout(st->ua, 10, st, send_timer_callback, "test_dummynet");
}

/* Burst отправка - все пакеты сразу */
static void send_burst(struct test_state* st) {
    for (uint32_t i = 0; i < st->pkts_to_send; i++) {
        send_one_packet(st, i);
        st->next_seq++;
    }
}

/* Callback приема на сокете A */
static void recv_callback_a(socket_t sock, void* arg) {
    struct test_state* st = (struct test_state*)arg;
    uint8_t buf[MAX_PKT_SIZE];
    struct sockaddr_storage src;
    socklen_t src_len = sizeof(src);
    
    ssize_t n = socket_recvfrom(sock, buf, sizeof(buf), (struct sockaddr*)&src, &src_len);
    if (n < (ssize_t)sizeof(struct test_pkt)) return;
    
    struct test_pkt* pkt = (struct test_pkt*)buf;
    uint64_t now = now_us();
    uint64_t latency = now - pkt->send_time_us;
    
    st->recv_a++;
    st->latency_sum_a += latency;
    st->latency_count_a++;
}

/* Callback приема на сокете B */
static void recv_callback_b(socket_t sock, void* arg) {
    struct test_state* st = (struct test_state*)arg;
    uint8_t buf[MAX_PKT_SIZE];
    struct sockaddr_storage src;
    socklen_t src_len = sizeof(src);
    
    ssize_t n = socket_recvfrom(sock, buf, sizeof(buf), (struct sockaddr*)&src, &src_len);
    if (n < (ssize_t)sizeof(struct test_pkt)) return;
    
    struct test_pkt* pkt = (struct test_pkt*)buf;
    uint64_t now = now_us();
    uint64_t latency = now - pkt->send_time_us;
    
    st->recv_b++;
    st->latency_sum_b += latency;
    st->latency_count_b++;
}

/* Создание привязанного сокета */
static socket_t create_bound_socket(uint16_t port) {
    socket_t sock = socket_create_udp(AF_INET);
    if (sock == SOCKET_INVALID) return SOCKET_INVALID;
    
    int reuse = 1;
    int rcvbuf = 2 * 1024 * 1024;  /* 2MB receive buffer */
    int sndbuf = 2 * 1024 * 1024;  /* 2MB send buffer */
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        socket_close_wrapper(sock);
        return SOCKET_INVALID;
    }
    
    return sock;
}

/* Запуск одного сценария */
static int run_scenario(const char* name, 
                        uint32_t delay_fixed_ms, 
                        uint32_t delay_random_ms,
                        uint32_t bandwidth_kbps,
                        uint32_t max_queue_pkts,
                        uint32_t loss_permille) {
    
    printf("\n=== Scenario: %s ===\n", name);
    printf("Config: delay=%u+%u ms, bw=%u kbps, queue=%u, loss=%u.%u%%\n",
           delay_fixed_ms, delay_random_ms, bandwidth_kbps, max_queue_pkts,
           loss_permille / 10, loss_permille % 10);
    
    struct test_state st;
    memset(&st, 0, sizeof(st));
    st.pkts_to_send = PKT_COUNT;
    
    /* Создаем uasync */
    st.ua = uasync_create();
    if (!st.ua) {
        printf("[FAIL] uasync_create failed\n");
        return 0;
    }
    
    /* Создаем сокеты */
    st.sock_a = create_bound_socket(LISTEN_PORT - 1);
    st.sock_b = create_bound_socket(LISTEN_PORT + 1);
    
    if (st.sock_a == SOCKET_INVALID || st.sock_b == SOCKET_INVALID) {
        printf("[FAIL] Failed to create sockets\n");
        uasync_destroy(st.ua, 1);
        return 0;
    }
    
    /* Добавляем сокеты в uasync */
    uasync_add_socket_t(st.ua, st.sock_a, recv_callback_a, NULL, NULL, "dummynet_a", &st);
    uasync_add_socket_t(st.ua, st.sock_b, recv_callback_b, NULL, NULL, "dummynet_b", &st);
    
    /* Создаем dummynet */
    st.dn = dummynet_create(st.ua, "127.0.0.1", LISTEN_PORT);
    if (!st.dn) {
        printf("[FAIL] dummynet_create failed\n");
        uasync_destroy(st.ua, 1);
        return 0;
    }
    
    /* Увеличиваем буферы dummynet сокета */
    socket_t dn_sock = dummynet_get_socket(st.dn);
    if (dn_sock != SOCKET_INVALID) {
        int rcvbuf = 4 * 1024 * 1024;  /* 4MB receive buffer */
        int sndbuf = 4 * 1024 * 1024;  /* 4MB send buffer */
        setsockopt(dn_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        setsockopt(dn_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    }
    
    /* Настраиваем направления */
    dummynet_set_direction(st.dn, DUMMYNET_FORWARD,
                           delay_fixed_ms, delay_random_ms,
                           bandwidth_kbps, max_queue_pkts, loss_permille,
                           "127.0.0.1", LISTEN_PORT + 1);
    
    dummynet_set_direction(st.dn, DUMMYNET_BACKWARD,
                           delay_fixed_ms, delay_random_ms,
                           bandwidth_kbps, max_queue_pkts, loss_permille,
                           "127.0.0.1", LISTEN_PORT - 1);
    
    /* Быстрая отправка через uasync - 1 пакет каждые 10 мкс (100 pkt/ms) */
    printf("Sending %d packets (fast)...\n", PKT_COUNT);
    
    /* Запускаем таймер отправки с интервалом 1 timebase (0.1ms) */
    st.send_timer = uasync_set_timeout(st.ua, 1, &st, send_timer_callback, "test_dummynet");
    
    /* Умное ожидание завершения: ранний выход при готовности + watchdog */
    uint64_t start_time = now_us();
    uint64_t watchdog_start = 0;  /* Стартует после отправки всех пакетов */
    int watchdog_active = 0;
    const uint64_t WATCHDOG_US = 3000000ULL;  /* 3 секунды максимум после отправки */
    
    printf("Running (watchdog: 3s after send completion)...\n");
    
    /* Основной цикл - только uasync_poll */
    while (1) {
        uasync_poll(st.ua, 1);  /* 0.1ms timeout - чаще для обработки таймеров */
        
        uint64_t now = now_us();
        
        /* Активируем watchdog когда все пакеты отправлены */
        if (!watchdog_active && st.next_seq >= PKT_COUNT) {
            watchdog_start = now;
            watchdog_active = 1;
        }
        
        /* Проверяем условия завершения (только после отправки всех пакетов) */
        if (watchdog_active) {
            uint64_t dset_fw, dfire_fw, sset_fw, sfire_fw;
            uint64_t dset_bw, dfire_bw, sset_bw, sfire_bw;
            dummynet_get_debug_counters(st.dn, DUMMYNET_FORWARD, &dset_fw, &dfire_fw, &sset_fw, &sfire_fw);
            dummynet_get_debug_counters(st.dn, DUMMYNET_BACKWARD, &dset_bw, &dfire_bw, &sset_bw, &sfire_bw);
            
            /* Условие успешного завершения: все таймеры сработали + получили 95% пакетов */
            int timers_done = (dfire_fw >= dset_fw && dfire_bw >= dset_bw);
            int packets_received = (st.recv_a >= PKT_COUNT * 0.95 && st.recv_b >= PKT_COUNT * 0.95);
            
            if (timers_done && packets_received) {
                printf("  Completed successfully after %lu ms\n", (now - start_time) / 1000);
                break;
            }
            
            /* Watchdog: принудительный выход по таймауту */
            if ((now - watchdog_start) > WATCHDOG_US) {
                printf("  [WARN] Watchdog timeout after %lu ms (timers=%s, recv=%u/%u)\n",
                       (now - start_time) / 1000,
                       timers_done ? "done" : "pending",
                       st.recv_a, st.recv_b);
                break;
            }
        }
    }
    
    /* Получаем статистику dummynet */
    const struct dummynet_stats* stats_fw = dummynet_get_stats(st.dn, DUMMYNET_FORWARD);
    const struct dummynet_stats* stats_bw = dummynet_get_stats(st.dn, DUMMYNET_BACKWARD);
    
    /* Результаты с детальной диагностикой */
    int q_fw = dummynet_get_queue_size(st.dn, DUMMYNET_FORWARD);
    int q_bw = dummynet_get_queue_size(st.dn, DUMMYNET_BACKWARD);
    uint64_t dset_fw, dfire_fw, sset_fw, sfire_fw;
    uint64_t dset_bw, dfire_bw, sset_bw, sfire_bw;
    dummynet_get_debug_counters(st.dn, DUMMYNET_FORWARD, &dset_fw, &dfire_fw, &sset_fw, &sfire_fw);
    dummynet_get_debug_counters(st.dn, DUMMYNET_BACKWARD, &dset_bw, &dfire_bw, &sset_bw, &sfire_bw);
    
    printf("Results:\n");
    printf("  Forward (A->B):\n");
    printf("    Test sent:    %u\n", st.next_seq);
    printf("    Dummynet recv:%" PRIu64 "\n", stats_fw->recv);
    printf("    Delay timers: set=%" PRIu64 ", fire=%" PRIu64 "\n", dset_fw, dfire_fw);
    printf("    Dummynet sent:%" PRIu64 "\n", stats_fw->sent);
    printf("    Shaper timers: set=%" PRIu64 ", fire=%" PRIu64 "\n", sset_fw, sfire_fw);
    printf("    Test recv:    %u\n", st.recv_b);
    printf("    Queue size:   %d\n", q_fw);
    printf("    Dropped:      %" PRIu64 ", Lost: %" PRIu64 "\n", stats_fw->dropped, stats_fw->lost);
    printf("  Backward (B->A):\n");
    printf("    Test sent:    %u\n", st.next_seq);
    printf("    Dummynet recv:%" PRIu64 "\n", stats_bw->recv);
    printf("    Delay timers: set=%" PRIu64 ", fire=%" PRIu64 "\n", dset_bw, dfire_bw);
    printf("    Dummynet sent:%" PRIu64 "\n", stats_bw->sent);
    printf("    Shaper timers: set=%" PRIu64 ", fire=%" PRIu64 "\n", sset_bw, sfire_bw);
    printf("    Test recv:    %u\n", st.recv_a);
    printf("    Queue size:   %d\n", q_bw);
    printf("    Dropped:      %" PRIu64 ", Lost: %" PRIu64 "\n", stats_bw->dropped, stats_bw->lost);
    
    if (st.latency_count_b > 0) {
        printf("  Latency A->B: avg=%.2f ms\n", 
               (double)st.latency_sum_b / st.latency_count_b / 1000.0);
    }
    if (st.latency_count_a > 0) {
        printf("  Latency B->A: avg=%.2f ms\n",
               (double)st.latency_sum_a / st.latency_count_a / 1000.0);
    }
    
    /* Проверка */
    int passed = 1;
    
    if (strcmp(name, "Baseline") == 0) {
        /* Должны получить почти все */
        if (st.recv_b < PKT_COUNT * 0.95 || st.recv_a < PKT_COUNT * 0.95) {
            printf("  [FAIL] Expected >95%% packets, got A=%u, B=%u\n", st.recv_a, st.recv_b);
            passed = 0;
        } else {
            printf("  [PASS] >95%% packets received\n");
        }
    } 
    else if (strcmp(name, "Queue Overflow") == 0) {
        /* Должны быть дропы */
        if (stats_fw->dropped < 100 && stats_bw->dropped < 100) {
            printf("  [FAIL] Expected drops, got %" PRIu64 "/%" PRIu64 "\n",
                   stats_fw->dropped, stats_bw->dropped);
            passed = 0;
        } else {
            printf("  [PASS] Queue overflow detected: %" PRIu64 "/%" PRIu64 " dropped\n",
                   stats_fw->dropped, stats_bw->dropped);
        }
    }
    else if (strcmp(name, "Packet Loss") == 0) {
        /* Должны быть потери ~5% */
        uint32_t lost_b = PKT_COUNT - st.recv_b;
        uint32_t lost_a = PKT_COUNT - st.recv_a;
        double loss_b = 100.0 * lost_b / PKT_COUNT;
        double loss_a = 100.0 * lost_a / PKT_COUNT;
        
        if (loss_b < 2.0 || loss_b > 10.0 || loss_a < 2.0 || loss_a > 10.0) {
            printf("  [FAIL] Expected ~5%% loss, got %.1f%%/%.1f%%\n", loss_a, loss_b);
            passed = 0;
        } else {
            printf("  [PASS] Loss rate: %.1f%%/%.1f%% (expected ~5%%)\n", loss_a, loss_b);
        }
    }
    else if (strcmp(name, "Delay") == 0) {
        /* Проверяем задержку */
        if (st.latency_count_b > 0 && st.latency_count_a > 0) {
            double avg_lat_b = (double)st.latency_sum_b / st.latency_count_b / 1000.0;
            double avg_lat_a = (double)st.latency_sum_a / st.latency_count_a / 1000.0;
            
            /* Ожидаем ~60-80ms (50 + 0-20) */
            if (avg_lat_b >= 45.0 && avg_lat_b <= 85.0 &&
                avg_lat_a >= 45.0 && avg_lat_a <= 85.0) {
                printf("  [PASS] Latency in expected range: %.1f/%.1f ms\n", avg_lat_a, avg_lat_b);
            } else {
                printf("  [FAIL] Latency out of range: %.1f/%.1f ms (expected 45-85ms)\n",
                       avg_lat_a, avg_lat_b);
                passed = 0;
            }
        } else {
            printf("  [FAIL] No packets received\n");
            passed = 0;
        }
    }
    else if (strcmp(name, "Stress") == 0) {
        /* В стрессе должно быть что-то дропнуто или потеряно */
        uint64_t total_dropped = stats_fw->dropped + stats_bw->dropped;
        uint64_t total_lost = stats_fw->lost + stats_bw->lost;
        
        if (total_dropped == 0 && total_lost == 0) {
            printf("  [FAIL] Expected drops or losses in stress test\n");
            passed = 0;
        } else {
            printf("  [PASS] Stress applied: %" PRIu64 " dropped, %" PRIu64 " lost\n",
                   total_dropped, total_lost);
        }
    }
    
    /* Очистка */
    dummynet_destroy(st.dn);
    uasync_destroy(st.ua, 1);
    
    return passed;
}

int main(void) {
    printf("========================================\n");
    printf("Dummynet Comprehensive Test Suite\n");
    printf("(Single-threaded, uasync only)\n");
    printf("========================================\n");
    
    srand((unsigned int)time(NULL));
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    socket_platform_init();
    
    int passed = 0;
    int failed = 0;
    
    /* Сценарий 1: Базовый (минимальная задержка, без потерь) */
    if (run_scenario("Baseline", 5, 2, 0, 1000, 0)) {
        passed++;
    } else {
        failed++;
    }
    
    /* Сценарий 2: Проверка задержки */
    if (run_scenario("Delay", 50, 20, 0, 1000, 0)) {
        passed++;
    } else {
        failed++;
    }
    
    /* Сценарий 3: Переполнение очереди */
    if (run_scenario("Queue Overflow", 100, 0, 100, 10, 0)) {
        passed++;
    } else {
        failed++;
    }
    
    /* Сценарий 4: Потери пакетов */
    if (run_scenario("Packet Loss", 5, 0, 0, 1000, 50)) {
        passed++;
    } else {
        failed++;
    }
    
    /* Сценарий 5: Стресс (все вместе) */
    if (run_scenario("Stress", 50, 30, 1000, 50, 10)) {
        passed++;
    } else {
        failed++;
    }
    
    printf("\n========================================\n");
    printf("Final Results: %d passed, %d failed\n", passed, failed);
    printf("========================================\n");
    
    socket_platform_cleanup();
    return failed > 0 ? 1 : 0;
}
