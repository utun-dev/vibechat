#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/rand.h>

#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../utun_instance.h"
#include "../transport_layer/node_conn_direct.h"
#include "../chat/chat_setting.h"
#include "stream_optimizer.h"
#include "stream_priv.h"

#define STREAM_PROBE_BURST 32
#define STREAM_PROBE_PERIOD_MS 1000
#define STREAM_PROBE_CYCLE_MS 30000
#define STREAM_OFFER_TTL_MS 60000
#define STREAM_MIGRATION_TIMEOUT_MS 5000
#define STREAM_TRANSPORT_RETRY_MS 250
#define STREAM_RECOVERY_STALL_MS 5000
#define STREAM_BOOTSTRAP_RELAY_MS 100
#define STREAM_AVAILABILITY_RETRY_MS 100
#define STREAM_AVAILABILITY_WAIT_MS 5000
#define STREAM_PROBLEM_PENALTY_MS 1000
#define STREAM_HOP_PENALTY_MS 3
#define STREAM_RESCUE_PEERS 3
#define STREAM_RESCUE_CHOOSE_MS 50
#define STREAM_LOCAL_RETRY_MS 100
#define STREAM_STANDBY_PERIOD_MS 5000
#define STREAM_ACTIVE_REFRESH_MS 20000

static uint64_t problem_penalty(const struct stream_metric_snapshot *metrics, int valid);
static uint64_t quality_penalty(const struct stream_receive_info *info, const struct stream_metric_snapshot *metrics, int valid);
static uint32_t rescue_random(struct stream_optimizer *o);

struct stream_offer {
    struct stream_receive_info info;
    uint64_t retry_ms, missing_seq;
    unsigned failures;
    int missing_valid; // Отказ POSITION/EXPIRED точной позиции до следующей попытки восстановления
    uint64_t rescue_retry_ms;
    unsigned rescue_failures;
};
struct stream_supplier {
    struct stream_supplier *next;
    struct stream_address node;
    uint64_t cycle, retry_ms, availability_retry_ms, availability_until_ms;
    uint64_t admission_retry_ms; // Повтор INFO после accepting=0; только при дефиците DATA и в общем бюджете обзора.
    uint64_t local_retry_ms; // Местный бюджет; не меняет failures/backoff поставщика.
    uint64_t checked_ms, standby_ms; // Последний начатый обзор и срок обновления выбранного резерва.
    uint32_t rank; // Стабильный случайный порядок равных кандидатов, свой для каждого зрителя.
    uint64_t local_epoch, peer_epoch;
    struct STREAM_RECEIVE *probe;
    struct stream_offer *offers;
    unsigned probe_sub, failures;
    int online, seen, compatible, source, relay, refresh, standby;
};
struct stream_selection {
    struct STREAM_RECEIVE *current, *replacement;
    struct stream_receive_slot *slot; // Постоянный ресурс субпотока сохраняется при потере обоих сетевых handles.
    uint64_t opened_ms, replacement_ms, better_ms;
    struct stream_address better_node;
    uint64_t compare_ms, compare_seq; // После догона истории сравниваем время DATA одного будущего индекса;0 — ещё не начато.
    int opened, overloaded; // OVERLOAD вызывает один приоритетный обзор до следующего изменения состояния.
    struct stream_address rescue_peer;
    uint64_t rescue_seq, rescue_retry_ms;
    uint64_t rescue_until_ms; // Общий срок выбранного аварийного replacement; новые DATA/INFO его не продлевают.
    uint64_t relief_ms, relief_id; // Общий deadline переноса OVERLOAD5с; id предотвращает перезапуск по тому же старому запросу.
    int rescuing, emergency, relieving; // Поиск после паузы/OVERLOAD; replacement принадлежит аварийному переносу.
    struct stream_path path; // Фактический путь основной DATA последнего прохода; временное перекрытие не добавляет часть.
    int working; // ACTIVE, есть DATA, метрика живая и закрытие не запрошено.
};
struct stream_dependency {
    struct stream_address node;
    unsigned parts; // Число различных работающих частей через этот промежуточный узел.
};
struct stream_rescue_part {
    struct STREAM_RECEIVE *probes[STREAM_RESCUE_PEERS];
    uint64_t deadlines[STREAM_RESCUE_PEERS], choose_ms;
    int active;
    unsigned reported; // По одному диагностическому снимку каждого ответа в раунде.
};
struct stream_rescue_round {
    struct stream_rescue_round *next;
    struct stream_address peer, nodes[STREAM_RESCUE_PEERS];
    uint64_t id;
    struct stream_rescue_part *parts; // n позиций; один общий набор кандидатов для зависших частей поставщика.
};
struct stream_optimizer {
    struct UTUN_INSTANCE *inst;
    struct stream_descriptor descriptor;
    struct stream_supplier *suppliers;
    struct stream_selection *selected;
    struct stream_receive_reservation *reservation; // Атомарный минимум k; держится до END/suspend/destroy.
    struct stream_dependency *dependencies; // Буфер n*(STREAM_PATH_MAX-1); сортируется/сворачивается без allocation в tick.
    unsigned dependency_count;
    struct stream_selection_settings settings;
    struct stream_selection_info info;
    struct stream_probe_schedule schedule;
    void *startup_timer;
    uint64_t started_ms;
    struct stream_address startup_peer;
    uint64_t roster_ms, recovery_ms, start_seq, diagnostic_ms, deficient_ms, relay_poll_ms;
    uint64_t recovery_seq; // Позиция, на которой остановился decode; последующий GAP не доказывает её восстановление.
    uint64_t standby_ms; // Срок пересмотра небольшого набора резервов.
    uint64_t live_since_ms, live_retry_ms, live_refresh_ms; // Подтверждение превышения, срок следующего скачка и обновления предложений.
    uint64_t live_key_seq; // Последний запрошенный свежий KEY; один KEY не перезапрашивается из-за того же отставания.
    unsigned priority_run; // После двух приоритетных INFO гарантирован один шаг полного обхода.
    unsigned startup_budget; // Неиспользованные допуски burst32 для появившегося во время bootstrap поставщика.
    int media, recovering, draining, startup_complete;
    struct stream_rescue_round *rescues;
    void *rescue_timer;
    uint64_t rescue_timer_ms, rescue_serial;
    uint32_t random;
};

static void rescue_rounds_close(struct stream_optimizer *o);
static int rescue_uses_peer(struct stream_optimizer *o, struct stream_address node);

/* Переходы фаз логируются один раз; phase_since_ms сохраняется до следующего перехода. */
static void phase_set(struct stream_optimizer *o, enum stream_delivery_phase phase, uint64_t now) {
    if (o->info.phase == phase) return;
    static const char *names[] = {"none", "bootstrap", "catchup", "live", "recovery", "waiting", "draining", "finished"};
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream delivery phase source=%s epoch=%llx from=%s to=%s"
        " previous_ms=%llu working_parts=%u required=%u lag_ms=%llu/%d waiting_KEY=%d",
        STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
        names[o->info.phase], names[phase], (unsigned long long)(o->info.phase_since_ms ? now - o->info.phase_since_ms : 0),
        o->info.working_parts, (unsigned)(o->descriptor.n - o->descriptor.r),
        (unsigned long long)o->info.live_lag_ms, o->info.live_lag_valid, o->info.live_waiting_key);
    o->info.phase = phase; o->info.phase_since_ms = now;
}

/* Фаза выводится из фактов доставки, а не из часов показа одного из независимых viewers. */
static void phase_update(struct stream_optimizer *o, const struct stream_local *local, uint64_t now) {
    enum stream_delivery_phase phase = STREAM_DELIVERY_LIVE;
    int rescuing = 0;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) rescuing |= o->selected[sub].rescuing;
    if (local->waiting) phase = STREAM_DELIVERY_WAITING;
    else if (local->finished) phase = local->last == UINT64_MAX || (local->anchored && local->next > local->last)
        ? STREAM_DELIVERY_FINISHED : STREAM_DELIVERY_DRAINING;
    else if (!o->startup_complete) phase = STREAM_DELIVERY_BOOTSTRAP;
    else if (o->recovering || (rescuing && o->info.working_parts < (unsigned)(o->descriptor.n - o->descriptor.r)))
        phase = STREAM_DELIVERY_RECOVERY;
    else if (!local->decoded || o->live_since_ms || o->info.live_waiting_key) phase = STREAM_DELIVERY_CATCHUP;
    phase_set(o, phase, now);
}

static void startup_timeout(void *arg) {
    struct stream_optimizer *o = arg; o->startup_timer = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream bootstrap relay deadline group=%llx source=%s epoch=%llx elapsed_ms=%llu delay_ms=%u",
        (unsigned long long)o->descriptor.group_id, STREAM_ADDRESS_TEXT(o->descriptor.source),
        (unsigned long long)o->descriptor.source_epoch, (unsigned long long)(get_time_tb() / 10 - o->started_ms),
        STREAM_BOOTSTRAP_RELAY_MS);
    stream_network_wake(stream_network_get(o->inst));
}

unsigned stream_probe_schedule_tick(struct stream_probe_schedule *s, uint64_t now, unsigned pending, unsigned active) {
    if (!s) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream probe schedule missing"); return 0; }
    unsigned budget = 1;
    if (!s->cycle) { s->cycle = 1; s->started_ms = now; budget = STREAM_PROBE_BURST; }
    else if (now < s->next_ms) return 0;
    else if (!pending && !active && now - s->started_ms >= STREAM_PROBE_CYCLE_MS) { s->cycle++; s->started_ms = now; }
    s->next_ms = now + STREAM_PROBE_PERIOD_MS; return budget;
}

static struct stream_supplier *supplier_find(struct stream_optimizer *o, struct stream_address node) {
    for (struct stream_supplier *s = o->suppliers; s; s = s->next) if (stream_address_equal(s->node, node)) return s;
    return NULL;
}

static uint64_t backoff(unsigned failures) {
    return (uint64_t)STREAM_PROBE_CYCLE_MS << (failures > 4 ? 4 : failures - 1);
}

/* Исчезновение кандидата запрещает новые пробы, но не отменяет уже открытую DATA-выдачу. */
static int supplier_selected(struct stream_optimizer *o, struct stream_address peer) {
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct STREAM_RECEIVE *handles[] = {o->selected[sub].current, o->selected[sub].replacement};
        for (unsigned i = 0; i < 2; i++) if (handles[i]) {
            struct stream_receive_info info; stream_receive_get_info(handles[i], &info);
            if (stream_address_equal(info.peer, peer)) return 1;
        }
    }
    return 0;
}

static void roster_update(struct stream_optimizer *o, struct STREAM_HANDLE *h, uint64_t now) {
    struct stream_candidate *candidates = NULL; size_t count = 0;
    if (stream_get_candidates(h, &candidates, &count) != STREAM_OK) return;
    for (struct stream_supplier *s = o->suppliers; s; s = s->next) s->seen = 0;
    for (size_t i = 0; i < count; i++) {
        struct stream_supplier *s = supplier_find(o, candidates[i].address);
        if (!s) {
            s = u_calloc(1, sizeof(*s));
            if (!s) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream optimizer candidate allocation failed"); continue; }
            s->node = candidates[i].address; s->next = o->suppliers; o->suppliers = s;
            s->rank = rescue_random(o);
        }
        if (!s->online) {
            s->cycle = s->retry_ms = s->local_retry_ms = s->admission_retry_ms = 0;
            s->availability_retry_ms = s->availability_until_ms = 0; s->failures = 0;
            if (s->offers) memset(s->offers, 0, o->descriptor.n * sizeof(*s->offers));
            s->refresh = 1; o->standby_ms = 0;
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream supplier available group=%llx source=%s peer=%s refresh_INFO=1",
                (unsigned long long)o->descriptor.group_id, STREAM_ADDRESS_TEXT(o->descriptor.source),
                STREAM_ADDRESS_TEXT(s->node));
        }
        s->online = s->seen = 1; s->source = candidates[i].source;
        s->relay = stream_relay_confirmed(o->inst, &o->descriptor, s->node);
        int compatible = node_conn_direct_compatible_key(o->inst, s->node.ed);
        if (compatible && !s->compatible && s->cycle != o->schedule.cycle && now >= s->retry_ms) {
            s->refresh = 1;
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream optimizer new supplier wakes probe group=%llx source=%s peer=%s",
                        (unsigned long long)o->descriptor.group_id, STREAM_ADDRESS_TEXT(o->descriptor.source),
                        STREAM_ADDRESS_TEXT(s->node));
        }
        s->compatible = compatible;
    }
    u_free(candidates);
    struct stream_supplier **link = &o->suppliers;
    while (*link) {
        struct stream_supplier *s = *link;
        if (s->seen) { link = &s->next; continue; }
        int retained = supplier_selected(o, s->node);
        if (s->online) DEBUG_DEBUG(DEBUG_CATEGORY_STREAM,
            "stream supplier absent group=%llx source=%s peer=%s preserve_DATA=%d",
            (unsigned long long)o->descriptor.group_id, STREAM_ADDRESS_TEXT(o->descriptor.source),
            STREAM_ADDRESS_TEXT(s->node), retained);
        s->online = 0; stream_receive_close(s->probe); s->probe = NULL;
        if (retained) { link = &s->next; continue; }
        *link = s->next; u_free(s->offers); u_free(s);
    }
    o->roster_ms = now + STREAM_PROBE_PERIOD_MS;
}

/* Один NCD owner пира переиспользуется всеми пробами/выдачами, INFO не открывает DATA.
 * Обновление резерва также завершает его шаг полного обхода, но не начинает новый цикл. */
static int probe_start(struct stream_optimizer *o, struct STREAM_HANDLE *h, struct stream_supplier *s, uint64_t now) {
    s->probe_sub = 0;
    if (!s->offers) s->offers = u_calloc(o->descriptor.n, sizeof(*s->offers));
    if (!s->offers) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream optimizer offers allocation failed"); return 0; }
    struct stream_local local;
    if (stream_local_get(o->inst, &o->descriptor, &local) != STREAM_OK) return 0;
    uint64_t seq = s->offers[0].missing_valid ? s->offers[0].missing_seq : local.anchored ? local.next : UINT64_MAX;
    int result = stream_probe_open(h, s->node, 0, seq, &s->probe);
    if (result == STREAM_LOCAL_BUSY) {
        s->local_retry_ms = now + STREAM_LOCAL_RETRY_MS; o->schedule.next_ms = s->local_retry_ms;
        return 0;
    }
    s->cycle = o->schedule.cycle; s->local_retry_ms = 0; s->checked_ms = now;
    s->standby_ms = now + STREAM_STANDBY_PERIOD_MS + rescue_random(o) % 101; s->refresh = 0;
    if (result != STREAM_OK) {
        s->failures++; s->retry_ms = now + backoff(s->failures) + rescue_random(o) % 101;
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream optimizer probe refused peer=%s result=%d retry_ms=%llu",
                   STREAM_ADDRESS_TEXT(s->node), result, (unsigned long long)s->retry_ms); return 0;
    }
    o->info.probes_started++;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream optimizer probe peer=%s cycle=%llu ordinal=%llu source=%d standby=%d",
                STREAM_ADDRESS_TEXT(s->node), (unsigned long long)o->schedule.cycle,
                (unsigned long long)o->info.probes_started, s->source, s->standby); return 1;
}

