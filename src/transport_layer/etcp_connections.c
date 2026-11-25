#include "etcp_link_io.h"
#include "packet_transport.h"
#include "etcp_profile.h"
#include "etcp_connections.h"
#include "etcp_api.h"
#include "call_ring.h"
#include "../lib/socket_compat.h"
#include "../lib/platform_compat.h"
#include "../lib/getmyip.h"
#ifndef _WIN32
#include <net/if.h>
#include <endian.h>
#else
#include <winsock2.h>
#endif
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "utun_instance.h"
#include "config_parser.h"
#include "etcp.h"
#include "stcp_link.h"
#include "stcp.h"
#include "stcp_client.h"
#include "socks_client.h"
#include "topo_node.h"
#include "topo_node_rtt.h"
#include "topo_group.h"
#include "node_conn_direct.h"
#include "nat_detection.h"
#include "route_ping.h"
#include "../lib/memory_pool.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "etcp_loadbalancer.h"

#include "etcp_keepalive.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif
#include <stdlib.h>
#include <time.h>
#include "../lib/mem.h"
#include "etcp.h"

/* Вызывать только после аутентификации ciphertext/STCP, под active_users/callbacks_running.
 * Новый handshake перепроверяет членство; обычные DATA используют положительный кэш этого линка. */
static int admit_peer(struct ETCP_LINK* link, int handshake) {
    struct ETCP_CONN* c = link->etcp;
    if (!link->require_known_node) return 1;
    if (!handshake && link->peer_admitted) return 1;
    if (c->peer_node_id != sc_derive_node_id_from_pubkey(c->crypto_ctx.peer_public_key)
        || !topo_groups_peer_key_known(c->instance, c->crypto_ctx.peer_public_key)) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] authenticated handshake denied: unknown peer key path=%u", c->log_name, link->local_link_id);
        etcp_link_close(link); return 0;
    }
    link->peer_admitted = 1;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] authenticated known peer admitted path=%u", c->log_name, link->local_link_id);
    return 1;
}

// Читаемое имя типа сокета (CFG_SERVER_TYPE_*) для логов.
static const char* server_type_str(uint8_t type) {
    static const char* names[] = {"UNKNOWN", "PUBLIC", "NAT", "PRIVATE", "LOCAL"};
    return type < 5 ? names[type] : "?";
}
/* ID уникален среди живых сокетов; поколение не повторяется при переиспользовании ID. */
static int socket_identity_init(struct ETCP_SOCKET *socket, struct UTUN_INSTANCE *instance) {
    for (unsigned attempt = 0; attempt < 256; attempt++) {
        uint8_t candidate = instance->next_socket_id++;
        struct ETCP_SOCKET *existing = instance->etcp_sockets;
        while (existing && existing->sock_id != candidate) existing = existing->next;
        if (existing) continue;
        socket->sock_id = candidate;
        do {
            if (random_bytes((uint8_t *)&socket->generation, sizeof(socket->generation))) {
                DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket generation RNG failed name=%s", socket->name); return -1;
            }
        } while (!socket->generation);
        return 0;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "no free socket ID name=%s", socket->name); return -1;
}

/* ── Forward declarations ────────────────────────────────────────────
 * Только static-функции, используемые раньше своего определения.
 * Не-static функции уже объявлены в etcp_connections.h. */
static void tcp_link_close_cb(struct stcp_link *sl, int err, void *arg);
/* Wire fields may start at any byte offset. */
static uint16_t wire_read16(const void* p) { uint16_t v; memcpy(&v,p,2); return be16toh(v); }
static uint64_t wire_read64(const void* p) { uint64_t v; memcpy(&v,p,8); return be64toh(v); }
static void wire_write16(void* p, uint16_t v) { v = htobe16(v); memcpy(p,&v,2); }
static void wire_write64(void* p, uint64_t v) { v = htobe64(v); memcpy(p,&v,8); }

static void etcp_link_send_init(struct ETCP_LINK* link, uint8_t reset, uint8_t collision);
static void etcp_link_init_timer_cbk(void* arg);
static void burst_resp_timeout_cb(void* arg);
static int etcp_tcp_send(struct ETCP_DGRAM* dgram);
static void etcp_process_packet(struct ETCP_SOCKET* e_sock, uint8_t* data, ssize_t recv_len, struct sockaddr_storage addr);
static void etcp_process_packet_direct(struct ETCP_SOCKET* e_sock, uint8_t* data, ssize_t recv_len, struct sockaddr_storage addr);
static void etcp_process_packet_result(struct ETCP_SOCKET* e_sock, uint8_t* data, ssize_t recv_len,
    struct sockaddr_storage addr, const struct packet_transport_event* decoded);
static int etcp_datagram_send(struct ETCP_DGRAM* dgram);
static void udp_receive(struct ETCP_LINK* link, struct ll_entry* packet);
static void stcp_receive(struct ETCP_LINK* link, struct ll_entry* packet);
static void bound_receive(struct ETCP_LINK* link, struct ll_entry* packet);
static const struct etcp_link_codec udp_codec = {1, etcp_datagram_send, udp_receive};
static const struct etcp_link_codec stcp_codec = {1, etcp_tcp_send, stcp_receive};
static const struct etcp_link_codec bound_codec = {0, etcp_datagram_send, bound_receive};

int etcp_link_network_overhead(const struct ETCP_LINK* link) {
    if (link->codec && !link->codec->ip_headers) return 0;
    return link->remote_addr.ss_family == AF_INET6 ? IP_UDP_OVERHEAD_V6 : IP_UDP_OVERHEAD_V4;
}

static void socks_etcp_read_callback(socket_t sock, void* arg);
static void socks_relay_ready_cb(struct socks_udp* s, int err, void* arg);

static int publish_udp_key(struct ETCP_LINK* link) {
    if (!link->conn || !link->conn->transport || link->is_tcp || !link->etcp->crypto_ctx.session_ready) return 0;
    const uint8_t* key = link->etcp->crypto_ctx.session_key;
    if (link->transport_key_ready && !memcmp(link->transport_key, key, SC_SESSION_KEY_SIZE)) return 0;
    if (packet_transport_key(link->conn->transport, &link->remote_addr, &link->etcp->crypto_ctx)) return -1;
    memcpy(link->transport_key, key, SC_SESSION_KEY_SIZE); link->transport_key_ready = 1;
    return 0;
}

static void transport_event(void* arg, const struct packet_transport_event* event) {
    struct UTUN_INSTANCE* instance = arg;
    if (instance->destroying) return;
    struct ETCP_SOCKET* socket = instance->etcp_sockets;
    while (socket && socket->generation != event->socket_generation) socket = socket->next;
    if (!socket || !socket->transport) {
        if (event->type == PACKET_TRANSPORT_RX && !event->error && event->size > ETCP_ENCRYPTED_HDR_SIZE &&
            event->data[ETCP_ENCRYPTED_HDR_SIZE] >= ETCP_INIT_REQUEST && event->data[ETCP_ENCRYPTED_HDR_SIZE] <= ETCP_INIT_RESPONSE_NOINIT)
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "UDP handshake discarded: expired socket generation=%llu peer=%s code=%02x",
                       (unsigned long long)event->socket_generation, sockaddr_storage_to_str(&event->peer).str,
                       event->data[ETCP_ENCRYPTED_HDR_SIZE]);
        else DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "stale UDP completion: socket=%llu", (unsigned long long)event->socket_generation);
        return;
    }
    if (event->type == PACKET_TRANSPORT_RX) {
        etcp_process_packet_result(socket, NULL, event->wire_size, event->peer, event);
        return;
    }
    struct ETCP_LINK* link = etcp_link_find_by_addr(socket, &event->peer, 0);
    if (link && link->etcp->peer_node_id == event->tag.peer_id && link->etcp->reset_id == event->tag.epoch &&
        link->local_link_id == event->tag.link_id) {
        if (event->error) {
            link->send_errors++;
            etcp_tx_failed(link, event->tag.attempt_id, event->error);
        } else {
            link->etcp->crypto_ctx.tx_counter++;
            link->diagnostic_wire_tx_bytes += event->wire_size;
            link->etcp->diagnostic_wire_tx_bytes += event->wire_size;
        }
    }
    /* Освобождается общий кредит: будим также соединения, ещё не получившие собственный слот. */
    for (struct ll_entry* e = instance->connections->head; e; e = e->next)
        etcp_conn_wake_sender(((struct conn_queue_entry*)e->data)->conn);
}

int etcp_transport_start(struct UTUN_INSTANCE* instance) {
    if (instance->packet_transport) return 0;
    instance->packet_transport = packet_transport_create(instance->ua, instance->config->global.crypto_tx_threads,
        instance->config->global.crypto_rx_threads, &instance->my_keys, transport_event, instance);
    return instance->packet_transport ? 0 : -1;
}

// Коллбэк готовности SOCKS5 UDP ASSOCIATE: только логирование (sendto сам ждёт ready).
static void socks_relay_ready_cb(struct socks_udp* s, int err, void* arg) {
    struct ETCP_SOCKET* e_sock = (struct ETCP_SOCKET*)arg;
    if (e_sock->transport) {
        struct sockaddr_storage relay;
        packet_transport_relay(e_sock->transport, socks_udp_get_relay(s, &relay) ? &relay : NULL);
        for (struct ll_entry* e = e_sock->links_queue->head; e; e = e->next)
            etcp_link_io_update(((struct link_queue_entry*)e->data)->link);
    }
    if (err != 0) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socks UDP associate error for socket %s err=%d", e_sock->name, err); return; }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "socks UDP relay ready for socket %s", e_sock->name);
    if (!e_sock->transport) etcp_link_io_socket_writable(e_sock->fd, e_sock);
}

// Чтение с локального UDP-сокета SOCKS5 UDP ASSOCIATE: снять SOCKS-заголовок → реальный src,
// дальше обычный разбор через etcp_process_packet.
static void socks_etcp_read_callback(socket_t sock, void* arg) {
    struct ETCP_SOCKET* e_sock = (struct ETCP_SOCKET*)arg;
    if (!e_sock || !e_sock->socks_udp) return;
    uint8_t raw[PACKET_DATA_SIZE + 64];
    uint8_t payload[PACKET_DATA_SIZE];
    struct sockaddr_storage relay;
    socklen_t rl = sizeof(relay);
    memset(&relay, 0, sizeof(relay));
    ssize_t raw_len = socket_recvfrom(sock, raw, sizeof(raw), (struct sockaddr*)&relay, &rl);
    if (raw_len <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "socks recvfrom failed, error=%zd, sock_err=%d", raw_len, socket_get_error());
        return;
    }
    struct sockaddr_storage src;
    ssize_t plen = socks_udp_unwrap(e_sock->socks_udp, raw, (size_t)raw_len, payload, sizeof(payload), &src);
    if (plen <= 0) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "socks unwrap failed len=%zd from %s", plen, sockaddr_storage_to_str(&relay).str);
        return;
    }
    etcp_process_packet(e_sock, payload, plen, src);
}

// STCP-сервер: новое входящее TCP-соединение → найти/создать ETCP_CONN по node_id
// pubkey пира, создать TCP-линк и поднять его (ready).
void tcp_server_on_link(struct stcp_link *link, struct ETCP_SOCKET *tcp_sock) {
    struct UTUN_INSTANCE *inst = tcp_sock->instance;
    const uint8_t *pubkey = stcp_link_get_peer_pubkey(link);
    if (!pubkey) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "tcp_server_on_link: no peer pubkey"); stcp_link_close(link); return; }
    uint64_t node_id = sc_derive_node_id_from_pubkey(pubkey);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "tcp_server_on_link: accepted TCP from peer=0x%016llx", (unsigned long long)node_id);

    struct ETCP_CONN *conn = instance_find_conn_by_key(inst, pubkey);
    int new_conn = conn == NULL;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "tcp_server_on_link: %s conn=%p peer=0x%016llx pubkey=%016llx",
               conn ? "existing" : "NEW", (void*)conn, (unsigned long long)node_id, (unsigned long long)wire_read64(pubkey));

    if (!conn) {
        conn = etcp_connection_create(inst, NULL);
        if (!conn) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "tcp_server_on_link: etcp_connection_create failed/cancelled"); stcp_link_close(link); return; }
        conn->peer_node_id = node_id;
        if (sc_init_ctx(&conn->crypto_ctx, &inst->my_keys) != SC_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "tcp_server_on_link: sc_init_ctx failed");
            etcp_connection_close(conn); stcp_link_close(link); return;
        }
        if (sc_set_peer_public_key(&conn->crypto_ctx, pubkey, SC_PEER_PUBKEY_BIN) != SC_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "tcp_server_on_link: sc_set_peer_public_key failed");
            etcp_connection_close(conn); stcp_link_close(link); return;
        }
        etcp_update_log_name(conn);
        if (etcp_conn_index_peer(conn) < 0) { etcp_connection_close(conn); stcp_link_close(link); return; }
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "tcp_server_on_link: new ETCP_CONN peer=0x%016llx", (unsigned long long)node_id);
    }

    const struct sockaddr_storage* ra = stcp_link_get_peer_addr(link);
    if (!ra) ra = stcp_link_get_remote_addr(link);
    struct ETCP_LINK *tlink = etcp_link_new(conn, tcp_sock, ra, 1);
    if (!tlink) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "tcp_server_on_link: link creation failed/cancelled peer=%016llx", (unsigned long long)node_id);
        stcp_link_close(link);
        if (new_conn) {
            int owners = node_conn_direct_handle_count(conn);
            if (!conn->links && !owners) etcp_connection_close(conn);
            else DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] cancelled TCP accept preserves connection: other_links=%u owners=%d closing=%u",
                conn->log_name, conn->links != NULL, owners, conn->close_requested);
        }
        return;
    }
    tlink->tcp_link = link;
    tlink->is_server = 1;

    stcp_link_set_etcp_conn(link, conn);
    stcp_link_set_etcp_link(link, tlink);
    stcp_link_set_on_close(link, tcp_link_close_cb, tlink);
    callring_put(CR_SRV_ONLINK, (uintptr_t)tlink, (uintptr_t)link);

    etcp_link_enter_ready_tcp(tlink);
}

// === Burst sender: замер пропускной способности линка ===

// Старт burst: отправка пачки пакетов подряд для измерения bandwidth.
void etcp_link_burst_start(struct ETCP_LINK* link) {
    if (!link || !link->etcp || !link->etcp->instance) return;
    link->burst_active = 1;
    link->burst_seq = 0;
    link->burst_count = BURST_PACKET_COUNT;
    link->burst_id++;
    link->burst_last_time_tb = get_time_tb();
    link->burst_pkt_size = 0;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] burst start id=%u count=%u", link->etcp->log_name, link->burst_id, link->burst_count);
// resume send queue чтобы burst-пакеты были немедленно отправлены
    if (link->etcp->link_ready_for_send_fn) link->etcp->link_ready_for_send_fn(link->etcp);
}

// Возможность стартовать burst: линк насыщен (inflight достиг лимита) и прошёл интервал.
void etcp_link_burst_check(struct ETCP_LINK* link) {
    if (!link || !link->etcp) return;
    if (link->burst_active) return;
    if (link->link_status != 1) return;
    if (link->inflight_bytes < link->inflight_lim_bytes) return;
    uint64_t now = get_time_tb();
    if (now - link->burst_last_time_tb < MIN_BURST_INTERVAL_TB) return;
    etcp_link_burst_start(link);
}

// Завершение burst: сброс флага, запуск таймера ожидания ответа с замерами пира.
void etcp_link_burst_finish(struct ETCP_LINK* link) {
    if (!link || !link->etcp || !link->etcp->instance) return;
    link->burst_active = 0;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] burst end id=%u pkt_sz=%u wait_resp", link->etcp->log_name, link->burst_id, link->burst_pkt_size);
    link->burst_resp_timer = uasync_set_timeout(link->etcp->instance->ua, BURST_RESP_TIMEOUT_TB, link, burst_resp_timeout_cb, "burst_resp");
    if (link->etcp->link_ready_for_send_fn) link->etcp->link_ready_for_send_fn(link->etcp);
}

// Таймаут ожидания ответа на burst — просто сбрасываем таймер.
static void burst_resp_timeout_cb(void* arg) {
    struct ETCP_LINK* link = (struct ETCP_LINK*)arg;
    if (!link) return;
    link->burst_resp_timer = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] burst resp timeout id=%u", link->etcp->log_name, link->burst_id);
}

#define INIT_TIMEOUT_INITIAL 500
#define INIT_TIMEOUT_MAX 50000

// Клиент: собрать и отправить INIT_REQUEST (handshake) — заголовок, padding для обфускации
// размера и обфусцированный pubkey. reset=1 → запрос сброса эпохи, collision=1 → ответ на коллизию.
static void etcp_link_send_init(struct ETCP_LINK* link, uint8_t reset, uint8_t collision) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "link=%p, is_server=%d, reset=%d, collision=%d", link, link ? link->is_server : -1, reset, collision);
    if (!link || !link->etcp || !link->etcp->instance) return;
    if (link->is_tcp) return; // TCP uses STCP handshake instead of ETCP INIT

    struct ETCP_DGRAM* dgram = u_malloc(PACKET_DATA_SIZE);
    if (!dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "malloc failed");
        return;
    }

    memset(dgram, 0, sizeof(*dgram));
    dgram->link = link;
    dgram->noencrypt_len = SC_PUBKEY_ENC_SIZE;

    struct ETCP_INIT_REQUEST_PKT* req = (struct ETCP_INIT_REQUEST_PKT*)dgram->data;
    req->code = reset ? ETCP_INIT_REQUEST : ETCP_INIT_REQUEST_NOINIT;
    wire_write64(req->reset_id,link->etcp->reset_id);
    wire_write16(req->mtu,link->mtu_local);
    wire_write16(req->keepalive,link->etcp->instance->keepalive_interval);
    wire_write16(req->recovery,link->recovery_interval / 100);
    req->link_id = link->local_link_id;
    req->socket_id = link->conn ? link->conn->sock_id : 0;
    req->only_local = link->conn ? link->conn->only_local : 0;
    req->type = link->conn ? link->conn->type : CFG_SERVER_TYPE_UNKNOWN;
    // SOCKS5-прокси: пир не может дотянуться до нашего interface_addr — не рекламируем src.
    if (link->conn && link->conn->socks_udp) {
        memset(req->src_ipv4, 0, 4);
        memset(req->src_port, 0, 2);
    } else if (link->conn && link->conn->interface_addr.ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&link->conn->interface_addr;
        memcpy(req->src_ipv4, &sin->sin_addr.s_addr, 4);
        memcpy(req->src_port,&sin->sin_port,2);
    } else if (link->conn && link->conn->interface_addr.ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&link->conn->interface_addr;
        memset(req->src_ipv4, 0, 4);
        memcpy(req->src_port,&sin6->sin6_port,2);
    } else {
        memset(req->src_ipv4, 0, 4);
        memset(req->src_port, 0, 2);
    }
    req->collision = collision;
    memcpy(req->ed25519_pubkey, link->etcp->instance->my_ed25519_pubkey, SC_PUBKEY_SIZE);
    req->device_type = link->etcp->instance->client_type;
    size_t offset = ETCP_INIT_REQ_SIZE;

    // padding
    int wire_ov_init = (etcp_link_network_overhead(link) + SC_ENCRYPT_OVERHEAD);
    int pad_range = (int)link->handshake_maxsize - (int)link->handshake_minsize;
    if (pad_range <= 0) pad_range = 1;
    int s = rand() % pad_range + link->handshake_minsize;
    int s_max = (int)(link->mtu) - wire_ov_init;
    if (s > s_max) s = s_max;
    if (s < 0) s = 0;

    int to_add = s - offset - wire_ov_init;
    if (to_add<0) to_add=0;
    int max_data = PACKET_DATA_SIZE - (int)sizeof(struct ETCP_DGRAM);
    if (offset + to_add + SC_PUBKEY_ENC_SIZE > max_data) {
        to_add = max_data - offset - SC_PUBKEY_ENC_SIZE;
        if (to_add<0) to_add=0;
    }

    for (int i=0; i<to_add; i++) dgram->data[offset++]=rand();// fill pad
    // padding end

    uint8_t salt[SC_PUBKEY_ENC_SALT_SIZE];
    if (random_bytes(salt, sizeof(salt))) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] INIT salt generation failed link=%u", link->etcp->log_name, link->local_link_id);
        u_free(dgram); return;
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
    memcpy(dgram->data + offset, salt, SC_PUBKEY_ENC_SALT_SIZE);
    offset += SC_PUBKEY_ENC_SALT_SIZE;
#pragma GCC diagnostic pop

    uint8_t obfuscated_pubkey[SC_PUBKEY_SIZE];
    sc_obfuscate_pubkey(salt, link->etcp->crypto_ctx.peer_public_key, 
                        link->etcp->instance->my_keys.public_key, obfuscated_pubkey);
    DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "INIT_SEND: salt=%02x%02x%02x%02x%02x%02x%02x%02x obf_pub=%02x%02x%02x%02x peer_pub=%02x%02x%02x%02x my_pub=%02x%02x%02x%02x",
               salt[0], salt[1], salt[2], salt[3], salt[4], salt[5], salt[6], salt[7],
               obfuscated_pubkey[0], obfuscated_pubkey[1], obfuscated_pubkey[2], obfuscated_pubkey[3],
               link->etcp->crypto_ctx.peer_public_key[0], link->etcp->crypto_ctx.peer_public_key[1],
               link->etcp->crypto_ctx.peer_public_key[2], link->etcp->crypto_ctx.peer_public_key[3],
               link->etcp->instance->my_keys.public_key[0], link->etcp->instance->my_keys.public_key[1],
               link->etcp->instance->my_keys.public_key[2], link->etcp->instance->my_keys.public_key[3]);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
    memcpy(dgram->data + offset, obfuscated_pubkey, SC_PUBKEY_SIZE);
    offset += SC_PUBKEY_SIZE;
#pragma GCC diagnostic pop


    dgram->data_len = offset;
    
    int sent = etcp_encrypt_send(dgram);
    if (sent == -2)
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] INIT postponed: transport busy link=%u retry=%u body=%u",
                    link->etcp->log_name, link->local_link_id, link->init_retry_count, dgram->data_len);
    else if (sent < 0)
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] INIT send failed link=%u retry=%u body=%u result=%d",
                   link->etcp->log_name, link->local_link_id, link->init_retry_count, dgram->data_len, sent);
    else if (link->init_retry_count == 0 || link->init_retry_count == 10)
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] INIT submitted link=%u retry=%u reset=%d bytes=%d socket=%s dst=%s",
                   link->etcp->log_name, link->local_link_id, link->init_retry_count, reset, sent,
                   link->conn ? link->conn->name : "packet", sockaddr_storage_to_str(&link->remote_addr).str);
    else
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] INIT submitted link=%u retry=%u reset=%d bytes=%d",
                    link->etcp->log_name, link->local_link_id, link->init_retry_count, reset, sent);
    u_free(dgram);

    link->init_retry_count++;
}

// Периодический повтор INIT до установления связи (с экспоненциальным backoff до INIT_TIMEOUT_MAX).
static void etcp_link_init_timer_cbk(void* arg) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    struct ETCP_LINK* link = (struct ETCP_LINK*)arg;
    if (!link || !link->etcp || !link->etcp->instance) return;

    if ((link->init_retry_count % 10) == 0 && link->init_timeout < INIT_TIMEOUT_MAX) {
        link->init_timeout += link->init_timeout/4 +1;
        if (link->init_timeout > INIT_TIMEOUT_MAX) link->init_timeout = INIT_TIMEOUT_MAX;
    }
    link->init_timer = uasync_set_timeout(link->etcp->instance->ua, link->init_timeout, link, etcp_link_init_timer_cbk, "link_init");

    if (link->link_state == LINK_STATE_CONNECTED && link->initialized) { DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] init_timer: SUPPRESSED state=%d init=%d link_status=%d recv_ka=%d remote_ka=%d links_up=%d", link->etcp->log_name, link->link_state, link->initialized, link->link_status, link->recv_keepalive, link->remote_keepalive, link->etcp->links_up); return; }
    if (link->etcp->links_up > 0) { DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] init_timer: SUPPRESSED (links_up=%d > 0)", link->etcp->log_name, link->etcp->links_up); return; }
    if (link->etcp->fin_wait) { DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "[%s] init_timer: SUPPRESSED (fin_wait)", link->etcp->log_name); return; }
    if (link->is_tcp) { etcp_tcp_link_start_reconnect(link); return; } // TCP uses STCP handshake, not ETCP INIT
    if (link->etcp->got_initial_pkt == 0) etcp_link_send_init(link,1,0);
    else etcp_link_send_init(link,0,0);
}

