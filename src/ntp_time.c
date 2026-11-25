#include "ntp_time.h"
#include "ntp_node_time.h"
#include "utun_instance.h"
#include "chat/chat_sync.h"
#include "routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../lib/debug_config.h"
#include "../lib/socket_compat.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../lib/async_dns.h"
#include "../lib/db_sql.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define NTP_DELTA 2208988800ULL         // seconds from 1900 to 1970
#define NTP_PORT  123
#define NTP_TIMEOUT_SEC 2
#define NTP_MAX_TRIES   3

#pragma pack(push, 1)
struct ntp_packet {
    uint8_t  li_vn_mode;        // LI(2) | VN(3) | Mode(3)
    uint8_t  stratum;
    uint8_t  poll;
    uint8_t  precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint64_t ref_ts;
    uint64_t orig_ts;
    uint64_t recv_ts;
    uint64_t xmit_ts;
};
#pragma pack(pop)

_Static_assert(sizeof(struct ntp_packet) == 48, "NTP packet must be 48 bytes");

/* ─── синхронизация: неблокирующая машина состояний на uasync ─── */

struct ntp_sync {
    struct UTUN_INSTANCE* inst;
    int server_idx;          /* следующий сервер для попытки */
    int servers_left;        /* остаток цикла, общий для синхронных и асинхронных ошибок */
    int query_idx;           /* сервер, который сейчас опрашиваем (-1 для test_addr) */
    struct adns_query* dns_q; /* активный DNS-запрос (NULL когда нет) */
    socket_t sock;           /* NTP UDP-сокет (SOCKET_INVALID когда закрыт) */
    void* socket_id;         /* handle uasync */
    void* timer;             /* handle таймаута запроса */
    uint8_t req[48];         /* xmit_ts обновляется при каждой отправке, включая повторы */
    uint64_t t1_ntp;         /* originate timestamp */
    int tries;               /* попыток на текущем сервере */
    struct sockaddr_in qaddr;/* адрес текущего сервера */
};

static void ntp_time_sync_cb(void* arg);
static void ntp_try_next(struct ntp_sync* ctx);
static int ntp_start_query(struct ntp_sync* ctx, const struct sockaddr_in* addr);
static int ntp_send_request(struct ntp_sync* ctx);
static void ntp_close_query(struct ntp_sync* ctx);
static void ntp_finish_cycle(struct ntp_sync* ctx, int synced);
static void ntp_resolve_done_cb(const struct adns_result* res, void* arg);
static void ntp_query_read_cb(socket_t sock, void* arg);
static void ntp_query_timeout_cb(void* arg);

/* Открыть обмен групп один раз; NTP может продолжать запросы после истечения стартовых 4 секунд. */
static void ntp_release_startup(struct UTUN_INSTANCE* inst, const char* reason) {
    if (!inst->ntp.startup_wait) return;
    if (inst->ntp.startup_timer) uasync_cancel_timeout(inst->ua, inst->ntp.startup_timer);
    inst->ntp.startup_timer = NULL; inst->ntp.startup_wait = 0;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP: group startup released reason=%s priority=%d offset=%lldus",
        reason, inst->ntp.source, (long long)inst->ntp.offset_us);
    topo_groups_time_ready(inst);
    chat_sync_time_ready(inst);
}

static void ntp_startup_timeout(void* arg) {
    struct UTUN_INSTANCE* inst = arg;
    inst->ntp.startup_timer = NULL;
    ntp_release_startup(inst, "4-second deadline");
    ntp_node_sync_peers(inst);
}