/* Успешный INFO снимает только доказанно устаревшую причину отказа: новую эпоху транспорта либо появившуюся часть.
 * Обычный живой ответ не отменяет штраф DATA-отказа или аварийный backoff молчащего резерва. */
static void offer_update(struct stream_offer *offer, const struct stream_receive_info *info) {
    int epoch = offer->info.local_epoch && offer->info.peer_epoch
        && (offer->info.local_epoch != info->local_epoch || offer->info.peer_epoch != info->peer_epoch);
    int found = offer->missing_valid && info->requested_present && info->requested_seq <= offer->missing_seq
        && info->contiguous_last >= offer->missing_seq;
    if (epoch || found) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream offer condition changed peer=%s sub=%u epoch=%d missing_found=%d retry_reset=1",
            STREAM_ADDRESS_TEXT(info->peer), info->subblock, epoch, found);
        offer->retry_ms = 0; offer->failures = 0; offer->missing_valid = 0;
    }
    offer->info = *info;
}

static void probes_poll(struct stream_optimizer *o, struct STREAM_HANDLE *h, uint64_t now) {
    struct stream_local local;
    if (stream_local_get(o->inst, &o->descriptor, &local) != STREAM_OK) return;
    for (struct stream_supplier *s = o->suppliers; s; s = s->next) {
        if (!s->probe) continue;
        struct stream_receive_info info; stream_receive_get_info(s->probe, &info);
        if (!info.info_ready && info.state != STREAM_RECEIVE_CLOSED) continue;
        if (info.state == STREAM_RECEIVE_CLOSED) {
            s->offers[s->probe_sub].info.info_ms = 0;
            if (info.reason == STREAM_REASON_POSITION) {
                if (!s->availability_until_ms) s->availability_until_ms = now + STREAM_AVAILABILITY_WAIT_MS;
                if (now < s->availability_until_ms && !s->availability_retry_ms) {
                    s->availability_retry_ms = now + STREAM_AVAILABILITY_RETRY_MS;
                    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream supplier buffer pending source=%s peer=%s sub=%u retry_ms=%u",
                                STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(s->node),
                                s->probe_sub, STREAM_AVAILABILITY_RETRY_MS);
                }
            }
            if (info.reason == STREAM_REASON_TRANSPORT || info.reason == STREAM_REASON_MEMBER || info.reason == STREAM_REASON_PROTOCOL) {
                s->failures++;
                s->retry_ms = now + (info.reason == STREAM_REASON_TRANSPORT && s->local_epoch
                    ? STREAM_TRANSPORT_RETRY_MS : backoff(s->failures)) + rescue_random(o) % 101;
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream optimizer probe closed source=%s peer=%s sub=%u reason=%d"
                    " info_ready=%d local_epoch=%llx peer_epoch=%llx failures=%u retry_in_ms=%llu cycle=%llu",
                    STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(s->node), s->probe_sub, info.reason,
                    info.info_ready, (unsigned long long)info.local_epoch, (unsigned long long)info.peer_epoch, s->failures,
                    (unsigned long long)(s->retry_ms - now), (unsigned long long)s->cycle);
                stream_receive_close(s->probe); s->probe = NULL; continue;
            }
        } else {
            offer_update(&s->offers[s->probe_sub], &info); s->failures = 0; s->retry_ms = 0;
            s->local_epoch = info.local_epoch; s->peer_epoch = info.peer_epoch;
            if (info.accepting && info.free_slots) s->admission_retry_ms = 0;
            else if (!s->admission_retry_ms) s->admission_retry_ms = now + STREAM_PROBE_PERIOD_MS + rescue_random(o) % 101;
            if (local.media && !local.anchored && !info.key_valid && info.last_available != UINT64_MAX) {
                if (!s->availability_until_ms) s->availability_until_ms = now + STREAM_AVAILABILITY_WAIT_MS;
                if (now < s->availability_until_ms) s->availability_retry_ms = now + STREAM_AVAILABILITY_RETRY_MS;
            }
            /* Автор создаёт все n частей одной пачки и вытесняет их вместе. Его диапазон/путь общий;
             * новые DATA-запросы всё равно проходят собственный INFO/ACCEPT, но открываются параллельно. */
            if (s->source) {
                for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
                    struct stream_receive_info part = info; part.subblock = sub; offer_update(&s->offers[sub], &part);
                }
                DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream source offers ready peer=%s parts=%u range=%llu..%llu key=%llu..%llu valid=%d parallel=1",
                    STREAM_ADDRESS_TEXT(s->node), o->descriptor.n,
                    (unsigned long long)info.first_available, (unsigned long long)info.last_available,
                    (unsigned long long)info.key_seq, (unsigned long long)info.key_last_seq, info.key_valid);
                stream_receive_close(s->probe); s->probe = NULL; continue;
            }
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream optimizer offer peer=%s sub=%u range=%llu..%llu hops=%u"
                " rtt=%u/%d chain=%u/%d slots=%u accepting=%d lag=%llu stopped=%d age=%llu/%d queue=%llu queue_age_ms=%llu"
                " source_Bps=%llu/%d receive_Bps=%llu/%d head_pts=%llu video_pts=%llu penalty_ms=%llu score_ms=%llu",
                STREAM_ADDRESS_TEXT(s->node), s->probe_sub, (unsigned long long)info.first_available,
                (unsigned long long)info.last_available, info.path.count, info.rtt_ms, info.rtt_valid,
                info.path_rtt_ms, info.path_rtt_valid, info.free_slots, info.accepting,
                (unsigned long long)info.supplier_metrics.lag_ms, info.supplier_metrics.stopped,
                (unsigned long long)info.progress_age_ms, info.progress_valid, (unsigned long long)info.supplier_queue_bytes,
                (unsigned long long)info.supplier_queue_age_ms,
                (unsigned long long)info.supplier_metrics.source_rate_bytes_sec, info.supplier_metrics.source_rate_valid,
                (unsigned long long)info.supplier_metrics.receive_rate_bytes_sec, info.supplier_metrics.receive_rate_valid,
                (unsigned long long)info.live_head_us, (unsigned long long)info.media_pts_us,
                (unsigned long long)quality_penalty(&info, &info.supplier_metrics, info.supplier_metrics_valid),
                (unsigned long long)stream_supplier_score(&info, &info.supplier_metrics, info.supplier_metrics_valid, now));
        }
        unsigned next = s->probe_sub + 1;
        if (next == o->descriptor.n) { stream_receive_close(s->probe); s->probe = NULL; continue; }
        uint64_t seq = s->offers[next].missing_valid ? s->offers[next].missing_seq : local.anchored ? local.next : UINT64_MAX;
        if (info.state == STREAM_RECEIVE_CLOSED) {
            stream_receive_close(s->probe); s->probe = NULL;
            int result = stream_probe_open(h, s->node, (uint8_t)next, seq, &s->probe);
            if (result == STREAM_LOCAL_BUSY) {
                s->refresh = 1; s->local_retry_ms = now + STREAM_LOCAL_RETRY_MS; o->schedule.next_ms = s->local_retry_ms; continue;
            }
            if (result != STREAM_OK) {
                s->failures++; s->retry_ms = now + backoff(s->failures) + rescue_random(o) % 101; continue;
            }
        } else if (stream_probe_next(s->probe, (uint8_t)next, seq) != STREAM_OK) continue;
        s->probe_sub = next;
    }
}

/* Старейший непроверенный кандидат гарантирует продвижение обзора; rank распределяет равные варианты между зрителями. */
static struct stream_supplier *probe_choose(struct stream_optimizer *o, uint64_t now, int priority) {
    struct stream_supplier *best = NULL;
    for (struct stream_supplier *s = o->suppliers; s; s = s->next) {
        if (!s->online || !s->compatible || s->probe || now < s->retry_ms || now < s->local_retry_ms
            || rescue_uses_peer(o, s->node)) continue;
        if (priority ? !s->refresh && !(s->standby && now >= s->standby_ms) : s->cycle == o->schedule.cycle) continue;
        if (!best || s->checked_ms < best->checked_ms || (s->checked_ms == best->checked_ms
            && (s->relay > best->relay || (s->relay == best->relay && (s->source > best->source
                || (s->source == best->source && s->rank < best->rank)))))) best = s;
    }
    return best;
}

static void probes_schedule(struct stream_optimizer *o, struct STREAM_HANDLE *h, uint64_t now, int needs_data) {
    unsigned pending = 0, active = 0; int bootstrap_retry = 0;
    for (struct stream_supplier *s = o->suppliers; s; s = s->next) {
        if (!s->online) continue;
        if (needs_data && !s->probe && s->admission_retry_ms && now >= s->admission_retry_ms
            && s->compatible && now >= s->retry_ms) {
            s->admission_retry_ms = 0; s->refresh = 1;
            /* next_ms не сбрасывается: кандидаты делят обычный бюджет одной новой пробы в секунду. */
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream admission recheck self=%llx group=%llx source=%s epoch=%llx peer=%s"
                " reason=DATA_deficit budget=ordinary_probe",
                (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
                STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
                STREAM_ADDRESS_TEXT(s->node));
        }
        if (s->retry_ms && now >= s->retry_ms) {
            s->retry_ms = 0; s->refresh = 1;
        }
        if (!s->probe && s->availability_retry_ms && now >= s->availability_retry_ms) {
            s->availability_retry_ms = 0;
            if (s->compatible && now < s->availability_until_ms && now >= s->retry_ms) {
                s->refresh = 1;
                if (now - o->started_ms < STREAM_AVAILABILITY_WAIT_MS) bootstrap_retry = 1;
            }
        }
        if (s->probe) active++;
        if (now < s->retry_ms) s->cycle = o->schedule.cycle;
        if (s->compatible && s->cycle != o->schedule.cycle) pending++;
    }
    uint64_t previous = o->schedule.cycle;
    if (!previous) o->startup_budget = STREAM_PROBE_BURST;
    unsigned budget = stream_probe_schedule_tick(&o->schedule, now, pending, active);
    if (!budget && (!o->startup_complete || bootstrap_retry) && o->startup_budget) {
        struct stream_supplier *newcomer = probe_choose(o, now, 1);
        if (newcomer && (!newcomer->checked_ms || bootstrap_retry)) {
            budget = 1; o->schedule.next_ms = now + STREAM_PROBE_PERIOD_MS;
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream bootstrap pending burst source=%s peer=%s remaining=%u",
                STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(newcomer->node), o->startup_budget);
        }
    }
    o->info.cycles = o->schedule.cycle;
    if (o->schedule.cycle != previous) DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream optimizer cycle=%llu",
                                                 (unsigned long long)o->schedule.cycle);
    for (unsigned i = 0; i < budget; i++) {
        struct stream_supplier *scan = probe_choose(o, now, 0), *priority = probe_choose(o, now, 1);
        struct stream_supplier *chosen = priority && (!scan || o->priority_run < 2) ? priority : scan;
        if (!chosen) break;
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream INFO schedule source=%s peer=%s reason=%s cycle=%llu priority_run=%u",
            STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(chosen->node),
            chosen == scan ? "scan" : chosen->refresh ? "condition_changed" : "standby_due",
            (unsigned long long)o->schedule.cycle, o->priority_run);
        if (!probe_start(o, h, chosen, now)) break;
        if (o->startup_budget) o->startup_budget--;
        if (chosen == scan) o->priority_run = 0;
        else if (o->priority_run < 2) o->priority_run++;
    }
}

static unsigned usage(struct stream_optimizer *o, struct stream_address node, unsigned except) {
    unsigned count = 0;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct stream_receive_info info;
        if (sub == except || !o->selected[sub].current) continue;
        stream_receive_get_info(o->selected[sub].current, &info);
        if (info.state != STREAM_RECEIVE_CLOSED && stream_address_equal(info.peer, node)) count++;
    }
    return count;
}

/* Доля поздних блоков задаёт штраф в мс; текущая остановка/lag дают максимум, история остановок добавляет 10 мс. */
static uint64_t problem_penalty(const struct stream_metric_snapshot *m, int valid) {
    if (!valid) return 0;
    if (m->stopped || m->lag_ms > STREAM_LATE_MS) return STREAM_PROBLEM_PENALTY_MS;
    double fraction = m->blocks ? (double)m->late_blocks / (double)m->blocks : 0;
    double penalty = fraction * STREAM_PROBLEM_PENALTY_MS + (m->stops ? 10 : 0);
    return penalty >= STREAM_PROBLEM_PENALTY_MS ? STREAM_PROBLEM_PENALTY_MS : (uint64_t)penalty;
}

/* Коррелирующие признаки не складываем: большая очередь и тот же content lag не дают двойного штрафа. */
static uint64_t quality_penalty(const struct stream_receive_info *info, const struct stream_metric_snapshot *m, int valid) {
    uint64_t penalty = problem_penalty(m, valid);
    if (info->supplier_queue_age_ms > penalty) penalty = info->supplier_queue_age_ms;
    /* Частичный relay может отдавать свежую нужную часть, ещё не собрав полный видеокадр. */
    if (info->requested_seq != UINT64_MAX && info->last_available != UINT64_MAX && info->requested_seq > info->last_available
        && info->live_head_us != UINT64_MAX && info->media_pts_us != UINT64_MAX && info->live_head_us > info->media_pts_us) {
        uint64_t lag = (info->live_head_us - info->media_pts_us) / 1000;
        if (lag > penalty) penalty = lag;
    }
    if (valid && m->blocks >= 20 && m->source_rate_valid && m->receive_rate_valid && m->source_rate_bytes_sec
        && m->receive_rate_bytes_sec < m->source_rate_bytes_sec
        && m->source_rate_bytes_sec - m->receive_rate_bytes_sec >= m->source_rate_bytes_sec / 10 + (m->source_rate_bytes_sec % 10 != 0)) {
        double deficit = 1.0 - (double)m->receive_rate_bytes_sec / (double)m->source_rate_bytes_sec;
        uint64_t rate_penalty = (uint64_t)(deficit * STREAM_PROBLEM_PENALTY_MS + 0.5);
        if (rate_penalty > penalty) penalty = rate_penalty;
    }
    return penalty < STREAM_PROBLEM_PENALTY_MS ? penalty : STREAM_PROBLEM_PENALTY_MS;
}

/* path.count включает автора и поставщика: с последним переходом к нам это число hops от автора до получателя. */
uint64_t stream_supplier_score(const struct stream_receive_info *info, const struct stream_metric_snapshot *metrics, int valid, uint64_t now) {
    if (!info || !metrics) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream supplier score invalid arguments"); return UINT64_MAX; }
    if (!info->rtt_valid || !info->path_rtt_valid) return UINT64_MAX;
    uint64_t age = info->info_ms && now > info->info_ms ? now - info->info_ms : 0;
    if (age > STREAM_OFFER_TTL_MS) age = STREAM_OFFER_TTL_MS;
    return (uint64_t)info->path_rtt_ms + 2 * (uint64_t)info->rtt_ms + STREAM_HOP_PENALTY_MS * info->path.count
           + quality_penalty(info, metrics, valid) + age / 1000;
}

