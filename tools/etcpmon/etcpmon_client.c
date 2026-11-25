/*
 * etcpmon_client.c - ETCP Monitor Client Network Implementation
 */

#include "etcpmon_client.h"
#include "etcpmon_graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ws2tcpip.h>
#include <windows.h>

//#pragma comment(lib, "ws2_32.lib")

/* Log file name */
#define LOG_FILENAME "etcpmon.log"
#define REQUEST_TIMEOUT_MS 5000

static struct etcpmon_pending_request* find_pending_request(struct etcpmon_client* client, uint8_t seq_id);

/* Кадр живёт до полной отправки; offset не меняет исходные байты и порядок FIFO. */
struct etcpmon_tx_frame {
    struct etcpmon_tx_frame* next;
    uint16_t size;
    uint16_t offset;
    uint8_t seq_id;
    uint8_t bytes[];
};

/* Единственная ветка аварийного отключения: лог, освобождение транспорта и уведомления владельца. */
static int client_fail(struct etcpmon_client* client, const char* reason, int error) {
    FILE* log = client->log_file ? client->log_file : stderr;
    fprintf(log, "%llu: [ERROR] %s: error=%d queued_frames=%u queued_bytes=%u\n",
            (unsigned long long)get_timestamp_ms(), reason, error, client->tx_count, (unsigned)client->tx_bytes);
    fflush(log);
    etcpmon_client_disconnect(client);
    if (client->on_disconnected) client->on_disconnected(client->user_data);
    if (client->on_error) client->on_error(reason, client->user_data);
    return -1;
}

/* Неблокирующая досылка: за вызов не более одного максимального кадра по объёму. */
static int flush_outgoing(struct etcpmon_client* client) {
    size_t budget = ETCPMON_MAX_MSG_SIZE;
    while (client->fully_connected && client->tx_head && budget) {
        struct etcpmon_tx_frame* frame = client->tx_head;
        int size = frame->size - frame->offset;
        if ((size_t)size > budget) size = (int)budget;
        int sent = send(client->sock, (const char*)frame->bytes + frame->offset, size, 0);
        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) return client_fail(client, "TCP send failed", error);
            if (!client->tx_blocked && client->log_file) {
                fprintf(client->log_file, "[DEBUG] TX waiting: frames=%u bytes=%u offset=%u/%u\n",
                        client->tx_count, (unsigned)client->tx_bytes, frame->offset, frame->size);
                fflush(client->log_file);
            }
            client->tx_blocked = 1;
            return 0;
        }
        if (!sent) return client_fail(client, "TCP send made no progress", 0);
        if (client->tx_blocked && client->log_file) {
            fprintf(client->log_file, "[DEBUG] TX resumed: sent=%d frames=%u bytes=%u\n", sent, client->tx_count, (unsigned)client->tx_bytes);
            fflush(client->log_file);
        }
        client->tx_blocked = 0;
        client->tx_progress_at = GetTickCount();
        frame->offset += sent;
        budget -= sent;
        if (frame->offset != frame->size) continue;
        struct etcpmon_pending_request* req = find_pending_request(client, frame->seq_id);
        if (req) { req->sent = 1; req->sent_at = client->tx_progress_at; }
        if (client->log_file) {
            fprintf(client->log_file, "[DEBUG] TX complete: type=0x%02X seq=%u size=%u\n",
                    frame->bytes[2], frame->seq_id, frame->size);
            fflush(client->log_file);
        }
        client->tx_head = frame->next;
        if (!client->tx_head) client->tx_tail = NULL;
        client->tx_bytes -= frame->size;
        client->tx_count--;
        free(frame);
    }
    return 0;
}

/* Принимает req во владение, если передан; публикация кадра и pending выполняется до первого send. */
static int send_message(struct etcpmon_client* client, uint8_t type, const void* payload,
                        uint16_t payload_size, struct etcpmon_pending_request* req) {
    size_t size = sizeof(struct etcpmon_msg_header) + (size_t)payload_size;
    if (!client || !client->connected || client->sock == INVALID_SOCKET || size > ETCPMON_MAX_MSG_SIZE ||
        (payload_size && !payload)) {
        FILE* log = client && client->log_file ? client->log_file : stderr;
        fprintf(log, "[ERROR] Invalid outgoing command: type=0x%02X size=%u connected=%d\n",
                type, (unsigned)size, client ? client->connected : 0);
        fflush(log); free(req);
        return -1;
    }
    if (client->tx_count >= ETCPMON_TX_MAX_FRAMES || size > ETCPMON_TX_MAX_BYTES - client->tx_bytes) {
        FILE* log = client->log_file ? client->log_file : stderr;
        fprintf(log, "[ERROR] TX queue full: type=0x%02X size=%u frames=%u bytes=%u\n",
                type, (unsigned)size, client->tx_count, (unsigned)client->tx_bytes);
        fflush(log); free(req);
        return -1;
    }
    struct etcpmon_tx_frame* frame = malloc(sizeof(*frame) + size);
    if (!frame) {
        FILE* log = client->log_file ? client->log_file : stderr;
        fprintf(log, "[ERROR] TX allocation failed: type=0x%02X size=%u\n", type, (unsigned)size);
        fflush(log); free(req);
        return -1;
    }
    frame->next = NULL; frame->size = (uint16_t)size; frame->offset = 0; frame->seq_id = req ? req->seq_id : 0;
    etcpmon_build_header((void*)frame->bytes, payload_size, type, frame->seq_id);
    if (payload_size) memcpy(frame->bytes + sizeof(struct etcpmon_msg_header), payload, payload_size);
    if (req) {
        if (client->pending_tail) client->pending_tail->next = req;
        else client->pending_head = req;
        client->pending_tail = req;
    }
    if (client->tx_tail) client->tx_tail->next = frame;
    else { client->tx_head = frame; client->tx_progress_at = GetTickCount(); }
    client->tx_tail = frame; client->tx_bytes += size; client->tx_count++;
    if (client->log_file) {
        fprintf(client->log_file, "[DEBUG] TX queued: type=0x%02X seq=%u size=%u frames=%u bytes=%u\n",
                type, frame->seq_id, (unsigned)size, client->tx_count, (unsigned)client->tx_bytes);
        fflush(client->log_file);
    }
    return flush_outgoing(client);
}

