#include <stdio.h>
#include <string.h>

#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../../lib/db_async.h"
#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/node_conn_direct.h"
#include "../routing_layer/route_crypto.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../chat/chat_core.h"
#include "stream_member.h"
#include "stream_priv.h"

#define STREAM_CONTROL_MAX (STREAM_WIRE_HEADER + STREAM_OVERLOAD_HEADER + STREAM_OVERLOAD_MAX * 16)
#define STREAM_RENEW_TB 20000
#define STREAM_LEASE_TB 60000
#define STREAM_CONNECT_TB 50000
#define STREAM_TICK_TB 1000
#define STREAM_CPU_BUSY_PERCENT 80
#define STREAM_CPU_CLEAR_PERCENT 70
#define STREAM_RELAY_LIMIT 8
#define STREAM_RELAY_REQUEST_LIMIT 128
#define STREAM_CANDIDATE_READ_LIMIT 256
#define STREAM_RELAY_START_MS 15000
#define STREAM_RELAY_HANDOFF_MS 5000
#define STREAM_RELAY_IDLE_MS 3000
#define STREAM_RELAY_RETAIN_MS 60000
#define STREAM_CONTROL_BURST 8

/* Запросы не владеют буфером: один внутренний handle обслуживает всех получателей точной эпохи. */
struct stream_relay_request {
    struct stream_relay_request *next;
    struct stream_network *network;
    struct stream_descriptor descriptor;
    struct stream_address peer;
    uint64_t id, epoch, server_epoch, deadline_ms, retain_ms, sent_ms, checked_ms, progress_ms, progress_seq;
    int outgoing, status, reason, dirty, urgent, granted, blocked;
    struct queue_waiter_handle waiter;
    uint8_t end[STREAM_END_SIZE];
};
struct stream_relay {
    struct stream_relay *next;
    struct stream_descriptor descriptor;
    struct STREAM_HANDLE *owner;
    uint64_t idle_ms, checked_ms;
    int allowed;
};

enum stream_peer_reset_flags { STREAM_RESET_ROTATE = 1, STREAM_RESET_KEEP_PENDING = 2 };

struct stream_peer;
struct stream_cut { uint64_t handle, rate; int closed, selected, relocate; };
struct stream_pressure_request {
    struct stream_cut cuts[STREAM_OVERLOAD_MAX];
    struct stream_descriptor descriptor; // Ключ для SELECT/END после закрытия последней выдачи запроса.
    uint64_t id, required_rate, released_rate, deadline_ms;
    uint64_t select_ms; // Последний SELECT; повтор каждые500мс до END/таймаута, в том числе при частичной разгрузке.
    unsigned count, required_count, released_count;
    unsigned reported_selection; // Число выбранных закрытий + 1 в последнем диагностическом снимке; 0 — ещё не выводили.
    int active, sent, end_pending, outcome;
};
/* Интервальные счётчики переживают закрытие выдач; транспортные базы принадлежат одной эпохе ETCP. */
struct stream_peer_diagnostics {
    uint64_t report_ms, tx_blocks, tx_bytes, rx_blocks, rx_bytes, duplicates, expired, tx_gaps, rx_gaps, dropped_blocks, dropped_bytes;
    uint64_t last_rx_ms, reset_id, peer_reset_id, acked_bytes, retries, rtos;
    uint64_t control_packets, control_bytes, info_requests, wait_report_ms; // Control за интервал и срок диагностики ожидания.
};
enum stream_flow_budget { STREAM_FLOW_REGULAR, STREAM_FLOW_RESERVED, STREAM_FLOW_BACKGROUND, STREAM_FLOW_RESCUE };
struct stream_receive_reservation {
    struct stream_receive_reservation *next;
    struct stream_network *network; // Слабая ссылка; владелец — optimizer, освобождает после всех своих slots.
    struct stream_descriptor descriptor;
    unsigned minimum, slots; // Учёт regular: max(minimum, slots), поэтому потеря поставщика не теряет минимум k.
};
struct stream_receive_slot {
    struct stream_receive_slot *next;
    struct stream_network *network; // Слабая ссылка; destroy отсоединяет ещё не освобождённые slots.
    struct stream_descriptor descriptor;
    struct stream_receive_reservation *reservation;
    struct STREAM_RECEIVE *primary, *replacement; // Один постоянный слот, не более одного перекрытия.
    uint8_t subblock;
};
/* Отправленный запрос занимает окно до ответа/смены эпохи, даже после удаления вызывающего flow. */
struct stream_pending_request {
    struct stream_pending_request *next;
    struct stream_descriptor descriptor;
    uint64_t id, sent_ms; // INFO nonce либо handle SUBSCRIBE/SEEK; время передачи в ETCP, локальные мс.
    uint8_t command, subblock;
};
/* История нужна только живой выдаче; последний снимок остаётся в info после закрытия. */
struct stream_flow_history {
    struct stream_metrics metrics, offered_metrics;
    struct stream_load load;
};
struct STREAM_RECEIVE {
    struct STREAM_RECEIVE *next;
    struct stream_peer *peer;
    struct stream_descriptor descriptor;
    struct stream_receive_info info;
    uint64_t requested, deadline, renewed;
    uint64_t seek_from; // Старый ID до ACCEPT переноса; отмена незавершённого SEEK снимает также старую выдачу.
    uint8_t subblock;
    int outgoing, owned, bootstrap, info_sent, info_ready;
    int info_started; // Первый INFO передан ETCP; повтор после REINIT сохраняет исходный срок этой пробы.
    struct stream_flow_history *history; // Выделяется до допуска; освобождается при CLOSED, даже если уведомление ждёт ETCP.
    uint64_t admitted_ms, pending_highest, probe, probe_sent_ms;
    uint64_t watch_seq; // UINT64_MAX без наблюдения; первая DATA этой позиции определяет победителя recovery.
    int account_failed;
    uint64_t telemetry_sent_ms, telemetry_sample;
    int telemetry_dirty;
    int tail_waiting; // END уже известен, но relay ещё не завершил получение отсутствующего хвоста.
    int close_pending, accept_pending, seek_pending; // Терминальное уведомление, сохранённый ACCEPT, ещё не отправленный SEEK.
    uint64_t seek_target, closing_ms; // Ожидающая позиция SEEK и начало ожидания терминального уведомления.
    enum stream_flow_budget budget;
    struct stream_receive_slot *slot; // Постоянный ресурс принадлежит selector; flow не освобождает его.
};
struct stream_peer {
    struct stream_peer *next;
    struct stream_network *network;
    struct NODE_CONN_DIRECT *ncd;
    struct STREAM_RECEIVE *flows, *last_sent, *last_request;
    struct ll_queue *control, *wait_q; // control содержит только обязательные ответы, ограниченные согласованным окном.
    struct ll_entry *notices[3]; // Последние OVERLOAD, SELECT, END; не занимают запросное окно.
    struct stream_pending_request *requests;
    struct stream_descriptor control_descriptor; // Сохранённый ключ для изменения peer-окна после закрытия контента.
    unsigned request_count, reply_count, control_burst, control_turn;
    unsigned local_window, remote_window, window_current, window_pending; // Ёмкость RX, предел TX, согласованная цель RX, изменение в пути.
    uint64_t window_serial, remote_window_serial, window_ack; // Номер своего/чужого изменения и ожидающий APPLIED.
    int window_advertised; // Хотя бы один INFO-ответ с local_window передан в ETCP.
    struct queue_waiter_handle waiter;
    void *pump;
    struct stream_address address;
    uint64_t own_epoch, other_epoch, next_handle, highwater, born, idle, reverse_at;
    int up, busy;
    struct stream_load load;
    struct stream_pressure_request pressure, requested;
    uint64_t pressure_id, pressure_retry_ms, clear_since_ms, cooling_until_ms;
    uint64_t pressure_cursor; // Последний handle порции OVERLOAD; недоступные замены не блокируют остальные выдачи пира.
    int cooling;
    struct stream_peer_diagnostics diagnostic;
};
struct relay_candidates {
    char channel[65]; /* Worker использует только channel/peers/count. */
    struct topo_connect_peer* peers;
    size_t count;
    struct relay_candidates* next;
    struct stream_network* network; /* Слабая ссылка; destroy отсоединяет до освобождения. */
    struct stream_descriptor descriptor;
    int needed;
};
struct stream_stats_pending {
    struct stream_stats_pending *next;
    struct stream_descriptor descriptor;
    struct stream_stat_sample sample;
    uint64_t attempted_ms;
};
struct stream_stats_entry {
    struct stream_stats_entry *next;
    struct stream_descriptor descriptor;
    struct stream_stat_node data;
};
struct stream_network {
    struct UTUN_INSTANCE *inst;
    struct stream_peer *peers;
    void *timer, *work;
    unsigned peer_count, flow_count;
    unsigned regular_used, temporary_used, background_used; // Запущенные операции и постоянные резервы, включая ожидание DATA.
    struct stream_limits limits; // Применяются в uasync; уменьшение не закрывает необходимые DATA без замены.
    struct stream_receive_slot *slots;
    struct stream_receive_reservation *reservations;
    unsigned wanted_regular; // Неудовлетворённый допуск; дополнительные FEC уступают нужное место.
    uint64_t wanted_until_ms; // Отказ без последующего повтора не держит приоритет бесконечно.
    uint64_t budget_report_ms; // Ограничение диагностики ожидания местного ресурса.
    uint64_t limits_report_ms; // Недостигнутое снижение бюджета: не чаще раза в5с, с сохранённым минимумом RX.
    int stopping;
    struct stream_load load;
    struct uasync_load cpu;
    uint64_t next_tick_ms, callback_late_ms, diagnostic_ms;
    int global_pressure; // Измеренный CPU >80%; общий запрет новых выдач, не команда закрыть существующие.
    uint64_t global_since_ms; // Начало подтверждения восстановления CPU <=70%.
    int global_candidate; // Порог восстановления непрерывно удерживается; неизвестный CPU не меняет допуск.
    struct stream_relay_request *relay_requests;
    struct stream_relay *relays;
    struct relay_candidates* candidates;
    uint64_t relay_epoch, info_order;
    unsigned relay_count, relay_request_count;
    uint64_t telemetry_sequence, stats_sequence;
    struct stream_stats_entry *statistics;
    struct stream_stats_pending *stats_pending;
};

static void peer_pump(void *arg);
static void stats_flush(struct stream_network *net, uint64_t now);
static void network_tick(void *arg);
static void receive_raw(struct ETCP_CONN *conn, struct ll_entry *entry);
static void receive_routed(struct ETCP_CONN *conn, struct ll_entry *entry);
static void pressure_tick(struct stream_network *net, uint64_t now);
static void pressure_receive(struct stream_peer *peer, const struct stream_message *message);
static void relay_tick(struct stream_network *net, uint64_t now);
static void relay_dispatch(struct stream_network *net, struct stream_address sender, const struct stream_message *message);
static int removal_cost(struct stream_peer *peer, struct STREAM_RECEIVE *candidate);
static void peer_reset(struct stream_peer *p, int reason, unsigned flags, struct STREAM_RECEIVE *keep);

/* Конфиг применяется до нового допуска и на каждом tick; занятые слоты остаются до согласованной разгрузки. */
static void limits_refresh(struct stream_network *net) {
    struct stream_limits limits; stream_limits_get(net->inst, &limits);
    if (!memcmp(&net->limits, &limits, sizeof(limits))) return;
    net->limits = limits;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream budgets self=%s regular=%u/%u temporary=%u/%u background=%u/%u"
        " peer_temporary=%u peers=%u/%u control=%u extra_receive=%u reduction=OVERLOAD",
        sc_public_key_hex(net->inst ? net->inst->my_ed25519_pubkey : NULL, (char[65]){0}), net->regular_used, limits.regular,
        net->temporary_used, limits.temporary, net->background_used, limits.background,
        limits.peer_temporary, net->peer_count, limits.peers, limits.control, limits.extra_receive);
}

/* Отложенное управление читает живые сессии; callbacks протокола уже закончили работу с handles. */
static void network_work(void *arg) {
    struct stream_network *net = arg; net->work = NULL;
    uint64_t now = get_time_tb() / 10;
    stream_optimizers_tick(net->inst, now);
    relay_tick(net, now);
}

/* Один call_soon объединяет INFO/ACCEPT/READY; DATA не запускает дополнительный обход на каждом блоке. */
void stream_network_wake(struct stream_network *net) {
    if (!net || net->stopping || net->work) return;
    net->work = uasync_call_soon(net->inst->ua, net, network_work);
    if (!net->work) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream control scheduling failed");
}

/* Ключи очереди — cursor+кольцо, её возраст учитывает реальное время вставки и момент появления выдачи. */
static uint64_t flow_age(struct STREAM_RECEIVE *f, uint64_t now) {
    if (f->outgoing || f->info.state == STREAM_RECEIVE_CLOSED || !f->history->load.queued.packets) return 0;
    struct stream_local local; uint64_t oldest = 0;
    if (stream_local_get(f->peer->network->inst, &f->descriptor, &local) != STREAM_OK) return 0;
    if (stream_buffer_pending(local.buffer, f->info.next, f->subblock, NULL, NULL, &oldest) != STREAM_OK) return 0;
    if (oldest < f->admitted_ms) oldest = f->admitted_ms;
    return now > oldest ? now - oldest : 0;
}

/* Один блок принадлежит логической очереди выдачи/пира/модуля; ни одна из них не хранит копию DATA. */
static void load_account(struct STREAM_RECEIVE *f, uint64_t packets, uint64_t bytes, int added, int sent) {
    if (!packets) return;
    if (!added) stream_stats_add(f->peer->network->inst, &f->descriptor, sent ? ST_TX : ST_REMOVED, bytes);
    if (!added) {
        struct stream_peer_diagnostics *d = &f->peer->diagnostic;
        if (sent) { d->tx_blocks += packets; d->tx_bytes += bytes; }
        else { d->dropped_blocks += packets; d->dropped_bytes += bytes; }
    }
    struct stream_load *loads[] = {&f->history->load, &f->peer->load, &f->peer->network->load};
    for (unsigned i = 0; i < 3; i++) {
        int result = added ? stream_load_add(loads[i], packets, bytes) : stream_load_remove(loads[i], packets, bytes, sent);
        if (result != STREAM_OK) {
            f->account_failed = 1;
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream ledger failed level=%u peer=%s handle=%llu add=%d sent=%d packets=%llu bytes=%llu",
                        i, STREAM_ADDRESS_TEXT(f->peer->address), (unsigned long long)f->info.handle, added, sent,
                        (unsigned long long)packets, (unsigned long long)bytes);
        }
    }
}

static int same_stream(const struct stream_descriptor *a, const struct stream_descriptor *b) {
    return a->group_id == b->group_id && stream_address_equal(a->source, b->source) && a->source_epoch == b->source_epoch;
}

static void flow_gap_report(struct STREAM_RECEIVE *f, uint64_t first, uint64_t next, const char *reason) {
    if (f->outgoing) f->peer->diagnostic.rx_gaps++; else f->peer->diagnostic.tx_gaps++;
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream GAP peer=%s group=%llx source=%s epoch=%llx sub=%u direction=%s first=%llu next=%llu reason=%s",
        STREAM_ADDRESS_TEXT(f->peer->address), (unsigned long long)f->descriptor.group_id,
        STREAM_ADDRESS_TEXT(f->descriptor.source), (unsigned long long)f->descriptor.source_epoch,
        f->subblock, f->outgoing ? "RX" : "TX", (unsigned long long)first, (unsigned long long)next, reason);
}

/* Водяной знак и управляющая очередь принадлежат сессии пира, а не отдельной группе. */
static struct stream_peer *peer_find(struct stream_network *net, struct stream_address node) {
    for (struct stream_peer *p = net->peers; p; p = p->next) if (stream_address_equal(p->address, node)) return p;
    return NULL;
}

static uint64_t new_epoch(void) {
    uint64_t value = 0;
    do {
        if (random_bytes((uint8_t *)&value, sizeof(value)) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream peer epoch generation failed"); return 0;
        }
    } while (!value);
    return value;
}

/* Waiter callback только планирует pump; рекурсивная отправка из ETCP-очереди исключена. */
static void peer_schedule(struct stream_peer *p) {
    if (p->network->stopping || p->pump) return;
    p->pump = uasync_call_soon(p->network->inst->ua, p, peer_pump);
    if (!p->pump) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream pump scheduling failed peer=%s", STREAM_ADDRESS_TEXT(p->address));
}

static void send_ready(struct ll_queue *queue, void *arg) {
    (void)queue; struct stream_peer *p = arg; p->wait_q = NULL; peer_schedule(p);
}

/* Waiter только будит отправку актуального состояния запроса; DATA и старые STATUS в нём не копируются. */
static void relay_send_ready(struct ll_queue *queue, void *arg) {
    (void)queue; struct stream_relay_request *r = arg;
    r->blocked = 0; r->dirty = 1; r->sent_ms = 0;
    stream_network_wake(r->network);
}

/* Сохранённый указатель очереди используется только пока waiter действительно зарегистрирован. */
static void peer_cancel_wait(struct stream_peer *p) {
    if (p->waiter.internal || p->waiter.call_soon_id)
        queue_waiter_cancel(p->waiter.call_soon_id ? p->waiter.defer_q : p->wait_q, &p->waiter);
    memset(&p->waiter, 0, sizeof(p->waiter)); p->wait_q = NULL;
}

/* OVERLOAD общий для транспорта: каждый существующий handle сохраняет допуск своей группы.
 * Неизвестные/уже закрытые handles не дают доступа к данным; протокольную проверку выполняет pressure_receive. */
static int pressure_members_allowed(struct stream_peer *p, const struct stream_message *m, int receiving) {
    if (m->command != STREAM_CMD_OVERLOAD && m->command != STREAM_CMD_OVERLOAD_SELECT) return 1;
    int overload = m->command == STREAM_CMD_OVERLOAD;
    size_t header = overload ? STREAM_OVERLOAD_HEADER : 9, stride = overload ? 16 : 8;
    if (m->length < header || m->length != header + m->payload[header - 1] * stride) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream pressure invalid handle list peer=%s command=%u bytes=%zu",
            STREAM_ADDRESS_TEXT(p->address), m->command, m->length); return 0;
    }
    for (unsigned i = 0; i < m->payload[header - 1]; i++) {
        uint64_t handle = stream_read_be(m->payload + header + i * stride, 8);
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            if (f->info.handle != handle || f->outgoing != (overload == receiving)) continue;
            if (!topo_group_chat_key_allowed(p->network->inst, f->descriptor.group_id, p->address.ed)) {
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream pressure handle denied group=%llx peer=%s handle=%llu receiving=%d",
                    (unsigned long long)f->descriptor.group_id, STREAM_ADDRESS_TEXT(p->address),
                    (unsigned long long)handle, receiving); return 0;
            }
        }
    }
    return 1;
}

static int stream_packet_header(struct UTUN_INSTANCE* inst, uint8_t* wire, struct stream_message* m) {
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, m->descriptor.group_id);
    const uint8_t* key = group && group->group_type == TOPO_GROUP_TYPE_CHAT ? topo_group_wire_id(group) : NULL;
    if (!key) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream send with unknown group group=%llx command=%u",
            (unsigned long long)m->descriptor.group_id, m->command);
        return STREAM_ERR_NOT_MEMBER;
    }
    memcpy(m->descriptor.group_key, key, SC_PUBKEY_SIZE);
    stream_wire_header(wire, m); return STREAM_OK;
}

static int stream_packet_parse(struct UTUN_INSTANCE* inst, const uint8_t* wire, size_t length, struct stream_message* m) {
    int rc = stream_wire_parse(wire, length, m);
    if (rc != STREAM_OK) return rc;
    struct TOPO_GROUP* group = topo_groups_find_wire(inst->topo_groups, m->descriptor.group_key);
    if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream receive with unknown group key=%s command=%u",
            sc_public_key_hex(m->descriptor.group_key, (char[65]){0}), m->command); return STREAM_ERR_NOT_MEMBER;
    }
    m->descriptor.group_id = group->group_id; return STREAM_OK;
}

/* Сериализация для единственной текущей отправки; на отказе вызывающий сохраняет своё ожидающее действие. */
static struct ll_entry *control_packet(struct stream_peer *p, struct stream_message m, const uint8_t *payload, size_t length) {
    if (length + STREAM_WIRE_HEADER > STREAM_CONTROL_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream control invalid command=%u bytes=%zu", m.command, length); return NULL;
    }
    struct ll_entry *e = queue_entry_new(sizeof(uint64_t));
    if (e) e->dgram = u_malloc((uint32_t)(STREAM_WIRE_HEADER + length));
    if (!e || !e->dgram) {
        if (e) queue_entry_free(e);
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream control allocation failed command=%u", m.command); return NULL;
    }
    uint64_t queued_ms = get_time_tb() / 10; memcpy(e->data, &queued_ms, sizeof(queued_ms));
    m.sender_epoch = p->own_epoch; m.receiver_epoch = m.command == STREAM_CMD_INFO_REQ ? 0 : p->other_epoch;
    if (stream_packet_header(p->network->inst, e->dgram, &m) != STREAM_OK) {
        queue_dgram_free(e); queue_entry_free(e); return NULL;
    }
    if (length) memcpy(e->dgram + STREAM_WIRE_HEADER, payload, length);
    e->len = STREAM_WIRE_HEADER + length;
    return e;
}

/* OVERLOAD-состояние объединяется по типу сообщения; отвечает существующий запрос, а не запросное окно. */
static int control_put(struct stream_peer *p, struct stream_message m, const uint8_t *payload, size_t length) {
    int slot = m.command == STREAM_CMD_OVERLOAD ? 0 : m.command == STREAM_CMD_OVERLOAD_SELECT ? 1
        : m.command == STREAM_CMD_OVERLOAD_END ? 2 : -1;
    if (slot < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream notice invalid command=%u", m.command); return STREAM_ERR_ARGUMENT;
    }
    struct ll_entry *e = control_packet(p, m, payload, length);
    if (!e) return STREAM_ERR_MEMORY;
    if (p->notices[slot]) { queue_dgram_free(p->notices[slot]); queue_entry_free(p->notices[slot]); }
    p->notices[slot] = e;
    peer_schedule(p); return STREAM_OK;
}

/* Место ответа выделяется до изменения DATA; принятый запрос никогда не остаётся без результата из-за занятой FIFO. */
static struct ll_entry *reply_reserve(struct stream_peer *p) {
    if (!p->local_window) p->local_window = p->window_current = p->network->limits.control;
    if (p->reply_count >= p->local_window) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream request exceeds window peer=%s replies=%u window=%u",
            STREAM_ADDRESS_TEXT(p->address), p->reply_count, p->local_window);
        peer_reset(p, STREAM_REASON_PROTOCOL, STREAM_RESET_ROTATE, NULL); return NULL;
    }
    struct ll_entry *e = queue_entry_new(sizeof(uint64_t));
    if (e) e->dgram = u_malloc(STREAM_CONTROL_MAX);
    if (!e || !e->dgram) {
        if (e) queue_entry_free(e);
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream reply reservation failed peer=%s", STREAM_ADDRESS_TEXT(p->address));
        peer_reset(p, STREAM_REASON_LIMIT, STREAM_RESET_ROTATE, NULL); return NULL;
    }
    uint64_t now = get_time_tb() / 10; memcpy(e->data, &now, sizeof(now));
    p->reply_count++; return e;
}

/* Сохранённый результат владеет всеми своими байтами; очередь не зависит от жизни локального контента/flow. */
static void reply_commit(struct stream_peer *p, struct ll_entry *e, struct stream_message m, const uint8_t *payload, size_t length) {
    if (STREAM_WIRE_HEADER + length > STREAM_CONTROL_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream reply oversized command=%u bytes=%zu", m.command, length);
        queue_dgram_free(e); queue_entry_free(e); peer_reset(p, STREAM_REASON_PROTOCOL, STREAM_RESET_ROTATE, NULL); return;
    }
    m.sender_epoch = p->own_epoch; m.receiver_epoch = p->other_epoch;
    if (stream_packet_header(p->network->inst, e->dgram, &m) != STREAM_OK) {
        queue_dgram_free(e); queue_entry_free(e); peer_reset(p, STREAM_REASON_MEMBER, STREAM_RESET_ROTATE, NULL); return;
    }
    memcpy(e->dgram + STREAM_WIRE_HEADER, payload, length);
    e->len = STREAM_WIRE_HEADER + length;
    p->control_descriptor = m.descriptor;
    if (queue_data_put(p->control, e) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream reply enqueue failed peer=%s", STREAM_ADDRESS_TEXT(p->address));
        peer_reset(p, STREAM_REASON_PROTOCOL, STREAM_RESET_ROTATE, NULL); return;
    }
    peer_schedule(p);
}

/* Корреляция не заимствует flow: поздний ответ завершает учёт отменённого запроса, но не оживляет выдачу. */
static int request_complete(struct stream_peer *p, const struct stream_message *m) {
    int info = !m->handle;
    if (m->command != STREAM_CMD_REJECT && m->command != (info ? STREAM_CMD_INFO_RESP : STREAM_CMD_ACCEPT)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream response type mismatch peer=%s command=%u handle=%llu",
            STREAM_ADDRESS_TEXT(p->address), m->command, (unsigned long long)m->handle); return 0;
    }
    if (info && (m->length < (m->command == STREAM_CMD_REJECT ? 9u : 8u))) return 0;
    uint64_t id = info ? stream_read_be(m->payload + (m->command == STREAM_CMD_REJECT), 8) : m->handle;
    struct stream_pending_request **link = &p->requests;
    while (*link) {
        struct stream_pending_request *r = *link;
        if (r->id == id && (r->command == STREAM_CMD_INFO_REQ) == info && r->subblock == m->subblock
            && same_stream(&r->descriptor, &m->descriptor)) {
            *link = r->next; u_free(r); p->request_count--; peer_schedule(p); return 1;
        }
        link = &r->next;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream reply without pending request peer=%s command=%u id=%llu",
        STREAM_ADDRESS_TEXT(p->address), m->command, (unsigned long long)id);
    return 0;
}

/* Routed-управление обязательно подписано end-to-end, DATA этим путём не передаётся. */
static int route_control(struct stream_network *net, struct stream_address peer, struct stream_message m, const uint8_t *payload, size_t length,
                          struct stream_relay_request *request) {
    struct UTUN_INSTANCE *inst = net->inst;
    if (!topo_group_chat_key_allowed(inst, m.descriptor.group_id, peer.ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream routed send denied group=%llx peer=%s command=%u",
            (unsigned long long)m.descriptor.group_id, STREAM_ADDRESS_TEXT(peer), m.command); return STREAM_ERR_NOT_MEMBER;
    }
    int route = etcp_router_peer_reachable(inst, m.descriptor.group_id, peer.ed, ETCP_RT_ID_STREAM);
    if (!route) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream routed control waits for group route peer=%s group=%llx command=%u",
                    STREAM_ADDRESS_TEXT(peer), (unsigned long long)m.descriptor.group_id, m.command); return STREAM_WAIT;
    }
    struct ETCP_ROUTER_CONN* conn = etcp_router_conn_get_key(inst, m.descriptor.group_id, peer.ed, ETCP_RT_ID_STREAM);
    if (!conn) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream routed channel allocation failed peer=%s", STREAM_ADDRESS_TEXT(peer));
        return STREAM_ERR_MEMORY;
    }
    if (!etcp_router_conn_send_q_has_room(conn)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream routed control waits for queue room peer=%s command=%u",
                    STREAM_ADDRESS_TEXT(peer), m.command);
        if (request) {
            request->blocked = 1;
            etcp_router_conn_on_send_ready(conn, &request->waiter, relay_send_ready, request);
        }
        return STREAM_WAIT;
    }
    struct ll_entry *e = ll_alloc_lldgram((uint16_t)(1 + STREAM_WIRE_HEADER + length));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream routed control allocation failed"); return STREAM_ERR_MEMORY; }
    e->dgram[0] = ETCP_RT_ID_STREAM;
    if (stream_packet_header(inst, e->dgram + 1, &m) != STREAM_OK) {
        queue_dgram_free(e); queue_entry_free(e); return STREAM_ERR_NOT_MEMBER;
    }
    if (length) memcpy(e->dgram + 1 + STREAM_WIRE_HEADER, payload, length);
    e->len = 1 + STREAM_WIRE_HEADER + length;
    int result = etcp_route_send_key(inst, m.descriptor.group_id, peer.ed, e, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT);
    if (m.command >= STREAM_CMD_RELAY_REQUEST) DEBUG_DEBUG(DEBUG_CATEGORY_STREAM,
        "stream routed control queued self=%s peer=%s group=%llx command=%u id=%llx sender=%llx receiver=%llx result=%d",
        sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}), STREAM_ADDRESS_TEXT(peer), (unsigned long long)m.descriptor.group_id, m.command,
        (unsigned long long)m.handle, (unsigned long long)m.sender_epoch, (unsigned long long)m.receiver_epoch, result);
    if (result < 0) DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream routed control failed peer=%s command=%u result=%d",
                              STREAM_ADDRESS_TEXT(peer), m.command, result);
    return result < 0 ? STREAM_ERR_STATE : STREAM_OK;
}

/* Освобождается фактически занятый ресурс; RESERVED принадлежит slot, а не сетевому handle. */
static void budget_put(struct stream_network *net, enum stream_flow_budget budget) {
    if (budget == STREAM_FLOW_REGULAR) net->regular_used--;
    if (budget == STREAM_FLOW_BACKGROUND || budget == STREAM_FLOW_RESCUE) net->temporary_used--;
    if (budget == STREAM_FLOW_BACKGROUND) net->background_used--;
}

