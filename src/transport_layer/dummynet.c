/**
 * @file dummynet.c
 * @brief Реализация UDP dummynet эмулятора
 */

#include "dummynet.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/memory_pool.h"
#include "../lib/socket_compat.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"
#include "etcp_connections.h"
#include "etcp.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include "../lib/mem.h"

/* Timebase: 0.1 мс */
#define MS_TO_TB(ms) ((ms) * 10)

/* Debug category */
#define DEBUG_CATEGORY_DUMMYNET DEBUG_CATEGORY_ETCP

/* Forward declarations */
static void dummynet_read_callback(socket_t sock, void* user_arg);
static void dummynet_delay_callback(void* user_arg);
static void dummynet_shaper_callback(void* user_arg);
static void dummynet_freeze_callback(void* user_arg);

/**
 * @brief Пакет в обработке (внутренняя структура)
 */
struct dummynet_pkt {
    uint8_t data[DUMMYNET_MAX_PKT_SIZE];
    size_t len;
    uint64_t recv_time_tb;
    uint32_t delay_ms;
    int direction;
};

/**
 * @brief Направление передачи
 */
struct dummynet_dir {
    struct dummynet* owner;
    int index;
    enum dummynet_mode mode;
    uint32_t delay_fixed_ms;
    uint32_t delay_random_ms;
    uint32_t bandwidth_kbps;
    uint32_t max_queue_pkts;
    uint32_t loss_permille;
    struct sockaddr_storage dest_addr;
    socklen_t dest_len;
    int configured;

    struct ll_queue* queue;

    void* shaper_timer;
    uint64_t tx_finish_ns;        // абсолютный срок; дробные миллисекунды не теряются между пакетами
    int tx_started;

    /* Отладочные счетчики */
    uint64_t delay_timer_set;    /* Число установок delay таймеров */
    uint64_t delay_timer_fire;   /* Число срабатываний delay таймеров */
    uint64_t shaper_timer_set;   /* Число установок shaper таймеров */
    uint64_t shaper_timer_fire;  /* Число срабатываний shaper таймеров */

    struct dummynet_stats stats;
};

/**
 * @brief Контекст dummynet
 */
struct dummynet {
    struct UASYNC* ua;
    socket_t sock;
    int socket_registered;
    uint16_t listen_port;

    struct dummynet_dir dirs[DUMMYNET_DIR_COUNT];
    struct memory_pool* pkt_pool;
    struct delay_cb_arg* delays;  // владение аргументами/пакетами всех ожидающих delay-таймеров
    struct dummynet_freeze_spec freezes[DUMMYNET_MAX_FREEZE_TYPES];
    struct dummynet_freeze_stats freeze_stats;
    double freeze_rate;          // Сумма частот независимых источников
    uint64_t freeze_rng;         // Отдельный PRNG: трафик не меняет рисунок помех
    uint64_t freeze_next_us;     // Следующее начало, в том числе во время текущей паузы
    uint64_t freeze_timer_us;    // Срок активного таймера для контроля опоздания
    void* freeze_timer;          // Один таймер на ближайшее начало/конец паузы
};

/**
 * @brief Аргумент для delay callback
 */
struct delay_cb_arg {
    struct dummynet* dn;
    struct ll_entry* entry;
    void* timer;
    struct delay_cb_arg *next, *prev;
};

/* Генерация случайного числа 0..max */
static inline uint32_t random_range(uint32_t max) {
    if (max == 0) return 0;
    return (uint32_t)rand() % (max + 1);
}

/* Источник одного направления — endpoint назначения обратного направления. */
static int dummynet_get_direction(struct dummynet* dn, const struct sockaddr_storage* source) {
    if (source->ss_family != AF_INET) return -1;
    const struct sockaddr_in* src = (const struct sockaddr_in*)source;
    for (int i = 0; i < DUMMYNET_DIR_COUNT; i++) {
        struct dummynet_dir* reverse = &dn->dirs[1 - i];
        const struct sockaddr_in* peer = (const struct sockaddr_in*)&reverse->dest_addr;
        if (reverse->configured && peer->sin_family == AF_INET &&
            peer->sin_addr.s_addr == src->sin_addr.s_addr && peer->sin_port == src->sin_port) return i;
    }
    return -1;
}