static int64_t ntp_local_us(void) {
    struct timeval tv;
#ifdef _WIN32
    utun_gettimeofday(&tv, NULL);
#else
    gettimeofday(&tv, NULL);
#endif
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/* Свежий источник всегда выше сохранённого; среди сохранённых предпочитаем прежний NTP, затем пира. */
static int ntp_select_offset(struct UTUN_INSTANCE* inst) {
    struct NTP_TIME* ntp = &inst->ntp;
    enum ntp_time_source selected = NTP_TIME_LOCAL;
    int best = 0;
    for (int source = NTP_TIME_SAVED; source <= NTP_TIME_SERVER; source++) {
        struct ntp_time_offset* offset = &ntp->offsets[source];
        if (!offset->valid) continue;
        int priority = offset->fresh ? source : NTP_TIME_SAVED;
        if (priority >= best) { selected = source; best = priority; }
    }
    int64_t value = ntp->offsets[selected].offset_us;
    int changed = ntp->source != best || ntp->offset_us != value;
    ntp->source = best; ntp->synced = best != NTP_TIME_LOCAL; ntp->offset_us = value;
    if (changed) DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP: selected priority=%d stored_source=%d offset=%lldus fresh=%d",
        best, selected, (long long)value, ntp->offsets[selected].fresh);
    return changed;
}

static const char ntp_snapshot_sql[] = "SELECT source,offset_us,received_us FROM clock_offsets ORDER BY source";
static const char ntp_save_sql[] = "INSERT INTO clock_offsets(source,offset_us,received_us) VALUES(?,?,?)"
    " ON CONFLICT(source) DO UPDATE SET offset_us=excluded.offset_us,received_us=excluded.received_us";
struct ntp_restore { struct UTUN_INSTANCE* instance; };
struct ntp_write { int source; int64_t offset_us; };

/* Проверяем весь снимок до публикации; ни преобразований TEXT, ни частичного кэша. */
static int ntp_decode_offset(const struct db_value* values, struct ntp_time_offset offsets[4]) {
    if (values[0].type != SQLITE_INTEGER || values[0].i64 < NTP_TIME_SAVED || values[0].i64 > NTP_TIME_SERVER ||
        values[1].type != SQLITE_INTEGER || values[2].type != SQLITE_INTEGER ||
        values[1].i64 < -NTP_TIME_LIMIT_US || values[1].i64 > NTP_TIME_LIMIT_US) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: invalid saved measurement types=%d/%d/%d source=%lld offset=%lld; cache rejected",
            values[0].type, values[1].type, values[2].type, (long long)values[0].i64, (long long)values[1].i64); return -1;
    }
    int source = (int)values[0].i64;
    if (offsets[source].valid) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: duplicate saved source=%d; cache rejected", source); return -1;
    }
    offsets[source] = (struct ntp_time_offset){.valid=1, .offset_us=values[1].i64, .received_us=values[2].i64}; return 0;
}
static int ntp_import_offsets(struct UTUN_INSTANCE* inst, const struct ntp_time_offset offsets[4]) {
    for (int source = NTP_TIME_SAVED; source <= NTP_TIME_SERVER; source++) {
        /* Любое измерение после init, в том числе SAVED от пира, новее снимка БД. */
        if (!offsets[source].valid || inst->ntp.offsets[source].valid) continue;
        inst->ntp.offsets[source] = offsets[source];
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP: restored source=%d offset=%lldus received=%lld",
            source, (long long)offsets[source].offset_us, (long long)offsets[source].received_us);
    }
    return ntp_select_offset(inst);
}
static void ntp_offsets_loaded(void* arg, const struct db_sql_result* result) {
    struct ntp_restore* request = arg; struct UTUN_INSTANCE* inst = request->instance;
    int current = inst->ntp.restore == request;
    if (current) inst->ntp.restore = NULL;
    u_free(request);
    if (!current || inst->destroying) {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "NTP: saved offsets ignored after stop/restart"); return;
    }
    struct ntp_time_offset offsets[4] = {{0}}; int valid = !result->outcome.status && result->columns == 3;
    if (!valid) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: saved offsets failed status=%d rc=%d columns=%u: %s",
        result->outcome.status, result->outcome.sqlite_code, result->columns, result->outcome.message);
    for (size_t row = 0; valid && row < result->rows; row++)
        valid = !ntp_decode_offset(result->values + row * 3, offsets);
    int changed = valid ? ntp_import_offsets(inst, offsets) : 0;
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "NTP: saved offsets completed rows=%zu valid=%d changed=%d", result->rows, valid, changed);
    if (!inst->ntp.enabled || !inst->ntp.server_count) ntp_release_startup(inst, "saved offsets completed");
    if (changed) ntp_node_sync_peers(inst);
}
/* Общая схема уже создана ядром. Синхронный запасной путь допустим только для :memory:. */
static int ntp_restore_offsets(struct UTUN_INSTANCE* inst) {
    if (inst->db_worker) {
        struct ntp_restore* request = u_malloc(sizeof(*request));
        if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: saved offsets request allocation failed"); return -1; }
        request->instance = inst; inst->ntp.restore = request;
        db_query(inst->db_worker, ntp_snapshot_sql, DB_NO_ARGS, ntp_offsets_loaded, request); return 0;
    }
    sqlite3* db = inst->topo_sqlite_db;
    if (!db) return 0;
    const char* file = sqlite3_db_filename(db, "main");
    if (!file || *file) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: no asynchronous saved offsets reader"); return 0; }
    struct ntp_time_offset offsets[4] = {{0}}; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, ntp_snapshot_sql, -1, &st, NULL);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct db_value values[3] = {{0}};
        for (int col = 0; col < 3; col++) {
            values[col].type = sqlite3_column_type(st, col);
            if (values[col].type == SQLITE_INTEGER) values[col].i64 = sqlite3_column_int64(st, col);
        }
        if (ntp_decode_offset(values, offsets)) { rc = SQLITE_CORRUPT; break; }
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        int error = rc != SQLITE_DONE ? rc : final;
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: RAM offsets restore failed rc=%d finalize=%d: %s", rc, final,
            (sqlite3_errcode(db) & 255) == (error & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(error)); return 0;
    }
    ntp_import_offsets(inst, offsets); return 0;
}
static void ntp_offset_saved(void* arg, const struct db_sql_result* result) {
    struct ntp_write* request = arg;
    if (result->outcome.status) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL,
        "NTP: save offset failed source=%d offset=%lldus status=%d rc=%d: %s", request->source,
        (long long)request->offset_us, result->outcome.status, result->outcome.sqlite_code, result->outcome.message);
    else DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "NTP: offset committed source=%d offset=%lldus", request->source, (long long)request->offset_us);
    u_free(request);
}