/* Закрытие не ждёт доставки уведомления. Память flow освобождается после выхода из pump/receiver. */
static void flow_close(struct STREAM_RECEIVE *f, int reason, int notify) {
    if (f->info.state == STREAM_RECEIVE_CLOSED) return;
    struct stream_peer *p = f->peer;
    if (f->outgoing && !f->info.probe_only && p && reason != STREAM_REASON_STOPPED) {
        struct stream_local local;
        if (stream_local_get(p->network->inst, &f->descriptor, &local) == STREAM_OK)
            stream_buffer_telemetry_down(local.buffer, f->subblock, p->address);
    }
    enum stream_receive_state previous = f->info.state;
    if (f->outgoing && !f->info.probe_only && p && p->requested.active) {
        for (unsigned i = 0; i < p->requested.count; i++) {
            struct stream_cut *cut = &p->requested.cuts[i];
            if (!cut->closed && cut->handle == f->info.handle) cut->closed = 1;
        }
    }
    if (!f->outgoing && p) {
        for (unsigned i = 0; i < p->pressure.count; i++) {
            struct stream_cut *cut = &p->pressure.cuts[i];
            if (p->pressure.active && !cut->closed && cut->handle == f->info.handle) {
                cut->closed = 1; p->pressure.released_rate += cut->rate; p->pressure.released_count++;
            }
        }
        load_account(f, f->history->load.queued.packets, f->history->load.queued.bytes, 0, 0);
        stream_load_changed(&p->load); stream_load_changed(&p->network->load);
    }
    stream_metrics_end(&f->history->metrics); stream_metrics_end(&f->history->offered_metrics);
    f->info.metrics_valid = stream_metrics_poll(&f->history->metrics, get_time_tb() / 10, &f->info.metrics) == STREAM_OK;
    u_free(f->history); f->history = NULL;
    if (notify && p && f->info.handle) {
        uint8_t payload = (uint8_t)reason;
        struct stream_message m = {.descriptor = f->descriptor, .handle = f->info.handle, .subblock = f->subblock,
            .command = f->outgoing ? STREAM_CMD_UNSUBSCRIBE : STREAM_CMD_CLOSED,
            .sender_epoch = p->own_epoch, .receiver_epoch = p->other_epoch};
        if (p->up) f->close_pending = 1;
        else if (!f->outgoing) route_control(p->network, p->address, m, &payload, 1, NULL);
    }
    if (!p || !p->up) f->seek_from = 0;
    f->seek_pending = 0;
    f->info.state = STREAM_RECEIVE_CLOSED; f->info.reason = reason;
    f->closing_ms = get_time_tb() / 10;
    if (p) {
        budget_put(p->network, f->budget);
        if (f->slot) {
            struct stream_receive_slot *slot = f->slot;
            if (slot->primary == f) {
                slot->primary = slot->replacement; slot->replacement = NULL;
                if (slot->primary) {
                    budget_put(p->network, slot->primary->budget);
                    slot->primary->budget = STREAM_FLOW_RESERVED;
                }
            } else slot->replacement = NULL;
            f->slot = NULL;
        }
    }
    if (reason != STREAM_REASON_STOPPED)
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "stream issuance failure peer=%s handle=%llu sub=%u reason=%d next=%llu"
                   " previous_state=%d probe_only=%d info_sent=%d info_ready=%d blocks=%llu transport_up=%d local_epoch=%llx peer_epoch=%llx",
                   STREAM_ADDRESS_TEXT(f->info.peer), (unsigned long long)f->info.handle, f->subblock,
                   reason, (unsigned long long)f->info.next, previous, f->info.probe_only, f->info_sent, f->info.info_ready,
                   (unsigned long long)f->info.blocks, p ? p->up : 0,
                   (unsigned long long)f->info.local_epoch, (unsigned long long)f->info.peer_epoch);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream issuance closed peer=%s group=%llx source=%s handle=%llu sub=%u out=%d reason=%d blocks=%llu"
               " history_freed=%zu state_retained=%zu",
               STREAM_ADDRESS_TEXT(f->info.peer), (unsigned long long)f->descriptor.group_id,
               STREAM_ADDRESS_TEXT(f->descriptor.source), (unsigned long long)f->info.handle, f->subblock,
               f->outgoing, reason, (unsigned long long)f->info.blocks, sizeof(struct stream_flow_history), sizeof(*f));
    if (p && (f->close_pending || f->seek_from)) peer_schedule(p);
}

/* Caller-owned receive после detach живёт до close; внутренние downstream принадлежат модулю. */
static void peer_sweep(struct stream_peer *p) {
    if (p->busy) return;
    struct STREAM_RECEIVE **link = &p->flows;
    while (*link) {
        struct STREAM_RECEIVE *f = *link;
        if (f->info.state != STREAM_RECEIVE_CLOSED || f->close_pending || f->seek_from || f->accept_pending) {
            link = &f->next; continue;
        }
        *link = f->next; if (p->last_sent == f) p->last_sent = NULL;
        if (p->last_request == f) p->last_request = NULL;
        p->network->flow_count--; f->peer = NULL; f->next = NULL;
        if (!f->owned) u_free(f);
    }
    int relay_pending = 0;
    for (struct stream_relay_request *r = p->network->relay_requests; r; r = r->next)
        if (stream_address_equal(r->peer, p->address) && r->status <= STREAM_RELAY_READY) relay_pending = 1;
    if (p->flows || p->reply_count || p->notices[0] || p->notices[1] || p->notices[2] || relay_pending) p->idle = 0;
    else if (!p->idle) p->idle = get_time_tb();
}

/* Смена поколения может сохранить запросы до SUBSCRIBE; разрыв/ошибка отправки отменяют и эти запросы. */
static void peer_reset(struct stream_peer *p, int reason, unsigned flags, struct STREAM_RECEIVE *keep) {
    peer_cancel_wait(p);
    p->diagnostic.last_rx_ms = 0;
    uint64_t now = get_time_tb(); unsigned pending = 0;
    struct ll_entry *e;
    while ((e = queue_data_get(p->control))) { queue_dgram_free(e); queue_entry_free(e); }
    for (unsigned i = 0; i < 3; i++) {
        if (p->notices[i]) { queue_dgram_free(p->notices[i]); queue_entry_free(p->notices[i]); p->notices[i] = NULL; }
    }
    while (p->requests) { struct stream_pending_request *r = p->requests; p->requests = r->next; u_free(r); }
    p->request_count = p->reply_count = p->control_burst = 0;
    p->local_window = p->window_current = p->network->limits.control;
    p->remote_window = p->window_pending = p->window_advertised = 0;
    p->window_serial = p->remote_window_serial = p->window_ack = 0;
    for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
        f->close_pending = f->accept_pending = f->seek_pending = 0; f->seek_from = 0;
        if (f == keep) continue;
        if ((flags & STREAM_RESET_KEEP_PENDING) && f->outgoing && !f->info.handle
            && f->info.state < STREAM_RECEIVE_REQUESTED
            && (now < f->deadline || (p->up && (!f->info_started || (f->info_ready && !f->info.probe_only))))) {
            if (f->info_ready) f->info_started = 0;
            f->info_sent = f->info_ready = f->info.info_ready = 0; f->probe = f->probe_sent_ms = 0;
            f->info.local_epoch = f->info.peer_epoch = f->info.info_ms = 0;
            f->info.supplier_metrics_valid = f->info.rtt_valid = f->info.path_rtt_valid = 0;
            f->info.state = STREAM_RECEIVE_CONNECTING; pending++;
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream pending request preserved peer=%s group=%llx source=%s"
                        " sub=%u probe_only=%d remaining_ms=%llu",
                        STREAM_ADDRESS_TEXT(p->address), (unsigned long long)f->descriptor.group_id,
                        STREAM_ADDRESS_TEXT(f->descriptor.source), f->subblock, f->info.probe_only,
                        (unsigned long long)(f->deadline > now ? (f->deadline - now) / 10 : 0));
        } else flow_close(f, reason, 0);
    }
    p->other_epoch = p->highwater = 0; p->next_handle = 1; p->last_sent = p->last_request = NULL;
    if (flags & STREAM_RESET_ROTATE) p->own_epoch = new_epoch();
    peer_sweep(p);
    memset(&p->pressure, 0, sizeof(p->pressure)); memset(&p->requested, 0, sizeof(p->requested));
    p->pressure_id = p->cooling_until_ms = 0; p->cooling = 0; stream_load_init(&p->load, get_time_tb() / 10);
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream peer session reset peer=%s epoch=%llx rotate=%d reason=%d pending=%u",
               STREAM_ADDRESS_TEXT(p->address), (unsigned long long)p->own_epoch,
               !!(flags & STREAM_RESET_ROTATE), reason, pending);
}

/* NCD подтверждает UP собственного владельца, даже если ETCP уже использовался BGP/другим стримом. */
static void ncd_changed(struct NODE_CONN_DIRECT *handle, enum ncd_event event, void *arg) {
    (void)handle; struct stream_peer *p = arg;
    if (p->network->stopping) return;
    if (event == NCD_EVENT_UP) { p->up = 1; peer_schedule(p); }
    else if (event == NCD_EVENT_DOWN || event == NCD_EVENT_CLOSED) {
        p->up = 0;
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
            if (!f->outgoing && f->info.handle) {
                uint8_t reason = STREAM_REASON_TRANSPORT;
                struct stream_message m = {.descriptor = f->descriptor, .handle = f->info.handle,
                    .subblock = f->subblock, .command = STREAM_CMD_CLOSED,
                    .sender_epoch = p->own_epoch, .receiver_epoch = p->other_epoch};
                route_control(p->network, p->address, m, &reason, 1, NULL);
            }
        peer_reset(p, STREAM_REASON_TRANSPORT, STREAM_RESET_ROTATE, NULL);
    } else DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream NCD initial timeout peer=%s", STREAM_ADDRESS_TEXT(p->address));
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream NCD event peer=%s event=%d up=%d", STREAM_ADDRESS_TEXT(p->address), event, p->up);
    stream_network_wake(p->network);
}

/* На пира ровно один stream NCD owner, общий для всех его выдач и групп. */
static int stream_ncd_open(struct stream_peer *p) {
    uint8_t x[32];
    if (sc_identity_public_x("stream transport peer", p->address.ed, x) != SC_OK) return -1;
    return node_conn_direct_open_key(p->network->inst, sc_derive_node_id_from_pubkey(x), x, ncd_changed, p, &p->ncd, NULL);
}

static struct stream_peer *peer_open(struct stream_network *net, struct stream_address node) {
    struct stream_peer *p = peer_find(net, node);
    if (p) {
        if (!node_conn_direct_get_conn(p->ncd)) {
            node_conn_direct_set_callback(p->ncd, NULL, NULL); node_conn_direct_close(p->ncd); p->ncd = NULL;
            p->born = get_time_tb(); p->reverse_at = 0;
            if (stream_ncd_open(p) < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream peer NCD reopen failed peer=%s", STREAM_ADDRESS_TEXT(node)); return NULL;
            }
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream peer NCD reopened peer=%s", STREAM_ADDRESS_TEXT(node));
        }
        return p;
    }
    limits_refresh(net);
    if (net->peer_count >= net->limits.peers) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream peer limit reached"); return NULL; }
    p = u_calloc(1, sizeof(*p));
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream peer allocation failed"); return NULL; }
    p->network = net; p->address = node; p->own_epoch = new_epoch(); p->next_handle = 1; p->born = get_time_tb();
    p->local_window = p->window_current = net->limits.control;
    stream_load_init(&p->load, p->born / 10);
    p->control = queue_new(net->inst->ua, 0, 0, 0, "stream_control");
    if (!p->control || !p->own_epoch || stream_ncd_open(p) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream peer NCD/init failed peer=%s", STREAM_ADDRESS_TEXT(node));
        if (p->ncd) node_conn_direct_close(p->ncd);
        if (p->control) queue_free(p->control); u_free(p); return NULL;
    }
    p->next = net->peers; net->peers = p; net->peer_count++;
    p->diagnostic.report_ms = p->born / 10;
    struct ETCP_CONN *conn = node_conn_direct_get_conn(p->ncd);
    if (conn) {
        p->diagnostic.reset_id = conn->reset_id; p->diagnostic.peer_reset_id = conn->peer_reset_id;
        p->diagnostic.acked_bytes = conn->diagnostic_acked_bytes;
        p->diagnostic.retries = conn->diagnostic_retries; p->diagnostic.rtos = conn->diagnostic_rtos;
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream peer opened peer=%s epoch=%llx", STREAM_ADDRESS_TEXT(node), (unsigned long long)p->own_epoch);
    return p;
}

/* Все callbacks/waiters отменяются до NCD close и освобождения контекста. */
static void peer_free(struct stream_peer *p) {
    struct stream_network *net = p->network;
    if (p->pump) uasync_call_soon_cancel(net->inst->ua, p->pump);
    peer_cancel_wait(p);
    peer_reset(p, STREAM_REASON_STOPPED, 0, NULL);
    struct ll_entry *e;
    while ((e = queue_data_get(p->control))) { queue_dgram_free(e); queue_entry_free(e); }
    queue_free(p->control);
    node_conn_direct_set_callback(p->ncd, NULL, NULL); node_conn_direct_close(p->ncd);
    struct stream_peer **link = &net->peers;
    while (*link != p) link = &(*link)->next;
    *link = p->next; net->peer_count--;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream peer released peer=%s", STREAM_ADDRESS_TEXT(p->address)); u_free(p);
}

static struct STREAM_RECEIVE *flow_find(struct stream_peer *p, const struct stream_message *m, int outgoing) {
    for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
        if (f->outgoing == outgoing && f->info.state != STREAM_RECEIVE_CLOSED && same_stream(&f->descriptor, &m->descriptor)
            && f->subblock == m->subblock && (m->handle ? f->info.handle == m->handle : !f->info.handle)) return f;
    return NULL;
}

/* Местный отказ не является ошибкой поставщика; лимиты проверяются до выделения памяти. */
static int budget_check(struct stream_network *net, enum stream_flow_budget budget, unsigned peer_temporary) {
    limits_refresh(net);
    int temporary = budget == STREAM_FLOW_BACKGROUND || budget == STREAM_FLOW_RESCUE;
    const char *reason = budget == STREAM_FLOW_REGULAR && net->regular_used >= net->limits.regular ? "regular_slots"
        : temporary && net->temporary_used >= net->limits.temporary ? "temporary_slots"
        : temporary && peer_temporary >= net->limits.peer_temporary ? "peer_temporary_slots"
        : budget == STREAM_FLOW_BACKGROUND && net->background_used >= net->limits.background ? "background_slots" : NULL;
    if (!reason) return STREAM_OK;
    uint64_t now = get_time_tb() / 10;
    if (now >= net->budget_report_ms) {
        net->budget_report_ms = now + 1000;
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream local budget waits self=%s reason=%s regular=%u/%u temporary=%u/%u"
            " background=%u/%u flows=%u peer_temporary=%u/%u supplier_penalty=0",
            sc_public_key_hex(net->inst->my_ed25519_pubkey, (char[65]){0}), reason, net->regular_used, net->limits.regular,
            net->temporary_used, net->limits.temporary, net->background_used, net->limits.background,
            net->flow_count, peer_temporary, net->limits.peer_temporary);
    }
    return STREAM_LOCAL_BUSY;
}

static int flow_new(struct stream_peer *p, const struct stream_descriptor *d, uint8_t sub, int outgoing,
                    enum stream_flow_budget budget, struct STREAM_RECEIVE **out) {
    unsigned count = 0, objects = 0;
    for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
        objects++;
        if (f->info.state != STREAM_RECEIVE_CLOSED && (f->budget == STREAM_FLOW_BACKGROUND || f->budget == STREAM_FLOW_RESCUE)) count++;
    }
    int result = budget_check(p->network, budget, count);
    if (result == STREAM_OK && objects >= p->network->limits.regular + p->network->limits.temporary) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream peer state full peer=%s objects=%u limit=%u DATA_reserve_untouched=1",
            STREAM_ADDRESS_TEXT(p->address), objects, p->network->limits.regular + p->network->limits.temporary);
        return STREAM_LOCAL_BUSY;
    }
    if (result != STREAM_OK) {
        if (budget == STREAM_FLOW_REGULAR) {
            p->network->wanted_regular = 1; p->network->wanted_until_ms = get_time_tb() / 10 + 1500;
        }
        return result;
    }
    struct STREAM_RECEIVE *f = u_calloc(1, sizeof(*f));
    if (!f) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream issuance allocation failed"); return STREAM_ERR_MEMORY; }
    f->history = u_calloc(1, sizeof(*f->history));
    if (!f->history) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream issuance history allocation failed"); u_free(f); return STREAM_ERR_MEMORY; }
    f->budget = budget;
    if (budget == STREAM_FLOW_REGULAR) p->network->regular_used++;
    if (budget == STREAM_FLOW_BACKGROUND || budget == STREAM_FLOW_RESCUE) p->network->temporary_used++;
    if (budget == STREAM_FLOW_BACKGROUND) p->network->background_used++;
    f->peer = p; f->descriptor = *d; f->subblock = sub; f->outgoing = outgoing;
    f->admitted_ms = get_time_tb() / 10; f->watch_seq = UINT64_MAX; stream_load_init(&f->history->load, f->admitted_ms);
    stream_metrics_init(&f->history->metrics); stream_metrics_init(&f->history->offered_metrics);
    if (!outgoing) { stream_load_changed(&p->load); stream_load_changed(&p->network->load); }
    f->info.peer = p->address; f->info.subblock = sub; f->deadline = get_time_tb() + STREAM_CONNECT_TB;
    f->next = p->flows; p->flows = f; p->network->flow_count++; p->idle = 0; *out = f; return STREAM_OK;
}

/* Точная локальная эпоха проверяется до чтения подписанного профиля пира (unsubscribed => нет crypto). */
static int authorized(struct stream_network *net, struct stream_address peer, const struct stream_message *m, struct stream_local *local) {
    if (stream_local_get(net->inst, &m->descriptor, local) != STREAM_OK) return 0;
    struct stream_member_state state;
    int result = stream_member_load(net->inst, m->descriptor.group_id, peer, &state, NULL);
    if (result != STREAM_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream peer authorization failed peer=%s group=%llx result=%d",
                   STREAM_ADDRESS_TEXT(peer), (unsigned long long)m->descriptor.group_id, result); return 0;
    }
    return 1;
}

/* INFO и ACCEPT описывают реальный последний/начальный блок с его историей, а не BGP-метрику. */
static int info_payload(struct stream_local *local, uint8_t sub, uint64_t seq, struct stream_address peer, struct stream_address self, uint8_t *payload) {
    const uint8_t *wire; size_t length; uint64_t actual, first, last; struct stream_path path;
    int range = stream_buffer_range(local->buffer, sub, &first, &last);
    if (range != STREAM_OK && local->publisher && (seq == UINT64_MAX || seq == 0)) {
        path = (struct stream_path){.count = 1, .nodes = {self}};
        stream_write_be(payload + 8, seq, 8);
        stream_write_be(payload + 16, UINT64_MAX, 8); stream_write_be(payload + 24, UINT64_MAX, 8);
        payload[32] = local->descriptor.n; payload[33] = local->descriptor.r;
        stream_write_be(payload + 34, local->descriptor.block_size, 2); stream_path_write(payload + STREAM_INFO_PATH_OFF, &path);
        return 0;
    }
    int result = stream_buffer_read(local->buffer, seq, sub, &wire, &length, &actual);
    int tail = range == STREAM_OK && last < UINT64_MAX - 1 && seq == last + 1;
    if (tail) result = stream_buffer_read(local->buffer, UINT64_MAX, sub, &wire, &length, &actual);
    if (result == STREAM_EXPIRED) return STREAM_REASON_EXPIRED;
    if (result != STREAM_OK
        || range != STREAM_OK
        || stream_buffer_path(local->buffer, actual, sub, &path) != STREAM_OK) return STREAM_REASON_POSITION;
    if (stream_path_validate(&path, local->descriptor.source, self, peer) != STREAM_OK || path.count == STREAM_PATH_MAX)
        return STREAM_REASON_LOOP;
    stream_write_be(payload + 8, tail ? seq : actual, 8); stream_write_be(payload + 16, first, 8); stream_write_be(payload + 24, last, 8);
    payload[32] = local->descriptor.n; payload[33] = local->descriptor.r;
    stream_write_be(payload + 34, local->descriptor.block_size, 2);
    stream_path_write(payload + STREAM_INFO_PATH_OFF, &path); return 0;
}

static int parse_info(const struct stream_message *m, const struct stream_descriptor *d, struct stream_address peer, struct stream_address self,
                      struct stream_receive_info *info, uint64_t *start) {
    struct stream_path path; size_t used;
    uint64_t actual = m->length >= STREAM_INFO_SIZE ? stream_read_be(m->payload + 8, 8) : UINT64_MAX;
    uint64_t first = m->length >= STREAM_INFO_SIZE ? stream_read_be(m->payload + 16, 8) : UINT64_MAX;
    uint64_t last = m->length >= STREAM_INFO_SIZE ? stream_read_be(m->payload + 24, 8) : UINT64_MAX;
    int empty = first == UINT64_MAX && last == UINT64_MAX;
    if (m->length < STREAM_INFO_SIZE || m->payload[32] != d->n || m->payload[33] != d->r
        || stream_read_be(m->payload + 34, 2) != d->block_size
        || stream_path_parse(m->payload + STREAM_INFO_PATH_OFF, m->length - STREAM_INFO_PATH_OFF, &path, &used) != STREAM_OK
        || m->length != STREAM_INFO_PATH_OFF + used
        || path.count == STREAM_PATH_MAX || stream_path_validate(&path, d->source, peer, self) != STREAM_OK
        || (!empty && (actual == UINT64_MAX || first > last || last == UINT64_MAX
            || (actual > last && !(m->command == STREAM_CMD_ACCEPT && last < UINT64_MAX - 1 && actual == last + 1))))
        || (empty && (!stream_address_equal(peer, d->source) || path.count != 1
            || actual != (m->command == STREAM_CMD_ACCEPT ? 0 : UINT64_MAX)))) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream INFO/ACCEPT invalid command=%u bytes=%zu", m->command, m->length);
        return STREAM_ERR_FORMAT;
    }
    const uint8_t *offer = m->payload + STREAM_INFO_OFFER_OFF;
    uint64_t key = stream_read_be(m->payload + STREAM_INFO_KEY_OFF, 8);
    uint64_t key_last = stream_read_be(m->payload + STREAM_INFO_KEY_OFF + 8, 8);
    int key_valid = !!(offer[0] & 32);
    if ((offer[0] & ~63u) || !!(offer[0] & 16) == empty || stream_read_be(offer + 3, 2) > 10000
        || (key_valid && (empty || key > key_last || key_last == UINT64_MAX || key < first || key_last > last))
        || (!key_valid && (key != UINT64_MAX || key_last != UINT64_MAX))) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream invalid offer flags=%u cpu=%llu", offer[0],
                   (unsigned long long)stream_read_be(offer + 3, 2)); return STREAM_ERR_FORMAT;
    }
    const uint8_t *position = m->payload + STREAM_INFO_POSITION_OFF;
    uint64_t requested = stream_read_be(position, 8), contiguous = stream_read_be(position + 8, 8);
    if (position[24] > 1 || (position[24] && (empty || requested == UINT64_MAX || requested < first
        || contiguous < requested || contiguous > last)) || (!position[24] && contiguous != UINT64_MAX)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream invalid exact position offer peer=%s", STREAM_ADDRESS_TEXT(peer));
        return STREAM_ERR_FORMAT;
    }
    info->requested_seq = requested; info->contiguous_last = contiguous; info->requested_present = position[24];
    info->supplier_queue_age_ms = stream_read_be(position + 16, 8);
    *start = stream_read_be(m->payload + 8, 8);
    info->path = path; info->first_available = stream_read_be(m->payload + 16, 8);
    info->last_available = stream_read_be(m->payload + 24, 8); info->info_ms = get_time_tb() / 10;
    info->key_seq = key; info->key_last_seq = key_last; info->key_valid = key_valid;
    const uint8_t *times = m->payload + STREAM_INFO_TIME_OFF;
    info->live_head_us = stream_read_be(times, 8); info->media_pts_us = stream_read_be(times + 8, 8);
    info->key_pts_us = stream_read_be(times + 16, 8);
    if ((info->live_head_us != UINT64_MAX && info->live_head_us > INT64_MAX)
        || (info->media_pts_us != UINT64_MAX && info->media_pts_us > INT64_MAX)
        || (key_valid && (info->key_pts_us > INT64_MAX || info->media_pts_us == UINT64_MAX || info->key_pts_us > info->media_pts_us))
        || (!key_valid && info->key_pts_us != UINT64_MAX)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream invalid media positions peer=%s head=%llu media=%llu key=%llu valid=%d",
            STREAM_ADDRESS_TEXT(peer), (unsigned long long)info->live_head_us, (unsigned long long)info->media_pts_us,
            (unsigned long long)info->key_pts_us, key_valid); return STREAM_ERR_FORMAT;
    }
    info->accepting = (offer[0] & 1) != 0; info->path_rtt_valid = (offer[0] & 2) != 0;
    info->progress_valid = (offer[0] & 4) != 0; info->cpu_valid = (offer[0] & 8) != 0;
    info->free_slots = (uint16_t)stream_read_be(offer + 1, 2); info->cpu_basis_points = (uint16_t)stream_read_be(offer + 3, 2);
    info->supplier_queue_bytes = stream_read_be(offer + 5, 8); info->progress_age_ms = stream_read_be(offer + 13, 8);
    info->path_rtt_ms = (uint32_t)stream_read_be(offer + 21, 4); return STREAM_OK;
}

/* Предложение привязано к фактическому пути последнего блока; другой upstream не подтверждает его RTT. */
static void offer_payload(struct stream_peer *p, struct stream_local *local, uint8_t sub, uint8_t *payload) {
    struct stream_network *net = p->network; uint64_t now = get_time_tb() / 10;
    uint8_t *offer = payload + STREAM_INFO_OFFER_OFF;
    limits_refresh(net);
    stream_write_be(payload + STREAM_INFO_WINDOW_OFF, p->window_current, 2);
    unsigned slots = net->regular_used < net->limits.regular ? net->limits.regular - net->regular_used : 0;
    offer[0] = !local->waiting && !net->global_pressure && !p->pressure.active && !p->cooling ? 1 : 0;
    uint8_t *exact = payload + STREAM_INFO_POSITION_OFF;
    stream_write_be(exact, UINT64_MAX, 8); stream_write_be(exact + 8, UINT64_MAX, 8);
    stream_write_be(exact + 16, p->load.snapshot.oldest_age_ms, 8); exact[24] = 0;
    stream_write_be(payload + STREAM_INFO_KEY_OFF, UINT64_MAX, 8);
    stream_write_be(payload + STREAM_INFO_KEY_OFF + 8, UINT64_MAX, 8);
    uint8_t *times = payload + STREAM_INFO_TIME_OFF;
    stream_write_be(times, local->live_head_us, 8); stream_write_be(times + 8, UINT64_MAX, 8); stream_write_be(times + 16, UINT64_MAX, 8);
    struct stream_media_recovery position = {.video_us = -1};
    int media = local->media && stream_media_store_recovery(local->media, &position) == STREAM_OK;
    if (media && position.video_us >= 0) {
        stream_write_be(times + 8, (uint64_t)position.video_us, 8);
        if (local->live_head_us == UINT64_MAX || (uint64_t)position.video_us > local->live_head_us)
            stream_write_be(times, (uint64_t)position.video_us, 8);
    }
    if (media && position.key_valid
        && position.key_seq >= stream_read_be(payload + 16, 8)
        && position.key_last_seq <= stream_read_be(payload + 24, 8)) {
        int retained = 1;
        for (uint64_t seq = position.key_seq; seq <= position.key_last_seq; seq++) {
            const uint8_t *wire; size_t length; uint64_t actual;
            if (stream_buffer_read(local->buffer, seq, sub, &wire, &length, &actual) != STREAM_OK) { retained = 0; break; }
        }
        if (retained) {
            offer[0] |= 32; stream_write_be(payload + STREAM_INFO_KEY_OFF, position.key_seq, 8);
            stream_write_be(payload + STREAM_INFO_KEY_OFF + 8, position.key_last_seq, 8);
            stream_write_be(times + 16, (uint64_t)position.key_us, 8);
        }
    }
    if (stream_read_be(payload + 16, 8) != UINT64_MAX) offer[0] |= 16;
    stream_write_be(offer + 1, slots, 2);
    double cpu = net->cpu.valid ? net->cpu.average_percent : 0;
    if (cpu < 0) cpu = 0; if (cpu > 100) cpu = 100;
    if (net->cpu.valid) offer[0] |= 8;
    stream_write_be(offer + 3, (uint16_t)(cpu * 100), 2); stream_write_be(offer + 5, p->load.queued.bytes, 8);
    struct stream_metric_snapshot snapshot = {0};
    int valid = stream_local_metrics(net->inst, &local->descriptor, sub, now, &snapshot) == STREAM_OK;
    stream_metric_write(payload + STREAM_INFO_METRIC_OFF, &snapshot, valid);
    if (valid) offer[0] |= 4;
    stream_write_be(offer + 13, snapshot.progress_age_ms, 8);
    if (local->publisher) { offer[0] |= 2; return; }
    struct stream_path path; size_t used;
    if (stream_path_parse(payload + STREAM_INFO_PATH_OFF, 1 + payload[STREAM_INFO_PATH_OFF] * SC_PUBKEY_SIZE, &path, &used) != STREAM_OK) return;
    for (struct stream_peer *up = net->peers; up; up = up->next) {
        for (struct STREAM_RECEIVE *f = up->flows; f; f = f->next) {
            if (!f->outgoing || f->info.probe_only || f->info.state != STREAM_RECEIVE_ACTIVE || f->subblock != sub
                || !same_stream(&f->descriptor, &local->descriptor) || !f->info.rtt_valid || !f->info.path_rtt_valid
                || now - f->info.info_ms > 30000 || path.count != f->info.path.count + 1
                || memcmp(path.nodes, f->info.path.nodes, f->info.path.count * sizeof(struct stream_address))) continue;
            uint64_t total = (uint64_t)f->info.rtt_ms + f->info.path_rtt_ms;
            offer[0] |= 2; stream_write_be(offer + 21, total > UINT32_MAX ? UINT32_MAX : total, 4); return;
        }
    }
}

/* Объясняет именно локальный допуск; наличие CPU/слотов не доказывает достаточность полосы. */
static void admission_report(struct stream_peer *p, const struct stream_message *m, uint64_t seq, const char *stage, int reason) {
    if (!debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_STREAM)) return;
    struct stream_network *net = p->network; unsigned flows = 0;
    for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) flows++;
    const char *resource = net->global_pressure ? "node_pressure" : p->pressure.active ? "peer_pressure"
        : p->cooling ? "peer_cooling" : net->regular_used >= net->limits.regular ? "regular_slots" : "available";
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream admission self=%s group=%llx source=%s epoch=%llx peer=%s sub=%u"
        " stage=%s need=%llu reason=%d resource=%s peer_flows=%u node_flows=%u control=%d/%u regular=%u/%u temporary=%u/%u"
        " queue_B=%llu queue_age_ms=%llu growth_Bps=%llu cpu_pct=%.1f/%d loop_late_ms=%llu",
        sc_public_key_hex(net->inst->my_ed25519_pubkey, (char[65]){0}), (unsigned long long)m->descriptor.group_id,
        STREAM_ADDRESS_TEXT(m->descriptor.source), (unsigned long long)m->descriptor.source_epoch,
        STREAM_ADDRESS_TEXT(p->address), m->subblock, stage, (unsigned long long)seq, reason, resource,
        flows, net->flow_count, p->control->count, net->limits.control,
        net->regular_used, net->limits.regular, net->temporary_used, net->limits.temporary,
        (unsigned long long)p->load.queued.bytes, (unsigned long long)p->load.snapshot.oldest_age_ms,
        (unsigned long long)p->load.snapshot.growth_rate, net->cpu.average_percent, net->cpu.valid,
        (unsigned long long)net->callback_late_ms);
}