// Сброс таймера INIT на начальный таймаут (используется при каждом новом handshake).
void etcp_link_restart_init_timer(struct ETCP_LINK* link) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (link->init_timer) uasync_cancel_timeout(link->etcp->instance->ua, link->init_timer);
    link->init_timeout = INIT_TIMEOUT_INITIAL;
    link->init_timer = uasync_set_timeout(link->etcp->instance->ua, link->init_timeout, link, etcp_link_init_timer_cbk, "link_init");
}

static void etcp_link_nat_init_cb(void* arg) {
    struct ETCP_LINK* link = arg;
    link->init_timer = NULL;
    if (!link->initialized && !link->etcp->fin_wait) etcp_link_enter_init(link);
}

// Клиент: переход в handshake — отправить INIT и запустить таймер повторов.
void etcp_link_enter_init(struct ETCP_LINK* link) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!link || link->closing || link->etcp->close_requested) return;
    struct ETCP_CONN *conn = link->etcp;
    uint64_t delay = nat_detection_udp_init_delay(conn->instance->nat_det, link);
    if (delay) {
        if (link->init_timer) uasync_cancel_timeout(conn->instance->ua, link->init_timer);
        link->init_timer = uasync_set_timeout(conn->instance->ua, delay, link, etcp_link_nat_init_cb, "link_nat_init");
        if (link->init_timer) return;
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "[%s] NAT INIT delay timer failed socket=%s remote=%s",
                    conn->log_name, link->conn->name, sockaddr_storage_to_str(&link->remote_addr).str);
    }
    conn->callbacks_running++; link->active_users++;
    int old_state = link->link_state;
    link->link_state = LINK_STATE_HANDSHAKE; // handshake
    etcp_fire_link_status_cbk(link, old_state, link->link_status);
    if (!etcp_link_is_alive(conn, link) || link->is_server != 0) goto done;
    int reset = (link->etcp->got_initial_pkt == 0);
    etcp_link_send_init(link, reset, 0);
    etcp_link_restart_init_timer(link);
done:
    etcp_link_release(link); conn->callbacks_running--;
}

// Клиент: повторный handshake после разрыва — уведомить о падении, отправить INIT, снять keepalive.
void etcp_link_enter_reinit(struct ETCP_LINK* link) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!link || link->closing || link->etcp->close_requested) return;
    struct ETCP_CONN *conn = link->etcp;
    conn->callbacks_running++; link->active_users++;
    int old_state = link->link_state;
    link->link_state = LINK_STATE_TRY_RECONNECT; // reconnect
    etcp_fire_link_status_cbk(link, old_state, link->link_status);
    if (!etcp_link_is_alive(conn, link)) goto done;
    etcp_on_link_down(conn, link);
    if (!etcp_link_is_alive(conn, link) || link->is_server != 0) goto done;
    int reset = (link->etcp->got_initial_pkt == 0);
    etcp_link_send_init(link, reset, 0);

    if (link->keepalive_timer) {// keepalive заменяяется reinit запросами
        uasync_cancel_timeout(link->etcp->instance->ua, link->keepalive_timer);
        link->keepalive_timer = NULL;
    }

    etcp_link_restart_init_timer(link);
done:
    etcp_link_release(link); conn->callbacks_running--;
}


// Собирает 19-байтовый ключ из sockaddr (port+addr+family) для hash-индекса links_queue.
static void sockaddr_to_key(const struct sockaddr_storage* addr, uint8_t key[LINK_ADDR_KEY_SIZE], int is_tcp) {
    memset(key, 0, LINK_ADDR_KEY_SIZE);
    if (!addr) return;
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in* sa = (const struct sockaddr_in*)addr;
        memcpy(key,      &sa->sin_port,       2);
        memcpy(key + 2,  &sa->sin_addr.s_addr, 4);
        key[18] = 4 | (is_tcp ? 0x80 : 0);
    } else {
        const struct sockaddr_in6* sa = (const struct sockaddr_in6*)addr;
        memcpy(key,      &sa->sin6_port,  2);
        memcpy(key + 2,  &sa->sin6_addr,  16);
        key[18] = 6 | (is_tcp ? 0x80 : 0);
    }
}

// find_link_index, realloc_links, insert_link, remove_link — УДАЛЕНЫ. Заменены на ll_queue с хеш-индексом.

// Вставка линка в links_queue сокета по hash-ключу адреса. При занятом адресе:
// другой conn с тем же pubkey — ERROR (дубль-conn); другой conn с up-линком — отказ;
// другой conn с down-линком — вытеснение (etcp_link_close); тот же conn — замена stale-записи.
static int insert_link_queue(struct ETCP_SOCKET* e_sock, struct ETCP_LINK* link) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!e_sock || !link || !e_sock->links_queue) return -1;

    uint8_t key[LINK_ADDR_KEY_SIZE];
    sockaddr_to_key(&link->remote_addr, key, link->is_tcp);
    struct ll_entry* dup_qe = queue_find_data_by_index(e_sock->links_queue, key);
    if (dup_qe) {
        struct link_queue_entry* dup_lqe = (struct link_queue_entry*)dup_qe->data;
        if (dup_lqe->link && dup_lqe->link->etcp != link->etcp) {
            struct ETCP_CONN* new_c = link->etcp;
            struct ETCP_CONN* old_c = dup_lqe->link->etcp;

            /* одинаковые ключи на двух разных conn — баг «дубль-conn»: ERROR + отказ */
            if (!memcmp(new_c->crypto_ctx.peer_public_key, old_c->crypto_ctx.peer_public_key, SC_PUBKEY_SIZE)) {
                DEBUG_ERROR(DEBUG_CATEGORY_ETCP,
                    "insert_link_queue: SAME-PUBKEY DUPLICATE conn at %s — new=%p [%s] state=%d peer=0x%016llx / old=%p [%s] state=%d init=%d up=%d",
                    sockaddr_storage_to_str(&link->remote_addr).str,
                    (void*)new_c, new_c->log_name, new_c->state, (unsigned long long)new_c->peer_node_id,
                    (void*)old_c, old_c->log_name, old_c->state, old_c->initialized, old_c->links_up);
                return -1;
            }

            uint64_t new_qkey = 0, old_qkey = 0;
            if (new_c->conn_queue_entry) new_qkey = ((struct conn_queue_entry*)new_c->conn_queue_entry->data)->peer_node_id;
            if (old_c->conn_queue_entry) old_qkey = ((struct conn_queue_entry*)old_c->conn_queue_entry->data)->peer_node_id;
            if (dup_lqe->link->link_status != 0) {
                /* активный линк чужого conn — отказ (активный линк сносить нельзя) */
                DEBUG_ERROR(DEBUG_CATEGORY_ETCP,
                    "!!!!!!!!!!!! LINK ADDR COLLISION !!!!!!!!!!!! addr=%s "
                    "new: conn=%p [%s] peer=0x%016llx state=%d init=%d up=%d srv=%d tcp=%d qkey=0x%016llx "
                    "old: conn=%p [%s] peer=0x%016llx state=%d init=%d up=%d srv=%d tcp=%d qkey=0x%016llx",
                    sockaddr_storage_to_str(&link->remote_addr).str,
                    (void*)new_c, new_c->log_name, (unsigned long long)new_c->peer_node_id,
                    new_c->state, new_c->initialized, new_c->links_up, link->is_server, link->is_tcp,
                    (unsigned long long)new_qkey,
                    (void*)old_c, old_c->log_name, (unsigned long long)old_c->peer_node_id,
                    old_c->state, old_c->initialized, old_c->links_up,
                    dup_lqe->link->is_server, dup_lqe->link->is_tcp,
                    (unsigned long long)old_qkey);
                return -1;
            }

            /* разные ключи, линк чужого conn в down — вытесняем полным закрытием:
               etcp_link_close снимает запись из links_queue, стреляет link_status/on_link_down
               и освобождает link; линк восстановится при ретрае пира. */
            DEBUG_WARN(DEBUG_CATEGORY_ETCP,
                "insert_link_queue: evict stale DOWN link old=%p [%s] peer=0x%016llx for new peer=0x%016llx at %s",
                (void*)dup_lqe->link, old_c->log_name, (unsigned long long)old_c->peer_node_id,
                (unsigned long long)new_c->peer_node_id, sockaddr_storage_to_str(&link->remote_addr).str);
            etcp_link_close(dup_lqe->link);
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "insert_link_queue: replacing stale DUP addr in [%s] old_link=%p", e_sock->name, dup_lqe->link);
            if (dup_lqe->link) dup_lqe->link->link_queue_entry = NULL;
            queue_remove_data(e_sock->links_queue, dup_qe);
            queue_entry_free(dup_qe);
        }
    }

    struct ll_entry* qe = queue_entry_new(sizeof(struct link_queue_entry));
    if (!qe) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "queue_entry_new failed"); return -1; }
    struct link_queue_entry* lqe = (struct link_queue_entry*)qe->data;
    memcpy(lqe->key, key, LINK_ADDR_KEY_SIZE);
    lqe->link = link;
    link->link_queue_entry = qe;
    queue_data_put_with_index(e_sock->links_queue, qe);
    return 0;
}

// Удаление линка из links_queue сокета (освобождение hash-записи).
static void remove_link_from_queue(struct ETCP_LINK* link) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!link || !link->link_queue_entry) return;
    if (!link->conn || !link->conn->links_queue) return;
    queue_remove_data(link->conn->links_queue, link->link_queue_entry);
    queue_entry_free(link->link_queue_entry);
    link->link_queue_entry = NULL;
}

// Сравнение двух sockaddr_storage (family + addr + port) без учёта прочих полей.
static int sockaddr_equal(const struct sockaddr_storage* a, const struct sockaddr_storage* b) {
    if (!a || !b || a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *sa = (const struct sockaddr_in*)a;
        const struct sockaddr_in *sb = (const struct sockaddr_in*)b;
        return (sa->sin_addr.s_addr == sb->sin_addr.s_addr && sa->sin_port == sb->sin_port);
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *sa = (const struct sockaddr_in6*)a;
        const struct sockaddr_in6 *sb = (const struct sockaddr_in6*)b;
        return (memcmp(&sa->sin6_addr, &sb->sin6_addr, 16) == 0 && sa->sin6_port == sb->sin6_port);
    }
    return 0;
}

// Поиск линка в сокете по remote-адресу (через hash-индекс links_queue).
struct ETCP_LINK* etcp_link_find_by_addr(struct ETCP_SOCKET* e_sock, const struct sockaddr_storage* addr, int is_tcp) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!e_sock || !addr || !e_sock->links_queue) return NULL;
    uint8_t key[LINK_ADDR_KEY_SIZE];
    sockaddr_to_key(addr, key, is_tcp);
    struct ll_entry* e = queue_find_data_by_index(e_sock->links_queue, key);
    if (!e) return NULL;
    struct link_queue_entry* lqe = (struct link_queue_entry*)e->data;
    return lqe->link;
}

/* Перепривязывает UDP-линк на новый remote-адрес (пир сменил NAT-адрес, линк ещё down).
 * Пере-индексирует линк в socket->links_queue под новым ключом. */
static void etcp_link_update_remote_addr(struct ETCP_LINK* link, struct ETCP_SOCKET* e_sock,
                                         const struct sockaddr_storage* new_addr) {
    if (!link || !e_sock || !new_addr) return;
    if (link->is_tcp) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] update_remote_addr: TCP link skip", link->etcp->log_name);
        return;
    }
    if (link->conn != e_sock) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] update_remote_addr: link socket [%s] != recv socket [%s], skip",
                   link->etcp->log_name, link->conn ? link->conn->name : "-", e_sock->name);
        return;
    }
    remove_link_from_queue(link);
    if (e_sock->transport) packet_transport_key(e_sock->transport, &link->remote_addr, NULL);
    link->transport_key_ready = 0;
    memcpy(&link->remote_addr, new_addr, sizeof(struct sockaddr_storage));
    if (insert_link_queue(e_sock, link) < 0)
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] update_remote_addr: re-index FAILED at %s",
                    link->etcp->log_name, sockaddr_storage_to_str(new_addr).str);
    publish_udp_key(link);
}

/* Коллизия INIT возможна только на одной паре: принимающий сокет + адрес отправителя.
 * Исходящий линк через другой локальный сокет — независимый путь к тому же пиру. */
struct ETCP_LINK* etcp_select_collision_link(struct ETCP_CONN* conn, struct ETCP_SOCKET* e_sock,
                                             const struct sockaddr_storage* addr) {
    if (!conn || !e_sock || !addr) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "collision lookup: invalid conn=%p socket=%p addr=%p", conn, e_sock, addr);
        return NULL;
    }
    struct ETCP_LINK* l = conn->links;
    while (l) {
        if (l->is_server == 0 && l->conn == e_sock && sockaddr_equal(&l->remote_addr, addr)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] collision lookup: recv_socket=%s remote=%s outbound_link=%u/%u",
                        conn->log_name, e_sock->name, sockaddr_storage_to_str(addr).str, l->local_link_id, l->remote_link_id);
            return l;
        }
        l = l->next;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] collision lookup: recv_socket=%s remote=%s no matching outbound link",
                conn->log_name, e_sock->name, sockaddr_storage_to_str(addr).str);
    return NULL;
}

// Поиск линка по remote_link_id (id, под которым линк известен пиру).
struct ETCP_LINK* etcp_link_find_by_remote_id(struct ETCP_CONN* conn, uint8_t remote_link_id) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!conn || remote_link_id == 0) return NULL;
    struct ETCP_LINK* l = conn->links;
    while (l) {
        if (l->remote_link_id == remote_link_id) return l;
        l = l->next;
    }
    return NULL;
}

// Выделение свободного local_link_id (0-255): битовая маска занятых id + циклический поиск.
int etcp_find_free_local_link_id(struct ETCP_CONN* etcp) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!etcp) return -1;

    // Битовый массив для 256 id (32 байта * 8 бит = 256)
    uint8_t used_ids[32] = {1};// индекс 0 всегда занят

    // Помечаем занятые id
    struct ETCP_LINK* link = etcp->links;
    while (link) {
        used_ids[link->local_link_id >> 3] |= (1 << (link->local_link_id & 7));
        link = link->next;
    }

    // Ищем первый свободный id, начиная с last_assigned_link_id + 1 (циклический перебор)
    for (int i = 0; i < 256; i++) {
        int id = ((etcp->last_assigned_link_id + 1 + i) & 0xFF);
        if (id == 0) continue;
        if (!(used_ids[id >> 3] & (1 << (id & 7)))) {
            etcp->last_assigned_link_id = (uint8_t)id;
            return id;
        }
    }

    // Все id заняты
    return -1;
}


// Создание UDP-сокета приёма/отправки кодограмм: bind к interface_addr, настройка буферов,
// регистрация в uasync и добавление в instance->etcp_sockets.
struct ETCP_SOCKET* etcp_socket_add(struct UTUN_INSTANCE* instance, struct CFG_SERVER* server) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!instance || !server) return NULL;

    struct sockaddr_storage* ip = &server->ip;
    uint32_t netif_index = server->netif_index;
    int so_mark = server->so_mark;
    int fib = server->fib;
    uint8_t type = instance->config->global.hide_my_sockets ? CFG_SERVER_TYPE_PRIVATE : server->type;
    int mtu = server->mtu ? server->mtu : instance->config->global.mtu;
    if (mtu == 0 || mtu > PACKET_DATA_MAX_MTU) mtu = PACKET_DATA_MAX_MTU;
    uint8_t only_local = server->only_local;
    uint8_t socks_enabled = server->socks_enabled;
    char* name = server->name;
    
    struct ETCP_SOCKET* e_sock = u_calloc(1, sizeof(struct ETCP_SOCKET));
    if (!e_sock) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Failed to allocate connection");
        return NULL;
    }
    e_sock->fd = SOCKET_INVALID;  // Initialize to invalid socket
    
    if (name && name[0]) {
        strncpy(e_sock->name, name, MAX_CONN_NAME_LEN - 1);
        e_sock->name[MAX_CONN_NAME_LEN - 1] = '\0';
    } else {
        e_sock->name[0] = '\0';
    }

    if (ip && (server->ipv6_mode == CFG_IPV6_MODE_TEMPORARY || server->ipv6_mode == CFG_IPV6_MODE_PERMANENT)) {
        if (netif_index == 0) {
            netif_index = get_default_route_netif_index(AF_INET6);
            if (netif_index == 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "Cannot determine default route interface for IPv6 socket %s", name);
                u_free(e_sock);
                return NULL;
            }
        }
        int temp = (server->ipv6_mode == CFG_IPV6_MODE_TEMPORARY) ? 1 : 0;
        uint8_t v6addr[16];
        if (get_interface_ipv6_addr_nl(netif_index, !temp, v6addr) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "Failed to get %s IPv6 from netif_index=%u for socket %s",
                       temp ? "temporary" : "permanent", netif_index, name);
            u_free(e_sock);
            return NULL;
        }
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)ip;
        memcpy(&sin6->sin6_addr, v6addr, 16);
    }

    int family = AF_INET;
    if (ip) {
        family = ip->ss_family;
        e_sock->netif_index = netif_index;
        if (family != AF_INET && family != AF_INET6) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "Unsupported address family: %d", family);
            u_free(e_sock);
            return NULL;
        }
    }
    
    e_sock->fd = socket_create_udp(family);
    if (e_sock->fd == SOCKET_INVALID) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "Failed to create socket: %s", 
                   socket_strerror(socket_get_error()));
        u_free(e_sock);
        return NULL;
    }
    
    // Строго не используем reuseaddr, даже в тестах!
    socket_set_reuseaddr(e_sock->fd, 0);

    // Increase socket buffers for high throughput
    socket_set_udp_buffers(e_sock->fd, 4 * 1024 * 1024, 4 * 1024 * 1024);

    if (socket_set_nonblocking(e_sock->fd) != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "Failed to set non-blocking mode");
    }

    if (family == AF_INET6) {
        int v6only = 1;
        if (setsockopt(e_sock->fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only)) < 0) {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "Failed to set IPV6_V6ONLY: %s", socket_strerror(socket_get_error()));
        }
    }
    
    // Set socket mark if specified (Linux only)
    if (so_mark > 0) {
        socket_set_mark(e_sock->fd, so_mark);
    }

    // Set FIB for FreeBSD
#ifdef __FreeBSD__
    if (fib > 0) {
        if (setsockopt(e_sock->fd, SOL_SOCKET, SO_SETFIB, &fib, sizeof(fib)) < 0) {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "Failed to set FIB %d: %s", fib, strerror(errno));
        }
    }
#endif
    
    // Bind to interface if specified (Linux only)
#ifndef _WIN32
    if (netif_index > 0) {
        char ifname[IF_NAMESIZE];
        if (if_indextoname(netif_index, ifname)) {
            socket_bind_to_device(e_sock->fd, ifname);
        }
    }
