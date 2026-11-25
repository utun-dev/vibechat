#include "etcp_keepalive.h"
#include "etcp.h"
#include "etcp_api.h"
#include "stcp_link.h"
#include "topo_node.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/memory_pool.h"
#include <string.h>
#include <limits.h>
#include <openssl/rand.h>
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif

// forward declarations (static)
static void keepalive_timer_cb(void* arg);
static int etcp_all_links_down(struct ETCP_CONN* etcp);
static void ka_stop_keepalive_timer(struct ETCP_LINK* link);
static void ka_link_down(struct ETCP_LINK* link);
static void ka_send_sleep_keepalive(struct ETCP_LINK* link);
static void ka_send_sleep_ack(struct ETCP_LINK* link);
static void ka_announce_retry_cb(void* arg);
static void ka_announce_burst(struct ETCP_LINK* link);
static void ka_enter_sleep(struct ETCP_LINK* link);
static void ka_wake(struct ETCP_LINK* link);

// === перенесено из etcp_connections.c без изменений ===

// Вычислить keepalive по типу устройств: оба десктоп/сервер → min, иначе (есть mobile) → max
uint16_t negotiate_keepalive(uint8_t my_type, uint16_t my_ka,
                             uint8_t peer_type, uint16_t peer_ka) {
    int my_lp = (my_type == CLIENT_TYPE_MOBILE);
    int peer_lp = (peer_type == CLIENT_TYPE_MOBILE);
    uint16_t result = (my_lp || peer_lp) ? (my_ka > peer_ka ? my_ka : peer_ka)
                                         : (my_ka < peer_ka ? my_ka : peer_ka);
    DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "negotiate: my=%d(type=%d) peer=%d(type=%d) → %d (rule=%s)",
               my_ka, my_type, peer_ka, peer_type, result,
               (my_lp || peer_lp) ? "max(low_power)" : "min(both_desktop)");
    return result;
}

// Send keepalive packet: code(0x08) + flags + ka_period_ms + [active_remaining + sleep_duration]
static void etcp_link_send_keepalive_ex(struct ETCP_LINK* link, uint8_t flags) {
    DEBUG_TRACE(DEBUG_CATEGORY_KEEPALIVE, "");
    if (!link || !link->etcp || !link->etcp->instance) return;

    int padding = etcp_padding_size(&link->etcp->padding, ETCP_PADDING_MAX);
    if (padding < 0) return;
    struct ETCP_DGRAM* dgram = u_malloc(sizeof(struct ETCP_DGRAM) + 8 + padding);
    if (!dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_KEEPALIVE, "malloc failed");
        return;
    }

    memset(dgram, 0, sizeof(*dgram));
    dgram->link = link;
    dgram->data[0] = ETCP_KEEPALIVE;
    dgram->data[1] = flags;
    dgram->data[2] = link->ka_period_ms & 0xFF;
    dgram->data[3] = link->ka_period_ms >> 8;
    if (flags & KA_FLAG_SLEEP) {
        dgram->data[4] = link->ka_active_remaining & 0xFF;
        dgram->data[5] = link->ka_active_remaining >> 8;
        dgram->data[6] = link->ka_sleep_duration & 0xFF;
        dgram->data[7] = link->ka_sleep_duration >> 8;
        dgram->data_len = 8;
    } else {
        dgram->data_len = 4;
    }
    memset(dgram->data + dgram->data_len, 0, padding);
    dgram->data_len += padding;
    dgram->noencrypt_len = 0;
    dgram->timestamp = get_current_timestamp();
    dgram->flag_up = link->is_tcp ? 1 : link->recv_keepalive;

    int sent = etcp_encrypt_send(dgram);
    if (sent == -2)
        DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] keepalive postponed: transport busy link=%u flags=%02x body=%u",
                    link->etcp->log_name, link->local_link_id, flags, dgram->data_len);
    else if (sent < 0)
        DEBUG_WARN(DEBUG_CATEGORY_KEEPALIVE, "[%s] keepalive send failed link=%u flags=%02x body=%u result=%d",
                   link->etcp->log_name, link->local_link_id, flags, dgram->data_len, sent);
    else {
        link->keepalive_sent_count++;
        DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] keepalive submitted link=%u flags=%02x padding=%d body=%u bytes=%d",
                    link->etcp->log_name, link->local_link_id, flags, padding, dgram->data_len, sent);
    }
    u_free(dgram);
}