static int dependency_compare(const void *left, const void *right) {
    struct stream_address a = ((const struct stream_dependency *)left)->node, b = ((const struct stream_dependency *)right)->node;
    return stream_address_cmp(a, b);
}

/* Только фактические основные DATA-пути. Автор общий по определению, получателя в проверенном пути нет. */
static void dependencies_refresh(struct stream_optimizer *o) {
    unsigned entries = 0, working = 0, maximum = 0; struct stream_address worst = {{0}};
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct stream_selection *pick = &o->selected[sub]; struct stream_receive_info info = {0};
        if (pick->current) stream_receive_get_info(pick->current, &info);
        pick->working = info.state == STREAM_RECEIVE_ACTIVE && info.blocks && info.metrics_valid
            && !info.metrics.stopped && !info.close_requested && info.path.count;
        pick->path = info.path;
        if (!pick->working) continue;
        working++;
        for (unsigned i = 1; i < info.path.count; i++)
            o->dependencies[entries++] = (struct stream_dependency){.node = info.path.nodes[i], .parts = 1};
    }
    if (entries) qsort(o->dependencies, entries, sizeof(*o->dependencies), dependency_compare);
    o->dependency_count = 0;
    for (unsigned i = 0; i < entries; i++) {
        struct stream_dependency *last = o->dependency_count ? &o->dependencies[o->dependency_count - 1] : NULL;
        if (last && stream_address_equal(last->node, o->dependencies[i].node)) last->parts++;
        else { o->dependencies[o->dependency_count++] = o->dependencies[i]; last = &o->dependencies[o->dependency_count - 1]; }
        if (last->parts > maximum) { maximum = last->parts; worst = last->node; }
    }
    unsigned required = o->descriptor.n - o->descriptor.r;
    int protected = o->descriptor.r && working >= required && maximum <= working - required;
    if (protected != o->info.protected_by_fec || maximum != o->info.maximum_dependency || working != o->info.working_parts)
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream path protection source=%s working=%u required=%u dependencies=%u"
            " worst_node=%s dependent_parts=%u protected=%d",
            STREAM_ADDRESS_TEXT(o->descriptor.source), working, required, o->dependency_count,
            STREAM_ADDRESS_TEXT(worst), maximum, protected);
    o->info.working_parts = working; o->info.maximum_dependency = maximum; o->info.protected_by_fec = protected;
}

/* Вклад остальных частей в узел пути кандидата; текущая часть будет заменена и вычитается. */
static unsigned dependency_parts(const struct stream_optimizer *o, struct stream_address node, unsigned sub) {
    unsigned low = 0, high = o->dependency_count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (stream_address_cmp(o->dependencies[middle].node, node) < 0) low = middle + 1; else high = middle;
    }
    unsigned count = low < o->dependency_count && stream_address_equal(o->dependencies[low].node, node) ? o->dependencies[low].parts : 0;
    const struct stream_selection *pick = &o->selected[sub];
    if (count && pick->working) for (unsigned i = 1; i < pick->path.count; i++) if (stream_address_equal(pick->path.nodes[i], node)) { count--; break; }
    return count;
}

/* Сначала максимальная общая зависимость, затем сумма пересечений. Независимый relay и прямой автор равноправны. */
static uint64_t path_dependence(const struct stream_optimizer *o, const struct stream_path *path, unsigned sub) {
    unsigned maximum = 1, shared = 0;
    for (unsigned i = 1; i < path->count; i++) {
        unsigned count = dependency_parts(o, path->nodes[i], sub);
        if (count + 1 > maximum) maximum = count + 1;
        shared += count;
    }
    return (uint64_t)maximum * (RS_MAX_BLOCKS * STREAM_PATH_MAX + 1) + shared;
}

enum supplier_status {
    SUPPLIER_READY, SUPPLIER_OFFLINE, SUPPLIER_TRANSPORT, SUPPLIER_CURRENT, SUPPLIER_NO_INFO, SUPPLIER_BACKOFF,
    SUPPLIER_PROBING, SUPPLIER_STALE, SUPPLIER_BUSY, SUPPLIER_FULL, SUPPLIER_EXPIRED, SUPPLIER_MISSING, SUPPLIER_NO_KEY,
    SUPPLIER_STATUS_COUNT
};
static const char *const supplier_status_names[] = {
    "ready", "offline", "transport_incompatible", "current", "no_INFO", "backoff", "INFO_pending", "stale_INFO",
    "not_accepting", "no_slots", "expired_position", "missing_position", "no_KEY"
};

/* Общий предикат выбора и диагностики: причины в отчёте должны совпадать с реальными фильтрами. */
static enum supplier_status supplier_status(const struct stream_optimizer *o, const struct stream_supplier *s,
                                            unsigned sub, uint64_t seq, struct stream_address exclude, uint64_t now) {
    if (!s->online) return SUPPLIER_OFFLINE;
    if (!s->compatible) return SUPPLIER_TRANSPORT;
    if (stream_address_equal(s->node, exclude)) return SUPPLIER_CURRENT;
    if (!s->offers) return SUPPLIER_NO_INFO;
    const struct stream_offer *offer = &s->offers[sub];
    const struct stream_receive_info *info = &offer->info;
    if (now < s->retry_ms || (now < offer->retry_ms && (!offer->missing_valid || seq == offer->missing_seq))) return SUPPLIER_BACKOFF;
    if (s->probe && s->probe_sub == sub) return SUPPLIER_PROBING;
    if (!info->info_ms) return SUPPLIER_NO_INFO;
    if (now - info->info_ms > STREAM_OFFER_TTL_MS) return SUPPLIER_STALE;
    if (seq != UINT64_MAX && info->first_available != UINT64_MAX && seq < info->first_available) return SUPPLIER_EXPIRED;
    if (offer->missing_valid && seq == offer->missing_seq) return SUPPLIER_MISSING;
    if (seq != UINT64_MAX && info->requested_seq == seq && !info->requested_present
        && info->last_available != UINT64_MAX && seq <= info->last_available) return SUPPLIER_MISSING;
    if (seq == UINT64_MAX && o->media && info->last_available != UINT64_MAX && !info->key_valid) return SUPPLIER_NO_KEY;
    if (!info->accepting) return SUPPLIER_BUSY;
    if (!info->free_slots) return SUPPLIER_FULL;
    return SUPPLIER_READY;
}

/* До трёх резервов на трансляцию: сначала покрытие частей, затем score. INFO не удерживает DATA-подписку.
 * Стабильный rank не меняет выбранный резерв на каждом tick; новое состояние пересматривается не реже раза в секунду. */
static void standby_update(struct stream_optimizer *o, const struct stream_local *local, uint64_t now) {
    if (now < o->standby_ms) return;
    o->standby_ms = now + STREAM_PROBE_PERIOD_MS;
    struct stream_address selected[STREAM_RESCUE_PEERS] = {0}, current[RS_MAX_BLOCKS] = {0};
    unsigned coverage[RS_MAX_BLOCKS] = {0};
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) if (o->selected[sub].current) {
        struct stream_receive_info info; stream_receive_get_info(o->selected[sub].current, &info); current[sub] = info.peer;
    }
    uint64_t seq = local->anchored && local->next ? local->next - 1 : UINT64_MAX;
    for (unsigned i = 0; i < STREAM_RESCUE_PEERS; i++) {
        struct stream_supplier *best = NULL; unsigned best_cover = 0; uint64_t best_score = UINT64_MAX, best_dependence = UINT64_MAX;
        for (struct stream_supplier *s = o->suppliers; s; s = s->next) {
            int duplicate = 0;
            for (unsigned j = 0; j < i; j++) if (stream_address_equal(selected[j], s->node)) duplicate = 1;
            if (duplicate) continue;
            unsigned cover = 0, parts = 0; uint64_t total = 0, dependence = 0;
            for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
                enum supplier_status status = supplier_status(o, s, sub, seq, current[sub], now);
                if (status != SUPPLIER_READY && !(status == SUPPLIER_PROBING && s->standby)) continue;
                struct stream_receive_info *info = &s->offers[sub].info;
                if (!info->info_ms || !info->accepting || !info->free_slots || info->last_available == UINT64_MAX
                    || (info->supplier_metrics_valid && info->supplier_metrics.stopped)) continue;
                uint64_t score = stream_supplier_score(info, &info->supplier_metrics, info->supplier_metrics_valid, now);
                if (score == UINT64_MAX) continue;
                cover += STREAM_RESCUE_PEERS - coverage[sub]; total += score; parts++;
                dependence += path_dependence(o, &info->path, sub);
            }
            if (!cover) continue;
            uint64_t score = total / parts; dependence /= parts;
            uint64_t difference = score < best_score ? best_score - score : score - best_score;
            int better = difference <= o->settings.score_hysteresis_ms && dependence != best_dependence
                ? dependence < best_dependence : score < best_score || (score == best_score && (!best || s->rank < best->rank));
            if (!best || cover > best_cover || (cover == best_cover && better)) {
                best = s; best_cover = cover; best_score = score; best_dependence = dependence;
            }
        }
        if (!best) break;
        selected[i] = best->node;
        for (unsigned sub = 0; sub < o->descriptor.n; sub++)
            if (supplier_status(o, best, sub, seq, current[sub], now) == SUPPLIER_READY) coverage[sub]++;
    }
    for (struct stream_supplier *s = o->suppliers; s; s = s->next) {
        int standby = 0;
        for (unsigned i = 0; i < STREAM_RESCUE_PEERS; i++) standby |= stream_address_equal(selected[i], s->node);
        if (standby != s->standby) DEBUG_DEBUG(DEBUG_CATEGORY_STREAM,
            "stream standby source=%s peer=%s selected=%d INFO_due_ms=%llu last_probe_ms=%llu",
            STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(s->node), standby,
            (unsigned long long)s->standby_ms, (unsigned long long)s->checked_ms);
        s->standby = standby;
    }
}

/* Не сбрасывает полный обход и штраф неисправного поставщика; ускоряет проверку известных альтернатив. */
static void backups_refresh(struct stream_optimizer *o, struct stream_address peer) {
    o->standby_ms = 0;
    for (struct stream_supplier *s = o->suppliers; s; s = s->next)
        if (s->standby && !stream_address_equal(s->node, peer)) s->refresh = 1;
}

/* В пределах гистерезиса предпочитаем независимость всей цепочки; нехватка путей не запрещает полезную DATA. */
static struct stream_supplier *supplier_choose(struct stream_optimizer *o, unsigned sub, uint64_t seq, struct stream_address exclude, uint64_t now) {
    struct stream_supplier *best = NULL;
    for (struct stream_supplier *s = o->suppliers; s; s = s->next) {
        if (supplier_status(o, s, sub, seq, exclude, now) != SUPPLIER_READY) continue;
        struct stream_receive_info *info = &s->offers[sub].info;
        if (!best) { best = s; continue; }
        struct stream_receive_info *previous = &best->offers[sub].info;
        if (!o->startup_complete && seq == UINT64_MAX) {
            if (info->info_order < previous->info_order) best = s;
            continue;
        }
        uint64_t score = stream_supplier_score(info, &info->supplier_metrics, info->supplier_metrics_valid, now);
        uint64_t old_score = stream_supplier_score(previous, &previous->supplier_metrics, previous->supplier_metrics_valid, now);
        int better = score < old_score;
        uint64_t difference = score < old_score ? old_score - score : score - old_score;
        if (difference <= o->settings.score_hysteresis_ms) {
            uint64_t shared = path_dependence(o, &info->path, sub), old_shared = path_dependence(o, &previous->path, sub);
            int crowded = o->descriptor.r && usage(o, s->node, sub) >= o->descriptor.r;
            int old_crowded = o->descriptor.r && usage(o, best->node, sub) >= o->descriptor.r;
            if (shared != old_shared) better = shared < old_shared;
            else if (crowded != old_crowded) better = !crowded;
            else if (score == old_score) better = s->rank < best->rank;
        }
        if (better) best = s;
    }
    return best;
}

/* Не чаще раза в секунду и только при включённом stream=debug. Никаких чтений содержимого кольца.
 * DEBUG даёт сводку и проблемные части, TRACE — стабильные части и каждый отвергнутый обзорный INFO. */
