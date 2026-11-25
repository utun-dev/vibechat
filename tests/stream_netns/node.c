/* Агент стенда: настоящие JOIN/Merkle/ETCP и автоматический выбор поставщиков.
 * stdin/stdout — только JSONL RPC; диагностика ядра пишется в отдельный файл.
 * Заголовок нагрузки и SHA256 проверяют opaque DATA после восстановления FEC. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <openssl/sha.h>

#include "../../lib/debug_config.h"
#include "../../lib/json_flat.h"
#include "../../lib/mem.h"
#include "../../src/chat/chat_core.h"
#include "../../src/chat/chat_event.h"
#include "../../src/chat/chat_sync.h"
#include "../../src/chat/invite_build.h"
#include "../../src/chat/invite_link.h"
#include "../../src/stream/stream_priv.h"
#include "../../src/stream/stream_member.h"
#include "../../src/routing_layer/nat_detection.h"
#include "../../src/transport_layer/stun_nat.h"
#include "../../src/routing_layer/route_ping.h"
#include "../../src/routing_layer/topo_node_sqlite.h"
#include "../../src/transport_layer/node_conn_direct.h"
#include "../../src/transport_layer/etcp.h"
#include "../../src/utun_instance.h"

#define LAB_STREAMS 4
#define LAB_HISTOGRAM 60001
#define LAB_PAYLOAD_HEADER 72

struct lab_stream {
    struct STREAM_HANDLE *handle;
    struct stream_address source;
    uint64_t generated, received, bytes, gaps, gap_batches, errors, ends, closed, next, first, latency_max;
    uint64_t latency_sum, histogram[LAB_HISTOGRAM], last_data_ns, start_seq;
    uint64_t seed, interval_ns, due_ns;
    uint8_t *payload;
    size_t size;
    int publisher, started;
};
struct lab {
    struct UTUN_INSTANCE *inst;
    struct lab_stream streams[LAB_STREAMS];
    struct chat_invite_build *invite;
    uint64_t invite_id, group;
    struct NODE_CONN_DIRECT *nat_handles[32];
    struct TOPO_NODE *nat_records[32];
    unsigned nat_handle_count, nat_record_count;
    void *cpu_timer;
    uint64_t cpu_until_ns; // Ограниченный реальный вычислительный груз в uasync; метрики ядра не подменяются.
    int errors;
};
static volatile sig_atomic_t stopping;
static struct lab* active_lab;

static void join_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    if (!active_lab || active_lab->inst != inst || type != CHAT_EVT_CONNECT_RESULT || len != CHAT_CONNECT_RESULT_SIZE) return;
    int result;
    memcpy(&result, data + CHAT_CONNECT_RESULT_CODE_OFF, sizeof(result));
    if (!result) {
        struct TOPO_GROUP* group = topo_groups_find_wire(inst->topo_groups, data + CHAT_CONNECT_RESULT_GROUP_OFF);
        if (group) active_lab->group = group->group_id;
        else { active_lab->errors++; DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: joined group not registered"); }
    }
    else { active_lab->errors++; DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: JOIN failed result=%d", result); }
}

static uint64_t now_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + time.tv_nsec;
}

static void stop_signal(int signal_number) { (void)signal_number; stopping = 1; }

static void cpu_work(void *arg) {
    struct lab *lab = arg; lab->cpu_timer = NULL;
    uint64_t now = now_ns();
    if (now >= lab->cpu_until_ns) {
        lab->cpu_until_ns = 0; DEBUG_INFO(DEBUG_CATEGORY_STREAM, "lab: CPU work completed"); return;
    }
    uint64_t until = now + UINT64_C(90000000);
    if (until > lab->cpu_until_ns) until = lab->cpu_until_ns;
    uint8_t block[1024] = {1}, digest[32];
    do { SHA256(block, sizeof(block), digest); now = now_ns(); } while (now < until);
    lab->cpu_timer = uasync_set_timeout(lab->inst->ua, 100, lab, cpu_work, "lab_cpu_work");
    if (!lab->cpu_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: CPU timer allocation failed"); lab->errors++; lab->cpu_until_ns = 0;
    }
}

static void json_string(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static void reply(uint64_t id, int result) {
    if (result < 0) DEBUG_WARN(DEBUG_CATEGORY_STREAM, "lab: RPC failed id=%" PRIu64 " rc=%d", id, result);
    printf("{\"id\":%" PRIu64 ",\"rc\":%d}\n", id, result);
}

static int number(const char *json, const char *key, uint64_t *value) {
    char buffer[32], *end;
    if (json_flat_get(json, key, buffer, sizeof(buffer)) || !buffer[0] || buffer[0] == '-') return -1;
    errno = 0;
    uint64_t parsed = strtoull(buffer, &end, 10);
    if (errno || *end) return -1;
    *value = parsed;
    return 0;
}

static struct lab_stream *slot(struct lab *lab, struct stream_address source, int create) {
    for (unsigned i = 0; i < LAB_STREAMS; i++) if (stream_address_equal(lab->streams[i].source, source)) return &lab->streams[i];
    for (unsigned i = 0; create && i < LAB_STREAMS; i++) {
        if (!stream_address_set(lab->streams[i].source)) { lab->streams[i].source = source; return &lab->streams[i]; }
    }
    return NULL;
}

static void event(struct UTUN_INSTANCE *inst, const struct stream_event *event, void *arg) {
    (void)inst;
    struct lab *lab = arg;
    struct lab_stream *stream = slot(lab, event->descriptor.source, 0);
    if (!stream) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: event without owner source=%s", STREAM_ADDRESS_TEXT(event->descriptor.source));
        lab->errors++; return;
    }
    if (event->type == STREAM_EVENT_CLOSED) { stream->closed++; return; }
    if (event->type == STREAM_EVENT_END) { stream->ends++; return; }
    if (event->type == STREAM_EVENT_GAP) {
        if (!stream->started) stream->start_seq = event->first_seq;
        stream->gaps++; stream->gap_batches += event->last_seq - event->first_seq + 1;
        if (stream->started && event->first_seq != stream->next) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: GAP cursor expected=%" PRIu64 " first=%" PRIu64, stream->next, event->first_seq);
            stream->errors++;
        }
        stream->started = 1; stream->next = event->last_seq + 1;
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "lab: GAP source=%s first=%" PRIu64 " last=%" PRIu64,
                   STREAM_ADDRESS_TEXT(stream->source), event->first_seq, event->last_seq);
        return;
    }
    const uint8_t *data = event->data;
    uint8_t digest[32];
    int valid = event->length >= LAB_PAYLOAD_HEADER + sizeof(digest) && !memcmp(data, "SLABv002", 8);
    if (valid) {
        SHA256(data, event->length - 32, digest);
        valid = !memcmp(digest, data + event->length - 32, 32) && stream_read_be(data + 8, 8) == event->first_seq &&
                !memcmp(data + 32, stream->source.ed, 32) && stream_read_be(data + 64, 4) == event->length;
    }
    if (!valid || (stream->started && event->first_seq != stream->next)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: DATA validation source=%s seq=%" PRIu64
                    " expected=%" PRIu64 " bytes=%zu checksum=%d", STREAM_ADDRESS_TEXT(stream->source), event->first_seq, stream->next, event->length, valid);
        stream->errors++;
    }
    if (!stream->started) stream->start_seq = event->first_seq;
    if (!stream->received) stream->first = event->first_seq;
    stream->started = 1; stream->next = event->last_seq + 1;
    stream->received++; stream->bytes += event->length; stream->last_data_ns = now_ns();
    if (valid) {
        uint64_t generated_ns = stream_read_be(data + 16, 8);
        if (generated_ns > stream->last_data_ns) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: CLOCK_MONOTONIC moved backwards"); stream->errors++; return;
        }
        uint64_t delay = (stream->last_data_ns - generated_ns) / UINT64_C(1000000);
        stream->latency_sum += delay;
        if (delay > stream->latency_max) stream->latency_max = delay;
        stream->histogram[delay < LAB_HISTOGRAM ? delay : LAB_HISTOGRAM - 1]++;
    }
}

static uint64_t percentile(const struct lab_stream *stream, unsigned percent) {
    uint64_t target = (stream->received * percent + 99) / 100, count = 0;
    if (!target) return 0;
    for (unsigned i = 0; i < LAB_HISTOGRAM; i++) { count += stream->histogram[i]; if (count >= target) return i; }
    return LAB_HISTOGRAM - 1;
}

static void statistics(struct lab *lab, uint64_t id, int loads) {
    struct TOPO_GROUP *group = topo_groups_find(lab->inst->topo_groups, lab->group);
    struct timespec cpu = {0};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: process CPU clock failed: %s", strerror(errno)); lab->errors++;
    }
    uint64_t cpu_ns = (uint64_t)cpu.tv_sec * UINT64_C(1000000000) + cpu.tv_nsec;
    struct uasync_load load = {0};
    if (uasync_get_load(lab->inst->ua, &load) != ERR_OK) lab->errors++;
    struct stream_address self = stream_address_read(lab->inst->my_ed25519_pubkey);
    printf("{\"id\":%" PRIu64 ",\"rc\":0,\"node\":\"%s\",\"group\":\"%" PRIu64
           "\",\"errors\":%d,\"sample_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64
           ",\"uasync_cpu_pct\":%.3f,\"uasync_cpu_valid\":%d,\"online\":[",
           id, STREAM_ADDRESS_TEXT(self), lab->group, lab->errors, now_ns(), cpu_ns, load.average_percent, load.valid);
    int separator = 0;
    if (stream_member_online(lab->inst, lab->group, self)) {
        printf("\"%s\"", STREAM_ADDRESS_TEXT(self)); separator = 1;
    }
    for (struct ll_entry *e = group ? group->nodes->head : NULL; e; e = e->next) {
        struct TOPO_GROUP_NODE *node = (struct TOPO_GROUP_NODE *)e;
        struct stream_address peer = stream_address_read(node->ed25519);
        if (stream_member_online(lab->inst, lab->group, peer)) {
            printf("%s\"%s\"", separator++ ? "," : "", STREAM_ADDRESS_TEXT(peer));
        }
    }
    printf("],\"profile_status\":{"); separator = 0;
    if (group) {
        struct stream_member_state state;
        printf("\"%s\":%d", STREAM_ADDRESS_TEXT(self),
               stream_member_load(lab->inst, lab->group, self, &state, NULL)); separator = 1;
        for (struct ll_entry *e = group->nodes->head; e; e = e->next) {
            struct TOPO_GROUP_NODE *node = (struct TOPO_GROUP_NODE *)e;
            struct stream_address peer = stream_address_read(node->ed25519);
            printf("%s\"%s\":%d", separator++ ? "," : "", STREAM_ADDRESS_TEXT(peer),
                   stream_member_load(lab->inst, lab->group, peer, &state, NULL));
        }
    }
    printf("},\"address_profiles\":["); separator = 0;
    for (struct ETCP_SOCKET *socket = lab->inst->etcp_sockets; socket; socket = socket->next) {
        if (socket->probe_only) continue;
        struct nat_check_status status = {0}; nat_detection_get_status(lab->inst->nat_det, socket, &status);
        printf("%s{\"socket\":", separator++ ? "," : ""); json_string(socket->name);
        printf(",\"socket_id\":%u,\"generation\":\"%" PRIu64 "\",\"protocol\":\"%s\",\"local\":",
               socket->sock_id, socket->generation, socket->is_tcp ? "tcp" : "udp");
        json_string(sockaddr_storage_to_str(socket->is_tcp ? &socket->interface_addr : &socket->local_addr).str);
        printf(",\"source\":\"%s\"", address_profile_source_name(socket->address_profile.source));
        printf(",\"mapping\":\"%s\",\"inbound\":\"%s\",\"external\":",
               address_mapping_name(socket->address_profile.mapping), address_inbound_name(socket->address_profile.inbound));
        const struct sockaddr_storage *external = &socket->nat_addr;
        if (!external->ss_family && socket->address_profile.mapping == ADDRESS_MAP_REAL_IP &&
            socket->address_profile.source == ADDRESS_SOURCE_ETCP)
            external = socket->is_tcp ? &socket->interface_addr : &socket->local_addr;
        json_string(external->ss_family ? sockaddr_storage_to_str(external).str : "");
        struct stun_nat_result stun;
        if (!stun_nat_get_socket_result(lab->inst, socket, &stun)) {
            printf(",\"stun\":{\"members\":%u,\"generation\":\"%" PRIu64 "\",\"evidence\":\"%" PRIu64 "\",\"test_local\":",
                   stun.members, stun.generation, stun.evidence_id);
            json_string(sockaddr_storage_to_str(&stun.local).str);
            printf(",\"status\":%d,\"mapping\":\"%s\",\"filtering\":\"%s\"}", stun.status,
                   stun_nat_mapping_name(stun.mapping), stun_nat_filtering_name(stun.filtering));
        }
        printf(",\"phase\":%u,\"reason\":\"%s\",\"group\":\"%" PRIu64 "\",\"observer\":\"%s\",\"helper\":\"%s\","
               "\"rounds\":%u,\"helper_reselections\":%u,\"cycling\":%u,\"history_complete\":%u,\"contacts\":[", status.phase,
               nat_check_reason_name(status.reason), status.group_id, sc_public_key_hex(status.observer, (char[65]){0}), sc_public_key_hex(status.helper, (char[65]){0}), status.rounds,
               status.helper_reselections, status.cycling, status.history_complete);
        uint8_t local[ROUTE_ADDR_WIRE_SIZE]; sqlite3_stmt *contacts = NULL; int contact_separator = 0;
        if (!socket->is_tcp && !route_ping_addr_encode(local, &socket->local_addr) &&
            sqlite3_prepare_v2(lab->inst->topo_sqlite_db, "SELECT remote FROM nat_contacts WHERE local=?",
                              -1, &contacts, NULL) == SQLITE_OK) {
            sqlite3_bind_blob(contacts, 1, local, sizeof(local), SQLITE_TRANSIENT);
            while (sqlite3_step(contacts) == SQLITE_ROW) {
                const uint8_t *address = sqlite3_column_blob(contacts, 0); char text[64];
                if (!address || sqlite3_column_bytes(contacts, 0) != 17 || (address[0] != 4 && address[0] != 6)) continue;
                if (!inet_ntop(address[0] == 4 ? AF_INET : AF_INET6, address + 1, text, sizeof(text))) continue;
                if (contact_separator++) printf(",");
                json_string(text);
            }
        }
        sqlite3_finalize(contacts); printf("]}");
    }
    printf("],\"direct_up\":["); separator = 0;
    for (struct ll_entry *entry = lab->inst->connections->head; entry; entry = entry->next) {
        const struct ETCP_CONN *connection = ((const struct conn_queue_entry *)entry->data)->conn;
        if (connection && connection->links_up && !connection->close_requested && connection->state != 2)
            printf("%s\"%s\"", separator++ ? "," : "", sc_public_key_hex(connection->peer_ed25519_pubkey, (char[65]){0}));
    }
    printf("],\"group_ready\":["); separator = 0;
    struct TOPO_GROUP *nat_group = topo_groups_find(lab->inst->topo_groups, TOPO_GROUP_UTUN);
    for (struct ll_entry *entry = nat_group ? lab->inst->connections->head : NULL; entry; entry = entry->next) {
        const struct ETCP_CONN *connection = ((const struct conn_queue_entry *)entry->data)->conn;
        if (topo_group_connection_ready(nat_group, connection))
            printf("%s\"%s\"", separator++ ? "," : "", sc_public_key_hex(connection->peer_ed25519_pubkey, (char[65]){0}));
    }
    printf("],\"streams\":["); separator = 0;
    for (unsigned i = 0; i < LAB_STREAMS; i++) {
        struct lab_stream *stream = &lab->streams[i];
        if (!stream_address_set(stream->source)) continue;
        printf("%s{\"source\":\"%s\",\"publisher\":%d,\"generated\":%" PRIu64 ",\"received\":%" PRIu64
               ",\"bytes\":%" PRIu64 ",\"gaps\":%" PRIu64 ",\"gap_batches\":%" PRIu64 ",\"errors\":%" PRIu64
               ",\"ends\":%" PRIu64 ",\"closed\":%" PRIu64 ",\"first_seq\":%" PRIu64 ",\"next_seq\":%" PRIu64
               ",\"start_seq\":%" PRIu64 ",\"age_ms\":%" PRIu64 ",\"latency_p50_ms\":%" PRIu64 ",\"latency_p95_ms\":%" PRIu64
               ",\"latency_p99_ms\":%" PRIu64 ",\"latency_max_ms\":%" PRIu64 ",\"latency_limit_ms\":%u",
               separator++ ? "," : "", STREAM_ADDRESS_TEXT(stream->source), stream->publisher, stream->generated, stream->received,
               stream->bytes, stream->gaps, stream->gap_batches, stream->errors, stream->ends, stream->closed,
               stream->first, stream->next, stream->start_seq,
               stream->last_data_ns ? (now_ns() - stream->last_data_ns) / UINT64_C(1000000) : 0,
               percentile(stream, 50), percentile(stream, 95), percentile(stream, 99), stream->latency_max, LAB_HISTOGRAM - 1);
        struct stream_info info;
        if (stream->handle && stream_get_info(stream->handle, &info) == STREAM_OK) {
            struct stream_selection_info selection;
            memset(&selection, 0, sizeof(selection));
            stream_selection_get_info(stream->handle, &selection);
            printf(",\"active\":%d,\"epoch\":\"%" PRIu64 "\",\"stored_bytes\":%zu,\"capacity_bytes\":%zu"
                   ",\"evicted_batches\":%" PRIu64 ",\"subflows\":%u,\"providers\":%u,\"probes\":%" PRIu64
                   ",\"cycles\":%" PRIu64 ",\"migrations\":%" PRIu64 ",\"failures\":%" PRIu64 ",\"restarts\":%" PRIu64
                   ",\"protected\":%d,\"n\":%u,\"r\":%u,\"phase\":%d,\"working_parts\":%u,\"maximum_dependency\":%u,\"paths\":[",
                   stream_handle_active(stream->handle), info.descriptor.source_epoch,
                   info.buffer.stored_bytes, info.buffer.capacity_bytes, info.buffer.evicted_batches, selection.subflows,
                   selection.providers, selection.probes_started, selection.cycles, selection.migrations, selection.failures,
                   selection.restarts, selection.protected_by_fec, info.descriptor.n, info.descriptor.r, selection.phase,
                   selection.working_parts, selection.maximum_dependency);
            struct stream_local local;
            int path_separator = 0;
            if (stream_local_get(lab->inst, &info.descriptor, &local) == STREAM_OK) {
                for (uint8_t sub = 0; sub < info.descriptor.n; sub++) {
                    uint64_t first, last;
                    struct stream_path path;
                    if (stream_buffer_range(local.buffer, sub, &first, &last) != STREAM_OK ||
                        stream_buffer_path(local.buffer, last, sub, &path) != STREAM_OK) continue;
                    printf("%s{\"sub\":%u,\"seq\":%" PRIu64 ",\"nodes\":[", path_separator++ ? "," : "", sub, last);
                    for (unsigned p = 0; p < path.count; p++) printf("%s\"%s\"", p ? "," : "", STREAM_ADDRESS_TEXT(path.nodes[p]));
                    printf("]");
                    const uint8_t *wire; size_t length; uint64_t actual;
                    if (stream_buffer_read(local.buffer, last, sub, &wire, &length, &actual) == STREAM_OK)
                        printf(",\"source_timestamp_ms\":%u", (unsigned)stream_read_be(wire + STREAM_BLOCK_TIMESTAMP_OFF, 2));
                    struct stream_metric_snapshot metric;
                    if (stream_local_metrics(lab->inst, &info.descriptor, sub, get_time_tb() / 10, &metric) == STREAM_OK)
                        printf(",\"lag_ms\":%" PRIu64 ",\"source_rate\":%" PRIu64 ",\"receive_rate\":%" PRIu64
                               ",\"late_blocks\":%" PRIu64 ",\"stops\":%" PRIu64 ",\"stopped\":%d",
                               metric.lag_ms, metric.source_rate_bytes_sec, metric.receive_rate_bytes_sec,
                               metric.late_blocks, metric.stops, metric.stopped);
                    printf("}");
                }
            }
            printf("]");
        }
        printf("}");
    }
    printf("],\"peer_load\":["); separator = 0;
    for (struct ll_entry *e = loads && group ? group->nodes->head : NULL; e; e = e->next) {
        struct TOPO_GROUP_NODE *node = (struct TOPO_GROUP_NODE *)e;
        struct stream_address peer = stream_address_read(node->ed25519);
        if (stream_address_equal(peer, self)) continue;
        struct stream_peer_load_info load;
        if (stream_peer_get_load(lab->inst, peer, &load) != STREAM_OK) continue;
        printf("%s{\"peer\":\"%s\",\"queued_bytes\":%" PRIu64 ",\"growth_rate\":%" PRIu64
               ",\"transport_bytes\":%" PRIu64 ",\"inflight_bytes\":%" PRIu64 ",\"pressure\":%d,\"confirmed\":%d"
               ",\"global_pressure\":%d,\"accepting\":%d,\"overload_id\":%" PRIu64
               ",\"released_count\":%u,\"released_rate\":%" PRIu64 ",\"rejected\":%" PRIu64 "}",
               separator++ ? "," : "", STREAM_ADDRESS_TEXT(peer), load.load.queued.bytes, load.load.growth_rate,
               load.transport_queued_bytes, load.inflight_bytes, load.load.pressure, load.load.confirmed,
               load.global_pressure, load.accepting, load.request_id, load.released_count, load.released_rate, load.load.rejected);
    }
    printf("]}\n");
}

static void invite_ready(void *arg, int result, const char *link) {
    struct lab *lab = arg;
    lab->invite = NULL;
    if (result) DEBUG_WARN(DEBUG_CATEGORY_STREAM, "lab: invite failed id=%" PRIu64 " rc=%d", lab->invite_id, result);
    printf("{\"id\":%" PRIu64 ",\"rc\":%d,\"link\":", lab->invite_id, result);
    json_string(link ? link : ""); printf("}\n");
}

static struct TOPO_NODE *parse_snapshot(struct lab *lab, const char *json) {
    char hex[4097]; uint8_t wire[2048]; size_t length;
    if (json_flat_get(json, "snapshot", hex, sizeof(hex)) || !(length = strlen(hex)) || length % 2 || length / 2 > sizeof(wire)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "lab: invalid node snapshot size"); return NULL;
    }
    for (size_t i = 0; i < length / 2; i++) {
        unsigned value;
        if (sscanf(hex + i * 2, "%2x", &value) != 1) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "lab: invalid snapshot hex"); return NULL; }
        wire[i] = (uint8_t)value;
    }
    struct TOPO_GROUP group = {.instance = lab->inst};
    struct TOPO_NODE *node = NULL;
    if (topo_node_deserialize(&group, wire, length / 2, &node, NULL, NULL, NULL, NULL) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "lab: snapshot decode failed"); return NULL;
    }
    if (!memcmp(node->public_key, lab->inst->my_keys.public_key, 32) || topo_node_verify(node) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "lab: snapshot signature/identity rejected");
        topo_node_destroy(lab->inst->topo_groups, node); return NULL;
    }
    return node;
}

/* Стенд переносит подписанные снимки через RPC только до запуска группы.
 * NCD handshake, наблюдения и весь трафик всё равно проходят по настоящей сети. */