// Обычный keepalive (без sleep-флагов): «жив» / «проснулся».
void etcp_link_send_keepalive(struct ETCP_LINK* link) {
    etcp_link_send_keepalive_ex(link, 0);
}

// Check if all links for an ETCP_CONN are down
// Returns 1 if all links are down or no links exist, 0 otherwise
static int etcp_all_links_down(struct ETCP_CONN* etcp) {
    if (!etcp || !etcp->links) return 1;

    struct ETCP_LINK* l = etcp->links;
    while (l) {
        if (l->link_status == 1) {
            return 0;  // At least one link is up
        }
        l = l->next;
    }
    return 1;  // All links are down
}

// Ставит таймер обычной KA-пробы со свежим разбросом ±50% от номинального периода.
static void ka_schedule_probe(struct ETCP_LINK* link, uint16_t period_ms) {
    uint32_t random, delay_ms = period_ms;
    if (!period_ms) {
        delay_ms = KA_PERIOD_MIN_MS;
        DEBUG_WARN(DEBUG_CATEGORY_KEEPALIVE, "[%s] zero keepalive period link=%u, using %u ms",
                   link->etcp->log_name, link->local_link_id, delay_ms);
    } else if (RAND_bytes((unsigned char*)&random, sizeof(random)) != 1) {
        DEBUG_WARN(DEBUG_CATEGORY_KEEPALIVE, "[%s] keepalive RNG failed link=%u, using nominal %u ms",
                   link->etcp->log_name, link->local_link_id, delay_ms);
    } else {
        uint32_t half_ms = period_ms / 2;
        uint32_t choices = 2 * half_ms + 1;
        delay_ms = period_ms - half_ms + ((uint64_t)random * choices >> 32);
    }

    ka_stop_keepalive_timer(link);
    link->keepalive_timer = uasync_set_timeout(link->etcp->instance->ua, (int)(delay_ms * 10), link, keepalive_timer_cb, "link_keepalive");
    if (!link->keepalive_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_KEEPALIVE, "[%s] cannot schedule keepalive probe link=%u period_ms=%u delay_ms=%u",
                    link->etcp->log_name, link->local_link_id, period_ms, delay_ms);
        return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] keepalive probe scheduled link=%u period_ms=%u delay_ms=%u",
                link->etcp->log_name, link->local_link_id, period_ms, delay_ms);
}

void start_keepalive_timer(struct ETCP_LINK* link) {
    if (!link || !link->etcp || !link->etcp->instance || !link->etcp->instance->ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "start_keepalive_timer: BAD link=%p etcp=%p inst=%p ua=%p",
                    (void*)link, (void*)(link ? link->etcp : NULL),
                    (void*)(link && link->etcp ? link->etcp->instance : NULL),
                    (void*)(link && link->etcp && link->etcp->instance ? link->etcp->instance->ua : NULL));
        return;
    }
    // Start keepalive timer
    if (link->init_timer) {// cancel init timer
        uasync_cancel_timeout(link->etcp->instance->ua, link->init_timer);
        link->init_timer = NULL;
    }

    if (!link->keepalive_timer) ka_schedule_probe(link, link->keepalive_interval);
}

// RX проверяется независимо от отправки keepalive, в том числе при объявленном сне.
static void ka_rx_timeout_cb(void* arg) {
    struct ETCP_LINK* link = arg;
    link->rx_timeout_timer = NULL;
    if (link->etcp->close_requested || !link->recv_keepalive) return;
    uint64_t now = get_time_tb();
    if (now < link->rx_deadline_tb) {
        uint64_t remaining = link->rx_deadline_tb - now;
        int delay = remaining > INT_MAX ? INT_MAX : (int)remaining;
        link->rx_timeout_timer = uasync_set_timeout(link->etcp->instance->ua, delay, link, ka_rx_timeout_cb, "link_rx_timeout");
        if (!link->rx_timeout_timer)
            DEBUG_ERROR(DEBUG_CATEGORY_KEEPALIVE, "[%s] cannot rearm RX timeout link=%u", link->etcp->log_name, link->local_link_id);
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] RX timeout link=%u server=%u tcp=%u age_ms=%llu my_sleep=%u peer_sleep=%u",
               link->etcp->log_name, link->local_link_id, link->is_server, link->is_tcp,
               (unsigned long long)((now - link->last_recv_local_time) / 10), link->ka_my_sleeping, link->ka_peer_sleeping);
    ka_link_down(link); // callbacks могут удалить линк; дальше его не используем
}