/* Get current timestamp in milliseconds (Unix epoch) */
uint64_t get_timestamp_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    
    /* Convert FILETIME (100-nanosecond intervals since 1601) to milliseconds */
    uint64_t time = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    /* Convert from 1601 to 1970 epoch (11644473600000 milliseconds) */
    return (time / 10000) - 11644473600000ULL;
}

/* Log hex data to file */
static void log_hex_data(FILE* log, const char* prefix, const uint8_t* data, size_t len) {
    if (!log || !data || len == 0) return;
    
    fprintf(log, "%llu: [%s] ", (unsigned long long)get_timestamp_ms(), prefix);
    for (size_t i = 0; i < len && i < 256; i++) {  /* Limit to 256 bytes per line */
        fprintf(log, "%02X ", data[i]);
    }
    if (len > 256) {
        fprintf(log, "... (%llu bytes total)", (unsigned long long)len);
    }
    fprintf(log, "\n");
    fflush(log);
}

void etcpmon_client_init(struct etcpmon_client* client) {
    if (!client) return;
    memset(client, 0, sizeof(*client));
    client->sock = INVALID_SOCKET;
    client->connected = 0;
    client->fully_connected = 0;
    client->log_file = NULL;
    client->next_seq_id = 1;  /* Start from 1, 0 = broadcast */
    memset(&client->history, 0, sizeof(client->history));
    for (int i = 0; i < GRAPH_METRIC_COUNT; i++) {
        client->history.min_val[i] = 1e9f;
        client->history.max_val[i] = -1e9f;
    }
    client->history.last_minmax_update = 0;
    
    /* Новый таймер авто-запроса метрик */
    client->last_metrics_request_ms = 0;
}

void etcpmon_client_cleanup(struct etcpmon_client* client) {
    if (!client) return;
    
    etcpmon_client_disconnect(client);
    
    /* Free pending requests */
    struct etcpmon_pending_request* req = client->pending_head;
    while (req) {
        struct etcpmon_pending_request* next = req->next;
        free(req);
        req = next;
    }
    client->pending_head = NULL;
    client->pending_tail = NULL;
    
    /* Close log file */
    if (client->log_file) {
        fprintf(client->log_file, "%llu: [LOG] Log closed\n", 
               (unsigned long long)get_timestamp_ms());
        fclose(client->log_file);
        client->log_file = NULL;
    }
    
    /* Free cached data */
    if (client->conn_list) {
        free(client->conn_list);
        client->conn_list = NULL;
    }
    if (client->socket_list) {
        free(client->socket_list);
        client->socket_list = NULL;
    }
    if (client->last_metrics) {
        free(client->last_metrics);
        client->last_metrics = NULL;
    }
    if (client->last_links) {
        free(client->last_links);
        client->last_links = NULL;
    }
}

int etcpmon_client_connect(struct etcpmon_client* client, const char* addr, uint16_t port) {
    if (!client || !addr) return -1;
    
    if (client->connected) {
        etcpmon_client_disconnect(client);
    }

    /* Open log file (truncate on start) */
    if (!client->log_file) {
        client->log_file = fopen(LOG_FILENAME, "w");
        if (client->log_file) {
            fprintf(client->log_file, "%llu: [LOG] ETCP Monitor communication log started\n", 
                   (unsigned long long)get_timestamp_ms());
            fprintf(client->log_file, "%llu: [LOG] Connecting to %s:%d\n", 
                   (unsigned long long)get_timestamp_ms(), addr, port);
            fflush(client->log_file);
        }
    }
    
    /* Create socket */
    client->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client->sock == INVALID_SOCKET) {
        if (client->log_file) {
            fprintf(client->log_file, "%llu: [ERROR] Failed to create socket\n", 
                   (unsigned long long)get_timestamp_ms());
            fflush(client->log_file);
        }
        if (client->on_error) {
            client->on_error("Failed to create socket", client->user_data);
        }
        return -1;
    }
    
    /* Set non-blocking mode */
    u_long nonblock = 1;
    if (ioctlsocket(client->sock, FIONBIO, &nonblock) == SOCKET_ERROR)
        return client_fail(client, "Failed to enable nonblocking TCP", WSAGetLastError());
    
    /* Setup address */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, addr, &server_addr.sin_addr) != 1) {
        if (client->log_file) {
            fprintf(client->log_file, "%llu: [ERROR] Invalid address: %s\n", 
                   (unsigned long long)get_timestamp_ms(), addr);
            fflush(client->log_file);
        }
        closesocket(client->sock);
        client->sock = INVALID_SOCKET;
        if (client->on_error) {
            client->on_error("Invalid address", client->user_data);
        }
        return -1;
    }
    
    /* Connect (non-blocking) */
    int result = connect(client->sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            if (client->log_file) {
                fprintf(client->log_file, "%llu: [ERROR] Connection failed: %d\n", 
                       (unsigned long long)get_timestamp_ms(), err);
                fflush(client->log_file);
            }
            closesocket(client->sock);
            client->sock = INVALID_SOCKET;
            if (client->on_error) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Connection failed: %d", err);
                client->on_error(msg, client->user_data);
            }
            return -1;
        }
        /* Connection in progress, will complete asynchronously */
    }
    
    strncpy(client->server_addr, addr, sizeof(client->server_addr) - 1);
    client->server_port = port;
    client->connected = 1;
    client->fully_connected = 0;
    client->connect_started_at = GetTickCount();
    
    if (client->log_file) {
        fprintf(client->log_file, "%llu: [LOG] Socket created, connection in progress\n", 
               (unsigned long long)get_timestamp_ms());
        fflush(client->log_file);
    }
    
    return 0;
}