/* Один пакет на свободный порог; собственный буфер переживает синхронное освобождение entry в etcp_send. */
static int raw_send(struct stream_peer *p, struct stream_message m, const uint8_t *payload, size_t length) {
    if (!topo_group_chat_key_allowed(p->network->inst, m.descriptor.group_id, p->address.ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream raw send denied group=%llx peer=%s command=%u",
            (unsigned long long)m.descriptor.group_id, STREAM_ADDRESS_TEXT(p->address), m.command);
        return STREAM_ERR_NOT_MEMBER;
    }
    struct ETCP_CONN *conn = node_conn_direct_get_conn(p->ncd);
    struct ll_entry *e = ll_alloc_lldgram((uint16_t)(STREAM_WIRE_HEADER + length));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream raw allocation failed bytes=%zu", length); return STREAM_ERR_MEMORY; }
    m.sender_epoch = p->own_epoch; m.receiver_epoch = p->other_epoch;
    if (stream_packet_header(p->network->inst, e->dgram, &m) != STREAM_OK) {
        queue_dgram_free(e); queue_entry_free(e); return STREAM_ERR_NOT_MEMBER;
    }
    if (length) memcpy(e->dgram + STREAM_WIRE_HEADER, payload, length);
    e->len = STREAM_WIRE_HEADER + length;
    if (etcp_send(conn, e) != 0) {
        queue_dgram_free(e); queue_entry_free(e);
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream raw send failed peer=%s command=%u", STREAM_ADDRESS_TEXT(p->address), m.command);
        return STREAM_ERR_STATE;
    }
    if (p->own_epoch != m.sender_epoch || !p->up) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream raw send invalidated by reset peer=%s command=%u",
            STREAM_ADDRESS_TEXT(p->address), m.command); return STREAM_ERR_STATE;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "stream raw sent peer=%s command=%u handle=%llu bytes=%zu",
                STREAM_ADDRESS_TEXT(p->address), m.command, (unsigned long long)m.handle, length);
    return STREAM_OK;
}

/* Путь измерения относится к очередной DATA; пустая очередь сохраняет путь последнего доступного блока. */
static int telemetry_send(struct STREAM_RECEIVE *f, uint64_t now) {
    struct stream_peer *p = f->peer;
    struct stream_local local;
    if (stream_local_get(p->network->inst, &f->descriptor, &local) != STREAM_OK) return 0;
    const uint8_t *wire; size_t length; uint64_t actual;
    struct stream_path path = {.count = 1, .nodes = {stream_address_read(p->network->inst->my_ed25519_pubkey)}};
    int found = stream_buffer_read(local.buffer, f->info.next, f->subblock, &wire, &length, &actual);
    if (found != STREAM_OK) found = stream_buffer_read(local.buffer, UINT64_MAX, f->subblock, &wire, &length, &actual);
    if (found == STREAM_OK) {
        if (stream_buffer_path(local.buffer, actual, f->subblock, &path) != STREAM_OK) return 0;
    } else if (!local.publisher) return 0;
    if (stream_path_validate(&path, f->descriptor.source, stream_address_read(p->network->inst->my_ed25519_pubkey), p->address) != STREAM_OK) return 0;
    struct stream_telemetry t = {0};
    if (path.count > 1) {
        int matched = stream_buffer_get_telemetry(local.buffer, f->subblock, now, &t) == STREAM_OK && t.count + 1 == path.count;
        for (unsigned i = 0; matched && i < t.count; i++) if (!stream_address_equal(t.rows[i].node, path.nodes[i])) matched = 0;
        if (!matched) {
            t = (struct stream_telemetry){.count = (uint8_t)(path.count - 1), .received_ms = now};
            for (unsigned i = 0; i < t.count; i++) t.rows[i] = (struct stream_telemetry_row){.node = path.nodes[i],
                .age_ms = UINT32_MAX, .flags = STREAM_TELEMETRY_STALE};
        }
    }
    struct stream_buffer_delay delay;
    if (stream_buffer_get_delay(local.buffer, f->info.next, f->subblock, &delay) != STREAM_OK) return 0;
    if (local.publisher && local.pending.valid) {
        if (local.pending.video) {
            uint64_t first_pts = delay.frames ? delay.first_pts : local.pending.first_pts;
            uint32_t period = local.pending.period_us ? local.pending.period_us : delay.period_us;
            delay.frames += local.pending.started + (local.pending.continuation && !delay.frames);
            if (!period || local.pending.last_pts < first_pts) delay.known = 0;
            else delay.video_us = local.pending.last_pts - first_pts + period;
        }
        delay.audio_us += local.pending.audio_us;
    }
    if (p->network->telemetry_sequence == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream telemetry sequence exhausted"); return 0;
    }
    struct stream_telemetry_row *own = &t.rows[t.count++];
    *own = (struct stream_telemetry_row){.node = stream_address_read(p->network->inst->my_ed25519_pubkey), .sample = ++p->network->telemetry_sequence,
        .buffered_us = delay.known ? (delay.video_us > delay.audio_us ? delay.video_us : delay.audio_us) : 0,
        .frames = delay.known ? (delay.frames > UINT32_MAX ? UINT32_MAX : (uint32_t)delay.frames) : 0,
        .flags = delay.known ? STREAM_TELEMETRY_KNOWN : 0};
    uint8_t payload[1 + STREAM_PATH_MAX * STREAM_TELEMETRY_ROW_SIZE];
    struct stream_message m = {.descriptor = f->descriptor, .handle = f->info.handle, .subblock = f->subblock,
        .command = STREAM_CMD_TELEMETRY};
    if (raw_send(p, m, payload, stream_telemetry_write(payload, &t)) != STREAM_OK) return 0;
    f->telemetry_sent_ms = now; f->telemetry_dirty = 0;
    DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "stream telemetry TX peer=%s sub=%u nodes=%u own_frames=%u own_us=%llu total_us=%llu",
        STREAM_ADDRESS_TEXT(p->address), f->subblock, t.count, own->frames,
        (unsigned long long)own->buffered_us, (unsigned long long)stream_telemetry_total(&t));
    return 1;
}

/* Нижняя граница прогресса всех выбранных выдач. INFO/REQUESTED ещё могут принести старую пачку и запрещают скачок. */
static void receive_progress(struct stream_network *net, const struct stream_descriptor *d) {
    uint8_t seen[RS_MAX_BLOCKS] = {0}; unsigned distinct = 0; uint64_t before = UINT64_MAX;
    for (struct stream_peer *p = net->peers; p; p = p->next) {
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            if (!f->outgoing || f->info.probe_only || f->info.state == STREAM_RECEIVE_CLOSED || !same_stream(&f->descriptor, d)) continue;
            if (f->info.state < STREAM_RECEIVE_ACTIVE) return;
            if (!seen[f->subblock]) { seen[f->subblock] = 1; distinct++; }
            if (f->info.next < before) before = f->info.next;
        }
    }
    if (distinct >= (unsigned)(d->n - d->r)) stream_local_progress(net->inst, d, before);
}

/* Производитель не хранит готовые DATA: cursor проверяется непосредственно перед копированием в ETCP. */
static int downstream_send(struct STREAM_RECEIVE *f) {
    struct stream_peer *p = f->peer; struct stream_local local;
    if (stream_local_get(p->network->inst, &f->descriptor, &local) != STREAM_OK) { flow_close(f, STREAM_REASON_STOPPED, 1); return 0; }
    struct stream_message m = {.descriptor = f->descriptor, .handle = f->info.handle, .subblock = f->subblock};
    const uint8_t *wire; size_t length; uint64_t actual;
    int result = f->info.next == UINT64_MAX ? STREAM_WAIT
                 : stream_buffer_read(local.buffer, f->info.next, f->subblock, &wire, &length, &actual);
    if (result == STREAM_OK) {
        if (f->tail_waiting) {
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream relay tail resumed peer=%s source=%s sub=%u seq=%llu after_signed_END=1",
                STREAM_ADDRESS_TEXT(p->address), STREAM_ADDRESS_TEXT(f->descriptor.source), f->subblock,
                (unsigned long long)f->info.next);
            f->tail_waiting = 0;
        }
        struct stream_path path;
        if (stream_buffer_path(local.buffer, actual, f->subblock, &path) != STREAM_OK
            || stream_path_validate(&path, f->descriptor.source, stream_address_read(p->network->inst->my_ed25519_pubkey), p->address) != STREAM_OK) {
            flow_close(f, STREAM_REASON_LOOP, 1); return 0;
        }
        uint8_t payload[1 + STREAM_PATH_MAX * SC_PUBKEY_SIZE + STREAM_DATA_HEAD_SIZE + STREAM_FRAME_TIMING_SIZE + STREAM_BLOCK_MAX_SIZE];
        size_t path_bytes = stream_path_write(payload, &path);
        stream_write_be(payload + path_bytes, local.live_head_us, STREAM_DATA_HEAD_SIZE); path_bytes += STREAM_DATA_HEAD_SIZE;
        struct stream_frame_timing timing = {0};
        if (stream_buffer_get_timing(local.buffer, actual, &timing) != STREAM_OK) return 0;
        stream_frame_timing_write(payload + path_bytes, &timing); path_bytes += STREAM_FRAME_TIMING_SIZE;
        memcpy(payload + path_bytes, wire, length);
        m.command = STREAM_CMD_DATA;
        if (raw_send(p, m, payload, path_bytes + length) != STREAM_OK) { flow_close(f, STREAM_REASON_TRANSPORT, 0); return 0; }
        if (f->info.state == STREAM_RECEIVE_ACTIVE) {
            load_account(f, 1, length, 0, 1); f->info.next = actual + 1; f->info.blocks++;
        }
        return 1;
    }
    if (result == STREAM_EXPIRED || result == STREAM_GAP) {
        uint64_t next;
        int available = stream_buffer_next_seq(local.buffer, f->info.next, f->subblock, &next) == STREAM_OK;
        if (!available && local.finished && local.last != UINT64_MAX && f->info.next <= local.last) {
            /* END быстрой части задаёт границу публикации, а не готовность всех частей relay. */
            if (local.publisher || local.next > local.last) { next = local.last + 1; available = 1; }
            else if (!f->tail_waiting) {
                f->tail_waiting = 1;
                DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream relay tail waits peer=%s source=%s sub=%u seq=%llu"
                    " local_next=%llu signed_last=%llu preserve_missing_DATA=1",
                    STREAM_ADDRESS_TEXT(p->address), STREAM_ADDRESS_TEXT(f->descriptor.source), f->subblock,
                    (unsigned long long)f->info.next, (unsigned long long)local.next, (unsigned long long)local.last);
            }
        }
        if (available && next > f->info.next) {
            uint8_t payload[16]; stream_write_be(payload, f->info.next, 8); stream_write_be(payload + 8, next, 8);
            m.command = STREAM_CMD_GAP;
            if (raw_send(p, m, payload, sizeof(payload)) == STREAM_OK) {
                flow_gap_report(f, f->info.next, next, result == STREAM_EXPIRED ? "expired" : "missing");
                f->info.next = next; f->info.gaps++; return 1;
            }
            flow_close(f, STREAM_REASON_TRANSPORT, 0); return 0;
        }
    }
    if (local.finished && ((local.last != UINT64_MAX && f->info.next > local.last)
        || (local.last == UINT64_MAX && !f->info.blocks && !f->info.next))) {
        m.command = STREAM_CMD_END;
        if (raw_send(p, m, local.end, STREAM_END_SIZE) == STREAM_OK) {
            f->info.state = STREAM_RECEIVE_ENDED; stream_metrics_end(&f->history->offered_metrics);
            stream_load_changed(&p->load); stream_load_changed(&p->network->load); return 1;
        }
        flow_close(f, STREAM_REASON_TRANSPORT, 0);
    }
    return 0;
}

/* Вызывающий держит p->busy; синхронный REINIT внутри ETCP не позволяет зафиксировать отправку в старой эпохе. */
static int control_send(struct stream_peer *p, struct ETCP_CONN *conn, struct ll_entry *e) {
    if (!e) return 0;
    uint64_t epoch = p->own_epoch;
    unsigned command = e->dgram[2]; size_t bytes = e->len;
    if (etcp_send(conn, e) != 0) {
        queue_dgram_free(e); queue_entry_free(e);
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream control send failed peer=%s command=%u", STREAM_ADDRESS_TEXT(p->address), command);
        peer_reset(p, STREAM_REASON_TRANSPORT, STREAM_RESET_ROTATE, NULL); return 0;
    }
    if (p->own_epoch != epoch || !p->up) return 0;
    p->diagnostic.control_packets++; p->diagnostic.control_bytes += bytes;
    p->diagnostic.info_requests += command == STREAM_CMD_INFO_REQ;
    return 1;
}

/* APPLIED идёт перед новыми запросами; прежняя ёмкость RX сохраняется до встречного подтверждения и дренажа ответов. */
static int window_send(struct stream_peer *p, struct ETCP_CONN *conn) {
    if (!p->other_epoch || !p->control_descriptor.group_id) return 0;
    uint8_t payload[10]; struct stream_message m = {.descriptor = p->control_descriptor};
    unsigned target = p->network->limits.control;
    if (p->window_ack) {
        m.command = STREAM_CMD_WINDOW_APPLIED;
        stream_write_be(payload, p->window_ack, 8); stream_write_be(payload + 8, p->remote_window, 2);
        if (!control_send(p, conn, control_packet(p, m, payload, sizeof(payload)))) return 0;
        p->window_ack = 0; return 1;
    }
    if (!p->window_advertised || p->window_pending || target == p->window_current) return 0;
    if (p->window_serial == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream window serial exhausted peer=%s", STREAM_ADDRESS_TEXT(p->address));
        peer_reset(p, STREAM_REASON_PROTOCOL, STREAM_RESET_ROTATE, NULL); return 0;
    }
    m.command = STREAM_CMD_WINDOW_UPDATE;
    stream_write_be(payload, p->window_serial + 1, 8); stream_write_be(payload + 8, target, 2);
    struct ll_entry *e = control_packet(p, m, payload, sizeof(payload)); if (!e) return 0;
    p->window_serial++; p->window_pending = target;
    if (p->local_window < target) p->local_window = target;
    if (!control_send(p, conn, e)) return 0;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream window update peer=%s serial=%llu target=%u retained=%u replies=%u",
        STREAM_ADDRESS_TEXT(p->address), (unsigned long long)p->window_serial, target, p->local_window, p->reply_count);
    return 1;
}

/* Один запрос на свободное место окна; urgent раньше фона, порядок handles соответствует передаче в ETCP. */
static int request_send(struct stream_peer *p, struct ETCP_CONN *conn) {
    unsigned limit = p->remote_window ? p->remote_window : 1;
    if (limit > p->network->limits.control) limit = p->network->limits.control;
    if (p->request_count >= limit || p->window_ack) return 0;
    struct STREAM_RECEIVE *first = p->last_request && p->last_request->next ? p->last_request->next : p->flows;
    for (int priority = 1; priority >= 0; priority--) {
        struct STREAM_RECEIVE *f = first;
        if (!f) break;
        do {
            struct STREAM_RECEIVE *next = f->next ? f->next : p->flows;
            int urgent = f->seek_pending || f->budget == STREAM_FLOW_RESCUE || (f->slot && f->slot->replacement == f);
            if (!f->outgoing || f->info.state == STREAM_RECEIVE_CLOSED || urgent != priority
                || (!f->seek_pending && f->info.state >= STREAM_RECEIVE_REQUESTED)
                || (!urgent && p->remote_window && p->request_count + 1 >= limit)) { f = next; continue; }
            int info = !f->info_sent && !f->seek_pending;
            if (!info && !f->seek_pending && (!f->info_ready || f->info.probe_only || !p->remote_window)) { f = next; continue; }
            struct stream_local local;
            if (!topo_group_chat_key_allowed(p->network->inst, f->descriptor.group_id, p->address.ed)
                || stream_local_get(p->network->inst, &f->descriptor, &local) != STREAM_OK) {
                flow_close(f, STREAM_REASON_STOPPED, 1); f = next; continue;
            }
            uint64_t requested = f->seek_pending ? f->seek_target : f->requested;
            if (!info && !f->seek_pending) {
                int reason = !f->info.accepting ? STREAM_REASON_OVERLOAD : !f->info.free_slots ? STREAM_REASON_LIMIT : 0;
                if (!reason && requested != UINT64_MAX && !f->info.requested_present && f->info.last_available != UINT64_MAX) {
                    if (requested < f->info.first_available) reason = STREAM_REASON_EXPIRED;
                    else if (requested <= f->info.last_available) reason = STREAM_REASON_POSITION;
                }
                if (reason) { flow_close(f, reason, 0); f = next; continue; }
                if (requested == UINT64_MAX) {
                    if (local.anchored) requested = local.start;
                    else if (!f->bootstrap) { f = next; continue; }
                }
            }
            uint8_t payload[STREAM_INFO_REQUEST_SIZE] = {0}; size_t length;
            struct stream_message m = {.descriptor = f->descriptor, .subblock = f->subblock};
            if (info) {
                if (!f->probe) f->probe = new_epoch();
                if (!f->probe) { flow_close(f, STREAM_REASON_PROTOCOL, 0); f = next; continue; }
                m.command = STREAM_CMD_INFO_REQ; stream_write_be(payload, f->probe, 8); payload[8] = local.media != NULL;
                stream_write_be(payload + 9, requested, 8); length = STREAM_INFO_REQUEST_SIZE;
            } else {
                if (p->next_handle == UINT64_MAX) { flow_close(f, STREAM_REASON_LIMIT, 1); f = next; continue; }
                m.command = f->seek_pending ? STREAM_CMD_SEEK : STREAM_CMD_SUBSCRIBE; m.handle = p->next_handle;
                length = f->seek_pending ? 16 : 8;
                if (f->seek_pending) stream_write_be(payload, f->info.handle, 8);
                stream_write_be(payload + length - 8, requested, 8);
            }
            struct stream_pending_request *r = u_calloc(1, sizeof(*r));
            if (!r) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream pending request allocation failed"); return 0; }
            struct ll_entry *e = control_packet(p, m, payload, length);
            if (!e) { u_free(r); return 0; }
            *r = (struct stream_pending_request){.next = p->requests, .descriptor = m.descriptor,
                .id = info ? f->probe : m.handle, .command = m.command, .subblock = f->subblock};
            p->requests = r; p->request_count++; p->control_descriptor = m.descriptor; p->last_request = f;
            uint64_t now = get_time_tb();
            r->sent_ms = now / 10;
            if (f->deadline != UINT64_MAX && (!info || !f->info_started)) f->deadline = now + STREAM_CONNECT_TB;
            if (info) { f->info_sent = f->info_started = 1; f->info.state = STREAM_RECEIVE_INFO; f->probe_sent_ms = now / 10; }
            else {
                if (f->seek_pending) {
                    f->seek_from = f->info.handle; f->seek_pending = 0;
                    f->info.blocks = f->info.gaps = f->info.data_next = f->info.progress_ms = f->info.active_ms = 0;
                    f->info.recovery_ready = f->info.recovery_ms = f->info.metrics_valid = 0; f->watch_seq = UINT64_MAX;
                    stream_metrics_init(&f->history->metrics);
                    for (unsigned i = 0; i < p->requested.count; i++)
                        if (p->requested.cuts[i].handle == f->seek_from) p->requested.cuts[i].handle = m.handle;
                }
                f->info.handle = p->next_handle++; f->requested = f->info.start = requested;
                f->info.state = STREAM_RECEIVE_REQUESTED; f->info.subscribe_ms = now / 10;
            }
            if (!control_send(p, conn, e)) return 0;
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream request sent peer=%s command=%u handle=%llu sub=%u pending=%u window=%u urgent=%d",
                STREAM_ADDRESS_TEXT(p->address), m.command, (unsigned long long)m.handle, m.subblock, p->request_count, limit, urgent);
            return 1;
        } while (f != first);
    }
    return 0;
}

/* ACCEPT разрешает DATA только после передачи в транспорт; закрытый flow удерживает ответ до этого момента. */
static int reply_send(struct stream_peer *p, struct ETCP_CONN *conn) {
    struct ll_entry *e = queue_data_get(p->control); if (!e) return 0;
    struct stream_message m;
    if (stream_packet_parse(p->network->inst, e->dgram, e->len, &m) != STREAM_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream saved reply malformed peer=%s", STREAM_ADDRESS_TEXT(p->address));
        queue_dgram_free(e); queue_entry_free(e); peer_reset(p, STREAM_REASON_PROTOCOL, STREAM_RESET_ROTATE, NULL); return 0;
    }
    struct STREAM_RECEIVE *accepted = NULL;
    if (m.command == STREAM_CMD_ACCEPT) {
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
            if (!f->outgoing && f->info.handle == m.handle && f->subblock == m.subblock
                && same_stream(&f->descriptor, &m.descriptor)) { accepted = f; break; }
    }
    int initial = !m.handle && (m.command == STREAM_CMD_INFO_RESP || m.command == STREAM_CMD_REJECT);
    if ((m.command == STREAM_CMD_INFO_RESP || m.command == STREAM_CMD_ACCEPT)
        && !topo_group_chat_key_allowed(p->network->inst, m.descriptor.group_id, p->address.ed)) {
        uint8_t rejected[11] = {STREAM_REASON_MEMBER};
        if (initial) {
            memcpy(rejected + 1, m.payload, 8); stream_write_be(rejected + 9, p->window_current, 2);
        }
        if (accepted) flow_close(accepted, STREAM_REASON_MEMBER, 0);
        m.command = STREAM_CMD_REJECT; stream_wire_header(e->dgram, &m);
        memcpy(e->dgram + STREAM_WIRE_HEADER, rejected, initial ? 11 : 1); e->len = STREAM_WIRE_HEADER + (initial ? 11 : 1);
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream saved reply rejected after revoke peer=%s handle=%llu",
            STREAM_ADDRESS_TEXT(p->address), (unsigned long long)m.handle);
    }
    p->reply_count--;
    if (!control_send(p, conn, e)) return 0;
    if (initial) p->window_advertised = 1;
    if (!p->window_pending) p->local_window = p->reply_count > p->window_current ? p->reply_count : p->window_current;
    if (accepted) {
        accepted->accept_pending = 0;
        if (accepted->info.state != STREAM_RECEIVE_CLOSED) accepted->deadline = get_time_tb() + STREAM_LEASE_TB;
    }
    queue_resume_callback(p->control); return 1;
}

/* Закрытие и lease не используют запросное окно; DATA-ресурсы закрытого flow уже возвращены. */
static int maintenance_send(struct stream_peer *p, struct ETCP_CONN *conn, int closing) {
    uint64_t now = get_time_tb();
    for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
        if (closing) {
            if (f->info.state != STREAM_RECEIVE_CLOSED || f->accept_pending || (!f->close_pending && !f->seek_from)) continue;
        } else if (!f->outgoing || f->info.probe_only || f->info.state < STREAM_RECEIVE_ACTIVE
                   || f->info.state == STREAM_RECEIVE_CLOSED || now - f->renewed < STREAM_RENEW_TB) continue;
        int old = closing && f->seek_from;
        struct stream_message m = {.descriptor = f->descriptor, .handle = old ? f->seek_from : f->info.handle,
            .subblock = f->subblock, .command = !closing ? STREAM_CMD_RENEW
                : old || f->outgoing ? STREAM_CMD_UNSUBSCRIBE : STREAM_CMD_CLOSED};
        uint8_t reason = (uint8_t)f->info.reason;
        if (!control_send(p, conn, control_packet(p, m, &reason, closing ? 1 : 0))) return 0;
        if (!closing) f->renewed = now;
        else {
            if (old) f->seek_from = 0; else f->close_pending = 0;
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream close sent peer=%s command=%u handle=%llu DATA_slot_released=1",
                STREAM_ADDRESS_TEXT(p->address), m.command, (unsigned long long)m.handle);
        }
        return 1;
    }
    return 0;
}

/* Устаревшее предложение не переживает отмену; END сохраняется независимо от наличия DATA/локального контента. */
static int notice_send(struct stream_peer *p, struct ETCP_CONN *conn) {
    for (int i = 2; i >= 0; i--) {
        struct ll_entry *e = p->notices[i]; if (!e) continue;
        struct stream_message m;
        int valid = stream_packet_parse(p->network->inst, e->dgram, e->len, &m) == STREAM_OK;
        if (valid && i < 2) {
            struct stream_pressure_request *r = i ? &p->requested : &p->pressure;
            valid = r->active && m.length >= 8 && stream_read_be(m.payload, 8) == r->id
                && topo_group_chat_key_allowed(p->network->inst, m.descriptor.group_id, p->address.ed)
                && pressure_members_allowed(p, &m, 0);
        }
        p->notices[i] = NULL;
        if (!valid) { queue_dgram_free(e); queue_entry_free(e); continue; }
        return control_send(p, conn, e);
    }
    return 0;
}

/* Round-robin DATA не зависит от наполнения окна запросов и имеет гарантированную очередь после серии control. */
static int peer_data_send(struct stream_peer *p) {
    struct STREAM_RECEIVE *first = p->last_sent && p->last_sent->next ? p->last_sent->next : p->flows;
    struct STREAM_RECEIVE *f = first;
    if (!f) return 0;
    do {
        if (f->account_failed) flow_close(f, STREAM_REASON_PROTOCOL, 1);
        if (!f->outgoing && !f->accept_pending && f->info.state == STREAM_RECEIVE_ACTIVE && downstream_send(f)) {
            p->last_sent = f; return 1;
        }
        f = f->next ? f->next : p->flows;
    } while (f != first);
    return 0;
}

/* Один waiter на транспорт; круговой обход классов управления и не более8 control подряд при готовой DATA. */
static void peer_pump(void *arg) {
    struct stream_peer *p = arg; p->pump = NULL;
    if (!p->up || !p->own_epoch || p->network->stopping) return;
    limits_refresh(p->network);
    struct ETCP_CONN *conn = node_conn_direct_get_conn(p->ncd);
    if (!conn || !conn->send_input_q || conn->state == 2 || conn->close_requested || conn->reinit_pending || !conn->initialized) return;
    if (conn->send_input_q->count) {
        p->wait_q = conn->send_input_q;
        if (queue_waiter_wait(p->wait_q, &p->waiter, send_ready, p) < 0)
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream transport waiter failed peer=%s", STREAM_ADDRESS_TEXT(p->address));
        return;
    }
    p->busy++;
    uint64_t epoch = p->own_epoch, now = get_time_tb() / 10;
    int sent = p->control_burst >= STREAM_CONTROL_BURST && peer_data_send(p);
    if (sent) p->control_burst = 0;
    for (unsigned i = 0; !sent && i < 7 && p->own_epoch == epoch; i++) {
        unsigned turn = p->control_turn++ % 7;
        if (turn == 0) sent = window_send(p, conn);
        else if (turn == 1) sent = reply_send(p, conn);
        else if (turn == 2) sent = maintenance_send(p, conn, 1);
        else if (turn == 3) sent = maintenance_send(p, conn, 0);
        else if (turn == 4) sent = notice_send(p, conn);
        else if (turn == 5) sent = request_send(p, conn);
        else for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            if (f->outgoing || f->accept_pending || f->info.state != STREAM_RECEIVE_ACTIVE) continue;
            if ((f->telemetry_dirty || !f->telemetry_sent_ms || now - f->telemetry_sent_ms >= STREAM_TELEMETRY_PERIOD_MS)
                && telemetry_send(f, now)) { sent = 1; break; }
        }
        if (sent && p->control_burst < STREAM_CONTROL_BURST) p->control_burst++;
    }
    if (!sent && p->own_epoch == epoch && peer_data_send(p)) { sent = 1; p->control_burst = 0; }
    p->busy--; peer_sweep(p);
    if (sent && p->up && p->own_epoch == epoch) {
        p->wait_q = conn->send_input_q;
        if (queue_waiter_wait(p->wait_q, &p->waiter, send_ready, p) < 0)
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream post-send waiter failed peer=%s", STREAM_ADDRESS_TEXT(p->address));
    }
}

/* SUBSCRIBE выделяет слот; SEEK переносит существующий cursor вперёд с новым ID. Повтор ID/позиции идемпотентен. */
static void subscribe_request(struct stream_peer *p, struct stream_message m, struct stream_local *local, struct ll_entry *reply) {
    uint8_t payload[STREAM_INFO_SIZE + STREAM_PATH_MAX * SC_PUBKEY_SIZE] = {0};
    int reason = 0, seek = m.command == STREAM_CMD_SEEK;
    if (m.length != (seek ? 16u : 8u) || !m.handle || m.subblock >= local->descriptor.n) reason = STREAM_REASON_PROTOCOL;
    struct STREAM_RECEIVE *f = reason ? NULL : flow_find(p, &m, 0);
    uint64_t requested = !reason ? stream_read_be(m.payload + (seek ? 8 : 0), 8) : 0;
    uint64_t old_handle = !reason && seek ? stream_read_be(m.payload, 8) : 0;
    if (!reason && seek && (!old_handle || old_handle >= m.handle || requested == UINT64_MAX)) reason = STREAM_REASON_PROTOCOL;
    if (f && f->requested != requested) reason = STREAM_REASON_CONFLICT;
    if (!f && !reason && m.handle <= p->highwater) reason = STREAM_REASON_STALE;
    if (!f && !reason && local->waiting) { p->highwater = m.handle; reason = STREAM_REASON_STOPPED; }
    if (!seek && !f && !reason && (p->network->global_pressure || p->pressure.active || p->cooling)) {
        p->highwater = m.handle; reason = STREAM_REASON_OVERLOAD;
        p->load.rejected++; p->network->load.rejected++;
    }
    if (!f && !reason) {
        p->highwater = m.handle;
        if (seek) {
            struct stream_message previous = m; previous.handle = old_handle; f = flow_find(p, &previous, 0);
            if (!f || f->info.state != STREAM_RECEIVE_ACTIVE) reason = STREAM_REASON_STOPPED;
            else if (requested <= f->info.start) reason = STREAM_REASON_POSITION;
        }
        if (!reason) reason = info_payload(local, m.subblock, requested, p->address, stream_address_read(p->network->inst->my_ed25519_pubkey), payload);
        if (!reason) {
            if (!seek && flow_new(p, &local->descriptor, m.subblock, 0, STREAM_FLOW_REGULAR, &f) != STREAM_OK) reason = STREAM_REASON_LIMIT;
            if (!reason) {
                uint64_t count = 0, bytes = 0;
                stream_buffer_pending(local->buffer, stream_read_be(payload + 8, 8), m.subblock, &count, &bytes, NULL);
                int forward = !seek || requested >= f->info.next;
                if (seek && (forward ? count > f->history->load.queued.packets || bytes > f->history->load.queued.bytes
                    : count < f->history->load.queued.packets || bytes < f->history->load.queued.bytes)) {
                    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream SEEK ledger mismatch peer=%s handle=%llu pending=%llu/%llu retained=%llu/%llu",
                        STREAM_ADDRESS_TEXT(p->address), (unsigned long long)old_handle,
                        (unsigned long long)f->history->load.queued.packets, (unsigned long long)f->history->load.queued.bytes,
                        (unsigned long long)count, (unsigned long long)bytes);
                    reason = STREAM_REASON_PROTOCOL; goto rejected;
                }
                if (seek) {
                    uint64_t changed = forward ? f->history->load.queued.bytes - bytes : bytes - f->history->load.queued.bytes;
                    /* Сервер мог уже отправить KEY в транспорт. Если он потерян до получателя, выдаём его заново в том же слоте. */
                    if (forward) load_account(f, f->history->load.queued.packets - count, changed, 0, 0);
                    else load_account(f, count - f->history->load.queued.packets, changed, 1, 0);
                    for (unsigned i = 0; i < p->pressure.count; i++)
                        if (p->pressure.cuts[i].handle == old_handle) p->pressure.cuts[i].handle = m.handle;
                    f->info.blocks = f->info.gaps = 0; f->telemetry_dirty = 1;
                    stream_load_changed(&f->history->load); stream_load_changed(&p->load); stream_load_changed(&p->network->load);
                    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream SEEK accepted peer=%s sub=%u old_handle=%llu handle=%llu from=%llu to=%llu"
                        " removed_B=%llu requeued_B=%llu slots_unchanged=1 CPU_pressure=%d peer_pressure=%d",
                        STREAM_ADDRESS_TEXT(p->address), m.subblock, (unsigned long long)old_handle, (unsigned long long)m.handle,
                        (unsigned long long)f->info.next, (unsigned long long)requested,
                        (unsigned long long)(forward ? changed : 0), (unsigned long long)(forward ? 0 : changed),
                        p->network->global_pressure, p->pressure.active);
                } else load_account(f, count, bytes, 1, 0);
                f->info.handle = m.handle; f->requested = requested; f->info.start = f->info.next = stream_read_be(payload + 8, 8);
                if (f->info.start == UINT64_MAX) { f->info.start = f->info.next = 0; stream_write_be(payload + 8, 0, 8); }
                f->info.state = STREAM_RECEIVE_ACTIVE;
                f->pending_highest = stream_read_be(payload + 24, 8);
                if (f->pending_highest == UINT64_MAX) f->pending_highest = 0;
                const uint8_t *wire; size_t length; uint64_t seq;
                if (stream_buffer_read(local->buffer, UINT64_MAX, f->subblock, &wire, &length, &seq) == STREAM_OK)
                    stream_metrics_observe(&f->history->offered_metrics, seq, (uint16_t)stream_read_be(wire + STREAM_BLOCK_TIMESTAMP_OFF, 2),
                                           wire[STREAM_BLOCK_FLAGS_OFF], (uint32_t)length, get_time_tb() / 10);
            }
        }
    } else if (f && !reason) {
        reason = info_payload(local, m.subblock, f->info.start, p->address, stream_address_read(p->network->inst->my_ed25519_pubkey), payload);
        /* Идемпотентный ACCEPT возвращает первоначальный индекс даже если тот уже вытеснен. */
        if (reason == STREAM_REASON_POSITION || reason == STREAM_REASON_EXPIRED) {
            reason = info_payload(local, m.subblock, UINT64_MAX, p->address, stream_address_read(p->network->inst->my_ed25519_pubkey), payload);
            if (!reason) stream_write_be(payload + 8, f->info.start, 8);
        }
    }