// При RX меняется только deadline: уже поставленный таймер сам проверит продление.
void etcp_keepalive_watch_rx(struct ETCP_LINK* link) {
    if (!link || !link->etcp || link->etcp->close_requested || !link->initialized || !link->recv_keepalive) return;
    uint64_t timeout_ms = link->keepalive_timeout;
    if (link->ka_my_sleeping || link->ka_peer_sleeping) {
        uint64_t sleep_ms = ((uint64_t)link->ka_active_remaining + link->ka_sleep_duration) * 100;
        sleep_ms += (uint64_t)link->keepalive_interval * KA_TIMEOUT_MULT;
        if (sleep_ms > timeout_ms) timeout_ms = sleep_ms;
    }
    uint64_t previous_deadline = link->rx_deadline_tb;
    link->rx_deadline_tb = link->last_recv_local_time + timeout_ms * 10;
    if (link->rx_timeout_timer && link->rx_deadline_tb >= previous_deadline) return;
    if (link->rx_timeout_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->rx_timeout_timer); link->rx_timeout_timer = NULL;
        DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] RX deadline shortened link=%u timeout_ms=%llu",
                    link->etcp->log_name, link->local_link_id, (unsigned long long)timeout_ms);
    }
    uint64_t now = get_time_tb();
    uint64_t remaining = link->rx_deadline_tb > now ? link->rx_deadline_tb - now : 1;
    int delay = remaining > INT_MAX ? INT_MAX : (int)remaining;
    link->rx_timeout_timer = uasync_set_timeout(link->etcp->instance->ua, delay, link, ka_rx_timeout_cb, "link_rx_timeout");
    if (!link->rx_timeout_timer)
        DEBUG_ERROR(DEBUG_CATEGORY_KEEPALIVE, "[%s] cannot schedule RX timeout link=%u", link->etcp->log_name, link->local_link_id);
    else DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] RX watchdog started link=%u timeout_ms=%llu my_sleep=%u peer_sleep=%u",
                     link->etcp->log_name, link->local_link_id, (unsigned long long)timeout_ms,
                     link->ka_my_sleeping, link->ka_peer_sleeping);
}

// Таймер отправки keepalive и восстановления исходящих линков.
static void keepalive_timer_cb(void* arg) {
    DEBUG_TRACE(DEBUG_CATEGORY_KEEPALIVE, "");
    struct ETCP_LINK* link = (struct ETCP_LINK*)arg;
    if (!link || !link->etcp || !link->etcp->instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_KEEPALIVE, "KEEPALIVE NULL !!!!!!!!");
        return;
    }

    link->keepalive_timer = NULL;

    // Обычный keepalive работает только в normal-режиме. В спячке (моей или пира)
    // таймер гасится (ka_stop_keepalive_timer); если долетел — переарм и выход.
    if (link->ka_my_sleeping || link->ka_peer_sleeping) {
        link->keepalive_timer = uasync_set_timeout(link->etcp->instance->ua,
            link->ka_period_ms * 10, link, keepalive_timer_cb, "link_keepalive");
        return;
    }

    // Check if all links are down and start recovery if needed (client only)
    if (link->is_server == 0 && etcp_all_links_down(link->etcp)) {
        if (link->is_tcp) {
            if (link->tcp_link) { stcp_link_close(link->tcp_link); link->tcp_link = NULL; }
            etcp_tcp_link_start_reconnect(link);
            return;
        }
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] All links are down, starting recovery", link->etcp->log_name);
        etcp_link_enter_reinit(link);// keepalive timr после reinit не нужен
        return;
    }

    // Skip if link is not initialized
    if (!link->initialized) {
        DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] Keepalive skipped - link not initialized",
                    link->etcp->log_name);
        goto restart_timer;
    }

    // Период: адаптивный от базового (normal-режим).
    if (link->pkt_sent_since_keepalive)
        link->ka_period_ms = (uint16_t)link->keepalive_interval;
    else if (link->etcp->instance && link->etcp->instance->config &&
             link->etcp->instance->config->global.keepalive_adaptive) {
        uint32_t next = (uint32_t)link->ka_period_ms * 105 / 100 + 1;
        link->ka_period_ms = next > KA_PERIOD_MAX_MS ? KA_PERIOD_MAX_MS : (uint16_t)next;
    }
    link->pkt_sent_since_keepalive = 0;

    // Отправка обычного flagless-keepalive.
    if (!link->is_server || link->recv_keepalive)
        etcp_link_send_keepalive(link);