void etcpmon_client_disconnect(struct etcpmon_client* client) {
    if (!client) return;
    
    if (client->sock != INVALID_SOCKET) {
        if (client->log_file) {
            fprintf(client->log_file, "%llu: [LOG] Connection closed\n", 
                   (unsigned long long)get_timestamp_ms());
            fflush(client->log_file);
        }
        
        closesocket(client->sock);
        client->sock = INVALID_SOCKET;
    }
    
    client->connected = 0;
    client->fully_connected = 0;
    while (client->tx_head) {
        struct etcpmon_tx_frame* frame = client->tx_head;
        client->tx_head = frame->next;
        free(frame);
    }
    client->tx_tail = NULL; client->tx_count = 0; client->tx_bytes = 0;
    client->tx_blocked = 0; client->tx_progress_at = 0; client->connect_started_at = 0;
    client->recv_len = 0;
    client->selected_peer_id = 0;
    client->last_metrics_request_ms = 0;
    free(client->conn_list);
    client->conn_list = NULL;
    client->conn_count = 0;
    free(client->socket_list);
    client->socket_list = NULL;
    client->socket_count = 0;
    free(client->last_metrics);
    client->last_metrics = NULL;
    free(client->last_links);
    client->last_links = NULL;
    client->last_links_count = 0;
    memset(&client->history, 0, sizeof(client->history));
    
    /* Очистить очередь pending requests и сбросить seq_id */
    struct etcpmon_pending_request* req = client->pending_head;
    while (req) {
        struct etcpmon_pending_request* next = req->next;
        free(req);
        req = next;
    }
    client->pending_head = NULL;
    client->pending_tail = NULL;
    client->next_seq_id = 1;
    client->last_response_seq_id = 0;
}

int etcpmon_client_is_connected(const struct etcpmon_client* client) {
    return client ? client->connected : 0;
}

int etcpmon_client_request_list(struct etcpmon_client* client) {
    return send_message(client, ETCPMON_CMD_LIST_CONN, NULL, 0, NULL);
}

int etcpmon_client_select_connection(struct etcpmon_client* client, uint64_t peer_node_id) {
    struct etcpmon_cmd_select command = {peer_node_id};
    if (send_message(client, ETCPMON_CMD_SELECT_CONN, &command, sizeof(command), NULL) < 0) return -1;
    client->selected_peer_id = peer_node_id;
    client->last_metrics_request_ms = 0;
    memset(&client->history, 0, sizeof(client->history));
    return 0;
}

int etcpmon_client_request_socket_list(struct etcpmon_client* client) {
    return send_message(client, ETCPMON_CMD_LIST_SOCKETS, NULL, 0, NULL);
}

int etcpmon_client_request_metrics(struct etcpmon_client* client) {
    return send_message(client, ETCPMON_CMD_GET_METRICS, NULL, 0, NULL);
}

static void action_auto_callback(void* arg, uint8_t msg_type, const uint8_t* payload, uint16_t payload_size) {
    (void)arg; (void)msg_type; (void)payload; (void)payload_size;
    /* legacy callback is called via on_action_result in handle_response */
}

int etcpmon_client_send_action(struct etcpmon_client* client, const char* action) {
    if (!client || client->sock == INVALID_SOCKET || !action) return -1;

    struct etcpmon_cmd_action cmd;
    memset(&cmd, 0, sizeof(cmd));
    strncpy(cmd.action, action, sizeof(cmd.action) - 1);
    cmd.action[sizeof(cmd.action) - 1] = '\0';

    return etcpmon_client_send_request(client, ETCPMON_CMD_ACTION, &cmd, sizeof(cmd),
                                        action_auto_callback, NULL);
}