int ntp_time_record_offset(struct UTUN_INSTANCE* inst, enum ntp_time_source source, int64_t offset_us) {
    if (!inst || source < NTP_TIME_SAVED || source > NTP_TIME_SERVER || offset_us < -NTP_TIME_LIMIT_US || offset_us > NTP_TIME_LIMIT_US) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: invalid offset source=%d offset=%lld instance=%p",
            source, (long long)offset_us, (void*)inst); return -1;
    }
    struct ntp_time_offset* sample = &inst->ntp.offsets[source];
    *sample = (struct ntp_time_offset){.valid = 1, .fresh = source != NTP_TIME_SAVED,
        .offset_us = offset_us, .received_us = ntp_local_us()};
    sqlite3* db = inst->topo_sqlite_db;
    if (db_async_can_write(inst->db_worker)) {
        struct ntp_write* request = u_malloc(sizeof(*request));
        if (!request) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: save offset request allocation failed source=%d", source);
        else {
            *request = (struct ntp_write){source, offset_us};
            db_exec(inst->db_worker, ntp_save_sql, DB_ARGS(DB_I64(source), DB_I64(offset_us), DB_I64(sample->received_us)),
                ntp_offset_saved, request);
        }
    } else if (db) {
        /* Переходный RO worker оставляет прежнего единственного писателя до общего переключения. */
        const char* file = sqlite3_db_filename(db, "main");
        if (!inst->db_worker && (!file || *file)) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: no asynchronous offsets writer");
            inst->ntp.last_sync_tb = get_time_tb(); return ntp_select_offset(inst);
        }
        sqlite3_stmt* st = NULL;
        int rc = sqlite3_get_autocommit(db) ? SQLITE_OK : SQLITE_BUSY;
        if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(db, ntp_save_sql, -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 1, source);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, sample->offset_us);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 3, sample->received_us);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int final = sqlite3_finalize(st);
        if (rc != SQLITE_DONE || final != SQLITE_OK) {
            int error = rc != SQLITE_DONE ? rc : final;
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: save offset failed source=%d offset=%lldus rc=%d finalize=%d: %s",
                source, (long long)offset_us, rc, final,
                (sqlite3_errcode(db) & 255) == (error & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(error));
        } else DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "NTP: saved offset source=%d offset=%lldus received=%lld",
            source, (long long)offset_us, (long long)sample->received_us);
    }
    inst->ntp.last_sync_tb = get_time_tb();
    return ntp_select_offset(inst);
}