restart_timer:
    ka_schedule_probe(link, link->ka_period_ms);
}

// === протокол спячки (sleep) на флагах keepalive-пакета ===

// Период keepalive в спячке (мс): из настройки standby_keepalive_ms (Android).
static uint16_t ka_sleep_period_ms(struct UTUN_INSTANCE* inst) {
#ifdef UTUN_HAVE_STANDBY
    int ms = standby_get_keepalive_ms();
    if (ms < 100) ms = 100;
    return (uint16_t)ms;
#else
    (void)inst;
    return 30000;
#endif
}

// Сколько осталось до SLEEP-фазы (x0.1с): 0 = уже спим.
static uint16_t ka_active_remaining_01s(void) {
#ifdef UTUN_HAVE_STANDBY
    uint32_t v = (uint32_t)(standby_get_active_remaining_tb() / 1000);
    return v > 65535 ? 65535 : (uint16_t)v;
#else
    return 0;
#endif
}

// Длительность SLEEP-фазы (x0.1с): в SLEEP — оставшийся сон, в ACTIVE — полная длительность.
static uint16_t ka_sleep_duration_01s(void) {
#ifdef UTUN_HAVE_STANDBY
    int s = standby_get_sleep_tb();
    uint32_t v = s > 0 ? (uint32_t)(s / 1000)
                       : (uint32_t)(standby_get_sleep_duration_tb() / 1000);
    return v > 65535 ? 65535 : (uint16_t)v;
#else
    return 0;
#endif
}

// Отменить ретрай-таймер анонса спячки.
static void ka_cancel_announce_retry(struct ETCP_LINK* link) {
    if (link->ka_sleep_announce_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->ka_sleep_announce_timer);
        link->ka_sleep_announce_timer = NULL;
    }
}

// Гасим обычный keepalive-таймер (переход в спячку / пир уснул).
static void ka_stop_keepalive_timer(struct ETCP_LINK* link) {
    if (link->keepalive_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->keepalive_timer);
        link->keepalive_timer = NULL;
    }
}

// Максимум попыток анонса в ACTIVE-окне: сколько влезет в окно, но ≥ KA_SLEEP_MIN_ATTEMPTS.
static uint8_t ka_announce_max_calc(struct ETCP_LINK* link) {
    uint16_t interval = link->keepalive_interval ? link->keepalive_interval : 200;
    uint32_t window_ms = (uint32_t)ka_active_remaining_01s() * 100;  // остаток ACTIVE-окна, мс
    uint32_t fit = window_ms / interval + 1;                          // +1 за первый анонс
    if (fit < KA_SLEEP_MIN_ATTEMPTS) fit = KA_SLEEP_MIN_ATTEMPTS;
    if (fit > 255) fit = 255;
    return (uint8_t)fit;
}

// Разрыв линка (мёртвый пир в спячке). Может освободить link — после вызова link не трогать.
static void ka_link_down(struct ETCP_LINK* link) {
    if (!link || !link->etcp || link->closing || link->etcp->close_requested) return;
    struct ETCP_CONN* etcp = link->etcp;
    etcp->callbacks_running++; link->active_users++;
    if (link->rx_timeout_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->rx_timeout_timer); link->rx_timeout_timer = NULL;
    }
    link->recv_keepalive = 0;
    int old_link_status = link->link_status;
    link->link_status = 0;
    if (old_link_status) {
        DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] Link %u down (peer unresponsive): addr=%s remote_ka=%u state=%u init=%u",
            etcp->log_name, link->local_link_id, sockaddr_storage_to_str(&link->remote_addr).str,
            link->remote_keepalive, link->link_state, link->initialized);
    }
    etcp_fire_link_status_cbk(link, link->link_state, old_link_status);
    if (!etcp_link_is_alive(etcp, link)) goto done;
    etcp_on_link_down(etcp, link);
    if (!etcp_link_is_alive(etcp, link)) goto done;
    if (link->is_tcp && link->tcp_link) { stcp_link_close(link->tcp_link); link->tcp_link = NULL; }