/* Send request with callback - queued version */
int etcpmon_client_send_request(struct etcpmon_client* client,
                                 uint8_t type, const void* payload, uint16_t payload_size,
                                 void (*callback)(void*, uint8_t, const uint8_t*, uint16_t), void* arg) {
    if (!client || !callback) {
        fprintf(stderr, "[ERROR] Invalid request owner/callback: type=0x%02X\n", type);
        return -1;
    }
    int attempts = 0;
    while (find_pending_request(client, client->next_seq_id) && attempts++ < 255) {
        if (++client->next_seq_id == 0) client->next_seq_id = 1;
    }
    if (attempts >= 255) {
        FILE* log = client->log_file ? client->log_file : stderr;
        fprintf(log, "[ERROR] All request sequence IDs are in use\n");
        fflush(log);
        return -1;
    }
    struct etcpmon_pending_request* req = calloc(1, sizeof(*req));
    if (!req) {
        FILE* log = client->log_file ? client->log_file : stderr;
        fprintf(log, "[ERROR] Pending request allocation failed: type=0x%02X\n", type);
        fflush(log);
        return -1;
    }
    req->seq_id = client->next_seq_id++;
    if (!client->next_seq_id) client->next_seq_id = 1;
    req->callback = callback; req->arg = arg; req->type = type;
    return send_message(client, type, payload, payload_size, req);
}