/* Отправляет пакет и планирует следующую отправку */
static void dummynet_process_queue(struct dummynet* dn, int dir_idx) {
    struct dummynet_dir* dir = &dn->dirs[dir_idx];

    if (dir->mode != DUMMYNET_RUN || dir->shaper_timer || dn->freeze_stats.active_since_us) return;
    uint64_t now_ns = get_time_us() * 1000;
    if (!dir->tx_started) dir->tx_finish_ns = now_ns;
    while (dir->queue->head) {
        struct dummynet_pkt* pkt = (struct dummynet_pkt*)dir->queue->head->data;
        if (!dir->tx_started) {
            if (dir->bandwidth_kbps) dir->tx_finish_ns += ((uint64_t)pkt->len * 8000000 + dir->bandwidth_kbps - 1) /
                dir->bandwidth_kbps;
            dir->tx_started = 1;
        }
        if (dir->tx_finish_ns > now_ns) {
            uint64_t wait_tb = (dir->tx_finish_ns - now_ns + 99999) / 100000;
            if (wait_tb > INT_MAX) wait_tb = INT_MAX;
            dir->shaper_timer = uasync_set_timeout(dn->ua, (int)wait_tb, dir, dummynet_shaper_callback, "dummynet_shaper");
            if (!dir->shaper_timer) {
                DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Dir %d: shaper timer failed bytes=%zu queued=%u wait_tb=%llu; dropping head",
                            dir_idx, pkt->len, dir->stats.queue_size, (unsigned long long)wait_tb);
                struct ll_entry* failed = queue_data_get(dir->queue);
                queue_entry_free(failed); dir->stats.dropped++; dir->stats.queue_size--;
                dir->tx_started = 0; dir->tx_finish_ns = now_ns;
                continue;
            }
            dir->shaper_timer_set++;
            return;
        }
        struct ll_entry* entry = queue_data_get(dir->queue);
        ssize_t n = socket_sendto(dn->sock, pkt->data, pkt->len, (struct sockaddr*)&dir->dest_addr, dir->dest_len);
        if (n != (ssize_t)pkt->len) {
            DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Dir %d: sendto bytes=%zd expected=%zu error=%s", dir_idx, n,
                pkt->len, socket_strerror(socket_get_error()));
            dir->stats.dropped++;
        } else {
            dir->stats.sent++; dir->stats.sent_bytes += n;
            uint64_t elapsed_us = (get_time_tb() - pkt->recv_time_tb) * 100;
            uint64_t propagation_us = (uint64_t)pkt->delay_ms * 1000;
            uint64_t queued_us = elapsed_us > propagation_us ? elapsed_us - propagation_us : 0;
            if (queued_us > dir->stats.max_queue_delay_us) dir->stats.max_queue_delay_us = queued_us;
        }
        dir->stats.queue_size--;
        queue_entry_free(entry);
        dir->tx_started = 0;
    }
    dir->tx_finish_ns = 0;
}

/* Callback шейпера */
static void dummynet_shaper_callback(void* user_arg) {
    struct dummynet_dir* dir = user_arg;
    dir->shaper_timer = NULL;
    dir->shaper_timer_fire++;
    dummynet_process_queue(dir->owner, dir->index);
}

/* SplitMix64: одинаковый seed даёт одинаковые выборки независимо от rand() потерь и задержки. */
static uint64_t dummynet_freeze_random(uint64_t* state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

/* Экспоненциальный интервал; U строго внутри (0,1), чтобы log и целочисленный срок были конечны. */
static uint64_t dummynet_freeze_gap(uint64_t* state, double rate) {
    double u = ((double)(dummynet_freeze_random(state) >> 12) + 0.5) / 4503599627370496.0;
    return (uint64_t)ceil(-log(u) * 1000000.0 / rate);
}

/* Учитывает конец паузы без отправки: caller сначала обрабатывает начало следующей, затем запускает очереди. */
static void dummynet_freeze_resume(struct dummynet* dn, uint64_t now) {
    struct dummynet_freeze_stats* s = &dn->freeze_stats;
    if (!s->active_since_us) return;
    uint64_t paused = now - s->active_since_us;
    s->paused_us += paused;
    DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "DUMMYNET freeze end port=%u start_us=%llu end_us=%llu paused_us=%llu queue=%u/%u",
                dn->listen_port, (unsigned long long)s->active_since_us, (unsigned long long)now,
                (unsigned long long)paused, dn->dirs[0].stats.queue_size, dn->dirs[1].stats.queue_size);
    s->active_since_us = s->until_us = 0;
    for (int i = 0; i < DUMMYNET_DIR_COUNT; i++) {
        if (dn->dirs[i].tx_started) dn->dirs[i].tx_finish_ns += paused * 1000;
    }
}