done:
    etcp_link_release(link); etcp->callbacks_running--;
}

// Отправить один анонс спячки с таймингом.
static void ka_send_sleep_keepalive(struct ETCP_LINK* link) {
    link->ka_active_remaining = ka_active_remaining_01s();
    link->ka_sleep_duration = ka_sleep_duration_01s();
    link->ka_announce_attempts++;
    etcp_link_send_keepalive_ex(link, KA_FLAG_SLEEP);
}

// Заармить ретрай анонса (интервал = keepalive_interval).
static void ka_arm_announce_retry(struct ETCP_LINK* link) {
    if (link->ka_sleep_announce_timer) return;
    link->ka_sleep_announce_timer = uasync_set_timeout(link->etcp->instance->ua,
        (uint32_t)link->keepalive_interval * 10, link, ka_announce_retry_cb, "ka_sleep_announce");
}

// Ответ на анонс спячки пира: подтверждение доставки (без тайминга).
static void ka_send_sleep_ack(struct ETCP_LINK* link) {
    etcp_link_send_keepalive_ex(link, KA_FLAG_SLEEP_ACK);
}

static void ka_announce_retry_cb(void* arg) {
    struct ETCP_LINK* link = (struct ETCP_LINK*)arg;
    link->ka_sleep_announce_timer = NULL;
    if (!link || !link->etcp || !link->etcp->instance) return;
    if (!link->ka_my_sleeping || link->ka_sleep_acked) return;   // анонс завершён

    link->ka_fail_count++;                                        // предыдущая попытка без ACK
    if (link->ka_fail_count >= KA_SLEEP_MAX_FAILS) {
        DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "[%s] peer unresponsive in sleep (%u fails) — link down",
                   link->etcp->log_name, link->ka_fail_count);
        ka_link_down(link);
        return;
    }
    if (link->ka_announce_attempts >= link->ka_announce_max_attempts) {
        DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] announce attempts exhausted (%u/%u) in window",
                    link->etcp->log_name, link->ka_announce_attempts, link->ka_announce_max_attempts);
        return;
    }
    ka_send_sleep_keepalive(link);                                // ретрай
    ka_arm_announce_retry(link);
}

// Анонс-серия (ACTIVE-окно): анонс + ретраи до ACK, лимит попыток на окно.
// TCP доставляет надёжно — без ретраев и ACK, только один анонс.
static void ka_announce_burst(struct ETCP_LINK* link) {
    link->ka_announce_attempts = 0;
    link->ka_sleep_acked = 0;
    if (link->is_tcp) {
        ka_send_sleep_keepalive(link);
        return;
    }
    link->ka_announce_max_attempts = ka_announce_max_calc(link);
    ka_send_sleep_keepalive(link);   // попытка 1
    ka_arm_announce_retry(link);
}

// Вход в спячку — моя сторона: гасим normal-keepalive, анонс-серия.
static void ka_enter_sleep(struct ETCP_LINK* link) {
    if (!link || !link->etcp || !link->etcp->instance) return;
    uint16_t period = ka_sleep_period_ms(link->etcp->instance);
    link->ka_my_sleeping = 1;
    link->ka_sleep_period_ms = period;
    link->ka_period_ms = period;
    link->keepalive_timeout = (uint32_t)period * KA_TIMEOUT_MULT;
    ka_stop_keepalive_timer(link);
    DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "[%s] entering sleep (period=%dms)", link->etcp->log_name, period);
    ka_announce_burst(link);
    etcp_keepalive_watch_rx(link);
}