/* NAT-стенд имеет настоящую общую группу; UP прямого транспорта начинает групповой обмен. */
static void nat_link_ready(struct NODE_CONN_DIRECT *handle, enum ncd_event event, void *arg) {
    struct lab *lab = arg;
    if (event != NCD_EVENT_UP) return;
    struct TOPO_GROUP *group = topo_groups_find(lab->inst->topo_groups, TOPO_GROUP_UTUN);
    struct ETCP_CONN *connection = node_conn_direct_get_conn(handle);
    if (!group || !connection || topo_group_new_conn(group, connection))
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "lab: NAT group connection unavailable");
}

static int nat_command(struct lab *lab, const char *json, const char *operation, uint64_t id) {
    if (!strcmp(operation, "chat_start")) {
        int result = chat_service_start(lab->inst);
        if (!result) stream_set_event_callback(lab->inst, event, lab);
        reply(id, result); return 1;
    }
    if (!strcmp(operation, "nat_release")) {
        if (topo_groups_find(lab->inst->topo_groups, TOPO_GROUP_UTUN)) topo_groups_remove_group(lab->inst->topo_groups, TOPO_GROUP_UTUN);
        for (unsigned i = 0; i < lab->nat_handle_count; i++) node_conn_direct_close(lab->nat_handles[i]);
        for (unsigned i = 0; i < lab->nat_record_count; i++) topo_node_registry_release(lab->inst->topo_groups, lab->nat_records[i]);
        lab->nat_handle_count = lab->nat_record_count = 0; reply(id, 0); return 1;
    }
    if (!strcmp(operation, "nat_auto")) {
        uint64_t enabled;
        if (number(json, "enabled", &enabled) || enabled > 1) { reply(id, -1); return 1; }
        nat_detection_set_automatic(lab->inst->nat_det, enabled); reply(id, 0); return 1;
    }
    if (!strcmp(operation, "nat_check")) {
        char name[32], hex[65]; uint8_t observer[32];
        if (!json_flat_get(json, "socket", name, sizeof(name))) {
            if (json_flat_get(json, "observer", hex, sizeof(hex)) || strlen(hex) != 64 || sc_hex_to_binary(hex, observer, 32)) {
                reply(id, -1); return 1;
            }
            struct ETCP_SOCKET *socket = lab->inst->etcp_sockets;
            while (socket && (socket->probe_only || strcmp(socket->name, name))) socket = socket->next;
            reply(id, socket ? nat_detection_request_check_socket(lab->inst->nat_det, socket, observer) : -1);
        } else { nat_detection_request_check_all(lab->inst->nat_det); reply(id, 0); }
        return 1;
    }
    if (!strcmp(operation, "snapshot")) {
        uint8_t wire[2048]; struct TOPO_GROUP_NODE nq = {0};
        struct TOPO_NODE *node = topo_node_registry_lookup(lab->inst->topo_groups, lab->inst->node_id, lab->inst->my_keys.public_key);
        int length = topo_node_serialize(node, &nq, 0, wire, sizeof(wire), 0);
        if (length < 0) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "lab: snapshot serialize failed"); reply(id, -1); return 1; }
        printf("{\"id\":%" PRIu64 ",\"rc\":0,\"snapshot\":\"", id);
        for (int i = 0; i < length; i++) printf("%02x", wire[i]);
        printf("\"}\n"); return 1;
    }
    int store = !strcmp(operation, "nat_store"), link = !strcmp(operation, "nat_link");
    if (!store && !link) return 0;
    struct TOPO_NODE *node = parse_snapshot(lab, json);
    if (!node) { reply(id, -1); return 1; }
    int result = 0;
    if (link) {
        char source[32]; struct ETCP_SOCKET *socket = NULL;
        if (lab->nat_handle_count < 32 && !json_flat_get(json, "socket", source, sizeof(source)))
            for (socket = lab->inst->etcp_sockets; socket; socket = socket->next)
                if (!socket->probe_only && !strcmp(socket->name, source)) break;
        if (!socket) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "lab: native link source/handle unavailable"); result = -1; }
        else {
            struct NODE_CONN_DIRECT *handle = NULL;
            int opened = node_conn_direct_open_node(lab->inst, node->node_id, nat_link_ready, lab, &handle, node, socket);
            if (opened == NCD_ERR || !handle) result = -1;
            else lab->nat_handles[lab->nat_handle_count++] = handle;
        }
        topo_node_destroy(lab->inst->topo_groups, node);
    } else {
        if (lab->nat_record_count == 32) {
            DEBUG_ERROR(DEBUG_CATEGORY_NAT, "lab: imported snapshot limit reached"); result = -1;
            topo_node_destroy(lab->inst->topo_groups, node);
        } else {
            struct TOPO_NODE *stored = topo_node_registry_store(lab->inst->topo_groups, node);
            if (!stored) { topo_node_destroy(lab->inst->topo_groups, node); result = -1; }
            else {
                unsigned i;
                for (i = 0; i < lab->nat_record_count; i++) if (lab->nat_records[i] == stored) break;
                if (i == lab->nat_record_count) lab->nat_records[lab->nat_record_count++] = stored;
                else topo_node_registry_release(lab->inst->topo_groups, stored);
            }
        }
    }
    reply(id, result); return 1;
}