rejected:
    admission_report(p, &m, requested, seek ? "SEEK" : "SUBSCRIBE", reason);
    if (reason) {
        /* Получатель уже сменил ID. При отказе освобождаем найденную старую выдачу, не оставляя её до lease. */
        if (seek && f && f->info.handle == old_handle) flow_close(f, reason, 0);
        uint8_t rejection[17] = {(uint8_t)reason}; uint64_t first = UINT64_MAX, last = UINT64_MAX;
        if (m.subblock < local->descriptor.n) stream_buffer_range(local->buffer, m.subblock, &first, &last);
        stream_write_be(rejection + 1, first, 8); stream_write_be(rejection + 9, last, 8);
        m.command = STREAM_CMD_REJECT; reply_commit(p, reply, m, rejection, sizeof(rejection));
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream %s rejected peer=%s handle=%llu sub=%u index=%llu reason=%d range=%llu..%llu watermark=%llu",
                   seek ? "SEEK" : "SUBSCRIBE", STREAM_ADDRESS_TEXT(p->address), (unsigned long long)m.handle, m.subblock, (unsigned long long)requested,
                   reason, (unsigned long long)first, (unsigned long long)last, (unsigned long long)p->highwater);
        return;
    }
    f->deadline = UINT64_MAX; f->accept_pending = 1;
    offer_payload(p, local, m.subblock, payload);
    m.command = STREAM_CMD_ACCEPT;
    reply_commit(p, reply, m, payload, STREAM_INFO_SIZE + payload[STREAM_INFO_PATH_OFF] * SC_PUBKEY_SIZE);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream ACCEPT queued peer=%s handle=%llu sub=%u start=%llu lease=6000ms",
               STREAM_ADDRESS_TEXT(p->address), (unsigned long long)m.handle, m.subblock, (unsigned long long)f->info.start);
}

/* Единственное изменение окна в пути; подтверждение отделяет старые запросы от последующих в упорядоченном ETCP. */
static void window_receive(struct stream_peer *p, const struct stream_message *m) {
    uint64_t serial = m->length == 10 ? stream_read_be(m->payload, 8) : 0;
    unsigned window = m->length == 10 ? (unsigned)stream_read_be(m->payload + 8, 2) : 0;
    int valid = !m->handle && serial && window >= 8 && window <= 4096;
    if (valid && m->command == STREAM_CMD_WINDOW_UPDATE) {
        valid = p->remote_window && (serial == p->remote_window_serial + 1
            || (serial == p->remote_window_serial && window == p->remote_window));
        if (valid) {
            p->remote_window = window; p->remote_window_serial = p->window_ack = serial;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream TX window applied peer=%s serial=%llu window=%u pending=%u",
                STREAM_ADDRESS_TEXT(p->address), (unsigned long long)serial, window, p->request_count);
        }
    } else if (valid) {
        valid = serial == p->window_serial && window == (p->window_pending ? p->window_pending : p->window_current);
        if (valid) {
            p->window_current = window; p->window_pending = 0;
            p->local_window = p->reply_count > window ? p->reply_count : window;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream RX window confirmed peer=%s serial=%llu target=%u retained=%u replies=%u",
                STREAM_ADDRESS_TEXT(p->address), (unsigned long long)serial, window, p->local_window, p->reply_count);
        }
    }
    if (!valid) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream window rejected peer=%s command=%u serial=%llu window=%u own=%llu remote=%llu",
            STREAM_ADDRESS_TEXT(p->address), m->command, (unsigned long long)serial, window,
            (unsigned long long)p->window_serial, (unsigned long long)p->remote_window_serial);
        peer_reset(p, STREAM_REASON_PROTOCOL, STREAM_RESET_ROTATE, NULL);
    } else peer_schedule(p);
}

/* Ответы/закрытия известной эпохи завершают обязательства и после удаления контента; DATA всегда проверяет допуск группы. */
static void dispatch(struct stream_network *net, struct stream_address sender, const uint8_t *wire, size_t length, int routed) {
    struct stream_message m; struct stream_local local;
    if (stream_packet_parse(net->inst, wire, length, &m) != STREAM_OK || !stream_address_set(sender) || stream_address_equal(sender, stream_address_read(net->inst->my_ed25519_pubkey))) return;
    if (m.command == STREAM_CMD_STATS) {
        struct stream_local publication;
        if (!routed || m.length != STREAM_STAT_FIELDS * 8u || !stream_address_equal(m.descriptor.source, stream_address_read(net->inst->my_ed25519_pubkey))
            || !topo_group_chat_key_allowed(net->inst, m.descriptor.group_id, sender.ed)
            || stream_local_get(net->inst, &m.descriptor, &publication) != STREAM_OK || !publication.publisher) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "statistics rejected sender=%s routed=%d bytes=%zu",
                STREAM_ADDRESS_TEXT(sender), routed, m.length); return;
        }
        struct stream_stat_sample sample = {.at_ms = get_time_tb() / 10, .sequence = m.handle, .generation = m.sender_epoch};
        for (unsigned i = 0; i < STREAM_STAT_FIELDS; i++) sample.v[i] = stream_read_be(m.payload + i * 8, 8);
        if (!sample.sequence || !sample.v[ST_DT] || sample.v[ST_NAT] > 3 || sample.v[ST_RELAY] > 1
            || sample.v[ST_PAUSED] > 1 || sample.v[ST_FINISHED] > 1 || sample.v[ST_OBSERVED] > sample.v[ST_DT]
            || sample.v[ST_PAUSE_MS] > sample.v[ST_AUDIO_OBSERVED]) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "statistics invalid sender=%s", STREAM_ADDRESS_TEXT(sender)); return;
        }
        struct stream_stats_entry *e = net->statistics;
        while (e && !(same_stream(&e->descriptor, &m.descriptor) && stream_address_equal(e->data.node, sender))) e = e->next;
        if (!e) {
            e = u_calloc(1, sizeof(*e));
            if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "statistics history allocation failed"); return; }
            e->descriptor = publication.descriptor; e->data.node = sender; e->next = net->statistics; net->statistics = e;
        }
        unsigned n = e->data.count;
        if (!n || e->data.samples[n-1].v[ST_NAT] != sample.v[ST_NAT]
            || e->data.samples[n-1].v[ST_PAUSED] != sample.v[ST_PAUSED]
            || e->data.samples[n-1].v[ST_RELAY] != sample.v[ST_RELAY])
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "statistics node state node=%s NAT=%llu relay=%llu paused=%llu",
                STREAM_ADDRESS_TEXT(sender), (unsigned long long)sample.v[ST_NAT],
                (unsigned long long)sample.v[ST_RELAY], (unsigned long long)sample.v[ST_PAUSED]);
        if (n && e->data.samples[n-1].generation == sample.generation && e->data.samples[n-1].sequence >= sample.sequence) return;
        unsigned drop = n == STREAM_STATS_HISTORY ? 1 : 0;
        while (drop < n && sample.at_ms - e->data.samples[drop].at_ms > STREAM_STATS_HISTORY_MS) drop++;
        if (drop) { memmove(e->data.samples, e->data.samples + drop, (n-drop) * sizeof(sample)); n -= drop; }
        e->data.samples[n++] = sample; e->data.count = n;
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "statistics delivery node=%s dt_ms=%llu useful_Bps=%llu subflows_IN=%llu OUT=%llu"
            " providers=%llu receivers=%llu parts=%llu/k%llu queue_B=%llu queue_parts=%llu queue_age_ms=%llu"
            " removed_B=%llu new_audio_glitches=%llu max_audio_glitch_ms=%lld gaps=%llu rejected=%llu duplicates=%llu migrations=%llu jumps=%llu uasync_cpu=%llu",
            STREAM_ADDRESS_TEXT(sender), (unsigned long long)sample.v[ST_DT], (unsigned long long)sample.v[ST_USEFUL],
            (unsigned long long)sample.v[ST_IN], (unsigned long long)sample.v[ST_OUT],
            (unsigned long long)sample.v[ST_PROVIDERS], (unsigned long long)sample.v[ST_RECEIVERS],
            (unsigned long long)sample.v[ST_PARTS], (unsigned long long)sample.v[ST_REQUIRED],
            (unsigned long long)sample.v[ST_QUEUE], (unsigned long long)sample.v[ST_QUEUE_PARTS],
            (unsigned long long)sample.v[ST_QUEUE_AGE], (unsigned long long)sample.v[ST_REMOVED],
            (unsigned long long)sample.v[ST_PAUSES], sample.v[ST_PAUSE_MAX] == UINT64_MAX ? -1LL : (long long)sample.v[ST_PAUSE_MAX],
            (unsigned long long)sample.v[ST_GAPS], (unsigned long long)sample.v[ST_REJECTED],
            (unsigned long long)sample.v[ST_DUPLICATES], (unsigned long long)sample.v[ST_MIGRATIONS],
            (unsigned long long)sample.v[ST_JUMPS], (unsigned long long)sample.v[ST_UASYNC]);

        uint8_t x[32];
        const struct TOPO_NODE *node = sc_identity_public_x("stream statistics", sender.ed, x) == SC_OK
            ? topo_node_registry_lookup(net->inst->topo_groups, sc_derive_node_id_from_pubkey(x), x) : NULL;
        snprintf(e->data.name, sizeof(e->data.name), "%s", node && node->node_name ? node->node_name : "");
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "statistics RX node=%s epoch=%llx seq=%llu RX=%llu TX=%llu lag=%llu pause=%llu CPU=%llu NAT=%llu",
            STREAM_ADDRESS_TEXT(sender), (unsigned long long)m.descriptor.source_epoch, (unsigned long long)sample.sequence,
            (unsigned long long)sample.v[ST_RX], (unsigned long long)sample.v[ST_TX], (unsigned long long)sample.v[ST_LAG],
            (unsigned long long)sample.v[ST_PAUSE_MS], (unsigned long long)sample.v[ST_CPU], (unsigned long long)sample.v[ST_NAT]);
        return;
    }
    struct stream_peer *p = peer_find(net, sender);
    int same_epoch = p && m.receiver_epoch == p->own_epoch && m.sender_epoch == p->other_epoch;
    if (m.command == STREAM_CMD_WINDOW_UPDATE || m.command == STREAM_CMD_WINDOW_APPLIED) {
        if (!routed && p && p->up && same_epoch) {
            p->busy++; window_receive(p, &m); p->busy--; peer_sweep(p);
        } else DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream stale window ignored peer=%s", STREAM_ADDRESS_TEXT(sender));
        return;
    }
    if (m.command == STREAM_CMD_CLOSED || m.command == STREAM_CMD_UNSUBSCRIBE) {
        if (same_epoch && (p->up || routed) && (!routed || m.command == STREAM_CMD_CLOSED) && m.handle && m.length == 1) {
            struct STREAM_RECEIVE *f = flow_find(p, &m, m.command == STREAM_CMD_CLOSED);
            p->busy++; if (f) flow_close(f, m.payload[0], m.command == STREAM_CMD_UNSUBSCRIBE);
            p->busy--; peer_sweep(p); stream_network_wake(net);
        } else DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream stale/invalid close ignored peer=%s", STREAM_ADDRESS_TEXT(sender));
        return;
    }
    int response = m.command == STREAM_CMD_INFO_RESP || m.command == STREAM_CMD_ACCEPT || m.command == STREAM_CMD_REJECT;
    int info_response = response && !m.handle && m.command != STREAM_CMD_ACCEPT;
    if (response) {
        if (routed || !p || !p->up || m.receiver_epoch != p->own_epoch || (!info_response && !same_epoch)) return;
        if (!request_complete(p, &m)) return;
        if (info_response) {
            unsigned window = m.command == STREAM_CMD_INFO_RESP && m.length >= STREAM_INFO_SIZE
                ? (unsigned)stream_read_be(m.payload + STREAM_INFO_WINDOW_OFF, 2)
                : m.command == STREAM_CMD_REJECT && m.length == 11 ? (unsigned)stream_read_be(m.payload + 9, 2) : 0;
            if (window < 8 || window > 4096) {
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream INFO window invalid peer=%s window=%u", STREAM_ADDRESS_TEXT(sender), window);
                peer_reset(p, STREAM_REASON_PROTOCOL, STREAM_RESET_ROTATE, NULL); return;
            }
            if (p->other_epoch && p->other_epoch != m.sender_epoch)
                peer_reset(p, STREAM_REASON_TRANSPORT, STREAM_RESET_KEEP_PENDING, flow_find(p, &m, 1));
            p->other_epoch = m.sender_epoch;
            if (!p->remote_window) {
                p->remote_window = window;
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream window negotiated peer=%s TX=%u RX=%u",
                    STREAM_ADDRESS_TEXT(sender), window, p->local_window);
            }
        }
    }
    int allowed = topo_group_chat_key_allowed(net->inst, m.descriptor.group_id, sender.ed);
    int request = m.command == STREAM_CMD_INFO_REQ || m.command == STREAM_CMD_SUBSCRIBE || m.command == STREAM_CMD_SEEK;
    if (!allowed && !(request && p && m.sender_epoch == p->other_epoch) && m.command != STREAM_CMD_OVERLOAD_END) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream receive denied group=%llx peer=%s command=%u routed=%d",
            (unsigned long long)m.descriptor.group_id, STREAM_ADDRESS_TEXT(sender), m.command, routed);
        if (response) {
            struct STREAM_RECEIVE *f = flow_find(p, &m, 1);
            if (f) flow_close(f, STREAM_REASON_MEMBER, 1);
            peer_sweep(p); stream_network_wake(net);
        }
        return;
    }
    if (m.command >= STREAM_CMD_RELAY_REQUEST && m.command <= STREAM_CMD_RELAY_CANCEL) {
        if (routed) { relay_dispatch(net, sender, &m); stream_network_wake(net); }
        else DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay command requires signed routed control peer=%s command=%u",
                        STREAM_ADDRESS_TEXT(sender), m.command);
        return;
    }
    if (m.command == STREAM_CMD_OVERLOAD || m.command == STREAM_CMD_OVERLOAD_SELECT || m.command == STREAM_CMD_OVERLOAD_END) {
        if (!routed && p && p->up && m.receiver_epoch == p->own_epoch && m.sender_epoch == p->other_epoch && !m.handle) {
            p->busy++; pressure_receive(p, &m); p->busy--; peer_sweep(p); stream_network_wake(net);
        } else DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream stale/unknown pressure request peer=%s", STREAM_ADDRESS_TEXT(sender));
        return;
    }
    int local_found = stream_local_get(net->inst, &m.descriptor, &local) == STREAM_OK;
    if (!local_found && !request) {
        if (response) {
            struct STREAM_RECEIVE *f = flow_find(p, &m, 1);
            if (f) flow_close(f, STREAM_REASON_STOPPED, 1);
            peer_sweep(p);
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream packet completed without local content command=%u", m.command); return;
    }
    if (routed && m.command == STREAM_CMD_REVERSE_REQ) {
        if (m.length || m.handle || m.receiver_epoch) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream REVERSE_REQ malformed peer=%s bytes=%zu handle=%llu receiver_epoch=%llx",
                       STREAM_ADDRESS_TEXT(sender), m.length, (unsigned long long)m.handle, (unsigned long long)m.receiver_epoch); return;
        }
        if (!authorized(net, sender, &m, &local)) return;
        if (!p) p = peer_open(net, sender);
        if (p) { p->idle = get_time_tb(); DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream REVERSE_REQ peer=%s attempt=%llx NCD=%p",
            STREAM_ADDRESS_TEXT(sender), (unsigned long long)m.sender_epoch, (void *)p->ncd); }
        return;
    }
    if (routed && m.command != STREAM_CMD_CLOSED) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream routed DATA/control forbidden command=%u", m.command); return;
    }
    if (m.command == STREAM_CMD_INFO_REQ && !routed) {
        if (m.handle || m.length != STREAM_INFO_REQUEST_SIZE || !stream_read_be(m.payload, 8)
            || m.payload[8] > 1) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream INFO_REQ malformed peer=%s bytes=%zu handle=%llu sub=%u",
                       STREAM_ADDRESS_TEXT(sender), m.length, (unsigned long long)m.handle, m.subblock); return;
        }
        if (!p) p = peer_open(net, sender);
        if (!p || (m.receiver_epoch && m.receiver_epoch != p->own_epoch)) return;
        if (p->other_epoch && p->other_epoch != m.sender_epoch) {
            peer_reset(p, STREAM_REASON_TRANSPORT, STREAM_RESET_KEEP_PENDING, NULL);
            /* Изменившийся sender уже отличает пару поколений; own не вращаем в ответ, чтобы не получить ping-pong. */
        }
        p->other_epoch = m.sender_epoch;
        p->busy++;
        struct ll_entry *reply = reply_reserve(p);
        if (!reply) { p->busy--; peer_sweep(p); return; }
        int reason = !allowed ? STREAM_REASON_MEMBER : !local_found ? STREAM_REASON_STOPPED
            : m.subblock >= local.descriptor.n ? STREAM_REASON_PROTOCOL : 0;
        if (!reason && !authorized(net, sender, &m, &local)) reason = STREAM_REASON_MEMBER;
        if (!reason && m.payload[8] && (stream_local_media_prepare(net->inst, &local.descriptor) != STREAM_OK
            || stream_local_get(net->inst, &m.descriptor, &local) != STREAM_OK)) reason = STREAM_REASON_STOPPED;
        uint8_t payload[STREAM_INFO_SIZE + STREAM_PATH_MAX * SC_PUBKEY_SIZE] = {0};
        memcpy(payload, m.payload, 8);
        uint64_t requested = stream_read_be(m.payload + 9, 8), contiguous = UINT64_MAX, actual;
        const uint8_t *part; size_t part_length;
        int present = !reason && requested != UINT64_MAX
            && stream_buffer_read(local.buffer, requested, m.subblock, &part, &part_length, &actual) == STREAM_OK;
        if (!reason) reason = info_payload(&local, m.subblock, present ? requested : UINT64_MAX, sender, stream_address_read(net->inst->my_ed25519_pubkey), payload);
        admission_report(p, &m, requested, "INFO", reason);
        if (present && !reason) {
            contiguous = requested;
            while (contiguous < UINT64_MAX - 1
                && stream_buffer_read(local.buffer, contiguous + 1, m.subblock, &part, &part_length, &actual) == STREAM_OK)
                contiguous++;
        }
        m.command = reason ? STREAM_CMD_REJECT : STREAM_CMD_INFO_RESP;
        if (reason) {
            uint8_t failure[11] = {(uint8_t)reason}; memcpy(failure + 1, m.payload, 8);
            stream_write_be(failure + 9, p->window_current, 2);
            reply_commit(p, reply, m, failure, sizeof(failure));
        } else {
            offer_payload(p, &local, m.subblock, payload);
            uint8_t *position = payload + STREAM_INFO_POSITION_OFF;
            stream_write_be(position, requested, 8); stream_write_be(position + 8, contiguous, 8); position[24] = present;
            reply_commit(p, reply, m, payload, STREAM_INFO_SIZE + payload[STREAM_INFO_PATH_OFF] * SC_PUBKEY_SIZE);
            if (!p->flows) {
                p->idle = get_time_tb();
                DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream INFO offer retains session peer=%s epoch=%llx wait_ms=%u",
                    STREAM_ADDRESS_TEXT(sender), (unsigned long long)p->own_epoch, (unsigned)(STREAM_LEASE_TB / 10));
            }
        }
        p->busy--; peer_sweep(p);
        return;
    }
    if (!p || (!p->up && !routed) || m.receiver_epoch != p->own_epoch
        || (p->other_epoch && m.sender_epoch != p->other_epoch && m.command != STREAM_CMD_INFO_RESP)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream stale/session peer=%s command=%u handle=%llu sender=%llx/%llx receiver=%llx/%llx",
                    STREAM_ADDRESS_TEXT(sender), m.command, (unsigned long long)m.handle,
                    (unsigned long long)m.sender_epoch, (unsigned long long)(p ? p->other_epoch : 0),
                    (unsigned long long)m.receiver_epoch, (unsigned long long)(p ? p->own_epoch : 0)); return;
    }
    p->busy++;
    struct STREAM_RECEIVE *f = flow_find(p, &m, m.command != STREAM_CMD_SUBSCRIBE && m.command != STREAM_CMD_SEEK && m.command != STREAM_CMD_RENEW
                                       && m.command != STREAM_CMD_UNSUBSCRIBE);
    if (m.command == STREAM_CMD_INFO_RESP && f && !m.handle && f->info_sent) {
        uint64_t start;
        if (m.length < 8 || stream_read_be(m.payload, 8) != f->probe) {
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream stale INFO probe peer=%s", STREAM_ADDRESS_TEXT(sender));
        } else if (parse_info(&m, &f->descriptor, sender, stream_address_read(net->inst->my_ed25519_pubkey), &f->info, &start) == STREAM_OK
                   && f->info.requested_seq == f->requested
                   && stream_metric_read(m.payload + STREAM_INFO_METRIC_OFF, &f->info.supplier_metrics, &f->info.supplier_metrics_valid) == STREAM_OK) {
            if (p->other_epoch && p->other_epoch != m.sender_epoch)
                peer_reset(p, STREAM_REASON_TRANSPORT, STREAM_RESET_KEEP_PENDING, f);
            p->other_epoch = m.sender_epoch; f->info_ready = f->info.info_ready = 1;
            if (net->info_order == UINT64_MAX) {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream INFO response order exhausted");
                flow_close(f, STREAM_REASON_LIMIT, 0);
            } else f->info.info_order = ++net->info_order;
            f->info.local_epoch = p->own_epoch; f->info.peer_epoch = p->other_epoch; peer_schedule(p);
            stream_local_head(net->inst, &f->descriptor, f->info.live_head_us);
            uint64_t rtt = get_time_tb() / 10 - f->probe_sent_ms;
            f->info.rtt_valid = f->probe_sent_ms != 0; f->info.rtt_ms = rtt > UINT32_MAX ? UINT32_MAX : (uint32_t)rtt;
            /* У автора все части идут по одному прямому пути; один INFO обновляет RTT всех его действующих частей. */
            int source_path = stream_address_equal(sender, f->descriptor.source) && f->info.path.count == 1;
            for (struct STREAM_RECEIVE *active = p->flows; active; active = active->next) {
                if (active == f || !active->outgoing || active->info.probe_only || active->info.state != STREAM_RECEIVE_ACTIVE
                    || (active->subblock != f->subblock && !source_path) || !same_stream(&active->descriptor, &f->descriptor)
                    || active->info.path.count != f->info.path.count
                    || memcmp(active->info.path.nodes, f->info.path.nodes, f->info.path.count * sizeof(struct stream_address))) continue;
                active->info.rtt_ms = f->info.rtt_ms; active->info.rtt_valid = f->info.rtt_valid;
                active->info.path_rtt_ms = f->info.path_rtt_ms; active->info.path_rtt_valid = f->info.path_rtt_valid;
                active->info.info_ms = f->info.info_ms;
                /* Все поля предложения относятся к одному INFO, иначе свежий RTT маскирует устаревшую очередь/PTS. */
                active->info.supplier_metrics = f->info.supplier_metrics; active->info.supplier_metrics_valid = f->info.supplier_metrics_valid;
                active->info.first_available = f->info.first_available; active->info.last_available = f->info.last_available;
                active->info.supplier_queue_bytes = f->info.supplier_queue_bytes; active->info.supplier_queue_age_ms = f->info.supplier_queue_age_ms;
                active->info.live_head_us = f->info.live_head_us; active->info.media_pts_us = f->info.media_pts_us;
                active->info.key_seq = f->info.key_seq; active->info.key_last_seq = f->info.key_last_seq;
                active->info.key_pts_us = f->info.key_pts_us; active->info.key_valid = f->info.key_valid;
                active->info.requested_seq = f->info.requested_seq; active->info.contiguous_last = f->info.contiguous_last;
                active->info.requested_present = f->info.requested_present;
                active->info.progress_age_ms = f->info.progress_age_ms; active->info.progress_valid = f->info.progress_valid;
                active->info.accepting = f->info.accepting; active->info.free_slots = f->info.free_slots;
                active->info.cpu_basis_points = f->info.cpu_basis_points; active->info.cpu_valid = f->info.cpu_valid;
            }
        } else flow_close(f, STREAM_REASON_PROTOCOL, 0);
    } else if (m.command == STREAM_CMD_SUBSCRIBE || m.command == STREAM_CMD_SEEK) {
        struct ll_entry *reply = reply_reserve(p);
        if (reply) {
            if (!allowed || !local_found || !authorized(net, sender, &m, &local)) {
                uint8_t reason = !local_found ? STREAM_REASON_STOPPED : STREAM_REASON_MEMBER;
                m.command = STREAM_CMD_REJECT; reply_commit(p, reply, m, &reason, 1);
            } else subscribe_request(p, m, &local, reply);
        }
    } else if (m.command == STREAM_CMD_ACCEPT && f) {
        uint64_t start;
        if (parse_info(&m, &f->descriptor, sender, stream_address_read(net->inst->my_ed25519_pubkey), &f->info, &start) != STREAM_OK
            || (f->requested != UINT64_MAX && start != f->requested)) flow_close(f, STREAM_REASON_PROTOCOL, 1);
        else if (f->info.state == STREAM_RECEIVE_REQUESTED) {
            f->seek_from = 0;
            stream_local_head(net->inst, &f->descriptor, f->info.live_head_us);
            f->info.start = f->info.next = start; f->info.state = STREAM_RECEIVE_ACTIVE; f->renewed = get_time_tb();
            f->info.active_ms = get_time_tb() / 10;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream receive ACCEPT peer=%s handle=%llu sub=%u start=%llu",
                       STREAM_ADDRESS_TEXT(sender), (unsigned long long)m.handle, m.subblock, (unsigned long long)start);
        }
    } else if (m.command == STREAM_CMD_TELEMETRY && f && f->info.state == STREAM_RECEIVE_ACTIVE) {
        struct stream_telemetry t; struct stream_path path = {0};
        int valid = stream_telemetry_read(m.payload, m.length, &t) == STREAM_OK;
        if (valid) {
            path.count = t.count;
            for (unsigned i = 0; i < t.count; i++) path.nodes[i] = t.rows[i].node;
            valid = stream_path_validate(&path, f->descriptor.source, sender, stream_address_read(net->inst->my_ed25519_pubkey)) == STREAM_OK;
        }
        if (valid && t.rows[t.count - 1].sample > f->telemetry_sample) {
            t.received_ms = get_time_tb() / 10;
            f->telemetry_sample = t.rows[t.count - 1].sample;
            if (stream_buffer_set_telemetry(local.buffer, f->subblock, &t) == STREAM_OK) {
                for (struct stream_peer *down = net->peers; down; down = down->next)
                    for (struct STREAM_RECEIVE *flow = down->flows; flow; flow = flow->next)
                        if (!flow->outgoing && flow->subblock == f->subblock && same_stream(&flow->descriptor, &f->descriptor)) {
                            flow->telemetry_dirty = 1; peer_schedule(down);
                        }
                DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "stream telemetry RX peer=%s sub=%u nodes=%u total_us=%llu",
                    STREAM_ADDRESS_TEXT(sender), f->subblock, t.count, (unsigned long long)stream_telemetry_total(&t));
            }
        } else if (!valid) DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream telemetry rejected peer=%s sub=%u",
                                      STREAM_ADDRESS_TEXT(sender), f->subblock);
    } else if (m.command == STREAM_CMD_DATA && f && f->info.state == STREAM_RECEIVE_ACTIVE) {
        struct stream_path path; size_t used;
        if (stream_path_parse(m.payload, m.length, &path, &used) != STREAM_OK
            || stream_path_validate(&path, f->descriptor.source, sender, stream_address_read(net->inst->my_ed25519_pubkey)) != STREAM_OK) {
            flow_close(f, STREAM_REASON_LOOP, 1);
        } else if (m.length - used <= STREAM_DATA_HEAD_SIZE + STREAM_FRAME_TIMING_SIZE + STREAM_BLOCK_OVERHEAD + RS_HEADER_SIZE
                   || m.length - used - STREAM_DATA_HEAD_SIZE - STREAM_FRAME_TIMING_SIZE > stream_block_capacity(&f->descriptor)) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream DATA part size invalid peer=%s bytes=%zu max=%zu",
                       STREAM_ADDRESS_TEXT(sender), m.length - used, stream_block_capacity(&f->descriptor));
            flow_close(f, STREAM_REASON_PROTOCOL, 1);
        } else {
            uint64_t head_us = stream_read_be(m.payload + used, STREAM_DATA_HEAD_SIZE); used += STREAM_DATA_HEAD_SIZE;
            if (head_us != UINT64_MAX && head_us > INT64_MAX) {
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream DATA live head invalid peer=%s pts=%llu",
                    STREAM_ADDRESS_TEXT(sender), (unsigned long long)head_us);
                flow_close(f, STREAM_REASON_PROTOCOL, 1); p->busy--; peer_sweep(p); return;
            }
            struct stream_frame_timing timing;
            if (stream_frame_timing_read(m.payload + used, m.length - used, &timing) != STREAM_OK) {
                flow_close(f, STREAM_REASON_PROTOCOL, 1); p->busy--; peer_sweep(p); return;
            }
            used += STREAM_FRAME_TIMING_SIZE;
            const uint8_t *block = m.payload + used; uint64_t seq = stream_read_be(block + STREAM_BLOCK_SEQ_OFF, 8);
            if (block[STREAM_BLOCK_SUB_OFF] != f->subblock || seq != f->info.next
                || memcmp(block + STREAM_BLOCK_GROUP_OFF, m.descriptor.group_key, SC_PUBKEY_SIZE)
                || memcmp(block + STREAM_BLOCK_SOURCE_OFF, f->descriptor.source.ed, SC_PUBKEY_SIZE)
                || stream_read_be(block + STREAM_BLOCK_EPOCH_OFF, 8) != f->descriptor.source_epoch) {
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream DATA mismatch peer=%s handle=%llu sub=%u/%u seq=%llu/%llu"
                    " group=%s/%s source=%s/%s epoch=%llx/%llx received/expected",
                    STREAM_ADDRESS_TEXT(sender), (unsigned long long)f->info.handle, block[STREAM_BLOCK_SUB_OFF], f->subblock,
                    (unsigned long long)seq, (unsigned long long)f->info.next,
                    sc_public_key_hex(block + STREAM_BLOCK_GROUP_OFF, (char[65]){0}),
                    sc_public_key_hex(m.descriptor.group_key, (char[65]){0}),
                    sc_public_key_hex(block + STREAM_BLOCK_SOURCE_OFF, (char[65]){0}), STREAM_ADDRESS_TEXT(f->descriptor.source),
                    (unsigned long long)stream_read_be(block + STREAM_BLOCK_EPOCH_OFF, 8), (unsigned long long)f->descriptor.source_epoch);
                flow_close(f, STREAM_REASON_PROTOCOL, 1);
            } else {
                int result = stream_ingest_path(net->inst, block, m.length - used, &path);
                if (result == STREAM_OK || result == STREAM_DUPLICATE || result == STREAM_EXPIRED) {
                    stream_local_head(net->inst, &f->descriptor, head_us);
                    if (result != STREAM_EXPIRED && stream_buffer_set_timing(local.buffer, seq, &timing) < 0)
                        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream received frame timing conflict peer=%s seq=%llu",
                                   STREAM_ADDRESS_TEXT(sender), (unsigned long long)seq);
                    stream_metrics_observe(&f->history->metrics, seq, (uint16_t)stream_read_be(block + STREAM_BLOCK_TIMESTAMP_OFF, 2),
                                           block[STREAM_BLOCK_FLAGS_OFF], (uint32_t)(m.length - used), get_time_tb() / 10);
                    if (f->info.path.count != path.count || memcmp(f->info.path.nodes, path.nodes, path.count * sizeof(struct stream_address)))
                        f->info.path_rtt_valid = 0;
                    f->info.next = seq + 1; f->info.blocks++; f->info.path = path; f->deadline = get_time_tb() + STREAM_LEASE_TB;
                    if (result != STREAM_EXPIRED && seq + 1 > f->info.data_next) {
                        f->info.data_next = seq + 1; f->info.progress_ms = get_time_tb() / 10;
                        if (seq == f->watch_seq) { f->info.recovery_ready = 1; f->info.recovery_ms = f->info.progress_ms; }
                        if (f->watch_seq != UINT64_MAX) stream_network_wake(net);
                    }
                    stream_stats_add(net->inst, &f->descriptor, ST_RX, m.length - used);
                    p->diagnostic.rx_blocks++; p->diagnostic.rx_bytes += m.length - used;
                    p->diagnostic.last_rx_ms = get_time_tb() / 10;
                    p->diagnostic.duplicates += result == STREAM_DUPLICATE;
                    p->diagnostic.expired += result == STREAM_EXPIRED;
                } else flow_close(f, STREAM_REASON_PROTOCOL, 1);
            }
        }
    } else if (m.command == STREAM_CMD_GAP && f && f->info.state == STREAM_RECEIVE_ACTIVE) {
        if (m.length != 16 || stream_read_be(m.payload, 8) != f->info.next
            || stream_read_be(m.payload + 8, 8) <= f->info.next) flow_close(f, STREAM_REASON_PROTOCOL, 1);
        else {
            uint64_t next = stream_read_be(m.payload + 8, 8);
            flow_gap_report(f, f->info.next, next, "supplier");
            f->info.next = next; f->info.gaps++;
        }
    } else if (m.command == STREAM_CMD_END && f && f->info.state == STREAM_RECEIVE_ACTIVE) {
        uint64_t last = m.length == STREAM_END_SIZE ? stream_read_be(m.payload + STREAM_END_LAST_OFF, 8) : UINT64_MAX;
        if (m.length == STREAM_END_SIZE && ((last != UINT64_MAX && f->info.next == last + 1)
            || (last == UINT64_MAX && !f->info.blocks && !f->info.next))
            && stream_ingest_end(net->inst, &f->descriptor, m.payload, m.length) == STREAM_OK) {
            f->info.state = STREAM_RECEIVE_ENDED; stream_metrics_end(&f->history->metrics);
        }
        else flow_close(f, STREAM_REASON_PROTOCOL, 1);
    } else if (m.command == STREAM_CMD_RENEW && f && !m.length) {
        f->deadline = get_time_tb() + STREAM_LEASE_TB;
        DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "stream lease renewed peer=%s handle=%llu deadline=%llu",
                    STREAM_ADDRESS_TEXT(sender), (unsigned long long)m.handle, (unsigned long long)f->deadline);
    }
    else if (m.command == STREAM_CMD_UNSUBSCRIBE && f && m.length == 1) flow_close(f, m.payload[0], 1);
    else if (m.command == STREAM_CMD_REJECT && f && !m.handle && m.length == 11 && stream_read_be(m.payload + 1, 8) == f->probe)
        flow_close(f, m.payload[0], 0);
    else if ((m.command == STREAM_CMD_CLOSED || m.command == STREAM_CMD_REJECT) && f && m.handle && (m.length == 1 || m.length == 17))
        flow_close(f, m.payload[0], 0);
    else DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream unknown/inactive issuance peer=%s command=%u handle=%llu sub=%u bytes=%zu",
                     STREAM_ADDRESS_TEXT(sender), m.command, (unsigned long long)m.handle, m.subblock, m.length);
    if (local_found) receive_progress(net, &local.descriptor);
    p->busy--; peer_sweep(p);
    if (m.command == STREAM_CMD_INFO_RESP || m.command == STREAM_CMD_ACCEPT || m.command == STREAM_CMD_REJECT
        || m.command == STREAM_CMD_CLOSED || (m.command == STREAM_CMD_DATA && !local.anchored)) stream_network_wake(net);
}