static void optimizer_report(struct stream_optimizer *o, const struct stream_local *local, uint64_t now) {
    if (now < o->diagnostic_ms || !debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_STREAM)) return;
    o->diagnostic_ms = now + 1000;
    unsigned rescuing = 0, standby = 0;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) rescuing += o->selected[sub].rescuing;
    for (struct stream_supplier *s = o->suppliers; s; s = s->next) standby += s->standby;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream SELECTOR self=%llx group=%llx source=%s epoch=%llx sample_ms=%llu next=%llu"
        " anchored=%d decode_idle_ms=%llu recovering=%d rescue=%u candidates=%u probes=%u subflows=%u overlaps=%u"
        " probes_total=%llu migrations=%llu failures=%llu restarts=%llu standby=%u priority_run=%u scan_cycle=%llu"
        " working_parts=%u maximum_dependency=%u protected_by_fec=%d live_lag_ms=%llu/%d live_jumps=%llu waiting_KEY=%d",
        (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
        STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
        (unsigned long long)now, (unsigned long long)local->next, local->anchored,
        (unsigned long long)(now - (local->progress_ms ? local->progress_ms : o->started_ms)), o->recovering, rescuing,
        o->info.candidates, o->info.probes_active, o->info.subflows, o->info.overlaps,
        (unsigned long long)o->info.probes_started, (unsigned long long)o->info.migrations,
        (unsigned long long)o->info.failures, (unsigned long long)o->info.restarts, standby, o->priority_run,
        (unsigned long long)o->schedule.cycle, o->info.working_parts, o->info.maximum_dependency, o->info.protected_by_fec,
        (unsigned long long)o->info.live_lag_ms, o->info.live_lag_valid, (unsigned long long)o->info.live_jumps, o->info.live_waiting_key);
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct stream_selection *pick = &o->selected[sub]; struct stream_receive_info current = {0};
        if (pick->current) stream_receive_get_info(pick->current, &current);
        debug_level_t level = !pick->current || pick->rescuing || pick->replacement || current.metrics.stopped
            ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_TRACE;
        if (!debug_should_output(level, DEBUG_CATEGORY_STREAM)) continue;
        uint64_t seq = local->anchored ? (pick->opened ? local->next : local->start) : o->start_seq;
        unsigned reasons[SUPPLIER_STATUS_COUNT] = {0};
        for (struct stream_supplier *s = o->suppliers; s; s = s->next) {
            enum supplier_status status = supplier_status(o, s, sub, seq, current.peer, now); reasons[status]++;
            if (status != SUPPLIER_READY && status != SUPPLIER_CURRENT) DEBUG_TRACE(DEBUG_CATEGORY_STREAM,
                "stream candidate rejected self=%llx group=%llx source=%s epoch=%llx sub=%u peer=%s need=%llu reason=%s",
                (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
                STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
                sub, STREAM_ADDRESS_TEXT(s->node), (unsigned long long)seq, supplier_status_names[status]);
        }
        char candidates[512] = {0}; size_t used = 0;
        for (unsigned i = 0; i < SUPPLIER_STATUS_COUNT; i++) if (reasons[i]) {
            int written = snprintf(candidates + used, sizeof(candidates) - used, "%s%s:%u", used ? "," : "",
                supplier_status_names[i], reasons[i]);
            if (written < 0 || (size_t)written >= sizeof(candidates) - used) {
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream candidate diagnostic truncated sub=%u reason=%u", sub, i); break;
            }
            used += (size_t)written;
        }
        const char *state = pick->rescuing ? (pick->emergency ? "reserve_DATA" : "reserve_INFO")
            : pick->replacement ? "overlap" : !pick->current ? "no_supplier"
            : current.state != STREAM_RECEIVE_ACTIVE ? "connecting" : current.metrics.stopped ? "DATA_paused_unknown_cause" : "receiving";
        debug_output(level, DEBUG_CATEGORY_STREAM, __FUNCTION__, __FILE__, __LINE__,
            "stream CHOICE self=%llx group=%llx source=%s epoch=%llx sub=%u peer=%s state=%s need=%llu rescue_need=%llu"
            " data_next=%llu progress_ms=%llu INFO_age_ms=%llu/%d lag_ms=%llu/%d queue_B=%llu queue_age_ms=%llu"
            " cpu_bp=%u/%d score_ms=%llu rtt_ms=%u/%d chain_ms=%u/%d hops=%u penalty_ms=%llu"
            " source_Bps=%llu/%d receive_Bps=%llu/%d head_pts=%llu video_pts=%llu ordinary_offer_filters={%s}",
            (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
            STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
            sub, STREAM_ADDRESS_TEXT(current.peer), state, (unsigned long long)seq, (unsigned long long)pick->rescue_seq,
            (unsigned long long)current.data_next, (unsigned long long)current.progress_ms,
            (unsigned long long)(current.info_ms ? now - current.info_ms : 0), current.info_ms != 0,
            (unsigned long long)current.metrics.lag_ms, current.metrics_valid,
            (unsigned long long)current.supplier_queue_bytes, (unsigned long long)current.supplier_queue_age_ms,
            current.cpu_basis_points, current.cpu_valid, (unsigned long long)stream_supplier_score(&current, &current.metrics, current.metrics_valid, now),
            current.rtt_ms, current.rtt_valid, current.path_rtt_ms, current.path_rtt_valid, current.path.count,
            (unsigned long long)quality_penalty(&current, &current.metrics, current.metrics_valid),
            (unsigned long long)current.metrics.source_rate_bytes_sec, current.metrics.source_rate_valid,
            (unsigned long long)current.metrics.receive_rate_bytes_sec, current.metrics.receive_rate_valid,
            (unsigned long long)current.live_head_us, (unsigned long long)current.media_pts_us, candidates);
    }
}

/* Первый пригодный INFO среди всех субпотоков выигрывает гонку; порядок приёма не зависит от порядка списка пиров. */
static struct stream_supplier *startup_choose(struct stream_optimizer *o, uint64_t now, unsigned *subblock) {
    struct stream_supplier *first = NULL; uint64_t order = UINT64_MAX;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct stream_supplier *s = supplier_choose(o, sub, UINT64_MAX, (struct stream_address){{0}}, now);
        if (s && s->offers[sub].info.info_order < order) {
            first = s; order = s->offers[sub].info.info_order; *subblock = sub;
        }
    }
    return first;
}

static void receive_failed(struct stream_optimizer *o, struct STREAM_RECEIVE **handle, unsigned sub, uint64_t now) {
    struct stream_receive_info info; stream_receive_get_info(*handle, &info);
    if (handle == &o->selected[sub].current) o->selected[sub].relief_id = 0;
    backups_refresh(o, info.peer);
    struct stream_supplier *s = supplier_find(o, info.peer);
    if (s && s->offers) {
        struct stream_offer *offer = &s->offers[sub];
        s->refresh = 1;
        if ((info.reason == STREAM_REASON_POSITION || info.reason == STREAM_REASON_EXPIRED) && info.start != UINT64_MAX) {
            offer->missing_valid = 1; offer->missing_seq = info.start;
        }
        int resynchronized = info.reason == STREAM_REASON_TRANSPORT && info.local_epoch && info.peer_epoch
            && s->local_epoch && s->peer_epoch && (info.local_epoch != s->local_epoch || info.peer_epoch != s->peer_epoch);
        if (resynchronized) { offer->failures = 0; offer->retry_ms = 0; }
        else {
            offer->failures++;
            offer->retry_ms = now + (info.reason == STREAM_REASON_TRANSPORT && info.blocks
                ? STREAM_TRANSPORT_RETRY_MS : backoff(offer->failures)) + rescue_random(o) % 101;
        }
        if (info.reason == STREAM_REASON_OVERLOAD) s->retry_ms = offer->retry_ms;
    }
    o->info.failures++;
    DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream optimizer supplier failed peer=%s sub=%u reason=%d next=%llu",
               STREAM_ADDRESS_TEXT(info.peer), sub, info.reason, (unsigned long long)info.next);
    stream_receive_close(*handle); *handle = NULL;
}

static int improved(struct stream_optimizer *o, unsigned sub, struct stream_supplier *s, const struct stream_receive_info *current, uint64_t now) {
    struct stream_receive_info *offer = &s->offers[sub].info;
    if (!current->info_ms || now - current->info_ms > STREAM_STANDBY_PERIOD_MS || !current->path_rtt_valid || !current->rtt_valid) {
        struct stream_supplier *old = supplier_find(o, current->peer);
        if (old && !old->probe && !old->refresh) {
            old->refresh = 1;
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream current offer refresh peer=%s sub=%u INFO_age_ms=%llu rtt_valid=%d chain_valid=%d",
                STREAM_ADDRESS_TEXT(current->peer), sub, (unsigned long long)(current->info_ms ? now - current->info_ms : 0),
                current->rtt_valid, current->path_rtt_valid);
        }
        return 0;
    }
    uint64_t before = stream_supplier_score(current, &current->metrics, current->metrics_valid, now);
    uint64_t after = stream_supplier_score(offer, &offer->supplier_metrics, offer->supplier_metrics_valid, now);
    /* Неизвестный RTT живой цепочки не доказывает улучшения: сначала её приоритетный INFO, затем сравнение. */
    if (before == UINT64_MAX || after == UINT64_MAX) return 0;
    uint64_t difference = before < after ? after - before : before - after;
    if (difference <= o->settings.score_hysteresis_ms) {
        uint64_t old_path = path_dependence(o, &current->path, sub), new_path = path_dependence(o, &offer->path, sub);
        if (old_path != new_path) return new_path < old_path;
        if (o->descriptor.r && usage(o, current->peer, sub) >= o->descriptor.r && usage(o, s->node, sub) < o->descriptor.r) return 1;
    }
    return after < before && difference >= o->settings.score_hysteresis_ms;
}

/* Случайность нужна для распределения зрителей, не для протокольных идентификаторов. */
static uint32_t rescue_random(struct stream_optimizer *o) {
    uint32_t x = o->random; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return o->random = x;
}

static uint64_t rescue_timeout(const struct stream_receive_info *info, uint64_t minimum) {
    uint64_t rtt = info->rtt_valid ? (uint64_t)info->rtt_ms * 4 : 0;
    return rtt > minimum ? rtt : minimum;
}

static void rescue_part_close(struct stream_rescue_part *part) {
    for (unsigned i = 0; i < STREAM_RESCUE_PEERS; i++) {
        stream_receive_close(part->probes[i]); part->probes[i] = NULL;
    }
    part->active = 0; part->choose_ms = 0;
}

static void rescue_rounds_close(struct stream_optimizer *o) {
    if (o->rescue_timer) uasync_cancel_timeout(o->inst->ua, o->rescue_timer);
    o->rescue_timer = NULL; o->rescue_timer_ms = 0;
    while (o->rescues) {
        struct stream_rescue_round *round = o->rescues; o->rescues = round->next;
        for (unsigned sub = 0; sub < o->descriptor.n; sub++) rescue_part_close(&round->parts[sub]);
        u_free(round->parts); u_free(round);
    }
}

static int rescue_uses_peer(struct stream_optimizer *o, struct stream_address node) {
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) if (o->selected[sub].emergency) {
        struct stream_receive_info info; stream_receive_get_info(o->selected[sub].replacement, &info);
        if (stream_address_equal(info.peer, node)) return 1;
    }
    for (struct stream_rescue_round *round = o->rescues; round; round = round->next)
        for (unsigned i = 0; i < STREAM_RESCUE_PEERS; i++) if (stream_address_equal(round->nodes[i], node)) return 1;
    return 0;
}

static struct stream_rescue_part *rescue_part_find(struct stream_optimizer *o, unsigned sub) {
    for (struct stream_rescue_round *round = o->rescues; round; round = round->next)
        if (round->parts[sub].active) return &round->parts[sub];
    return NULL;
}

static void rescue_failure(struct stream_optimizer *o, struct stream_address node, unsigned sub, uint64_t now, const char *reason) {
    struct stream_supplier *s = supplier_find(o, node); uint64_t delay = 1000;
    if (s && s->offers) {
        struct stream_offer *offer = &s->offers[sub];
        if (offer->rescue_failures < 6) offer->rescue_failures++;
        delay <<= offer->rescue_failures - 1;
        if (delay > 30000) delay = 30000;
        uint64_t jitter = rescue_random(o) % 101;
        delay = delay == 30000 ? delay - jitter : delay + jitter;
        offer->rescue_retry_ms = now + delay;
    }
    o->info.failures++;
    DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream rescue failed self=%llx group=%llx source=%s epoch=%llx peer=%s sub=%u reason=%s retry_ms=%llu",
        (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
        STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
        STREAM_ADDRESS_TEXT(node), sub, reason, (unsigned long long)delay);
}

static uint64_t rescue_position(const struct stream_local *local, unsigned sub, uint64_t from) {
    uint64_t seq = from < local->next ? from : local->next, actual; const uint8_t *wire; size_t length;
    /* При живом FEC восстанавливаем избыточную часть у последней собранной пачки: старая история
     * уже выдана consumer и могла исчезнуть у всех поставщиков за время долгого отказа этой части. */
    if (local->descriptor.r && seq + 1 < local->next
        && stream_buffer_next_decodable(local->buffer, local->next - 1, &actual) == STREAM_OK && actual == local->next - 1)
        seq = actual;
    struct stream_buffer_stats stats; stream_buffer_get_stats(local->buffer, &stats);
    if (seq < stats.expired_before) seq = stats.expired_before;
    while (seq < UINT64_MAX && stream_buffer_read(local->buffer, seq, sub, &wire, &length, &actual) == STREAM_OK) seq++;
    return local->finished && (local->last == UINT64_MAX || seq > local->last) ? UINT64_MAX : seq;
}

/* Сравниваются позиции одной source_epoch; это не оценка абсолютной live-задержки в миллисекундах. */
static int rescue_compare(struct stream_optimizer *o, unsigned sub, const struct stream_receive_info *a, const struct stream_receive_info *b) {
    if (a->last_available != b->last_available) return a->last_available > b->last_available ? -1 : 1;
    if (a->contiguous_last != b->contiguous_last) return a->contiguous_last > b->contiguous_last ? -1 : 1;
    if (a->supplier_queue_age_ms != b->supplier_queue_age_ms) return a->supplier_queue_age_ms < b->supplier_queue_age_ms ? -1 : 1;
    unsigned acpu = a->cpu_valid ? a->cpu_basis_points : 10001, bcpu = b->cpu_valid ? b->cpu_basis_points : 10001;
    if (acpu != bcpu) return acpu < bcpu ? -1 : 1;
    uint64_t artt = a->rtt_valid ? a->rtt_ms : UINT64_MAX, brtt = b->rtt_valid ? b->rtt_ms : UINT64_MAX;
    uint64_t difference = artt < brtt ? brtt - artt : artt - brtt;
    if (difference <= o->settings.score_hysteresis_ms) {
        uint64_t adep = path_dependence(o, &a->path, sub), bdep = path_dependence(o, &b->path, sub);
        if (adep != bdep) return adep < bdep ? -1 : 1;
    }
    return artt < brtt ? -1 : artt > brtt ? 1 : 0;
}

static int rescue_suitable(const struct stream_receive_info *info, uint64_t seq) {
    return info->state == STREAM_RECEIVE_INFO && info->info_ready && info->requested_present && info->requested_seq == seq
        && info->contiguous_last >= seq && info->accepting && info->free_slots;
}

/* Один набор из максимум трёх узлов на зависшего поставщика; все его части проверяются параллельно.
 * Неизвестный/отказавший кандидат не блокирует остальные. Лимиты network общие для всех трансляций. */