static void command(struct lab *lab, const char *json) {
    uint64_t id = 0, value = 0;
    char operation[32];
    if (number(json, "id", &id) || json_flat_get(json, "op", operation, sizeof(operation))) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: invalid RPC"); reply(id, -1); return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "lab: RPC id=%" PRIu64 " op=%s", id, operation);
    if (!strcmp(operation, "stats")) { number(json, "loads", &value); statistics(lab, id, value != 0); return; }
    if (!strcmp(operation, "quit")) { stopping = 1; reply(id, 0); return; }
    if (!strcmp(operation, "cpu")) {
        if (number(json, "seconds", &value) || value > 30) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: CPU duration must be 0..30 seconds"); reply(id, -1); return;
        }
        if (lab->cpu_timer) uasync_cancel_timeout(lab->inst->ua, lab->cpu_timer);
        lab->cpu_timer = NULL; lab->cpu_until_ns = value ? now_ns() + value * UINT64_C(1000000000) : 0;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "lab: CPU work seconds=%" PRIu64 " busy_ms=90 idle_ms=10", value);
        if (value) lab->cpu_timer = uasync_set_timeout(lab->inst->ua, 1, lab, cpu_work, "lab_cpu_work");
        if (value && !lab->cpu_timer) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: CPU timer allocation failed"); lab->errors++; reply(id, -1); return;
        }
        reply(id, 0); return;
    }
    if (nat_command(lab, json, operation, id)) return;
    if (!strcmp(operation, "create")) {
        if (lab->group) { reply(id, -1); return; }
        chat_core_create_channel_auto(lab->inst, "stream-netns");
        for (struct ll_entry *e = lab->inst->topo_groups->group_list->head; e; e = e->next) {
            struct TOPO_GROUP *group = (struct TOPO_GROUP *)e;
            if (group->group_type == TOPO_GROUP_TYPE_CHAT) { lab->group = group->group_id; break; }
        }
        if (!lab->group) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: CHAT group creation failed id=%" PRIu64, id);
        printf("{\"id\":%" PRIu64 ",\"rc\":%d,\"group\":\"%" PRIu64 "\"}\n", id, lab->group ? 0 : -1, lab->group); return;
    }
    if (!strcmp(operation, "invite")) {
        if (lab->invite || !lab->group) { reply(id, -1); return; }
        lab->invite_id = id;
        lab->invite = chat_invite_build_link(lab->inst, lab->group, lab->inst->my_ed25519_pubkey, invite_ready, lab);
        if (!lab->invite) reply(id, -1);
        return;
    }
    if (!strcmp(operation, "join")) {
        char link[2048], error[256];
        struct InviteData invite;
        uint8_t addresses[2048];
        if (json_flat_get(json, "link", link, sizeof(link)) || invite_link_decode(link, strlen(link), &invite, error, sizeof(error))) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: invalid invite"); reply(id, -1); return;
        }
        int size = invite_serialize_addrs(&invite, addresses, sizeof(addresses));
        if (size <= 0) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: invite has no endpoint"); reply(id, -1); return; }
        if (chat_sync_join_link(lab->inst, 1, link, strlen(link))) { reply(id, -1); return; }
        reply(id, 0); return;
    }
    int publish = !strcmp(operation, "publish");
    struct stream_address source = stream_address_read(lab->inst->my_ed25519_pubkey);
    if (!publish) {
        char hex[65];
        if (json_flat_get(json, "source", hex, sizeof(hex)) || sc_hex_to_binary(hex, source.ed, sizeof(source.ed)) || !stream_address_set(source)) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: invalid Ed25519 source"); reply(id, -1); return;
        }
    }
    struct lab_stream *stream = slot(lab, source, publish || !strcmp(operation, "subscribe"));
    if (!stream) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: no stream slot"); reply(id, -1); return; }
    if (publish || !strcmp(operation, "subscribe")) {
        if (stream->handle) { reply(id, 0); return; }
        uint64_t n = 0, r = 0;
        if (publish && (number(json, "n", &n) || number(json, "r", &r) || !n || n > RS_MAX_BLOCKS || r >= n)) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: invalid FEC parameters n=%" PRIu64 " r=%" PRIu64, n, r); reply(id, -1); return;
        }
        struct stream_parameters parameters = {.n = (uint8_t)n, .r = (uint8_t)r, .block_size = STREAM_RS_DEFAULT_SIZE};
        int result = publish ? stream_publish_open(lab->inst, lab->group, &parameters, &stream->handle) :
                               stream_subscribe_open(lab->inst, lab->group, source, &stream->handle);
        if (result == STREAM_OK && publish) {
            struct stream_info info;
            stream_get_info(stream->handle, &info);
            stream->size = stream_input_size(&info.descriptor);
            stream->payload = u_malloc(stream->size);
            stream->publisher = 1;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "lab: publisher FEC n=%u r=%u useful_batch_bytes=%zu",
                info.descriptor.n, info.descriptor.r, stream->size);
            if (!stream->payload) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: payload allocation failed"); stopping = 1; lab->errors++; }
            if (!number(json, "seed", &value)) stream->seed = value;
        }
        reply(id, result); return;
    }
    if (!strcmp(operation, "rate") && stream->publisher && !number(json, "bps", &value)) {
        stream->interval_ns = value ? stream->size * UINT64_C(8000000000) / value : 0;
        if (value && !stream->interval_ns) stream->interval_ns = 1;
        stream->due_ns = now_ns(); reply(id, 0); return;
    }
    if (!strcmp(operation, "finish") && stream->publisher) {
        stream->interval_ns = 0; reply(id, stream_publish_finish(stream->handle)); return;
    }
    if (!strcmp(operation, "close")) {
        stream->interval_ns = 0;
        if (stream->handle) stream_close(stream->handle);
        stream->handle = NULL; reply(id, 0); return;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: unsupported RPC op=%s", operation); reply(id, -1);
}