/* Общая Wi-Fi помеха: оба сериализатора останавливаются, поступления и delay-таймеры продолжают работать. */
static void dummynet_freeze_callback(void* user_arg) {
    struct dummynet* dn = user_arg;
    struct dummynet_freeze_stats* s = &dn->freeze_stats;
    uint64_t now = get_time_us();
    dn->freeze_timer = NULL;
    uint64_t late = now > dn->freeze_timer_us ? now - dn->freeze_timer_us : 0;
    if (late > s->timer_late_max_us) s->timer_late_max_us = late;
    if (s->active_since_us && now >= s->until_us) dummynet_freeze_resume(dn, now);
    if (now >= dn->freeze_next_us) {
        double pick = (double)(dummynet_freeze_random(&dn->freeze_rng) >> 11) / 9007199254740992.0 * dn->freeze_rate;
        unsigned type = 0;
        while (type + 1 < s->type_count && pick >= dn->freezes[type].events_per_second)
            pick -= dn->freezes[type++].events_per_second;
        uint32_t duration = dummynet_freeze_random(&dn->freeze_rng) % (dn->freezes[type].max_duration_ms + 1);
        s->events[type]++;
        if (duration) {
            if (s->active_since_us) s->overlaps++;
            else {
                s->active_since_us = now;
                for (int i = 0; i < DUMMYNET_DIR_COUNT; i++) {
                    struct dummynet_dir* dir = &dn->dirs[i];
                    if (dir->shaper_timer) { uasync_cancel_timeout(dn->ua, dir->shaper_timer); dir->shaper_timer = NULL; }
                    // Уже просроченная отправка не превращается в кредит для залпа после помехи.
                    if (dir->tx_started && dir->tx_finish_ns < now * 1000) dir->tx_finish_ns = now * 1000;
                }
            }
            if (now + duration * 1000ULL > s->until_us) s->until_us = now + duration * 1000ULL;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "DUMMYNET freeze event port=%u type=%u time_us=%llu duration_ms=%u until_us=%llu "
                    "events=%llu overlaps=%llu late_us=%llu queue=%u/%u", dn->listen_port, type, (unsigned long long)now,
                    duration, (unsigned long long)s->until_us, (unsigned long long)s->events[type], (unsigned long long)s->overlaps,
                    (unsigned long long)late, dn->dirs[0].stats.queue_size, dn->dirs[1].stats.queue_size);
        dn->freeze_next_us = now + dummynet_freeze_gap(&dn->freeze_rng, dn->freeze_rate);
    }
    uint64_t due = dn->freeze_next_us;
    if (s->active_since_us && s->until_us < due) due = s->until_us;
    uint64_t wait_tb = due > now ? (due - now + 99) / 100 : 1;
    if (wait_tb > INT_MAX) wait_tb = INT_MAX;
    dn->freeze_timer_us = now + wait_tb * 100;
    dn->freeze_timer = uasync_set_timeout(dn->ua, (int)wait_tb, dn, dummynet_freeze_callback, "dummynet_freeze");
    if (!dn->freeze_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Dummynet port=%u: freeze timer failed, disabling interference queued=%u/%u",
                    dn->listen_port, dn->dirs[0].stats.queue_size, dn->dirs[1].stats.queue_size);
        s->type_count = 0; dn->freeze_rate = 0; dn->freeze_next_us = 0;
        dummynet_freeze_resume(dn, now);
    }
    for (int i = 0; i < DUMMYNET_DIR_COUNT; i++) dummynet_process_queue(dn, i);
}

/* Ошибки конфигурации/первого таймера не затрагивают работающие помехи и начатую сериализацию. */
int dummynet_set_microfreezes(struct dummynet* dn, const struct dummynet_freeze_spec* specs, unsigned count, uint32_t seed) {
    if (!dn || count > DUMMYNET_MAX_FREEZE_TYPES || (count && !specs)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Invalid microfreeze configuration context=%p specs=%p count=%u",
                    (void*)dn, (void*)specs, count); return -1;
    }
    double rate = 0;
    for (unsigned i = 0; i < count; i++) {
        rate += specs[i].events_per_second;
        if (!isfinite(rate) || specs[i].events_per_second < 0.001 || rate > 1000 ||
            !specs[i].max_duration_ms || specs[i].max_duration_ms > DUMMYNET_MAX_DELAY_MS) {
            DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Invalid freeze type=%u rate=%g/s max_ms=%u total_rate=%g/s",
                        i, specs[i].events_per_second, specs[i].max_duration_ms, rate); return -1;
        }
    }
    uint64_t rng = seed, now = get_time_us(), gap = count ? dummynet_freeze_gap(&rng, rate) : 0;
    uint64_t wait_tb = (gap + 99) / 100;
    if (wait_tb > INT_MAX) wait_tb = INT_MAX;
    void* timer = count ? uasync_set_timeout(dn->ua, (int)wait_tb, dn, dummynet_freeze_callback, "dummynet_freeze") : NULL;
    if (count && !timer) { DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Cannot start microfreeze timer port=%u", dn->listen_port); return -1; }
    if (dn->freeze_timer) uasync_cancel_timeout(dn->ua, dn->freeze_timer);
    dummynet_freeze_resume(dn, now);
    memset(&dn->freeze_stats, 0, sizeof(dn->freeze_stats));
    dn->freeze_stats.type_count = count;
    if (count) memcpy(dn->freezes, specs, count * sizeof(*specs));
    dn->freeze_rng = rng; dn->freeze_rate = rate; dn->freeze_timer = timer;
    dn->freeze_next_us = count ? now + gap : 0; dn->freeze_timer_us = count ? now + wait_tb * 100 : 0;
    DEBUG_INFO(DEBUG_CATEGORY_DUMMYNET, "Dummynet port=%u: microfreezes types=%u rate=%g/s seed=%u shared_directions=1",
               dn->listen_port, count, rate, seed);
    for (unsigned i = 0; i < count; i++)
        DEBUG_INFO(DEBUG_CATEGORY_DUMMYNET, "Dummynet freeze type=%u rate=%g/s duration_ms=0..%u",
                   i, specs[i].events_per_second, specs[i].max_duration_ms);
    for (int i = 0; i < DUMMYNET_DIR_COUNT; i++) dummynet_process_queue(dn, i);
    return 0;
}