static int rescue_round_open(struct stream_optimizer *o, struct STREAM_HANDLE *h, struct stream_address peer, uint64_t now) {
    struct stream_rescue_round *round = u_calloc(1, sizeof(*round));
    if (round) round->parts = u_calloc(o->descriptor.n, sizeof(*round->parts));
    if (!round || !round->parts) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream rescue round allocation failed"); if (round) u_free(round); return STREAM_ERR_MEMORY;
    }
    int local_wait = 0;
    round->peer = peer; round->id = ++o->rescue_serial;
    for (unsigned i = 0; i < STREAM_RESCUE_PEERS; i++) {
        struct stream_supplier *chosen = NULL; unsigned ties = 0, best_parts = 0, best_fresh = 0; uint64_t best_dependence = 0;
        for (struct stream_supplier *s = o->suppliers; s; s = s->next) {
            if (!s->online || !s->compatible) continue;
            int duplicate = 0;
            for (unsigned j = 0; j < i; j++) if (stream_address_equal(round->nodes[j], s->node)) duplicate = 1;
            if (duplicate) continue;
            unsigned parts = 0, fresh = 0; uint64_t dependence = 0;
            for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
                struct stream_selection *pick = &o->selected[sub];
                if (!pick->rescuing || pick->replacement || !stream_address_equal(pick->rescue_peer, peer) || now < pick->rescue_retry_ms
                    || rescue_part_find(o, sub) || (pick->current && stream_address_equal(s->node, peer))) continue;
                if (!s->offers || now >= s->offers[sub].rescue_retry_ms) {
                    parts++;
                    if (s->offers) {
                        struct stream_receive_info *info = &s->offers[sub].info;
                        if (info->info_ms && now - info->info_ms <= STREAM_STANDBY_PERIOD_MS + STREAM_PROBE_PERIOD_MS
                            && info->accepting && info->free_slots && info->first_available <= pick->rescue_seq
                            && info->last_available != UINT64_MAX && info->last_available >= pick->rescue_seq
                            && !(info->requested_seq == pick->rescue_seq && !info->requested_present)) {
                            fresh++; dependence += path_dependence(o, &info->path, sub);
                        }
                    }
                }
            }
            if (!parts) continue;
            if (chosen && !stream_address_equal(chosen->node, peer) && stream_address_equal(s->node, peer)) continue;
            if (!chosen || (stream_address_equal(chosen->node, peer) && !stream_address_equal(s->node, peer)) || fresh > best_fresh
                || (fresh == best_fresh && (parts > best_parts || (parts == best_parts && dependence < best_dependence)))) {
                chosen = s; best_parts = parts; best_fresh = fresh; best_dependence = dependence; ties = 1;
            } else if (fresh == best_fresh && parts == best_parts && dependence == best_dependence && rescue_random(o) % ++ties == 0) chosen = s;
        }
        if (!chosen) break;
        round->nodes[i] = chosen->node;
        if (!chosen->offers) chosen->offers = u_calloc(o->descriptor.n, sizeof(*chosen->offers));
        if (!chosen->offers) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream rescue offers allocation failed"); continue; }
        /* Отменённая фоновая проба не отнимает слот точного запроса. */
        stream_receive_close(chosen->probe); chosen->probe = NULL;
        for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
            struct stream_selection *pick = &o->selected[sub]; struct stream_rescue_part *part = &round->parts[sub];
            if (!pick->rescuing || pick->replacement || !stream_address_equal(pick->rescue_peer, peer) || now < pick->rescue_retry_ms
                || rescue_part_find(o, sub) || (pick->current && stream_address_equal(chosen->node, peer))
                || now < chosen->offers[sub].rescue_retry_ms) continue;
            int result = stream_probe_open_at(h, chosen->node, (uint8_t)sub, pick->rescue_seq, &part->probes[i]);
            if (result == STREAM_LOCAL_BUSY) { local_wait = 1; continue; }
            if (result != STREAM_OK) {
                rescue_failure(o, chosen->node, sub, now, "probe_open"); continue;
            }
            part->active = 1; o->info.probes_started++;
            part->deadlines[i] = now + rescue_timeout(&chosen->offers[sub].info, 1000);
        }
    }
    unsigned parts = 0;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) parts += round->parts[sub].active;
    if (!parts) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream rescue no available probes source=%s old=%s round=%llu local_budget=%d",
            STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(peer), (unsigned long long)round->id, local_wait);
        u_free(round->parts); u_free(round); return local_wait ? STREAM_LOCAL_BUSY : STREAM_WAIT;
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream rescue round self=%llx group=%llx source=%s epoch=%llx round=%llu"
        " old=%s peers=%s,%s,%s parts=%u",
        (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
        STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
        (unsigned long long)round->id, STREAM_ADDRESS_TEXT(peer), STREAM_ADDRESS_TEXT(round->nodes[0]),
        STREAM_ADDRESS_TEXT(round->nodes[1]), STREAM_ADDRESS_TEXT(round->nodes[2]), parts);
    round->next = o->rescues; o->rescues = round;
    return local_wait ? STREAM_LOCAL_BUSY : STREAM_OK;
}

static void rescue_rounds_poll(struct stream_optimizer *o, uint64_t now) {
    struct stream_rescue_round **link = &o->rescues;
    while (*link) {
        struct stream_rescue_round *round = *link; unsigned active = 0;
        for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
            struct stream_rescue_part *part = &round->parts[sub]; struct stream_selection *pick = &o->selected[sub];
            if (!part->active) continue;
            if (!pick->rescuing) { rescue_part_close(part); continue; }
            struct stream_receive_info replies[STREAM_RESCUE_PEERS] = {0}; unsigned pending = 0;
            for (unsigned i = 0; i < STREAM_RESCUE_PEERS; i++) {
                if (!part->probes[i]) continue;
                struct stream_receive_info *info = &replies[i]; stream_receive_get_info(part->probes[i], info);
                if (info->state == STREAM_RECEIVE_CLOSED || (info->info_ready ? info->info_ms > part->deadlines[i] : now >= part->deadlines[i])) {
                    if (info->reason != STREAM_REASON_POSITION && info->reason != STREAM_REASON_EXPIRED)
                        rescue_failure(o, round->nodes[i], sub, now, info->state == STREAM_RECEIVE_CLOSED ? "INFO_closed" : "INFO_timeout");
                    stream_receive_close(part->probes[i]); part->probes[i] = NULL; continue;
                }
                if (!info->info_ready) { pending++; continue; }
                if (!(part->reported & (1u << i))) {
                    part->reported |= 1u << i;
                    const char *decision = !info->requested_present ? "missing_position" : !info->accepting ? "not_accepting"
                        : !info->free_slots ? "no_slots" : rescue_suitable(info, pick->rescue_seq) ? "eligible" : "position_or_state";
                    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream rescue offer self=%llx group=%llx source=%s epoch=%llx"
                        " round=%llu sub=%u peer=%s need=%llu present=%d decision=%s"
                        " first=%llu head=%llu contiguous=%llu accepting=%d slots=%u progress_age_ms=%llu/%d"
                        " queue_ms=%llu cpu=%u/%d rtt_ms=%u/%d",
                        (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
                        STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
                        (unsigned long long)round->id, sub, STREAM_ADDRESS_TEXT(info->peer),
                        (unsigned long long)pick->rescue_seq, info->requested_present, decision, (unsigned long long)info->first_available,
                        (unsigned long long)info->last_available, (unsigned long long)info->contiguous_last,
                        info->accepting, info->free_slots, (unsigned long long)info->progress_age_ms, info->progress_valid,
                        (unsigned long long)info->supplier_queue_age_ms, info->cpu_basis_points, info->cpu_valid, info->rtt_ms, info->rtt_valid);
                }
                struct stream_supplier *s = supplier_find(o, info->peer);
                if (s && s->offers) s->offers[sub].info = *info;
                if (rescue_suitable(info, pick->rescue_seq)) {
                    uint64_t deadline = info->info_ms + STREAM_RESCUE_CHOOSE_MS;
                    if (!part->choose_ms || deadline < part->choose_ms) {
                        part->choose_ms = deadline;
                        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream rescue first suitable round=%llu sub=%u peer=%s seq=%llu choose_ms=%llu",
                            (unsigned long long)round->id, sub, STREAM_ADDRESS_TEXT(info->peer),
                            (unsigned long long)pick->rescue_seq, (unsigned long long)deadline);
                    }
                }
            }
            if ((!part->choose_ms && pending) || (part->choose_ms && now < part->choose_ms)) { active++; continue; }
            int best = -1; unsigned ties = 0;
            for (unsigned i = 0; i < STREAM_RESCUE_PEERS; i++) {
                if (!part->probes[i] || !rescue_suitable(&replies[i], pick->rescue_seq) || replies[i].info_ms > part->choose_ms) continue;
                int comparison = best < 0 ? -1 : rescue_compare(o, sub, &replies[i], &replies[best]);
                if (comparison < 0) { best = (int)i; ties = 1; }
                else if (!comparison && rescue_random(o) % ++ties == 0) best = (int)i;
            }
            if (best >= 0 && stream_probe_subscribe(part->probes[best], pick->rescue_seq, pick->slot) == STREAM_OK) {
                pick->replacement = part->probes[best]; part->probes[best] = NULL;
                pick->replacement_ms = now; pick->emergency = 1;
                const struct stream_receive_info *info = &replies[best];
                uint64_t wait = chat_setting_get_int(o->inst, "stream_stall_timeout_ms", 500);
                if (wait < STREAM_MIGRATION_TIMEOUT_MS) wait = STREAM_MIGRATION_TIMEOUT_MS;
                pick->rescue_until_ms = now + rescue_timeout(info, wait);
                if (pick->relieving && pick->rescue_until_ms > pick->relief_ms + STREAM_MIGRATION_TIMEOUT_MS)
                    pick->rescue_until_ms = pick->relief_ms + STREAM_MIGRATION_TIMEOUT_MS;
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream rescue selected self=%llx group=%llx source=%s epoch=%llx"
                    " round=%llu sub=%u old=%s new=%s seq=%llu"
                    " head=%llu contiguous=%llu queue_ms=%llu cpu=%u/%d rtt_ms=%u/%d window_ms=%u elapsed_ms=%llu attempt_left_ms=%llu",
                    (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
                    STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
                    (unsigned long long)round->id, sub, STREAM_ADDRESS_TEXT(round->peer), STREAM_ADDRESS_TEXT(info->peer),
                    (unsigned long long)pick->rescue_seq, (unsigned long long)info->last_available,
                    (unsigned long long)info->contiguous_last, (unsigned long long)info->supplier_queue_age_ms,
                    info->cpu_basis_points, info->cpu_valid, info->rtt_ms, info->rtt_valid, STREAM_RESCUE_CHOOSE_MS,
                    (unsigned long long)(now - (part->choose_ms - STREAM_RESCUE_CHOOSE_MS)),
                    (unsigned long long)(pick->rescue_until_ms > now ? pick->rescue_until_ms - now : 0));
            } else {
                pick->rescue_retry_ms = now + 1000 + rescue_random(o) % 101;
                DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream rescue waits source=%s sub=%u seq=%llu reason=no_suitable_DATA_offer retry_ms=%llu",
                    STREAM_ADDRESS_TEXT(o->descriptor.source), sub, (unsigned long long)pick->rescue_seq,
                    (unsigned long long)(pick->rescue_retry_ms - now));
            }
            rescue_part_close(part);
        }
        if (active) { link = &round->next; continue; }
        *link = round->next; u_free(round->parts); u_free(round);
    }
}

static void rescue_finish(struct stream_optimizer *o, unsigned sub, uint64_t now, int promote) {
    struct stream_selection *pick = &o->selected[sub];
    struct stream_receive_info winner;
    stream_receive_get_info(promote ? pick->replacement : pick->current, &winner);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream rescue recovered self=%llx group=%llx source=%s epoch=%llx"
        " sub=%u seq=%llu winner=%s old=%s promote=%d next=%llu cause=%s",
        (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
        STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
        sub, (unsigned long long)pick->rescue_seq,
        STREAM_ADDRESS_TEXT(winner.peer), STREAM_ADDRESS_TEXT(pick->rescue_peer), promote, (unsigned long long)winner.data_next,
        pick->relieving ? "OVERLOAD" : "DATA_pause");
    if (promote) {
        if (pick->current && !stream_address_equal(pick->rescue_peer, winner.peer)) {
            rescue_failure(o, pick->rescue_peer, sub, now, pick->relieving ? "replaced_after_OVERLOAD" : "replaced_after_stall");
            struct stream_supplier *old = supplier_find(o, pick->rescue_peer);
            if (old && old->offers) {
                old->offers[sub].info.info_ms = 0;
                old->offers[sub].retry_ms = old->offers[sub].rescue_retry_ms;
            }
        }
        stream_receive_close(pick->current); pick->current = pick->replacement; pick->replacement = NULL;
        pick->relief_id = 0;
        o->info.migrations++;
    } else {
        if (pick->relieving && winner.overload_id > pick->relief_id) pick->relief_id = winner.overload_id;
        stream_receive_close(pick->replacement); pick->replacement = NULL;
    }
    struct stream_rescue_part *part = rescue_part_find(o, sub); if (part) rescue_part_close(part);
    struct stream_supplier *s = supplier_find(o, winner.peer);
    if (s && s->offers) { s->offers[sub].rescue_failures = 0; s->offers[sub].rescue_retry_ms = 0; }
    stream_receive_watch(pick->current, UINT64_MAX);
    pick->rescuing = pick->emergency = pick->relieving = 0; pick->rescue_retry_ms = pick->relief_ms = pick->rescue_until_ms = 0;
    pick->better_ms = 0; pick->better_node = (struct stream_address){{0}}; pick->opened_ms = now;
    if (promote) dependencies_refresh(o);
}

/* Точный срок гонки не зависит от фазы общего 100-мс таймера; selector остаётся отложенным. */
static void rescue_timer_fire(void *arg) {
    struct stream_optimizer *o = arg; o->rescue_timer = NULL; o->rescue_timer_ms = 0;
    stream_network_wake(stream_network_get(o->inst));
}

static void rescue_arm(struct stream_optimizer *o, uint64_t deadline, uint64_t now) {
    if (deadline == UINT64_MAX || (o->rescue_timer && o->rescue_timer_ms == deadline)) return;
    if (o->rescue_timer) uasync_cancel_timeout(o->inst->ua, o->rescue_timer);
    o->rescue_timer_ms = deadline;
    o->rescue_timer = uasync_set_timeout(o->inst->ua, (deadline > now ? deadline - now : 1) * 10, o, rescue_timer_fire, "stream_rescue");
    if (!o->rescue_timer) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream rescue timer allocation failed deadline=%llu", (unsigned long long)deadline);
}