static uint64_t timeval_to_ntp(struct timeval *tv) {
    uint64_t sec  = (uint64_t)(tv->tv_sec + NTP_DELTA);
    uint64_t frac = ((uint64_t)tv->tv_usec << 32) / 1000000ULL;
    return (sec << 32) | frac;
}

static int64_t ntp64_to_us(uint64_t ntp) {
    int64_t sec  = (int64_t)(ntp >> 32) - (int64_t)NTP_DELTA;
    int64_t frac = ((int64_t)(ntp & 0xFFFFFFFFULL) * 1000000ULL) >> 32;
    return sec * 1000000LL + frac;
}

/* ─── чистые функции сборки/разбора пакета ─── */

static void ntp_build_request(uint8_t* req, uint64_t* t1_ntp_out) {
    struct ntp_packet request;
    memset(&request, 0, sizeof(request));
    request.li_vn_mode = (0 << 6) | (4 << 3) | 3;
    struct timeval t1_tv;
#ifdef _WIN32
    utun_gettimeofday(&t1_tv, NULL);
#else
    gettimeofday(&t1_tv, NULL);
#endif
    uint64_t t1_ntp = timeval_to_ntp(&t1_tv);
    request.xmit_ts = htobe64(t1_ntp);
    memcpy(req, &request, sizeof(request));
    if (t1_ntp_out) *t1_ntp_out = t1_ntp;
}

/* возвращает 0 если ответ валиден; иначе -1 (пакет игнорируется, ждём timeout) */
static int ntp_parse_reply(const uint8_t* buf, size_t len, uint64_t t1_ntp,
                           int64_t* offset_us_out, int64_t* rtt_us_out, int* stratum_out) {
    if (len < sizeof(struct ntp_packet)) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP: short reply: bytes=%zu required=%zu", len, sizeof(struct ntp_packet));
        return -1;
    }
    const struct ntp_packet* reply = (const struct ntp_packet*)buf;

    uint8_t mode = reply->li_vn_mode & 0x07;
    if (mode != 4) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP: invalid reply mode=%u", (unsigned)mode);
        return -1;
    }

    int stratum = reply->stratum;
    if (stratum == 0) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP: server returned stratum=0 (no time sample)");
        return -1;
    }

    if (be64toh(reply->orig_ts) != t1_ntp) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP: stale/unmatched reply: originate=%llu expected=%llu",
                    (unsigned long long)be64toh(reply->orig_ts), (unsigned long long)t1_ntp);
        return -1;
    }

    struct timeval t4_tv;
#ifdef _WIN32
    utun_gettimeofday(&t4_tv, NULL);
#else
    gettimeofday(&t4_tv, NULL);
#endif
    int64_t t1_us = ntp64_to_us(t1_ntp);
    int64_t t2_us = ntp64_to_us(be64toh(reply->recv_ts));
    int64_t t3_us = ntp64_to_us(be64toh(reply->xmit_ts));
    int64_t t4_us = ntp64_to_us(timeval_to_ntp(&t4_tv));

    int64_t offset_us = ((t1_us - t2_us) + (t4_us - t3_us)) / 2; /* local - reference */
    int64_t rtt_us    = (t4_us - t1_us) - (t3_us - t2_us);

    *offset_us_out = offset_us;
    *rtt_us_out    = rtt_us;
    *stratum_out   = stratum;
    return 0;
}

/* ─── машина состояний ─── */

static uint16_t ntp_query_port(const struct NTP_TIME* ntp) {
    return ntp->test_port ? ntp->test_port : NTP_PORT;
}

