// Выбор линка по индивидуальному окну DATA и pacing; ACK не занимает окно DATA.

#include "etcp_link_io.h"
#include "etcp_loadbalancer.h"
#include "etcp_profile.h"
#include "../lib/debug_config.h"
#include "../lib/u_async.h"
#include <stdlib.h>
#include <stdint.h>  // For UINT64_MAX
#include <time.h>    // For timespec
#include <limits.h>
#include "../lib/mem.h"

// Enable comprehensive debug output for loadbalancer module
#define DEBUG_CATEGORY_LOADBALANCER 1

// Forward declarations
static void shaper_timer_cb(void* arg);  // Shaper wait callback

// Ждём первого TB, в котором долг строго меньше разрешённого допуска, а не его полного погашения.
static void shaper_arm(struct ETCP_LINK* link) {
    uint64_t now = get_time_tb();
    if (link->shaper_timer || !link->bbr_pacing_rate || link->shaper_load_time_tb < now + SHAPER_BURST_DELAY_TB) return;
    uint64_t delay = link->shaper_load_time_tb - now - SHAPER_BURST_DELAY_TB + 1;
    if (delay > INT_MAX) delay = INT_MAX;
    link->shaper_timer = uasync_set_timeout(link->etcp->instance->ua, (int)delay, link, shaper_timer_cb, "etcp_shaper");
    if (!link->shaper_timer) {
        struct ETCP_CONN* conn = link->etcp;
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot schedule pacing link=%u rate_Bps=%u delay_tb=%llu",
            conn->log_name, link->local_link_id, link->bbr_pacing_rate, (unsigned long long)delay);
        conn->callbacks_running++; etcp_connection_close(conn); conn->callbacks_running--;
    } else {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] pacing arm link=%u rate_Bps=%u now_tb=%llu debt_until_tb=%llu fraction=%llu delay_tb=%llu",
            link->etcp->log_name, link->local_link_id, link->bbr_pacing_rate, (unsigned long long)now,
            (unsigned long long)link->shaper_load_time_tb, (unsigned long long)link->shaper_sub_nanotime, (unsigned long long)delay);
    }
}

void etcp_loadbalancer_set_pacing_rate(struct ETCP_LINK* link, uint32_t rate) {
    if (!link || !link->etcp) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "pacing: invalid link=%p", (void*)link); return; }
    uint32_t old_rate = link->bbr_pacing_rate;
    if (rate == old_rate) return;
    uint64_t now = get_time_tb(), debt = 0, credit = 0, fraction = 0;
    int was_blocked = link->shaper_timer != NULL;
    if (old_rate && rate && link->shaper_load_time_tb < now) {
        credit = now - link->shaper_load_time_tb;
        if (credit > SHAPER_BURST_DELAY_TB) credit = SHAPER_BURST_DELAY_TB;
        else fraction = link->shaper_sub_nanotime;
    } else if (old_rate && rate) {
        debt = (link->shaper_load_time_tb - now) * SUB_MODULO + link->shaper_sub_nanotime;
        // debt*old_rate/rate без переполнения: остаток и обе ставки не шире uint32.
        uint64_t quotient = debt / rate, product = (debt % rate) * old_rate;
        uint64_t tail = product / rate + !!(product % rate);
        if (quotient > (UINT64_MAX - tail) / old_rate) debt = UINT64_MAX;
        else debt = quotient * old_rate + tail;
    }
    if (link->shaper_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->shaper_timer); link->shaper_timer = NULL;
    }
    link->bbr_pacing_rate = rate;
    link->bandwidth = (uint32_t)(((uint64_t)rate * 8 + 999) / 1000);
    link->shaper_load_time_tb = now + debt / SUB_MODULO - credit;
    link->shaper_sub_nanotime = credit ? fraction : debt % SUB_MODULO;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] pacing link=%u rate_Bps=%u->%u debt_sub=%llu deadline_tb=%llu now_tb=%llu",
        link->etcp->log_name, link->local_link_id, old_rate, rate, (unsigned long long)debt,
        (unsigned long long)link->shaper_load_time_tb, (unsigned long long)now);
    shaper_arm(link);
    if (was_blocked && !link->shaper_timer) etcp_conn_wake_sender(link->etcp);
}

// Минимум inflight среди вмещающих целый DATA; при равенстве начинаем после прошлого выбранного линка.
struct ETCP_LINK* etcp_loadbalancer_select_link(struct ETCP_CONN* etcp, uint32_t data_len, const struct INFLIGHT_PACKET* retry) {
    if (!etcp || !etcp->links) return NULL;
    unsigned healthy = 0, total = 0, untried_healthy = 0, untried_total = 0;
    if (retry) for (struct ETCP_LINK* l = etcp->links; l; l = l->next) {
        if (!l->initialized || !l->link_status) continue;
        total++;
        int untried = !etcp_packet_link_used(retry, l->local_link_id);
        untried_total += untried;
        if (!l->tx_stalled) { healthy++; untried_healthy += untried; }
    }
    unsigned eligible = healthy ? healthy : total;
    unsigned untried = healthy ? untried_healthy : untried_total;
    struct ETCP_LINK* start = etcp->links;
    for (struct ETCP_LINK* l = etcp->links; l; l = l->next)
        if (l == (retry ? retry->last_link : etcp->last_rr_link)) { start = l->next ? l->next : etcp->links; break; }
    struct ETCP_LINK* best = NULL;
    struct ETCP_LINK* link = start;
    do {
        if (link->initialized && link->link_status && (!retry || !healthy || !link->tx_stalled) &&
            (!retry || !untried || !etcp_packet_link_used(retry, link->local_link_id))) {
            // В новом обходе прежний путь также ждёт другие допустимые каналы, даже если те временно заняты.
            if (retry && link == retry->last_link && eligible > 1) {
                DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] retry waits seq=%u previous=%u eligible=%u untried=%u DATA=%u",
                    etcp->log_name, retry->seq, link->local_link_id, eligible, untried, data_len);
                break;
            }
            int full = data_len && (etcp->active_attempts >= 2 * MAX_INFLIGHT_SIZE ||
                link->attempts->count >= MAX_INFLIGHT_SIZE || data_len > link->inflight_lim_bytes ||
                link->inflight_bytes > link->inflight_lim_bytes - data_len);
            if (full) link->cwnd_limited = 1;
            else if (etcp_link_io_ready(link) && (!data_len || loadbalancer_link_can_send(link)) && (retry || !best || link->inflight_bytes < best->inflight_bytes)) {
                best = link;
                if (retry) break; // За один круг прежний путь встретится только после всех альтернатив.
            }
        }
        link = link->next ? link->next : etcp->links;
    } while (link != start);
    if (best) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] select link=%u DATA=%u inflight=%u/%u", etcp->log_name,
            best->local_link_id, data_len, best->inflight_bytes, best->inflight_lim_bytes);
    }
    return best;
}