// Пробуждение (форграунд) — обычный keepalive, возврат к normal-режиму.
static void ka_wake(struct ETCP_LINK* link) {
    if (!link || !link->etcp || !link->etcp->instance) return;
    if (!link->ka_my_sleeping) return;
    link->ka_my_sleeping = 0;
    link->ka_sleep_acked = 0;
    link->ka_fail_count = 0;
    link->ka_announce_attempts = 0;
    ka_cancel_announce_retry(link);
    link->ka_period_ms = (uint16_t)link->keepalive_interval;
    link->keepalive_timeout = (uint32_t)link->keepalive_interval * KA_TIMEOUT_MULT;
    DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "[%s] woke up, back to normal (interval=%dms)",
               link->etcp->log_name, link->keepalive_interval);
    etcp_link_send_keepalive(link);
    if (!link->ka_peer_sleeping)
        ka_schedule_probe(link, link->ka_period_ms);
    etcp_keepalive_watch_rx(link);
}

typedef void (*ka_link_fn)(struct ETCP_LINK* link);

static int ka_for_each_link(struct UTUN_INSTANCE* inst, ka_link_fn fn) {
    int count = 0;
    if (inst->connections) {
        for (struct ll_entry* e = inst->connections->head; e; e = e->next) {
            struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
            if (!ce || !ce->conn) continue;
            for (struct ETCP_LINK* l = ce->conn->links; l; l = l->next)
                if (l->initialized) { fn(l); count++; }
        }
    }
    return count;
}

#ifdef UTUN_HAVE_STANDBY
// Фактический режим standby меняется после подготовки его фазы и таймера.
static void ka_standby_switch(int enabled, void* arg) {
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    if (!inst || inst->client_type != CLIENT_TYPE_MOBILE) return;
    int links = ka_for_each_link(inst, enabled ? ka_enter_sleep : ka_wake);
    DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "standby mode=%s applied to %d links node=%016llx",
               enabled ? "sleep" : "normal", links, (unsigned long long)inst->node_id);
}

// Вход в ACTIVE-окно — анонс-серия с ретраями до ACK (active_remaining>0).
static void ka_active_announce(struct ETCP_LINK* link) {
    if (!link->ka_my_sleeping) return;
    ka_announce_burst(link);
}

// Фаза duty-cycle: SLEEP → переанонс спячки; ACTIVE → анонс активности (флаг всегда стоит).
static void ka_phase_cb(int sleeping, void* arg) {
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    if (!inst || inst->client_type != CLIENT_TYPE_MOBILE) return;
    if (sleeping) {
        /* SLEEP-фаза не анонсируется отдельно — её тайминг уже передан в ACTIVE-окне
           (active_remaining→0 + sleep_duration), пир выведет переход по своему фазовому
           таймеру. Гасим ретраи анонса окна, чтобы не дослали лишний анонс в SLEEP. */
        ka_for_each_link(inst, ka_cancel_announce_retry);
        DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "keepalive: duty-cycle SLEEP phase (no announce, window retries cancelled, node=%016llx)",
                   (unsigned long long)inst->node_id);
    } else {
        ka_for_each_link(inst, ka_active_announce);
        DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "keepalive: duty-cycle ACTIVE phase, announce (node=%016llx)",
                   (unsigned long long)inst->node_id);
    }
}
#endif

/* ── Пир: машина фаз троттлинга (по active_remaining/sleep_duration) ── */

static void ka_peer_arm_phase(struct ETCP_CONN* etcp);

static void ka_peer_set_phase(struct ETCP_CONN* etcp, int sleeping) {
    if (!etcp) return;
    if (etcp->peer_sleep_phase == (uint8_t)(sleeping ? 1 : 0)) return;
    etcp->peer_sleep_phase = sleeping ? 1 : 0;
    utun_fire_peer_sleep_cbk(etcp, sleeping);
}

static void ka_peer_phase_timer_cb(void* arg) {
    struct ETCP_CONN* etcp = (struct ETCP_CONN*)arg;
    etcp->peer_phase_timer = NULL;
    if (!etcp || etcp->state == 2) return;
    ka_peer_set_phase(etcp, etcp->peer_sleep_phase ? 0 : 1);
    ka_peer_arm_phase(etcp);
}