static int ntp_schedule_sync(struct UTUN_INSTANCE* inst, int delay_sec) {
    if (delay_sec < 0 || delay_sec > INT_MAX / 10000) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: timer delay out of range: %ds", delay_sec);
        return -1;
    }
    inst->ntp.timer = uasync_set_timeout(inst->ua, delay_sec * 10000, inst, ntp_time_sync_cb, "ntp_sync");
    if (!inst->ntp.timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: failed to schedule sync in %ds; automatic polling stopped", delay_sec);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "NTP: next cycle in %ds", delay_sec);
    return 0;
}

static void ntp_try_next(struct ntp_sync* ctx) {
    struct NTP_TIME* ntp = &ctx->inst->ntp;
    ntp_close_query(ctx);

    while (ctx->servers_left > 0) {
        ctx->servers_left--;
        if (ntp->test_addr.sin_port != 0) {
            ctx->query_idx = -1;
            if (ntp_start_query(ctx, &ntp->test_addr) == 0) return;
            ntp_close_query(ctx);
            continue;
        }
        int idx = ctx->server_idx;
        ctx->query_idx = idx;
        ctx->server_idx = (idx + 1) % ntp->server_count;
        const char* name = ntp->servers[idx];
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "NTP: polling server %d/%d '%s', remaining=%d",
                    idx + 1, ntp->server_count, name, ctx->servers_left);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(ntp_query_port(ntp));
        if (inet_pton(AF_INET, name, &addr.sin_addr) == 1) {
            if (ntp_start_query(ctx, &addr) == 0) return;
            ntp_close_query(ctx);
            continue;
        }

        struct adns_opts dopts, *popts = NULL;
        if (ntp->test_dns.sin_port != 0) {
            memset(&dopts, 0, sizeof(dopts));
            dopts.server = ntp->test_dns;
            popts = &dopts;
        }

        ctx->dns_q = adns_resolve(ctx->inst->ua, name, popts, ntp_resolve_done_cb, ctx);
        if (ctx->dns_q) return;
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP: resolve '%s' failed immediately", name);
    }

    DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP: all servers unreachable, retry in %ds", ntp->resync_interval_sec);
    ntp_finish_cycle(ctx, 0);
}

static void ntp_resolve_done_cb(const struct adns_result* res, void* arg) {
    struct ntp_sync* ctx = (struct ntp_sync*)arg;
    ctx->dns_q = NULL;
    struct NTP_TIME* ntp = &ctx->inst->ntp;
    int idx = ctx->query_idx;

    if (res->status == ADNS_OK && res->count > 0) {
        struct sockaddr_in addr = res->addrs[0];
        addr.sin_port = htons(ntp_query_port(ntp));
        if (ntp_start_query(ctx, &addr) == 0) return;
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP: resolve '%s' failed (status=%d)", ntp->servers[idx], res->status);
    }
    ntp_try_next(ctx);
}

static int ntp_start_query(struct ntp_sync* ctx, const struct sockaddr_in* addr) {
    ctx->qaddr = *addr;
    ctx->tries = 0;
    return ntp_send_request(ctx);
}