int dummynet_get_freeze_stats(struct dummynet* dn, struct dummynet_freeze_stats* out) {
    if (!dn || !out) { DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Invalid freeze statistics context=%p out=%p", (void*)dn, (void*)out); return -1; }
    *out = dn->freeze_stats;
    if (out->active_since_us) out->paused_us += get_time_us() - out->active_since_us;
    return 0;
}

/* Применяет заданные потери/лимит/задержку к одной целиком принятой датаграмме. */
static void dummynet_receive(struct dummynet* dn, const uint8_t* buf, size_t n, const struct sockaddr_storage* src_addr) {
    int dir_idx = dummynet_get_direction(dn, src_addr);
    if (dir_idx < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Packet from unknown endpoint %s, expected %s or %s",
                   sockaddr_storage_to_str(src_addr).str, sockaddr_storage_to_str(&dn->dirs[DUMMYNET_BACKWARD].dest_addr).str,
                   sockaddr_storage_to_str(&dn->dirs[DUMMYNET_FORWARD].dest_addr).str);
        return;
    }

    struct dummynet_dir* dir = &dn->dirs[dir_idx];
    if (!dir->configured) {
        DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Direction %d not configured, dropping packet", dir_idx);
        return;
    }

    dir->stats.recv++;
    dir->stats.recv_bytes += n;
    if (n > DUMMYNET_MAX_PKT_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Dir %d: oversized UDP packet bytes=%zu limit=%d source=%s",
                   dir_idx, n, DUMMYNET_MAX_PKT_SIZE, sockaddr_storage_to_str(src_addr).str);
        dir->stats.dropped++; return;
    }
    if (dir->mode == DUMMYNET_DROP) {
        dir->stats.lost++; dir->stats.blocked++;
        return;
    }

    /* Random loss check */
    if (dir->loss_permille > 0) {
        uint32_t roll = (uint32_t)rand() % 1000;
        if (roll < dir->loss_permille) {
            dir->stats.lost++;
            DEBUG_DEBUG(DEBUG_CATEGORY_DUMMYNET, "Packet lost randomly (roll=%u, threshold=%u)",
                       roll, dir->loss_permille);
            return;
        }
    }

    if (dir->stats.queue_size >= dir->max_queue_pkts) {
        dir->stats.dropped++;
        DEBUG_DEBUG(DEBUG_CATEGORY_DUMMYNET, "Dir %d: queue full size=%u limit=%u bytes=%zu",
                    dir_idx, dir->stats.queue_size, dir->max_queue_pkts, n);
        return;
    }

    /* Выделяем пакет */
    struct ll_entry* entry = queue_entry_new_from_pool(dn->pkt_pool);
    if (!entry) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Failed to allocate packet entry");
        dir->stats.dropped++;
        return;
    }

    struct dummynet_pkt* pkt = (struct dummynet_pkt*)entry->data;
    pkt->len = n;
    memcpy(pkt->data, buf, n);
    pkt->recv_time_tb = get_time_tb();
    pkt->direction = dir_idx;

    /* Вычисляем задержку */
    uint32_t delay_ms = dir->delay_fixed_ms + random_range(dir->delay_random_ms);
    int delay_tb = MS_TO_TB(delay_ms);
    pkt->delay_ms = delay_ms;

    DEBUG_DEBUG(DEBUG_CATEGORY_DUMMYNET, "Dir %d: packet recv (%zu bytes), delay %u ms",
                dir_idx, n, delay_ms);

    /* Создаём таймер задержки */
    struct delay_cb_arg* targ = (struct delay_cb_arg*)u_malloc(sizeof(struct delay_cb_arg));
    if (!targ) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Failed to allocate timer arg");
        dir->stats.dropped++;
        queue_entry_free(entry);
        return;
    }
    targ->dn = dn;
    targ->entry = entry;
    targ->prev = NULL; targ->next = dn->delays;
    if (dn->delays) dn->delays->prev = targ;
    dn->delays = targ;

    targ->timer = uasync_set_timeout(dn->ua, delay_tb, targ, dummynet_delay_callback, "dummynet_delay");
    if (!targ->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Failed to set delay timer");
        dir->stats.dropped++;
        dn->delays = targ->next;
        if (targ->next) targ->next->prev = NULL;
        u_free(targ);
        queue_entry_free(entry);
        return;
    }
    dir->delay_timer_set++;
    dir->stats.queue_size++;
    if (dir->stats.queue_size > dir->stats.queue_max) dir->stats.queue_max = dir->stats.queue_size;
}

/* Ограниченная пачка осушает UDP backlog без монополизации потока таймеров/других сокетов.
 * Одно чтение за poll создавало скрытый предел PPS и потери в kernel до учёта эмулятором. */
static void dummynet_read_callback(socket_t sock, void* user_arg) {
    struct dummynet* dn = user_arg;
    uint8_t buf[65536]; /* Принимаем всю датаграмму; превышение лимита разбирается без усечения. */
    unsigned count=0;
    for (;count<64;count++) {
        struct sockaddr_storage src_addr;socklen_t src_len=sizeof(src_addr);
        ssize_t n=socket_recvfrom(sock,buf,sizeof(buf),(struct sockaddr*)&src_addr,&src_len);
        if (n<0) {
            int error=socket_get_error();
            if (error!=ERR_WOULDBLOCK && error!=ERR_AGAIN)
                DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET,"recvfrom failed after %u packets: %s",count,socket_strerror(error));
            break;
        }
        dummynet_receive(dn,buf,(size_t)n,&src_addr);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DUMMYNET,"UDP receive batch port=%u packets=%u budget_exhausted=%d",dn->listen_port,count,count==64);
}