static void handle_response(struct etcpmon_client* client, uint8_t msg_type, 
                           const uint8_t* payload, uint16_t payload_size) {
    switch (msg_type) {
        case ETCPMON_RSP_CONN_LIST: {
            if (payload_size < sizeof(struct etcpmon_rsp_conn_list)) {
                if (client->log_file) fprintf(client->log_file, "[ERROR] Empty RSP_CONN_LIST payload\n");
                break;
            }
            
            struct etcpmon_rsp_conn_list* rsp = (struct etcpmon_rsp_conn_list*)payload;
            uint8_t count = rsp->count;
            
            if (count > ETCPMON_MAX_CONNECTIONS || payload_size != sizeof(*rsp) + count * sizeof(struct etcpmon_conn_info)) {
                if (client->log_file) fprintf(client->log_file, "[ERROR] Invalid RSP_CONN_LIST: count=%u size=%u\n", count, payload_size);
                break;
            }
            
            /* Update cached list */
            struct etcpmon_conn_info* list = count ? malloc(count * sizeof(*list)) : NULL;
            if (count && !list) {
                if (client->log_file) fprintf(client->log_file, "[ERROR] Failed to allocate connection list: count=%u\n", count);
                break;
            }
            {
                if (count) memcpy(list, payload + sizeof(*rsp), count * sizeof(*list));
                for (uint8_t i = 0; i < count; i++) list[i].name[ETCPMON_MAX_CONN_NAME - 1] = '\0';
                free(client->conn_list);
                client->conn_list = list;
                client->conn_count = count;
                
                if (client->log_file) {
                    fprintf(client->log_file, "%llu: [LOG] RSP_CONN_LIST count=%d\n", 
                           (unsigned long long)get_timestamp_ms(), count);
                    for (uint8_t i = 0; i < count; i++) {
                        fprintf(client->log_file, "%llu: [LOG]   [%d] peer_id=%016llX name='%s'\n", 
                               (unsigned long long)get_timestamp_ms(), i,
                               (unsigned long long)client->conn_list[i].peer_node_id,
                               client->conn_list[i].name);
                    }
                    fflush(client->log_file);
                }
                
                if (client->on_conn_list) {
                    client->on_conn_list(client->conn_list, count, client->user_data);
                }
            }
            break;
        }
        
        case ETCPMON_RSP_SOCKET_LIST: {
            if (payload_size < sizeof(struct etcpmon_rsp_socket_list)) break;

            struct etcpmon_rsp_socket_list* rsp = (struct etcpmon_rsp_socket_list*)payload;
            uint8_t count = rsp->count;

            if (count > ETCPMON_MAX_CONNECTIONS || payload_size != sizeof(*rsp) + count * sizeof(struct etcpmon_socket_info)) {
                if (client->log_file) fprintf(client->log_file, "[ERROR] Invalid RSP_SOCKET_LIST: count=%u size=%u\n", count, payload_size);
                break;
            }

            struct etcpmon_socket_info* list = count ? malloc(count * sizeof(*list)) : NULL;
            if (count && !list) {
                if (client->log_file) fprintf(client->log_file, "[ERROR] Failed to allocate socket list: count=%u\n", count);
                break;
            }
            {
                if (count) memcpy(list, payload + sizeof(*rsp), count * sizeof(*list));
                for (uint8_t i = 0; i < count; i++) {
                    list[i].name[sizeof(list[i].name) - 1] = '\0';
                    list[i].ip[sizeof(list[i].ip) - 1] = '\0';
                    list[i].nat_ip[sizeof(list[i].nat_ip) - 1] = '\0';
                }
                free(client->socket_list);
                client->socket_list = list;
                client->socket_count = count;

                if (client->log_file) {
                    fprintf(client->log_file, "%llu: [LOG] RSP_SOCKET_LIST count=%d\n",
                           (unsigned long long)get_timestamp_ms(), count);
                    fflush(client->log_file);
                }

                if (client->on_socket_list) {
                    client->on_socket_list(client->socket_list, count, client->user_data);
                }
            }
            break;
        }

        case ETCPMON_RSP_METRICS: {
            if (payload_size < sizeof(struct etcpmon_rsp_metrics)) break;

            struct etcpmon_rsp_metrics* rsp = (struct etcpmon_rsp_metrics*)payload;
            uint8_t links_count = rsp->etcp.links_count;

            if (rsp->etcp.peer_node_id != client->selected_peer_id || client->selected_peer_id == 0) {
                if (client->log_file) {
                    fprintf(client->log_file, "[LOG] Ignoring stale metrics: peer=%016llX selected=%016llX\n",
                            (unsigned long long)rsp->etcp.peer_node_id, (unsigned long long)client->selected_peer_id);
                }
                break;
            }
            
            if (links_count > ETCPMON_MAX_LINKS || payload_size != sizeof(*rsp) + links_count * sizeof(struct etcpmon_link_metrics)) {
                if (client->log_file) fprintf(client->log_file, "[ERROR] Invalid RSP_METRICS: links=%u size=%u\n", links_count, payload_size);
                break;
            }
            
            /* Update cached metrics */
            struct etcpmon_rsp_metrics* metrics = malloc(sizeof(*metrics));
            struct etcpmon_link_metrics* links = links_count ? malloc(links_count * sizeof(*links)) : NULL;
            if (!metrics || (links_count && !links)) {
                free(metrics);
                free(links);
                if (client->log_file) fprintf(client->log_file, "[ERROR] Failed to allocate metrics: links=%u\n", links_count);
                break;
            }
            {
                free(client->last_metrics);
                free(client->last_links);
                client->last_metrics = metrics;
                client->last_links = links;
                memcpy(client->last_metrics, rsp, sizeof(*rsp));
                if (links_count) memcpy(client->last_links, payload + sizeof(*rsp),
                       links_count * sizeof(struct etcpmon_link_metrics));
                client->last_links_count = links_count;
                
                if (client->log_file) {
                    fprintf(client->log_file, "%llu: [LOG] RSP_METRICS rtt=%u bytes=%llu links=%d\n", 
                           (unsigned long long)get_timestamp_ms(),
                           rsp->etcp.rtt_last,
                           (unsigned long long)rsp->etcp.bytes_sent_total,
                           links_count);
                    fflush(client->log_file);
                }
                
                etcpmon_client_add_to_history(client, client->last_metrics);
                if (client->on_metrics) {
                    client->on_metrics(client->last_metrics, client->last_links, 
                                        links_count, client->user_data);
                }
                
            }
            break;
        }
        
        case ETCPMON_RSP_ERROR: {
            if (payload_size > sizeof(struct etcpmon_rsp_error) &&
                memchr(payload + sizeof(struct etcpmon_rsp_error), 0, payload_size - sizeof(struct etcpmon_rsp_error))) {
                struct etcpmon_rsp_error* rsp = (struct etcpmon_rsp_error*)payload;
                const char* msg = (const char*)(payload + sizeof(*rsp));

                if (client->log_file) {
                    fprintf(client->log_file, "%llu: [LOG] RSP_ERROR code=%d msg='%s'\n",
                           (unsigned long long)get_timestamp_ms(), rsp->error_code, msg);
                    fflush(client->log_file);
                }

                if (client->on_error) {
                    client->on_error(msg, client->user_data);
                }
            } else if (client->log_file) {
                fprintf(client->log_file, "[ERROR] Invalid RSP_ERROR string: size=%u\n", payload_size);
            }
            break;
        }

        case ETCPMON_RSP_ACTION_RESULT: {
            if (payload_size > sizeof(struct etcpmon_rsp_action_result) &&
                memchr(payload + sizeof(struct etcpmon_rsp_action_result), 0, payload_size - sizeof(struct etcpmon_rsp_action_result))) {
                const char* text = (const char*)(payload + sizeof(struct etcpmon_rsp_action_result));

                if (client->log_file) {
                    fprintf(client->log_file, "%llu: [LOG] RSP_ACTION_RESULT len=%u\n",
                           (unsigned long long)get_timestamp_ms(), payload_size);
                    fflush(client->log_file);
                }

                if (client->on_action_result) {
                    client->on_action_result(text, client->user_data);
                }
            } else if (client->log_file) {
                fprintf(client->log_file, "[ERROR] Invalid RSP_ACTION_RESULT string: size=%u\n", payload_size);
            }
            break;
        }

        case ETCPMON_RSP_DEBUG_CONFIG: {
            const uint8_t* names_end = payload_size > 2 ? memchr(payload + 2, 0, payload_size - 2) : NULL;
            if (!names_end || payload[1] > ETCPMON_MAX_DEBUG_CATEGORIES ||
                (size_t)(names_end - payload) + 1 + payload[1] != payload_size) {
                if (client->log_file) fprintf(client->log_file, "[ERROR] Invalid RSP_DEBUG_CONFIG: size=%u\n", payload_size);
                break;
            }
            {
                client->debug_global_level = payload[0];
                client->debug_category_count = payload[1];
                uint16_t names_len = (uint16_t)(names_end - payload - 2) + 1;
                if (2 + names_len <= payload_size) {
                    size_t copy_len = names_len < sizeof(client->debug_category_names) ? names_len : sizeof(client->debug_category_names) - 1;
                    memcpy(client->debug_category_names, payload + 2, copy_len);
                    client->debug_category_names[copy_len] = '\0';
                    uint16_t levels_off = 2 + names_len;
                    uint8_t lvl_count = client->debug_category_count;
                    if (lvl_count > ETCPMON_MAX_DEBUG_CATEGORIES) lvl_count = ETCPMON_MAX_DEBUG_CATEGORIES;
                    for (uint8_t i = 0; i < lvl_count && levels_off + i < payload_size; i++)
                        client->debug_category_levels[i] = payload[levels_off + i];
                    client->debug_config_dirty = 1;
                    if (client->log_file) {
                        fprintf(client->log_file, "%llu: [LOG] RSP_DEBUG_CONFIG global=%d categories=%d\n",
                               (unsigned long long)get_timestamp_ms(), client->debug_global_level, client->debug_category_count);
                        fflush(client->log_file);
                    }
                }
            }
            break;
        }
    }
}