static void ka_peer_arm_phase(struct ETCP_CONN* etcp) {
    if (!etcp || !etcp->instance) return;
    uint16_t dur_01s = etcp->peer_sleep_phase ? etcp->peer_sleep_duration : etcp->peer_active_remaining;
    if (etcp->peer_phase_timer) {
        uasync_cancel_timeout(etcp->instance->ua, etcp->peer_phase_timer);
        etcp->peer_phase_timer = NULL;
    }
    /* гвард: нулевой тайминг (битый/legacy пакет) → не армать, иначе timer=0 даёт busy-loop */
    if (dur_01s == 0) {
        DEBUG_WARN(DEBUG_CATEGORY_KEEPALIVE, "[%s] peer: zero phase duration (phase=%d), skip arming",
                   etcp->log_name, etcp->peer_sleep_phase);
        return;
    }
    etcp->peer_phase_timer = uasync_set_timeout(etcp->instance->ua, (int)((uint32_t)dur_01s * 1000),
                                                etcp, ka_peer_phase_timer_cb, "peer_phase");
}

int etcp_keepalive_on_recv(struct ETCP_DGRAM* pkt) {
    if (!pkt || !pkt->link || !pkt->link->etcp) {
        DEBUG_ERROR(DEBUG_CATEGORY_KEEPALIVE, "keepalive receive without packet/link/connection pkt=%p", (void*)pkt);
        return 0;
    }
    struct ETCP_LINK* link = pkt->link;
    struct UTUN_INSTANCE* instance = link->etcp->instance;
    uint8_t code = pkt->data[0];

    if (code == ETCP_KEEPALIVE) {
        uint8_t flags = (pkt->data_len >= 2) ? pkt->data[1] : 0;
        size_t body_len = (flags & KA_FLAG_SLEEP) ? 8 : 4;
        if (pkt->data_len < body_len || pkt->data_len - body_len > ETCP_PADDING_MAX) {
            DEBUG_WARN(DEBUG_CATEGORY_KEEPALIVE, "[%s] invalid keepalive size=%u flags=%02x body=%zu",
                       link->etcp->log_name, pkt->data_len, flags, body_len);
            memory_pool_free(instance->pkt_pool, pkt);
            return 1;
        }
        uint16_t peer_period = pkt->data[2] | ((uint16_t)pkt->data[3] << 8);
        if (peer_period) link->keepalive_timeout = (uint32_t)peer_period * KA_TIMEOUT_MULT;
        DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] keepalive RX link=%u flags=%02x padding=%zu period=%ums",
                    link->etcp->log_name, link->local_link_id, flags, pkt->data_len - body_len, peer_period);

        if (flags & KA_FLAG_SLEEP_ACK) {
            /* Ответ на мой анонс сна — команда дошла, ретраи сняты, счётчик провалов обнулён. */
            link->ka_sleep_acked = 1;
            link->ka_fail_count = 0;
            link->ka_announce_attempts = 0;
            ka_cancel_announce_retry(link);
            DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] sleep ack received", link->etcp->log_name);
        } else if (flags & KA_FLAG_SLEEP) {
            /* Пир объявил сон — заткнуться (гасим свой normal-keepalive) и вести фазу троттлинга. */
            uint16_t active_remaining = (pkt->data_len >= 6)
                ? (pkt->data[4] | ((uint16_t)pkt->data[5] << 8)) : 0;
            uint16_t sleep_duration = (pkt->data_len >= 8)
                ? (pkt->data[6] | ((uint16_t)pkt->data[7] << 8)) : 0;
            link->ka_peer_sleeping = 1;
            link->ka_sleep_period_ms = peer_period;
            link->ka_active_remaining = active_remaining;
            link->ka_sleep_duration = sleep_duration;
            link->ka_period_ms = peer_period;
            ka_stop_keepalive_timer(link);
            if (link->etcp) {
                link->etcp->peer_active_remaining = active_remaining;
                link->etcp->peer_sleep_duration = sleep_duration;
                ka_peer_set_phase(link->etcp, active_remaining == 0);
                ka_peer_arm_phase(link->etcp);
            }
            DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "[%s] peer sleeping (period=%dms active_rem=%d.%ds sleep_dur=%d.%ds)",
                       link->etcp->log_name, peer_period,
                       (int)(active_remaining / 10), (int)(active_remaining % 10),
                       (int)(sleep_duration / 10), (int)(sleep_duration % 10));
            if (!link->is_tcp)
                ka_send_sleep_ack(link);   /* TCP доставляет надёжно — ACK не нужен */
        } else {
            /* Обычный keepalive снимает сон соединения даже по новому линку без ka_peer_sleeping. */
            struct ETCP_CONN* conn = link->etcp;
            int link_was_sleeping = link->ka_peer_sleeping;
            if (link_was_sleeping || conn->peer_sleep_phase || conn->peer_phase_timer) {
                DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE,
                           "[%s] peer normal link=%u tcp=%u link_sleep=%u phase=%u phase_timer=%u: cancel standby",
                           conn->log_name, link->local_link_id, link->is_tcp, link->ka_peer_sleeping,
                           conn->peer_sleep_phase, conn->peer_phase_timer != NULL);
            }
            if (link_was_sleeping) {
                link->ka_peer_sleeping = 0;
                link->ka_period_ms = (uint16_t)link->keepalive_interval;
                link->keepalive_timeout = (uint32_t)link->keepalive_interval * KA_TIMEOUT_MULT;
            }
            if (conn->peer_phase_timer) {
                uasync_cancel_timeout(instance->ua, conn->peer_phase_timer);
                conn->peer_phase_timer = NULL;
            }
            conn->peer_active_remaining = conn->peer_sleep_duration = 0;
            ka_peer_set_phase(conn, 0);
            /* Повторный normal RX не должен отодвигать очередную отправку keepalive. */
            if (link_was_sleeping && !link->ka_my_sleeping)
                ka_schedule_probe(link, link->ka_period_ms);
        }

        link->keepalive_recv_count++;
        etcp_keepalive_watch_rx(link);
        memory_pool_free(instance->pkt_pool, pkt);
        return 1;
    }

    return 0;
}