static void generate(struct lab *lab) {
    for (unsigned i = 0; i < LAB_STREAMS; i++) {
        struct lab_stream *stream = &lab->streams[i];
        for (unsigned burst = 0; stream->interval_ns && now_ns() >= stream->due_ns && burst < 8; burst++) {
            uint8_t *data = stream->payload;
            memset(data, 0, LAB_PAYLOAD_HEADER); memcpy(data, "SLABv002", 8);
            stream_write_be(data + 8, stream->generated, 8); stream_write_be(data + 16, now_ns(), 8);
            stream_write_be(data + 24, stream->seed, 8); memcpy(data + 32, stream->source.ed, sizeof(stream->source.ed));
            stream_write_be(data + 64, stream->size, 4);
            uint64_t random = stream->seed ^ stream->generated ^ stream_read_be(stream->source.ed, 8);
            for (size_t p = LAB_PAYLOAD_HEADER; p < stream->size - 32; p++) {
                random ^= random << 13; random ^= random >> 7; random ^= random << 17; data[p] = (uint8_t)random;
            }
            SHA256(data, stream->size - 32, data + stream->size - 32);
            size_t accepted = 0;
            int result = stream_publish_write(stream->handle, data, stream->size, &accepted);
            if (result != STREAM_OK || accepted != stream->size) {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: write failed rc=%d accepted=%zu expected=%zu", result, accepted, stream->size);
                lab->errors++; stream->interval_ns = 0; break;
            }
            stream->generated++; stream->due_ns += stream->interval_ns;
        }
    }
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--identity")) {
        uint8_t private_key[32], public_key[32], transport_secret[32], ed[32];
        debug_enable_console(0);
        SHA256((const unsigned char *)argv[2], strlen(argv[2]), private_key);
        unsigned attempt = 0;
        while (sc_identity_from_seed(private_key, public_key, transport_secret, ed) != SC_OK) {
            if (++attempt == 256) return 1;
            private_key[31]++;
        }
        printf("{\"private\":\"");
        for (unsigned i = 0; i < 32; i++) printf("%02x", private_key[i]);
        printf("\",\"public\":\"");
        for (unsigned i = 0; i < 32; i++) printf("%02x", public_key[i]);
        printf("\",\"ed25519\":\"%s\"}\n", sc_public_key_hex(ed, (char[65]){0})); return 0;
    }
    int core_only = argc == 4 && !strcmp(argv[3], "--core-only");
    if (argc != 3 && !core_only) { fprintf(stderr, "Usage: %s CONFIG LOG [--core-only] | --identity SEED\n", argv[0]); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal); signal(SIGPIPE, SIG_IGN);
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_enable_console(0); debug_enable_file_output(argv[2], 1);
    utun_instance_set_tun_init_enabled(0);
    struct UASYNC *async = uasync_create();
    struct lab *lab = u_calloc(1, sizeof(*lab));
    if (!async || !lab) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: initialization allocation failed"); return 1; }
    lab->inst = utun_instance_create(async, argv[1]);
    if (!lab->inst || utun_core_start(lab->inst) || (!core_only && chat_service_start(lab->inst))) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: service startup failed"); return 1;
    }
    if (core_only) {
        nat_detection_set_automatic(lab->inst->nat_det, 0);
        if (!topo_groups_create_group(lab->inst->topo_groups, TOPO_GROUP_UTUN, TOPO_GROUP_TYPE_UTUN, "")) {
            DEBUG_ERROR(DEBUG_CATEGORY_NAT, "lab: NAT control group creation failed"); return 1;
        }
    }
    debug_enable_console(0);
    active_lab = lab; chat_event_set_handler(lab->inst, join_event);
    if (!core_only) stream_set_event_callback(lab->inst, event, lab);
    if (fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: stdin nonblocking failed: %s", strerror(errno)); return 1;
    }
    printf("{\"ready\":true,\"node\":\"%s\"}\n", sc_public_key_hex(lab->inst->my_ed25519_pubkey, (char[65]){0}));
    char input[8192]; size_t used = 0;
    while (!stopping) {
        uasync_poll(async, 100); generate(lab);
        ssize_t size = read(STDIN_FILENO, input + used, sizeof(input) - used - 1);
        if (size == 0) break;
        if (size < 0) {
            if (errno != EAGAIN && errno != EINTR) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: stdin failed: %s", strerror(errno)); lab->errors++; break; }
            continue;
        }
        used += (size_t)size; input[used] = 0;
        char *line;
        while ((line = memchr(input, '\n', used))) {
            *line = 0; command(lab, input);
            size_t consumed = (size_t)(line - input) + 1;
            memmove(input, input + consumed, used - consumed); used -= consumed; input[used] = 0;
        }
        if (used == sizeof(input) - 1) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "lab: RPC exceeds input limit"); lab->errors++; break; }
    }
    if (lab->invite) chat_invite_build_cancel(lab->invite);
    if (lab->cpu_timer) uasync_cancel_timeout(async, lab->cpu_timer);
    for (unsigned i = 0; i < LAB_STREAMS; i++) {
        if (lab->streams[i].handle) stream_close(lab->streams[i].handle);
        u_free(lab->streams[i].payload);
        if (lab->streams[i].errors) lab->errors++;
    }
    int errors = lab->errors;
    for (unsigned i = 0; i < lab->nat_handle_count; i++) node_conn_direct_force_close(lab->nat_handles[i]);
    for (unsigned i = 0; i < lab->nat_record_count; i++) topo_node_registry_release(lab->inst->topo_groups, lab->nat_records[i]);
    utun_instance_destroy(lab->inst); uasync_destroy(async, 1); u_free(lab);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "lab: stopped errors=%d", errors);
    debug_disable_file_output(); return errors ? 1 : 0;
}