/* Entry/dgram принадлежат обработчику во всех ветках. */
static void receive_raw(struct ETCP_CONN *conn, struct ll_entry *entry) {
    struct stream_network *net = conn ? stream_network_get(conn->instance) : NULL;
    if (net && entry && entry->dgram) dispatch(net, stream_address_read(conn->peer_ed25519_pubkey), entry->dgram, entry->len, 0);
    if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
}

/* Router сообщает реального отправителя и признаки проверенной подписи, не next hop. */
static void receive_routed(struct ETCP_CONN *conn, struct ll_entry *entry) {
    struct stream_network *net = conn ? stream_network_get(conn->instance) : NULL;
    if (net && entry && entry->dgram && entry->len >= ROUTER_SVC_PAYLOAD_OFF + STREAM_WIRE_HEADER
        && (entry->dgram[ROUTER_SVC_FLAGS_OFF] & ROUTER_FLAG_SIGNED)) {
        uint64_t group;
        struct stream_address sender = stream_address_read(entry->dgram + ROUTER_SVC_SRC_ED_OFF);
        memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
        const uint8_t *wire = entry->dgram + ROUTER_SVC_PAYLOAD_OFF;
        const uint8_t* key = topo_group_wire_id(topo_groups_find(net->inst->topo_groups, group));
        if (key && !memcmp(wire + STREAM_WIRE_GROUP_OFF, key, SC_PUBKEY_SIZE))
            dispatch(net, sender, wire, entry->len - ROUTER_SVC_PAYLOAD_OFF, 1);
        else DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream routed group mismatch");
    } else if (net && entry && entry->len > ROUTER_SVC_PAYLOAD_OFF)
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream routed control unsigned/truncated bytes=%u", entry->len);
    if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
}

/* REINIT меняет stream generation при сохранении физического ETCP-соединения. */
static void transport_changed(struct ETCP_CONN *conn, int status, void *arg) {
    struct stream_network *net = arg;
    if (status != ETCP_CONN_STATUS_REINIT || net->stopping) return;
    struct stream_peer *p = peer_find(net, stream_address_read(conn->peer_ed25519_pubkey));
    if (p) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream transport REINIT peer=%s up=%d local_epoch=%llx peer_epoch=%llx control_queued=%d",
                    STREAM_ADDRESS_TEXT(p->address), p->up, (unsigned long long)p->own_epoch,
                    (unsigned long long)p->other_epoch, p->control->count);
        peer_reset(p, STREAM_REASON_TRANSPORT, STREAM_RESET_ROTATE | STREAM_RESET_KEEP_PENDING, NULL);
        if (p->up) peer_schedule(p);
    }
}

/* Возраст диагностирует задержку; обязательные ответы и закрытия не исчезают из-за срока FIFO. */
static void control_report_wait(struct stream_peer *p, uint64_t now) {
    if (now < p->diagnostic.wait_report_ms) return;
    uint64_t oldest = now; unsigned closing = 0;
    if (p->control->head) memcpy(&oldest, p->control->head->data, sizeof(oldest));
    for (struct stream_pending_request *r = p->requests; r; r = r->next) if (r->sent_ms < oldest) oldest = r->sent_ms;
    for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) if (f->close_pending || f->seek_from) {
        closing++; if (f->closing_ms < oldest) oldest = f->closing_ms;
    }
    if (now < oldest || now - oldest < STREAM_LOAD_MAX_AGE_MS) return;
    p->diagnostic.wait_report_ms = now + 5000;
    DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream control waits peer=%s age_ms=%llu requests=%u TX_window=%u replies=%u RX_window=%u"
        " closing=%u transport_up=%d obligations_retained=1",
        STREAM_ADDRESS_TEXT(p->address), (unsigned long long)(now - oldest), p->request_count, p->remote_window,
        p->reply_count, p->local_window, closing, p->up);
}

/* Неизвестное время не смешивается с достоверным нулём; все числовые длительности — мс. */
static void diagnostic_value(char *text, size_t capacity, uint64_t value, int valid) {
    if (valid) snprintf(text, capacity, "%llu", (unsigned long long)value);
    else snprintf(text, capacity, "na");
}

/* Один снимок на peer, не на выдачу: транспорт общий также с другими сервисами.
 * Очереди берём из ledger, без обхода кольца; счётчики интервала сохраняются при отключённом выводе только до следующей границы. */
static void peer_report(struct stream_peer *p, uint64_t now) {
    struct stream_peer_diagnostics *d = &p->diagnostic;
    struct ETCP_CONN *conn = node_conn_direct_get_conn(p->ncd);
    uint64_t elapsed = d->report_ms ? now - d->report_ms : 0;
    if (d->report_ms && elapsed < 500) return;
    int same_epoch = conn && d->reset_id == conn->reset_id && d->peer_reset_id == conn->peer_reset_id;
    uint64_t acked = same_epoch ? conn->diagnostic_acked_bytes - d->acked_bytes : 0;
    uint64_t retries = same_epoch ? conn->diagnostic_retries - d->retries : 0;
    uint64_t rtos = same_epoch ? conn->diagnostic_rtos - d->rtos : 0;
    unsigned tx = 0, rx = 0, connecting = 0, stopped = 0, flows = 0, probes = 0, closing = 0;
    uint64_t lag = 0, worst_group = 0, worst_epoch = 0;
    struct stream_address worst_source = {{0}};
    unsigned worst_sub = 0;
    int lag_valid = 0;
    int enabled = debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_VIDEO);
    if (enabled && d->report_ms) {
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            flows++; probes += f->info.probe_only;
            closing += f->info.state == STREAM_RECEIVE_CLOSED;
            if (f->info.probe_only || f->info.state >= STREAM_RECEIVE_ENDED) continue;
            if (f->info.state != STREAM_RECEIVE_ACTIVE) { connecting++; continue; }
            if (!f->outgoing) { tx++; continue; }
            rx++;
            if (!f->info.metrics_valid) continue;
            const struct stream_metric_snapshot *m = &f->info.metrics;
            stopped += m->stopped;
            if (!lag_valid || m->lag_ms > lag) {
                lag_valid = 1; lag = m->lag_ms; worst_group = f->descriptor.group_id;
                worst_source = f->descriptor.source; worst_epoch = f->descriptor.source_epoch; worst_sub = f->subblock;
            }
        }
        if (flows || d->tx_blocks || d->rx_blocks || d->tx_gaps || d->rx_gaps || d->dropped_blocks || d->control_packets) {
            unsigned udp = 0, tcp = 0, stalled = 0, full = 0, paced = 0, rtt_count = 0, jitter_count = 0;
            uint64_t flight = 0, window = 0, pacing = 0, rtt_min = UINT64_MAX, rtt_max = 0, jitter_max = 0, rto_max = 0;
            uint64_t send_errors = 0, recv_errors = 0, crypto_errors = 0;
            if (conn) for (struct ETCP_LINK *link = conn->links; link; link = link->next) {
                flight += link->inflight_bytes;
                send_errors += link->send_errors; recv_errors += link->recv_errors;
                crypto_errors += link->encrypt_errors + link->decrypt_errors;
                if (!link->initialized || !link->link_status) continue;
                if (link->is_tcp) tcp++; else udp++;
                stalled += link->tx_stalled;
                uint32_t need = conn->input_send_q && conn->input_send_q->head ? conn->input_send_q->head->len : 0;
                full += need && (need > link->inflight_lim_bytes || link->inflight_bytes > link->inflight_lim_bytes - need);
                paced += !link->burst_active && link->shaper_timer != NULL;
                window += link->inflight_lim_bytes; pacing += link->bbr_pacing_rate;
                uint64_t rtt = link->data_rtt_us;
                if (!rtt) rtt = (uint64_t)link->rtt_last * 100;
                uint64_t rto = etcp_tx_rto_us(link);
                if (rto > rto_max) rto_max = rto;
                if (link->data_rtt_us) {
                    jitter_count++;
                    if (link->data_jitter_us > jitter_max) jitter_max = link->data_jitter_us;
                }
                if (rtt) {
                    rtt_count++;
                    if (rtt < rtt_min) rtt_min = rtt;
                    if (rtt > rtt_max) rtt_max = rtt;
                }
            }
            char ack_idle[24], rx_idle[24], lag_text[24], rtt_text[48], jitter_text[24], rto_text[24], worst[80];
            char acked_text[24], retries_text[24], rtos_text[24];
            diagnostic_value(acked_text, sizeof(acked_text), acked, same_epoch);
            diagnostic_value(retries_text, sizeof(retries_text), retries, same_epoch);
            diagnostic_value(rtos_text, sizeof(rtos_text), rtos, same_epoch);
            uint64_t now_us = get_time_us();
            diagnostic_value(ack_idle, sizeof(ack_idle), conn && conn->diagnostic_ack_us
                ? (now_us - conn->diagnostic_ack_us) / 1000 : 0, conn && conn->diagnostic_ack_us);
            diagnostic_value(rx_idle, sizeof(rx_idle), now - d->last_rx_ms, d->last_rx_ms != 0);
            diagnostic_value(lag_text, sizeof(lag_text), lag, lag_valid);
            if (jitter_count) snprintf(jitter_text, sizeof(jitter_text), "%.1f", jitter_max / 1000.0);
            else snprintf(jitter_text, sizeof(jitter_text), "na");
            diagnostic_value(rto_text, sizeof(rto_text), rto_max / 1000, udp + tcp != 0);
            if (rtt_count) snprintf(rtt_text, sizeof(rtt_text), "%.1f..%.1f", rtt_min / 1000.0, rtt_max / 1000.0);
            else snprintf(rtt_text, sizeof(rtt_text), "na");
            if (lag_valid) snprintf(worst, sizeof(worst), "%llx/%s/%llx/%u", (unsigned long long)worst_group,
                STREAM_ADDRESS_TEXT(worst_source), (unsigned long long)worst_epoch, worst_sub);
            else snprintf(worst, sizeof(worst), "na");
            const char *blocked = !p->up ? "down" : !conn || !conn->initialized ? "session"
                : p->wait_q ? "etcp_queue"
                : conn->tx_state == ETCP_TX_STATE_LINK_WAIT ? "link_wait" : p->load.queued.packets ? "ready" : "head";
            DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO,
                "stream NET self=%s peer=%s dt_ms=%llu session=%llx/%llx changed=%d "
                "TX{flows=%u sent=%llu/%llu remain=%llu/%llu oldest_ms=%llu growth_Bps=%llu drop=%llu/%llu gap=%llu} "
                "ETCP{q_B=%llu unacked_B=%u flight_B=%llu window_B=%llu acked_B=%s ack_idle_ms=%s retry=%s rto=%s "
                "rx_reorder=%d/%zu errors_total=%llu/%llu/%llu} "
                "PATH{udp=%u tcp=%u stalled=%u full=%u paced=%u pacing_Bps=%llu rtt_ms=%s jitter_ms=%s rto_ms=%s blocked=%s} "
                "RX{flows=%u connecting=%u received=%llu/%llu idle_ms=%s lag_max_ms=%s stopped=%u worst=%s gap=%llu dup=%llu expired=%llu} "
                "CONTROL{sent=%llu/%llu INFO_req=%llu requests=%u TX_window=%u/%u replies=%u RX_window=%u/%u target=%u} "
                "BUDGET{peer_flows=%u closing=%u node_flows=%u peers=%u/%u probes=%u regular=%u/%u temporary=%u/%u background=%u/%u} "
                "overload=%d cooling=%d global_pressure=%d cpu_pct=%.1f/%d loop_late_ms=%llu counts=blocks/bytes errors=send/recv/crypto",
                sc_public_key_hex(p->network->inst->my_ed25519_pubkey, (char[65]){0}), STREAM_ADDRESS_TEXT(p->address), (unsigned long long)elapsed,
                (unsigned long long)(conn ? conn->reset_id : 0), (unsigned long long)(conn ? conn->peer_reset_id : 0), !same_epoch,
                tx, (unsigned long long)d->tx_blocks, (unsigned long long)d->tx_bytes,
                (unsigned long long)p->load.queued.packets, (unsigned long long)p->load.queued.bytes,
                (unsigned long long)p->load.snapshot.oldest_age_ms, (unsigned long long)p->load.snapshot.growth_rate,
                (unsigned long long)d->dropped_blocks, (unsigned long long)d->dropped_bytes,
                (unsigned long long)d->tx_gaps,
                (unsigned long long)(conn ? etcp_tx_queued(conn) : 0), conn ? conn->unacked_bytes : 0,
                (unsigned long long)flight, (unsigned long long)window, acked_text, ack_idle,
                retries_text, rtos_text, conn && conn->recv_q ? conn->recv_q->count : 0,
                conn && conn->recv_q ? conn->recv_q->total_bytes : 0,
                (unsigned long long)send_errors, (unsigned long long)recv_errors, (unsigned long long)crypto_errors,
                udp, tcp, stalled, full, paced, (unsigned long long)pacing, rtt_text, jitter_text, rto_text, blocked,
                rx, connecting, (unsigned long long)d->rx_blocks, (unsigned long long)d->rx_bytes, rx_idle, lag_text, stopped, worst,
                (unsigned long long)d->rx_gaps, (unsigned long long)d->duplicates, (unsigned long long)d->expired,
                (unsigned long long)d->control_packets, (unsigned long long)d->control_bytes, (unsigned long long)d->info_requests,
                p->request_count, p->remote_window, p->network->limits.control, p->reply_count,
                p->local_window, p->window_current, p->window_pending, flows, closing, p->network->flow_count,
                p->network->peer_count, p->network->limits.peers, probes, p->network->regular_used, p->network->limits.regular,
                p->network->temporary_used, p->network->limits.temporary, p->network->background_used, p->network->limits.background,
                p->pressure.active, p->cooling, p->network->global_pressure,
                p->network->cpu.average_percent, p->network->cpu.valid, (unsigned long long)p->network->callback_late_ms);
        }
    }
    d->report_ms = now; d->reset_id = conn ? conn->reset_id : 0; d->peer_reset_id = conn ? conn->peer_reset_id : 0;
    d->acked_bytes = conn ? conn->diagnostic_acked_bytes : 0;
    d->retries = conn ? conn->diagnostic_retries : 0; d->rtos = conn ? conn->diagnostic_rtos : 0;
    d->tx_blocks = d->tx_bytes = d->rx_blocks = d->rx_bytes = d->duplicates = d->expired = d->tx_gaps = d->rx_gaps = 0;
    d->dropped_blocks = d->dropped_bytes = 0;
    d->control_packets = d->control_bytes = d->info_requests = 0;
}

/* Один таймер обслуживает сроки/продления и ограниченные reverse-попытки; DATA не опрашивается таймером. */
static void network_tick(void *arg) {
    struct stream_network *net = arg; net->timer = NULL;
    uint64_t now = get_time_tb();
    uint64_t now_ms = now / 10;
    net->callback_late_ms = now_ms > net->next_tick_ms ? now_ms - net->next_tick_ms : 0;
    uasync_get_load(net->inst->ua, &net->cpu);
    struct stream_peer *p = net->peers;
    while (p) {
        struct stream_peer *next = p->next;
        p->busy++;
        control_report_wait(p, now_ms);
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            if (f->info.state == STREAM_RECEIVE_CLOSED) continue;
            if (!f->outgoing && f->info.state == STREAM_RECEIVE_ACTIVE
                && (!f->telemetry_sent_ms || now_ms - f->telemetry_sent_ms >= STREAM_TELEMETRY_PERIOD_MS)) peer_schedule(p);
            f->info.metrics_valid = stream_metrics_poll(&f->history->metrics, now_ms, &f->info.metrics) == STREAM_OK;
            int local_wait = p->up && f->outgoing && f->info.state < STREAM_RECEIVE_REQUESTED
                && (!f->info_started || (f->info_ready && !f->info.probe_only));
            if (!local_wait && !f->accept_pending
                && (!f->outgoing || (f->info.state < STREAM_RECEIVE_ACTIVE && !(f->info.probe_only && f->info.info_ready))
                || (f->outgoing && !f->info.probe_only && f->info.state == STREAM_RECEIVE_ACTIVE && !f->info.blocks
                    && f->info.last_available != UINT64_MAX))
                && now >= f->deadline)
                flow_close(f, f->outgoing ? STREAM_REASON_TRANSPORT : STREAM_REASON_LEASE, 1);
            else if (f->outgoing && f->info.state >= STREAM_RECEIVE_ACTIVE && now - f->renewed >= STREAM_RENEW_TB) {
                peer_schedule(p);
            }
        }
        struct STREAM_RECEIVE *pending = NULL;
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
            if (f->outgoing && f->info.state < STREAM_RECEIVE_ACTIVE && now < f->deadline) { pending = f; break; }
        if (!p->up && pending && (!p->reverse_at || now - p->reverse_at >= 10000)) {
            struct stream_message m = {.descriptor = pending->descriptor, .command = STREAM_CMD_REVERSE_REQ,
                                       .sender_epoch = p->own_epoch};
            if (route_control(net, p->address, m, NULL, 0, NULL) == STREAM_OK) p->reverse_at = now;
        }
        p->busy--; peer_sweep(p);
        if (p->up) peer_schedule(p);
        if (p->idle && now - p->idle >= STREAM_LEASE_TB && !p->control->count) peer_free(p);
        p = next;
    }
    stats_flush(net, now_ms);
    pressure_tick(net, now_ms);
    for (p = net->peers; p; p = p->next) peer_report(p, now_ms);
    stream_network_wake(net);
    net->next_tick_ms = get_time_tb() / 10 + STREAM_TICK_TB / 10;
    net->timer = uasync_set_timeout(net->inst->ua, STREAM_TICK_TB, net, network_tick, "stream_network");
    if (!net->timer) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream network timer scheduling failed");
}

/* Binding collision — ошибка запуска, другие обработчики не затираются. */
int stream_network_init(struct UTUN_INSTANCE *inst, struct stream_network **out) {
    if (!inst || !out || *out || inst->api_bindings.callbacks[ETCP_ID_STREAM] || inst->router_bindings.callbacks[ETCP_RT_ID_STREAM]) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream network invalid state/binding collision"); return STREAM_ERR_STATE;
    }
    struct stream_network *net = u_calloc(1, sizeof(*net));
    if (!net) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream network allocation failed"); return STREAM_ERR_MEMORY; }
    net->inst = inst; net->relay_epoch = new_epoch(); limits_refresh(net);
    if (!net->relay_epoch) { u_free(net); return STREAM_ERR_CRYPTO; }
    stream_load_init(&net->load, get_time_tb() / 10); net->next_tick_ms = get_time_tb() / 10 + STREAM_TICK_TB / 10;
    if (etcp_bind(inst, ETCP_ID_STREAM, receive_raw) < 0 || etcp_router_bind(inst, ETCP_RT_ID_STREAM, receive_routed) < 0
        || etcp_router_require_crypto(inst, ETCP_RT_ID_STREAM, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT)
        || etcp_router_set_authorize(inst, ETCP_RT_ID_STREAM, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream bindings registration failed"); stream_network_destroy(net); return STREAM_ERR_STATE;
    }
    etcp_add_conn_status_cbk(inst, transport_changed, net);
    int registered = 0;
    for (struct etcp_status_cbk_entry *e = inst->conn_status_cbks; e; e = e->next) if (e->fn == transport_changed && e->arg == net) registered = 1;
    net->timer = uasync_set_timeout(inst->ua, STREAM_TICK_TB, net, network_tick, "stream_network");
    if (!registered || !net->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream network callback/timer registration failed");
        stream_network_destroy(net); return STREAM_ERR_MEMORY;
    }
    *out = net;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream routed handler installed self=%s callback=%p net=%p",
                sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}), (void *)receive_routed, (void *)net);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream network initialized raw=%u routed=%u peers=%u flows=%u controls=%u lease=6000ms renew=2000ms",
               ETCP_ID_STREAM, ETCP_RT_ID_STREAM, net->limits.peers, net->limits.regular + net->limits.temporary, net->limits.control);
    return STREAM_OK;
}

void stream_network_destroy(struct stream_network *net) {
    if (!net) return;
    while (net->stats_pending) {
        struct stream_stats_pending *pending = net->stats_pending; net->stats_pending = pending->next; u_free(pending);
    }
    while (net->statistics) {
        struct stream_stats_entry *e = net->statistics; net->statistics = e->next; u_free(e);
    }
    net->stopping = 1; if (net->timer) uasync_cancel_timeout(net->inst->ua, net->timer);
    if (net->work) uasync_call_soon_cancel(net->inst->ua, net->work);
    while (net->candidates) {
        struct relay_candidates* read = net->candidates; net->candidates = read->next; read->network = NULL;
    }
    while (net->relays) {
        struct stream_relay *relay = net->relays; net->relays = relay->next;
        stream_close(relay->owner); u_free(relay);
    }
    while (net->relay_requests) {
        struct stream_relay_request *r = net->relay_requests; net->relay_requests = r->next;
        etcp_router_cancel_send_ready(&r->waiter); u_free(r);
    }
    etcp_remove_conn_status_cbk(net->inst, transport_changed, net);
    etcp_unbind(net->inst, ETCP_ID_STREAM); etcp_router_unbind(net->inst, ETCP_RT_ID_STREAM);
    while (net->peers) peer_free(net->peers);
    while (net->slots) {
        struct stream_receive_slot *slot = net->slots; net->slots = slot->next;
        if (!slot->reservation || slot->reservation->slots-- > slot->reservation->minimum) net->regular_used--;
        slot->network = NULL; slot->next = NULL; slot->reservation = NULL;
    }
    while (net->reservations) {
        struct stream_receive_reservation *r = net->reservations; net->reservations = r->next;
        net->regular_used -= r->minimum; r->network = NULL; r->next = NULL;
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream network stopped flows=%u regular=%u temporary=%u background=%u",
        net->flow_count, net->regular_used, net->temporary_used, net->background_used); u_free(net);
}

void stream_network_changed(struct stream_network *net) {
    if (net && !net->stopping) for (struct stream_peer *p = net->peers; p; p = p->next) peer_schedule(p);
}

/* Кольцо освобождается только после отмены сетевых курсоров данной точной эпохи. */
void stream_network_cancel(struct stream_network *net, const struct stream_descriptor *d) {
    if (net) {
        struct stream_stats_pending **link = &net->stats_pending;
        while (*link) {
            struct stream_stats_pending *pending = *link;
            if (same_stream(&pending->descriptor, d)) { *link = pending->next; u_free(pending); }
            else link = &pending->next;
        }
    }
    if (!net) return;
    for (struct relay_candidates* read = net->candidates; read; read = read->next)
        if (same_stream(&read->descriptor, d)) read->needed = 0;
    for (struct stream_relay_request *r = net->relay_requests; r; r = r->next)
        if (same_stream(&r->descriptor, d) && r->status < STREAM_RELAY_REJECTED) {
            r->status = STREAM_RELAY_CANCELLED; r->reason = STREAM_REASON_STOPPED; r->dirty = r->urgent = 1;
            r->retain_ms = get_time_tb() / 10 + STREAM_RELAY_RETAIN_MS;
        }
    for (struct stream_peer *p = net->peers; p; p = p->next) {
        p->busy++;
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
            if (same_stream(&f->descriptor, d)) flow_close(f, STREAM_REASON_STOPPED, 1);
        p->busy--; peer_sweep(p);
    }
    stream_network_wake(net);
}