#endif
    
    // Определяем interface_addr до bind, чтобы забиндить на конкретный адрес
    memset(&e_sock->interface_addr, 0, sizeof(e_sock->interface_addr));
    if (netif_index > 0 && ip && ip->ss_family == AF_INET) {
        uint32_t if_ip = get_interface_ip_by_index(netif_index);
        if (if_ip != 0) {
            struct sockaddr_in* sin = (struct sockaddr_in*)&e_sock->interface_addr;
            sin->sin_family = AF_INET;
            sin->sin_addr.s_addr = if_ip;
        }
    } else if (ip) {
        if (ip->ss_family == AF_INET) {
            struct sockaddr_in* sin = (struct sockaddr_in*)ip;
            if (sin->sin_addr.s_addr == 0) {
                struct sockaddr_storage remote;
                memset(&remote, 0, sizeof(remote));
                struct sockaddr_in* rem_sin = (struct sockaddr_in*)&remote;
                rem_sin->sin_family = AF_INET;
                rem_sin->sin_port = htons(53);
                inet_pton(AF_INET, "8.8.8.8", &rem_sin->sin_addr);
                if (get_outgoing_local_ip(ip, &remote, &e_sock->interface_addr) != 0) {
                    DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "Failed to detect outgoing local IP for socket %s", e_sock->name);
                }
            }
        } else if (ip->ss_family == AF_INET6) {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)ip;
            if (memcmp(&sin6->sin6_addr, &in6addr_any, sizeof(struct in6_addr)) == 0) {
                uint16_t v6if = (netif_index > 0) ? netif_index : get_default_route_netif_index(AF_INET6);
                if (v6if > 0) {
                    uint8_t v6addr[16];
                    int got = get_interface_ipv6_addr_nl(v6if, 1, v6addr);
                    if (got != 0) got = get_interface_ipv6_addr_nl(v6if, 0, v6addr);
                    if (got == 0) {
                        struct sockaddr_in6* if6 = (struct sockaddr_in6*)&e_sock->interface_addr;
                        if6->sin6_family = AF_INET6;
                        memcpy(&if6->sin6_addr, v6addr, 16);
                        if6->sin6_port = sin6->sin6_port;
                    }
                }
                if (e_sock->interface_addr.ss_family == 0) {
                    struct sockaddr_storage remote;
                    memset(&remote, 0, sizeof(remote));
                    struct sockaddr_in6* rem_sin6 = (struct sockaddr_in6*)&remote;
                    rem_sin6->sin6_family = AF_INET6;
                    rem_sin6->sin6_port = htons(53);
                    inet_pton(AF_INET6, "2001:4860:4860::8888", &rem_sin6->sin6_addr);
                    if (get_outgoing_local_ip(ip, &remote, &e_sock->interface_addr) == 0) {
                        struct sockaddr_in6* if6 = (struct sockaddr_in6*)&e_sock->interface_addr;
                        if6->sin6_port = sin6->sin6_port;
                    }
                }
            }
        }
    }
    if (e_sock->interface_addr.ss_family == 0) {
        if (ip) memcpy(&e_sock->interface_addr, ip, sizeof(struct sockaddr_storage));
    }
    // Копируем порт из конфига в interface_addr
    if (ip && e_sock->interface_addr.ss_family == ip->ss_family) {
        if (ip->ss_family == AF_INET) {
            struct sockaddr_in* sin_cfg = (struct sockaddr_in*)ip;
            struct sockaddr_in* sin_if = (struct sockaddr_in*)&e_sock->interface_addr;
            sin_if->sin_port = sin_cfg->sin_port;
        } else if (ip->ss_family == AF_INET6) {
            struct sockaddr_in6* sin6_cfg = (struct sockaddr_in6*)ip;
            struct sockaddr_in6* sin6_if = (struct sockaddr_in6*)&e_sock->interface_addr;
            sin6_if->sin6_port = sin6_cfg->sin6_port;
        }
    }

    // Bind — используем interface_addr если определён, иначе ip из конфига
    {
        struct sockaddr_storage* bind_addr = (e_sock->interface_addr.ss_family != 0) ? &e_sock->interface_addr : ip;
        if (bind_addr && bind_addr->ss_family != 0) {
            memcpy(&e_sock->local_addr, bind_addr, sizeof(struct sockaddr_storage));
            socklen_t addr_len = (bind_addr->ss_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
            if (bind(e_sock->fd, (struct sockaddr*)bind_addr, addr_len) < 0) {
                int error = socket_get_error();
                uint32_t scope = family == AF_INET6 ? ((struct sockaddr_in6*)bind_addr)->sin6_scope_id : 0;
                DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[ETCP] UDP bind failed name=%s ifidx=%u addr=%s scope=%u err=%d: %s",
                            name, netif_index, sockaddr_storage_to_str(bind_addr).str, scope, error, socket_strerror(error));
                socket_close_wrapper(e_sock->fd);
                u_free(e_sock);
                socket_set_error(error);
                return NULL;
            }
            socklen_t bound_len = sizeof(e_sock->local_addr);
            if (getsockname(e_sock->fd, (struct sockaddr*)&e_sock->local_addr, &bound_len) < 0) {
                int error = socket_get_error();
                DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket %s: cannot read bound address err=%d: %s",
                            name, error, socket_strerror(error));
                socket_close_wrapper(e_sock->fd); u_free(e_sock); socket_set_error(error); return NULL;
            }
            // Порт 0 выбирает ОС; NODEINFO и подбор сокетов должны видеть фактический адрес.
            e_sock->interface_addr = e_sock->local_addr;
            if (bind_addr->ss_family == AF_INET) {
                struct sockaddr_in* sin = (struct sockaddr_in*)bind_addr;
                DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[ETCP] Successfully bound socket to local address, family=AF_INET %s:%d", ip_to_str(&sin->sin_addr, AF_INET).str, ntohs(sin->sin_port));
            } else if (bind_addr->ss_family == AF_INET6) {
                struct sockaddr_in6* sin6 = (struct sockaddr_in6*)bind_addr;
                DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[ETCP] Successfully bound socket to local address, family=AF_INET6 %s:%d", ip_to_str(&sin6->sin6_addr, AF_INET6).str, ntohs(sin6->sin6_port));
            }
        }
    }

    char config_str[64] = "none";
    char netif_str[64] = "none";
    if (e_sock->local_addr.ss_family != 0) {
        snprintf(config_str, sizeof(config_str), "%s", sockaddr_storage_to_str(&e_sock->local_addr).str);
    }
    if (e_sock->interface_addr.ss_family != 0) {
        snprintf(netif_str, sizeof(netif_str), "%s", sockaddr_storage_to_str(&e_sock->interface_addr).str);
    }
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Listen socket initialized: name=%s fd=%d config=%s, netif=%s", e_sock->name, e_sock->fd, config_str, netif_str);

    e_sock->instance = instance;
    e_sock->errorcode = 0;
    e_sock->pkt_format_errors = 0;
    e_sock->type = type;
    if (socket_identity_init(e_sock, instance)) { socket_close_wrapper(e_sock->fd); u_free(e_sock); return NULL; }
    e_sock->mtu = mtu;
    e_sock->only_local = only_local;
    e_sock->links_queue = queue_new(instance->ua, 256, offsetof(struct link_queue_entry, key), LINK_ADDR_KEY_SIZE, "links");
    if (!e_sock->links_queue) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "Failed to create links_queue for socket %s", e_sock->name);
        socket_close_wrapper(e_sock->fd);
        u_free(e_sock);
        return NULL;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "Add Socket type=%s", server_type_str(type));
    e_sock->next = instance->etcp_sockets;
    instance->etcp_sockets = e_sock;

    if (socks_enabled) {
        // SOCKS5 UDP ASSOCIATE: весь сокет туннелируется через прокси.
        struct global_config* gc = &instance->config->global;
        if (!gc->socks_host[0] || !gc->socks_port) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket %s socks=yes but [socks] addr not configured", e_sock->name);
            socket_close_wrapper(e_sock->fd);
            queue_free(e_sock->links_queue);
            instance->etcp_sockets = e_sock->next;
            u_free(e_sock);
            return NULL;
        }
        struct socks_cfg scfg;
        memset(&scfg, 0, sizeof(scfg));
        strncpy(scfg.host, gc->socks_host, sizeof(scfg.host) - 1);
        scfg.port = gc->socks_port;
        strncpy(scfg.user, gc->socks_username, sizeof(scfg.user) - 1);
        strncpy(scfg.pass, gc->socks_password, sizeof(scfg.pass) - 1);
        e_sock->socks_udp = socks_udp_associate(instance->ua, &scfg, e_sock->fd, socks_relay_ready_cb, e_sock);
        if (!e_sock->socks_udp) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socks_udp_associate failed for socket %s", e_sock->name);
            socket_close_wrapper(e_sock->fd);
            queue_free(e_sock->links_queue);
            instance->etcp_sockets = e_sock->next;
            u_free(e_sock);
            return NULL;
        }

        DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "socket %s tunneled via SOCKS5 UDP (proxy=%s:%u)", e_sock->name, gc->socks_host, gc->socks_port);
    }
    if (!instance->config->global.crypto_tx_threads && !instance->config->global.crypto_rx_threads) {
        e_sock->socket_id = uasync_add_socket_t(instance->ua, e_sock->fd,
            socks_enabled ? socks_etcp_read_callback : etcp_connections_read_callback_socket,
            etcp_link_io_socket_writable, NULL, e_sock->name, e_sock);
        if (e_sock->socket_id) uasync_set_socket_write(instance->ua, e_sock->socket_id, 0);
    } else if (!etcp_transport_start(instance))
        e_sock->transport = packet_transport_socket(instance->packet_transport, e_sock->fd, e_sock->generation, socks_enabled);
    if (!e_sock->transport && !e_sock->socket_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "Failed to register socket with uasync");
        if (e_sock->socks_udp) { socks_udp_destroy(e_sock->socks_udp); e_sock->socks_udp = NULL; }
        socket_close_wrapper(e_sock->fd);
        queue_free(e_sock->links_queue);
        instance->etcp_sockets = e_sock->next;
        u_free(e_sock);
        return NULL;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[ETCP] Socket %p registered and active", e_sock);
    return e_sock;
}

// Удаление UDP-сокета: снятие с uasync, закрытие fd, закрытие всех линков и освобождение.
void etcp_socket_remove(struct ETCP_SOCKET* conn) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!conn) return;
    if (conn->instance && conn->instance->nat_det) nat_detection_socket_removed(conn->instance->nat_det, conn);
    
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[ETCP] Removing socket %p, socket_id=%p", conn, conn->socket_id);
    
    if (conn->transport) { packet_transport_close(conn->transport); conn->transport = NULL; }

    // Remove from uasync if registered
    if (conn->socket_id && conn->instance) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[ETCP] Removing socket from uasync, instance=%p, ua=%p", conn->instance, conn->instance->ua);
        uasync_remove_socket_t(conn->instance->ua, conn->fd);
        conn->socket_id = NULL;
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[ETCP] Unregistered socket from uasync");
    }
    
    if (conn->fd != SOCKET_INVALID) {
        socket_close_wrapper(conn->fd);
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[ETCP] Closed socket");
    }
    
    if (conn->socks_udp) { socks_udp_destroy(conn->socks_udp); conn->socks_udp = NULL; }

    if (conn->links_queue) {
        struct ll_entry* entry;
        while ((entry = conn->links_queue->head) != NULL) {
            struct link_queue_entry* lqe = (struct link_queue_entry*)entry->data;
            etcp_link_close(lqe->link);
        }
        queue_free(conn->links_queue);
        conn->links_queue = NULL;
    }

    if (conn->instance) {
        struct ETCP_SOCKET** pp = &conn->instance->etcp_sockets;
        while (*pp && *pp != conn) pp = &(*pp)->next;
        if (*pp) *pp = conn->next;
    }

    u_free(conn);
}

/* ── TCP socket management ── */

// TCP-сокет (запись ETCP_SOCKET с is_tcp=1): определение bind-адреса по конфигу.
// Сам listen/accept выполняет stcp_server — здесь только модель сокета.
struct ETCP_SOCKET* tcp_socket_add(struct UTUN_INSTANCE* instance, struct CFG_SERVER* server) {
    if (!instance || !server) return NULL;

    struct ETCP_SOCKET* ts = u_calloc(1, sizeof(struct ETCP_SOCKET));
    if (!ts) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_socket_add: alloc failed"); return NULL; }
    ts->instance = instance;
    ts->is_tcp = 1;
    ts->netif_index = server->netif_index;
    ts->reality_enabled = server->reality_enabled;
    ts->fd = SOCKET_INVALID;
    ts->type = instance->config->global.hide_my_sockets ? CFG_SERVER_TYPE_PRIVATE : server->type;
    if (server->name && server->name[0]) {
        strncpy(ts->name, server->name, MAX_CONN_NAME_LEN - 1);
        ts->name[MAX_CONN_NAME_LEN - 1] = '\0';
    }
    if (socket_identity_init(ts, instance)) { u_free(ts); return NULL; }

    if (server->ip.ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&server->ip;
        memcpy(&ts->local_addr, sin, sizeof(*sin));
        ts->interface_addr = ts->local_addr;
        if (sin->sin_addr.s_addr == 0 && server->netif_index > 0) {
            uint32_t if_ip = get_interface_ip_by_index(server->netif_index);
            if (if_ip != 0) {
                struct sockaddr_in* if_sin = (struct sockaddr_in*)&ts->interface_addr;
                if_sin->sin_family = AF_INET;
                if_sin->sin_addr.s_addr = if_ip;
                if_sin->sin_port = sin->sin_port;
            }
        }
        if (server->type == CFG_SERVER_TYPE_PUBLIC && sin->sin_addr.s_addr == 0) {
            struct sockaddr_storage remote; memset(&remote, 0, sizeof(remote));
            struct sockaddr_in* rs = (struct sockaddr_in*)&remote;
            rs->sin_family = AF_INET; rs->sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &rs->sin_addr);
            struct sockaddr_storage local;
            if (get_outgoing_local_ip(&server->ip, &remote, &local) == 0) {
                ts->interface_addr = local;
                ((struct sockaddr_in*)&ts->interface_addr)->sin_port = sin->sin_port;
            }
        }
    } else if (server->ip.ss_family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&server->ip;
        memcpy(&ts->local_addr, sin6, sizeof(*sin6));
        ts->interface_addr = ts->local_addr;
        if (memcmp(&sin6->sin6_addr, &in6addr_any, sizeof(struct in6_addr)) == 0 && server->netif_index > 0) {
            uint8_t v6addr[16];
            int got = get_interface_ipv6_addr_nl(server->netif_index, 1, v6addr);
            if (got != 0) got = get_interface_ipv6_addr_nl(server->netif_index, 0, v6addr);
            if (got == 0) {
                struct sockaddr_in6* if6 = (struct sockaddr_in6*)&ts->interface_addr;
                if6->sin6_family = AF_INET6; memcpy(&if6->sin6_addr, v6addr, 16);
                if6->sin6_port = sin6->sin6_port;
            }
        }
        if (server->type == CFG_SERVER_TYPE_PUBLIC && memcmp(&sin6->sin6_addr, &in6addr_any, sizeof(struct in6_addr)) == 0) {
            uint16_t v6if = (server->netif_index > 0) ? server->netif_index : get_default_route_netif_index(AF_INET6);
            if (v6if > 0) {
                uint8_t v6addr[16];
                int got = get_interface_ipv6_addr_nl(v6if, 1, v6addr);
                if (got != 0) got = get_interface_ipv6_addr_nl(v6if, 0, v6addr);
                if (got == 0) {
                    struct sockaddr_in6* if6 = (struct sockaddr_in6*)&ts->interface_addr;
                    if6->sin6_family = AF_INET6; memcpy(&if6->sin6_addr, v6addr, 16);
                }
            }
            if (ts->interface_addr.ss_family == 0) {
                struct sockaddr_storage remote; memset(&remote, 0, sizeof(remote));
                struct sockaddr_in6* rs6 = (struct sockaddr_in6*)&remote;
                rs6->sin6_family = AF_INET6; rs6->sin6_port = htons(53);
                inet_pton(AF_INET6, "2001:4860:4860::8888", &rs6->sin6_addr);
                if (get_outgoing_local_ip(&server->ip, &remote, &ts->interface_addr) == 0)
                    ((struct sockaddr_in6*)&ts->interface_addr)->sin6_port = sin6->sin6_port;
                else
                    ts->interface_addr = ts->local_addr;
            }
        }
    }

    ts->next = instance->etcp_sockets;
    instance->etcp_sockets = ts;
    {   char loc_str[64] = "none", if_str[64] = "none";
        if (ts->local_addr.ss_family) snprintf(loc_str, sizeof(loc_str), "%s", sockaddr_storage_to_str(&ts->local_addr).str);
        if (ts->interface_addr.ss_family) snprintf(if_str, sizeof(if_str), "%s", sockaddr_storage_to_str(&ts->interface_addr).str);
        DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "tcp_socket_add: %s type=%s sock_id=%u ifidx=%u local=%s iface=%s",
                   ts->name, server_type_str(ts->type), ts->sock_id, ts->netif_index, loc_str, if_str); }
    return ts;
}

// Удаление TCP-сокета и всех его линков, включая созданные вне NCD.
void tcp_socket_remove(struct ETCP_SOCKET* sock) {
    if (!sock || !sock->instance) return;
    if (sock->instance->nat_det) nat_detection_socket_removed(sock->instance->nat_det, sock);
    struct UTUN_INSTANCE* inst = sock->instance;
    struct ETCP_SOCKET** pp = &inst->etcp_sockets;
    while (*pp && *pp != sock) pp = &(*pp)->next;
    if (*pp) *pp = sock->next;
    unsigned closed = 0;
    for (;;) {
        struct ETCP_LINK* found = NULL;
        for (struct ll_entry* e = inst->connections ? inst->connections->head : NULL; e && !found; e = e->next) {
            struct ETCP_CONN* conn = ((struct conn_queue_entry*)e->data)->conn;
            for (struct ETCP_LINK* link = conn->links; link; link = link->next)
                if (link->conn == sock) { found = link; break; }
        }
        if (!found) break;
        /* DOWN callbacks могут менять список соединений: после close начинаем поиск заново. */
        etcp_link_close(found);
        closed++;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "tcp_socket_remove: %s sock_id=%u closed_links=%u", sock->name, sock->sock_id, closed);
    u_free(sock);
}

// Создание нового ETCP_LINK: аллокация, выделение local_link_id, инициализация BBR/keepalive,
// вставка в список коннекта и links_queue сокета. Для outbound UDP сразу запускает handshake.
static struct ETCP_LINK* link_create(struct ETCP_CONN* etcp, struct ETCP_SOCKET* conn,
    const struct sockaddr_storage* remote_addr, uint8_t is_server, const struct packet_link_driver* driver) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!etcp) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "null etcp"); return NULL; }
    if (etcp->state == 2 || etcp->close_requested) { DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] link_new on closing conn", etcp->log_name); return NULL; }
    int is_tcp = !driver && ((conn == NULL) || conn->is_tcp);

    struct ETCP_LINK* link = u_calloc(1, sizeof(struct ETCP_LINK));
    if (!link) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "calloc failed - out of memory or pool exhausted");
        return NULL;
    }

    link->conn = conn;
    link->codec = driver ? &bound_codec : (is_tcp ? &stcp_codec : &udp_codec);
    link->etcp = etcp;
    link->is_server = is_server;
    link->is_tcp = (uint8_t)is_tcp;
    // Предел кадра STCP не задаёт MTU ETCP: фрагмент со служебными секциями должен вмещаться в 1500 байт.
    int mtu = is_tcp || !conn ? PACKET_DATA_MAX_MTU : conn->mtu;
    if (mtu == 0) mtu = 1280;
    if (mtu > PACKET_DATA_MAX_MTU) mtu = PACKET_DATA_MAX_MTU;
    if (driver) {
        size_t maximum = driver->get_state(driver->ctx).max_packet;
        mtu = maximum < PACKET_DATA_MAX_MTU ? (int)maximum : PACKET_DATA_MAX_MTU;
    }
    link->mtu_local = link->mtu = mtu;
    link->handshake_minsize = 100; link->handshake_maxsize = mtu;
    link->initialized = 0;
    link->init_timer = NULL;
    link->init_timeout = 0;
    link->init_retry_count = 0;
    link->link_status = 0;
    link->send_hook = NULL;
    link->send_hook_ctx = NULL;
    link->send_hook_release = NULL;

    // Initialize keepalive timeout from global config
    if (etcp->instance && etcp->instance->config) {
        link->keepalive_timeout = etcp->instance->config->global.keepalive_timeout;
        link->keepalive_interval = etcp->instance->config->global.keepalive_interval;
    } else {
        link->keepalive_timeout = 2000;  // Default 2 seconds
        link->keepalive_interval = 200;  // Default 0.2 s
    }
    if (link->keepalive_interval < 10) link->keepalive_interval = 10;
    link->keepalive_sent_count = 0;
    link->keepalive_recv_count = 0;
    link->ka_period_ms = (uint16_t)link->keepalive_interval;
    link->ka_sleep_period_ms = 0;
    link->ka_my_sleeping = 0;
    link->ka_peer_sleeping = 0;
    link->ka_sleep_acked = 0;
    link->ka_active_remaining = 0;
    link->ka_sleep_duration = 0;
    link->ka_sleep_announce_timer = NULL;
    link->ka_announce_attempts = 0;
    link->ka_announce_max_attempts = 0;
    link->ka_fail_count = 0;
    link->bandwidth = 0; // До первого ACK pacing не задан; исходный burst ограничен начальным DATA-окном.
    link->burst_id = 0;
    link->burst_active = 0;
    link->burst_last_time_tb = 0;
    link->burst_target_bdp = 0;

    link->delivered_bytes = 0;
    link->acked_bytes = 0;
    link->acked_packets = 0;
    link->last_ack_time_us = get_time_us();
    link->bbr_pacing_rate = 0;
    link->bbr_loss_since_ack = 0;
    link->bbr = u_calloc(1, sizeof(struct bbr));
    if (!link->bbr) {
        u_free(link);
        return NULL;
    }
    bbr_init(link->bbr);
    if (etcp_tx_link_init(link) < 0) { u_free(link->bbr); u_free(link); return NULL; }

    // Выделяем свободный local_link_id
    int free_id = etcp_find_free_local_link_id(etcp);
    if (free_id <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "no free local_link_id available");
        etcp_tx_link_destroy(link); u_free(link->bbr);
        u_free(link);
        return NULL;
    }
    link->local_link_id = (uint8_t)free_id;
    link->diagnostic_id = ++etcp->diagnostic_next_link_id;
    if (link->bbr) link->bbr->link_id = (uint8_t)free_id;

    if (remote_addr) memcpy(&link->remote_addr, remote_addr, sizeof(struct sockaddr_storage));

    if (conn && conn->interface_addr.ss_family) memcpy(&link->local_bound_addr, &conn->interface_addr, sizeof(struct sockaddr_storage));
    else memset(&link->local_bound_addr, 0, sizeof(link->local_bound_addr));

    link->last_recv_local_time = get_time_tb();  // Initialize to prevent immediate timeout

    link->total_retransmissions  = 0;

    if (!driver && !is_tcp && insert_link_queue(conn, link) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Can not insert link to socket");
        etcp_tx_link_destroy(link); u_free(link->bbr);
        u_free(link);
        return NULL;
    }

    if (driver && etcp_link_io_bind(link, driver)) {
        etcp_tx_link_destroy(link); u_free(link->bbr); u_free(link); return NULL;
    }
    struct ETCP_LINK* l=etcp->links;
    while (l && l->next) l=l->next;
    if (l) l->next = link; else etcp->links = link;

    etcp_update_mtu(etcp);
    etcp_link_update_inflight_lim(link, link->bbr->init_cwnd * etcp->normalizer->frag_size);

    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "NEW link etcp=[%s] link=%p socket=%s id=%u server=%u mtu=%u tcp=%u DATA_MSS=%u cwnd=%u",
        etcp->log_name, link, driver ? driver->name : conn ? conn->name : "tcp", link->local_link_id, link->is_server, link->mtu, link->is_tcp,
        etcp->normalizer->frag_size, link->inflight_lim_bytes);
    if (driver) return link; /* Событие packet_link начнёт RX/INIT после возврата. */
    link->active_users++;
    if (etcp_link_io_create(link)) { etcp_link_close(link); etcp_link_release(link); return NULL; }
    if (publish_udp_key(link)) { etcp_link_close(link); etcp_link_release(link); return NULL; }
    etcp_fire_link_status_cbk(link, -1, 0);

    if (!link->closing && !etcp->close_requested && !is_tcp && is_server == 0) {
        etcp_link_enter_init(link);
    }
    if (link->closing) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] link creation cancelled path=%u", etcp->log_name, link->local_link_id);
        etcp_link_release(link);
        return NULL;
    }
    if (etcp->close_requested) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] link creation stopped by connection close path=%u", etcp->log_name, link->local_link_id);
        etcp_link_close(link);
        etcp_link_release(link);
        return NULL;
    }

    callring_put(CR_LINK_NEW, (uintptr_t)link, (uintptr_t)etcp);
    etcp_link_release(link);
    return link;
}

struct ETCP_LINK* etcp_link_add(struct ETCP_CONN* conn, const struct packet_link_driver* driver, int require_known_node) {
    if (!conn || !driver || !driver->get_state || driver->ua != conn->instance->ua ||
        !conn->crypto_ctx.session_ready || !conn->peer_node_id ||
        conn->peer_node_id != sc_derive_node_id_from_pubkey(conn->crypto_ctx.peer_public_key)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "link_add: invalid driver/event loop or missing peer identity"); return NULL;
    }
    struct packet_link_state state = driver->get_state(driver->ctx);
    if (state.max_packet < SC_ENCRYPT_OVERHEAD + ETCP_PAYLOAD_HEADER_SIZE + ACK_REZERV + 3) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "link_add: packet capacity too small: %zu", state.max_packet); return NULL;
    }
    struct ETCP_LINK* link = link_create(conn, NULL, NULL, 0, driver);
    if (link) link->require_known_node = !!require_known_node; /* RX/state callbacks драйвера отложены. */
    return link;
}

struct ETCP_LINK* etcp_link_new(struct ETCP_CONN* conn, struct ETCP_SOCKET* socket,
    const struct sockaddr_storage* address, uint8_t is_server) {
    return link_create(conn, socket, address, is_server, NULL);
}

// Применить положительное окно DATA с верхним ограничением конфига; минимум выбирает BBR.
void etcp_link_update_inflight_lim(struct ETCP_LINK* link, uint32_t new_lim) {
    if (!link || !link->etcp) return;
    if (!new_lim) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] link=%u rejected zero DATA window", link->etcp->log_name, link->local_link_id);
        return;
    }
    if (new_lim > link->etcp->max_inflight) new_lim = link->etcp->max_inflight;

    link->inflight_lim_bytes = new_lim;
    etcp_conn_on_inflight_lim_changed(link->etcp);
}

// Убрать курсор и бит слота до удаления линка из списка: callbacks смогут безопасно переиспользовать его local_link_id.
static void etcp_inflight_nullify_link(struct ll_queue* q, struct ETCP_LINK* dead_link) {
    if (!q) return;
    struct ll_entry* entry = q->head;
    while (entry) {
        struct INFLIGHT_PACKET* inf = (struct INFLIGHT_PACKET*)entry;
        if (inf->last_link == dead_link) inf->last_link = NULL;
        inf->used_links[dead_link->local_link_id >> 3] &= (uint8_t)~(1u << (dead_link->local_link_id & 7));
        entry = entry->next;
    }
}

void etcp_link_release(struct ETCP_LINK* link) {
    if (!link || !link->active_users) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "link release without active owner link=%p", (void*)link);
        abort();
    }
    link->active_users--;
    if (link->closing && !link->active_users) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] closed link memory released path=%u", link->etcp->log_name, link->local_link_id);
        u_free(link->bbr); u_free(link);
    }
}