#ifndef UTUN_HAVE_STANDBY
static void etcp_keepalive_on_activity(struct UTUN_INSTANCE* inst, int active, void* arg) {
    (void)arg;
    if (!inst || inst->client_type != CLIENT_TYPE_MOBILE) return;
    ka_for_each_link(inst, active ? ka_wake : ka_enter_sleep);
    DEBUG_INFO(DEBUG_CATEGORY_KEEPALIVE, "keepalive activity change: %s -> applied to links (node=%016llx)",
               active ? "ACTIVE(wake)" : "STANDBY(sleep)", (unsigned long long)inst->node_id);
}
#endif

// Сразу сообщает текущий режим/фазу нового или восстановленного линка и запускает нужный механизм keepalive.
void etcp_keepalive_link_ready(struct ETCP_LINK* link) {
    if (!link || !link->etcp || !link->etcp->instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_KEEPALIVE, "link_ready: missing link/connection/instance link=%p", (void*)link);
        return;
    }
    int sleeping = 0;
#ifdef UTUN_HAVE_STANDBY
    sleeping = link->etcp->instance->client_type == CLIENT_TYPE_MOBILE && standby_is_enabled();
#endif
    DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] link ready id=%u tcp=%u server=%u mode=%s",
                link->etcp->log_name, link->local_link_id, link->is_tcp, link->is_server, sleeping ? "standby" : "normal");
    if (sleeping) {
        ka_enter_sleep(link);
    } else if (link->ka_my_sleeping) {
        ka_wake(link);
    } else {
        if (!link->ka_peer_sleeping && !link->keepalive_timer)
            link->ka_period_ms = link->keepalive_interval;
        etcp_link_send_keepalive(link);
        if (!link->ka_peer_sleeping) start_keepalive_timer(link);
    }
    etcp_keepalive_watch_rx(link);
}

void etcp_keepalive_register(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
#ifdef UTUN_HAVE_STANDBY
    standby_set_switch_callback(ka_standby_switch, inst);
    standby_set_phase_callback(ka_phase_cb, inst);
#else
    utun_add_activity_cbk(inst, etcp_keepalive_on_activity, NULL);
#endif
    DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "keepalive mode/phase callbacks registered node=%016llx", (unsigned long long)inst->node_id);
}