/* Старые DATA сохраняются до фактического заполнения дырки. INFO/ACCEPT и GAP не являются прогрессом. */
static void rescue_tick(struct stream_optimizer *o, struct STREAM_HANDLE *h, const struct stream_local *local, uint64_t now) {
    if (!local->anchored) return;
    uint64_t stall = chat_setting_get_int(o->inst, "stream_stall_timeout_ms", 500), next = UINT64_MAX;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct stream_selection *pick = &o->selected[sub]; struct stream_receive_info old = {0}, reserve = {0};
        if (pick->current) stream_receive_get_info(pick->current, &old);
        if (pick->rescuing) {
            int relief_cancelled = old.overload_id >= pick->relief_id
                && (old.overload_outcome == STREAM_OVERLOAD_COMPLETED || old.overload_outcome == STREAM_OVERLOAD_DRAINED
                    || (old.overload_id > pick->relief_id && !old.overload_outcome && !old.relocate_requested));
            int relief_expired = now - pick->relief_ms >= STREAM_MIGRATION_TIMEOUT_MS;
            if (pick->relieving && (relief_cancelled || relief_expired)) {
                uint64_t progress = old.progress_ms ? old.progress_ms : old.active_ms;
                if (pick->current && old.state == STREAM_RECEIVE_ACTIVE && now < progress + stall) {
                    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream OVERLOAD migration stopped source=%s peer=%s sub=%u"
                        " reason=%s elapsed_ms=%llu preserve_DATA=1",
                        STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(old.peer), sub,
                        relief_cancelled ? "no_longer_needed" : "migration_deadline", (unsigned long long)(now - pick->relief_ms));
                    if (!relief_cancelled && pick->replacement) {
                        stream_receive_get_info(pick->replacement, &reserve);
                        rescue_failure(o, reserve.peer, sub, now, "OVERLOAD_migration_deadline");
                    }
                    rescue_finish(o, sub, now, 0); continue;
                }
                /* Отмена разгрузки не лечит настоящую паузу DATA: тот же поиск продолжает восстановление дырки. */
                pick->relieving = 0;
            }
            if (pick->emergency && pick->replacement) stream_receive_get_info(pick->replacement, &reserve);
            int old_ready = !pick->relieving && pick->current && old.state != STREAM_RECEIVE_CLOSED && old.recovery_ready;
            int new_ready = pick->emergency && reserve.state != STREAM_RECEIVE_CLOSED && reserve.recovery_ready
                && (!pick->current || reserve.data_next >= old.next);
            if (old_ready || new_ready) {
                /* При готовности обоих в одном проходе сохраняем старый, избегая лишнего переноса. */
                rescue_finish(o, sub, now, new_ready && !old_ready); continue;
            }
            if (pick->emergency) {
                uint64_t progress = reserve.progress_ms ? reserve.progress_ms : reserve.subscribe_ms ? reserve.subscribe_ms : pick->replacement_ms;
                uint64_t wait = pick->relieving && stall > 1000 ? 1000 : stall;
                uint64_t deadline = progress + rescue_timeout(&reserve, wait);
                if (deadline > pick->rescue_until_ms) deadline = pick->rescue_until_ms;
                int missing = reserve.state >= STREAM_RECEIVE_ACTIVE && reserve.next > pick->rescue_seq && !reserve.recovery_ready;
                if (reserve.state == STREAM_RECEIVE_CLOSED || missing || now >= deadline) {
                    const char *reason = reserve.state == STREAM_RECEIVE_CLOSED ? "DATA_closed" : missing ? "DATA_missing"
                        : now >= pick->rescue_until_ms ? "attempt_deadline" : "DATA_timeout";
                    DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream rescue attempt stopped source=%s sub=%u peer=%s reason=%s"
                        " elapsed_ms=%llu idle_ms=%llu exact_DATA=%d old_next=%llu reserve_DATA_next=%llu preserve_old=%d",
                        STREAM_ADDRESS_TEXT(o->descriptor.source), sub, STREAM_ADDRESS_TEXT(reserve.peer), reason,
                        (unsigned long long)(now - pick->replacement_ms), (unsigned long long)(now - progress), reserve.recovery_ready,
                        (unsigned long long)old.next, (unsigned long long)reserve.data_next, pick->current != NULL);
                    rescue_failure(o, reserve.peer, sub, now, reason);
                    stream_receive_close(pick->replacement); pick->replacement = NULL; pick->emergency = 0; pick->rescue_retry_ms = now;
                    pick->rescue_until_ms = 0;
                } else { if (deadline < next) next = deadline; continue; }
            }
            if (rescue_part_find(o, sub)) continue;
        } else {
            if (!pick->current || old.state != STREAM_RECEIVE_ACTIVE) continue;
            uint64_t progress = old.progress_ms ? old.progress_ms : old.active_ms;
            int relocate = old.relocate_requested && old.overload_id > pick->relief_id;
            if (now < progress + stall && !relocate) {
                if (progress + stall < next) next = progress + stall; continue;
            }
            pick->relieving = relocate && now < progress + stall;
            if (pick->relieving) { pick->relief_ms = now; pick->relief_id = old.overload_id; }
            pick->rescue_peer = old.peer;
            pick->compare_ms = 0;
        }
        if (now < pick->rescue_retry_ms) { if (pick->rescue_retry_ms < next) next = pick->rescue_retry_ms; continue; }
        /* FEC может продвинуть consumer без этой части: её дырку ищем от курсора поставщика. */
        uint64_t seq = pick->relieving && old.data_next ? old.data_next - 1
            : rescue_position(local, sub, pick->current ? old.next : pick->rescue_seq);
        if (seq == UINT64_MAX) continue;
        if (!pick->rescuing) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream rescue begin source=%s peer=%s sub=%u seq=%llu idle_ms=%llu threshold_ms=%llu"
                " cause=%s overload_id=%llu",
                STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(old.peer), sub, (unsigned long long)seq,
                (unsigned long long)(now - (old.progress_ms ? old.progress_ms : old.active_ms)), (unsigned long long)stall,
                pick->relieving ? "OVERLOAD" : "DATA_pause", (unsigned long long)old.overload_id);
            stream_receive_close(pick->replacement); pick->replacement = NULL;
        }
        if (!pick->rescuing || pick->rescue_seq != seq) stream_receive_watch(pick->current, seq);
        pick->rescuing = 1; pick->rescue_seq = seq;
    }
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) if (o->selected[sub].relieving) {
        uint64_t deadline = o->selected[sub].relief_ms + STREAM_MIGRATION_TIMEOUT_MS;
        if (deadline < next) next = deadline;
    }
    rescue_rounds_poll(o, now);
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct stream_selection *pick = &o->selected[sub];
        if (!pick->rescuing || pick->replacement || rescue_part_find(o, sub) || now < pick->rescue_retry_ms) continue;
        int result = rescue_round_open(o, h, pick->rescue_peer, now);
        /* Все части этого поставщика уже получили шанс; без кандидатов повтор не чаще секунды. */
        for (unsigned other = sub; other < o->descriptor.n; other++) {
            struct stream_selection *same = &o->selected[other];
            if (same->rescuing && stream_address_equal(same->rescue_peer, pick->rescue_peer) && !same->replacement && !rescue_part_find(o, other))
                same->rescue_retry_ms = now + (result == STREAM_LOCAL_BUSY ? STREAM_LOCAL_RETRY_MS : 1000 + rescue_random(o) % 101);
        }
    }
    for (struct stream_rescue_round *round = o->rescues; round; round = round->next)
        for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
            struct stream_rescue_part *part = &round->parts[sub];
            if (part->active && part->choose_ms && part->choose_ms < next) next = part->choose_ms;
            for (unsigned i = 0; i < STREAM_RESCUE_PEERS; i++)
                if (part->probes[i] && part->deadlines[i] > now && part->deadlines[i] < next) next = part->deadlines[i];
        }
    rescue_arm(o, next, now);
}

int stream_optimizer_create(struct UTUN_INSTANCE *inst, const struct stream_descriptor *d, struct stream_optimizer **out) {
    if (!inst || !d || !out || *out) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream optimizer create invalid arguments"); return STREAM_ERR_ARGUMENT; }
    struct stream_optimizer *o = u_calloc(1, sizeof(*o));
    if (o) o->selected = u_calloc(d->n, sizeof(*o->selected));
    if (o) o->dependencies = u_calloc((unsigned)d->n * (STREAM_PATH_MAX - 1), sizeof(*o->dependencies));
    if (!o || !o->selected || !o->dependencies) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream optimizer allocation failed");
        if (o) { u_free(o->selected); u_free(o->dependencies); u_free(o); } return STREAM_ERR_MEMORY;
    }
    o->inst = inst; o->descriptor = *d; o->settings = (struct stream_selection_settings){2000, 5000, 20};
    o->started_ms = get_time_tb() / 10; o->start_seq = UINT64_MAX;
    if (RAND_bytes((unsigned char *)&o->random, sizeof(o->random)) != 1 || !o->random) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream rescue random seed fallback"); o->random = (uint32_t)(inst->node_id ^ o->started_ms) | 1;
    }
    o->startup_timer = uasync_set_timeout(inst->ua, STREAM_BOOTSTRAP_RELAY_MS * 10, o, startup_timeout, "stream_bootstrap");
    if (!o->startup_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream bootstrap timer allocation failed");
        u_free(o->dependencies); u_free(o->selected); u_free(o); return STREAM_ERR_MEMORY;
    }
    o->info.automatic = 1; phase_set(o, STREAM_DELIVERY_BOOTSTRAP, o->started_ms); *out = o; return STREAM_OK;
}

/* Освобождает основные и заменяющие выдачи; повторный вызов безопасен, статистика истории сохраняется. */
static unsigned selections_close(struct stream_optimizer *o) {
    unsigned closed = 0;
    rescue_rounds_close(o);
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct stream_selection *pick = &o->selected[sub];
        closed += (pick->current != NULL) + (pick->replacement != NULL);
        stream_receive_close(pick->current); stream_receive_close(pick->replacement);
        stream_receive_slot_close(pick->slot);
        memset(pick, 0, sizeof(*pick));
    }
    stream_receive_unreserve(o->reservation); o->reservation = NULL;
    o->info.subflows = o->info.overlaps = o->info.providers = o->info.protected_by_fec = 0;
    o->dependency_count = o->info.working_parts = o->info.maximum_dependency = 0;
    return closed;
}

void stream_optimizer_destroy(struct stream_optimizer *o) {
    if (!o) return;
    if (o->startup_timer) uasync_cancel_timeout(o->inst->ua, o->startup_timer);
    selections_close(o);
    while (o->suppliers) {
        struct stream_supplier *s = o->suppliers; o->suppliers = s->next;
        stream_receive_close(s->probe); u_free(s->offers); u_free(s);
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream optimizer stopped source=%s probes=%llu migrations=%llu failures=%llu restarts=%llu",
               STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->info.probes_started,
               (unsigned long long)o->info.migrations, (unsigned long long)o->info.failures, (unsigned long long)o->info.restarts);
    u_free(o->dependencies); u_free(o->selected); u_free(o);
}

/* Сетевое ожидание не завершает локальных читателей и не сбрасывает позицию воспроизведения. */
void stream_optimizer_suspend(struct stream_optimizer *o) {
    if (o->startup_timer) { uasync_cancel_timeout(o->inst->ua, o->startup_timer); o->startup_timer = NULL; }
    unsigned closed = selections_close(o), probes = 0;
    while (o->suppliers) {
        struct stream_supplier *s = o->suppliers; o->suppliers = s->next;
        probes += s->probe != NULL; stream_receive_close(s->probe); u_free(s->offers); u_free(s);
    }
    struct stream_local local;
    if (stream_local_get(o->inst, &o->descriptor, &local) == STREAM_OK) {
        o->start_seq = local.anchored ? local.next : UINT64_MAX;
        o->recovering = local.anchored; o->recovery_seq = local.next; o->recovery_ms = get_time_tb() / 10;
        for (unsigned sub = 0; sub < o->descriptor.n; sub++) o->selected[sub].opened = local.anchored;
    }
    memset(&o->schedule, 0, sizeof(o->schedule)); o->roster_ms = 0; o->info.probes_active = 0;
    phase_set(o, STREAM_DELIVERY_WAITING, get_time_tb() / 10);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream optimizer suspended group=%llx source=%s epoch=%llx"
        " released_DATA=%u released_probes=%u resume_seq=%llu preserve_cursors=1",
        (unsigned long long)o->descriptor.group_id, STREAM_ADDRESS_TEXT(o->descriptor.source),
        (unsigned long long)o->descriptor.source_epoch, closed, probes, (unsigned long long)o->start_seq);
}

int stream_optimizer_configure(struct stream_optimizer *o, const struct stream_selection_settings *settings) {
    if (!o || !settings || settings->migration_confirm_ms > 60000 || settings->minimum_hold_ms > 60000
        || settings->score_hysteresis_ms > 60000) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream optimizer settings invalid"); return STREAM_ERR_ARGUMENT;
    }
    o->settings = *settings; return STREAM_OK;
}

void stream_optimizer_info(struct stream_optimizer *o, struct stream_selection_info *info) { *info = o->info; }

/* Несколько изменений до одного прохода объединяются; новый поставщик получает приоритет в обычном бюджете. */
void stream_optimizer_roster_changed(struct stream_optimizer *o, struct stream_address peer) {
    if (o->roster_ms) DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream optimizer roster invalidated group=%llx source=%s peer=%s",
        (unsigned long long)o->descriptor.group_id, STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(peer));
    o->roster_ms = 0;
}

/* READY отменяет ожидание старого цикла/ошибки только для подтверждённого поставщика. */
void stream_optimizer_refresh(struct stream_optimizer *o, struct stream_address peer) {
    o->roster_ms = o->standby_ms = 0;
    struct stream_supplier *s = supplier_find(o, peer);
    if (s) {
        s->retry_ms = 0; s->failures = 0; s->refresh = 1;
        if (s->offers) for (unsigned sub = 0; sub < o->descriptor.n; sub++) s->offers[sub].retry_ms = 0;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream optimizer relay wake source=%s peer=%s",
                STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(peer));
}

/* Перезапрос не меняет consumer/media cursor: уже принятые части и decoder references остаются пригодны. */
static void recovery_request(struct stream_optimizer *o, const struct stream_local *local, uint64_t now) {
    unsigned closed = 0;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct stream_selection *pick = &o->selected[sub];
        if (pick->rescuing) continue;
        struct STREAM_RECEIVE **handles[] = {&pick->current, &pick->replacement};
        for (unsigned i = 0; i < 2; i++) if (*handles[i]) {
            struct stream_receive_info info; stream_receive_get_info(*handles[i], &info);
            /* Срок тишины ACTIVE задаёт rescue, в том числе при настройке больше5с. */
            if (local->anchored && info.state == STREAM_RECEIVE_ACTIVE) continue;
            if (!local->anchored || info.next <= local->next) {
                struct stream_supplier *s = supplier_find(o, info.peer); if (s) s->refresh = 1;
                stream_receive_close(*handles[i]); *handles[i] = NULL; closed++;
            }
        }
    }
    for (struct stream_supplier *s = o->suppliers; s; s = s->next) {
        /* Обновляем автора/зависших поставщиков. Остальной обход и уже начатые INFO сохраняют свои сроки:
         * перезапуск всего списка каждые 5с никогда не дал бы дойти до дальних кандидатов. */
        if (!s->source && !s->refresh) continue;
        s->refresh = 1;
    }
    if (!local->anchored) o->start_seq = UINT64_MAX;
    if (!o->recovering) o->recovery_seq = local->next;
    backups_refresh(o, (struct stream_address){{0}}); o->recovering = 1; o->recovery_ms = now; o->info.restarts++;
    DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "stream recovery request source=%s epoch=%llx next=%llu idle_ms=%llu closed=%u preserve_media=1",
        STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
        (unsigned long long)local->next, (unsigned long long)(now - (local->progress_ms ? local->progress_ms : o->started_ms)), closed);
}

/* KEY нужен лишь если свежие диапазоны уже не позволяют собрать k частей недостающей пачки.
 * Позиция одна для всех выдач; неподтверждённый INFO не является медиа, подпись/KEY проверяются обычным ingest. */
static int recovery_expired(struct stream_optimizer *o, const struct stream_local *local, uint64_t now) {
    uint8_t available[RS_MAX_BLOCKS] = {0}; unsigned count = 0;
    int expired = 0; uint64_t missing_seq = o->recovery_seq;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        const uint8_t *wire; size_t length; uint64_t actual;
        if (stream_buffer_read(local->buffer, missing_seq, sub, &wire, &length, &actual) == STREAM_OK) available[sub] = 1;
    }
    for (struct stream_supplier *s = o->suppliers; s; s = s->next) if (s->offers && s->online && s->compatible) {
        for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
            struct stream_offer *offer = &s->offers[sub];
            struct stream_receive_info *info = &offer->info;
            if (!info->info_ms || info->info_ms < o->recovery_ms || now - info->info_ms > STREAM_OFFER_TTL_MS
                || info->first_available == UINT64_MAX) continue;
            int missing = (offer->missing_valid && offer->missing_seq == missing_seq)
                || (info->requested_seq == missing_seq && !info->requested_present);
            if (info->first_available <= missing_seq && info->last_available >= missing_seq && !missing) available[sub] = 1;
            if (info->first_available > missing_seq || missing) expired = 1;
        }
    }
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) count += available[sub];
    return expired && count < (unsigned)(o->descriptor.n - o->descriptor.r);
}