// Закрытие линка: отмена таймеров (init/shaper/keepalive/tcp), удаление из списков и очереди,
// зануление ссылок inflight (защита от use-after-free) и освобождение памяти.
void etcp_link_close(struct ETCP_LINK* link) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!link || link->closing) return;
    link->active_users++;
    link->closing = 1;
    if (link->conn && link->conn->transport && !link->is_tcp)
        packet_transport_key(link->conn->transport, &link->remote_addr, NULL);
    etcp_link_io_destroy(link);
    struct ETCP_CONN *etcp = link->etcp;
    etcp->callbacks_running++;
    void (*release_hook)(struct ETCP_LINK*, void*) = link->send_hook_release;
    void* hook_context = link->send_hook_ctx;
    link->send_hook = NULL; link->send_hook_ctx = NULL; link->send_hook_release = NULL;
    if (release_hook) release_hook(link, hook_context);
    int old_state = link->link_state, old_status = link->link_status;
    link->link_status = link->recv_keepalive = link->initialized = 0;
    if (link->rx_timeout_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->rx_timeout_timer); link->rx_timeout_timer = NULL;
    }
    callring_put(CR_LINK_CLOSE_IN, (uintptr_t)link, (uintptr_t)(link->etcp));
    if (link->etcp && link->etcp->last_rr_link == link) link->etcp->last_rr_link = NULL;

    if (link->is_tcp) {
        callring_put(CR_LINK_CLOSE_TCP, (uintptr_t)(link->tcp_link), (uintptr_t)link);
        if (link->tcp_reconnect_timer) { uasync_cancel_timeout(link->etcp->instance->ua, link->tcp_reconnect_timer); link->tcp_reconnect_timer = NULL; }
        if (link->tcp_link) { stcp_link_set_on_close(link->tcp_link, NULL, NULL); stcp_link_close(link->tcp_link); link->tcp_link = NULL; }
    }

    if (link->burst_resp_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->burst_resp_timer);
        link->burst_resp_timer = NULL;
    }
    if (link->ka_sleep_announce_timer) {
        uasync_cancel_timeout(etcp->instance->ua, link->ka_sleep_announce_timer); link->ka_sleep_announce_timer = NULL;
    }

    // Cancel init timer if active
    if (link->init_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->init_timer);
        link->init_timer = NULL;
    }

    // Cancel shaper timer if active
    if (link->shaper_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->shaper_timer);
        link->shaper_timer = NULL;
    }

    // Cancel keepalive timer if active
    if (link->keepalive_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->keepalive_timer);
        link->keepalive_timer = NULL;
    }

    etcp_inflight_nullify_link(etcp->input_wait_ack, link);
    etcp_inflight_nullify_link(etcp->input_send_q, link);

    // универсальное удаление из односвязного списка
    struct ETCP_LINK **pp = &link->etcp->links;
    while (*pp) {
        if (*pp == link) {
            *pp = link->next;
            break;
        }
        pp = &(*pp)->next;
    }

    remove_link_from_queue(link);
    etcp_tx_link_destroy(link);
    etcp_conn_on_inflight_lim_changed(link->etcp);
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] Link %d closed: addr=%s rcvd=%zub ack=%llub infl=%ub/%upkt",
               link->etcp->log_name, link->local_link_id, sockaddr_storage_to_str(&link->remote_addr).str, link->total_decrypted,
               (unsigned long long)link->acked_bytes, link->inflight_bytes, link->inflight_packets);
    etcp_fire_link_status_cbk(link, old_state, old_status);
    if (!etcp->close_requested) etcp_on_link_down(etcp, NULL);
    callring_put(CR_LINK_CLOSE_FREE, (uintptr_t)link, 0);
    etcp_link_release(link);
    etcp->callbacks_running--;
}

// ====== TCP link helpers ======

/* По готовому STCP сообщает пиру его фактический IP; исходящий порт не является портом listener. */
static void tcp_send_observed_ip(struct ETCP_LINK *link) {
    struct UTUN_INSTANCE *inst = link->etcp->instance;
    if (!link->tcp_link || (!link->is_server && inst->config && inst->config->global.socks_enabled)) return;
    const struct sockaddr_storage *peer = stcp_link_get_peer_addr(link->tcp_link);
    if (!peer) { DEBUG_WARN(DEBUG_CATEGORY_NAT, "TCP IP report cannot read peer socket=%s", link->etcp->log_name); return; }
    struct ETCP_DGRAM *pkt = memory_pool_alloc(inst->pkt_pool);
    if (!pkt) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "TCP IP report allocation failed peer=%s", link->etcp->log_name); return; }
    pkt->link = link; pkt->data_len = 18; pkt->noencrypt_len = 0;
    memset(pkt->data, 0, pkt->data_len); pkt->data[0] = ETCP_TCP_ADDRESS;
    if (peer->ss_family == AF_INET) {
        pkt->data[1] = 4; memcpy(pkt->data + 2, &((const struct sockaddr_in *)peer)->sin_addr, 4);
    } else if (peer->ss_family == AF_INET6) {
        pkt->data[1] = 6; memcpy(pkt->data + 2, &((const struct sockaddr_in6 *)peer)->sin6_addr, 16);
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "TCP IP report unsupported family=%d", peer->ss_family);
        memory_pool_free(inst->pkt_pool, pkt); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "TCP IP report peer=%s observed=%s", link->etcp->log_name, sockaddr_storage_to_str(peer).str);
    if (etcp_encrypt_send(pkt) < 0) DEBUG_WARN(DEBUG_CATEGORY_NAT, "TCP IP report send failed");
    memory_pool_free(inst->pkt_pool, pkt);
}

/* STCP-handle живёт до следующего poll; его отвязка обнаруживает удаление/замену линка, включая переиспользование адреса. */
static int tcp_link_attached(struct ETCP_CONN *conn, struct ETCP_LINK *link, struct stcp_link *transport) {
    if (conn->close_requested || conn->state == 2) return 0;
    if (transport && stcp_link_get_etcp_link(transport) != link) return 0;
    return etcp_link_is_alive(conn, link) && link->tcp_link == transport;
}

// Поднять TCP-линк в ready: синхронизация reset-эпохи (gop/rid), keepalive, ed25519,
// mark initialized + link_status=1, уведомить коннект и loadbalancer.
void etcp_link_enter_ready_tcp(struct ETCP_LINK *link) {
    if (!link || !link->etcp || link->closing || link->etcp->close_requested) return;
    struct ETCP_CONN *etcp = link->etcp;
    struct stcp_link *transport = link->tcp_link;
    etcp_link_io_update(link);
    etcp->callbacks_running++;
    link->active_users++;

    if (!admit_peer(link, 1)) goto done;

    /* reinit только при смене reset_id пира — синхронизация через etcp_conn_apply_peer_reset_id */
    if (link->tcp_link) {
        uint64_t peer_rid = stcp_link_get_peer_reset_id(link->tcp_link);
        etcp_conn_apply_peer_reset_id(etcp, peer_rid);
        if (!tcp_link_attached(etcp, link, transport)) goto done;

        link->peer_device_type = stcp_link_get_peer_device_type(link->tcp_link);
        {   uint16_t peer_ka = stcp_link_get_peer_keepalive_interval(link->tcp_link);
            link->keepalive_interval = negotiate_keepalive(etcp->instance->client_type,
                etcp->instance->keepalive_interval, link->peer_device_type, peer_ka); }
    }

    int old_state = link->link_state, old_status = link->link_status;
    link->initialized = 1; link->link_state = LINK_STATE_CONNECTED; link->link_status = 1;
    link->recv_keepalive = 1;
    link->last_recv_local_time = get_time_tb();
    if (!link->mtu_remote) link->mtu_remote = link->mtu;
    link->tcp_reconnect_delay_ms = 0;
    if (link->tcp_link) { const uint8_t* ed = stcp_link_get_peer_ed25519_pubkey(link->tcp_link); if (ed) memcpy(etcp->peer_ed25519_pubkey, ed, SC_PUBKEY_SIZE); }
    // Известная эпоха пира не подтверждает новый поток; следующий линк не перезапускает согласование.
    if (!etcp->initialized && !etcp->session_required) etcp_session_start(etcp);
    if (!tcp_link_attached(etcp, link, transport)) goto done;
    if (etcp->initialized == 0) etcp_conn_ready(etcp);
    if (!tcp_link_attached(etcp, link, transport)) goto done;
    etcp_keepalive_link_ready(link);
    if (!tcp_link_attached(etcp, link, transport)) goto done;
    loadbalancer_link_ready(link);
    if (!tcp_link_attached(etcp, link, transport)) goto done;
    etcp_fire_link_status_cbk(link, old_state, old_status);
    if (!tcp_link_attached(etcp, link, transport)) goto done;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] TCP link %d UP (mtu=%d init=%d up=%d)",
               etcp->log_name, link->local_link_id, link->mtu,
               etcp->initialized, etcp->links_up);
    if (etcp->instance->nat_det) nat_detection_link_ready(etcp->instance->nat_det, link);
    if (!tcp_link_attached(etcp, link, transport)) goto done;
    tcp_send_observed_ip(link);
done:
    etcp_link_release(link);
    etcp->callbacks_running--;
}

// Повторная попытка STCP-подключения с экспоненциальным backoff (до 30 с).
static void tcp_link_reconnect_cb(void *arg) {
    struct ETCP_LINK *link = (struct ETCP_LINK *)arg;
    if (!link || !link->etcp || !link->etcp->instance) return;
    link->tcp_reconnect_timer = NULL;
    if (link->closing || link->etcp->close_requested || link->etcp->state == 2) return;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] TCP link %d reconnect attempt (delay=%ums)", link->etcp->log_name, link->local_link_id, link->tcp_reconnect_delay_ms);
    etcp_tcp_link_start_connect(link, &link->remote_addr, ss_get_port(&link->remote_addr));
}

// Исходящее TCP-подключение линка: stcp_link_connect + установка close-callback.
void etcp_tcp_link_start_connect(struct ETCP_LINK *link, struct sockaddr_storage *addr, uint16_t port) {
    if (!link || !link->etcp || !addr || link->closing || link->etcp->close_requested || link->etcp->state == 2) return;
    if (link->tcp_link || link->tcp_reconnect_timer) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] TCP connect already pending path=%u", link->etcp->log_name, link->local_link_id);
        return;
    }
    /* link->conn опционален: NULL = без локального bind (ОС выбирает source IP/интерфейс).
     * Задан — bind к interface_addr этого сокета (выбор интерфейса/ip). */
    if (addr != &link->remote_addr) memcpy(&link->remote_addr, addr, sizeof(*addr));
    link->link_state = LINK_STATE_HANDSHAKE;
    struct stcp_link *sl = stcp_link_connect(link, &link->remote_addr, port);
    if (!sl) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] TCP connect startup failed path=%u", link->etcp->log_name, link->local_link_id);
        etcp_tcp_link_start_reconnect(link); return;
    }
    link->tcp_link = sl;
    if (link->is_server == 0) stcp_link_set_on_close(sl, tcp_link_close_cb, link);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] TCP link %d → stcp_link_connect %s:%u rc=%p bind=%s",
               link->etcp->log_name, link->local_link_id, sockaddr_storage_to_str(addr).str, port, (void*)sl,
               link->conn ? sockaddr_storage_to_str(&link->conn->interface_addr).str : "auto");
}

// Запланировать реконнект TCP-линка после обрыва (через tcp_reconnect_timer).
void etcp_tcp_link_start_reconnect(struct ETCP_LINK *link) {
    if (!link || !link->etcp || !link->etcp->instance || link->is_server) return;
    if (link->closing || link->etcp->close_requested || link->etcp->state == 2) return;
    if (link->tcp_reconnect_timer) return;
    if (link->tcp_link) { stcp_link_close(link->tcp_link); link->tcp_link = NULL; }
    link->initialized = link->recv_keepalive = link->link_status = 0;
    link->link_state = LINK_STATE_HANDSHAKE;
    uint32_t previous = link->tcp_reconnect_delay_ms;
    link->tcp_reconnect_delay_ms = !previous ? 1000 : previous >= 15000 ? 30000 : previous * 2;
    link->tcp_reconnect_timer = uasync_set_timeout(link->etcp->instance->ua, (int)(link->tcp_reconnect_delay_ms * 10), link, tcp_link_reconnect_cb, "tcp_rct");
    if (!link->tcp_reconnect_timer) {
        struct ETCP_CONN *conn = link->etcp;
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] TCP reconnect timer allocation failed path=%u", conn->log_name, link->local_link_id);
        conn->callbacks_running++; etcp_connection_close(conn); conn->callbacks_running--;
        return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] TCP reconnect scheduled path=%u delay_ms=%u previous_ms=%u",
        link->etcp->log_name, link->local_link_id, link->tcp_reconnect_delay_ms, previous);
}

// Callback закрытия STCP-линка: серверный линк закрываем, клиентский — отправляем на реконнект.
static void tcp_link_close_cb(struct stcp_link *sl, int err, void *arg) {
    struct ETCP_LINK *link = (struct ETCP_LINK *)arg;
    callring_put(CR_TCPCB_IN, (uintptr_t)sl, (uintptr_t)arg);
    if (!link || !link->etcp || link->closing) return;
    if (link->tcp_link != sl) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] stale TCP close ignored path=%u", link->etcp->log_name, link->local_link_id);
        stcp_link_close(sl); return;
    }
    /* Сохраняем до каскада: etcp_on_link_down может закрыть conn и освободить link */
    int old_state = link->link_state, old_status = link->link_status;
    int is_server = link->is_server;
    int local_link_id = link->local_link_id;
    struct ETCP_CONN *etcp = link->etcp;
    etcp->callbacks_running++;
    stcp_link_close(sl);
    link->tcp_link = NULL;
    etcp_link_io_update(link);

    if (is_server) {
        /* Серверный линк: etcp_link_close сам снимает link из списка conn, затем
           стреляет link_status и on_link_down, затем освобождает. Здесь коллбэки
           НЕ стреляем — преждевременный огонь создавал окно реентерабельности, в
           котором реентерабельный etcp_connection_close успевал освободить link,
           а мы после этого закрывали его повторно (use-after-free). */
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] TCP server link %d down err=%d, closing", etcp->log_name, local_link_id, err);
        callring_put(CR_TCPCB_FREE, (uintptr_t)link, 0);
        etcp_link_close(link);
    } else {
        link->initialized = link->recv_keepalive = link->link_status = 0;
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] TCP link %d down err=%d, scheduling reconnect", etcp->log_name, local_link_id, err);
        etcp_tcp_link_start_reconnect(link);
        if (!etcp->close_requested && (old_state != link->link_state || old_status))
            etcp_fire_link_status_cbk(link, old_state, old_status);
        /* Callback мог удалить link или соседний путь; агрегат пересчитывается только по живому списку. */
        if (!etcp->close_requested) etcp_on_link_down(etcp, NULL);
    }
    etcp->callbacks_running--;
}

// Единый plaintext для UDP/STCP. Эпохи DATA/ACK подписываются вместе с содержимым.
static size_t etcp_encode_plain(struct ETCP_DGRAM* dgram, uint8_t* buf) {
    size_t offset = 3, len = dgram->data_len - dgram->noencrypt_len;
    memcpy(buf, &dgram->timestamp, 3);
    if (len && dgram->data[0] <= 1) {
        etcp_session_encode(buf+offset, ETCP_SESSION_DATA, dgram->link->etcp->reset_id, dgram->link->etcp->peer_reset_id);
        offset += ETCP_SESSION_HEADER_SIZE;
    }
    memcpy(buf+offset, dgram->data, len);
    return offset+len;
}

// Отправка ETCP_DGRAM по TCP: сборка буфера (timestamp+flag_up+data) и передача в stcp_link_send.
static int etcp_tcp_send(struct ETCP_DGRAM* dgram) {
    struct ETCP_LINK *link = dgram->link;
    if (!link->tcp_link || !stcp_link_is_ready(link->tcp_link)) return -1;
    link->pkt_sent_since_keepalive = 1;
    dgram->flag_up = 1;
    dgram->timestamp = get_current_timestamp();
    uint8_t buf[PACKET_DATA_SIZE + ETCP_SESSION_HEADER_SIZE + 3];
    size_t total_len = etcp_encode_plain(dgram, buf);
    int rc = etcp_link_io_send(link, buf, total_len);
    if (rc >= 0 && !link->send_hook) rc = (int)dgram->data_len;
    if (rc > 0) link->total_encrypted += (size_t)rc;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] TCP send: link=%d total=%zu dlen=%d rc=%d via=%s",
               link->etcp->log_name, link->local_link_id, total_len, dgram->data_len, rc,
               link->send_hook ? "hook" : (link->tcp_link ? "stcp" : "NONE"));
    return rc;
}


// Единая точка отправки UDP: SOCKS5-релей (если сокет proxied) → send_hook (dummynet) → socket_sendto.
ssize_t etcp_udp_send(struct ETCP_LINK* link, socket_t fd, const void* buf, size_t len,
                       const struct sockaddr* addr, socklen_t addr_len) {
    if (link && link->conn && link->conn->instance->nat_det)
        nat_detection_note_send(link->conn->instance->nat_det, link->conn, (const struct sockaddr_storage *)addr);
    if (link && link->conn && link->conn->socks_udp)
        return socks_udp_sendto(link->conn->socks_udp, buf, len, (const struct sockaddr_storage*)addr);
    if (link && link->send_hook)
        return link->send_hook(fd, buf, len, addr, addr_len, link, link->send_hook_ctx);
    return socket_sendto(fd, buf, len, addr, addr_len);
}

// Зашифровать и отправить пакет: TCP → etcp_tcp_send, UDP → AES-CCM + socket_sendto.
int etcp_encrypt_send(struct ETCP_DGRAM* dgram) {
    if (!dgram || !dgram->link) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Null pointer"); return -1; }
    const struct etcp_link_codec* codec = dgram->link->codec;
    /* Старые вручную созданные пути привязываются при первой отправке. */
    if (!codec) codec = dgram->link->codec = &udp_codec;
    return codec->send(dgram);
}

static int etcp_datagram_send(struct ETCP_DGRAM* dgram) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_TX_STACK);

    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] Send rk=%d lk=%d up=%d",
                    dgram->link->etcp->log_name, dgram->link->recv_keepalive, dgram->link->remote_keepalive, dgram->link->link_status);

    // Mark that packet was sent (for keepalive logic)
    dgram->link->pkt_sent_since_keepalive = 1;
    dgram->flag_up=dgram->link->recv_keepalive;
// Размер зашифрованной части не должен превышать UDP payload = MTU - заголовки
    int errcode=0;
    sc_context_t* sc = &dgram->link->etcp->crypto_ctx;
    int len=dgram->data_len-dgram->noencrypt_len;
    int udp_ov = etcp_link_network_overhead(dgram->link);
    int wire_ov = udp_ov + SC_ENCRYPT_OVERHEAD;
    if (len<0 || len + (int)dgram->noencrypt_len > (int)(dgram->link->mtu - wire_ov)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] packet exceeds MTU link=%u body=%u tail=%u overhead=%d mtu=%u",
                    dgram->link->etcp->log_name, dgram->link->local_link_id, dgram->data_len, dgram->noencrypt_len,
                    wire_ov, dgram->link->mtu);
        dgram->link->send_errors++; errcode=1; goto es_err;
    }
    uint8_t enc_buf[PACKET_DATA_MAX_MTU];// макс запись = data_len + 32 <= mtu - wire_ov + 32 = 1468
    size_t enc_buf_len=0;
    dgram->timestamp=get_current_timestamp();
    
    dgram->link->total_encrypted += dgram->data_len;
    uint8_t plain[PACKET_DATA_SIZE + ETCP_SESSION_HEADER_SIZE + 3];
    size_t plain_len = etcp_encode_plain(dgram, plain);
    if (dgram->link->conn && dgram->link->conn->transport && !dgram->link->send_hook) {
        struct ETCP_LINK* link = dgram->link;
        if (publish_udp_key(link)) return -1;
        if (link->conn->socks_udp) {
            struct sockaddr_storage relay;
            packet_transport_relay(link->conn->transport, socks_udp_get_relay(link->conn->socks_udp, &relay) ? &relay : NULL);
        }
        uint64_t attempt_id = dgram->data[0] == ETCP_SECTION_ACK && dgram->attempt ? dgram->attempt->attempt_id : 0;
        struct packet_transport_tag tag = {link->etcp->peer_node_id, link->etcp->reset_id, attempt_id, link->local_link_id};
        int rc = packet_transport_send(link->conn->transport, &link->remote_addr, sc, plain, plain_len,
            dgram->data + len, dgram->noencrypt_len, &tag);
        if (!rc && link->conn->instance->nat_det)
            nat_detection_note_send(link->conn->instance->nat_det, link->conn, &link->remote_addr);
        return rc ? rc : (int)(plain_len + SC_NONCE_SIZE + SC_TAG_SIZE + SC_CRC32_SIZE + dgram->noencrypt_len);
    }
    if (dgram->payload_len) ETCP_PROFILE_MARK(ETCP_PROF_ENCRYPT_BEGIN, dgram->link->etcp);
    ETCP_PROFILE_STAGE(ETCP_PROF_ENCRYPT);
    sc_status_t encrypted = sc_encrypt(sc, plain, plain_len, enc_buf, &enc_buf_len);
    ETCP_PROFILE_STAGE(ETCP_PROF_TX_POST);
    if (encrypted != SC_OK || enc_buf_len == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] packet encryption failed link=%u plain=%zu result=%d encrypted=%zu",
                    dgram->link->etcp->log_name, dgram->link->local_link_id, plain_len, encrypted, enc_buf_len);
        dgram->link->send_errors++; errcode=2; goto es_err; }
    if (enc_buf_len + dgram->noencrypt_len > (size_t)(dgram->link->mtu - udp_ov)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "packet too long enc=%zu ne=%d mtu=%d", enc_buf_len, dgram->noencrypt_len, dgram->link->mtu);
        dgram->link->send_errors++; errcode=3; goto es_err; }
    memcpy(enc_buf+enc_buf_len, dgram->data+len, dgram->noencrypt_len);
    
    if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CRYPTO)) log_dump(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CRYPTO, "Encrypted", enc_buf, enc_buf_len + dgram->noencrypt_len);
    
    int sent = etcp_link_io_send(dgram->link, enc_buf, enc_buf_len + dgram->noencrypt_len);
    if (sent == -2) return -2;
    if (sent < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] packet submission failed link=%u wire=%zu result=%d",
                    dgram->link->etcp->log_name, dgram->link->local_link_id, enc_buf_len + dgram->noencrypt_len, sent);
        dgram->link->send_errors++; errcode=4; goto es_err;
    }
    return (int)sent;
es_err:
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] packet send failed link=%u phase=%d",
                dgram->link->etcp->log_name, dgram->link->local_link_id, errcode);
    return -1;
}