// New: Send dgram (select link, encrypt/send, update shaper)
int etcp_loadbalancer_send(struct ETCP_DGRAM* dgram) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_TX_STACK);
    if (!dgram) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "called with NULL dgram");
        return -1;
    }
    
    struct ETCP_CONN* etcp = dgram->link ? dgram->link->etcp : NULL;
    if (!etcp) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "no ETCP_CONN associated with dgram (link=%p)", (void*)dgram->link);
        return -1;
    }
    
    struct ETCP_LINK* link = dgram->link;
    // Encrypt and send (from etcp_connections.c)
    int send_result = etcp_encrypt_send(dgram);
    ETCP_PROFILE_STAGE(ETCP_PROF_TX_COMPLETE);
    etcp_tx_complete(dgram, send_result >= 0);
    if (send_result < 0) {
        if (send_result != -2) DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] encrypt/send failed (%d)", etcp->log_name, send_result);
        memory_pool_free(etcp->instance->pkt_pool, dgram);
        return send_result;
    }
    
    // Update shaper after successful send
    if (!dgram->payload_len) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] ACK/control bypasses DATA pacer link=%u", etcp->log_name, link->local_link_id);
    } else if (dgram->burst_packet) {
        // burst bypasses shaper — не обновляем нагрузку
    } else if (link->bbr_pacing_rate == 0) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] unlimited bandwidth, skipping shaper update", etcp->log_name);
        // Don't return here, let the function continue to free dgram at the end
    } else {
        uint64_t now_tb = get_time_tb();
        if (link->shaper_load_time_tb + SHAPER_BURST_DELAY_TB < now_tb) {
            link->shaper_load_time_tb = now_tb - SHAPER_BURST_DELAY_TB; link->shaper_sub_nanotime = 0;
        }
        // Ставка B/s; SUB_MODULO соответствует 0.1мс, один sub равен 0.1нс.
        uint64_t tx_sub = ((uint64_t)dgram->payload_len * 10000000000ULL + link->bbr_pacing_rate - 1) / link->bbr_pacing_rate;
        
        // Add to load
        link->shaper_sub_nanotime += tx_sub;
        uint64_t carry=link->shaper_sub_nanotime / SUB_MODULO;
        link->shaper_sub_nanotime -= carry * SUB_MODULO;
        link->shaper_load_time_tb += carry;
        
        shaper_arm(link);
    }
//    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] updated load_tb=%llu, sub=%llu, state=%u", etcp->log_name,
//                (unsigned long long)link->shaper_load_time_tb, (unsigned long long)link->shaper_sub_nanotime, link->shaper_timer==NULL?1:0);
    
    memory_pool_free(etcp->instance->pkt_pool, dgram);
    return send_result;
}

int loadbalancer_link_can_send(struct ETCP_LINK* link) {
    if (!link || !etcp_link_io_ready(link)) return 0;
    if (link->burst_active) return 1; // burst bypasses all limits
    if (link->shaper_timer) return 0;
    return 1;
}

void loadbalancer_link_ready(struct ETCP_LINK* link) {
    if (!link || !link->etcp) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "invalid link (%p)", link);
        return;
    }
    if (link->shaper_timer) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "link still blocked by shaper");
        return;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] loadbalancer_link_ready: link=%p links_up=%d initialized=%d tx_state=%d", link->etcp->log_name, (void*)link, link->etcp->links_up, link->etcp->initialized, link->etcp->tx_state);
    if (link->etcp->link_ready_for_send_fn) {
        link->etcp->link_ready_for_send_fn(link->etcp);
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "no link_ready_for_send_fn set");
    }
}

// Get ETCP link status: 1 = at least one link is up, 0 = all links down or no links
int etcp_loadbalancer_get_link_status(struct ETCP_CONN* etcp) {
    if (!etcp) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "NULL etcp");
        return 0;
    }

    struct ETCP_LINK* link = etcp->links;
    int alive_count = 0;

    while (link) {
        if (link->link_status == 1) {
            alive_count++;
        }
        link = link->next;
    }

    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] link status check: %d alive links",
                etcp->log_name[0] ? etcp->log_name : "????→????", alive_count);

    return (alive_count > 0) ? 1 : 0;
}

// Shaper timer callback
static void shaper_timer_cb(void* arg) {
    struct ETCP_LINK* link = (struct ETCP_LINK*)arg;
    if (!link) return;
    
    link->shaper_timer = NULL;
    shaper_arm(link);
    if (link->shaper_timer || link->etcp->close_requested) return;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "link=%p now ready", link);
    
    // Notify loadbalancer that link is ready
    loadbalancer_link_ready(link);
}