static int ntp_send_request(struct ntp_sync* ctx) {
    struct UTUN_INSTANCE* inst = ctx->inst;
    const char* name = ctx->query_idx >= 0 ? inst->ntp.servers[ctx->query_idx] : "test-server";

    if (ctx->sock == SOCKET_INVALID) {
        ctx->sock = socket_create_udp(AF_INET);
        if (ctx->sock == SOCKET_INVALID) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: socket() failed for %s: %s", name, socket_strerror(socket_get_error()));
            return -1;
        }
        if (socket_set_nonblocking(ctx->sock) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: nonblocking mode failed for %s: %s", name, socket_strerror(socket_get_error()));
            return -1;
        }
    }

    ntp_build_request(ctx->req, &ctx->t1_ntp);
    ctx->tries++;

    ssize_t sent = socket_sendto(ctx->sock, ctx->req, sizeof(ctx->req),
                                 (const struct sockaddr*)&ctx->qaddr, sizeof(ctx->qaddr));
    if (sent != (ssize_t)sizeof(ctx->req)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: sendto failed/short for %s: sent=%lld error=%s",
                    name, (long long)sent, sent < 0 ? socket_strerror(socket_get_error()) : "short datagram");
        return -1;
    }

    if (!ctx->socket_id) {
        ctx->socket_id = uasync_add_socket_t(inst->ua, ctx->sock, ntp_query_read_cb, NULL, NULL, "ntp_query", ctx);
        if (!ctx->socket_id) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: uasync_add_socket_t failed for %s", name);
            return -1;
        }
    }

    ctx->timer = uasync_set_timeout(inst->ua, NTP_TIMEOUT_SEC * 10000, ctx, ntp_query_timeout_cb, "ntp_q");
    if (!ctx->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: request timer failed for %s", name);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "NTP: sent request server=%s attempt=%d/%d timestamp=%llu",
                name, ctx->tries, NTP_MAX_TRIES, (unsigned long long)ctx->t1_ntp);
    return 0;
}

static void ntp_query_read_cb(socket_t sock, void* arg) {
    (void)sock;
    struct ntp_sync* ctx = (struct ntp_sync*)arg;
    struct ntp_packet reply;
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = socket_recvfrom(ctx->sock, (void*)&reply, sizeof(reply), (struct sockaddr*)&from, &from_len);
    if (n < 0) {
        int error = socket_get_error();
        if (error != ERR_AGAIN && error != ERR_WOULDBLOCK)
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP: recvfrom failed server=%d: %s", ctx->query_idx, socket_strerror(error));
        return;
    }

    int64_t offset_us, rtt_us;
    int stratum;
    if (ntp_parse_reply((const uint8_t*)&reply, (size_t)n, ctx->t1_ntp, &offset_us, &rtt_us, &stratum) != 0) return;

    struct NTP_TIME* ntp = &ctx->inst->ntp;
    ntp_time_record_offset(ctx->inst, NTP_TIME_SERVER, offset_us);
    if (ctx->query_idx >= 0)
        ntp->server_current = (ctx->query_idx + 1) % ntp->server_count;

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP: synced from %s offset=%lldus rtt=%lldus stratum=%d",
               ctx->query_idx >= 0 ? ntp->servers[ctx->query_idx] : "test-server",
               (long long)offset_us, (long long)rtt_us, stratum);

    ntp_finish_cycle(ctx, 1);
}

static void ntp_query_timeout_cb(void* arg) {
    struct ntp_sync* ctx = (struct ntp_sync*)arg;
    ctx->timer = NULL;
    const char* name = ctx->query_idx >= 0 ? ctx->inst->ntp.servers[ctx->query_idx] : "test-server";
    DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP: response timeout server=%s attempt=%d/%d", name, ctx->tries, NTP_MAX_TRIES);
    if (ctx->tries < NTP_MAX_TRIES) {
        if (ntp_send_request(ctx) == 0) return;
    }
    ntp_try_next(ctx);
}

static void ntp_close_query(struct ntp_sync* ctx) {
    if (ctx->timer) { uasync_cancel_timeout(ctx->inst->ua, ctx->timer); ctx->timer = NULL; }
    if (ctx->socket_id) { uasync_remove_socket_t(ctx->inst->ua, ctx->sock); ctx->socket_id = NULL; }
    if (ctx->sock != SOCKET_INVALID) { socket_close_wrapper(ctx->sock); ctx->sock = SOCKET_INVALID; }
}