// Шифрует и отправляет готовый PING/PONG-датаграмм без линка (сырой sendto по сокету).
// Для proxied-сокета идёт через SOCKS5 UDP-релей (dst = addr пира).
static int etcp_send_ping_raw(struct ETCP_DGRAM* dgram, struct ETCP_SOCKET* e_sock, sc_context_t* sc, const struct sockaddr_storage* addr) {
    if (!dgram || !e_sock || !sc || !addr) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Null pointer in ping send");
        return -1;
    }
    int len = dgram->data_len - dgram->noencrypt_len;
    uint8_t enc_buf[1600];
    struct ETCP_LINK* link = etcp_link_find_by_addr(e_sock, addr, 0);
    int mtu = link ? link->mtu : PACKET_DATA_MAX_MTU;
    int udp_overhead = addr->ss_family == AF_INET6 ? IP_UDP_OVERHEAD_V6 : IP_UDP_OVERHEAD_V4;
    int available = mtu - udp_overhead - len - 3 - SC_NONCE_SIZE - SC_CRC32_SIZE - SC_TAG_SIZE - dgram->noencrypt_len;
    if (len < 0 || available < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "ping exceeds MTU mtu=%d body=%d tail=%u addr=%s",
                    mtu, len, dgram->noencrypt_len, sockaddr_storage_to_str(addr).str);
        return -1;
    }
    const struct ETCP_PADDING* profile = etcp_padding_for_peer(e_sock->instance, sc->peer_public_key);
    int padding = etcp_padding_size(profile, available);
    if (padding < 0) return -1;
    memmove(dgram->data + len + padding, dgram->data + len, dgram->noencrypt_len);
    memset(dgram->data + len, 0, padding);
    dgram->data_len += padding;
    len += padding;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "ping TX code=%u owner=%s range=%u..%u padding=%d body=%d mtu=%d",
                dgram->data[0], profile == &e_sock->instance->padding ? "instance" : "connection",
                profile->min, profile->max, padding, len, mtu);
    if (len < 0 || len + (int)dgram->noencrypt_len + SC_ENCRYPT_OVERHEAD > (int)sizeof(enc_buf)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "ping packet too large len=%d ne=%d", len, dgram->noencrypt_len);
        return -1;
    }
    size_t enc_buf_len = 0;
    dgram->timestamp = get_current_timestamp();
    dgram->flag_up = 1;
    uint8_t plain[PACKET_DATA_SIZE + ETCP_SESSION_HEADER_SIZE + 3];
    size_t plain_len = etcp_encode_plain(dgram, plain);
    if (e_sock->transport) {
        if (e_sock->socks_udp) {
            struct sockaddr_storage relay;
            packet_transport_relay(e_sock->transport, socks_udp_get_relay(e_sock->socks_udp, &relay) ? &relay : NULL);
        }
        int rc = packet_transport_send(e_sock->transport, addr, sc, plain, plain_len,
            dgram->data + len, dgram->noencrypt_len, NULL);
        if (rc) DEBUG_WARN(DEBUG_CATEGORY_ETCP, "UDP probe queue rejected code=%u peer=%s socket=%s bytes=%zu rc=%d",
            dgram->data[0], sockaddr_storage_to_str(addr).str, e_sock->name, plain_len, rc);
        if (!rc && e_sock->instance->nat_det) nat_detection_note_send(e_sock->instance->nat_det, e_sock, addr);
        return rc ? rc : (int)(plain_len + SC_NONCE_SIZE + SC_TAG_SIZE + SC_CRC32_SIZE + dgram->noencrypt_len);
    }
    sc_encrypt(sc, plain, plain_len, enc_buf, &enc_buf_len);
    if (enc_buf_len == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "encryption failed for ping");
        return -1;
    }
    if (enc_buf_len + dgram->noencrypt_len > (size_t)(PACKET_DATA_SIZE - IP_UDP_OVERHEAD_V4)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "ping packet too long enc=%zu ne=%d", enc_buf_len, dgram->noencrypt_len);
        return -1;
    }
    memcpy(enc_buf + enc_buf_len, dgram->data + len, dgram->noencrypt_len);
    size_t total = enc_buf_len + dgram->noencrypt_len;
    if (e_sock->instance->nat_det) nat_detection_note_send(e_sock->instance->nat_det, e_sock, addr);
    ssize_t sent;
    if (e_sock->socks_udp) {
        sent = socks_udp_sendto(e_sock->socks_udp, enc_buf, total, addr);
        if (sent < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "ping via socks failed addr=%s len=%zu", sockaddr_storage_to_str(addr).str, total);
            return -1;
        }
        return (int)sent;
    }
    socklen_t addr_len = (addr->ss_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    sent = socket_sendto(e_sock->fd, enc_buf, total, (struct sockaddr*)addr, addr_len);
    if (sent < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "sendto failed for ping, err=%d addr=%s fd=%d len=%zu", socket_get_error(), sockaddr_storage_to_str(addr).str, e_sock->fd, total);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "ping sendto succeeded to %s sent=%zd bytes fd=%d", sockaddr_storage_to_str(addr).str, sent, e_sock->fd);
    return (int)sent;
}

// Копирует пользовательские данные запроса в ответ (эхо) с ограничением буфера.
static size_t etcp_build_ping_response_data(const uint8_t* req_data, size_t req_data_len,
                                            uint8_t* resp_buf, size_t resp_buf_len) {
    if (req_data_len > resp_buf_len) req_data_len = resp_buf_len;
    if (req_data_len && req_data) memcpy(resp_buf, req_data, req_data_len);
    return req_data_len;
}

// Таймаут пинга: снять из pending_pings и вызвать callback с success=0.
static void ping_timeout_cbk(void* arg) {
    struct PING_CONTEXT* ctx = (struct PING_CONTEXT*)arg;
    if (!ctx) return;
    ctx->timeout_timer = NULL;
    if (ctx->instance->pending_pings == ctx) {
        ctx->instance->pending_pings = ctx->next;
    } else {
        struct PING_CONTEXT* prev = ctx->instance->pending_pings;
        while (prev && prev->next != ctx) prev = prev->next;
        if (prev) prev->next = ctx->next;
    }
    if (ctx->cb) ctx->cb(0, 0, ctx->arg, ctx->nonce, NULL, 0);
    if (ctx->user_data) u_free(ctx->user_data);
    u_free(ctx);
}

void etcp_cancel_pings(struct UTUN_INSTANCE *instance, etcp_ping_callback_t callback, void *arg) {
    if (!instance) return;
    struct PING_CONTEXT **cursor = &instance->pending_pings;
    while (*cursor) {
        struct PING_CONTEXT *ping = *cursor;
        if (ping->cb != callback || ping->arg != arg) { cursor = &ping->next; continue; }
        *cursor = ping->next;
        if (ping->timeout_timer) uasync_cancel_timeout(instance->ua, ping->timeout_timer);
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "UDP probe cancelled nonce=%016llx socket=%u generation=%016llx",
                    (unsigned long long)ping->nonce, ping->socket_id, (unsigned long long)ping->socket_generation);
        u_free(ping->user_data); u_free(ping);
    }
}

// Отправка UDP-пинга через конкретный сокет: сборка PING-кодограммы (nonce, user_data,
// обфусцированный pubkey), регистрация в pending_pings с таймаутом.
int etcp_send_ping_to_socket(struct UTUN_INSTANCE* instance, struct ETCP_SOCKET* e_sock,
                               const uint8_t* peer_pubkey_bin, const struct sockaddr_storage* addr,
                               int timeout_ms, etcp_ping_callback_t cb, void* user_arg,
                               const uint8_t* user_data, size_t user_data_len,
                               uint8_t flags) {
    if (!instance || !e_sock || !peer_pubkey_bin || !addr || timeout_ms <= 0 || !cb) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "bad args");
        return -1;
    }
    if (e_sock->is_tcp || e_sock->fd == SOCKET_INVALID) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "UDP reachability probe rejected: selected socket is %s; "
                    "socket_name=%s fd=%d local=%s target=%s peer=%016llx timeout_ms=%d",
                    e_sock->is_tcp ? "TCP (UDP socket required)" : "closed", e_sock->name, (int)e_sock->fd,
                    sockaddr_storage_to_str(&e_sock->local_addr).str, sockaddr_storage_to_str(addr).str,
                    (unsigned long long)sc_derive_node_id_from_pubkey(peer_pubkey_bin), timeout_ms);
        return -2;
    }
    if ((user_data_len && !user_data) || user_data_len > PACKET_DATA_SIZE - sizeof(struct ETCP_PING_HDR) - SC_PUBKEY_ENC_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "user_data too long");
        return -2;
    }
    struct PING_CONTEXT* ctx = u_malloc(sizeof(struct PING_CONTEXT));
    if (!ctx) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "malloc ctx");
        return -3;
    }
    ctx->next = NULL;
    ctx->instance = instance;
    ctx->cb = cb;
    ctx->arg = user_arg;
    do {
        if (random_bytes((uint8_t *)&ctx->nonce, sizeof(ctx->nonce))) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "UDP ping nonce RNG failed"); u_free(ctx); return -3;
        }
        struct PING_CONTEXT *pending = instance->pending_pings;
        while (pending && pending->nonce != ctx->nonce) pending = pending->next;
        if (ctx->nonce && !pending) break;
    } while (1);
    ctx->socket_id = e_sock->sock_id; ctx->socket_generation = e_sock->generation; ctx->peer_addr = *addr;
    ctx->timeout_timer = NULL;
    ctx->send_time = get_time_tb();
    ctx->user_data = NULL;
    ctx->user_data_len = 0;
    if (user_data_len > 0) {
        ctx->user_data = u_malloc(user_data_len);
        if (!ctx->user_data) {
            u_free(ctx);
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "malloc user_data");
            return -3;
        }
        memcpy(ctx->user_data, user_data, user_data_len);
        ctx->user_data_len = user_data_len;
    }
    memcpy(ctx->peer_pubkey, peer_pubkey_bin, SC_PUBKEY_SIZE);
    struct ETCP_DGRAM* dgram = u_malloc(sizeof(struct ETCP_DGRAM) + PACKET_DATA_SIZE);
    if (!dgram) {
        if (ctx->user_data) u_free(ctx->user_data);
        u_free(ctx);
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "malloc dgram");
        return -4;
    }
    dgram->link = NULL;
    dgram->noencrypt_len = SC_PUBKEY_ENC_SIZE;
    struct ETCP_PING_HDR* ping = (struct ETCP_PING_HDR*)dgram->data;
    ping->code = ETCP_PING; ping->flags = flags;
    memcpy(ping->ed25519, instance->my_ed25519_pubkey, sizeof(ping->ed25519));
    wire_write64(ping->nonce, ctx->nonce); wire_write16(ping->size, (uint16_t)user_data_len);
    uint8_t* p = (uint8_t*)(ping + 1);
    if (user_data_len) {
        memcpy(p, user_data, user_data_len);
        p += user_data_len;
    }
    uint8_t salt[SC_PUBKEY_ENC_SALT_SIZE];
    random_bytes(salt, sizeof(salt));
    memcpy(p, salt, SC_PUBKEY_ENC_SALT_SIZE); p += SC_PUBKEY_ENC_SALT_SIZE;
    uint8_t obfuscated_pubkey[SC_PUBKEY_SIZE];
    sc_obfuscate_pubkey(salt, peer_pubkey_bin, instance->my_keys.public_key, obfuscated_pubkey);
    memcpy(p, obfuscated_pubkey, SC_PUBKEY_SIZE); p += SC_PUBKEY_SIZE;
    dgram->data_len = (uint16_t)(p - dgram->data);
    struct secure_channel sc;
    sc_init_ctx(&sc, &instance->my_keys);
    if (sc_set_peer_public_key(&sc, peer_pubkey_bin, SC_PEER_PUBKEY_BIN) != SC_OK) {
        u_free(dgram);
        if (ctx->user_data) u_free(ctx->user_data);
        u_free(ctx);
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "set key failed");
        return -5;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "ping send nonce=%016llx timeout=%d ulen=%zu",
               (unsigned long long)ctx->nonce, timeout_ms, user_data_len);
    int send_rc = etcp_send_ping_raw(dgram, e_sock, &sc, addr);
    u_free(dgram);
    if (send_rc < 0) {
        if (ctx->user_data) u_free(ctx->user_data);
        u_free(ctx);
        return -6;
    }
    ctx->timeout_timer = uasync_set_timeout(instance->ua, timeout_ms * 10, ctx, ping_timeout_cbk, "ping_timeout");
    if (!ctx->timeout_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "UDP ping timeout allocation failed socket=%s nonce=%016llx timeout=%d",
                    e_sock->name, (unsigned long long)ctx->nonce, timeout_ms);
        u_free(ctx->user_data); u_free(ctx); return -7;
    }
    if (instance->pending_pings == NULL) {
        instance->pending_pings = ctx;
    } else {
        struct PING_CONTEXT* last = instance->pending_pings;
        while (last->next) last = last->next;
        last->next = ctx;
    }
    return 0;
}

// Отправка UDP-пинга: подбор сокета по family адреса, делегирование в etcp_send_ping_to_socket.
int etcp_send_ping(struct UTUN_INSTANCE* instance, const uint8_t* peer_pubkey_bin,
                   const struct sockaddr_storage* addr, int timeout_ms,
                   etcp_ping_callback_t cb, void* user_arg,
                   const uint8_t* user_data, size_t user_data_len) {
    if (!instance || !peer_pubkey_bin || !addr || timeout_ms <= 0 || !cb) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "bad args: inst=%p pubkey=%p addr=%p timeout=%d cb=%p udata=%p udata_len=%zu",
                   (void*)instance, (void*)peer_pubkey_bin, (void*)addr, timeout_ms,
                   (void*)(uintptr_t)cb, (void*)user_data, user_data_len);
        return -1;
    }
    struct ETCP_SOCKET* e_sock = instance->etcp_sockets;
    while (e_sock && (e_sock->is_tcp || e_sock->probe_only || e_sock->local_addr.ss_family != addr->ss_family)) e_sock = e_sock->next;
    if (!e_sock) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "no UDP socket for addr_family=%d", addr->ss_family);
        return -2;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "ping N1 [%s]", sockaddr_storage_to_str(addr).str);
    return etcp_send_ping_to_socket(instance, e_sock, peer_pubkey_bin, addr, timeout_ms,
                                     cb, user_arg, user_data, user_data_len, 0);
}

struct tcp_ping_adapter {
    etcp_ping_callback_t cb;
    void* arg;
};

// Адаптер: приводит callback STCP-пинга к типу etcp_ping_callback_t.
static void tcp_ping_cb_adapter(int success, uint16_t rtt, void* arg) {
    struct tcp_ping_adapter* a = (struct tcp_ping_adapter*)arg;
    a->cb(success, rtt, a->arg, 0, NULL, 0);
    u_free(a);
}

// TCP-пинг: STCP-хендшейк с пиром и замер RTT по handshake (через stcp_ping_send).
int etcp_send_tcp_ping(struct UTUN_INSTANCE* instance, const uint8_t* peer_pubkey_bin,
                       const struct sockaddr_storage* addr, int timeout_ms,
                       etcp_ping_callback_t cb, void* user_arg,
                       struct stcp_client** out_cli, const struct reality_client_config *reality) {
    if (!instance || !peer_pubkey_bin || !addr || timeout_ms <= 0 || !cb) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "bad args: inst=%p pubkey=%p addr=%p timeout=%d cb=%p",
                    (void*)instance, (void*)peer_pubkey_bin, (void*)addr, timeout_ms, (void*)(uintptr_t)cb);
        return -1;
    }
    if (out_cli) *out_cli = NULL;
    char addr_str[INET6_ADDRSTRLEN];
    uint16_t port;
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in* sin = (const struct sockaddr_in*)addr;
        snprintf(addr_str, sizeof(addr_str), "%s", ip_to_str(&sin->sin_addr, AF_INET).str);
        port = ntohs(sin->sin_port);
    } else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)addr;
        snprintf(addr_str, sizeof(addr_str), "%s", ip_to_str(&sin6->sin6_addr, AF_INET6).str);
        port = ntohs(sin6->sin6_port);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "unsupported addr family=%d", addr->ss_family);
        return -2;
    }
    struct tcp_ping_adapter* a = u_malloc(sizeof(struct tcp_ping_adapter));
    if (!a) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "malloc adapter"); return -3; }
    a->cb = cb; a->arg = user_arg;

    struct socks_cfg socks_buf;
    const struct socks_cfg* socks = NULL;
    if (instance->config && instance->config->global.socks_enabled &&
        instance->config->global.socks_host[0] && instance->config->global.socks_port) {
        memset(&socks_buf, 0, sizeof(socks_buf));
        strncpy(socks_buf.host, instance->config->global.socks_host, sizeof(socks_buf.host) - 1);
        socks_buf.port = instance->config->global.socks_port;
        strncpy(socks_buf.user, instance->config->global.socks_username, sizeof(socks_buf.user) - 1);
        strncpy(socks_buf.pass, instance->config->global.socks_password, sizeof(socks_buf.pass) - 1);
        socks = &socks_buf;
    }

    struct stcp_client* cli = stcp_ping_send(instance->ua, addr_str, port, &instance->my_keys, peer_pubkey_bin,
                                             instance->my_ed25519_pubkey, instance->client_type,
                                             instance->keepalive_interval, timeout_ms, tcp_ping_cb_adapter, a, socks, instance, reality);
    if (!cli) { u_free(a); return -4; }
    if (out_cli) *out_cli = cli;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "tcp ping to %s:%u timeout=%d", addr_str, (unsigned)port, timeout_ms);
    return 0;
}

// === Helpers extracted from etcp_connections_read_callback_socket ===

// Обработка входящего PING: обновление RTT (если флаг SEND_RTT), ответ PONG с эхо-данными.
static int handle_ping(struct ETCP_SOCKET* e_sock, struct ETCP_DGRAM* pkt, const struct sockaddr_storage* addr, const uint8_t* decrypted_pubkey) {
    const struct ETCP_PING_HDR* ping = (const struct ETCP_PING_HDR*)pkt->data;
    uint8_t flags = ping->flags;
    uint64_t nonce = wire_read64(ping->nonce);
    uint16_t ulen = wire_read16(ping->size);
    size_t available = pkt->data_len - sizeof(*ping);
    if (ulen > available || available - ulen > ETCP_PADDING_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "invalid PING user_len=%u body=%u from=%s", ulen, pkt->data_len, sockaddr_storage_to_str(addr).str);
        return 7;
    }
    const uint8_t* udata = ulen ? (const uint8_t*)(ping + 1) : NULL;

    if ((flags & ETCP_PING_FLAG_OBSERVE) && (flags & ETCP_PING_FLAG_ADDRESS_CHECK)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "UDP probe has incompatible flags=%02x", flags); return 7;
    }
    if ((flags & ETCP_PING_FLAG_NO_REPLY) &&
        (!(flags & (ETCP_PING_FLAG_OBSERVE | ETCP_PING_FLAG_ADDRESS_CHECK)) || ulen != ROUTE_OBSERVE_TAG_SIZE + 9)) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "One-way NAT probe has invalid flags=%02x tag_size=%u", flags, ulen); return 7;
    }
    if (!nat_detection_ping_allowed(e_sock->instance->nat_det, e_sock, decrypted_pubkey, addr, flags, udata, ulen)) {
        memory_pool_free(e_sock->instance->pkt_pool, pkt); return 0;
    }
    if ((flags & ETCP_PING_FLAG_OBSERVE) && e_sock->instance->nat_det) {
        nat_detection_record_observation(e_sock->instance->nat_det, e_sock, decrypted_pubkey, addr, udata, ulen);
    }
    if ((flags & ETCP_PING_FLAG_ADDRESS_CHECK) && e_sock->instance->nat_det)
        nat_detection_receive_ingress(e_sock->instance->nat_det, e_sock, decrypted_pubkey, addr, udata, ulen);

    if (flags & ETCP_PING_FLAG_NO_REPLY) {
        memory_pool_free(e_sock->instance->pkt_pool, pkt); return 0;
    }

    if ((flags & ETCP_PING_FLAG_SEND_RTT) && ulen >= 2) {
        uint16_t rtt_val = wire_read16(udata);
        topo_node_rtt_probe_sample(e_sock->instance, decrypted_pubkey, rtt_val, addr);
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "PING rtt=%u from peer=%s", (unsigned)rtt_val, sc_public_key_hex(ping->ed25519, (char[65]){0}));
    }

    uint8_t pong_flags = 0;
    if (e_sock->instance->topo_groups && topo_node_ping_request_cbk(e_sock->instance->topo_groups, sc_derive_node_id_from_pubkey(decrypted_pubkey)))
        pong_flags |= ETCP_PING_FLAG_WANT_RTT;

    struct ETCP_DGRAM* resp = u_malloc(sizeof(struct ETCP_DGRAM) + PACKET_DATA_SIZE);
    if (resp) {
        resp->link = NULL;
        resp->noencrypt_len = SC_PUBKEY_ENC_SIZE;
        struct ETCP_PING_HDR* pong = (struct ETCP_PING_HDR*)resp->data;
        pong->code = ETCP_PONG; pong->flags = pong_flags;
        memcpy(pong->ed25519, e_sock->instance->my_ed25519_pubkey, sizeof(pong->ed25519));
        wire_write64(pong->nonce, nonce);
        size_t response_size = ulen + ((flags & (ETCP_PING_FLAG_OBSERVE | ETCP_PING_FLAG_ADDRESS_CHECK)) ? ROUTE_ADDR_WIRE_SIZE : 0);
        if (response_size > PACKET_DATA_SIZE - sizeof(*pong) - SC_PUBKEY_ENC_SIZE) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "PONG payload too large size=%zu", response_size); u_free(resp); return 7;
        }
        wire_write16(pong->size, (uint16_t)response_size);
        uint8_t* p = (uint8_t*)(pong + 1);
        if (flags & (ETCP_PING_FLAG_OBSERVE | ETCP_PING_FLAG_ADDRESS_CHECK)) {
            if (route_ping_addr_encode(p, addr)) { u_free(resp); return 7; }
            p += ROUTE_ADDR_WIRE_SIZE;
        }
        size_t copied = etcp_build_ping_response_data(udata, ulen, p, PACKET_DATA_SIZE - (p - resp->data) - SC_PUBKEY_ENC_SIZE);
        p += copied;
        uint8_t salt[SC_PUBKEY_ENC_SALT_SIZE];
        random_bytes(salt, sizeof(salt));
        memcpy(p, salt, SC_PUBKEY_ENC_SALT_SIZE); p += SC_PUBKEY_ENC_SALT_SIZE;
        uint8_t obfuscated_pubkey[SC_PUBKEY_SIZE];
        sc_obfuscate_pubkey(salt, decrypted_pubkey, e_sock->instance->my_keys.public_key, obfuscated_pubkey);
        memcpy(p, obfuscated_pubkey, SC_PUBKEY_SIZE); p += SC_PUBKEY_SIZE;
        resp->data_len = (uint16_t)(p - resp->data);
        sc_context_t resp_sc;
        sc_init_ctx(&resp_sc, &e_sock->instance->my_keys);
        if (sc_set_peer_public_key(&resp_sc, decrypted_pubkey, SC_PEER_PUBKEY_BIN) == SC_OK) {
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "PONG send nonce=%016llx flags=%02x to=%s fd=%d",
                       (unsigned long long)nonce, (unsigned)pong_flags, sockaddr_storage_to_str(addr).str, e_sock->fd);
            etcp_send_ping_raw(resp, e_sock, &resp_sc, addr);
        }
        u_free(resp);
    }
    memory_pool_free(e_sock->instance->pkt_pool, pkt);
    return 0;
}

// Пустой callback для RTT-ответа (получателю RTT-пинга ответ не важен).
static void rtt_send_ping_cb(int success, uint16_t rtt, void* arg, uint64_t nonce,
                              const uint8_t* resp_data, size_t resp_data_len) {
    (void)success; (void)rtt; (void)arg; (void)nonce; (void)resp_data; (void)resp_data_len;
}