/* Исключение поставщика/получателя или self закрывает выдачи, сохраняя локальное кольцо остальных трансляций. */
void stream_network_membership_changed(struct stream_network *net, const char *channel) {
    if (!net || net->stopping || !channel) return;
    for (struct stream_peer *p = net->peers; p; p = p->next) {
        p->busy++;
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            if (f->info.state == STREAM_RECEIVE_CLOSED) continue;
            struct TOPO_GROUP *group = topo_groups_find(net->inst->topo_groups, f->descriptor.group_id);
            if (!group || strcmp(group->channel_id, channel)) continue;
            struct stream_member_state state;
            int result = stream_member_load(net->inst, group->group_id, p->address, &state, NULL);
            if (result == STREAM_ERR_NOT_MEMBER || result == STREAM_ERR_SIGNATURE || result == STREAM_ERR_FORMAT ||
                !topo_group_chat_key_allowed(net->inst, group->group_id, p->address.ed))
                flow_close(f, STREAM_REASON_MEMBER, 1);
        }
        p->busy--; peer_sweep(p);
    }
}

int stream_receive_reserve(struct UTUN_INSTANCE *inst, const struct stream_descriptor *d, struct stream_receive_reservation **out) {
    struct stream_network *net = stream_network_get(inst);
    if (!net || !d || !out || *out || d->n <= d->r) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream minimum reservation invalid arguments/network"); return STREAM_ERR_ARGUMENT;
    }
    for (struct stream_receive_reservation *r = net->reservations; r; r = r->next) if (same_stream(&r->descriptor, d)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream minimum reservation already owned source=%s", STREAM_ADDRESS_TEXT(d->source));
        return STREAM_ERR_STATE;
    }
    limits_refresh(net);
    unsigned minimum = d->n - d->r;
    unsigned free = net->regular_used < net->limits.regular ? net->limits.regular - net->regular_used : 0;
    if (minimum > free) {
        net->wanted_regular = minimum; net->wanted_until_ms = get_time_tb() / 10 + 1500;
        if (get_time_tb() / 10 >= net->budget_report_ms) {
            net->budget_report_ms = get_time_tb() / 10 + 1000;
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream minimum reservation waits source=%s required=%u available=%u regular=%u/%u partial=0",
                STREAM_ADDRESS_TEXT(d->source), minimum, free, net->regular_used, net->limits.regular);
        }
        return STREAM_LOCAL_BUSY;
    }
    struct stream_receive_reservation *r = u_calloc(1, sizeof(*r));
    if (!r) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream minimum reservation allocation failed"); return STREAM_ERR_MEMORY; }
    r->network = net; r->descriptor = *d; r->minimum = minimum; r->next = net->reservations;
    net->reservations = r; net->regular_used += minimum; *out = r;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream minimum reserved source=%s required=%u regular=%u/%u",
        STREAM_ADDRESS_TEXT(d->source), minimum, net->regular_used, net->limits.regular);
    return STREAM_OK;
}

void stream_receive_unreserve(struct stream_receive_reservation *r) {
    if (!r) return;
    if (r->slots) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream minimum release with slots=%u", r->slots); return; }
    if (r->network) {
        struct stream_network *net = r->network;
        struct stream_receive_reservation **link = &net->reservations;
        while (*link && *link != r) link = &(*link)->next;
        if (!*link) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream minimum reservation not owned"); return; }
        *link = r->next; net->regular_used -= r->minimum;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream minimum released source=%s slots=%u regular=%u/%u",
            STREAM_ADDRESS_TEXT(r->descriptor.source), r->minimum, net->regular_used, net->limits.regular);
    }
    u_free(r);
}

int stream_receive_slot_open(struct STREAM_HANDLE *h, uint8_t sub, struct stream_receive_slot **out) {
    struct UTUN_INSTANCE *inst; struct stream_descriptor d;
    if (!out || *out || stream_handle_context(h, &inst, &d) != STREAM_OK || sub >= d.n) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream slot invalid subscription/sub=%u", sub); return STREAM_ERR_ARGUMENT;
    }
    struct stream_network *net = stream_network_get(inst);
    if (!net || net->stopping) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream slot network stopped"); return STREAM_ERR_STATE; }
    for (struct stream_receive_slot *s = net->slots; s; s = s->next) if (s->subblock == sub && same_stream(&s->descriptor, &d)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream slot already reserved source=%s sub=%u", STREAM_ADDRESS_TEXT(d.source), sub);
        return STREAM_ERR_STATE;
    }
    struct stream_receive_reservation *r = net->reservations;
    while (r && !same_stream(&r->descriptor, &d)) r = r->next;
    int extra = !r || r->slots >= r->minimum;
    if (extra) {
        limits_refresh(net);
        if (r && (r->slots >= r->minimum + net->limits.extra_receive || get_time_tb() / 10 < net->wanted_until_ms)) return STREAM_LOCAL_BUSY;
        int result = budget_check(net, STREAM_FLOW_REGULAR, 0); if (result != STREAM_OK) return result;
    }
    struct stream_receive_slot *slot = u_calloc(1, sizeof(*slot));
    if (!slot) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream slot allocation failed"); return STREAM_ERR_MEMORY; }
    slot->network = net; slot->descriptor = d; slot->subblock = sub; slot->reservation = r;
    slot->next = net->slots; net->slots = slot; net->regular_used += extra; if (r) r->slots++; *out = slot;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream slot reserved self=%s group=%llx source=%s epoch=%llx sub=%u regular=%u/%u",
        sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}), (unsigned long long)d.group_id, STREAM_ADDRESS_TEXT(d.source),
        (unsigned long long)d.source_epoch, sub, net->regular_used, net->limits.regular);
    return STREAM_OK;
}

void stream_receive_slot_close(struct stream_receive_slot *slot) {
    if (!slot) return;
    if (slot->primary || slot->replacement) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream slot close with DATA handles sub=%u", slot->subblock); return; }
    struct stream_network *net = slot->network;
    if (net) {
        struct stream_receive_slot **link = &net->slots;
        while (*link && *link != slot) link = &(*link)->next;
        if (!*link) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream slot not owned by network"); return; }
        *link = slot->next;
        if (!slot->reservation || slot->reservation->slots-- > slot->reservation->minimum) net->regular_used--;
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream slot released self=%s group=%llx source=%s sub=%u regular=%u/%u",
            sc_public_key_hex(net->inst->my_ed25519_pubkey, (char[65]){0}), (unsigned long long)slot->descriptor.group_id,
            STREAM_ADDRESS_TEXT(slot->descriptor.source), slot->subblock, net->regular_used, net->limits.regular);
    }
    u_free(slot);
}

int stream_receive_slot_surplus(struct stream_receive_slot *slot) {
    if (!slot || !slot->network || !slot->reservation || slot->reservation->slots <= slot->reservation->minimum) return 0;
    struct stream_network *net = slot->network; limits_refresh(net);
    unsigned free = net->regular_used < net->limits.regular ? net->limits.regular - net->regular_used : 0;
    int demand = get_time_tb() / 10 < net->wanted_until_ms && free < net->wanted_regular;
    if (!demand && net->regular_used <= net->limits.regular
        && slot->reservation->slots <= slot->reservation->minimum + net->limits.extra_receive) return 0;
    if (slot->replacement || (slot->primary && removal_cost(slot->primary->peer, slot->primary) >= 4)) return 0;
    for (struct stream_peer *p = net->peers; p; p = p->next)
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
            if (!f->outgoing && f->info.state != STREAM_RECEIVE_CLOSED && f->subblock == slot->subblock
                && same_stream(&f->descriptor, &slot->descriptor)) return 0;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream optional receive released source=%s sub=%u regular=%u/%u demand=%u preserve_k=%u",
        STREAM_ADDRESS_TEXT(slot->descriptor.source), slot->subblock, net->regular_used, net->limits.regular,
        demand ? net->wanted_regular : 0, slot->reservation->minimum);
    return 1;
}

static int receive_open(struct STREAM_HANDLE *subscription, struct stream_address node, uint8_t sub, uint64_t seq, int probe_only, int rescue,
                         struct stream_receive_slot *slot, struct STREAM_RECEIVE **out) {
    struct UTUN_INSTANCE *inst; struct stream_descriptor d;
    if (!out || *out || stream_handle_context(subscription, &inst, &d) != STREAM_OK || sub >= d.n || !stream_address_set(node) || stream_address_equal(node, stream_address_read(inst->my_ed25519_pubkey))) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream receive open invalid arguments peer=%s sub=%u", STREAM_ADDRESS_TEXT(node), sub);
        return STREAM_ERR_ARGUMENT;
    }
    struct stream_network *net = stream_network_get(inst);
    struct stream_local local;
    if (stream_local_get(inst, &d, &local) != STREAM_OK || local.waiting) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream receive waits for source route group=%llx source=%s peer=%s",
            (unsigned long long)d.group_id, STREAM_ADDRESS_TEXT(d.source), STREAM_ADDRESS_TEXT(node));
        return STREAM_WAIT;
    }
    if (slot && (slot->network != net || slot->subblock != sub || !same_stream(&slot->descriptor, &d))) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream receive slot does not match subscription"); return STREAM_ERR_ARGUMENT;
    }
    if (slot && slot->replacement) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream slot overlap already pending sub=%u", sub); return STREAM_LOCAL_BUSY; }
    struct stream_member_state member;
    int online = net && stream_member_online(inst, d.group_id, node);
    int loaded = online ? stream_member_load(inst, d.group_id, node, &member, NULL) : STREAM_WAIT;
    if (loaded != STREAM_OK || (!stream_member_has_subscription(&member, d.source, d.source_epoch) && !stream_relay_confirmed(inst, &d, node))) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream supplier unavailable group=%llx source=%s epoch=%llx peer=%s online=%d profile_rc=%d",
            (unsigned long long)d.group_id, STREAM_ADDRESS_TEXT(d.source), (unsigned long long)d.source_epoch,
            STREAM_ADDRESS_TEXT(node), online, loaded);
        return STREAM_WAIT;
    }
    enum stream_flow_budget budget = probe_only ? (rescue ? STREAM_FLOW_RESCUE : STREAM_FLOW_BACKGROUND)
        : slot ? (slot->primary ? STREAM_FLOW_BACKGROUND : STREAM_FLOW_RESERVED) : STREAM_FLOW_REGULAR;
    int result = budget_check(net, budget, 0); if (result != STREAM_OK) return result;
    if (!peer_find(net, node) && net->peer_count >= net->limits.peers) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream local peer budget waits self=%s peer=%s limit=%u supplier_penalty=0",
            sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}), STREAM_ADDRESS_TEXT(node), net->limits.peers); return STREAM_LOCAL_BUSY;
    }
    struct stream_peer *p = peer_open(net, node);
    if (!p) return STREAM_ERR_STATE;
    for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
        if (f->outgoing && f->info.state != STREAM_RECEIVE_CLOSED && same_stream(&f->descriptor, &d) && f->subblock == sub
            && (f->info.probe_only == probe_only || !f->info.handle)) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream receive already open peer=%s sub=%u", STREAM_ADDRESS_TEXT(node), sub);
            return STREAM_ERR_STATE;
        }
    int bootstrap = 1;
    for (struct stream_peer *other = net->peers; other; other = other->next)
        for (struct STREAM_RECEIVE *f = other->flows; f; f = f->next)
            if (f->outgoing && !f->info.probe_only && f->info.state != STREAM_RECEIVE_CLOSED && same_stream(&f->descriptor, &d)) bootstrap = 0;
    struct STREAM_RECEIVE *f = NULL;
    result = flow_new(p, &d, sub, 1, budget, &f); if (result != STREAM_OK) return result;
    if (slot) { f->slot = slot; if (slot->primary) slot->replacement = f; else slot->primary = f; }
    f->owned = 1; f->requested = f->info.start = seq; f->bootstrap = bootstrap; f->info.probe_only = probe_only; *out = f; peer_schedule(p);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream receive opened peer=%s group=%llx source=%s epoch=%llx sub=%u index=%llu"
               " bootstrap=%d probe_only=%d transport_up=%d timeout_ms=%u",
               STREAM_ADDRESS_TEXT(node), (unsigned long long)d.group_id, STREAM_ADDRESS_TEXT(d.source),
               (unsigned long long)d.source_epoch, sub, (unsigned long long)seq, bootstrap, probe_only, p->up, STREAM_CONNECT_TB / 10);
    return STREAM_OK;
}

int stream_receive_open(struct STREAM_HANDLE *subscription, struct stream_address node, uint8_t sub, uint64_t seq, struct STREAM_RECEIVE **out) {
    return receive_open(subscription, node, sub, seq, 0, 0, NULL, out);
}

int stream_receive_reserved_open(struct STREAM_HANDLE *subscription, struct stream_receive_slot *slot,
                                 struct stream_address node, uint64_t seq, struct STREAM_RECEIVE **out) {
    if (!slot) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream reserved receive requires slot"); return STREAM_ERR_ARGUMENT; }
    return receive_open(subscription, node, slot->subblock, seq, 0, 0, slot, out);
}

int stream_probe_open(struct STREAM_HANDLE *subscription, struct stream_address node, uint8_t sub, uint64_t seq, struct STREAM_RECEIVE **out) {
    return receive_open(subscription, node, sub, seq, 1, 0, NULL, out);
}

int stream_probe_open_at(struct STREAM_HANDLE *subscription, struct stream_address node, uint8_t sub, uint64_t seq, struct STREAM_RECEIVE **out) {
    if (seq == UINT64_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream exact probe requires position"); return STREAM_ERR_ARGUMENT; }
    int result = receive_open(subscription, node, sub, seq, 1, 1, NULL, out);
    if (result == STREAM_OK) (*out)->deadline = UINT64_MAX; // Срок точной пробы контролирует её владелец с учётом RTT.
    return result;
}

int stream_probe_subscribe(struct STREAM_RECEIVE *f, uint64_t seq, struct stream_receive_slot *slot) {
    if (!f || !f->peer || !f->info.probe_only || !f->info_ready || f->info.state != STREAM_RECEIVE_INFO || !slot
        || slot->network != f->peer->network || slot->subblock != f->subblock || !same_stream(&slot->descriptor, &f->descriptor)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream probe subscribe requires ready live probe"); return STREAM_ERR_STATE;
    }
    if (slot->replacement) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream probe overlap already pending sub=%u", f->subblock); return STREAM_LOCAL_BUSY; }
    if (!f->info.requested_present || seq < f->info.requested_seq || seq > f->info.contiguous_last
        || !f->info.accepting || !f->info.free_slots) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream probe subscribe position/admission changed peer=%s seq=%llu",
                    STREAM_ADDRESS_TEXT(f->info.peer), (unsigned long long)seq); return STREAM_WAIT;
    }
    for (struct STREAM_RECEIVE *other = f->peer->flows; other; other = other->next)
        if (other != f && other->outgoing && !other->info.probe_only && other->info.state != STREAM_RECEIVE_CLOSED
            && other->subblock == f->subblock && same_stream(&other->descriptor, &f->descriptor)) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream probe subscribe duplicate DATA peer=%s sub=%u",
                       STREAM_ADDRESS_TEXT(f->info.peer), f->subblock); return STREAM_ERR_STATE;
        }
    f->slot = slot;
    if (slot->primary) slot->replacement = f;
    else { slot->primary = f; budget_put(slot->network, f->budget); f->budget = STREAM_FLOW_RESERVED; }
    f->info.probe_only = 0; f->requested = f->info.start = seq; f->deadline = UINT64_MAX;
    stream_receive_watch(f, seq); peer_schedule(f->peer);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream exact probe becomes DATA peer=%s sub=%u seq=%llu without_new_INFO=1",
               STREAM_ADDRESS_TEXT(f->info.peer), f->subblock, (unsigned long long)seq);
    return STREAM_OK;
}

void stream_receive_watch(struct STREAM_RECEIVE *f, uint64_t seq) {
    if (!f) return;
    f->watch_seq = seq; f->info.recovery_ready = 0; f->info.recovery_ms = 0;
}

/* Новое поколение cursor в прежнем слоте: старые DATA не совпадут с handle даже до прихода ACCEPT. */
int stream_receive_seek(struct STREAM_RECEIVE *f, uint64_t seq) {
    if (!f || !f->peer || !f->outgoing || f->info.probe_only || f->info.state != STREAM_RECEIVE_ACTIVE
        || f->seek_pending || !f->peer->up || seq == UINT64_MAX || seq <= f->info.start) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream SEEK requires active forward cursor seq=%llu", (unsigned long long)seq);
        return STREAM_ERR_STATE;
    }
    struct stream_peer *p = f->peer;
    struct stream_local local;
    if (stream_local_get(p->network->inst, &f->descriptor, &local) != STREAM_OK || (local.anchored && seq < local.next)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream SEEK cannot rewind the consumer seq=%llu", (unsigned long long)seq); return STREAM_ERR_STATE;
    }
    if (p->next_handle == UINT64_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream SEEK handle space exhausted"); return STREAM_ERR_STATE; }
    f->seek_target = seq; f->seek_pending = 1; peer_schedule(p);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream SEEK pending peer=%s sub=%u old_handle=%llu index=%llu slots_unchanged=1",
        STREAM_ADDRESS_TEXT(p->address), f->subblock, (unsigned long long)f->info.handle, (unsigned long long)seq);
    return STREAM_OK;
}

int stream_probe_next(struct STREAM_RECEIVE *f, uint8_t sub, uint64_t seq) {
    if (!f || !f->peer || !f->info.probe_only || !f->info.info_ready || sub >= f->descriptor.n) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream probe next invalid state/sub=%u", sub); return STREAM_ERR_STATE;
    }
    for (struct STREAM_RECEIVE *other = f->peer->flows; other; other = other->next)
        if (other != f && other->outgoing && !other->info.handle && other->subblock == sub
            && same_stream(&other->descriptor, &f->descriptor)) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream probe next conflicts with pending INFO sub=%u", sub); return STREAM_WAIT;
        }
    f->subblock = f->info.subblock = sub; f->info_started = f->info_sent = f->info_ready = f->info.info_ready = 0;
    f->requested = f->info.start = seq;
    f->probe = f->probe_sent_ms = 0; f->deadline = get_time_tb() + STREAM_CONNECT_TB;
    f->info.state = STREAM_RECEIVE_CONNECTING; peer_schedule(f->peer); return STREAM_OK;
}

void stream_receive_close(struct STREAM_RECEIVE *f) {
    if (!f) return;
    f->owned = 0;
    if (!f->peer) { u_free(f); return; }
    struct stream_peer *p = f->peer;
    flow_close(f, STREAM_REASON_STOPPED, 1); peer_sweep(p);
}

/* Смена стартовой попытки инвалидирует только upstream этой трансляции; номера новых SUBSCRIBE продолжают возрастать. */
int stream_receive_restart(struct STREAM_HANDLE *subscription) {
    struct UTUN_INSTANCE *inst; struct stream_descriptor d;
    if (stream_handle_context(subscription, &inst, &d) != STREAM_OK) return STREAM_ERR_STATE;
    struct stream_network *net = stream_network_get(inst);
    if (!net) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream restart without network"); return STREAM_ERR_STATE; }
    for (struct stream_peer *p = net->peers; p; p = p->next) {
        p->busy++;
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
            if (f->outgoing && same_stream(&f->descriptor, &d)) flow_close(f, STREAM_REASON_STOPPED, 1);
        p->busy--; peer_sweep(p);
    }
    return stream_local_restart(inst, &d, UINT64_MAX);
}

int stream_receive_get_info(struct STREAM_RECEIVE *f, struct stream_receive_info *info) {
    if (!f || !info) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream receive info invalid arguments"); return STREAM_ERR_ARGUMENT; }
    *info = f->info;
    if (f->history) info->metrics_valid = stream_metrics_poll(&f->history->metrics, get_time_tb() / 10, &info->metrics) == STREAM_OK;
    if (f->peer) { info->overload_id = f->peer->requested.id; info->overload_outcome = f->peer->requested.outcome; }
    if (f->peer && f->peer->requested.active) for (unsigned i = 0; i < f->peer->requested.count; i++)
        if (f->peer->requested.cuts[i].handle == f->info.handle) {
            info->supplier_overloaded = 1;
            info->relocate_requested = f->peer->requested.cuts[i].relocate;
            info->overload_id = f->peer->requested.id;
            if (f->peer->requested.cuts[i].selected) info->close_requested = 1;
        }
    return STREAM_OK;
}

/* Терминальные записи остаются на минуту: повтор REQUEST/CANCEL не создаёт новое владение. */
static struct stream_relay_request *relay_request_find(struct stream_network *net, const struct stream_descriptor *key,
                                                       struct stream_address peer, uint64_t id, int outgoing) {
    for (struct stream_relay_request *r = net->relay_requests; r; r = r->next)
        if (r->outgoing == outgoing && stream_address_equal(r->peer, peer) && same_stream(&r->descriptor, key) && (!id || r->id == id)) return r;
    return NULL;
}

/* Лимит включает tombstones и проверяется до выделения; исчерпание не вытесняет защиту от повторов. */
static struct stream_relay_request *relay_request_new(struct stream_network *net, const struct stream_descriptor *key,
                                                      struct stream_address peer, uint64_t id, uint64_t epoch, int outgoing) {
    if (net->relay_request_count >= STREAM_RELAY_REQUEST_LIMIT) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay request limit peer=%s count=%u", STREAM_ADDRESS_TEXT(peer),
                   net->relay_request_count); return NULL;
    }
    struct stream_relay_request *r = u_calloc(1, sizeof(*r));
    if (!r) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "relay request allocation failed"); return NULL; }
    r->descriptor = *key; r->peer = peer; r->id = id; r->epoch = epoch; r->outgoing = outgoing;
    r->network = net;
    r->status = STREAM_RELAY_STARTING; r->dirty = r->urgent = 1;
    r->deadline_ms = get_time_tb() / 10 + STREAM_RELAY_START_MS;
    r->next = net->relay_requests; net->relay_requests = r; net->relay_request_count++;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "relay request opened group=%llx source=%s epoch=%llx peer=%s id=%llx out=%d deadline=%llu",
               (unsigned long long)key->group_id, STREAM_ADDRESS_TEXT(key->source), (unsigned long long)key->source_epoch,
               STREAM_ADDRESS_TEXT(peer), (unsigned long long)id, outgoing, (unsigned long long)r->deadline_ms);
    return r;
}

/* Downstream удерживает приём только в ACTIVE; выбранный upstream удерживает запрос также до ACCEPT, но INFO-пробы — нет. */
static unsigned relay_flows(struct stream_network *net, const struct stream_descriptor *key, struct stream_address peer, int outgoing, int pending) {
    unsigned count = 0;
    for (struct stream_peer *p = net->peers; p; p = p->next) {
        if (stream_address_set(peer) && !stream_address_equal(p->address, peer)) continue;
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
            if (f->outgoing == outgoing && !f->info.probe_only
                && (f->info.state == STREAM_RECEIVE_ACTIVE || (pending && f->info.state < STREAM_RECEIVE_ACTIVE))
                && same_stream(&f->descriptor, key)) count++;
    }
    return count;
}

/* Неуспешная помощь закрывает только выдачи своего пира и трансляции, не общий ETCP. */
static void relay_finish(struct stream_network *net, struct stream_relay_request *r, int status, int reason, uint64_t now) {
    if (r->status >= STREAM_RELAY_REJECTED) return;
    etcp_router_cancel_send_ready(&r->waiter);
    memset(&r->waiter, 0, sizeof(r->waiter)); r->blocked = 0;
    r->status = status; r->reason = reason; r->retain_ms = now + STREAM_RELAY_RETAIN_MS; r->dirty = r->urgent = 1;
    if (r->outgoing) {
        struct stream_peer *p = peer_find(net, r->peer);
        if (p) {
            p->busy++;
            for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
                if (f->outgoing && same_stream(&f->descriptor, &r->descriptor)) flow_close(f, reason, 1);
            p->busy--; peer_sweep(p);
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "relay request finished source=%s epoch=%llx peer=%s id=%llx out=%d status=%d reason=%d",
               STREAM_ADDRESS_TEXT(r->descriptor.source), (unsigned long long)r->descriptor.source_epoch,
               STREAM_ADDRESS_TEXT(r->peer), (unsigned long long)r->id, r->outgoing, status, reason);
}

/* Одна подписка/кольцо на точную эпоху, независимо от числа ожидающих и действующих получателей. */
static int relay_open(struct stream_network *net, const struct stream_descriptor *key) {
    for (struct stream_relay *relay = net->relays; relay; relay = relay->next)
        if (same_stream(&relay->descriptor, key)) return stream_handle_active(relay->owner) ? STREAM_OK : STREAM_ERR_STATE;
    if (net->relay_count >= STREAM_RELAY_LIMIT) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay stream limit count=%u limit=%u", net->relay_count, STREAM_RELAY_LIMIT);
        return STREAM_ERR_STATE;
    }
    struct stream_relay *relay = u_calloc(1, sizeof(*relay));
    if (!relay) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "relay context allocation failed"); return STREAM_ERR_MEMORY; }
    int result = stream_relay_open(net->inst, key, &relay->owner);
    if (result != STREAM_OK) { u_free(relay); return result; }
    relay->descriptor = *key; relay->next = net->relays; net->relays = relay; net->relay_count++;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "relay receiving started group=%llx source=%s epoch=%llx streams=%u limit=%u",
               (unsigned long long)key->group_id, STREAM_ADDRESS_TEXT(key->source),
               (unsigned long long)key->source_epoch, net->relay_count, STREAM_RELAY_LIMIT);
    return STREAM_OK;
}

/* READY ускоряет поиск только в подтверждённом запросе; membership и DATA-подпись остаются обязательными. */
int stream_relay_confirmed(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct stream_address peer) {
    struct stream_network *net = stream_network_get(inst);
    if (!net || net->stopping) return 0;
    struct stream_relay_request *r = relay_request_find(net, key, peer, 0, 1);
    return r && (r->status == STREAM_RELAY_BOUND
        || (r->status == STREAM_RELAY_READY && get_time_tb() / 10 < r->deadline_ms));
}

/* SQL-приоритет сохраняется, живые права/маршрут проверяются заново после READ. */
static int relay_candidates_read(sqlite3* db, void* arg) {
    struct relay_candidates* read = arg;
    return -topo_node_sqlite_get_connect_peers(db, read->channel, &read->peers, &read->count);
}
static void relay_candidates_free(void* arg) {
    struct relay_candidates* read = arg; u_free(read->peers); u_free(read);
}
static void relay_candidates_done(void* arg, const struct db_async_result* result) {
    struct relay_candidates* read = arg; struct stream_network* net = read->network;
    if (!net) return;
    struct relay_candidates** link = &net->candidates; while (*link != read) link = &(*link)->next;
    *link = read->next; read->network = NULL;
    if (result->status) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "relay candidates READ failed group=%llx status=%d rc=%d: %s",
            (unsigned long long)read->descriptor.group_id, result->status, result->sqlite_code, result->message); return;
    }
    struct UTUN_INSTANCE* inst = net->inst; const struct stream_descriptor* key = &read->descriptor;
    struct stream_local local;
    if (net->stopping || !read->needed || stream_local_get(inst, key, &local) != STREAM_OK
        || local.relay_only || local.finished || local.waiting) return;
    for (struct stream_relay_request* r = net->relay_requests; r; r = r->next)
        if (r->outgoing && same_stream(&r->descriptor, key) && r->status < STREAM_RELAY_REJECTED) return;
    struct stream_address chosen = {{0}};
    for (size_t i = 0; i < read->count; i++) {
        struct stream_address peer = stream_address_read(read->peers[i].ed);
        if (stream_address_equal(peer, stream_address_read(inst->my_ed25519_pubkey)) || stream_address_equal(peer, key->source) || stream_member_supernode(inst, key->group_id, peer) != 1
            || !stream_member_online(inst, key->group_id, peer) || !node_conn_direct_compatible_key(inst, peer.ed)) continue;
        struct stream_relay_request *previous = relay_request_find(net, key, peer, 0, 1);
        if (previous && (previous->status < STREAM_RELAY_REJECTED || previous->status == STREAM_RELAY_REJECTED
                         || previous->reason != STREAM_REASON_STOPPED)) continue;
        struct stream_member_state member;
        if (stream_member_load(inst, key->group_id, peer, &member, NULL) != STREAM_OK) continue;
        chosen = peer; break;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "relay candidates loaded group=%llx rows=%zu chosen=%s",
        (unsigned long long)key->group_id, read->count, STREAM_ADDRESS_TEXT(chosen));
    if (!stream_address_set(chosen)) return;
    uint64_t id = new_epoch(), epoch = new_epoch();
    if (!id || !epoch) return;
    uint64_t now = get_time_tb() / 10;
    struct stream_relay_request *r = relay_request_new(net, key, chosen, id, epoch, 1);
    if (r && !peer_open(net, chosen)) relay_finish(net, r, STREAM_RELAY_CANCELLED, STREAM_REASON_TRANSPORT, get_time_tb() / 10);
}

/* Один незавершённый READ на эпоху трансляции; отсутствие demand отменяет только применение результата. */
void stream_relay_demand(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, int needed, uint64_t now) {
    struct stream_network *net = stream_network_get(inst); struct stream_local local;
    if (!net || net->stopping || stream_local_get(inst, key, &local) != STREAM_OK) return;
    struct relay_candidates* pending = NULL; unsigned reads = 0;
    for (struct relay_candidates* read = net->candidates; read; read = read->next) {
        reads++; if (same_stream(&read->descriptor, key)) { read->needed = needed; pending = read; }
    }
    for (struct stream_relay_request *r = net->relay_requests; r; r = r->next) {
        if (!r->outgoing || !same_stream(&r->descriptor, key) || r->status >= STREAM_RELAY_REJECTED) continue;
        unsigned in_use = relay_flows(net, key, r->peer, 1, 1);
        int drained = local.finished && (local.last == UINT64_MAX || (local.anchored && local.next > local.last));
        if (needed && r->status == STREAM_RELAY_BOUND && now - r->progress_ms >= STREAM_RELAY_START_MS)
            relay_finish(net, r, STREAM_RELAY_CANCELLED, STREAM_REASON_TRANSPORT, now);
        else if (local.relay_only || drained || (!needed && !in_use))
            relay_finish(net, r, STREAM_RELAY_CANCELLED, STREAM_REASON_STOPPED, now);
        else return;
    }
    if (!needed || local.relay_only || local.finished || local.waiting || pending) return;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find(inst->topo_groups, key->group_id) : NULL;
    if (!group || group->stopping) return;
    if (!chat_channel_valid(group->channel_id)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "relay candidate namespace invalid"); return; }
    if (reads >= STREAM_CANDIDATE_READ_LIMIT) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay candidate READ limit=%u", reads); return; }
    struct relay_candidates* read = u_calloc(1, sizeof(*read));
    if (!read) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "relay candidate request allocation failed"); return; }
    memcpy(read->channel, group->channel_id, strlen(group->channel_id) + 1);
    read->network = net; read->descriptor = *key; read->needed = 1; read->next = net->candidates; net->candidates = read;
    if (inst->db_worker) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="stream-relay-candidates", .mode=DB_ASYNC_READ,
            .work=relay_candidates_read, .data=read, .done=relay_candidates_done, .cleanup=relay_candidates_free}); return;
    }
    sqlite3* ram = inst->topo_sqlite_db; const char* file = ram ? sqlite3_db_filename(ram, "main") : NULL;
    struct db_async_result result = {.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_ABORT};
    snprintf(result.message, sizeof(result.message), "no asynchronous relay candidate reader");
    if (file && !*file) {
        result.sqlite_code = relay_candidates_read(ram, read); result.status = result.sqlite_code ? DB_ASYNC_SQL : DB_ASYNC_OK;
        snprintf(result.message, sizeof(result.message), "RAM relay candidate read: %s", sqlite3_errstr(result.sqlite_code));
    }
    relay_candidates_done(read, &result); relay_candidates_free(read);
}