static void ntp_finish_cycle(struct ntp_sync* ctx, int synced) {
    struct UTUN_INSTANCE* inst = ctx->inst;
    struct NTP_TIME* ntp = &inst->ntp;

    ntp_close_query(ctx);
    if (ctx->dns_q) { adns_cancel(ctx->dns_q); ctx->dns_q = NULL; }

    ntp->sync = NULL;
    u_free(ctx);

    if (synced) {
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP: time corrected, offset=%lldus", (long long)ntp->offset_us);
    }

    /* Доступность интернет-NTP меняет классификацию сокетов (LOCAL при оффлайне) */
    {   int prev = ntp->reachable;
        ntp->reachable = synced ? 1 : 0;
        if (prev != ntp->reachable) {
            DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP: reachability changed %d -> %d, updating nodeinfo", prev, ntp->reachable);
            topo_node_update_my_addresses(inst);
        }
    }

    if (synced) ntp_release_startup(inst, "NTP reply");
    ntp_node_sync_peers(inst);
    ntp_schedule_sync(inst, ntp->resync_interval_sec);
}

static void ntp_time_sync_cb(void* arg) {
    struct UTUN_INSTANCE* instance = (struct UTUN_INSTANCE*)arg;
    if (!instance) return;
    struct NTP_TIME* ntp = &instance->ntp;
    ntp->timer = NULL;

    if (!ntp->enabled || ntp->server_count == 0) {
        return;
    }

    if (ntp->sync) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP: sync already in progress, skipping");
        return;
    }

    struct ntp_sync* ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: calloc sync ctx failed");
        ntp_schedule_sync(instance, ntp->resync_interval_sec);
        return;
    }
    ctx->inst = instance;
    ctx->sock = SOCKET_INVALID;
    ntp->sync = ctx;

    ctx->server_idx = ntp->server_current;
    ctx->servers_left = ntp->test_addr.sin_port ? 1 : ntp->server_count;
    ntp_try_next(ctx);
}

int ntp_time_init(struct UTUN_INSTANCE* instance) {
    if (!instance || !instance->config || !instance->ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: init requires instance, config and uasync");
        return -1;
    }
    struct NTP_TIME* ntp = &instance->ntp;
    if (ntp->servers || ntp->timer || ntp->sync || ntp->startup_timer || ntp->restore) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: destroy previous state before init");
        return -1;
    }

    struct global_config* g = &instance->config->global;
    ntp->enabled             = g->ntp_enabled;
    ntp->synced              = 0;
    ntp->reachable           = 1;  /* оптимистично: до первого результата считаем, что интернет есть */
    ntp->offset_us           = 0;
    ntp->last_sync_tb        = 0;
    ntp->source              = NTP_TIME_LOCAL;
    ntp->startup_wait        = 0;
    memset(ntp->offsets, 0, sizeof(ntp->offsets));
    ntp->timer               = NULL;
    ntp->servers             = NULL;
    ntp->server_count        = g->ntp_server_count;
    ntp->server_current      = 0;
    ntp->resync_interval_sec = g->ntp_resync_interval;
    ntp->sync                = NULL;
    ntp->test_port           = 0;
    memset(&ntp->test_addr, 0, sizeof(ntp->test_addr));
    memset(&ntp->test_dns, 0, sizeof(ntp->test_dns));

    if (g->ntp_server_count < 0 || (ntp->enabled && ntp->server_count > 0 &&
        (ntp->resync_interval_sec <= 0 || ntp->resync_interval_sec > INT_MAX / 10000))) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: invalid config: servers=%d interval=%ds",
                    ntp->server_count, ntp->resync_interval_sec);
        goto fail;
    }
    if (g->ntp_server_count > 0) {
        ntp->servers = u_calloc(g->ntp_server_count, sizeof(char*));
        if (!ntp->servers) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "NTP: failed to allocate servers array");
            goto fail;
        }
        struct CFG_NTP_SERVER *ns = g->ntp_servers;
        int i = 0;
        while (i < g->ntp_server_count) {
            if (!ns || !ns->name[0]) {
                DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: missing/empty server at index %d", i);
                goto fail;
            }
            ntp->servers[i] = u_strdup(ns->name);
            if (!ntp->servers[i]) {
                DEBUG_ERROR(DEBUG_CATEGORY_SYS, "NTP: failed to duplicate server name");
                goto fail;
            }
            i++;
            ns = ns->next;
        }
    }

    int polling = ntp->enabled && ntp->server_count > 0;
    if (polling || instance->db_worker) {
        ntp->startup_timer = uasync_set_timeout(instance->ua, NTP_STARTUP_WAIT_SEC * 10000,
            instance, ntp_startup_timeout, "ntp_startup");
        if (!ntp->startup_timer) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: startup deadline allocation failed"); goto fail; }
        ntp->startup_wait = 1;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP: group exchange held for offsets/NTP, at most %d seconds", NTP_STARTUP_WAIT_SEC);
    }
    if (polling) {
        if (ntp_schedule_sync(instance, NTP_FIRST_SYNC_DELAY_SEC) != 0) goto fail;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP: initialized, %d servers, interval=%ds, first sync immediately",
            ntp->server_count, ntp->resync_interval_sec);
    } else DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP: internet polling disabled or no servers configured");
    if (ntp_restore_offsets(instance)) goto fail;
    return 0;