// Обработка входящего PONG: поиск pending_pings по nonce, вызов callback с RTT и данными.
static int handle_pong(struct ETCP_SOCKET* e_sock, struct ETCP_DGRAM* pkt, const struct sockaddr_storage* addr,
                        const uint8_t *decrypted_pubkey) {
    const struct ETCP_PING_HDR* pong = (const struct ETCP_PING_HDR*)pkt->data;
    uint8_t flags = pong->flags;
    uint64_t nonce = wire_read64(pong->nonce);
    uint16_t ulen = wire_read16(pong->size);
    size_t available = pkt->data_len - sizeof(*pong);
    if (ulen > available || available - ulen > ETCP_PADDING_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "invalid PONG user_len=%u body=%u from=%s", ulen, pkt->data_len, sockaddr_storage_to_str(addr).str);
        return 7;
    }
    const uint8_t* udata = ulen ? (const uint8_t*)(pong + 1) : NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "PONG recv nonce=%016llx flags=%02x data_len=%u from=%s socket=%s",
               (unsigned long long)nonce, (unsigned)flags, (unsigned)pkt->data_len,
               sockaddr_storage_to_str(addr).str, e_sock->name);
    struct PING_CONTEXT* ctx = e_sock->instance->pending_pings;
    struct PING_CONTEXT* prev = NULL;
    int found = 0;
    while (ctx) {
        if (ctx->nonce == nonce) {
            if (ctx->socket_id != e_sock->sock_id || ctx->socket_generation != e_sock->generation ||
                memcmp(ctx->peer_pubkey, decrypted_pubkey, SC_PUBKEY_SIZE) ||
                !route_ping_addr_equal(&ctx->peer_addr, addr, 1)) {
                DEBUG_WARN(DEBUG_CATEGORY_ETCP, "UDP PONG identity/endpoint mismatch nonce=%016llx socket=%u generation=%016llx",
                            (unsigned long long)nonce, e_sock->sock_id, (unsigned long long)e_sock->generation);
                memory_pool_free(e_sock->instance->pkt_pool, pkt); return 0;
            }
            found = 1;
            if (prev) prev->next = ctx->next;
            else e_sock->instance->pending_pings = ctx->next;
            if (ctx->timeout_timer) {
                uasync_cancel_timeout(e_sock->instance->ua, ctx->timeout_timer);
                ctx->timeout_timer = NULL;
            }
            uint64_t now = get_time_tb();
            uint16_t rtt = (now >= ctx->send_time) ? (uint16_t)(now - ctx->send_time) : 0;
            if (!rtt) rtt = 1; /* Успешный loopback-замер не равен отсутствию RTT. */
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "PONG matched nonce=%016llx rtt=%u want_rtt=%d",
                       (unsigned long long)nonce, (unsigned)rtt, (flags & ETCP_PING_FLAG_WANT_RTT) ? 1 : 0);

            topo_node_rtt_probe_sample(e_sock->instance, decrypted_pubkey, rtt, addr);

            ctx->cb(1, rtt, ctx->arg, nonce, udata, ulen);

            if ((flags & ETCP_PING_FLAG_WANT_RTT) && memcmp(ctx->peer_pubkey, (const uint8_t[32]){0}, SC_PUBKEY_SIZE)) {
                uint8_t rtt_buf[2];
                rtt_buf[0] = (uint8_t)(rtt >> 8);
                rtt_buf[1] = (uint8_t)(rtt & 0xFF);
                etcp_send_ping_to_socket(e_sock->instance, e_sock, ctx->peer_pubkey, addr, 1000,
                                          rtt_send_ping_cb, NULL, rtt_buf, 2, ETCP_PING_FLAG_SEND_RTT);
            }
            if (ctx->user_data) u_free(ctx->user_data);
            u_free(ctx);
            break;
        }
        prev = ctx;
        ctx = ctx->next;
    }
    if (!found) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "PONG nonce=%016llx NOT FOUND in pending (timeout?)",
                   (unsigned long long)nonce);
    }
    memory_pool_free(e_sock->instance->pkt_pool, pkt);
    return 0;
}

/* Принятый RX доказывает прямую достижимость; обратную сообщает только входной flag_up.
 * Вызывается до переиспользования pkt для ответа, без callbacks; возвращает прежний статус. */
static int etcp_link_record_rx(struct ETCP_LINK* link, const struct ETCP_DGRAM* pkt) {
    int previous = link->link_status;
    link->recv_keepalive = 1;
    link->remote_keepalive = pkt->flag_up;
    link->link_status = link->is_tcp ? 1 : link->remote_keepalive;
    link->last_recv_local_time = pkt->received_us ? pkt->received_us / 100 : get_time_tb();
    link->last_recv_timestamp = pkt->timestamp;
    link->last_recv_updated = 1;
    link->total_decrypted += pkt->data_len;
    return previous;
}

/* Проверка заимствованного link после callback: conn удерживается вызывающим, сам link мог быть удалён. */
int etcp_link_is_alive(struct ETCP_CONN* conn, struct ETCP_LINK* link) {
    if (conn->close_requested || link->closing) return 0;
    for (struct ETCP_LINK* current = conn->links; current; current = current->next) if (current == link) return 1;
    return 0;
}

/* Публикация готового состояния: callbacks могут запросить close или удалить линк.
 * Вызывающий удерживает conn через callbacks_running; 0 запрещает дальнейшее использование link. */
static int etcp_link_publish_rx(struct ETCP_LINK* link, int old_state, int old_status) {
    struct ETCP_CONN* conn = link->etcp;
    if (conn->close_requested) return 0;
    if (old_state != link->link_state || old_status != link->link_status)
        etcp_fire_link_status_cbk(link, old_state, old_status);
    if (!etcp_link_is_alive(conn, link)) return 0;
    if (old_status != link->link_status)
        DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] RX link=%u status=%d->%u recv=%u remote=%u state=%u init=%u",
            conn->log_name, link->local_link_id, old_status, link->link_status, link->recv_keepalive,
            link->remote_keepalive, link->link_state, link->initialized);
    if (old_status && !link->link_status) etcp_on_link_down(conn, link);
    else if (link->initialized && link->link_status && (old_status != link->link_status || old_state != link->link_state))
        loadbalancer_link_ready(link);
    return etcp_link_is_alive(conn, link);
}

// Сервер: принять RX INIT, отправить ответ и запустить keepalive даже при неподтверждённом обратном пути.
static void send_init_response(struct ETCP_SOCKET* e_sock, struct ETCP_DGRAM* pkt, struct ETCP_LINK* link, struct ETCP_CONN* conn, const struct ETCP_INIT_REQUEST_PKT* req, const struct sockaddr_storage* addr, uint8_t send_reset, uint32_t req_src_ip, uint16_t req_src_port, size_t pkt_len) {
    if (conn->close_requested) { memory_pool_free(conn->instance->pkt_pool, pkt); return; }
    conn->callbacks_running++;
    link->active_users++;
    int old_state = link->link_state, old_status = etcp_link_record_rx(link, pkt);
    uint8_t client_socket_id = req->socket_id;
    if (e_sock) link->is_server = 1;
    link->initialized = 1;
    link->link_state = LINK_STATE_CONNECTED;
    if (link->init_timer) { uasync_cancel_timeout(conn->instance->ua, link->init_timer); link->init_timer = NULL; }
    struct ETCP_INIT_RESPONSE_PKT* resp = (struct ETCP_INIT_RESPONSE_PKT*)pkt->data;

    // Set response code: 0x03 (with reset) or 0x05 (without reset)
    // response with init (0x03) only if reinit was actually done on server side
    if (send_reset != 0) {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "send init_response with reset");
        resp->code = ETCP_INIT_RESPONSE;  // 0x03 - with reset
    } else {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "send init_response without reset");
        resp->code = ETCP_INIT_RESPONSE_NOINIT;  // 0x05 - without reset
    }
    wire_write64(resp->reset_id,conn->reset_id);

    resp->mtu[0]=link->mtu_local>>8;
    resp->mtu[1]=link->mtu_local;
    resp->link_id = link->local_link_id;
    resp->remote_socket_id = (e_sock ? e_sock->sock_id : 0);
    resp->only_local = (e_sock ? e_sock->only_local : 0);
    resp->type = (e_sock ? e_sock->type : CFG_SERVER_TYPE_UNKNOWN);
    /* Наблюдение относится к клиенту; адрес сервера хранится только в remote_addr. */
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *source = (const struct sockaddr_in *)addr;
        memcpy(resp->peer_ipv4, &source->sin_addr.s_addr, 4);
        memcpy(resp->peer_port, &source->sin_port, 2);
    } else {
        memset(resp->peer_ipv4, 0, sizeof(resp->peer_ipv4));
        memset(resp->peer_port, 0, sizeof(resp->peer_port));
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "INIT client address claim=%s:%u observed_peer=%s bytes=%zu server_socket=%s client_sid=%u",
                ip_to_str(&req_src_ip, AF_INET).str, req_src_port, sockaddr_storage_to_str(addr).str, pkt_len,
                (e_sock ? e_sock->name : "packet"), client_socket_id);
    memcpy(resp->ed25519_pubkey, conn->instance->my_ed25519_pubkey, SC_PUBKEY_SIZE);
    resp->device_type = conn->instance->client_type;
    wire_write16(resp->keepalive,conn->instance->keepalive_interval);
    pkt->attempt = NULL; pkt->payload_len = 0;
    pkt->noencrypt_len=0;
    pkt->link=link;

    int xoffset=sizeof(struct ETCP_INIT_RESPONSE_PKT);
    // padding
    int wire_ov = (etcp_link_network_overhead(link) + SC_ENCRYPT_OVERHEAD);
    int max_data = link->mtu - wire_ov;
    int pad_range = (int)link->handshake_maxsize - (int)link->handshake_minsize;
    if (pad_range <= 0) pad_range = 1;
    int s = rand() % pad_range + link->handshake_minsize;
    if (s > (int)(link->mtu)) s = (int)(link->mtu);
    if (s < 0) s = 0;

    int to_add = s - xoffset - wire_ov;
    if (to_add < 0) to_add = 0;
    if (xoffset + to_add > PACKET_DATA_SIZE) { to_add = PACKET_DATA_SIZE - xoffset; if (to_add < 0) to_add = 0; }
    if (xoffset + to_add > max_data) to_add = max_data - xoffset;
    if (to_add < 0) to_add = 0;

    for (int i=0; i<to_add; i++) pkt->data[xoffset++]=rand();// fill pad
    // padding end
    pkt->data_len=xoffset;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] INIT_RESPONSE sending (link=%d, mtu=%d, dst=%s)",
                link->etcp->log_name, link->local_link_id, link->mtu_local,
                sockaddr_storage_to_str(&link->remote_addr).str);
    if (etcp_encrypt_send(pkt) < 0)
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] INIT_RESPONSE send failed link=%u", conn->log_name, link->local_link_id);

    memory_pool_free(conn->instance->pkt_pool, pkt);
    if (!conn->initialized) etcp_conn_ready(conn);
    if (etcp_link_is_alive(conn, link)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] INIT accepted link=%u status=%u recv=%u remote=%u", conn->log_name,
            link->local_link_id, link->link_status, link->recv_keepalive, link->remote_keepalive);
        etcp_keepalive_link_ready(link);
        if (etcp_link_publish_rx(link, old_state, old_status) && conn->instance->nat_det)
            nat_detection_link_ready(conn->instance->nat_det, link);
    }
    etcp_link_release(link); conn->callbacks_running--;
}

// Клиент: обработка INIT_RESPONSE — MTU, link_id, NAT-адрес, ed25519, keepalive, подъём линка.
static int handle_init_response_client(struct ETCP_SOCKET* e_sock, struct ETCP_DGRAM* pkt, struct ETCP_LINK* link) {
    if (pkt->data_len < ETCP_INIT_RESP_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "INIT_RESPONSE too short: code=%02x body=%u required=%zu",
                    pkt->data[0], pkt->data_len, (size_t)ETCP_INIT_RESP_SIZE);
        return 46;
    }
    // Код ответа не подтверждает смену эпохи; её проверяет etcp_session.
    struct ETCP_INIT_RESPONSE_PKT* resp = (struct ETCP_INIT_RESPONSE_PKT*)pkt->data;
    if (sc_identity_check("ETCP INIT_RESPONSE", link->etcp->crypto_ctx.peer_public_key, resp->ed25519_pubkey)) return 46;
    uint16_t remote_mtu = wire_read16(resp->mtu);
    if (!resp->link_id || remote_mtu < (etcp_link_network_overhead(link) + SC_ENCRYPT_OVERHEAD + ETCP_PAYLOAD_HEADER_SIZE + ACK_REZERV + 3)) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "INIT_RESPONSE invalid path: link_id=%u MTU=%u minimum=%u",
            resp->link_id, remote_mtu, (unsigned)(etcp_link_network_overhead(link) + SC_ENCRYPT_OVERHEAD + ETCP_PAYLOAD_HEADER_SIZE + ACK_REZERV + 3));
        return 46;
    }
    struct ETCP_CONN* conn = link->etcp;
    int old_state = link->link_state, old_status = etcp_link_record_rx(link, pkt);
    uint64_t resp_reset_id = wire_read64(resp->reset_id);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "INIT_RESPONSE rid=%016llx", (unsigned long long)resp_reset_id);
    etcp_conn_apply_peer_reset_id(link->etcp, resp_reset_id);
    link->mtu_remote = remote_mtu;
    if (link->mtu_remote > PACKET_DATA_MAX_MTU) link->mtu_remote = PACKET_DATA_MAX_MTU;
    link->mtu = link->mtu_local < link->mtu_remote ? link->mtu_local : link->mtu_remote;
    etcp_update_mtu(link->etcp);
    link->remote_link_id = resp->link_id;
    link->remote_socket_id = resp->remote_socket_id;
    link->remote_only_local = resp->only_local;
    link->remote_type = resp->type;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] INIT_RESPONSE received (link=%d, mtu=%d, rid=%016llx)", link->etcp->log_name, link->remote_link_id, link->mtu, (unsigned long long)resp_reset_id);

    if (e_sock && !link->is_tcp && e_sock->local_addr.ss_family == AF_INET && wire_read16(resp->peer_port)) {
        struct sockaddr_storage observed; memset(&observed, 0, sizeof(observed));
        struct sockaddr_in *source = (struct sockaddr_in *)&observed;
        source->sin_family = AF_INET; memcpy(&source->sin_addr, resp->peer_ipv4, 4);
        memcpy(&source->sin_port, resp->peer_port, 2);
        if (link->local_as_seen_by_peer.ss_family && !route_ping_addr_equal(&link->local_as_seen_by_peer, &observed, 1)) {
            DEBUG_INFO(DEBUG_CATEGORY_NAT, "Own endpoint observation changed peer=%s local=%s before=%s after=%s",
                       link->etcp->log_name, e_sock->name, sockaddr_storage_to_str(&link->local_as_seen_by_peer).str,
                       sockaddr_storage_to_str(&observed).str);
            link->nat_changes_count++;
        } else link->nat_hits_count++;
        link->local_as_seen_by_peer = observed;
        if (e_sock->instance->nat_det) nat_detection_observed_by_peer(e_sock->instance->nat_det, link, &observed);
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "Own endpoint observed peer=%s socket=%s generation=%016llx address=%s",
                    link->etcp->log_name, e_sock->name, (unsigned long long)e_sock->generation,
                    sockaddr_storage_to_str(&observed).str);
    }

    memcpy(link->etcp->peer_ed25519_pubkey, resp->ed25519_pubkey, SC_PUBKEY_SIZE);
    DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "[%s] Received Ed25519 pubkey from peer", link->etcp->log_name);

    link->peer_device_type = resp->device_type;
    {   uint16_t server_ka = (resp->keepalive[0]<<8) | resp->keepalive[1];
        link->keepalive_interval = negotiate_keepalive(link->etcp->instance->client_type,
            link->etcp->instance->keepalive_interval, resp->device_type, server_ka); }

    link->etcp->peer_node_id = sc_derive_node_id_from_pubkey(link->etcp->crypto_ctx.peer_public_key);
    etcp_update_log_name(link->etcp);

    link->initialized = 1;// получен init response (client)
    link->link_state = LINK_STATE_CONNECTED;
    if (link->init_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->init_timer);
        link->init_timer = NULL;
    }

    memory_pool_free(conn->instance->pkt_pool, pkt);
    if (!conn->initialized) etcp_conn_ready(conn);
    if (etcp_link_is_alive(conn, link)) {
        DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] INIT_RESPONSE accepted link=%u status=%u recv=%u remote=%u mtu=%u",
            conn->log_name, link->local_link_id, link->link_status, link->recv_keepalive, link->remote_keepalive, link->mtu);
        etcp_keepalive_link_ready(link);
        etcp_link_publish_rx(link, old_state, old_status);
    }
    return 0;
}

// Главный приёмник UDP-кодограмм: normal/init decrypt, диспетчеризация PING/PONG/INIT,
// создание/переиспользование линков и коннектов, обработка коллизий.
// Приём прямого UDP-пакета: recvfrom → etcp_process_packet.
void etcp_connections_read_callback_socket(socket_t sock, void* arg) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_RX_INGRESS);
    struct ETCP_SOCKET* e_sock = (struct ETCP_SOCKET*)arg;
    if (!e_sock) return;
    struct sockaddr_storage addr;
    uint8_t data[PACKET_DATA_SIZE];
    socklen_t addr_len = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    ssize_t recv_len = socket_recvfrom(sock, data, PACKET_DATA_SIZE, (struct sockaddr*)&addr, &addr_len);
    if (recv_len <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "recvfrom failed, error=%zd, sock_err=%d", recv_len, socket_get_error());
        return;
    }
    etcp_process_packet(e_sock, data, recv_len, addr);
}

/* Неизвестный UDP endpoint проходит прежний INIT/bootstrap; известный — общий packet_link. */
static void etcp_process_packet(struct ETCP_SOCKET* sock, uint8_t* data, ssize_t size, struct sockaddr_storage addr) {
    struct ETCP_LINK* link = etcp_link_find_by_addr(sock, &addr, 0);
    if (!link || !link->io) { etcp_process_packet_direct(sock, data, size, addr); return; }
    struct ll_entry* p = ll_alloc_lldgram(size);
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "UDP packet RX allocation failed size=%zd", size); return; }
    memcpy(p->dgram, data, size); p->len = size;
    etcp_link_io_receive(link, p);
}

static void udp_receive(struct ETCP_LINK* link, struct ll_entry* entry) {
    etcp_process_packet_direct(link->conn, entry->dgram, entry->len, link->remote_addr);
    queue_dgram_free(entry); queue_entry_free(entry);
}

static void stcp_receive(struct ETCP_LINK* link, struct ll_entry* entry) {
    if (entry->len < 3 || entry->len > PACKET_DATA_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "STCP invalid packet size=%u", entry->len);
    } else {
        struct ETCP_DGRAM* pkt = memory_pool_alloc(link->etcp->instance->pkt_pool);
        if (!pkt) DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "STCP packet RX pool exhausted");
        else {
            memset(pkt, 0, sizeof(*pkt));
            if (entry->size >= sizeof(pkt->received_us)) memcpy(&pkt->received_us, entry->data, sizeof(pkt->received_us));
            memcpy(&pkt->timestamp, entry->dgram, 2); pkt->flag_up = entry->dgram[2] & 1;
            memcpy(pkt->data, entry->dgram + 3, entry->len - 3); pkt->link = link;
            link->total_decrypted += entry->len;
            if (etcp_packet_decrypted(link->conn, pkt, link, entry->len))
                memory_pool_free(link->etcp->instance->pkt_pool, pkt);
        }
    }
    queue_dgram_free(entry); queue_entry_free(entry);
}

/* Канал уже привязан к ключу пира: адресный bootstrap/NAT здесь не нужны. */
static void bound_receive(struct ETCP_LINK* link, struct ll_entry* entry) {
    struct ETCP_CONN* conn = link->etcp;
    conn->callbacks_running++; link->active_users++;
    struct ETCP_DGRAM* pkt = NULL;
    if (entry->len > link->mtu_local) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] driver RX oversized: %u > %u", conn->log_name, entry->len, link->mtu_local);
        goto done;
    }
    pkt = memory_pool_alloc(conn->instance->pkt_pool);
    if (!pkt) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "packet channel RX allocation failed"); goto done; }
    memset(pkt, 0, sizeof(*pkt));
    size_t length = 0;
    int result = sc_decrypt(&conn->crypto_ctx, entry->dgram, entry->len, (uint8_t*)&pkt->timestamp, &length);
    if (result && entry->len > SC_PUBKEY_ENC_SIZE + UDP_SC_HDR_SIZE) {
        const uint8_t* salt = entry->dgram + entry->len - SC_PUBKEY_ENC_SIZE;
        uint8_t key[SC_PUBKEY_SIZE];
        sc_obfuscate_pubkey(salt, conn->instance->my_keys.public_key, salt + SC_PUBKEY_ENC_SALT_SIZE, key);
        if (!memcmp(key, conn->crypto_ctx.peer_public_key, sizeof(key)))
            result = sc_decrypt(&conn->crypto_ctx, entry->dgram, entry->len - SC_PUBKEY_ENC_SIZE,
                (uint8_t*)&pkt->timestamp, &length);
    }
    if (result || length < 4) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] packet channel authentication/format failed: result=%d bytes=%zu",
            conn->log_name, result, length); goto done;
    }
    if (!admit_peer(link, !link->initialized || pkt->data[0] == ETCP_INIT_REQUEST
        || pkt->data[0] == ETCP_INIT_REQUEST_NOINIT)) goto done;
    pkt->data_len = length - 3; pkt->noencrypt_len = 0; pkt->link = link;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] packet channel RX path=%u wire=%u plain=%zu code=%02x up=%u",
                conn->log_name, link->local_link_id, entry->len, length, pkt->data[0], pkt->flag_up);
    if (pkt->data[0] == ETCP_INIT_REQUEST || pkt->data[0] == ETCP_INIT_REQUEST_NOINIT) {
        const struct ETCP_INIT_REQUEST_PKT* req = (const struct ETCP_INIT_REQUEST_PKT*)pkt->data;
        if (pkt->data_len < ETCP_INIT_REQ_SIZE || !req->link_id ||
            wire_read16(req->mtu) < SC_ENCRYPT_OVERHEAD + ETCP_PAYLOAD_HEADER_SIZE + ACK_REZERV + 3) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] packet channel invalid INIT", conn->log_name); goto done;
        }
        if (sc_identity_check("ETCP channel INIT", conn->crypto_ctx.peer_public_key, req->ed25519_pubkey)) goto done;
        link->remote_link_id = req->link_id;
        link->mtu_remote = wire_read16(req->mtu);
        link->mtu = link->mtu_local < link->mtu_remote ? link->mtu_local : link->mtu_remote;
        link->peer_device_type = req->device_type;
        link->keepalive_interval = negotiate_keepalive(conn->instance->client_type, conn->instance->keepalive_interval,
            req->device_type, wire_read16(req->keepalive));
        if (link->keepalive_interval < 10) link->keepalive_interval = 10;
        link->recovery_interval = wire_read16(req->recovery) * 100;
        memcpy(conn->peer_ed25519_pubkey, req->ed25519_pubkey, SC_PUBKEY_SIZE);
        etcp_update_mtu(conn);
        etcp_conn_apply_peer_reset_id(conn, wire_read64(req->reset_id));
        if (etcp_link_is_alive(conn, link)) {
            send_init_response(NULL, pkt, link, conn, req, &link->remote_addr, 0, 0, 0, length);
            pkt = NULL;
        }
    } else if (!etcp_packet_decrypted(NULL, pkt, link, length)) pkt = NULL;
done:
    if (pkt) memory_pool_free(conn->instance->pkt_pool, pkt);
    queue_dgram_free(entry); queue_entry_free(entry);
    etcp_link_release(link); conn->callbacks_running--;
}

// Обработка принятого сырого UDP-пакета: расшифровка и диспетчеризация.
// Выделена из read_callback, чтобы один путь обслуживал прямой приём и приём через
// SOCKS5 UDP ASSOCIATE (socks_etcp_read_callback подставляет реальный src пира).
// !!!!!! DANGER: в этой функции ПРЕДЕЛЬНАЯ АККУРАТНОСТЬ !!!!!
// НЕ РУИНИТЬ (uint8_t*)&pkt->timestamp - это правильно !!!!
//
// Ошибки функции (errorcode):
// 1 - пакет слишком маленький для init (< SC_PUBKEY_SIZE)
// 2 - не удалось установить peer public key при init
// 3 - не удалось расшифровать init пакет
// 4 - не init/ping пакет (неверный код)
// 5 - коллизия peer ID и ключей
// 6 - не удалось расшифровать обычный пакет
// 7 - слишком короткий пакет
// 8 - ключ не в списке allowed_keys
// 13 - переполнение при парсинге пакета
// 46 - расшифрованный пакет слишком маленький (< 3 байта)
// 55 - не удалось создать подключение
// 66 - не удалось создать линк
static void etcp_process_packet_direct(struct ETCP_SOCKET* e_sock, uint8_t* data,
    ssize_t recv_len, struct sockaddr_storage addr) {
    etcp_process_packet_result(e_sock, data, recv_len, addr, NULL);
}