/* Существующую выдачу можно перенести без нового допуска и слота; остальные причины непригодности сохраняются. */
static int live_supplier(struct stream_optimizer *o, struct stream_supplier *s, unsigned sub, uint64_t seq, uint64_t now) {
    enum supplier_status status = supplier_status(o, s, sub, seq, (struct stream_address){{0}}, now);
    if (status == SUPPLIER_READY) return 1;
    if ((status != SUPPLIER_BUSY && status != SUPPLIER_FULL) || !o->selected || !o->selected[sub].current) return 0;
    struct stream_receive_info current; stream_receive_get_info(o->selected[sub].current, &current);
    return stream_address_equal(current.peer, s->node) && current.state == STREAM_RECEIVE_ACTIVE;
}

/* Пригодный полный KEY должен быть свежим относительно уже известной публикации, а не часов приёмника. */
static int live_offer(struct stream_optimizer *o, struct stream_supplier *s, unsigned sub, const struct stream_local *local,
                      uint64_t limit_us, uint64_t now) {
    if (!s->offers) return 0;
    const struct stream_receive_info *info = &s->offers[sub].info;
    return info->key_valid && info->key_seq >= local->next && (!o->info.live_jumps || info->key_seq > o->live_key_seq)
        && info->key_pts_us != UINT64_MAX && info->key_pts_us <= local->live_head_us
        && local->live_head_us - info->key_pts_us <= limit_us && now - info->info_ms <= STREAM_OFFER_TTL_MS
        && live_supplier(o, s, sub, info->key_seq, now);
}

/* Не более трёх новейших KEY за проход; для каждого нужны k разных частей одного полного диапазона. */
static int live_key_choose(struct stream_optimizer *o, const struct stream_local *local, uint64_t limit_us, uint64_t now,
                           uint64_t *key, uint64_t *last) {
    uint64_t before = UINT64_MAX;
    for (unsigned attempt = 0; attempt < STREAM_RESCUE_PEERS; attempt++) {
        const struct stream_receive_info *best = NULL;
        for (struct stream_supplier *s = o->suppliers; s; s = s->next) for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
            if (!live_offer(o, s, sub, local, limit_us, now)) continue;
            const struct stream_receive_info *info = &s->offers[sub].info;
            if (info->key_seq < before && (!best || info->key_seq > best->key_seq)) best = info;
        }
        if (!best) return 0;
        unsigned parts = 0;
        for (unsigned sub = 0; sub < o->descriptor.n; sub++) for (struct stream_supplier *s = o->suppliers; s; s = s->next) {
            if (!live_supplier(o, s, sub, best->key_seq, now)) continue;
            const struct stream_receive_info *info = &s->offers[sub].info;
            int same_key = info->key_valid && info->key_seq == best->key_seq && info->key_last_seq == best->key_last_seq
                && info->key_pts_us == best->key_pts_us;
            /* При большом RTT последовательные INFO видят разные последние KEY; точный непрерывный диапазон
             * всё равно подтверждает наличие выбранного общего KEY, даже если у пира уже объявлен более новый. */
            int retained = info->requested_present && info->requested_seq <= best->key_seq && info->contiguous_last >= best->key_last_seq;
            if (same_key || retained) {
                parts++; break;
            }
        }
        if (parts >= (unsigned)(o->descriptor.n - o->descriptor.r)) {
            *key = best->key_seq; if (last) *last = best->key_last_seq; return 1;
        }
        before = best->key_seq;
    }
    return 0;
}

/* Общий переход при вытеснении истории и превышении live lag; существующие слоты не требуют нового допуска. */
static int selections_seek(struct stream_optimizer *o, const struct stream_local *local, uint64_t key, uint64_t last,
                           uint64_t now, const char *reason) {
    if (stream_local_restart(o->inst, &o->descriptor, key) != STREAM_OK) return 0;
    rescue_rounds_close(o);
    unsigned kept = 0, sought = 0;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct stream_selection *pick = &o->selected[sub];
        struct STREAM_RECEIVE *current = pick->current; struct stream_receive_slot *slot = pick->slot;
        stream_receive_close(pick->replacement);
        int retain = 0;
        if (current) {
            struct stream_receive_info info; stream_receive_get_info(current, &info);
            if (info.state == STREAM_RECEIVE_ACTIVE && info.next > key) {
                /* Быстрая часть уже приняла KEY целиком либо продолжает его хвост; первый индекс не теряем. */
                uint64_t end = info.next - 1 < last ? info.next - 1 : last;
                retain = 1;
                for (uint64_t seq = key; seq <= end; seq++) {
                    const uint8_t *wire; size_t length; uint64_t actual;
                    if (stream_buffer_read(local->buffer, seq, sub, &wire, &length, &actual) != STREAM_OK) { retain = 0; break; }
                }
                kept += retain;
            }
            if (!retain && info.state == STREAM_RECEIVE_ACTIVE && stream_receive_seek(current, key) == STREAM_OK) {
                retain = 1; sought++;
            }
            if (!retain) { stream_receive_close(current); current = NULL; }
        }
        *pick = (struct stream_selection){.current = current, .slot = slot, .opened = current != NULL, .opened_ms = now};
    }
    o->start_seq = key; o->recovering = 0;
    DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream KEY jump source=%s floor=%llu key=%llu cause=%s GAP=explicit retained=%u SEEK=%u",
        STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)local->next, (unsigned long long)key, reason, kept, sought);
    return 1;
}

/* Превышение подтверждается1с. Пока общего KEY нет, старые DATA полезны; память и INFO остаются в общих лимитах. */
static int live_tick(struct stream_optimizer *o, const struct stream_local *local, uint64_t now) {
    o->info.live_lag_valid = 0; o->info.live_lag_ms = 0;
    struct stream_media_recovery position;
    if (!local->media || !local->decoded || local->finished || local->live_head_us == UINT64_MAX
        || stream_media_store_recovery(local->media, &position) != STREAM_OK || position.video_us < 0) {
        o->live_since_ms = 0; o->info.live_waiting_key = 0; return 0;
    }
    o->info.live_lag_valid = 1;
    uint64_t lag = local->live_head_us > (uint64_t)position.video_us ? local->live_head_us - (uint64_t)position.video_us : 0;
    o->info.live_lag_ms = lag / 1000;
    uint64_t limit = chat_setting_get_int(o->inst, "stream_max_live_lag_ms", 10000);
    if (lag <= limit * 1000) {
        if (o->live_since_ms) DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream live lag recovered source=%s lag_ms=%llu limit_ms=%llu",
            STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->info.live_lag_ms, (unsigned long long)limit);
        o->live_since_ms = 0; o->info.live_waiting_key = 0; return 0;
    }
    if (!o->live_since_ms) {
        o->live_since_ms = now;
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream live lag exceeds limit source=%s lag_ms=%llu limit_ms=%llu head_pts=%llu local_pts=%lld",
            STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->info.live_lag_ms, (unsigned long long)limit,
            (unsigned long long)local->live_head_us, (long long)position.video_us);
    }
    if (now >= o->live_refresh_ms) {
        o->live_refresh_ms = now + STREAM_STANDBY_PERIOD_MS;
        backups_refresh(o, (struct stream_address){{0}});
        for (struct stream_supplier *s = o->suppliers; s; s = s->next) if (usage(o, s->node, o->descriptor.n)) s->refresh = 1;
    }
    if (now - o->live_since_ms < 1000 || now < o->live_retry_ms) return 0;
    uint64_t key, last;
    if (!live_key_choose(o, local, limit * 1000, now, &key, &last)) {
        if (!o->info.live_waiting_key) DEBUG_WARN(DEBUG_CATEGORY_STREAM,
            "stream live recovery waits source=%s lag_ms=%llu reason=no_common_fresh_KEY preserve_DATA=1",
            STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->info.live_lag_ms);
        o->info.live_waiting_key = 1; o->live_retry_ms = now + STREAM_PROBE_PERIOD_MS; return 0;
    }
    if (!selections_seek(o, local, key, last, now, "live_limit")) return 0;
    o->live_since_ms = 0; o->live_key_seq = key;
    o->live_retry_ms = now + (limit > STREAM_MIGRATION_TIMEOUT_MS ? limit : STREAM_MIGRATION_TIMEOUT_MS);
    o->info.live_jumps++; o->info.live_waiting_key = 0;
    DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream live KEY jump source=%s floor=%llu key=%llu lag_ms=%llu limit_ms=%llu",
        STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)local->next, (unsigned long long)key,
        (unsigned long long)o->info.live_lag_ms, (unsigned long long)limit);
    return 1;
}