/* Callback таймера задержки */
static void dummynet_delay_callback(void* user_arg) {
    struct delay_cb_arg* arg = (struct delay_cb_arg*)user_arg;
    struct dummynet* dn = arg->dn;
    struct ll_entry* entry = arg->entry;
    if (arg->prev) arg->prev->next = arg->next;
    else dn->delays = arg->next;
    if (arg->next) arg->next->prev = arg->prev;
    u_free(arg);

    struct dummynet_pkt* pkt = (struct dummynet_pkt*)entry->data;
    int dir_idx = pkt->direction;
    struct dummynet_dir* dir = &dn->dirs[dir_idx];
    dir->delay_timer_fire++;

    /* Добавляем в очередь */
    // При провале queue_data_put сам освобождает entry, повторный free дал бы double-free.
    if (queue_data_put(dir->queue, entry) != 0) {
        dir->stats.dropped++;
        dir->stats.queue_size--;
        DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Dir %d: queue_data_put failed", dir_idx);
        return;
    }

    dummynet_process_queue(dn, dir_idx);
}

/* Создаёт dummynet контекст */
struct dummynet* dummynet_create(struct UASYNC* ua, const char* bind_ip, uint16_t listen_port) {
    if (!ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Invalid parameters");
        return NULL;
    }

    struct dummynet* dn = (struct dummynet*)u_calloc(1, sizeof(struct dummynet));
    if (!dn) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Failed to allocate dummynet context");
        return NULL;
    }

    dn->ua = ua;
    dn->sock = SOCKET_INVALID;
    dn->listen_port = listen_port;

    /* Создаём пул для пакетов */
    dn->pkt_pool = memory_pool_init(sizeof(struct ll_entry) + sizeof(struct dummynet_pkt), "pkt_pool");
    if (!dn->pkt_pool) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Failed to create packet pool");
        u_free(dn);
        return NULL;
    }

    /* Создаём очереди для направлений */
    for (int i = 0; i < DUMMYNET_DIR_COUNT; i++) {
        dn->dirs[i].owner = dn; dn->dirs[i].index = i;
        dn->dirs[i].queue = queue_new(ua, 0, 0, 0, i ? "dummynet-backward" : "dummynet-forward");
        if (!dn->dirs[i].queue) {
            DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Failed to create queue for direction %d", i);
            dummynet_destroy(dn);
            return NULL;
        }
    }

    /* Создаём UDP сокет */
    dn->sock = socket_create_udp(AF_INET);
    if (dn->sock == SOCKET_INVALID) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Failed to create UDP socket");
        dummynet_destroy(dn);
        return NULL;
    }

    if (socket_set_nonblocking(dn->sock) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Failed to set nonblocking");
        dummynet_destroy(dn);
        return NULL;
    }

    /* Привязываем сокет */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(listen_port);

    if (bind_ip && strcmp(bind_ip, "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, bind_ip, &bind_addr.sin_addr) != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Invalid bind IP: %s", bind_ip);
            dummynet_destroy(dn);
            return NULL;
        }
    } else {
        bind_addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(dn->sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Failed to bind to %s:%u: %s",
                   bind_ip ? bind_ip : "0.0.0.0", listen_port, socket_strerror(socket_get_error()));
        dummynet_destroy(dn);
        return NULL;
    }
    dn->listen_port = socket_get_bound_port(dn->sock);
    if (!dn->listen_port) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Cannot query bound dummynet UDP port"); dummynet_destroy(dn); return NULL;
    }

    /* Добавляем сокет в uasync */
    void* sock_id = uasync_add_socket_t(ua, dn->sock, dummynet_read_callback, NULL, NULL, "dummynet", dn);
    if (!sock_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Failed to add socket to uasync");
        dummynet_destroy(dn);
        return NULL;
    }
    dn->socket_registered = 1;

    DEBUG_INFO(DEBUG_CATEGORY_DUMMYNET, "Dummynet created on %s:%u; configure both peer endpoints before traffic",
               bind_ip ? bind_ip : "0.0.0.0", dn->listen_port);

    return dn;
}

/* Уничтожает dummynet контекст */
void dummynet_destroy(struct dummynet* dn) {
    if (!dn) return;

    if (dn->freeze_timer) uasync_cancel_timeout(dn->ua, dn->freeze_timer);

    DEBUG_INFO(DEBUG_CATEGORY_DUMMYNET, "Destroying dummynet port=%u queued=%u/%u sent=%llu/%llu dropped=%llu/%llu",
               dn->listen_port, dn->dirs[0].stats.queue_size, dn->dirs[1].stats.queue_size,
               (unsigned long long)dn->dirs[0].stats.sent, (unsigned long long)dn->dirs[1].stats.sent,
               (unsigned long long)dn->dirs[0].stats.dropped, (unsigned long long)dn->dirs[1].stats.dropped);

    if (dn->sock != SOCKET_INVALID) {
        if (dn->socket_registered) uasync_remove_socket_t(dn->ua, dn->sock);
        socket_close_wrapper(dn->sock);
    }

    for (int i = 0; i < DUMMYNET_DIR_COUNT; i++) {
        if (dn->dirs[i].shaper_timer) {
            uasync_cancel_timeout(dn->ua, dn->dirs[i].shaper_timer);
        }
        if (dn->dirs[i].queue) {
            struct ll_entry* entry;
            while ((entry = queue_data_get(dn->dirs[i].queue)) != NULL) {
                queue_entry_free(entry);
            }
            queue_free(dn->dirs[i].queue);
        }
    }

    while (dn->delays) {
        struct delay_cb_arg* arg = dn->delays;
        dn->delays = arg->next;
        uasync_cancel_timeout(dn->ua, arg->timer);
        queue_entry_free(arg->entry); u_free(arg);
    }

    if (dn->pkt_pool) {
        memory_pool_destroy(dn->pkt_pool);
    }

    u_free(dn);
}