/* Только end-to-end signed router-вход; команды до local_get авторизуются отдельно по канальному членству. */
static void relay_dispatch(struct stream_network *net, struct stream_address sender, const struct stream_message *m) {
    uint64_t now = get_time_tb() / 10;
    if (!m->handle || m->subblock || (m->command != STREAM_CMD_RELAY_STATUS && m->length)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay malformed control peer=%s command=%u bytes=%zu handle=%llx",
                   STREAM_ADDRESS_TEXT(sender), m->command, m->length, (unsigned long long)m->handle); return;
    }
    struct stream_member_state member;
    if (stream_member_load(net->inst, m->descriptor.group_id, sender, &member, NULL) != STREAM_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay control unauthorized peer=%s group=%llx", STREAM_ADDRESS_TEXT(sender),
                   (unsigned long long)m->descriptor.group_id); return;
    }
    if (m->command == STREAM_CMD_RELAY_STATUS) {
        struct stream_relay_request *r = relay_request_find(net, &m->descriptor, sender, m->handle, 1);
        if (!r || r->status >= STREAM_RELAY_REJECTED || m->receiver_epoch != r->epoch
            || (r->server_epoch && r->server_epoch != m->sender_epoch)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "relay stale status peer=%s id=%llx", STREAM_ADDRESS_TEXT(sender),
                        (unsigned long long)m->handle); return;
        }
        if (m->length < 6 || m->payload[0] < STREAM_RELAY_STARTING || m->payload[0] > STREAM_RELAY_CANCELLED
            || m->length != (size_t)(6 + (m->payload[0] == STREAM_RELAY_ENDED ? STREAM_END_SIZE : 0))) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay invalid status peer=%s bytes=%zu", STREAM_ADDRESS_TEXT(sender), m->length); return;
        }
        if (stream_member_supernode(net->inst, m->descriptor.group_id, sender) != 1) {
            relay_finish(net, r, STREAM_RELAY_CANCELLED, STREAM_REASON_MEMBER, now); return;
        }
        if (!r->server_epoch) {
            r->server_epoch = m->sender_epoch; r->dirty = r->urgent = 1;
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "relay server generation confirmed peer=%s id=%llx generation=%llx",
                        STREAM_ADDRESS_TEXT(sender), (unsigned long long)r->id, (unsigned long long)r->server_epoch);
        }
        unsigned status = m->payload[0]; uint64_t remaining = stream_read_be(m->payload + 2, 4);
        if (status == STREAM_RELAY_READY || status == STREAM_RELAY_BOUND) {
            if (!remaining && status == STREAM_RELAY_READY) return;
            if (remaining > STREAM_RELAY_HANDOFF_MS) remaining = STREAM_RELAY_HANDOFF_MS;
            if (r->status == STREAM_RELAY_STARTING) {
                r->status = STREAM_RELAY_READY; r->deadline_ms = now + remaining;
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "relay ready source=%s peer=%s id=%llx handoff_ms=%llu",
                           STREAM_ADDRESS_TEXT(r->descriptor.source), STREAM_ADDRESS_TEXT(sender),
                           (unsigned long long)r->id, (unsigned long long)remaining);
                stream_relay_ready(net->inst, &r->descriptor, sender);
            }
        } else if (status == STREAM_RELAY_ENDED) {
            if (stream_read_be(m->payload + 6 + STREAM_END_LAST_OFF, 8) != UINT64_MAX
                || stream_ingest_end(net->inst, &r->descriptor, m->payload + 6, STREAM_END_SIZE) != STREAM_OK) {
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay empty END verification failed peer=%s", STREAM_ADDRESS_TEXT(sender));
                relay_finish(net, r, STREAM_RELAY_CANCELLED, STREAM_REASON_PROTOCOL, now);
            } else relay_finish(net, r, STREAM_RELAY_CANCELLED, STREAM_REASON_STOPPED, now);
        } else if (status >= STREAM_RELAY_REJECTED) {
            relay_finish(net, r, STREAM_RELAY_REJECTED, m->payload[1], now);
        }
        return;
    }
    if (m->receiver_epoch && m->receiver_epoch != net->relay_epoch) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "relay stale server generation peer=%s", STREAM_ADDRESS_TEXT(sender)); return;
    }
    struct stream_relay_request *r = relay_request_find(net, &m->descriptor, sender, m->handle, 0);
    if (r && r->epoch != m->sender_epoch) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay request nonce conflicts peer=%s id=%llx", STREAM_ADDRESS_TEXT(sender),
                   (unsigned long long)m->handle); return;
    }
    if (!r && m->command == STREAM_CMD_RELAY_REQUEST && m->receiver_epoch) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "relay expired request confirmation ignored peer=%s id=%llx",
                    STREAM_ADDRESS_TEXT(sender), (unsigned long long)m->handle); return;
    }
    if (!r) r = relay_request_new(net, &m->descriptor, sender, m->handle, m->sender_epoch, 0);
    if (!r) return;
    if (m->command == STREAM_CMD_RELAY_CANCEL) relay_finish(net, r, STREAM_RELAY_CANCELLED, STREAM_REASON_STOPPED, now);
    else {
        r->dirty = 1;
        if (r->status >= STREAM_RELAY_REJECTED) return;
        if ((!r->granted && !stream_member_online(net->inst, m->descriptor.group_id, sender))
            || stream_member_supernode(net->inst, m->descriptor.group_id, stream_address_read(net->inst->my_ed25519_pubkey)) != 1) {
            relay_finish(net, r, STREAM_RELAY_REJECTED, STREAM_REASON_MEMBER, now); return;
        }
        struct stream_local local;
        if (stream_local_get(net->inst, &m->descriptor, &local) == STREAM_OK && local.waiting) {
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "relay waits for source route group=%llx source=%s peer=%s",
                (unsigned long long)m->descriptor.group_id, STREAM_ADDRESS_TEXT(m->descriptor.source), STREAM_ADDRESS_TEXT(sender));
            relay_finish(net, r, STREAM_RELAY_REJECTED, STREAM_REASON_STOPPED, now); return;
        }
        struct stream_peer *peer = peer_find(net, sender);
        int pressure = net->global_pressure || (peer && (peer->pressure.active || peer->cooling));
        int slots_full = net->regular_used >= net->limits.regular;
        /* Повтор уже принятой помощи не создаёт новую нагрузку и не должен рвать работающий downstream. */
        if (!r->granted && (pressure || slots_full)) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay admission refused self=%s group=%llx source=%s epoch=%llx peer=%s"
                " CPU_overload=%d peer_pressure=%d peer_cooling=%d regular=%u/%u",
                sc_public_key_hex(net->inst->my_ed25519_pubkey, (char[65]){0}), (unsigned long long)m->descriptor.group_id,
                STREAM_ADDRESS_TEXT(m->descriptor.source), (unsigned long long)m->descriptor.source_epoch,
                STREAM_ADDRESS_TEXT(sender), net->global_pressure, peer ? peer->pressure.active : 0, peer ? peer->cooling : 0,
                net->regular_used, net->limits.regular);
            relay_finish(net, r, STREAM_RELAY_REJECTED, pressure ? STREAM_REASON_OVERLOAD : STREAM_REASON_LIMIT, now); return;
        }
        for (size_t i = 0; i < member.count; i++)
            if (stream_address_equal(member.subscriptions[i].source, m->descriptor.source)
                && member.subscriptions[i].source_epoch == m->descriptor.source_epoch && member.subscriptions[i].relay_only) {
                relay_finish(net, r, STREAM_RELAY_REJECTED, STREAM_REASON_LOOP, now); return;
            }
        /* Первый REQUEST только подтверждает поколение сервиса. Старый пакет после tombstone не запускает приём. */
        if (!m->receiver_epoch) return;
        if (!peer_open(net, sender)) { relay_finish(net, r, STREAM_RELAY_REJECTED, STREAM_REASON_TRANSPORT, now); return; }
        int result = relay_open(net, &m->descriptor);
        if (result == STREAM_OK && !r->granted) { r->granted = r->urgent = 1; }
        if (result != STREAM_OK && result != STREAM_WAIT)
            relay_finish(net, r, STREAM_RELAY_REJECTED, result == STREAM_ERR_STATE ? STREAM_REASON_STOPPED : STREAM_REASON_LIMIT, now);
    }
}

/* Один network tick обслуживает bootstrap, передачу владения выдачам, отмену и остановку пустых ретрансляторов. */
static void relay_tick(struct stream_network *net, uint64_t now) {
    for (struct stream_relay_request *r = net->relay_requests; r; r = r->next) {
        if (r->status < STREAM_RELAY_REJECTED) {
            if (now >= r->checked_ms) {
                r->checked_ms = now + 1000;
                struct stream_member_state member;
                int valid = stream_member_load(net->inst, r->descriptor.group_id, r->peer, &member, NULL) == STREAM_OK
                    && stream_member_supernode(net->inst, r->descriptor.group_id, r->outgoing ? r->peer : stream_address_read(net->inst->my_ed25519_pubkey)) == 1;
                if (!valid) relay_finish(net, r, r->outgoing ? STREAM_RELAY_CANCELLED : STREAM_RELAY_REJECTED, STREAM_REASON_MEMBER, now);
            }
            unsigned active = relay_flows(net, &r->descriptor, r->peer, r->outgoing, 0);
            if (r->status < STREAM_RELAY_REJECTED && active) {
                if (r->status != STREAM_RELAY_BOUND) {
                    r->status = STREAM_RELAY_BOUND; r->dirty = r->urgent = 1; r->progress_ms = now;
                    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "relay request bound to issuance source=%s peer=%s out=%d flows=%u",
                               STREAM_ADDRESS_TEXT(r->descriptor.source), STREAM_ADDRESS_TEXT(r->peer), r->outgoing, active);
                }
                struct stream_local local;
                if (r->outgoing && stream_local_get(net->inst, &r->descriptor, &local) == STREAM_OK
                    && r->progress_seq != local.next) { r->progress_seq = local.next; r->progress_ms = now; }
            } else if (r->status == STREAM_RELAY_BOUND) {
                relay_finish(net, r, STREAM_RELAY_CANCELLED, STREAM_REASON_STOPPED, now);
            } else if (r->status < STREAM_RELAY_REJECTED && now >= r->deadline_ms) {
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay attempt expired source=%s peer=%s id=%llx status=%d",
                           STREAM_ADDRESS_TEXT(r->descriptor.source), STREAM_ADDRESS_TEXT(r->peer),
                           (unsigned long long)r->id, r->status);
                relay_finish(net, r, r->outgoing ? STREAM_RELAY_CANCELLED : STREAM_RELAY_REJECTED, STREAM_REASON_TRANSPORT, now);
            }
            if (!r->outgoing && r->granted && r->status == STREAM_RELAY_STARTING) {
                struct stream_local local;
                if (stream_local_get(net->inst, &r->descriptor, &local) == STREAM_OK) {
                    if (local.finished && local.last == UINT64_MAX) {
                        memcpy(r->end, local.end, STREAM_END_SIZE);
                        relay_finish(net, r, STREAM_RELAY_ENDED, STREAM_REASON_STOPPED, now);
                    } else if (local.decoded) {
                        uint8_t info[STREAM_INFO_SIZE + STREAM_PATH_MAX * SC_PUBKEY_SIZE] = {0};
                        int available = 0;
                        for (unsigned sub = 0; sub < local.descriptor.n && !available; sub++)
                            available = info_payload(&local, (uint8_t)sub, UINT64_MAX, r->peer, stream_address_read(net->inst->my_ed25519_pubkey), info) == 0;
                        if (available) {
                            r->status = STREAM_RELAY_READY; r->deadline_ms = now + STREAM_RELAY_HANDOFF_MS;
                            r->dirty = r->urgent = 1;
                            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "relay ready after FEC source=%s peer=%s id=%llx start=%llu k=%u",
                                STREAM_ADDRESS_TEXT(r->descriptor.source), STREAM_ADDRESS_TEXT(r->peer),
                                (unsigned long long)r->id, (unsigned long long)local.start,
                                (unsigned)(local.descriptor.n - local.descriptor.r));
                        }
                    }
                }
            }
        }
        if (r->blocked || (!r->dirty && !(r->outgoing && r->status <= STREAM_RELAY_READY && now - r->sent_ms >= 1000))) continue;
        if (!r->urgent && r->sent_ms && now - r->sent_ms < 1000) continue;
        struct stream_message m = {.descriptor = r->descriptor, .handle = r->id,
            .sender_epoch = r->outgoing ? r->epoch : net->relay_epoch,
            .receiver_epoch = r->outgoing ? r->server_epoch : r->epoch};
        uint8_t payload[6 + STREAM_END_SIZE]; size_t length = 0;
        if (r->outgoing) m.command = r->status >= STREAM_RELAY_REJECTED ? STREAM_CMD_RELAY_CANCEL : STREAM_CMD_RELAY_REQUEST;
        else {
            m.command = STREAM_CMD_RELAY_STATUS; payload[0] = (uint8_t)r->status; payload[1] = (uint8_t)r->reason;
            uint64_t remaining = r->deadline_ms > now ? r->deadline_ms - now : 0;
            stream_write_be(payload + 2, remaining, 4); length = 6;
            if (r->status == STREAM_RELAY_ENDED) { memcpy(payload + 6, r->end, STREAM_END_SIZE); length += STREAM_END_SIZE; }
        }
        r->sent_ms = now; r->urgent = 0;
        if (route_control(net, r->peer, m, payload, length, r) == STREAM_OK) r->dirty = 0;
    }
    struct stream_relay **link = &net->relays;
    while (*link) {
        struct stream_relay *relay = *link;
        unsigned pending = 0;
        for (struct stream_relay_request *r = net->relay_requests; r; r = r->next)
            if (!r->outgoing && same_stream(&r->descriptor, &relay->descriptor) && r->status <= STREAM_RELAY_READY) pending++;
        unsigned active = relay_flows(net, &relay->descriptor, (struct stream_address){{0}}, 0, 0);
        if (now >= relay->checked_ms) {
            relay->checked_ms = now + 1000;
            relay->allowed = stream_member_supernode(net->inst, relay->descriptor.group_id, stream_address_read(net->inst->my_ed25519_pubkey)) == 1;
        }
        int valid = stream_handle_active(relay->owner) && relay->allowed;
        if (valid && (pending || active)) { relay->idle_ms = 0; link = &relay->next; continue; }
        if (valid && !relay->idle_ms) relay->idle_ms = now;
        if (valid && now - relay->idle_ms < STREAM_RELAY_IDLE_MS) { link = &relay->next; continue; }
        *link = relay->next; net->relay_count--;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "relay receiving stopped source=%s epoch=%llx pending=%u downstream=%u valid=%d idle_ms=%llu",
                   STREAM_ADDRESS_TEXT(relay->descriptor.source), (unsigned long long)relay->descriptor.source_epoch,
                   pending, active, valid, (unsigned long long)(now - relay->idle_ms));
        stream_close(relay->owner); u_free(relay);
    }
    struct stream_relay_request **request_link = &net->relay_requests;
    while (*request_link) {
        struct stream_relay_request *r = *request_link;
        if (r->status < STREAM_RELAY_REJECTED || now < r->retain_ms) { request_link = &r->next; continue; }
        *request_link = r->next; net->relay_request_count--;
        etcp_router_cancel_send_ready(&r->waiter); u_free(r);
    }
}

/* Добавление/вытеснение учитывает только ещё не переданные блоки действующей downstream-выдачи. */
void stream_network_cache_event(struct stream_network *net, const struct stream_descriptor *d, const uint8_t *wire, size_t length, int added) {
    if (!net || net->stopping) return;
    uint64_t seq = stream_read_be(wire + STREAM_BLOCK_SEQ_OFF, 8); uint8_t sub = wire[STREAM_BLOCK_SUB_OFF];
    for (struct stream_peer *p = net->peers; p; p = p->next) {
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            if (f->outgoing || f->info.state != STREAM_RECEIVE_ACTIVE || f->subblock != sub || seq < f->info.next
                || !same_stream(&f->descriptor, d)) continue;
            load_account(f, 1, length, added, 0);
            if (added) {
                if (seq > f->pending_highest) f->pending_highest = seq;
                stream_metrics_observe(&f->history->offered_metrics, seq, (uint16_t)stream_read_be(wire + STREAM_BLOCK_TIMESTAMP_OFF, 2),
                                       wire[STREAM_BLOCK_FLAGS_OFF], (uint32_t)length, get_time_tb() / 10);
            }
        }
    }
}

/* Нужна скорость входящего вклада, а не урезанный D. При отсутствии source-дельты используем измеренные arrivals. */
static uint64_t flow_rate(struct STREAM_RECEIVE *f, uint64_t now) {
    struct stream_metric_snapshot metric = {0};
    stream_metrics_poll(&f->history->offered_metrics, now, &metric);
    uint64_t rate = metric.source_rate_valid && !metric.stopped ? metric.source_rate_bytes_sec : f->history->load.snapshot.input_rate;
    return rate ? rate : 1;
}

static struct STREAM_RECEIVE *flow_by_handle(struct stream_peer *p, uint64_t handle, int outgoing) {
    for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
        if (f->outgoing == outgoing && !f->accept_pending && f->info.handle == handle && f->info.state == STREAM_RECEIVE_ACTIVE) return f;
    return NULL;
}

/* Ограниченная порция обхода handles; после неё сортируем rates. Неудачная порция не зацикливает разгрузку на тех же64 выдачах. */
static int pressure_begin(struct stream_peer *p, uint64_t required, unsigned required_count, uint64_t now) {
    if (p->pressure.active || p->pressure.end_pending || p->cooling) return 0;
    struct stream_pressure_request request = {0}; uint64_t total = 0; unsigned eligible = 0;
    unsigned maximum = STREAM_OVERLOAD_MAX;
    for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
        if (f->outgoing || f->accept_pending || f->info.state != STREAM_RECEIVE_ACTIVE) continue;
        struct stream_cut cut = {.handle = f->info.handle, .rate = flow_rate(f, now)};
        eligible++;
        unsigned at = 0;
        while (at < request.count) {
            uint64_t current = request.cuts[at].handle;
            int after = cut.handle > p->pressure_cursor, current_after = current > p->pressure_cursor;
            if (after > current_after || (after == current_after && cut.handle < current)) break;
            at++;
        }
        if (at == maximum) continue;
        if (request.count < maximum) request.count++;
        for (unsigned i = request.count - 1; i > at; i--) request.cuts[i] = request.cuts[i - 1];
        request.cuts[at] = cut;
    }
    if (!request.count) return 0;
    if (p->pressure_id == UINT64_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "stream OVERLOAD id exhausted"); return 0; }
    p->pressure_cursor = request.cuts[request.count - 1].handle;
    request.descriptor = flow_by_handle(p, request.cuts[0].handle, 0)->descriptor;
    for (unsigned i = 1; i < request.count; i++) {
        struct stream_cut cut = request.cuts[i]; unsigned at = i;
        while (at && request.cuts[at - 1].rate < cut.rate) { request.cuts[at] = request.cuts[at - 1]; at--; }
        request.cuts[at] = cut;
    }
    for (unsigned i = 0; i < request.count; i++) total += request.cuts[i].rate;
    request.required_rate = required_count ? 0 : required && required < total ? required : total;
    request.required_count = required_count < request.count ? required_count : request.count;
    uint64_t picked = 0;
    while (request.required_count < request.count && picked < request.required_rate) picked += request.cuts[request.required_count++].rate;
    request.id = ++p->pressure_id; request.deadline_ms = now + STREAM_LOAD_GRACE_MS; request.active = 1;
    p->pressure = request; p->pressure_retry_ms = 0; p->clear_since_ms = 0;
    DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "stream OVERLOAD begin peer=%s id=%llu count=%u cut=%llu g=%llu A=%llu D=%llu X=%llu Q=%llu age=%llu"
               " global=%d candidates=%u/%u cursor=%llu reason=%s",
               STREAM_ADDRESS_TEXT(p->address), (unsigned long long)request.id, request.required_count,
               (unsigned long long)request.required_rate, (unsigned long long)p->load.snapshot.growth_rate,
               (unsigned long long)p->load.arrivals.bytes, (unsigned long long)p->load.sent.bytes,
               (unsigned long long)p->load.removed.bytes, (unsigned long long)p->load.queued.bytes,
               (unsigned long long)p->load.snapshot.oldest_age_ms, p->network->global_pressure,
               request.count, eligible, (unsigned long long)p->pressure_cursor, required_count ? "slot_budget" : "queue_growth");
    return 1;
}

/* Повтор запроса не меняет rate/срок. Header несёт один активный ключ, а список handles относится ко всем группам пира. */
static void pressure_send(struct stream_peer *p, uint64_t now) {
    if (p->pressure_retry_ms && now - p->pressure_retry_ms < 500) return;
    if (p->pressure.end_pending) {
        uint8_t payload[9]; stream_write_be(payload, p->pressure.id, 8); payload[8] = (uint8_t)p->pressure.end_pending;
        struct stream_message m = {.descriptor = p->pressure.descriptor, .command = STREAM_CMD_OVERLOAD_END};
        p->pressure_retry_ms = now;
        if (control_put(p, m, payload, sizeof(payload)) == STREAM_OK) {
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream OVERLOAD END sent peer=%s id=%llu outcome=%u",
                STREAM_ADDRESS_TEXT(p->address), (unsigned long long)p->pressure.id, payload[8]);
            p->pressure.end_pending = 0;
        }
        return;
    }
    if (!p->pressure.active) return;
    struct STREAM_RECEIVE *first = NULL;
    for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
        if (!f->outgoing && f->info.state == STREAM_RECEIVE_ACTIVE) { first = f; break; }
    if (!first) return;
    uint8_t payload[STREAM_OVERLOAD_HEADER + STREAM_OVERLOAD_MAX * 16];
    stream_write_be(payload, p->pressure.id, 8); stream_write_be(payload + 8, p->pressure.required_rate, 8);
    payload[16] = (uint8_t)p->pressure.required_count; payload[17] = (uint8_t)p->pressure.count;
    for (unsigned i = 0; i < p->pressure.count; i++) {
        stream_write_be(payload + STREAM_OVERLOAD_HEADER + i * 16, p->pressure.cuts[i].handle, 8);
        stream_write_be(payload + STREAM_OVERLOAD_HEADER + i * 16 + 8, p->pressure.cuts[i].rate, 8);
    }
    struct stream_message m = {.descriptor = first->descriptor, .command = STREAM_CMD_OVERLOAD};
    if (control_put(p, m, payload, STREAM_OVERLOAD_HEADER + p->pressure.count * 16) == STREAM_OK) p->pressure_retry_ms = now;
}

/* Закрывать можно только избыток над k реально работающих частей, включая выбор других перегруженных пиров. */
static int removal_cost(struct stream_peer *p, struct STREAM_RECEIVE *candidate) {
    uint8_t seen[RS_MAX_BLOCKS] = {0}; unsigned distinct = 0;
    for (struct stream_peer *other = p->network->peers; other; other = other->next) {
        for (struct STREAM_RECEIVE *f = other->flows; f; f = f->next) {
            if (!f->outgoing || f == candidate || f->info.state != STREAM_RECEIVE_ACTIVE
                || !f->info.blocks || !f->info.metrics_valid || f->info.metrics.stopped
                || (f->slot && f->slot->replacement == f)
                || !same_stream(&f->descriptor, &candidate->descriptor)) continue;
            int selected = 0;
            if (other->requested.active) for (unsigned i = 0; i < other->requested.count; i++)
                if (other->requested.cuts[i].selected && other->requested.cuts[i].handle == f->info.handle) selected = 1;
            if (!selected && !seen[f->subblock]) { seen[f->subblock] = 1; distinct++; }
        }
    }
    int parity = candidate->subblock >= candidate->descriptor.n - candidate->descriptor.r;
    return (distinct >= (unsigned)(candidate->descriptor.n - candidate->descriptor.r) ? 0 : 4) + (parity ? 0 : 1);
}

/* Выбор предлагает только безопасные закрытия. Клиент сохраняет handles до CLOSED; поздний SELECT игнорируется отправителем. */
static void pressure_select(struct stream_peer *p, uint64_t now) {
    struct stream_pressure_request *r = &p->requested;
    if (!r->active || now >= r->deadline_ms) return;
    uint64_t reduction = 0; unsigned selected = 0, covered = 0;
    for (unsigned i = 0; i < r->count; i++) {
        if (r->cuts[i].selected || r->cuts[i].closed) { reduction += r->cuts[i].rate; covered++; }
        if (r->cuts[i].selected) selected++;
    }
    while (reduction < r->required_rate || covered < r->required_count) {
        int best = -1, best_cost = 8;
        for (unsigned i = 0; i < r->count; i++) {
            struct STREAM_RECEIVE *f = flow_by_handle(p, r->cuts[i].handle, 1);
            if (r->cuts[i].selected || r->cuts[i].closed || !f) continue;
            int cost = removal_cost(p, f);
            if (cost >= 4) continue;
            if (cost < best_cost || (cost == best_cost && (best < 0 || r->cuts[i].rate > r->cuts[best].rate))) {
                best = (int)i; best_cost = cost;
            }
        }
        if (best < 0) break;
        r->cuts[best].selected = 1; reduction += r->cuts[best].rate; selected++; covered++;
    }
    uint64_t planned = reduction; unsigned planned_count = covered;
    for (unsigned i = 0; i < r->count; i++) {
        struct stream_cut *cut = &r->cuts[i];
        if (cut->selected || cut->closed || (planned >= r->required_rate && planned_count >= r->required_count)) cut->relocate = 0;
        else if (cut->relocate) { planned += cut->rate; planned_count++; }
    }
    while (planned < r->required_rate || planned_count < r->required_count) {
        int best = -1;
        for (unsigned i = 0; i < r->count; i++) {
            struct stream_cut *cut = &r->cuts[i];
            if (cut->selected || cut->closed || cut->relocate || !flow_by_handle(p, cut->handle, 1)) continue;
            if (best < 0 || cut->rate > r->cuts[best].rate) best = (int)i;
        }
        if (best < 0) break;
        r->cuts[best].relocate = 1; planned += r->cuts[best].rate; planned_count++;
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream OVERLOAD migration needed peer=%s id=%llu handle=%llu rate=%llu"
            " requested=%llu safe_or_closed=%llu planned=%llu slots=%u/%u",
            STREAM_ADDRESS_TEXT(p->address), (unsigned long long)r->id, (unsigned long long)r->cuts[best].handle,
            (unsigned long long)r->cuts[best].rate, (unsigned long long)r->required_rate,
            (unsigned long long)reduction, (unsigned long long)planned, planned_count, r->required_count);
    }
    if (r->reported_selection != selected + 1 && debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_VIDEO)) {
        /* Один снимок при получении запроса; после него — только при изменении выбранного набора. */
        r->reported_selection = selected + 1;
        for (unsigned i = 0; i < r->count; i++) {
            struct STREAM_RECEIVE *f = flow_by_handle(p, r->cuts[i].handle, 1);
            if (!f) continue;
            DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "stream OVERLOAD candidate self=%s peer=%s id=%llu group=%llx source=%s epoch=%llx"
                " sub=%u handle=%llu selected=%d required_parts=%u reason=%s migration=%d",
                sc_public_key_hex(p->network->inst->my_ed25519_pubkey, (char[65]){0}), STREAM_ADDRESS_TEXT(p->address), (unsigned long long)r->id,
                (unsigned long long)f->descriptor.group_id, STREAM_ADDRESS_TEXT(f->descriptor.source),
                (unsigned long long)f->descriptor.source_epoch, f->subblock, (unsigned long long)f->info.handle,
                r->cuts[i].selected, f->descriptor.n - f->descriptor.r,
                r->cuts[i].selected ? "safe_surplus" : removal_cost(p, f) >= 4 ? "essential" : "surplus_not_needed",
                r->cuts[i].relocate);
        }
    }
    if (!selected || (r->sent && now - r->select_ms < 500)) return;
    uint8_t payload[9 + STREAM_OVERLOAD_MAX * 8]; stream_write_be(payload, r->id, 8); payload[8] = (uint8_t)selected;
    unsigned at = 0;
    for (unsigned i = 0; i < r->count; i++) if (r->cuts[i].selected) {
        stream_write_be(payload + 9 + at++ * 8, r->cuts[i].handle, 8);
    }
    struct stream_message m = {.descriptor = r->descriptor, .command = STREAM_CMD_OVERLOAD_SELECT};
    if (control_put(p, m, payload, 9 + at * 8) == STREAM_OK) {
        r->sent = 1; r->select_ms = now;
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream OVERLOAD selected self=%s peer=%s id=%llu handles=%u rate=%llu requested=%llu"
            " shortfall=%llu delivered=0",
            sc_public_key_hex(p->network->inst->my_ed25519_pubkey, (char[65]){0}), STREAM_ADDRESS_TEXT(p->address), (unsigned long long)r->id, at,
            (unsigned long long)reduction, (unsigned long long)r->required_rate,
            (unsigned long long)(reduction < r->required_rate ? r->required_rate - reduction : 0));
    }
}