void stream_optimizer_tick(struct stream_optimizer *o, struct STREAM_HANDLE *h, uint64_t now) {
    struct stream_local local;
    if (stream_local_get(o->inst, &o->descriptor, &local) != STREAM_OK) return;
    if (local.waiting) { phase_set(o, STREAM_DELIVERY_WAITING, now); return; }
    o->media = local.media != NULL;
    struct stream_media_recovery initial;
    int ready = !o->startup_complete && local.decoded && (!local.media
        || (stream_media_store_recovery(local.media, &initial) == STREAM_OK && initial.key_valid));
    if (ready && !o->startup_complete) {
        o->startup_complete = 1;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream bootstrap complete group=%llx source=%s epoch=%llx peer=%s elapsed_ms=%llu start=%llu",
            (unsigned long long)o->descriptor.group_id, STREAM_ADDRESS_TEXT(o->descriptor.source),
            (unsigned long long)o->descriptor.source_epoch, STREAM_ADDRESS_TEXT(o->startup_peer),
            (unsigned long long)(now - o->started_ms), (unsigned long long)local.start);
    }
    if (local.finished) {
        /* END одного поставщика задаёт границу источника; остальные выдачи ещё могут требовать восстановления. */
        if (local.last == UINT64_MAX || (local.anchored && local.next > local.last)) {
            stream_relay_demand(o->inst, &o->descriptor, 0, now);
            unsigned closed = selections_close(o);
            for (struct stream_supplier *s = o->suppliers; s; s = s->next) { stream_receive_close(s->probe); s->probe = NULL; }
            if (o->startup_timer) { uasync_cancel_timeout(o->inst->ua, o->startup_timer); o->startup_timer = NULL; }
            if (closed || o->draining) DEBUG_INFO(DEBUG_CATEGORY_STREAM,
                "stream optimizer END drained source=%s next=%llu last=%llu released=%u probes=%u",
                STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)local.next,
                (unsigned long long)local.last, closed, o->info.probes_active);
            o->draining = 0; o->info.probes_active = 0; phase_set(o, STREAM_DELIVERY_FINISHED, now); return;
        }
        if (!o->draining) DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream optimizer draining END source=%s next=%llu last=%llu",
            STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)local.next, (unsigned long long)local.last);
        o->draining = 1;
    }
    if (!o->reservation && stream_receive_reserve(o->inst, &o->descriptor, &o->reservation) != STREAM_OK) {
        phase_set(o, STREAM_DELIVERY_WAITING, now); return;
    }
    /* Избыточные части уступают место реальному спросу. Рабочие k, незавершённый перенос и downstream защищены. */
    for (unsigned sub = o->descriptor.n; sub > 0; sub--) {
        struct stream_selection *pick = &o->selected[sub - 1];
        if (pick->rescuing || pick->replacement || !stream_receive_slot_surplus(pick->slot)) continue;
        stream_receive_close(pick->current); stream_receive_slot_close(pick->slot); memset(pick, 0, sizeof(*pick));
    }
    if (now >= o->roster_ms) roster_update(o, h, now);
    probes_poll(o, h, now);
    unsigned busy = 0;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct stream_selection *pick = &o->selected[sub]; struct stream_receive_info info;
        if (pick->current) {
            stream_receive_get_info(pick->current, &info);
            if (info.state == STREAM_RECEIVE_CLOSED || !supplier_find(o, info.peer)) {
                if (local.anchored && !pick->rescuing) {
                    pick->rescue_peer = info.peer;
                    pick->rescue_seq = rescue_position(&local, sub, info.active_ms ? info.next : local.next);
                    pick->rescuing = pick->rescue_seq != UINT64_MAX;
                    DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream rescue disconnect peer=%s sub=%u next=%llu immediate=1",
                        STREAM_ADDRESS_TEXT(info.peer), sub, (unsigned long long)pick->rescue_seq);
                    stream_receive_close(pick->replacement); pick->replacement = NULL;
                }
                receive_failed(o, &pick->current, sub, now);
            }
            else {
                busy++;
                if (info.supplier_overloaded && !pick->overloaded) {
                    backups_refresh(o, info.peer);
                    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream OVERLOAD refresh backups source=%s peer=%s sub=%u selected=%d",
                        STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(info.peer), sub, info.close_requested);
                }
                pick->overloaded = info.supplier_overloaded;
                struct stream_supplier *supplier = supplier_find(o, info.peer);
                if (!info.path_rtt_valid || !info.rtt_valid || now - info.info_ms >= STREAM_ACTIVE_REFRESH_MS) {
                    if (!supplier->refresh && !supplier->probe) DEBUG_DEBUG(DEBUG_CATEGORY_STREAM,
                        "stream active supplier refresh source=%s peer=%s sub=%u INFO_age_ms=%llu chain_valid=%d",
                        STREAM_ADDRESS_TEXT(o->descriptor.source), STREAM_ADDRESS_TEXT(info.peer), sub,
                        (unsigned long long)(now - info.info_ms), info.path_rtt_valid);
                    supplier->refresh = 1;
                }
                if (supplier->offers && info.metrics_valid) {
                    supplier->offers[sub].info.supplier_metrics = info.metrics;
                    supplier->offers[sub].info.supplier_metrics_valid = 1;
                    supplier->offers[sub].info.last_available = info.next - 1;
                }
            }
        }
        if (!pick->replacement || pick->rescuing) continue;
        struct stream_receive_info replacement; stream_receive_get_info(pick->replacement, &replacement);
        if (replacement.state == STREAM_RECEIVE_CLOSED || !supplier_find(o, replacement.peer)
            || now - pick->replacement_ms >= STREAM_MIGRATION_TIMEOUT_MS) {
            stream_receive_watch(pick->current, UINT64_MAX); pick->compare_ms = 0;
            receive_failed(o, &pick->replacement, sub, now); continue;
        }
        if (!pick->compare_ms && replacement.recovery_ready && replacement.data_next >= info.next) {
            pick->compare_seq = replacement.next > info.next ? replacement.next : info.next; pick->compare_ms = now;
            stream_receive_watch(pick->current, pick->compare_seq); stream_receive_watch(pick->replacement, pick->compare_seq);
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream overlap history caught up source=%s sub=%u compare_seq=%llu old=%s new=%s",
                STREAM_ADDRESS_TEXT(o->descriptor.source), sub, (unsigned long long)pick->compare_seq,
                STREAM_ADDRESS_TEXT(info.peer), STREAM_ADDRESS_TEXT(replacement.peer));
            continue;
        }
        if (pick->compare_ms && replacement.recovery_ready) {
            uint64_t pause = chat_setting_get_int(o->inst, "stream_stall_timeout_ms", 500);
            if (!info.recovery_ready && info.next <= pick->compare_seq && now < pick->compare_ms + pause) continue;
            if (info.recovery_ready && replacement.recovery_ms > info.recovery_ms
                && replacement.recovery_ms - info.recovery_ms > o->settings.score_hysteresis_ms) {
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream overlap rejected source=%s sub=%u seq=%llu old=%s new=%s"
                    " slower_ms=%llu threshold_ms=%u preserve_DATA=1",
                    STREAM_ADDRESS_TEXT(o->descriptor.source), sub, (unsigned long long)pick->compare_seq,
                    STREAM_ADDRESS_TEXT(info.peer), STREAM_ADDRESS_TEXT(replacement.peer),
                    (unsigned long long)(replacement.recovery_ms - info.recovery_ms), o->settings.score_hysteresis_ms);
                struct stream_supplier *supplier = supplier_find(o, replacement.peer);
                if (supplier && supplier->offers) supplier->offers[sub].retry_ms = now + STREAM_MIGRATION_TIMEOUT_MS;
                stream_receive_close(pick->replacement); pick->replacement = NULL; pick->compare_ms = 0;
                stream_receive_watch(pick->current, UINT64_MAX); pick->better_ms = 0; pick->better_node = (struct stream_address){{0}}; continue;
            }
            if (replacement.data_next < info.next) continue;
            struct stream_address previous = pick->current ? info.peer : (struct stream_address){{0}};
            stream_receive_close(pick->current); pick->current = pick->replacement; pick->replacement = NULL;
            pick->relief_id = pick->compare_ms = 0; stream_receive_watch(pick->current, UINT64_MAX);
            pick->opened_ms = now; pick->better_ms = 0; pick->better_node = (struct stream_address){{0}}; o->info.migrations++;
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream optimizer moved self=%llx group=%llx source=%s epoch=%llx"
                " sub=%u from=%s to=%s received_blocks=%llu next=%llu compared_seq=%llu old_arrival_ms=%llu new_arrival_ms=%llu",
                       (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
                       STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
                       sub, STREAM_ADDRESS_TEXT(previous), STREAM_ADDRESS_TEXT(replacement.peer),
                       (unsigned long long)replacement.blocks, (unsigned long long)replacement.next, (unsigned long long)pick->compare_seq,
                       (unsigned long long)info.recovery_ms, (unsigned long long)replacement.recovery_ms);
        }
    }
    dependencies_refresh(o);
    rescue_tick(o, h, &local, now);
    standby_update(o, &local, now);
    uint64_t progress = local.progress_ms ? local.progress_ms : o->started_ms;
    uint64_t stall = chat_setting_get_int(o->inst, "stream_stall_timeout_ms", 500);
    probes_schedule(o, h, now, o->recovering || !local.decoded || now - progress >= stall);
    uint64_t recovered_seq = UINT64_MAX;
    int recovered = 0;
    if (o->recovering && local.progress_ms > o->recovery_ms)
        recovered = !local.media || (stream_buffer_next_decodable(local.buffer, o->recovery_seq, &recovered_seq) == STREAM_OK
            && recovered_seq == o->recovery_seq);
    if (o->recovering && local.progress_ms > o->recovery_ms && recovered) {
        o->recovering = 0;
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream recovery resumed source=%s next=%llu elapsed_ms=%llu preserve_media=1",
            STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)local.next,
            (unsigned long long)(now - o->recovery_ms));
    }
    if ((local.anchored || local.media) && now - progress >= STREAM_RECOVERY_STALL_MS
        && now - o->recovery_ms >= STREAM_RECOVERY_STALL_MS) {
        recovery_request(o, &local, now);
        busy = 0;
        for (unsigned sub = 0; sub < o->descriptor.n; sub++) busy += o->selected[sub].current != NULL;
    }
    uint64_t key, last;
    if (o->recovering && local.media && local.anchored && recovery_expired(o, &local, now)
        && live_key_choose(o, &local, UINT64_MAX, now, &key, &last)
        && selections_seek(o, &local, key, last, now, "unavailable")) {
        busy = 0;
        for (unsigned sub = 0; sub < o->descriptor.n; sub++) busy += o->selected[sub].current != NULL;
        stream_local_get(o->inst, &o->descriptor, &local);
    }
    if (live_tick(o, &local, now)) {
        busy = 0;
        for (unsigned sub = 0; sub < o->descriptor.n; sub++) busy += o->selected[sub].current != NULL;
        stream_local_get(o->inst, &o->descriptor, &local);
    }
    phase_update(o, &local, now);
    int migrating = 0;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) if (o->selected[sub].replacement) migrating = 1;
    if (!local.anchored && !busy) {
        o->startup_peer = (struct stream_address){{0}};
        unsigned sub = 0;
        struct stream_supplier *s = o->start_seq == UINT64_MAX ? startup_choose(o, now, &sub)
            : supplier_choose(o, sub, o->start_seq, (struct stream_address){{0}}, now);
        if (s && o->media && s->offers[sub].info.key_valid && o->start_seq == UINT64_MAX)
            o->start_seq = s->offers[sub].info.key_seq;
        struct stream_selection *pick = &o->selected[sub];
        if (s && !pick->slot) stream_receive_slot_open(h, (uint8_t)sub, &pick->slot);
        if (s && pick->slot && stream_receive_reserved_open(h, pick->slot, s->node, o->start_seq, &pick->current) == STREAM_OK) {
            o->startup_peer = s->node; o->selected[sub].opened = 1; o->selected[sub].opened_ms = now; busy++;
            if (o->startup_timer) { uasync_cancel_timeout(o->inst->ua, o->startup_timer); o->startup_timer = NULL; }
            /* Остальные INFO нужны для недостающих частей: победитель определяет только общий DATA старт. */
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream bootstrap winner group=%llx source=%s epoch=%llx peer=%s sub=%u"
                " order=%llu elapsed_ms=%llu relay=%d start=%llu head=%llu key=%llu..%llu",
                (unsigned long long)o->descriptor.group_id, STREAM_ADDRESS_TEXT(o->descriptor.source),
                (unsigned long long)o->descriptor.source_epoch, STREAM_ADDRESS_TEXT(s->node), sub,
                (unsigned long long)s->offers[sub].info.info_order, (unsigned long long)(now - o->started_ms), s->relay,
                (unsigned long long)o->start_seq, (unsigned long long)s->offers[sub].info.last_available,
                (unsigned long long)s->offers[sub].info.key_seq, (unsigned long long)s->offers[sub].info.key_last_seq);
        }
    }
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct stream_selection *pick = &o->selected[sub]; struct stream_receive_info info = {0};
        if (pick->rescuing || pick->replacement || (!local.anchored && busy && o->start_seq == UINT64_MAX)) continue;
        if (pick->current) stream_receive_get_info(pick->current, &info);
        uint64_t seq = local.anchored ? (pick->opened ? local.next : local.start) : o->start_seq;
        struct stream_supplier *s = supplier_choose(o, sub, seq, info.peer, now);
        if (!s) { pick->better_ms = 0; pick->better_node = (struct stream_address){{0}}; continue; }
        if (!pick->current) {
            if (!pick->slot) stream_receive_slot_open(h, (uint8_t)sub, &pick->slot);
            if (pick->slot && stream_receive_reserved_open(h, pick->slot, s->node, seq, &pick->current) == STREAM_OK) {
                pick->opened_ms = now; pick->opened = 1; busy++;
                struct stream_receive_info *offer = &s->offers[sub].info;
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream optimizer selected self=%llx group=%llx source=%s epoch=%llx"
                    " peer=%s sub=%u index=%llu score_ms=%llu"
                    " rtt_ms=%u chain_ms=%u hops=%u penalty_ms=%llu",
                    (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
                    STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
                    STREAM_ADDRESS_TEXT(s->node), sub, (unsigned long long)seq,
                    (unsigned long long)stream_supplier_score(offer, &offer->supplier_metrics, offer->supplier_metrics_valid, now),
                    offer->rtt_ms, offer->path_rtt_ms, offer->path.count,
                    (unsigned long long)quality_penalty(offer, &offer->supplier_metrics, offer->supplier_metrics_valid));
            }
            continue;
        }
        if ((o->info.phase != STREAM_DELIVERY_LIVE && o->info.phase != STREAM_DELIVERY_CATCHUP)
            || info.state != STREAM_RECEIVE_ACTIVE || info.close_requested || migrating
            || now - pick->opened_ms < o->settings.minimum_hold_ms || !improved(o, sub, s, &info, now)) {
            pick->better_ms = 0; pick->better_node = (struct stream_address){{0}}; continue;
        }
        if (!stream_address_equal(pick->better_node, s->node)) { pick->better_node = s->node; pick->better_ms = now; }
        if (now - pick->better_ms < o->settings.migration_confirm_ms) continue;
        struct stream_receive_info *offer = &s->offers[sub].info;
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream optimizer improvement sub=%u old=%s new=%s old_score_ms=%llu new_score_ms=%llu"
            " old_penalty_ms=%llu new_penalty_ms=%llu old_rtt_ms=%u/%d new_rtt_ms=%u/%d old_chain_ms=%u/%d new_chain_ms=%u/%d"
            " old_hops=%u new_hops=%u old_usage=%u new_usage=%u confirmed_ms=%llu",
            sub, STREAM_ADDRESS_TEXT(info.peer), STREAM_ADDRESS_TEXT(s->node),
            (unsigned long long)stream_supplier_score(&info, &info.metrics, info.metrics_valid, now),
            (unsigned long long)stream_supplier_score(offer, &offer->supplier_metrics, offer->supplier_metrics_valid, now),
            (unsigned long long)quality_penalty(&info, &info.metrics, info.metrics_valid),
            (unsigned long long)quality_penalty(offer, &offer->supplier_metrics, offer->supplier_metrics_valid),
            info.rtt_ms, info.rtt_valid, offer->rtt_ms, offer->rtt_valid,
            info.path_rtt_ms, info.path_rtt_valid, offer->path_rtt_ms, offer->path_rtt_valid, info.path.count, offer->path.count,
            usage(o, info.peer, sub), usage(o, s->node, sub), (unsigned long long)(now - pick->better_ms));
        if (stream_receive_reserved_open(h, pick->slot, s->node, seq, &pick->replacement) == STREAM_OK) {
            pick->replacement_ms = now; pick->compare_ms = 0; migrating = 1;
            stream_receive_watch(pick->replacement, seq);
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream optimizer overlap self=%llx group=%llx source=%s epoch=%llx"
                " sub=%u old=%s new=%s index=%llu reason=confirmed_improvement",
                (unsigned long long)o->inst->node_id, (unsigned long long)o->descriptor.group_id,
                STREAM_ADDRESS_TEXT(o->descriptor.source), (unsigned long long)o->descriptor.source_epoch,
                sub, STREAM_ADDRESS_TEXT(info.peer), STREAM_ADDRESS_TEXT(s->node), (unsigned long long)seq);
        }
    }
    o->info.candidates = o->info.probes_active = o->info.subflows = o->info.overlaps = o->info.providers = 0;
    dependencies_refresh(o);
    for (struct stream_supplier *s = o->suppliers; s; s = s->next) {
        o->info.candidates++; if (s->probe) o->info.probes_active++;
        unsigned used = usage(o, s->node, o->descriptor.n);
        if (used) o->info.providers++;
        if (used || s->probe || !s->offers || rescue_uses_peer(o, s->node)) continue;
        uint64_t keep_until = 0;
        for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
            struct stream_offer *offer = &s->offers[sub];
            uint64_t until = offer->info.info_ms + STREAM_OFFER_TTL_MS;
            if (until < offer->retry_ms) until = offer->retry_ms;
            if (until < offer->rescue_retry_ms) until = offer->rescue_retry_ms;
            if (until > keep_until) keep_until = until;
        }
        if (now >= keep_until) { u_free(s->offers); s->offers = NULL; }
    }
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        if (o->selected[sub].current) {
            o->info.subflows++;
        }
        if (o->selected[sub].replacement) o->info.overlaps++;
    }
    for (struct stream_rescue_round *round = o->rescues; round; round = round->next)
        for (unsigned sub = 0; sub < o->descriptor.n; sub++)
            for (unsigned i = 0; i < STREAM_RESCUE_PEERS; i++) if (round->parts[sub].probes[i]) o->info.probes_active++;
    /* Помощь нужна для получения k субпотоков, а не для идеальной защиты n/r; пауза источника не является отказом пути. */
    unsigned active = 0, viable = 0; int newer = 0;
    for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
        struct STREAM_RECEIVE *current = o->selected[sub].current;
        if (!current) continue;
        struct stream_receive_info info; stream_receive_get_info(current, &info);
        if (info.state != STREAM_RECEIVE_ACTIVE || info.close_requested) continue;
        active++;
        if ((info.blocks && info.metrics_valid && !info.metrics.stopped) || (!info.blocks && info.last_available == UINT64_MAX)) viable++;
    }
    for (struct stream_supplier *s = o->suppliers; s; s = s->next)
        if (s->offers) for (unsigned sub = 0; sub < o->descriptor.n; sub++) {
            struct stream_receive_info *offer = &s->offers[sub].info;
            if (offer->info_ms && now - offer->info_ms < STREAM_OFFER_TTL_MS
                && offer->last_available != UINT64_MAX && offer->last_available > local.next) newer = 1;
        }
    unsigned required = (unsigned)(o->descriptor.n - o->descriptor.r);
    int deficient = active < required || (viable < required && newer);
    if (!deficient) o->deficient_ms = 0;
    else if (!o->deficient_ms) o->deficient_ms = now;
    if (!o->startup_complete || now >= o->relay_poll_ms) {
        o->relay_poll_ms = now + STREAM_PROBE_PERIOD_MS;
        int needed = deficient && now - o->deficient_ms >= STREAM_MIGRATION_TIMEOUT_MS;
        if (!o->startup_complete && !stream_address_set(o->startup_peer) && now - o->started_ms >= STREAM_BOOTSTRAP_RELAY_MS) needed = 1;
        if (!o->startup_complete && stream_address_set(o->startup_peer) && stream_relay_confirmed(o->inst, &o->descriptor, o->startup_peer)) needed = 1;
        stream_relay_demand(o->inst, &o->descriptor, needed, now);
    }
    optimizer_report(o, &local, now);
}