/* Настраивает параметры направления */
int dummynet_set_direction(struct dummynet* dn, int direction,
                           uint32_t delay_fixed_ms, uint32_t delay_random_ms,
                           uint32_t bandwidth_kbps, uint32_t max_queue_pkts,
                           uint32_t loss_permille,
                           const char* dest_ip, uint16_t dest_port) {
    if (!dn || direction < 0 || direction >= DUMMYNET_DIR_COUNT) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Invalid direction: %d", direction);
        return -1;
    }

    if (!dest_ip || dest_port == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Invalid destination");
        return -1;
    }
    if ((uint64_t)delay_fixed_ms + delay_random_ms > DUMMYNET_MAX_DELAY_MS || max_queue_pkts > INT_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Dir %d: invalid delay=%u+%u ms (max=%d) or queue=%u (max=%d)",
                    direction, delay_fixed_ms, delay_random_ms, DUMMYNET_MAX_DELAY_MS, max_queue_pkts, INT_MAX);
        return -1;
    }

    struct sockaddr_storage destination = {0};
    struct sockaddr_in* dest_sin = (struct sockaddr_in*)&destination;
    dest_sin->sin_family = AF_INET; dest_sin->sin_port = htons(dest_port);
    if (inet_pton(AF_INET, dest_ip, &dest_sin->sin_addr) != 1 || dest_sin->sin_addr.s_addr == INADDR_ANY) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Dir %d: invalid destination IPv4=%s port=%u", direction, dest_ip, dest_port); return -1;
    }
    struct dummynet_dir* dir = &dn->dirs[direction];
    struct dummynet_dir* reverse = &dn->dirs[1-direction];
    const struct sockaddr_in* opposite = (const struct sockaddr_in*)&reverse->dest_addr;
    if (reverse->configured && dest_sin->sin_addr.s_addr == opposite->sin_addr.s_addr && dest_sin->sin_port == opposite->sin_port) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Dir %d: endpoints must differ, destination=%s:%u", direction, dest_ip, dest_port); return -1;
    }
    const struct sockaddr_in* previous = (const struct sockaddr_in*)&dir->dest_addr;
    if ((!dir->configured || previous->sin_addr.s_addr != dest_sin->sin_addr.s_addr || previous->sin_port != dest_sin->sin_port) &&
        (dir->stats.queue_size || reverse->stats.queue_size)) {
        DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Dir %d: endpoint change rejected while queued=%u/%u destination=%s:%u",
                   direction, dir->stats.queue_size, reverse->stats.queue_size, dest_ip, dest_port); return -1;
    }

    if (loss_permille > DUMMYNET_MAX_LOSS_PERMILLE) {
        DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Loss permille %u clamped to %d",
                   loss_permille, DUMMYNET_MAX_LOSS_PERMILLE);
        loss_permille = DUMMYNET_MAX_LOSS_PERMILLE;
    }

    dir->delay_fixed_ms = delay_fixed_ms;
    dir->delay_random_ms = delay_random_ms;
    dir->bandwidth_kbps = bandwidth_kbps;
    dir->max_queue_pkts = max_queue_pkts;
    dir->loss_permille = loss_permille;

    dir->dest_addr = destination;
    dir->dest_len = sizeof(struct sockaddr_in);
    dir->configured = 1;

    DEBUG_INFO(DEBUG_CATEGORY_DUMMYNET, "Direction %d configured: delay=%u+%u ms, bw=%u kbps, "
               "queue=%u pkts, loss=%u.%u%%, dest=%s:%u",
               direction, delay_fixed_ms, delay_random_ms, bandwidth_kbps,
               max_queue_pkts, loss_permille / 10, loss_permille % 10,
               dest_ip, dest_port);

    return 0;
}

/* HOLD сохраняет пакеты; DROP освобождает также delay-пакеты, не оставляя callbacks на удалённые записи. */
int dummynet_set_mode(struct dummynet* dn, int direction, enum dummynet_mode mode) {
    if (!dn || direction < 0 || direction >= DUMMYNET_DIR_COUNT || mode < DUMMYNET_RUN || mode > DUMMYNET_HOLD) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Invalid mode: direction=%d mode=%d", direction, mode);
        return -1;
    }
    struct dummynet_dir* dir = &dn->dirs[direction];
    if (dir->mode == mode) return 0;
    DEBUG_INFO(DEBUG_CATEGORY_DUMMYNET, "Dir %d: mode %d -> %d queued=%u", direction, dir->mode, mode, dir->stats.queue_size);
    dir->mode = mode;
    if (dir->shaper_timer) { uasync_cancel_timeout(dn->ua, dir->shaper_timer); dir->shaper_timer = NULL; }
    dir->tx_started = 0; dir->tx_finish_ns = 0;
    if (mode == DUMMYNET_DROP) {
        struct ll_entry* entry;
        while ((entry = queue_data_get(dir->queue))) {
            queue_entry_free(entry); dir->stats.lost++; dir->stats.blocked++; dir->stats.queue_size--;
        }
        struct delay_cb_arg* arg = dn->delays;
        while (arg) {
            struct delay_cb_arg* next = arg->next;
            if (((struct dummynet_pkt*)arg->entry->data)->direction == direction) {
                if (arg->prev) arg->prev->next = arg->next;
                else dn->delays = arg->next;
                if (arg->next) arg->next->prev = arg->prev;
                uasync_cancel_timeout(dn->ua, arg->timer);
                queue_entry_free(arg->entry); u_free(arg);
                dir->stats.lost++; dir->stats.blocked++; dir->stats.queue_size--;
            }
            arg = next;
        }
    }
    if (mode == DUMMYNET_RUN) dummynet_process_queue(dn, direction);
    return 0;
}