/* Find pending request by seq_id */
static struct etcpmon_pending_request* find_pending_request(struct etcpmon_client* client, uint8_t seq_id) {
    struct etcpmon_pending_request* req = client->pending_head;
    while (req) {
        if (req->seq_id == seq_id) {
            return req;
        }
        req = req->next;
    }
    return NULL;
}

/* Remove pending request from queue */
static void remove_pending_request(struct etcpmon_client* client, struct etcpmon_pending_request* req) {
    struct etcpmon_pending_request** curr = &client->pending_head;
    struct etcpmon_pending_request* prev = NULL;
    while (*curr) {
        if (*curr == req) {
            *curr = req->next;
            if (client->pending_tail == req) {
                client->pending_tail = prev;
            }
            free(req);
            return;
        }
        prev = *curr;
        curr = &(*curr)->next;
    }
}

/* Handle response - find matching request and call callback */
/* Полный payload проверяется до снятия pending и любых пользовательских callbacks. */
static int valid_response(uint8_t type, const uint8_t* payload, uint16_t size) {
    switch (type) {
        case ETCPMON_RSP_CONN_LIST:
            return size >= 1 && payload[0] <= ETCPMON_MAX_CONNECTIONS &&
                size == 1 + payload[0] * sizeof(struct etcpmon_conn_info);
        case ETCPMON_RSP_SOCKET_LIST:
            return size >= 1 && payload[0] <= ETCPMON_MAX_CONNECTIONS &&
                size == 1 + payload[0] * sizeof(struct etcpmon_socket_info);
        case ETCPMON_RSP_METRICS: {
            if (size < sizeof(struct etcpmon_rsp_metrics)) return 0;
            const struct etcpmon_rsp_metrics* metrics = (const void*)payload;
            return metrics->etcp.links_count <= ETCPMON_MAX_LINKS &&
                size == sizeof(*metrics) + metrics->etcp.links_count * sizeof(struct etcpmon_link_metrics);
        }
        case ETCPMON_RSP_ERROR:
        case ETCPMON_RSP_ACTION_RESULT:
            return size >= 2 && memchr(payload + 1, 0, size - 1) != NULL;
        case ETCPMON_RSP_DEBUG_CONFIG: {
            const uint8_t* end = size > 2 ? memchr(payload + 2, 0, size - 2) : NULL;
            if (!end || payload[0] > 5 || payload[1] > ETCPMON_MAX_DEBUG_CATEGORIES ||
                (size_t)(end - payload) + 1 + payload[1] != size) return 0;
            unsigned names = end == payload + 2 ? 0 : 1;
            for (const uint8_t* p = payload + 2; p < end; p++) if (*p == ',') names++;
            return names == payload[1];
        }
        case ETCPMON_RSP_NODE_INFO: return size >= 2 && payload[0] == 1 && payload[1] == 4;
        case ETCPMON_RSP_NODE_INFO_END: return size == 0;
        case ETCPMON_RSP_NODE_REMOVED: return size == sizeof(uint64_t);
        default: return 0;
    }
}

/* Тип успешного ответа; RSP_ERROR завершает любой запрос. */
static uint8_t expected_response(uint8_t type) {
    switch (type) {
        case ETCPMON_CMD_LIST_CONN: return ETCPMON_RSP_CONN_LIST;
        case ETCPMON_CMD_GET_METRICS: return ETCPMON_RSP_METRICS;
        case ETCPMON_CMD_LIST_SOCKETS: return ETCPMON_RSP_SOCKET_LIST;
        case ETCPMON_CMD_ACTION: return ETCPMON_RSP_ACTION_RESULT;
        case ETCPMON_CMD_GET_DEBUG_CONFIG: return ETCPMON_RSP_DEBUG_CONFIG;
        case ETCPMON_CMD_SUBSCRIBE_NODES: return ETCPMON_RSP_NODE_INFO_END;
        default: return 0;
    }
}

static void handle_response_with_callback(struct etcpmon_client* client, uint8_t msg_type,
                                           const uint8_t* payload, uint16_t payload_size) {
    uint8_t seq_id = client->last_response_seq_id;
    if (!valid_response(msg_type, payload, payload_size)) {
        if (client->log_file) fprintf(client->log_file, "[ERROR] Invalid response: type=0x%02X seq=%u size=%u\n",
                                      msg_type, seq_id, payload_size);
        client_fail(client, "Invalid response payload", 0);
        return;
    }
    
    /* Find pending request */
    struct etcpmon_pending_request* req = find_pending_request(client, seq_id);
    
    if (req) {
        if (!req->sent || (msg_type != ETCPMON_RSP_ERROR && msg_type != expected_response(req->type))) {
            if (client->log_file) fprintf(client->log_file, "[ERROR] Response mismatch: command=0x%02X response=0x%02X seq=%u sent=%u\n",
                                          req->type, msg_type, seq_id, req->sent);
            client_fail(client, "Response does not match request", 0);
            return;
        }
        /* Callback может отправить новый запрос или отключить клиент. */
        void (*callback)(void*, uint8_t, const uint8_t*, uint16_t) = req->callback;
        void* arg = req->arg;
        remove_pending_request(client, req);
        if (callback) callback(arg, msg_type, payload, payload_size);
        if (!client->connected) return;
        
        if (client->log_file) {
            fprintf(client->log_file, "%llu: [LOG] Response handled, seq=%d removed from queue\n",
                   (unsigned long long)get_timestamp_ms(), seq_id);
            fflush(client->log_file);
        }
    } else {
        if (seq_id) {
            if (client->log_file) {
                fprintf(client->log_file, "[WARN] Ignoring response without pending: type=0x%02X seq=%u\n", msg_type, seq_id);
                fflush(client->log_file);
            }
            return;
        }
        /* No pending request - maybe broadcast/unsolicited or stale */
        if (client->log_file) {
            fprintf(client->log_file, "%llu: [LOG] Response seq=%d - no pending request (broadcast?)\n",
                   (unsigned long long)get_timestamp_ms(), seq_id);
            fflush(client->log_file);
        }
    }
    
    /* === НОВОЕ: ВСЕГДА вызываем legacy обработчики (on_conn_list / on_metrics / on_error) ===
     * Это позволяет использовать и старый код, и новый асинхронный send_request одновременно */
    if (seq_id == 0 || msg_type == ETCPMON_RSP_CONN_LIST ||
        msg_type == ETCPMON_RSP_SOCKET_LIST || msg_type == ETCPMON_RSP_DEBUG_CONFIG ||
        msg_type == ETCPMON_RSP_METRICS || msg_type == ETCPMON_RSP_ERROR ||
        msg_type == ETCPMON_RSP_ACTION_RESULT) {
        handle_response(client, msg_type, payload, payload_size);
    }
}