fail:
    ntp_time_destroy(instance);
    return -1;
}

void ntp_time_destroy(struct UTUN_INSTANCE* instance) {
    if (!instance) return;
    struct NTP_TIME* ntp = &instance->ntp;

    if (ntp->startup_timer && instance->ua) uasync_cancel_timeout(instance->ua, ntp->startup_timer);
    ntp->startup_timer = NULL; ntp->startup_wait = 0; ntp->restore = NULL;
    if (ntp->sync && instance->ua) {
        struct ntp_sync* ctx = (struct ntp_sync*)ntp->sync;
        ntp_close_query(ctx);
        if (ctx->dns_q) adns_cancel(ctx->dns_q);
        u_free(ctx);
        ntp->sync = NULL;
    }

    if (ntp->timer && instance->ua) {
        uasync_cancel_timeout(instance->ua, ntp->timer);
        ntp->timer = NULL;
    }
    if (ntp->servers) {
        for (int i = 0; i < ntp->server_count; i++) u_free(ntp->servers[i]);
        u_free(ntp->servers);
        ntp->servers = NULL;
    }
    ntp->enabled = 0;
    ntp->server_count = 0;
    ntp->server_current = 0;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NTP: destroyed (synced=%d, offset=%lldus)",
               ntp->synced, (long long)ntp->offset_us);
}

int64_t ntp_time_get_us(struct UTUN_INSTANCE* instance) {
    if (!instance || !instance->ntp.synced) {
        struct timeval tv;
#ifdef _WIN32
        utun_gettimeofday(&tv, NULL);
#else
        gettimeofday(&tv, NULL);
#endif
        return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
    }
    struct timeval tv;
#ifdef _WIN32
    utun_gettimeofday(&tv, NULL);
#else
    gettimeofday(&tv, NULL);
#endif
    int64_t local_us = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
    return local_us - instance->ntp.offset_us;
}

time_t ntp_time_get_seconds(struct UTUN_INSTANCE* instance) {
    return (time_t)(ntp_time_get_us(instance) / 1000000LL);
}

int ntp_time_is_synced(struct UTUN_INSTANCE* instance) {
    return instance && instance->ntp.synced;
}

void ntp_time_set_test_addr(struct NTP_TIME* ntp, const char* ip, uint16_t port) {
    if (!ntp || !ip) return;
    memset(&ntp->test_addr, 0, sizeof(ntp->test_addr));
    ntp->test_addr.sin_family = AF_INET;
    ntp->test_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &ntp->test_addr.sin_addr) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: test addr parse failed for %s", ip);
        memset(&ntp->test_addr, 0, sizeof(ntp->test_addr));
    }
}

void ntp_time_set_test_dns(struct NTP_TIME* ntp, const char* ip, uint16_t dns_port, uint16_t ntp_port) {
    if (!ntp || !ip) return;
    memset(&ntp->test_dns, 0, sizeof(ntp->test_dns));
    ntp->test_dns.sin_family = AF_INET;
    ntp->test_dns.sin_port = htons(dns_port);
    ntp->test_port = ntp_port;
    if (inet_pton(AF_INET, ip, &ntp->test_dns.sin_addr) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NTP: test dns parse failed for %s", ip);
        memset(&ntp->test_dns, 0, sizeof(ntp->test_dns));
        ntp->test_port = 0;
    }
}