/* Получает статистику направления */
const struct dummynet_stats* dummynet_get_stats(struct dummynet* dn, int direction) {
    if (!dn || direction < 0 || direction >= DUMMYNET_DIR_COUNT) {
        return NULL;
    }
    return &dn->dirs[direction].stats;
}

/* Сбрасывает статистику */
void dummynet_reset_stats(struct dummynet* dn, int direction) {
    if (!dn || direction < -1 || direction >= DUMMYNET_DIR_COUNT) {
        DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Invalid statistics reset direction=%d context=%p", direction, (void*)dn); return;
    }

    if (direction < 0) {
        for (int i = 0; i < DUMMYNET_DIR_COUNT; i++) {
            uint32_t queued = dn->dirs[i].stats.queue_size;
            memset(&dn->dirs[i].stats, 0, sizeof(struct dummynet_stats));
            dn->dirs[i].stats.queue_size = dn->dirs[i].stats.queue_max = queued;
        }
    } else if (direction >= 0 && direction < DUMMYNET_DIR_COUNT) {
        uint32_t queued = dn->dirs[direction].stats.queue_size;
        memset(&dn->dirs[direction].stats, 0, sizeof(struct dummynet_stats));
        dn->dirs[direction].stats.queue_size = dn->dirs[direction].stats.queue_max = queued;
    }
}

/* Возвращает файловый дескриптор сокета */
socket_t dummynet_get_socket(struct dummynet* dn) {
    if (!dn) return SOCKET_INVALID;
    return dn->sock;
}

/* Возвращает listen_port */
uint16_t dummynet_get_listen_port(struct dummynet *dn) {
    if (!dn) return 0;
    return dn->listen_port;
}

/* Возвращает указатель на UASYNC */
struct UASYNC *dummynet_get_uasync(struct dummynet *dn) {
    if (!dn) return NULL;
    return dn->ua;
}

/* Возвращает размер очереди направления */
int dummynet_get_queue_size(struct dummynet *dn, int direction) {
    if (!dn || direction < 0 || direction >= DUMMYNET_DIR_COUNT) {
        return -1;
    }
    return (int)dn->dirs[direction].stats.queue_size;
}

/* Получает отладочные счетчики направления */
void dummynet_get_debug_counters(struct dummynet *dn, int direction,
                                  uint64_t *delay_set, uint64_t *delay_fire,
                                  uint64_t *shaper_set, uint64_t *shaper_fire) {
    struct dummynet_dir* dir = dn && direction >= 0 && direction < DUMMYNET_DIR_COUNT ? &dn->dirs[direction] : NULL;
    if (!dir) DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Invalid timer counter query direction=%d context=%p", direction, (void*)dn);
    if (delay_set) *delay_set = dir ? dir->delay_timer_set : 0;
    if (delay_fire) *delay_fire = dir ? dir->delay_timer_fire : 0;
    if (shaper_set) *shaper_set = dir ? dir->shaper_timer_set : 0;
    if (shaper_fire) *shaper_fire = dir ? dir->shaper_timer_fire : 0;
}

// === Inline filter: send_hook для etcp_udp_send ===

struct dummynet_filter {
    struct UASYNC* ua;
    uint32_t loss_permille;
    struct ETCP_LINK* link;
    struct dummynet_stats stats;
    struct {
        uint32_t ip;
        uint16_t port;
    } block_addrs[4];
    uint8_t block_count;
};

static ssize_t dummynet_filter_send_hook(socket_t fd, const void* buf, size_t len,
                                          const struct sockaddr* addr, socklen_t addr_len,
                                          struct ETCP_LINK* link, void* ctx) {
    struct dummynet_filter* df = (struct dummynet_filter*)ctx;
    (void)link;
    df->stats.recv++;
    df->stats.recv_bytes += len;
    if (addr && addr->sa_family == AF_INET && df->block_count > 0) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)addr;
        for (uint8_t i = 0; i < df->block_count; i++) {
            if (sin->sin_addr.s_addr == df->block_addrs[i].ip &&
                sin->sin_port == df->block_addrs[i].port) {
                df->stats.lost++; df->stats.blocked++;
                return (ssize_t)len;
            }
        }
    }
    if (df->loss_permille > 0) {
        uint32_t roll = (uint32_t)rand() % 1000;
        if (roll < df->loss_permille) {
            df->stats.lost++;
            return (ssize_t)len;
        }
    }
    ssize_t sent = socket_sendto(fd, buf, len, addr, addr_len);
    if (sent == (ssize_t)len) { df->stats.sent++; df->stats.sent_bytes += len; }
    else {
        df->stats.dropped++;
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Inline UDP send failed bytes=%zd expected=%zu error=%s",
                    sent, len, socket_strerror(socket_get_error()));
    }
    return sent;
}