static void metrics_auto_callback(void* arg, uint8_t msg_type, const uint8_t* payload, uint16_t payload_size) {
    (void)arg;
    (void)msg_type;
    (void)payload;
    (void)payload_size;
    /* Ничего не делаем — legacy on_metrics вызывается автоматически ниже */
}

int etcpmon_client_process(struct etcpmon_client* client) {
    if (!client || client->sock == INVALID_SOCKET) return -1;
    if (!client->fully_connected) {
        if (GetTickCount() - client->connect_started_at >= REQUEST_TIMEOUT_MS)
            return client_fail(client, "Connect timeout", WSAETIMEDOUT);
        fd_set writable, failed;
        FD_ZERO(&writable); FD_ZERO(&failed);
        FD_SET(client->sock, &writable); FD_SET(client->sock, &failed);
        struct timeval timeout = {0, 0};
        int ready = select(0, NULL, &writable, &failed, &timeout);
        if (!ready) return 0;
        if (ready == SOCKET_ERROR) return client_fail(client, "Connect select failed", WSAGetLastError());
        int error = 0, len = sizeof(error);
        if (getsockopt(client->sock, SOL_SOCKET, SO_ERROR, (char*)&error, &len) == SOCKET_ERROR)
            return client_fail(client, "Connect SO_ERROR failed", WSAGetLastError());
        if (error == WSAEINPROGRESS || error == WSAEALREADY) return 0;
        if (error) return client_fail(client, "Connect failed", error);
        client->fully_connected = 1;
        if (client->log_file) {
            fprintf(client->log_file, "%llu: [INFO] Connection established to %s:%u\n",
                    (unsigned long long)get_timestamp_ms(), client->server_addr, client->server_port);
            fflush(client->log_file);
        }
        if (client->on_connected) client->on_connected(client->user_data);
        return client->connected ? 0 : -1;
    }
    if (flush_outgoing(client) < 0) return -1;
    int space = ETCPMON_MAX_MSG_SIZE - client->recv_len;
    if (space <= 0) return client_fail(client, "Receive buffer full without a complete frame", 0);
    uint8_t* input = client->recv_buffer + client->recv_len;
    int received = recv(client->sock, (char*)input, space, 0);
    if (received == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
        return client_fail(client, "TCP recv failed", WSAGetLastError());
    if (!received) return client_fail(client, "Connection closed by server", 0);
    if (received > 0) {
        if (client->log_file) log_hex_data(client->log_file, "RX", input, received);
        client->recv_len += received;
    }
    int processed = 0;
    while (client->recv_len >= sizeof(struct etcpmon_msg_header)) {
        struct etcpmon_msg_header* header = (void*)client->recv_buffer;
        if (etcpmon_validate_header(header) < 0) return client_fail(client, "Invalid TCP frame header", 0);
        uint16_t size = header->size;
        if (client->recv_len < size) break;
        client->last_response_seq_id = header->seq_id;
        if (client->log_file) {
            fprintf(client->log_file, "[DEBUG] RX frame: type=0x%02X seq=%u size=%u\n", header->type, header->seq_id, size);
            fflush(client->log_file);
        }
        handle_response_with_callback(client, header->type, client->recv_buffer + sizeof(*header), size - sizeof(*header));
        if (!client->connected) return -1;
        client->recv_len -= size;
        memmove(client->recv_buffer, client->recv_buffer + size, client->recv_len);
        processed++;
    }
    DWORD tick = GetTickCount();
    if (client->tx_head && tick - client->tx_progress_at >= REQUEST_TIMEOUT_MS)
        return client_fail(client, "TCP TX progress timeout", WSAETIMEDOUT);
    int metrics_pending = 0;
    for (struct etcpmon_pending_request* req = client->pending_head; req; req = req->next) {
        if (req->sent && tick - req->sent_at >= REQUEST_TIMEOUT_MS) {
            if (client->log_file) fprintf(client->log_file, "[ERROR] Response timeout: type=0x%02X seq=%u elapsed=%lu ms\n",
                                          req->type, req->seq_id, (unsigned long)(tick - req->sent_at));
            return client_fail(client, "Response timeout", WSAETIMEDOUT);
        }
        if (req->type == ETCPMON_CMD_GET_METRICS) metrics_pending = 1;
    }
    if (client->selected_peer_id && !metrics_pending) {
        uint64_t now = GetTickCount64();
        if (now - client->last_metrics_request_ms >= ETCPMON_UPDATE_INTERVAL_MS) {
            if (etcpmon_client_send_request(client, ETCPMON_CMD_GET_METRICS, NULL, 0, metrics_auto_callback, NULL) < 0)
                return client->connected ? client_fail(client, "Failed to queue metrics request", 0) : -1;
            client->last_metrics_request_ms = now;
        }
    }
    return processed;
}

void etcpmon_client_set_callbacks(struct etcpmon_client* client,
                                   void (*on_connected)(void*),
                                   void (*on_disconnected)(void*),
                                   void (*on_conn_list)(struct etcpmon_conn_info*, uint8_t, void*),
                                   void (*on_socket_list)(struct etcpmon_socket_info*, uint8_t, void*),
                                   void (*on_metrics)(struct etcpmon_rsp_metrics*,
                                                      struct etcpmon_link_metrics*,
                                                      uint8_t, void*),
                                   void (*on_error)(const char*, void*),
                                   void (*on_action_result)(const char*, void*),
                                   void* user_data) {
    if (!client) return;

    client->on_connected = on_connected;
    client->on_disconnected = on_disconnected;
    client->on_conn_list = on_conn_list;
    client->on_socket_list = on_socket_list;
    client->on_metrics = on_metrics;
    client->on_error = on_error;
    client->on_action_result = on_action_result;
    client->user_data = user_data;
}

const char* etcpmon_client_get_conn_name(struct etcpmon_client* client, uint64_t peer_id) {
    if (!client || !client->conn_list) return NULL;
    
    for (uint8_t i = 0; i < client->conn_count; i++) {
        if (client->conn_list[i].peer_node_id == peer_id) {
            return client->conn_list[i].name;
        }
    }
    return NULL;
}

struct metrics_history* etcpmon_client_get_history(struct etcpmon_client* client) {
    if (!client) return NULL;
    return &client->history;
}

static void debug_config_auto_callback(void* arg, uint8_t msg_type, const uint8_t* payload, uint16_t payload_size) {
    (void)arg; (void)msg_type; (void)payload; (void)payload_size;
}

int etcpmon_client_request_debug_config(struct etcpmon_client* client) {
    if (!client || client->sock == INVALID_SOCKET) return -1;
    return etcpmon_client_send_request(client, ETCPMON_CMD_GET_DEBUG_CONFIG, NULL, 0,
                                        debug_config_auto_callback, NULL);
}

int etcpmon_client_send_debug_config(struct etcpmon_client* client,
                                      uint8_t global_level, uint8_t count, const uint8_t* levels) {
    if (!client || !levels || !count || count > ETCPMON_MAX_DEBUG_CATEGORIES || global_level > 5) {
        fprintf(stderr, "[ERROR] Invalid debug config: global=%u count=%u\n", global_level, count);
        return -1;
    }
    uint8_t payload[2 + ETCPMON_MAX_DEBUG_CATEGORIES];
    payload[0] = global_level; payload[1] = count;
    memcpy(payload + 2, levels, count);
    if (send_message(client, ETCPMON_CMD_SET_DEBUG_CONFIG, payload, 2 + count, NULL) < 0) return -1;
    client->debug_global_level = global_level;
    memcpy(client->debug_category_levels, levels, count);
    return 0;
}

void etcpmon_client_add_to_history(struct etcpmon_client* client, struct etcpmon_rsp_metrics* metrics) {
    if (!client || !metrics) return;
    
    struct metrics_history* h = &client->history;
    int idx = h->head;
    for (int metric = 0; metric < GRAPH_METRIC_COUNT; metric++) h->values[metric][idx] = 0;
    
   /* BBR per-link metrics (use first link as representative) */
    if (client->last_links_count > 0 && client->last_links) {
        struct etcpmon_link_metrics* l = &client->last_links[0];
        h->values[GRAPH_METRIC_INFLIGHT_BYTES][idx] = (float)l->inflight_bytes;
        h->values[GRAPH_METRIC_INFLIGHT_LIM][idx]   = (float)l->inflight_lim_bytes;
        h->values[GRAPH_METRIC_MIN_RTT][idx]        = l->bbr_min_rtt_us / 1000.f;
        h->values[GRAPH_METRIC_BW_HI][idx]          = (float)l->bbr_bw_hi;
        h->values[GRAPH_METRIC_PACING_RATE][idx]    = (float)l->bbr_pacing_rate;
        h->values[GRAPH_METRIC_BW_LO][idx]          = (float)l->bbr_bw_lo;
        h->values[GRAPH_METRIC_INFLIGHT_HI][idx]     = (float)l->bbr_inflight_hi;
        h->values[GRAPH_METRIC_BBR_MODE][idx]        = (float)l->bbr_mode;
    }
    
    h->head = (h->head + 1) % GRAPH_HISTORY_SIZE;
    if (h->count < GRAPH_HISTORY_SIZE) h->count++;

    if (client->log_file) {
        fprintf(client->log_file, "history updated: count=%d head=%d\n", h->count, h->head);
        fflush(client->log_file);
    }
}