/* Проверяем весь список до действия: повторы handles и несуществующие элементы не создают частичных закрытий. */
static void pressure_receive(struct stream_peer *p, const struct stream_message *m) {
    if (!pressure_members_allowed(p, m, 1)) return;
    limits_refresh(p->network);
    uint64_t now = get_time_tb() / 10;
    if (m->command == STREAM_CMD_OVERLOAD_END) {
        if (m->length != 9 || !stream_read_be(m->payload, 8) || m->payload[8] < STREAM_OVERLOAD_COMPLETED
            || m->payload[8] > STREAM_OVERLOAD_EXPIRED) goto malformed;
        uint64_t id = stream_read_be(m->payload, 8);
        if (id < p->requested.id || (id == p->requested.id && !p->requested.active
            && (p->requested.outcome != STREAM_OVERLOAD_EXPIRED || m->payload[8] == STREAM_OVERLOAD_EXPIRED))) return;
        p->requested.id = id; p->requested.active = 0; p->requested.outcome = m->payload[8];
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream OVERLOAD END received peer=%s id=%llu outcome=%u cancel_pending_migration=%d",
            STREAM_ADDRESS_TEXT(p->address), (unsigned long long)id, m->payload[8], m->payload[8] != STREAM_OVERLOAD_EXPIRED);
        return;
    }
    if (m->command == STREAM_CMD_OVERLOAD) {
        if (m->length < STREAM_OVERLOAD_HEADER) goto malformed;
        unsigned count = m->payload[17]; uint64_t id = stream_read_be(m->payload, 8), required = stream_read_be(m->payload + 8, 8);
        if (!id || !count || count > STREAM_OVERLOAD_MAX || !m->payload[16] || m->payload[16] > count
            || m->length != STREAM_OVERLOAD_HEADER + count * 16) goto malformed;
        if (id <= p->requested.id) return;
        struct stream_pressure_request request = {.descriptor = m->descriptor, .id = id,
            .required_rate = required, .required_count = m->payload[16],
            .count = count, .deadline_ms = now + STREAM_LOAD_GRACE_MS, .active = 1};
        uint64_t total = 0;
        for (unsigned i = 0; i < count; i++) {
            request.cuts[i].handle = stream_read_be(m->payload + STREAM_OVERLOAD_HEADER + i * 16, 8);
            request.cuts[i].rate = stream_read_be(m->payload + STREAM_OVERLOAD_HEADER + i * 16 + 8, 8);
            if (!request.cuts[i].handle || !request.cuts[i].rate || request.cuts[i].rate > UINT64_MAX - total) goto malformed;
            for (unsigned j = 0; j < i; j++) if (request.cuts[j].handle == request.cuts[i].handle) goto malformed;
            total += request.cuts[i].rate;
        }
        if (required > total) goto malformed;
        p->requested = request; pressure_select(p, now);
        if (!p->requested.sent) DEBUG_INFO(DEBUG_CATEGORY_VIDEO,
            "stream OVERLOAD keep subscriptions peer=%s id=%llu candidates=%u requested_rate=%llu; safe selection pending",
            STREAM_ADDRESS_TEXT(p->address), (unsigned long long)id, count, (unsigned long long)required);
        return;
    }
    if (m->length < 9) goto malformed;
    unsigned count = m->payload[8]; uint64_t id = stream_read_be(m->payload, 8);
    if (!id || !count || count > STREAM_OVERLOAD_MAX || m->length != 9 + count * 8) goto malformed;
    if (!p->pressure.active || id != p->pressure.id || now >= p->pressure.deadline_ms) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream stale OVERLOAD_SELECT peer=%s id=%llu",
                    STREAM_ADDRESS_TEXT(p->address), (unsigned long long)id); return;
    }
    for (unsigned i = 0; i < count; i++) {
        uint64_t handle = stream_read_be(m->payload + 9 + i * 8, 8); int found = 0;
        for (unsigned j = 0; j < p->pressure.count; j++) if (p->pressure.cuts[j].handle == handle) found = 1;
        for (unsigned j = 0; j < i; j++) if (stream_read_be(m->payload + 9 + j * 8, 8) == handle) goto malformed;
        if (!found) goto malformed;
    }
    for (unsigned i = 0; i < count && (p->pressure.released_rate < p->pressure.required_rate
        || p->pressure.released_count < p->pressure.required_count); i++) {
        if (!p->pressure.required_rate && p->network->regular_used <= p->network->limits.regular) break;
        struct STREAM_RECEIVE *f = flow_by_handle(p, stream_read_be(m->payload + 9 + i * 8, 8), 0);
        if (f) flow_close(f, STREAM_REASON_OVERLOAD, 1);
    }
    return;
malformed:
    DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream malformed pressure command=%u peer=%s bytes=%zu", m->command,
               STREAM_ADDRESS_TEXT(p->address), m->length);
}

/* Завершение не закрывает DATA: completed/drained отменяет перенос, expired оставляет ему общий срок5с. */
static void pressure_end(struct stream_peer *p, int outcome, uint64_t now) {
    p->pressure.active = 0; p->pressure.outcome = outcome; p->pressure.end_pending = outcome; p->pressure_retry_ms = 0;
    p->cooling = 1; p->cooling_until_ms = now + 1000; p->clear_since_ms = 0;
}

/* 100-мс обход не отправляет DATA и не заменяет waiter. Метрики фиксируются с фактическим периодом >=200мс. */
static void pressure_tick(struct stream_network *net, uint64_t now) {
    limits_refresh(net);
    unsigned troubled = 0; uint64_t maximum_age = 0;
    for (struct stream_peer *p = net->peers; p; p = p->next) {
        uint64_t oldest = 0;
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            if (f->info.state == STREAM_RECEIVE_CLOSED) continue;
            uint64_t age = flow_age(f, now); if (age > oldest) oldest = age;
            struct stream_load_snapshot snapshot; stream_load_poll(&f->history->load, now, age, &snapshot);
        }
        struct stream_load_snapshot snapshot;
        int sampled = stream_load_poll(&p->load, now, oldest, &snapshot) == STREAM_OK;
        if (snapshot.pressure) troubled++;
        if (oldest > maximum_age) maximum_age = oldest;
        if (sampled && p->load.queued.bytes) DEBUG_TRACE(DEBUG_CATEGORY_STREAM,
            "stream load peer=%s A=%llu D=%llu X=%llu Q=%llu age=%llu input=%llu output=%llu g=%llu interval=%llu confirm=%d",
            STREAM_ADDRESS_TEXT(p->address), (unsigned long long)snapshot.arrivals.bytes, (unsigned long long)snapshot.sent.bytes,
            (unsigned long long)snapshot.removed.bytes, (unsigned long long)snapshot.queued.bytes, (unsigned long long)oldest,
            (unsigned long long)snapshot.input_rate, (unsigned long long)snapshot.output_rate,
            (unsigned long long)snapshot.growth_rate, (unsigned long long)snapshot.interval_ms, snapshot.confirmed);
    }
    struct stream_load_snapshot global; stream_load_poll(&net->load, now, maximum_age, &global);
    /* Несколько растущих очередей не доказывают нехватку CPU. Полоса разгружается через peer OVERLOAD;
     * измеренный CPU >80% сразу закрывает допуск, отдельный порог восстановления сохраняет запас. */
    int previous_global = net->global_pressure;
    if (net->cpu.valid && net->cpu.average_percent > STREAM_CPU_BUSY_PERCENT) {
        net->global_pressure = 1; net->global_candidate = 0;
    } else if (net->global_pressure && net->cpu.valid && net->cpu.average_percent <= STREAM_CPU_CLEAR_PERCENT) {
        if (!net->global_candidate) { net->global_candidate = 1; net->global_since_ms = now; }
        if (now - net->global_since_ms >= STREAM_LOAD_CONFIRM_MS) { net->global_pressure = 0; net->global_candidate = 0; }
    } else {
        net->global_candidate = 0;
    }
    if (previous_global != net->global_pressure) DEBUG_INFO(DEBUG_CATEGORY_STREAM,
        "stream node admission self=%s global_pressure=%d troubled_peers=%u cpu_pct=%.1f/%d loop_late_ms=%llu queue_B=%llu"
        " reason=CPU_reserve enter_gt_pct=%u clear_le_pct=%u clear_confirm_ms=%u",
        sc_public_key_hex(net->inst->my_ed25519_pubkey, (char[65]){0}), net->global_pressure, troubled, net->cpu.average_percent, net->cpu.valid,
        (unsigned long long)net->callback_late_ms, (unsigned long long)global.queued.bytes,
        STREAM_CPU_BUSY_PERCENT, STREAM_CPU_CLEAR_PERCENT, STREAM_LOAD_CONFIRM_MS);
    if (net->flow_count && now >= net->diagnostic_ms && debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_STREAM)) {
        net->diagnostic_ms = now + 1000;
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream LOAD self=%s scope=node objects=%u DATA=%u/%u temporary=%u/%u peers=%u/%u"
            " A=%llu D=%llu X=%llu Q=%llu age_ms=%llu input_Bps=%llu output_Bps=%llu growth_Bps=%llu"
            " cpu_pct=%.1f/%d loop_late_ms=%llu troubled_peers=%u global_pressure=%d rejected=%llu diagnosis=%s",
            sc_public_key_hex(net->inst->my_ed25519_pubkey, (char[65]){0}), net->flow_count, net->regular_used, net->limits.regular,
            net->temporary_used, net->limits.temporary,
            net->peer_count, net->limits.peers,
            (unsigned long long)global.arrivals.bytes, (unsigned long long)global.sent.bytes,
            (unsigned long long)global.removed.bytes, (unsigned long long)global.queued.bytes,
            (unsigned long long)global.oldest_age_ms, (unsigned long long)global.input_rate,
            (unsigned long long)global.output_rate, (unsigned long long)global.growth_rate,
            net->cpu.average_percent, net->cpu.valid, (unsigned long long)net->callback_late_ms, troubled, net->global_pressure,
            (unsigned long long)global.rejected, net->global_pressure ? "CPU_admission_hold"
                : troubled > 1 ? "multi_peer_backlog_unknown_cause" : troubled ? "peer_backlog_unknown_cause" : "no_confirmed_overload");
    }
    if (net->global_pressure && troubled >= 2 && global.confirmed) {
        uint64_t required = global.growth_rate ? global.growth_rate + global.growth_rate / 100 + 1 : global.queued.bytes / 2 + 1;
        for (struct stream_peer *p = net->peers; p && required; p = p->next) if (p->pressure.active) {
            uint64_t pending = p->pressure.required_rate > p->pressure.released_rate
                               ? p->pressure.required_rate - p->pressure.released_rate : 0;
            required = pending >= required ? 0 : required - pending;
        }
        for (unsigned pass = 0; pass < net->peer_count && required; pass++) {
            struct stream_peer *best = NULL; uint64_t best_rate = 0;
            for (struct stream_peer *p = net->peers; p; p = p->next) {
                if (p->pressure.active || p->cooling) continue;
                uint64_t total = 0;
                for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
                    if (!f->outgoing && f->info.state == STREAM_RECEIVE_ACTIVE) total += flow_rate(f, now);
                if (total > best_rate) { best = p; best_rate = total; }
            }
            if (!best) break;
            uint64_t cut = required < best_rate ? required : best_rate;
            if (!pressure_begin(best, cut, 0, now)) break;
            required -= best->pressure.required_rate;
        }
    }
    /* Изменённый regular требует числа закрытий, даже если очереди пусты и скорости равны нулю.
     * Один и тот же дефицит не назначается нескольким пирам; следующие порции запрашиваются после результата. */
    unsigned excess = net->regular_used > net->limits.regular ? net->regular_used - net->limits.regular : 0;
    for (struct stream_peer *p = net->peers; p && excess; p = p->next) if (p->pressure.active) {
        unsigned pending = p->pressure.required_count > p->pressure.released_count
            ? p->pressure.required_count - p->pressure.released_count : 0;
        excess = pending >= excess ? 0 : excess - pending;
    }
    for (unsigned pass = 0; pass < net->peer_count && excess; pass++) {
        struct stream_peer *best = NULL; unsigned largest = 0;
        for (struct stream_peer *p = net->peers; p; p = p->next) {
            if (p->pressure.active || p->pressure.end_pending || p->cooling) continue;
            unsigned count = 0;
            for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
                if (!f->outgoing && f->info.state == STREAM_RECEIVE_ACTIVE) count++;
            if (count && (!best || p->pressure.deadline_ms < best->pressure.deadline_ms
                || (p->pressure.deadline_ms == best->pressure.deadline_ms && count > largest))) { largest = count; best = p; }
        }
        if (!best || !pressure_begin(best, 0, excess, now)) break;
        excess -= best->pressure.required_count;
    }
    if (excess && now >= net->limits_report_ms) {
        unsigned minimum = 0;
        for (struct stream_receive_reservation *r = net->reservations; r; r = r->next) minimum += r->minimum;
        net->limits_report_ms = now + 5000;
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream budget reduction pending regular=%u/%u unassigned=%u reserved_RX_minimum=%u"
            " reason=no_ready_downstream preserve_essential=1 retry=100ms",
            net->regular_used, net->limits.regular, excess, minimum);
    }
    for (struct stream_peer *p = net->peers; p; p = p->next) {
        p->busy++;
        int accepting_before = !net->global_pressure && !p->pressure.active && !p->cooling;
        if ((p->pressure.active || p->pressure.outcome == STREAM_OVERLOAD_EXPIRED)
            && (p->pressure.required_rate ? !p->load.queued.packets : net->regular_used <= net->limits.regular)) {
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream OVERLOAD drained self=%s peer=%s id=%llu"
                " outcome=queue_drained released=%llu requested=%llu preserve_subscriptions=1",
                sc_public_key_hex(net->inst->my_ed25519_pubkey, (char[65]){0}), STREAM_ADDRESS_TEXT(p->address), (unsigned long long)p->pressure.id,
                (unsigned long long)p->pressure.released_rate, (unsigned long long)p->pressure.required_rate);
            pressure_end(p, STREAM_OVERLOAD_DRAINED, now);
        }
        if (p->cooling) {
            if (p->load.snapshot.pressure) {
                p->clear_since_ms = 0;
                if (now >= p->cooling_until_ms && !p->pressure.end_pending) {
                    p->cooling = 0;
                    DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "stream OVERLOAD retry peer=%s previous=%llu backlog=%llu age_ms=%llu",
                        STREAM_ADDRESS_TEXT(p->address), (unsigned long long)p->pressure.id,
                        (unsigned long long)p->load.queued.bytes, (unsigned long long)p->load.snapshot.oldest_age_ms);
                }
            } else if (!p->clear_since_ms) p->clear_since_ms = now;
            else if (now - p->clear_since_ms >= STREAM_LOAD_CONFIRM_MS) { p->cooling = 0; p->clear_since_ms = 0; }
        }
        if (p->load.snapshot.confirmed && !p->pressure.active && !p->cooling) {
            uint64_t g = p->load.snapshot.growth_rate;
            pressure_begin(p, g ? g + g / 100 + 1 : p->load.queued.bytes / 2 + 1, 0, now);
        }
        int completed = p->pressure.released_rate >= p->pressure.required_rate && p->pressure.released_count >= p->pressure.required_count;
        if (p->pressure.active && (completed || now >= p->pressure.deadline_ms)) {
            pressure_end(p, completed ? STREAM_OVERLOAD_COMPLETED : STREAM_OVERLOAD_EXPIRED, now);
            debug_level_t level = completed ? DEBUG_LEVEL_INFO : DEBUG_LEVEL_WARN;
            if (debug_should_output(level, DEBUG_CATEGORY_VIDEO)) debug_output(level, DEBUG_CATEGORY_VIDEO,
                __FUNCTION__, __FILE__, __LINE__, "stream OVERLOAD result self=%s peer=%s id=%llu outcome=%s"
                " closed=%u/%u rate=%llu requested=%llu shortfall=%llu cooling=1 preserve_essential=1",
                sc_public_key_hex(net->inst->my_ed25519_pubkey, (char[65]){0}), STREAM_ADDRESS_TEXT(p->address), (unsigned long long)p->pressure.id,
                completed ? "completed" : "deadline_unmet", p->pressure.released_count, p->pressure.required_count,
                (unsigned long long)p->pressure.released_rate, (unsigned long long)p->pressure.required_rate,
                (unsigned long long)(p->pressure.required_rate > p->pressure.released_rate
                    ? p->pressure.required_rate - p->pressure.released_rate : 0));
        }
        pressure_send(p, now); pressure_select(p, now);
        if (p->requested.active && now >= p->requested.deadline_ms) {
            p->requested.active = 0; p->requested.outcome = STREAM_OVERLOAD_EXPIRED;
            DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "stream OVERLOAD receiver deadline peer=%s id=%llu END_missing=1 preserve_essential=1",
                STREAM_ADDRESS_TEXT(p->address), (unsigned long long)p->requested.id);
        }
        int accepting = !net->global_pressure && !p->pressure.active && !p->cooling;
        if (accepting != accepting_before) DEBUG_INFO(DEBUG_CATEGORY_STREAM,
            "stream peer admission self=%s peer=%s accepting=%d pressure=%d cooling=%d global_pressure=%d",
            sc_public_key_hex(net->inst->my_ed25519_pubkey, (char[65]){0}), STREAM_ADDRESS_TEXT(p->address), accepting,
            p->pressure.active, p->cooling, net->global_pressure);
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            if (f->account_failed) flow_close(f, STREAM_REASON_PROTOCOL, 1);
        }
        p->busy--; peer_sweep(p);
    }
}

uint64_t stream_network_pending_bytes(struct UTUN_INSTANCE *inst, const struct stream_descriptor *d) {
    struct stream_network *net = stream_network_get(inst); uint64_t bytes = 0;
    if (!net || !d) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "pending stream snapshot unavailable"); return 0; }
    for (struct stream_peer *p = net->peers; p; p = p->next)
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next)
            if (!f->outgoing && f->info.state != STREAM_RECEIVE_CLOSED && same_stream(&f->descriptor, d)) bytes += f->history->load.queued.bytes;
    return bytes;
}

/* Выбираем целый путь; общие узлы разных субпотоков никогда не склеиваются в искусственную цепочку. */
int stream_network_telemetry(struct UTUN_INSTANCE *inst, const struct stream_descriptor *d, struct stream_telemetry *out) {
    struct stream_network *net = stream_network_get(inst); struct stream_local local;
    if (!net || !d || !out || stream_local_get(inst, d, &local) != STREAM_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream telemetry snapshot unavailable"); return STREAM_ERR_STATE;
    }
    struct stream_telemetry worst = {0}; uint64_t total = 0, now = get_time_tb() / 10;
    int active = 0;
    for (struct stream_peer *p = net->peers; p; p = p->next)
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            if (!f->outgoing || f->info.probe_only || f->info.state != STREAM_RECEIVE_ACTIVE
                || !same_stream(&f->descriptor, d) || !f->info.path.count) continue;
            active = 1;
            struct stream_telemetry t;
            int matched = stream_buffer_get_telemetry(local.buffer, f->subblock, now, &t) == STREAM_OK
                          && t.count == f->info.path.count;
            for (unsigned i = 0; matched && i < t.count; i++) if (!stream_address_equal(t.rows[i].node, f->info.path.nodes[i])) matched = 0;
            if (!matched) {
                t = (struct stream_telemetry){.count = f->info.path.count, .received_ms = now};
                for (unsigned i = 0; i < t.count; i++) t.rows[i] = (struct stream_telemetry_row){
                    .node = f->info.path.nodes[i], .flags = STREAM_TELEMETRY_STALE, .age_ms = UINT32_MAX};
            }
            uint64_t sum = stream_telemetry_total(&t);
            if (!worst.count || sum > total) { worst = t; total = sum; }
        }
    if (!active) for (unsigned sub = 0; sub < d->n; sub++) {
        struct stream_telemetry t;
        if (stream_buffer_get_telemetry(local.buffer, (uint8_t)sub, now, &t) != STREAM_OK) continue;
        uint64_t sum = stream_telemetry_total(&t);
        if (!worst.count || sum > total) { worst = t; total = sum; }
    }
    *out = worst; return STREAM_OK;
}

int stream_network_buffer_positions(struct UTUN_INSTANCE *inst, const struct stream_descriptor *d,
                                    struct stream_buffer_positions *out) {
    struct stream_network *net = stream_network_get(inst);
    struct stream_local local;
    if (!net || !d || !out) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "stream buffer positions invalid arguments"); return STREAM_ERR_ARGUMENT;
    }
    if (stream_local_get(inst, d, &local) != STREAM_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "stream buffer positions missing source=%s", STREAM_ADDRESS_TEXT(d->source));
        return STREAM_ERR_STATE;
    }
    struct stream_buffer_stats stats;
    int result = stream_buffer_get_stats(local.buffer, &stats);
    if (result != STREAM_OK) return result;
    struct stream_buffer_positions positions = {.first = stats.expired_before, .head = stats.blocks ? stats.highest_seq + 1 : 0};
    for (struct stream_peer *p = net->peers; p; p = p->next) {
        struct stream_buffer_peer_position row = {.peer = p->address, .worst = UINT64_MAX, .best_us = UINT64_MAX};
        int timing_known = 1;
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            if (f->outgoing || f->info.state != STREAM_RECEIVE_ACTIVE || f->info.next == UINT64_MAX
                || !same_stream(&f->descriptor, d)) continue;
            row.subflows++;
            if (f->info.next < row.worst) row.worst = f->info.next;
            if (f->info.next > row.best) row.best = f->info.next;
            struct stream_buffer_delay delay;
            result = stream_buffer_get_delay(local.buffer, f->info.next, f->subblock, &delay);
            if (result != STREAM_OK) return result;
            if (!delay.known) timing_known = 0;
            uint64_t duration = delay.video_us > delay.audio_us ? delay.video_us : delay.audio_us;
            if (duration > row.worst_us) row.worst_us = duration;
            if (duration < row.best_us) row.best_us = duration;
        }
        if (!row.subflows) continue;
        if (!timing_known) row.worst_us = row.best_us = UINT64_MAX;
        DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "stream buffer position peer=%s subflows=%u next=%llu..%llu pending_us=%llu..%llu known=%d",
            STREAM_ADDRESS_TEXT(row.peer), row.subflows, (unsigned long long)row.worst, (unsigned long long)row.best,
            (unsigned long long)row.best_us, (unsigned long long)row.worst_us, timing_known);
        positions.total++;
        unsigned at = 0;
        while (at < positions.count && (positions.peers[at].worst < row.worst
            || (positions.peers[at].worst == row.worst && stream_address_cmp(positions.peers[at].peer, row.peer) < 0))) at++;
        if (at == STREAM_BUFFER_POSITION_PEERS) continue;
        if (positions.count < STREAM_BUFFER_POSITION_PEERS) positions.count++;
        for (unsigned i = positions.count - 1; i > at; i--) positions.peers[i] = positions.peers[i - 1];
        positions.peers[at] = row;
    }
    *out = positions; return STREAM_OK;
}

int stream_peer_get_load(struct UTUN_INSTANCE *inst, struct stream_address node, struct stream_peer_load_info *info) {
    struct stream_network *net = stream_network_get(inst); struct stream_peer *p = net ? peer_find(net, node) : NULL;
    if (!p || !info) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream load unavailable peer=%s", STREAM_ADDRESS_TEXT(node)); return STREAM_ERR_STATE; }
    struct stream_peer_load_info result = {.load = p->load.snapshot, .request_id = p->pressure.id,
        .required_rate = p->pressure.required_rate, .released_rate = p->pressure.released_rate,
        .deadline_ms = p->pressure.deadline_ms, .required_count = p->pressure.required_count, .released_count = p->pressure.released_count,
        .request_active = p->pressure.active, .cpu_percent = net->cpu.valid ? net->cpu.average_percent : 0,
        .request_outcome = p->pressure.outcome,
        .callback_late_ms = net->callback_late_ms, .global_pressure = net->global_pressure,
        .accepting = !net->global_pressure && !p->pressure.active && !p->cooling};
    if (result.request_outcome == STREAM_OVERLOAD_EXPIRED && result.required_rate > result.released_rate)
        result.shortfall_rate = result.required_rate - result.released_rate;
    result.load.queued = p->load.queued; result.load.arrivals = p->load.arrivals; result.load.sent = p->load.sent;
    result.load.removed = p->load.removed; result.load.rejected = p->load.rejected;
    struct ETCP_CONN *conn = node_conn_direct_get_conn(p->ncd);
    if (conn) {
        struct ll_queue *queues[] = {conn->send_input_q, conn->input_queue};
        for (unsigned i = 0; i < 2; i++) if (queues[i]) {
            result.transport_queued_packets += queues[i]->count; result.transport_queued_bytes += queues[i]->total_bytes;
        }
        result.transport_queued_bytes = etcp_tx_queued(conn);
        result.inflight_bytes = conn->unacked_bytes;
    }
    *info = result; return STREAM_OK;
}

/* Снимок очередей точной трансляции, без общей ETCP очереди других сервисов. */
void stream_network_stats(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct stream_stat_sample *sample) {
    struct stream_network *net = stream_network_get(inst); uint8_t parts[256] = {0};
    for (struct stream_peer *p = net->peers; p; p = p->next) {
        int provider = 0, receiver = 0;
        for (struct STREAM_RECEIVE *f = p->flows; f; f = f->next) {
            if (!same_stream(&f->descriptor, key) || f->info.probe_only || f->info.state != STREAM_RECEIVE_ACTIVE) continue;
            if (f->outgoing) { sample->v[ST_IN]++; parts[f->subblock] = 1; provider = 1; }
            else {
                sample->v[ST_OUT]++; receiver = 1;
                sample->v[ST_QUEUE] += f->history->load.queued.bytes;
                sample->v[ST_QUEUE_PARTS] += f->history->load.queued.packets;
                uint64_t age = flow_age(f, get_time_tb() / 10);
                if (age > sample->v[ST_QUEUE_AGE]) sample->v[ST_QUEUE_AGE] = age;
            }
        }
        sample->v[ST_PROVIDERS] += provider; sample->v[ST_RECEIVERS] += receiver;
    }
    for (unsigned i = 0; i < 256; i++) sample->v[ST_PARTS] += parts[i];
}

static void stats_flush(struct stream_network *net, uint64_t now) {
    struct stream_stats_pending **link = &net->stats_pending;
    while (*link) {
        struct stream_stats_pending *pending = *link;
        if (pending->attempted_ms && now - pending->attempted_ms < 1000) { link = &pending->next; continue; }
        pending->attempted_ms = now;
        const struct stream_descriptor *key = &pending->descriptor;
        struct ETCP_ROUTER_TX_STATS tx;
        int state = etcp_router_tx_stats_key(net->inst, key->group_id, key->source.ed, ETCP_RT_ID_STREAM, get_time_us(), &tx);
        if (!state && (tx.queued_packets || tx.inflight_packets)) { link = &pending->next; continue; }
        uint8_t payload[STREAM_STAT_FIELDS * 8];
        for (unsigned i = 0; i < STREAM_STAT_FIELDS; i++) stream_write_be(payload + i * 8, pending->sample.v[i], 8);
        struct stream_message m = {.descriptor = *key, .sender_epoch = net->relay_epoch,
            .handle = pending->sample.sequence, .command = STREAM_CMD_STATS};
        int result = route_control(net, key->source, m, payload, sizeof(payload), NULL);
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "statistics TX source=%s epoch=%llx seq=%llu audio_output_ms=%llu max_audio_glitch_ms=%lld"
            " audio_glitch_ms=%llu NAT=%llu CPU=%llu result=%d",
            STREAM_ADDRESS_TEXT(key->source), (unsigned long long)key->source_epoch,
            (unsigned long long)m.handle, (unsigned long long)pending->sample.v[ST_AUDIO_OBSERVED],
            pending->sample.v[ST_PAUSE_MAX] == UINT64_MAX ? -1LL : (long long)pending->sample.v[ST_PAUSE_MAX],
            (unsigned long long)pending->sample.v[ST_PAUSE_MS], (unsigned long long)pending->sample.v[ST_NAT],
            (unsigned long long)pending->sample.v[ST_CPU], result);
        if (result == STREAM_OK) { *link = pending->next; u_free(pending); }
        else link = &pending->next;
    }
}

void stream_network_report(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct stream_stat_sample *sample) {
    struct stream_network *net = stream_network_get(inst);
    sample->sequence = ++net->stats_sequence; sample->generation = net->relay_epoch;
    struct stream_stats_pending *pending = net->stats_pending;
    while (pending && !same_stream(&pending->descriptor, key)) pending = pending->next;
    if (!pending) {
        pending = u_calloc(1, sizeof(*pending));
        if (!pending) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "statistics pending allocation failed"); return; }
        pending->descriptor = *key; pending->next = net->stats_pending; net->stats_pending = pending;
    }
    pending->sample = *sample; pending->attempted_ms = 0; stats_flush(net, get_time_tb() / 10);
}

/* Автор начинает новый эфир либо удаляет группу: архив прежней эпохи больше не нужен. */
void stream_stats_forget(struct UTUN_INSTANCE *inst, uint64_t group) {
    struct stream_network *net = stream_network_get(inst);
    if (!net) return;
    struct stream_stats_entry **link = &net->statistics;
    while (*link) {
        struct stream_stats_entry *entry = *link;
        if (entry->descriptor.group_id == group) { *link = entry->next; u_free(entry); }
        else link = &entry->next;
    }
}

/* Один узел считается один раз независимо от числа его reader handles и пути через relay. */
unsigned stream_stats_viewer_count(struct UTUN_INSTANCE *inst, uint64_t group, uint64_t epoch) {
    struct stream_network *net = stream_network_get(inst);
    if (!net || !group || !epoch) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer count invalid instance/group/epoch group=%llx epoch=%llx",
            (unsigned long long)group, (unsigned long long)epoch); return 0;
    }
    unsigned count = 0; uint64_t now = get_time_tb() / 10;
    for (struct stream_stats_entry *e = net->statistics; e; e = e->next) {
        if (e->descriptor.group_id != group || e->descriptor.source_epoch != epoch || !e->data.count) continue;
        const struct stream_stat_sample *sample = &e->data.samples[e->data.count - 1];
        if (!sample->v[ST_RELAY] && !sample->v[ST_FINISHED] && now - sample->at_ms < STREAM_STATS_STALE_MS) count++;
    }
    return count;
}

int stream_stats_read(struct UTUN_INSTANCE *inst, uint64_t group, uint64_t epoch, struct stream_stat_node **out, size_t *count) {
    if (!out || *out || !count || !stream_network_get(inst)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "statistics read invalid arguments/state"); return STREAM_ERR_ARGUMENT;
    }
    *count = 0; struct stream_network *net = stream_network_get(inst);
    for (struct stream_stats_entry *e = net->statistics; e; e = e->next)
        if (e->descriptor.group_id == group && e->descriptor.source_epoch == epoch) (*count)++;
    if (!*count) return STREAM_OK;
    *out = u_calloc(*count, sizeof(**out));
    if (!*out) { *count = 0; DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "statistics snapshot allocation failed"); return STREAM_ERR_MEMORY; }
    struct stream_descriptor key = {.group_id=group, .source=stream_address_read(inst->my_ed25519_pubkey), .source_epoch=epoch};
    struct stream_local publication; uint64_t snapshot_ms = 0;
    int live = stream_local_get(inst, &key, &publication) == STREAM_OK && publication.publisher && !publication.finished;
    if (live) snapshot_ms = get_time_tb() / 10;
    else for (struct stream_stats_entry *e = net->statistics; e; e = e->next)
        if (same_stream(&e->descriptor, &key) && e->data.count
            && e->data.samples[e->data.count-1].at_ms > snapshot_ms) snapshot_ms = e->data.samples[e->data.count-1].at_ms;
    size_t n = 0;
    for (struct stream_stats_entry *e = net->statistics; e; e = e->next)
        if (e->descriptor.group_id == group && e->descriptor.source_epoch == epoch) { (*out)[n] = e->data; (*out)[n++].now_ms = snapshot_ms; }
    return STREAM_OK;
}