/* Линк освобождается независимо от фильтра; после этого detach/destroy не обращаются к его памяти. */
static void dummynet_filter_link_closed(struct ETCP_LINK* link, void* ctx) {
    struct dummynet_filter* df = ctx;
    if (df->link == link) df->link = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_DUMMYNET, "Inline filter detached by link close path=%u", link->local_link_id);
}

struct dummynet_filter* dummynet_filter_create(struct UASYNC* ua) {
    if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Inline filter requires uasync"); return NULL; }
    struct dummynet_filter* df = u_calloc(1, sizeof(*df));
    if (!df) { DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Inline filter allocation failed"); return NULL; }
    df->ua = ua;
    DEBUG_INFO(DEBUG_CATEGORY_DUMMYNET, "Inline UDP loss filter created");
    return df;
}

void dummynet_filter_set_loss(struct dummynet_filter* df, uint32_t loss_permille) {
    if (!df) { DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "NULL inline filter loss update"); return; }
    if (loss_permille > DUMMYNET_MAX_LOSS_PERMILLE) {
        DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Inline loss=%u clamped to %d", loss_permille, DUMMYNET_MAX_LOSS_PERMILLE);
        loss_permille = DUMMYNET_MAX_LOSS_PERMILLE;
    }
    df->loss_permille = loss_permille;
    DEBUG_DEBUG(DEBUG_CATEGORY_DUMMYNET, "Inline loss updated permille=%u", loss_permille);
}

int dummynet_filter_attach(struct dummynet_filter* df, struct ETCP_LINK* link) {
    if (!df || !link || !link->etcp || !link->etcp->instance || !link->conn || link->is_tcp || link->closing ||
        link->etcp->close_requested || link->etcp->state == 2 || link->etcp->instance->ua != df->ua || link->conn->socks_udp) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Inline filter requires live native UDP link on its uasync link=%p", (void*)link); return -1;
    }
    if (df->link == link && link->send_hook == dummynet_filter_send_hook && link->send_hook_ctx == df) return 0;
    if (link->send_hook || link->send_hook_ctx || link->send_hook_release) {
        DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Inline filter target already has send hook path=%u", link->local_link_id); return -1;
    }
    dummynet_filter_detach(df);
    df->link = link;
    link->send_hook = dummynet_filter_send_hook;
    link->send_hook_ctx = df;
    link->send_hook_release = dummynet_filter_link_closed;
    DEBUG_INFO(DEBUG_CATEGORY_DUMMYNET, "Inline filter attached path=%u", link->local_link_id);
    return 0;
}

void dummynet_filter_detach(struct dummynet_filter* df) {
    if (!df || !df->link) return;
    struct ETCP_LINK* link = df->link;
    if (link->send_hook == dummynet_filter_send_hook && link->send_hook_ctx == df) {
        link->send_hook = NULL; link->send_hook_ctx = NULL;
    } else DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Inline filter hook replaced externally path=%u; preserving replacement", link->local_link_id);
    if (link->send_hook_release == dummynet_filter_link_closed) link->send_hook_release = NULL;
    df->link = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_DUMMYNET, "Inline filter detached path=%u", link->local_link_id);
}

void dummynet_filter_destroy(struct dummynet_filter* df) {
    if (!df) return;
    dummynet_filter_detach(df);
    DEBUG_INFO(DEBUG_CATEGORY_DUMMYNET, "Inline filter destroyed recv=%llu sent=%llu lost=%llu dropped=%llu",
               (unsigned long long)df->stats.recv, (unsigned long long)df->stats.sent,
               (unsigned long long)df->stats.lost, (unsigned long long)df->stats.dropped);
    u_free(df);
}

int dummynet_filter_block_addr(struct dummynet_filter* df, uint32_t ip, uint16_t port) {
    if (!df || !ip || !port) { DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Invalid inline block address"); return -1; }
    for (uint8_t i = 0; i < df->block_count; i++)
        if (df->block_addrs[i].ip == ip && df->block_addrs[i].port == port) return 0;
    if (df->block_count == 4) { DEBUG_ERROR(DEBUG_CATEGORY_DUMMYNET, "Inline block list full (4 endpoints)"); return -1; }
    df->block_addrs[df->block_count].ip = ip;
    df->block_addrs[df->block_count].port = port;
    df->block_count++;
    DEBUG_DEBUG(DEBUG_CATEGORY_DUMMYNET, "Inline block added ip=%08x port=%u count=%u", ntohl(ip), ntohs(port), df->block_count);
    return 0;
}

int dummynet_filter_unblock_addr(struct dummynet_filter* df, uint32_t ip, uint16_t port) {
    if (!df || !ip || !port) { DEBUG_WARN(DEBUG_CATEGORY_DUMMYNET, "Invalid inline unblock address"); return -1; }
    for (uint8_t i = 0; i < df->block_count; i++) {
        if (df->block_addrs[i].ip == ip && df->block_addrs[i].port == port) {
            df->block_count--;
            if (i < df->block_count) memmove(&df->block_addrs[i], &df->block_addrs[i+1], (df->block_count - i) * sizeof(df->block_addrs[0]));
            DEBUG_DEBUG(DEBUG_CATEGORY_DUMMYNET, "Inline block removed ip=%08x port=%u count=%u", ntohl(ip), ntohs(port), df->block_count);
            return 0;
        }
    }
    return 0;
}

const struct dummynet_stats* dummynet_filter_get_stats(struct dummynet_filter* df) {
    return df ? &df->stats : NULL;
}