static void etcp_process_packet_result(struct ETCP_SOCKET* e_sock, uint8_t* data, ssize_t recv_len,
    struct sockaddr_storage addr, const struct packet_transport_event* decoded) {
    ETCP_PROFILE_SCOPE(ETCP_PROF_RX_INGRESS);
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");

    // DUMP: Show received packet content
    if (data && debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CRYPTO)) log_dump(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CRYPTO, "RECV in:", data, recv_len);

    struct ETCP_DGRAM* pkt = memory_pool_alloc(e_sock->instance->pkt_pool);
    if (!pkt) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "pkt_pool exhausted, dropping packet from %s", sockaddr_storage_to_str(&addr).str); return; }
    memset(pkt, 0, sizeof(*pkt));
    size_t pkt_len=0;
    int errorcode=0;
    struct secure_channel sc;
    uint8_t decrypted_pubkey[SC_PUBKEY_SIZE];

    struct ETCP_LINK* link=etcp_link_find_by_addr(e_sock, &addr, 0);
    if (decoded) {
        if (decoded->error || decoded->size < 3 || decoded->size > PACKET_DATA_SIZE + 3) {
            errorcode = decoded->error ? decoded->error : 7; goto ec_fr;
        }
        pkt_len = decoded->size; pkt->received_us = decoded->received_us;
        memcpy(&pkt->timestamp, decoded->data, pkt_len);
        if (!decoded->init) {
            if (!link || memcmp(link->etcp->crypto_ctx.session_key, decoded->crypto.session_key, SC_SESSION_KEY_SIZE)) {
                DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "stale UDP result discarded after link/key change");
                memory_pool_free(e_sock->instance->pkt_pool, pkt); return;
            }
            link->etcp->crypto_ctx.rx_counter++;
            link->diagnostic_wire_rx_bytes += decoded->wire_size;
            link->etcp->diagnostic_wire_rx_bytes += decoded->wire_size;
            errorcode = etcp_packet_decrypted(e_sock, pkt, link, pkt_len);
            if (errorcode) goto ec_fr;
            return;
        }
        sc = decoded->crypto; sc.pk = &e_sock->instance->my_keys;
        memcpy(decrypted_pubkey, sc.peer_public_key, sizeof(decrypted_pubkey));
        goto init_ready;
    }
    // Try normal decryption first if we have an established link with session keys
    // This is the common case for data packets and responses
//    if (link) {
//        link->recv_keepalive = 1;  // Link is up after successful initialization - не ставим ап от неизвестных пакетов
//        }

    if (link!=NULL && link->etcp!=NULL && link->etcp->crypto_ctx.session_ready) {
        DEBUG_TRACE(DEBUG_CATEGORY_CRYPTO, "NORM_DECRYPT_TRY: recv_len=%zd rx=%llu",
                   recv_len, (unsigned long long)link->etcp->crypto_ctx.rx_counter);
        ETCP_PROFILE_STAGE(ETCP_PROF_DECRYPT);
        sc_status_t dec_rc = sc_decrypt(&link->etcp->crypto_ctx, data, recv_len, (uint8_t*)&pkt->timestamp, &pkt_len);
        ETCP_PROFILE_STAGE(ETCP_PROF_RX_STACK);
        if (!dec_rc) {
            int ec = etcp_packet_decrypted(e_sock, pkt, link, pkt_len);
            if (ec) { errorcode = ec; goto ec_fr; }
            return;
        }
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "etcp: DECRYPT FAIL on existing link. rc=%d link=%p log=%s from=%s sess=%d link_state=%d keepalive=%d enc_errs=%zu my_pub=%016llx peer_pub=%016llx",
                       dec_rc, link, link->etcp->log_name, sockaddr_storage_to_str(&addr).str, link->etcp->crypto_ctx.session_ready,
                       link->link_state, link->recv_keepalive, link->encrypt_errors,
                       (unsigned long long)wire_read64(link->etcp->crypto_ctx.pk->public_key), (unsigned long long)wire_read64(link->etcp->crypto_ctx.peer_public_key));
    } else {
        DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "SKIP normal decrypt: link=%p session_ready=%d — trying init decrypt",
                    link, link && link->etcp ? link->etcp->crypto_ctx.session_ready : -1);
    }

    // Try INIT decryption (for incoming connection requests)
    // This handles: no link found, or link without session, or normal decrypt failed
    if (recv_len <= SC_PUBKEY_ENC_SIZE + UDP_SC_HDR_SIZE) {
        if (e_sock->pkt_format_errors < 2)
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "packet too small for init, size=%zd from %s", recv_len, sockaddr_storage_to_str(&addr).str);
        errorcode=1;
        goto ec_fr;
    }

    ETCP_PROFILE_STAGE(ETCP_PROF_INIT);
    ETCP_PROFILE_MARK(ETCP_PROF_INIT_BEGIN, e_sock);
    ETCP_PROFILE_STAGE(ETCP_PROF_INIT_CONTEXT);
    sc_init_ctx(&sc, &e_sock->instance->my_keys);

    const uint8_t* salt = data + recv_len - SC_PUBKEY_ENC_SIZE;
    const uint8_t* encrypted_pubkey = salt + SC_PUBKEY_ENC_SALT_SIZE;

    ETCP_PROFILE_STAGE(ETCP_PROF_INIT_OBFUSCATE);
    sc_obfuscate_pubkey(salt, e_sock->instance->my_keys.public_key, encrypted_pubkey, decrypted_pubkey);
    ETCP_PROFILE_STAGE(ETCP_PROF_INIT);

    if (sc_set_peer_public_key(&sc, decrypted_pubkey, SC_PEER_PUBKEY_BIN)!=SC_OK) { 
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "failed to set peer public key during init, from %s", sockaddr_storage_to_str(&addr).str);
        errorcode=2; 
        goto ec_fr; 
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "X25519 decrypt OK from %s", sockaddr_storage_to_str(&addr).str);
    DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "INIT_DECRYPT_TRY: recv_len=%zd strip40=%zd salt=%02x%02x%02x%02x%02x%02x%02x%02x enc_pub=%02x%02x%02x%02x",
               recv_len, recv_len - SC_PUBKEY_ENC_SIZE,
               salt[0], salt[1], salt[2], salt[3], salt[4], salt[5], salt[6], salt[7],
               encrypted_pubkey[0], encrypted_pubkey[1], encrypted_pubkey[2], encrypted_pubkey[3]);
    if (sc_decrypt(&sc, data, recv_len - SC_PUBKEY_ENC_SIZE, (uint8_t*)&pkt->timestamp, &pkt_len)) {
#ifdef UTUN_HAVE_STANDBY
        if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_KEEPALIVE)) {
            char who[64]; snprintf(who, sizeof(who), "addr=%s", sockaddr_storage_to_str(&addr).str);
            standby_log_rx("udp undecryptable", who);
        }
#endif
        if (link) {
            DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO,
                        "packet undecryptable (normal+init fail) from=%s — existing link log=%s state=%d sess=%d",
                        sockaddr_storage_to_str(&addr).str,
                        link->etcp ? link->etcp->log_name : "null",
                        link->link_state,
                        link->etcp ? link->etcp->crypto_ctx.session_ready : -1);
        } else if (e_sock->pkt_format_errors < 2) {
            DEBUG_WARN(DEBUG_CATEGORY_CRYPTO,
                        "packet undecryptable (normal+init fail) from=%s — no link for this address",
                        sockaddr_storage_to_str(&addr).str);
        }
        errorcode=3; 
        goto ec_fr; 
    }

    ETCP_PROFILE_STAGE(ETCP_PROF_RX_STACK);
init_ready:
    // INIT decryption succeeded - process packet
    if (pkt_len <= ETCP_ENCRYPTED_HDR_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "missing encrypted packet code size=%zu from=%s", pkt_len, sockaddr_storage_to_str(&addr).str);
        errorcode=7; goto ec_fr;
    }
    pkt->data_len = pkt_len - ETCP_ENCRYPTED_HDR_SIZE;
    pkt->noencrypt_len = 0;
    uint8_t code = pkt->data[0];
    if (code == ETCP_PING || code == ETCP_PONG) {
        const struct ETCP_PING_HDR* probe = (const struct ETCP_PING_HDR*)pkt->data;
        if (pkt->data_len < sizeof(*probe)) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "short UDP probe code=%u bytes=%u from=%s", code, pkt->data_len, sockaddr_storage_to_str(&addr).str);
            errorcode = 7; goto ec_fr;
        }
        if (sc_identity_check("ETCP UDP probe", decrypted_pubkey, probe->ed25519)) { errorcode = 7; goto ec_fr; }
    }
    if (code == ETCP_PING) {
#ifdef UTUN_HAVE_STANDBY
        if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_KEEPALIVE)) {
            char who[64]; snprintf(who, sizeof(who), "addr=%s", sockaddr_storage_to_str(&addr).str);
            standby_log_rx("udp ping", who);
        }
#endif
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "X25519 decrypted: PING from peer=%s src=%s",
                   sc_public_key_hex(((const struct ETCP_PING_HDR*)pkt->data)->ed25519, (char[65]){0}), sockaddr_storage_to_str(&addr).str);
        int ret = handle_ping(e_sock, pkt, &addr, decrypted_pubkey);
        if (ret) { errorcode = ret; goto ec_fr; }
        return;
    }
    if (code == ETCP_PONG) {
#ifdef UTUN_HAVE_STANDBY
        if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_KEEPALIVE)) {
            char who[64]; snprintf(who, sizeof(who), "addr=%s", sockaddr_storage_to_str(&addr).str);
            standby_log_rx("udp pong", who);
        }
#endif
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "X25519 decrypted: PONG from peer=%s src=%s",
                   sc_public_key_hex(((const struct ETCP_PING_HDR*)pkt->data)->ed25519, (char[65]){0}), sockaddr_storage_to_str(&addr).str);
        int ret = handle_pong(e_sock, pkt, &addr, decrypted_pubkey);
        if (ret) { errorcode = ret; goto ec_fr; }
        return;
    }
    if (code!=ETCP_INIT_REQUEST && code!=ETCP_INIT_REQUEST_NOINIT) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "not an init packet: code=0x%02x (expected 0x02/0x04) from %s — packet dropped",
                    code, sockaddr_storage_to_str(&addr).str);
        errorcode=4;
        goto ec_fr;
    }// не init
    if (e_sock->type == CFG_SERVER_TYPE_PRIVATE) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "INIT rejected: socket %s (type=PRIVATE) does not accept incoming connections", e_sock->name);
        errorcode=8;
        goto ec_fr;
    }
    if (pkt->data_len < ETCP_INIT_REQ_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "INIT REQUEST too short: code=%02x body=%u required=%zu from=%s",
            code, pkt->data_len, (size_t)ETCP_INIT_REQ_SIZE, sockaddr_storage_to_str(&addr).str);
        errorcode=7;
        goto ec_fr;
    }

    if (link)
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "INIT packet on existing link from=%s log=%s link_state=%d",
                   sockaddr_storage_to_str(&addr).str,
                   link->etcp ? link->etcp->log_name : "null", link->link_state);

    // Check allowed keys for incoming connections
    struct global_config *global = &e_sock->instance->config->global;
    if (!global->allowed_keys_allow_all) {
        if (global->allowed_keys_count == 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Connection rejected: no allowed_keys configured, allow_all=0");
            errorcode=8;
            goto ec_fr;
        }
        int found = 0;
        struct CFG_ALLOWED_KEY *ak = global->allowed_keys;
        while (ak) {
            if (memcmp(ak->key_bin, sc.peer_public_key, SC_PUBKEY_SIZE) == 0) {
                found = 1;
                break;
            }
            ak = ak->next;
        }
        if (!found) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Connection rejected: peer public key not in allowed_keys list");
            errorcode=8;
            goto ec_fr;
        }
    }

    struct ETCP_INIT_REQUEST_PKT* req = (struct ETCP_INIT_REQUEST_PKT*)pkt->data;
    if (sc_identity_check("ETCP INIT", sc.peer_public_key, req->ed25519_pubkey)) { errorcode = 7; goto ec_fr; }
    uint64_t peer_id = sc_derive_node_id_from_pubkey(sc.peer_public_key);
    int local_master = memcmp(e_sock->instance->my_ed25519_pubkey, req->ed25519_pubkey, SC_PUBKEY_SIZE) < 0;
    uint64_t reset_id = wire_read64(req->reset_id);
    uint16_t mtu = wire_read16(req->mtu);
    if (!req->link_id || mtu < ETCP_MIN_UDP_MTU(addr.ss_family)) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "INIT_REQUEST invalid path: link_id=%u MTU=%u minimum=%u from=%s",
            req->link_id, mtu, (unsigned)ETCP_MIN_UDP_MTU(addr.ss_family), sockaddr_storage_to_str(&addr).str);
        errorcode = 7; goto ec_fr;
    }
    uint16_t src_port = wire_read16(req->src_port);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "INIT received: peer=%s mtu=%u link=%u sock=%u rid=%016llx src=%s",
               sc_public_key_hex(req->ed25519_pubkey, (char[65]){0}), mtu, req->link_id, req->socket_id,
               (unsigned long long)reset_id, sockaddr_storage_to_str(&addr).str);

    struct ETCP_CONN* conn = instance_find_conn_by_key(e_sock->instance, sc.peer_public_key);
    if (conn && !nat_detection_udp_init_allowed(e_sock->instance->nat_det, conn, e_sock, &addr)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "[%s] NAT defers INIT reply on socket=%s from=%s",
                    conn->log_name, e_sock->name, sockaddr_storage_to_str(&addr).str);
        memory_pool_free(e_sock->instance->pkt_pool, pkt);
        return;
    }
    int indexed = conn != NULL;
    if (!conn) {
        struct ETCP_LINK* ol = etcp_link_find_by_addr(e_sock, &addr, 0);
        if (ol && ol->etcp && ol->etcp->conn_queue_entry) {
            struct conn_queue_entry* ce = (struct conn_queue_entry*)ol->etcp->conn_queue_entry->data;
            if (!ce->peer_node_id && !memcmp(ol->etcp->crypto_ctx.peer_public_key, sc.peer_public_key, SC_PUBKEY_SIZE)) {
                conn = ol->etcp;
                DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] reusing unindexed outbound conn (q_key=0, peer=0x%016llx) for incoming INIT from %s",
                           conn->log_name, (unsigned long long)peer_id, sockaddr_storage_to_str(&addr).str);
            }
        }
    }

    int new_conn = conn == NULL;
    if (new_conn) {
        conn = etcp_connection_create(e_sock->instance, "");
        if (!conn) { errorcode = 55; DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "failed to create connection"); goto ec_fr; }
        memcpy(&conn->crypto_ctx, &sc, sizeof(sc));
    }
    if (!indexed) {
        memcpy(conn->peer_ed25519_pubkey, req->ed25519_pubkey, SC_PUBKEY_SIZE);
        conn->peer_node_id = peer_id;
        etcp_update_log_name(conn);
        if (etcp_conn_index_peer(conn) < 0) {
            if (new_conn) etcp_connection_close(conn);
            errorcode = 55; goto ec_fr;
        }
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "INIT registered socket=%s peer=%s address=%s", e_sock->name,
            sc_public_key_hex(req->ed25519_pubkey, (char[65]){0}), sockaddr_storage_to_str(&addr).str);
    }

    if (conn->fin_wait) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] received INIT during fin_wait, clearing", conn->log_name);
        conn->fin_wait = 0;
        if (conn->fin_wait_clear_cb) { conn->fin_wait_clear_cb(conn, conn->fin_wait_clear_arg); conn->fin_wait_clear_cb = NULL; conn->fin_wait_clear_arg = NULL; }
    }

    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "INIT conn=%s new_conn=%d peer=0x%016llx state=%d links_up=%d links=%p", conn->log_name, new_conn, (unsigned long long)peer_id, conn->state, conn->links_up, (void*)conn->links);

    // Check if link already exists (for CHANNEL_INIT recovery)

    struct ETCP_LINK* existing_link = etcp_link_find_by_remote_id(conn, req->link_id);

    /* Правила 1+2: найден по id, линк down, адрес сменился */
    if (existing_link && existing_link->link_status == 0
        && !sockaddr_equal(&existing_link->remote_addr, &addr)) {
        struct ETCP_LINK* L_addr = etcp_link_find_by_addr(e_sock, &addr, 0);
        if (L_addr && L_addr != existing_link && L_addr->etcp == conn && L_addr->link_status == 0) {
            /* Правило 2: своп remote_link_id, дальше работаем с L_addr */
            uint8_t id_old = existing_link->remote_link_id;
            uint8_t id_swap = L_addr->remote_link_id;
            existing_link->remote_link_id = id_swap;
            L_addr->remote_link_id = id_old;
            DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] swap remote_link_id: id=%u @ %s <-> id=%u @ %s",
                       conn->log_name, id_old, sockaddr_storage_to_str(&existing_link->remote_addr).str,
                       id_swap, sockaddr_storage_to_str(&L_addr->remote_addr).str);
            existing_link = L_addr;
        } else if (!L_addr) {
            /* Правило 1: перепривязать адрес */
            etcp_link_update_remote_addr(existing_link, e_sock, &addr);
            DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] remote addr updated: id=%u -> %s",
                       conn->log_name, existing_link->remote_link_id, sockaddr_storage_to_str(&addr).str);
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] addr %s busy (up/foreign) — link id=%u left as-is",
                       conn->log_name, sockaddr_storage_to_str(&addr).str, existing_link->remote_link_id);
        }
    }

    if (!existing_link) {
    existing_link = etcp_link_find_by_addr(e_sock, &addr, 0);
        if (existing_link && existing_link->etcp == conn) {
            if (memcmp(conn->crypto_ctx.peer_public_key, sc.peer_public_key, SC_PUBKEY_SIZE)) {
                DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] link address match but pubkey mismatch, firing node_changed", conn->log_name);
                DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "pubkey mismatch on reused link for node %016llx", (unsigned long long)peer_id);
                conn->callbacks_running++;
                etcp_cbk_fire(conn, ETCP_CBK_EVENT_NODE_CHANGED);
                conn->callbacks_running--;
                errorcode = 67;
                goto ec_fr;
            }
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] found existing outbound link by addr for incoming INIT, reusing link=%p",
                       conn->log_name, existing_link);
        } else if (existing_link) {
            struct ETCP_CONN* old_conn = existing_link->etcp;
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] conflicting link at %s belongs to %s, firing node_changed",
                       conn->log_name, sockaddr_storage_to_str(&addr).str, old_conn->log_name);
            DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "link address conflict for node %016llx, firing node_changed callbacks on %s",
                        (unsigned long long)peer_id, old_conn->log_name);
            old_conn->callbacks_running++;
            etcp_cbk_fire(old_conn, ETCP_CBK_EVENT_NODE_CHANGED);
            old_conn->callbacks_running--;
            errorcode = 67;
            goto ec_fr;
        } else {
            existing_link = NULL;
        }
    }

    uint8_t send_reset = 0;

    if (existing_link && existing_link->etcp == conn) {// существующий линк
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] found existing link for id=%d, socket=[%s]", conn->log_name, req->link_id, e_sock->name);
        link = existing_link;
        if (!sockaddr_equal(&link->remote_addr, &addr)) {
            DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[%s] IP:port changed for remote_link_id=%d socket:[%s] — closing old link, creating new",
                       conn->log_name, req->link_id, e_sock->name);
            etcp_link_close(link);
            goto create_new_link;
        }
        // Link exists - reuse it for recovery
        link->remote_link_id = req->link_id;
        link->remote_socket_id = req->socket_id;
        link->remote_only_local = req->only_local;
        link->remote_type = req->type;

        // ── Collision handling ──
        if (req->collision) {
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] INIT collision=1 from peer=%016llx — remote is master, becoming slave",
                       conn->log_name, (unsigned long long)peer_id);
            // Коллизия INIT меняет роль линка, но не эпоху надёжного потока.
            send_reset = 1;
        } else {
            // Check if WE have an outbound (master) link on this conn
            struct ETCP_LINK* ml = etcp_select_collision_link(conn, e_sock, &addr);
            if (ml) {
                if (local_master) {
                    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] COLLISION: we=master ed=%s peer=%s — sending collision INIT",
                               conn->log_name, sc_public_key_hex(conn->instance->my_ed25519_pubkey, (char[65]){0}),
                           sc_public_key_hex(req->ed25519_pubkey, (char[65]){0}));
                    memory_pool_free(e_sock->instance->pkt_pool, pkt);
                    etcp_link_send_init(ml, 0, 1);
                    return;
                }
                DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] COLLISION: peer smaller, yielding master, processing as slave",
                           conn->log_name);
                if (ml->init_timer) { uasync_cancel_timeout(conn->instance->ua, ml->init_timer); ml->init_timer = NULL; }
            }

            if (req->code == ETCP_INIT_REQUEST) {
                if (conn->got_initial_pkt) {
                    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] INIT_REQUEST on existing link: code=0x%02x got_init=%d → reinit",
                               conn->log_name, req->code, conn->got_initial_pkt);
                    // Повтор INIT не сбрасывает поток. Новая эпоха требует session challenge.
                }
                send_reset = 0;
            } else {
                if (conn->got_initial_pkt == 0) send_reset = 1;
            }
        }

        // Cancel existing timers
        if (link->init_timer) {
            uasync_cancel_timeout(link->etcp->instance->ua, link->init_timer);
            link->init_timer = NULL;
        }
    } else {
create_new_link:
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] NO existing link for id=%d, socket=[%s]", conn->log_name, req->link_id, e_sock->name);
        // Create new link
        link = etcp_link_new(conn, e_sock, &addr, 1);
        if (!link) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] UDP link creation failed/cancelled peer=%016llx from=%s",
                conn->log_name, (unsigned long long)peer_id, sockaddr_storage_to_str(&addr).str);
            if (new_conn) {
                int owners = node_conn_direct_handle_count(conn);
                if (!conn->links && !owners) etcp_connection_close(conn);
                else DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] cancelled UDP accept preserves connection: other_links=%u owners=%d closing=%u",
                    conn->log_name, conn->links != NULL, owners, conn->close_requested);
            }
            errorcode = 66; goto ec_fr;
        }
        link->remote_link_id = req->link_id;
        link->remote_socket_id = req->socket_id;
        link->remote_only_local = req->only_local;
        link->remote_type = req->type;

        // ── Collision handling ──
        if (req->collision) {
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] INIT collision=1 from peer=%016llx — remote is master, becoming slave",
                       conn->log_name, (unsigned long long)peer_id);
            // Коллизия INIT меняет роль линка, но не эпоху надёжного потока.
            send_reset = 1;
        } else {
            // Check if WE have an outbound (master) link
            struct ETCP_LINK* ml = etcp_select_collision_link(conn, e_sock, &addr);
            if (ml && local_master) {
                DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] COLLISION: we=master ed=%s peer=%s — sending collision INIT",
                           conn->log_name, sc_public_key_hex(conn->instance->my_ed25519_pubkey, (char[65]){0}),
                           sc_public_key_hex(req->ed25519_pubkey, (char[65]){0}));
                memory_pool_free(e_sock->instance->pkt_pool, pkt);
                etcp_link_send_init(ml, 0, 1);
                return;
            }

            if (conn->got_initial_pkt == 0) send_reset = 1;
        }
        uint16_t client_ka = (req->keepalive[0]<<8) | req->keepalive[1];
        link->peer_device_type = req->device_type;
        link->keepalive_interval = negotiate_keepalive(e_sock->instance->client_type, e_sock->instance->keepalive_interval, req->device_type, client_ka);
        link->recovery_interval=((req->recovery[0]<<8) | req->recovery[1])*100;// timebase в link, timebase/100 в кодограмме
        if (link->keepalive_interval < 10) link->keepalive_interval = 10;
        DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE, "set keepalive for link=%d", link->keepalive_interval);
    }

    link->mtu_remote = (req->mtu[0] << 8) | req->mtu[1];
    if (link->mtu_remote > PACKET_DATA_MAX_MTU) link->mtu_remote = PACKET_DATA_MAX_MTU;
    link->mtu = link->mtu_local < link->mtu_remote ? link->mtu_local : link->mtu_remote;
    etcp_update_mtu(link->etcp);

    uint32_t req_src_ip; memcpy(&req_src_ip, req->src_ipv4, 4);
    uint16_t req_src_port = wire_read16(req->src_port);

    memcpy(conn->peer_ed25519_pubkey, req->ed25519_pubkey, SC_PUBKEY_SIZE);

    // Применяем эпоху ресета пира ПОСЛЕ всей reinit-логики
    etcp_conn_apply_peer_reset_id(conn, reset_id);

    send_init_response(e_sock, pkt, link, conn, req, &addr, send_reset, req_src_ip, req_src_port, pkt_len);
    return;


ec_fr:
    e_sock->pkt_format_errors++;
    if (e_sock->pkt_format_errors < 3 || (e_sock->pkt_format_errors % 500 == 0))
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "error %d, from %s (count=%zu)", errorcode, sockaddr_storage_to_str(&addr).str, e_sock->pkt_format_errors);
    e_sock->errorcode=errorcode;
    memory_pool_free(e_sock->instance->pkt_pool, pkt);
    return;
}

// Обработка пакета, расшифрованного на установленном линке: keepalive-статус, INIT_RESPONSE,
// обычные данные → etcp_conn_input.
int etcp_packet_decrypted(struct ETCP_SOCKET* e_sock, struct ETCP_DGRAM* pkt,
                           struct ETCP_LINK* link, size_t pkt_len) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "Decrypt ok - normal pkt");
    struct ETCP_CONN* conn = link->etcp;
    uint32_t reset = conn->reset_count;
    int result = 0;
    conn->callbacks_running++;
    link->active_users++;
    if (pkt_len<3) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "decrypted packet too small, size=%zu", pkt_len); result = 46; goto done; }
    pkt->data_len=pkt_len-3;
    pkt->noencrypt_len=0;
    pkt->link=link;
    if (!pkt->data_len) { DEBUG_WARN(DEBUG_CATEGORY_ETCP,"empty decrypted ETCP packet"); result = 46; goto done; }
    if (conn->close_requested) { memory_pool_free(conn->instance->pkt_pool, pkt); goto done; }
    if (!admit_peer(link, pkt->data[0] == ETCP_INIT_RESPONSE || pkt->data[0] == ETCP_INIT_RESPONSE_NOINIT
        || (pkt->data[0] >= ETCP_SESSION_HELLO && pkt->data[0] <= ETCP_SESSION_CONFIRM))) {
        result = 8; goto done;
    }
    if (etcp_session_receive(link->etcp,pkt->data,pkt->data_len)) {
        memory_pool_free(conn->instance->pkt_pool,pkt); goto done;
    }
    if (pkt->data[0] == ETCP_SESSION_DATA || pkt->data[0] <= 1) {
        if (!etcp_session_accept_data(link->etcp,pkt->data,pkt->data_len)) {
            memory_pool_free(conn->instance->pkt_pool,pkt); goto done;
        }
        pkt->data_len -= ETCP_SESSION_HEADER_SIZE;
        memmove(pkt->data,pkt->data+ETCP_SESSION_HEADER_SIZE,pkt->data_len);
        pkt_len -= ETCP_SESSION_HEADER_SIZE;
    }
    if (pkt->data_len && pkt->data[0] != ETCP_KEEPALIVE) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "[%s] decrypt: code=%02x dlen=%u plen=%zu recv=%d sock=%p",
                    link->etcp->log_name, pkt->data[0], pkt->data_len, pkt_len,
                    link->recv_keepalive, (void*)e_sock);
    }

    uint8_t pkt_code = pkt->data[0];
    if (pkt_code == ETCP_INIT_RESPONSE || pkt_code == ETCP_INIT_RESPONSE_NOINIT) {
        result = handle_init_response_client(e_sock, pkt, link);
        goto done;
    }
    int old_status = etcp_link_record_rx(link, pkt);
    if (!etcp_link_publish_rx(link, link->link_state, old_status) || conn->reset_count != reset) {
        memory_pool_free(conn->instance->pkt_pool, pkt); goto done;
    }
    if (pkt->data[0] != ETCP_KEEPALIVE) etcp_keepalive_watch_rx(link);

    if (pkt_code == ETCP_TCP_ADDRESS) {
        struct UTUN_INSTANCE *inst = link->etcp->instance;
        static const uint8_t zero[12];
        if (!link->is_tcp || pkt->data_len != 18 ||
            (pkt->data[1] != 4 && pkt->data[1] != 6) ||
            (pkt->data[1] == 4 && memcmp(pkt->data + 6, zero, sizeof(zero)))) {
            DEBUG_WARN(DEBUG_CATEGORY_NAT, "TCP IP report invalid transport/format peer=%s size=%u",
                       link->etcp->log_name, pkt->data_len);
            memory_pool_free(inst->pkt_pool, pkt); goto done;
        }
        if (!link->is_server && inst->config && inst->config->global.socks_enabled) {
            DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "TCP IP report ignored through SOCKS peer=%s", link->etcp->log_name);
            memory_pool_free(inst->pkt_pool, pkt); goto done;
        }
        struct sockaddr_storage observed = {0};
        if (pkt->data[1] == 4) {
            observed.ss_family = AF_INET; memcpy(&((struct sockaddr_in *)&observed)->sin_addr, pkt->data + 2, 4);
        } else {
            observed.ss_family = AF_INET6; memcpy(&((struct sockaddr_in6 *)&observed)->sin6_addr, pkt->data + 2, 16);
        }
        link->local_as_seen_by_peer = observed;
        const struct sockaddr_storage *local = stcp_link_get_local_addr(link->tcp_link);
        if (local && inst->nat_det && link->conn) nat_detection_tcp_observed(inst->nat_det, link, local, &observed);
        else if (!local) DEBUG_WARN(DEBUG_CATEGORY_NAT, "TCP IP report cannot read local socket peer=%s", link->etcp->log_name);
        memory_pool_free(inst->pkt_pool, pkt); goto done;
    }

#ifdef UTUN_HAVE_STANDBY
    if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_KEEPALIVE)) {
        char who[256];
        snprintf(who, sizeof(who), "%s addr=%s", link->etcp->log_name,
                 sockaddr_storage_to_str(&link->remote_addr).str);
        int is_ka = (pkt_code == ETCP_KEEPALIVE);
        standby_log_rx(is_ka ? (link->is_tcp ? "tcp keepalive" : "udp keepalive")
                              : (link->is_tcp ? "tcp data" : "udp data"), who);
    }
#endif

    if (etcp_keepalive_on_recv(pkt)) {
        goto done;
    }
    if (link->link_state == LINK_STATE_TRY_RECONNECT) {// 0 - just init, 1 - handshake, 2 - try reconnect, 3 - connected
        etcp_keepalive_link_ready(link);
        int old_state = link->link_state; link->link_state = LINK_STATE_CONNECTED;
        if (!etcp_link_publish_rx(link, old_state, link->link_status) || conn->reset_count != reset) {
            memory_pool_free(conn->instance->pkt_pool, pkt); goto done;
        }
    }

    if (link->link_state == LINK_STATE_CONNECTED) {
        if (memory_pool_is_freed(link->etcp->instance->pkt_pool, pkt)) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "pkt=%p ALREADY FREED in pkt_pool — HALTING", (void*)pkt);
            volatile int _halt = 1; while (_halt) {}
        }
        etcp_conn_input(pkt);
    } else {
        memory_pool_free(link->etcp->instance->pkt_pool, pkt);
    }
done:
    etcp_link_release(link); conn->callbacks_running--;
    return result;
}

/* Создаёт один серверный сокет из CFG_SERVER: UDP (etcp_socket_add) или TCP
 * (tcp_socket_add + stcp_server_listen + stcp_server_list_add).
 * Линки к соединениям не добавляет — это забота вызывающего.
 * Возвращает 0 при успехе и *out_sock = созданный сокет, -1 при ошибке. */
int etcp_create_server_socket(struct UTUN_INSTANCE* instance, struct CFG_SERVER* server, struct ETCP_SOCKET** out_sock) {
    if (!instance || !server || !out_sock) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "etcp_create_server_socket: invalid args"); return -1; }
    *out_sock = NULL;

    if (server->transport) {
        uint16_t port = 0;
        if (server->ip.ss_family == AF_INET) port = ntohs(((struct sockaddr_in*)&server->ip)->sin_port);
        else if (server->ip.ss_family == AF_INET6) port = ntohs(((struct sockaddr_in6*)&server->ip)->sin6_port);

        struct ETCP_SOCKET* ts = tcp_socket_add(instance, server);
        if (!ts) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "tcp_socket_add failed for %s", server->name); return -1; }

        struct stcp_link_config scfg = {.inst = instance, .listen_family = server->ip.ss_family,
                                        .reality_enabled = server->reality_enabled};
        struct stcp_server* tsrv = stcp_server_listen(&scfg, port, tcp_server_on_link, ts);
        if (!tsrv) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to create TCP server for %s", server->name);
            tcp_socket_remove(ts);
            return -1;
        }
        stcp_server_list_add(instance, tsrv);
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "TCP server %s on port %u", server->name, ss_get_port(&ts->local_addr));
        *out_sock = ts;
        return 0;
    }

    struct ETCP_SOCKET* e_sock = etcp_socket_add(instance, server);
    if (!e_sock) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to create socket for server %s", server->name); return -1; }
    *out_sock = e_sock;
    return 0;
}

// Создание всех listen-сокетов из конфига ([server] секции); автосокеты пропускаются.
// Вызывается до topo_group_init() чтобы заполнить etcp_sockets для nodeinfo.
// Возврат: 0 = все сокеты созданы, -1 = хотя бы один обязательный сокет не создан.
int init_sockets(struct UTUN_INSTANCE* instance) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!instance || !instance->config) return -1;
    if (instance->etcp_sockets || instance->stcp_servers) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Sockets already initialized, skipping");
        return 0;
    }

    struct utun_config* config = instance->config;

    /* автосокеты — ручные [server] сокеты пропускаются, create_sockets вызовется отдельно */
    if (config->global.auto_sockets) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "auto_sockets=enabled, skipping manual server sockets");
        instance->auto_socket_enabled = 1;
        return 0;
    }

    int success_count = 0;
    int fail_count = 0;

    // Create sockets for servers (incoming connections)
    struct CFG_SERVER* server = config->servers;
    while (server) {
        // Auto-detect local IP for public servers with 0.0.0.0
        uint32_t default_ip = 0;
        uint8_t default_ip6[16] = {0};
        int have_default_ip6 = 0;
        if (server->type == CFG_SERVER_TYPE_PUBLIC) {
            if (server->ip.ss_family == AF_INET) {
                struct sockaddr_in* sin = (struct sockaddr_in*)&server->ip;
                if (sin->sin_addr.s_addr == 0) {
                    struct sockaddr_storage remote;
                    memset(&remote, 0, sizeof(remote));
                    struct sockaddr_in* rem_sin = (struct sockaddr_in*)&remote;
                    rem_sin->sin_family = AF_INET;
                    rem_sin->sin_port = htons(53);
                    inet_pton(AF_INET, "8.8.8.8", &rem_sin->sin_addr);
                    struct sockaddr_storage local;
                    if (get_outgoing_local_ip(&server->ip, &remote, &local) == 0) {
                        struct sockaddr_in* local_sin = (struct sockaddr_in*)&local;
                        default_ip = local_sin->sin_addr.s_addr;
                    }
                    if (default_ip == 0) {
                        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "Failed to detect default route IP for server %s", server->name);
                    }
                }
            } else if (server->ip.ss_family == AF_INET6) {
                struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&server->ip;
                if (memcmp(&sin6->sin6_addr, &in6addr_any, 16) == 0) {
                    struct sockaddr_storage remote;
                    memset(&remote, 0, sizeof(remote));
                    struct sockaddr_in6* rem_sin6 = (struct sockaddr_in6*)&remote;
                    rem_sin6->sin6_family = AF_INET6;
                    rem_sin6->sin6_port = htons(53);
                    inet_pton(AF_INET6, "2001:4860:4860::8888", &rem_sin6->sin6_addr);
                    struct sockaddr_storage local;
                    if (get_outgoing_local_ip(&server->ip, &remote, &local) == 0) {
                        struct sockaddr_in6* local_sin6 = (struct sockaddr_in6*)&local;
                        memcpy(default_ip6, &local_sin6->sin6_addr, 16);
                        have_default_ip6 = 1;
                    }
                    if (!have_default_ip6) {
                        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "Failed to detect default route IPv6 for server %s", server->name);
                    }
                }
            }
        }

        struct ETCP_SOCKET* sock = NULL;
        if (etcp_create_server_socket(instance, server, &sock) != 0) {
            fail_count++;
            server = server->next;
            continue;
        }
        if (sock && !server->transport && default_ip != 0) {
            sock->local_defaultroute_ip = default_ip;
            if (server->ip.ss_family == AF_INET) {
                struct in_addr addr;
                addr.s_addr = default_ip;
                DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Server %s type=%s ip=%s", server->name, server_type_str(server->type), ip_to_str(&addr, AF_INET).str);
            }
        }
        if (sock && !server->transport && have_default_ip6) {
            memcpy(sock->local_defaultroute_ip6, default_ip6, 16);
        }
        success_count++;

        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Initialized server %s on %s (links: %d)",
               server->name, sockaddr_storage_to_str(&sock->local_addr).str, queue_entry_count(sock->links_queue));
        server = server->next;
    }

    if (fail_count) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "Configured socket initialization failed: opened=%d failed=%d", success_count, fail_count);
        return -1;
    }
    return 0;  // All OK
}

static void config_client_event(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* arg);

/* Адреса конфигурации добавляются в NCD; один постоянный handle принадлежит ядру, независимо от групп. */
static struct NODE_CONN_DIRECT* config_client_open(struct CONFIG_CONN_HANDLE* owner) {
    struct UTUN_INSTANCE* instance = owner->instance;
    struct CFG_CLIENT* client = &owner->config;
    struct TOPO_NODE ni = { .node_name = client->name };
    if (sc_hex_to_binary(client->peer_public_key_hex, ni.public_key, SC_PUBKEY_SIZE) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "invalid public key for client %s", client->name); return NULL;
    }
    ni.node_id = sc_derive_node_id_from_pubkey(ni.public_key);
    memcpy(owner->x25519, ni.public_key, sizeof(owner->x25519));
    for (struct CFG_CLIENT_LINK* endpoint = client->links; endpoint; endpoint = endpoint->next) {
        if (endpoint->remote_addr.ss_family != AF_INET && endpoint->remote_addr.ss_family != AF_INET6) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "client %s: invalid address family=%d", client->name, endpoint->remote_addr.ss_family);
            return NULL;
        }
        if (endpoint->server_name[0]) {
            struct ETCP_SOCKET* sock = NULL;
            for (struct ETCP_SOCKET* s = instance->etcp_sockets; s; s = s->next)
                if (!strcmp(s->name, endpoint->server_name)) { sock = s; break; }
            if (!sock || sock->probe_only || sock->local_addr.ss_family != endpoint->remote_addr.ss_family ||
                (client->reality_enabled && !sock->is_tcp)) {
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "client %s: missing/incompatible bind socket %s",
                            client->name, endpoint->server_name); return NULL;
            }
        }
    }
    struct NODE_CONN_DIRECT* handle = NULL;
    int result = client->links ? node_conn_direct_open_config(instance, &ni, config_client_event, owner, &handle) :
        node_conn_direct_open_key(instance, ni.node_id, ni.public_key, config_client_event, owner, &handle, NULL);
    if (result == NCD_ERR)
        DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "client %s: cannot open peer=%016llx source=%s", client->name,
                    (unsigned long long)ni.node_id, client->links ? "config" : "database");
    else DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "client %s: prepared peer=%016llx source=%s", client->name,
                     (unsigned long long)ni.node_id, client->links ? "config" : "database");
    return handle;
}

static int config_client_join_utun(struct CONFIG_CONN_HANDLE* owner) {
    struct TOPO_GROUP* group = topo_groups_get_default(owner->instance->topo_groups);
    if (!group) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "client %s: UTUN group is not running", owner->config.name); return -1; }
    if (owner->request && topo_group_peer_phase(owner->request) != TOPO_PEER_FAILED) return 0;
    topo_group_peer_close(owner->request); owner->request = NULL;
    if (topo_group_peer_open_key(group, owner->node_id, owner->x25519, &owner->request) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "client %s: cannot request UTUN peer=%016llx",
                    owner->config.name, (unsigned long long)owner->node_id); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "client %s: UTUN requested peer=%016llx",
               owner->config.name, (unsigned long long)owner->node_id);
    return 0;
}

static void config_client_retry(void* arg) {
    struct CONFIG_CONN_HANDLE* owner = arg; owner->retry_timer = NULL;
    struct NODE_CONN_DIRECT* transport = config_client_open(owner);
    if (transport) {
        struct NODE_CONN_DIRECT* old = owner->transport;
        owner->transport = transport; node_conn_direct_close(old);
        if (owner->config.links) node_conn_direct_config_changed(owner->instance, owner->node_id, owner->x25519);
    }
    if (!transport || (owner->instance->utun_started && config_client_join_utun(owner) < 0)) {
        owner->retry_timer = uasync_set_timeout(owner->instance->ua, 10000, owner, config_client_retry, "config_client_retry");
        if (!owner->retry_timer) DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "client %s: retry scheduling failed", owner->config.name);
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "client %s: configured endpoints applied peer=%016llx utun=%d", owner->config.name,
               (unsigned long long)owner->node_id, owner->instance->utun_started);
}

static void config_client_event(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* arg) {
    struct CONFIG_CONN_HANDLE* owner = arg;
    if (event == NCD_EVENT_UP) {
        DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "client %s: transport UP peer=%016llx utun=%d", owner->config.name,
                   (unsigned long long)owner->node_id, owner->instance->utun_started);
        if (!owner->instance->utun_started || config_client_join_utun(owner) == 0) return;
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_CONNECTION, "client %s: transport event=%d peer=%016llx; retaining configured request",
                   owner->config.name, event, (unsigned long long)owner->node_id);
        if (event != NCD_EVENT_CLOSED) return; /* NCD сохраняет DOWN/TIMEOUT для позднего UP. */
        owner->transport = NULL; node_conn_direct_close(handle);
        topo_group_peer_close(owner->request); owner->request = NULL;
    }
    if (!owner->retry_timer) {
        owner->retry_timer = uasync_set_timeout(owner->instance->ua, 10000, owner, config_client_retry, "config_client_retry");
        if (!owner->retry_timer) DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "client %s: retry scheduling failed", owner->config.name);
    }
}

static void config_handles_close(struct CONFIG_CONN_HANDLE* head, struct TOPO_GROUP* group, int replaced) {
    while (head) {
        struct CONFIG_CONN_HANDLE* next = head->next;
        struct UTUN_INSTANCE* inst = head->instance;
        uint64_t node_id = head->node_id;
        uint8_t key[SC_PUBKEY_SIZE]; memcpy(key, head->x25519, sizeof(key));
        const struct CONFIG_CONN_HANDLE* retained = inst->config_conn_handles;
        while (retained && memcmp(retained->x25519, key, sizeof(key))) retained = retained->next;
        if (replaced && !retained && group && head->request) topo_group_peer_leave(group, node_id);
        if (head->retry_timer) uasync_cancel_timeout(head->instance->ua, head->retry_timer);
        topo_group_peer_close(head->request);
        node_conn_direct_close(head->transport);
        while (head->config.links) {
            struct CFG_CLIENT_LINK* link = head->config.links;
            head->config.links = link->next; u_free(link);
        }
        u_free(head); head = next;
        if (replaced && !retained) node_conn_direct_config_changed(inst, node_id, key);
    }
}

int client_connections_join_utun(struct UTUN_INSTANCE* instance) {
    for (struct CONFIG_CONN_HANDLE* owner = instance->config_conn_handles; owner; owner = owner->next)
        if (config_client_join_utun(owner) < 0) return -1;
    return 0;
}

void client_connections_leave_utun(struct UTUN_INSTANCE* instance) {
    for (struct CONFIG_CONN_HANDLE* owner = instance ? instance->config_conn_handles : NULL; owner; owner = owner->next) {
        topo_group_peer_close(owner->request); owner->request = NULL;
    }
}

void client_connections_network_changed(struct UTUN_INSTANCE* instance) {
    for (struct CONFIG_CONN_HANDLE* owner = instance ? instance->config_conn_handles : NULL; owner; owner = owner->next) {
        if (owner->retry_timer) continue;
        owner->retry_timer = uasync_set_timeout(instance->ua, 1, owner, config_client_retry, "config_client_network_changed");
        if (!owner->retry_timer) DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "client %s: network refresh scheduling failed", owner->config.name);
        else DEBUG_DEBUG(DEBUG_CATEGORY_CONNECTION, "client %s: network changed; refreshing configured endpoints", owner->config.name);
    }
}

int init_client_connections(struct UTUN_INSTANCE* instance, struct CFG_CLIENT* clients) {
    if (!instance) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "configure clients without instance"); return -1; }
    struct TOPO_GROUP* group = topo_groups_get_default(instance->topo_groups);
    struct CONFIG_CONN_HANDLE* updated = NULL;
    int count = 0;
    for (struct CFG_CLIENT* client = clients; client; client = client->next) {
        struct CONFIG_CONN_HANDLE* owner = u_calloc(1, sizeof(*owner));
        if (!owner) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "client handle allocation failed"); goto fail; }
        owner->instance = instance; owner->config = *client; owner->config.links = NULL; owner->config.next = NULL;
        owner->next = updated; updated = owner;
        struct CFG_CLIENT_LINK** tail = &owner->config.links;
        for (struct CFG_CLIENT_LINK* link = client->links; link; link = link->next) {
            struct CFG_CLIENT_LINK* copy = u_malloc(sizeof(*copy));
            if (!copy) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "client %s: endpoint allocation failed", client->name); goto fail; }
            *copy = *link; copy->next = NULL;
            if (link->local_srv) snprintf(copy->server_name, sizeof(copy->server_name), "%s", link->local_srv->name);
            copy->local_srv = NULL; *tail = copy; tail = &copy->next;
        }
        owner->transport = config_client_open(owner);
        if (!owner->transport) goto fail;
        owner->node_id = node_conn_direct_node_id(owner->transport);
        if (instance->utun_started && config_client_join_utun(owner) < 0) goto fail;
        count++;
    }
    struct CONFIG_CONN_HANDLE* old = instance->config_conn_handles;
    instance->config_conn_handles = updated;
    config_handles_close(old, group, 1);
    for (struct CONFIG_CONN_HANDLE* owner = updated; owner; owner = owner->next) {
        const struct CONFIG_CONN_HANDLE* first = updated;
        while (memcmp(first->x25519, owner->x25519, SC_PUBKEY_SIZE)) first = first->next;
        if (first == owner) node_conn_direct_config_changed(instance, owner->node_id, owner->x25519);
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "configured %d core client transports utun=%d", count, instance->utun_started);
    return 0;
fail:
    config_handles_close(updated, NULL, 0);
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "client configuration rejected; previous transports and group requests retained");
    return -1;
}

// Инициализация сети: listen-сокеты (если ещё нет) + client-соединения через NCD из конфига.
int init_connections(struct UTUN_INSTANCE* instance) {
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "");
    if (!instance || !instance->config) return -1;

    struct utun_config* config = instance->config;
    int socket_result = 0;

    // If sockets already exist (created by init_sockets), check stored status
    if (instance->etcp_sockets || instance->stcp_servers) {
        socket_result = instance->socket_init_status;
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Sockets not initialized, calling init_sockets()");
        socket_result = init_sockets(instance);
        instance->socket_init_status = socket_result;
    }
    if (socket_result != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "Core transport cannot start after socket initialization failure: result=%d", socket_result);
        return -1;
    }

    if (node_conn_direct_init(instance) < 0) return -1;
    etcp_keepalive_register(instance);

    return 0;
}
