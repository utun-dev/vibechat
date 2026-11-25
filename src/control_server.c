/*
 * control_server.c - Control Socket Server Implementation
 * 
 * Handles ETCP monitoring requests from clients
 */

#include "control_server.h"
#include "utun_instance.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "tun_if.h"
#include "route_lib.h"
#include "topo_group.h"
#include "nat_detection.h"
#include "stun_nat.h"
#include "etcp_router.h"
#include "topo_node.h"
#include "pkt_normalizer.h"
#include "../tools/etcpmon/etcpmon_protocol.h"
#include "../lib/u_async.h"
#include "../lib/timeout_heap.h"
#include "../lib/memory_pool.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/ll_queue.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#endif

#ifndef DEBUG_CATEGORY_CONTROL
#define DEBUG_CATEGORY_CONTROL 1
#endif

/* Log file name */
#define LOG_FILENAME "control_server.log"

/* ============================================================================
 * Logging Helpers
 * ============================================================================ */

#ifdef _WIN32
#include <windows.h>

static uint64_t get_timestamp_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t time = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (time / 10000) - 11644473600000ULL;
}
#else
#include <time.h>

static uint64_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

static void log_hex_data(FILE* log, const char* prefix, const uint8_t* data, size_t len) {
    if (!log || !data || len == 0) return;
    
    fprintf(log, "%llu: [%s] ", (unsigned long long)get_timestamp_ms(), prefix);
    for (size_t i = 0; i < len && i < 256; i++) {
        fprintf(log, "%02X ", data[i]);
    }
    if (len > 256) {
        fprintf(log, "... (%llu bytes total)", (unsigned long long)len);
    }
    fprintf(log, "\n");
    fflush(log);
}

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void accept_callback(socket_t fd, void* arg);
static void client_read_callback(socket_t fd, void* arg);
static void client_write_callback(socket_t fd, void* arg);
static void client_except_callback(socket_t fd, void* arg);
static int handle_client_data(struct control_server* server, struct control_client* client);
static void close_client(struct control_server* server, struct control_client* client);
static void control_client_idle_cb(void* arg);
static void control_output_ready(struct ll_queue* queue, void* arg);
static int control_wait_output(struct control_client* client);
static int control_client_arm_timer(struct control_client* client, int tx);

static void send_conn_list(struct control_server* server, struct control_client* client, uint8_t seq_id);
static void send_socket_list(struct control_server* server, struct control_client* client, uint8_t seq_id);
static void send_metrics(struct control_server* server, struct control_client* client, uint8_t seq_id);
static void send_debug_config(struct control_server* server, struct control_client* client, uint8_t seq_id);
static void send_error(struct control_client* client, uint8_t error_code, const char* msg, uint8_t seq_id);
static int handle_action(struct control_server* server, struct control_client* client,
                         const uint8_t* payload, uint16_t size, uint8_t seq_id);

static void control_server_send_node_list(struct control_server* server, struct control_client* client, uint8_t seq_id);
static void send_single_node_info(struct control_server* server, struct control_client* client, struct TOPO_GROUP_NODE* nq);
static int control_client_send(struct control_server* server, struct control_client* client, uint8_t* dgram, uint16_t len);


/* ETCP ID/subcmd constants for BGP_NODEINFO_PACKET (from route_bgp.h) */
#define CTRL_BGP_CMD     0x01
#define CTRL_BGP_SUBCMD  0x04

/* ============================================================================
 * Server Initialization
 * ============================================================================ */

int control_server_init(struct control_server* server,
                        struct UASYNC* ua,
                        struct UTUN_INSTANCE* instance,
                        struct sockaddr_storage* bind_addr,
                        uint32_t max_clients) {
    if (!server || !ua || !instance || !bind_addr) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Invalid parameters for control_server_init");
        return -1;
    }

    memset(server, 0, sizeof(*server));
    server->instance = instance;
    server->ua = ua;
    server->max_clients = max_clients ? max_clients : 8;
    memcpy(&server->bind_addr, bind_addr, sizeof(*bind_addr));
    
    /* Linux-профиль задаёт абсолютный db_path; отдельный лог не должен зависеть от рабочего каталога GUI. */
    const char* log_path = LOG_FILENAME;
#ifdef __linux__
    char profile_log[sizeof(instance->config->global.db_path) + sizeof(LOG_FILENAME) + 1];
    if (instance->config && instance->config->global.db_path[0]) {
        snprintf(profile_log, sizeof(profile_log), "%s/%s", instance->config->global.db_path, LOG_FILENAME);
        log_path = profile_log;
    }
#endif
    server->log_file = fopen(log_path, "w");
    if (server->log_file) {
        fprintf(server->log_file, "%llu: [LOG] Control server log started\n", 
               (unsigned long long)get_timestamp_ms());
        fflush(server->log_file);
        DEBUG_DEBUG(DEBUG_CATEGORY_CONTROL, "Control protocol log opened: %s", log_path);
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Cannot open control protocol log %s: %s", log_path, strerror(errno));
    }
    
    /* Create listening socket */
    int family = bind_addr->ss_family;
    server->listen_fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
    
#ifdef _WIN32
    if (server->listen_fd == INVALID_SOCKET) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to create listening socket: %d", WSAGetLastError());
        return -1;
    }
    
    /* Set non-blocking mode */
    u_long nonblock = 1;
    ioctlsocket(server->listen_fd, FIONBIO, &nonblock);
    
    /* Enable address reuse */
    int reuse = 1;
    if (setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, 
                   (const char*)&reuse, sizeof(reuse)) == SOCKET_ERROR) {
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Failed to set SO_REUSEADDR: %d", WSAGetLastError());
    }
#else
    if (server->listen_fd < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to create listening socket: %s", strerror(errno));
        return -1;
    }
    
    int reuse = 1;
    if (setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, 
                   &reuse, sizeof(reuse)) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Failed to set SO_REUSEADDR: %s", strerror(errno));
    }
    
    /* Set non-blocking */
    int flags = fcntl(server->listen_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(server->listen_fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif
    
    /* Bind to address */
    socklen_t addr_len = (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    
#ifdef _WIN32
    if (bind(server->listen_fd, (struct sockaddr*)bind_addr, addr_len) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to bind control socket: %d", err);
        closesocket(server->listen_fd);
        server->listen_fd = INVALID_SOCKET;
        return -1;
    }
    
    /* Listen */
    if (listen(server->listen_fd, 5) == SOCKET_ERROR) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to listen on control socket: %d", WSAGetLastError());
        closesocket(server->listen_fd);
        server->listen_fd = INVALID_SOCKET;
        return -1;
    }
#else
    if (bind(server->listen_fd, (struct sockaddr*)bind_addr, addr_len) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to bind control socket: %s", strerror(errno));
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }
    
    if (listen(server->listen_fd, 5) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to listen on control socket: %s", strerror(errno));
        close(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }
#endif
    
    /* Register with uasync */
    server->listen_socket_id = uasync_add_socket_t(ua, server->listen_fd,
                                                      accept_callback,
                                                      NULL,  /* write callback */
                                                      NULL,  /* except callback */
                                                      "control_listen",
                                                      server);
    
    if (!server->listen_socket_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to register control socket with uasync");
#ifdef _WIN32
        closesocket(server->listen_fd);
        server->listen_fd = INVALID_SOCKET;
#else
        close(server->listen_fd);
        server->listen_fd = -1;
#endif
        return -1;
    }
    
    if (family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)bind_addr;
        DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "Control server listening on %s:%d", 
                   ip_to_str(&sin->sin_addr, AF_INET).str, ntohs(sin->sin_port));
    } else {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)bind_addr;
        DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "Control server listening on [%s]:%d",
                   ip_to_str(&sin6->sin6_addr, AF_INET6).str, ntohs(sin6->sin6_port));
    }
    
    return 0;
}

/* ============================================================================
 * Server Shutdown
 * ============================================================================ */

void control_server_shutdown(struct control_server* server) {
    if (!server) return;
    
    /* Close all client connections */
    while (server->clients) {
        close_client(server, server->clients);
    }

#ifdef _WIN32
    if (server->listen_fd != INVALID_SOCKET) {
        uasync_remove_socket_t(server->ua, server->listen_fd);
        server->listen_socket_id = NULL;
        closesocket(server->listen_fd);
        server->listen_fd = INVALID_SOCKET;
    }
#else
    if (server->listen_fd >= 0) {
        uasync_remove_socket_t(server->ua, server->listen_fd);
        server->listen_socket_id = NULL;
        close(server->listen_fd);
        server->listen_fd = -1;
    }
#endif
    
    /* Close log file */
    if (server->log_file) {
        fprintf(server->log_file, "%llu: [LOG] Control server shutdown complete\n", 
               (unsigned long long)get_timestamp_ms());
        fclose(server->log_file);
        server->log_file = NULL;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "Control server shutdown complete");
}

/* ============================================================================
 * Client Connection Handling
 * ============================================================================ */

static int is_control_ip_allowed(const struct control_server* server, uint32_t client_ip) {
    if (!server || !server->instance || !server->instance->config) {
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Control IP check: no config available");
        return 0;
    }
    const struct global_config *g = &server->instance->config->global;
    if (g->control_allow_count == 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Control connection denied (no allow rules) - add control_allow=IP/mask to [control] in config");
        return 0;
    }
    for (int i = 0; i < g->control_allow_count; i++) {
        const struct CFG_CONTROL_ALLOW *r = &g->control_allows[i];
        if ((client_ip & r->netmask) == r->network) return 1;
    }
    DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Control connection denied from IP (not in allow list) - add control_allow=IP/mask to [control]");
    return 0;
}

static void accept_callback(socket_t fd, void* arg) {
    struct control_server* server = (struct control_server*)arg;
    
    struct sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
#ifdef _WIN32
    socket_t client_fd = accept(fd, (struct sockaddr*)&client_addr, &addr_len);
    
    if (client_fd == INVALID_SOCKET) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            if (server->log_file) {
                fprintf(server->log_file, "%llu: [ERROR] Accept failed: %d\n", 
                       (unsigned long long)get_timestamp_ms(), err);
                fflush(server->log_file);
            }
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Accept failed: %d", err);
        }
        return;
    }
    
    /* Set non-blocking */
    u_long nonblock = 1;
    ioctlsocket(client_fd, FIONBIO, &nonblock);
    
    /* Small delay to let socket stabilize (Windows-specific workaround) */
//    Sleep(10);
#else
    socket_t client_fd = accept(fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            if (server->log_file) {
                fprintf(server->log_file, "%llu: [ERROR] Accept failed: %s\n", 
                       (unsigned long long)get_timestamp_ms(), strerror(errno));
                fflush(server->log_file);
            }
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Accept failed: %s", strerror(errno));
        }
        return;
    }
    
    /* Set non-blocking */
    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif
    /* Check allowed IP (default deny all) */
    uint32_t client_ip = 0;
    if (client_addr.ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&client_addr;
        client_ip = ntohl(sin->sin_addr.s_addr);
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "IPv6 not supported for control allow list");
#ifdef _WIN32
        closesocket(client_fd);
#else
        close(client_fd);
#endif
        return;
    }
    if (!is_control_ip_allowed(server, client_ip)) {
#ifdef _WIN32
        closesocket(client_fd);
#else
        close(client_fd);
#endif
        return;
    }
    
    /* Check max clients */
    if (server->client_count >= server->max_clients) {
        if (server->log_file) {
            fprintf(server->log_file, "%llu: [ERROR] Max clients reached, rejecting connection\n", 
                   (unsigned long long)get_timestamp_ms());
            fflush(server->log_file);
        }
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Max clients reached, rejecting connection");
#ifdef _WIN32
        closesocket(client_fd);
#else
        close(client_fd);
#endif
        return;
    }
    
    /* Allocate client structure */
    struct control_client* client = (struct control_client*)u_calloc(1, sizeof(*client));
    if (!client) {
        if (server->log_file) {
            fprintf(server->log_file, "%llu: [ERROR] Failed to allocate client structure\n", 
                   (unsigned long long)get_timestamp_ms());
            fflush(server->log_file);
        }
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to allocate client structure");
#ifdef _WIN32
        closesocket(client_fd);
#else
        close(client_fd);
#endif
        return;
    }
    
    client->fd = client_fd;
    client->server = server;  /* Store back pointer to server */
    client->selected_peer_id = 0;

    client->output_queue = queue_new(server->ua, 0, 0, 0, "ctrl_out");
    if (!client->output_queue) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to create output queue for client");
        u_free(client);
#ifdef _WIN32
        closesocket(client_fd);
#else
        close(client_fd);
#endif
        return;
    }
    client->subscribed_nodes = 0;
    client->write_registered = 0;
    queue_set_size_limit(client->output_queue, 1);
    queue_set_threshold(client->output_queue, 0, 0);
    queue_set_waiter_defer(client->output_queue, 1);

    /* Register with uasync */
    client->socket_id = uasync_add_socket_t(server->ua, client_fd,
                                            client_read_callback,
                                            client_write_callback,
                                            client_except_callback,
                                            "control_client",
                                            client);
    
    if (!client->socket_id) {
        if (server->log_file) {
            fprintf(server->log_file, "%llu: [ERROR] Failed to register client socket with uasync\n", 
                   (unsigned long long)get_timestamp_ms());
            fflush(server->log_file);
        }
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to register client socket with uasync");
        queue_free(client->output_queue);
        u_free(client);
#ifdef _WIN32
        closesocket(client_fd);
#else
        close(client_fd);
#endif
        return;
    }
    
    /* Add to list */
    client->next = server->clients;
    server->clients = client;
    server->client_count++;

    client->idle_timer = uasync_set_timeout(server->ua, 300000, client, control_client_idle_cb, "ctrl_idle");
    if (!client->idle_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to arm control client idle timer fd=%d", (int)client_fd);
        close_client(server, client);
        return;
    }

    const char* addr_str;
    if (client_addr.ss_family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&client_addr;
        addr_str = ip_to_str(&sin->sin_addr, AF_INET).str;
    } else {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&client_addr;
        addr_str = ip_to_str(&sin6->sin6_addr, AF_INET6).str;
    }
    
    if (server->log_file) {
        fprintf(server->log_file, "%llu: [LOG] Client connected from %s (total: %u)\n", 
               (unsigned long long)get_timestamp_ms(), addr_str, server->client_count);
        fflush(server->log_file);
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "Control client connected from %s (total: %u)",
               addr_str, server->client_count);
}

static void client_read_callback(socket_t fd, void* arg) {
    struct control_client* client = (struct control_client*)arg;
    struct control_server* server = client->server;
    if (client->read_paused) return;

    if (client->recv_len >= ETCPMON_MAX_MSG_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Control recv buffer overflow (pre-check)");
        close_client(server, client);
        return;
    }

    /* Read available data */
    uint8_t* buf = client->recv_buffer + client->recv_len;
    size_t buf_space = ETCPMON_MAX_MSG_SIZE - client->recv_len;

#ifdef _WIN32
    int received = recv(fd, (char*)buf, (int)buf_space, 0);

    if (received == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            if (err == 10054) {
                /* Connection reset by peer — нормальное отключение */
                if (server) {
                    close_client(server, client);
                }
                return;
            }
            if (server && server->log_file) {
                fprintf(server->log_file, "%llu: [ERROR] Client recv error: %d\n",
                       (unsigned long long)get_timestamp_ms(), err);
                fflush(server->log_file);
            }
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Client recv error: %d", err);
            if (server) {
                close_client(server, client);
            }
            return;
        }
        return;
    }
    if (received == 0) {
        /* Connection closed gracefully */
        if (server && server->log_file) {
            fprintf(server->log_file, "%llu: [LOG] Client disconnected (recv returned 0)\n",
                   (unsigned long long)get_timestamp_ms());
            fflush(server->log_file);
        }
        if (server) {
            close_client(server, client);
        }
        return;
    }
#else
    ssize_t received = recv(fd, buf, buf_space, 0);
    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            if (server && server->log_file) {
                fprintf(server->log_file, "%llu: [ERROR] Client recv error: %s\n",
                       (unsigned long long)get_timestamp_ms(), strerror(errno));
                fflush(server->log_file);
            }
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Client recv error: %s", strerror(errno));
            if (server) {
                close_client(server, client);
            }
            return;
        }
        return;
    }
    if (received == 0) {
        /* Connection closed gracefully */
        if (server && server->log_file) {
            fprintf(server->log_file, "%llu: [LOG] Client disconnected (recv returned 0)\n",
                   (unsigned long long)get_timestamp_ms());
            fflush(server->log_file);
        }
        if (server) {
            close_client(server, client);
        }
        return;
    }
#endif

    /* Log received data */
    if (server && server->log_file) {
        log_hex_data(server->log_file, "RX", buf, received);
    }

    client->recv_len += received;
    if (client->recv_len > ETCPMON_MAX_MSG_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Control recv buffer overflow");
        if (server) close_client(server, client);
        return;
    }
    if (server) {
        handle_client_data(server, client);
    }
}

/* Один таймер: ожидание команд в покое, отсутствие положительного send во время TX. */
static int control_client_arm_timer(struct control_client* client, int tx) {
    if (client->idle_timer) uasync_cancel_timeout(client->server->ua, client->idle_timer);
    client->timer_is_tx = tx != 0;
    client->idle_timer = uasync_set_timeout(client->server->ua, 300000, client, control_client_idle_cb, tx ? "ctrl_tx" : "ctrl_idle");
    if (!client->idle_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Control timer failed fd=%d mode=%s", (int)client->fd, tx ? "TX" : "idle");
        client->failed = 1;
        return -1;
    }
    return 0;
}

/* Останавливает входных producers до пустой очереди. Waiter всегда отложен, повторных регистраций нет. */
static int control_wait_output(struct control_client* client) {
    if (!client->read_paused) {
        if (uasync_set_socket_read(client->server->ua, client->socket_id, 0) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to pause control reads fd=%d", (int)client->fd);
            client->failed = 1;
            return -1;
        }
        client->read_paused = 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_CONTROL, "Control reads paused fd=%d queued=%zu snapshot=%zu/%zu dirty=%zu",
                    (int)client->fd, queue_total_bytes(client->output_queue), client->snapshot_index,
                    client->snapshot_count, client->dirty_count);
    }
    if (!client->timer_is_tx && control_client_arm_timer(client, 1) < 0) return -1;
    if (!client->output_waiter.internal && !client->output_waiter.call_soon_id &&
        queue_waiter_wait(client->output_queue, &client->output_waiter, control_output_ready, client) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to wait for control output fd=%d", (int)client->fd);
        client->failed = 1;
        return -1;
    }
    return 0;
}

static void client_write_callback(socket_t fd, void* arg) {
    struct control_client* client = arg;
    struct control_server* server = client->server;
    struct ll_queue* queue = client->output_queue;
    struct ll_entry* entry = queue->head;
    if (client->failed) { close_client(server, client); return; }
    if (entry) {
#ifdef _WIN32
        int sent = send(fd, (const char*)entry->dgram + client->output_offset, entry->len - client->output_offset, 0);
        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) return;
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Control send failed fd=%d error=%d", (int)fd, error);
#else
        ssize_t sent = send(fd, entry->dgram + client->output_offset, entry->len - client->output_offset, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Control send failed fd=%d: %s", (int)fd, strerror(errno));
#endif
            close_client(server, client);
            return;
        }
        if (!sent) {
            DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Control send made no progress fd=%d", (int)fd);
            close_client(server, client);
            return;
        }
        client->output_offset += (uint16_t)sent;
        if (client->output_offset < entry->len) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CONTROL, "Control partial send fd=%d offset=%u/%u",
                        (int)fd, client->output_offset, entry->len);
            if (control_client_arm_timer(client, 1) < 0) close_client(server, client);
            return;
        }
        entry = queue_data_get(queue);
        queue_dgram_free(entry); queue_entry_free(entry);
        client->output_offset = 0;
    }
    if (uasync_set_socket_write(server->ua, client->socket_id, 0) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to disable control writes fd=%d", (int)fd);
        close_client(server, client);
        return;
    }
    client->write_registered = 0;
    if (control_client_arm_timer(client, client->snapshot_active || client->dirty_count) < 0) close_client(server, client);
}

static void client_except_callback(socket_t fd, void* arg) {
    struct control_client* client = (struct control_client*)arg;
    struct control_server* server = client ? client->server : NULL;

    if (server) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Client socket exception");
        close_client(server, client);   // <-- сразу удаляем из uasync
    }
    (void)fd;
}

/* Принимает владение буфером, включая ветки ошибки постановки в очередь. */
static int control_client_send(struct control_server* server, struct control_client* client, uint8_t* dgram, uint16_t len) {
    if (!client || !client->output_queue || !dgram || !len || len > ETCPMON_MAX_MSG_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Invalid control output client=%p size=%u", (void*)client, len);
        if (client) client->failed = 1;
        u_free(dgram);
        return -1;
    }
    if (client->output_queue->head || client->failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Control producer violated TX backpressure fd=%d queued=%zu incoming=%u",
                    (int)client->fd, queue_total_bytes(client->output_queue), len);
        client->failed = 1; u_free(dgram);
        return -1;
    }
    struct ll_entry* entry = queue_entry_new(0);
    if (!entry) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to allocate control output entry fd=%d size=%u", (int)client->fd, len);
        client->failed = 1; u_free(dgram);
        return -1;
    }
    entry->dgram = dgram; entry->len = len;
    if (queue_data_put(client->output_queue, entry) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to enqueue control output fd=%d size=%u", (int)client->fd, len);
        client->failed = 1;
        return -1;
    }
    if (!client->write_registered) {
        if (uasync_set_socket_write(server->ua, client->socket_id, 1) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to enable control writes fd=%d", (int)client->fd);
            client->failed = 1;
            return -1;
        }
        client->write_registered = 1;
    }
    return control_wait_output(client);
}

static void control_client_idle_cb(void* arg) {
    struct control_client* client = arg;
    if (!client || !client->server) return;
    client->idle_timer = NULL;
    DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Control %s timeout fd=%d queued_bytes=%zu snapshot=%zu/%zu dirty=%zu",
               client->timer_is_tx ? "TX progress" : "idle", (int)client->fd, queue_total_bytes(client->output_queue),
               client->snapshot_index, client->snapshot_count, client->dirty_count);
    close_client(client->server, client);
}

static void close_client(struct control_server* server, struct control_client* client) {
    if (!server || !client) return;

    DEBUG_DEBUG(DEBUG_CATEGORY_CONTROL, "Closing control client fd=%d queued_packets=%d queued_bytes=%zu output_offset=%u timer=%d",
                (int)client->fd, queue_entry_count(client->output_queue), queue_total_bytes(client->output_queue),
                client->output_offset, client->idle_timer != NULL);
    
    if (client->idle_timer) { uasync_cancel_timeout(server->ua, client->idle_timer); client->idle_timer = NULL; }
    if (client->output_queue) queue_waiter_cancel(client->output_queue, &client->output_waiter);
    u_free(client->snapshot_ids); client->snapshot_ids = NULL;

    /* Log disconnection */
    if (server->log_file) {
        fprintf(server->log_file, "%llu: [LOG] Client connection closed\n", 
               (unsigned long long)get_timestamp_ms());
        fflush(server->log_file);
    }

    /* Close socket */
#ifdef _WIN32
    if (client->fd != INVALID_SOCKET) {
        uasync_remove_socket_t(server->ua, client->fd);
        closesocket(client->fd);
    }
#else
    if (client->fd >= 0) {
        uasync_remove_socket_t(server->ua, client->fd);
        close(client->fd);
    }
#endif

    if (client->output_queue) {
        struct ll_entry* entry;
        while ((entry = queue_data_get(client->output_queue))) {
            queue_dgram_free(entry);
            queue_entry_free(entry);
        }
        queue_free(client->output_queue);
        client->output_queue = NULL;
    }

    /* Remove from list */
    struct control_client** curr = &server->clients;
    while (*curr) {
        if (*curr == client) {
            *curr = client->next;
            break;
        }
        curr = &(*curr)->next;
    }
    
    server->client_count--;
    u_free(client);
    
    DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "Control client disconnected (total: %u)",
               server->client_count);
}

/* ============================================================================
 * Message Handling
 * ============================================================================ */

static int handle_client_data(struct control_server* server, struct control_client* client) {
    if (client->failed) { close_client(server, client); return -1; }
    if (client->output_queue->head || client->snapshot_active || client->dirty_count) {
        if (control_wait_output(client) < 0) { close_client(server, client); return -1; }
        return 0;
    }
    while (client->recv_len >= sizeof(struct etcpmon_msg_header)) {
        struct etcpmon_msg_header* hdr = (struct etcpmon_msg_header*)client->recv_buffer;
        
        if (hdr->size == 0 || hdr->size > ETCPMON_MAX_MSG_SIZE) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Invalid message size from client: %u", hdr->size);
            close_client(server, client);
            return -1;
        }
        if (hdr->type < ETCPMON_CMD_LIST_CONN || hdr->type > ETCPMON_CMD_SUBSCRIBE_NODES) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Invalid command type from client: 0x%02X", hdr->type);
            close_client(server, client);
            return -1;
        }
        
        /* Validate header */
        if (etcpmon_validate_header(hdr) != 0) {
            if (server->log_file) {
                fprintf(server->log_file, "%llu: [ERROR] Invalid message header from client\n",
                       (unsigned long long)get_timestamp_ms());
                fflush(server->log_file);
            }
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Invalid message header from client");
            close_client(server, client);
            return -1;
        }
        
        /* Check if full message received */
        if (client->recv_len < hdr->size) {
            break;  /* Wait for more data */
        }
        
        /* Process message */
        uint8_t* payload = client->recv_buffer + sizeof(struct etcpmon_msg_header);
        uint16_t payload_size = hdr->size - sizeof(struct etcpmon_msg_header);
        
        uint8_t req_seq = hdr->seq_id;
        if ((hdr->type == ETCPMON_CMD_SELECT_CONN && payload_size != sizeof(struct etcpmon_cmd_select)) ||
            (hdr->type != ETCPMON_CMD_SELECT_CONN && hdr->type != ETCPMON_CMD_ACTION &&
             hdr->type != ETCPMON_CMD_SET_DEBUG_CONFIG && payload_size)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Invalid command payload fd=%d type=0x%02X size=%u",
                        (int)client->fd, hdr->type, payload_size);
            close_client(server, client);
            return -1;
        }
        
        if (server->log_file) {
            fprintf(server->log_file, "%llu: [LOG] Received command type=0x%02X seq=%d size=%d\n",
                   (unsigned long long)get_timestamp_ms(), hdr->type, req_seq, hdr->size);
            fflush(server->log_file);
        }
        
        switch (hdr->type) {
            case ETCPMON_CMD_LIST_CONN:
                send_conn_list(server, client, req_seq);
                break;

            case ETCPMON_CMD_SELECT_CONN:
                if (payload_size >= sizeof(struct etcpmon_cmd_select)) {
                    struct etcpmon_cmd_select* cmd = (struct etcpmon_cmd_select*)payload;
                    client->selected_peer_id = cmd->peer_node_id;
                    if (server->log_file) {
                        fprintf(server->log_file, "%llu: [LOG] Client selected connection: %016llX\n",
                               (unsigned long long)get_timestamp_ms(), (unsigned long long)cmd->peer_node_id);
                        fflush(server->log_file);
                    }
                    DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "Client selected connection: %016llX",
                               (unsigned long long)cmd->peer_node_id);
                } else {
                    DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Bad SELECT_CONN payload size %u", payload_size);
                    close_client(server, client);
                    return -1;
                }
                break;

            case ETCPMON_CMD_GET_METRICS:
                if (client->selected_peer_id == 0) {
                    send_error(client, ETCPMON_ERR_NO_CONN_SELECTED,
                              "No connection selected", req_seq);
                } else {
                    send_metrics(server, client, req_seq);
                }
                break;

            case ETCPMON_CMD_LIST_SOCKETS:
                send_socket_list(server, client, req_seq);
                break;

            case ETCPMON_CMD_ACTION:
                if (handle_action(server, client, payload, payload_size, req_seq) < 0) {
                    close_client(server, client);
                    return -1;
                }
                break;

            case ETCPMON_CMD_GET_DEBUG_CONFIG:
                send_debug_config(server, client, req_seq);
                break;

            case ETCPMON_CMD_SET_DEBUG_CONFIG: {
                if (payload_size < 2 || payload[1] > ETCPMON_MAX_DEBUG_CATEGORIES || payload_size != 2u + payload[1]) {
                    DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Bad SET_DEBUG_CONFIG payload size=%u categories=%d",
                                payload_size, payload_size >= 2 ? payload[1] : -1);
                    close_client(server, client);
                    return -1;
                }
                uint8_t global_lvl = payload[0];
                uint8_t cat_count = payload[1];
                if (global_lvl <= DEBUG_LEVEL_TRACE) debug_set_level((debug_level_t)global_lvl);
                const uint8_t* levels = payload + 2;
                for (uint8_t i = 0; i < cat_count && i + 1 < DEBUG_CATEGORY_COUNT; i++) {
                    if (levels[i] <= DEBUG_LEVEL_TRACE)
                        debug_set_category_level((debug_category_t)(i + 1), (debug_level_t)levels[i]);
                }
                DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "Debug config applied: global=%d categories=%d", global_lvl, cat_count);
                if (server->log_file) {
                    fprintf(server->log_file, "%llu: [LOG] Debug config set: global=%d categories=%d\n",
                           (unsigned long long)get_timestamp_ms(), global_lvl, cat_count);
                    fflush(server->log_file);
                }
                break;
            }

            case ETCPMON_CMD_SUBSCRIBE_NODES:
                control_server_send_node_list(server, client, req_seq);
                break;

            case ETCPMON_CMD_DISCONNECT:
                if (server->log_file) {
                    fprintf(server->log_file, "%llu: [LOG] Client requested disconnect\n",
                           (unsigned long long)get_timestamp_ms());
                    fflush(server->log_file);
                }
                close_client(server, client);
                return -1;

            default:
                DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Unknown command from client: 0x%02X", hdr->type);
                close_client(server, client);
                return -1;
        }
        
        if (client->failed) { close_client(server, client); return -1; }

        /* Remove processed message from buffer */
        uint16_t msg_size = hdr->size;
        if (client->recv_len > msg_size) {
            memmove(client->recv_buffer, client->recv_buffer + msg_size,
                    client->recv_len - msg_size);
        }
        client->recv_len -= msg_size;
        if (client->output_queue->head || client->snapshot_active || client->dirty_count) {
            if (control_wait_output(client) < 0) { close_client(server, client); return -1; }
            return 0;
        }
        if (control_client_arm_timer(client, 0) < 0) { close_client(server, client); return -1; }
    }
    return 0;
}

/* ============================================================================
 * Response Builders
 * ============================================================================ */

/* Любая корректная Action завершается ответом с исходным seq; nat подтверждает запуск, а не результат проверки. */
static int handle_action(struct control_server* server, struct control_client* client,
                         const uint8_t* payload, uint16_t size, uint8_t seq_id) {
    if (size != sizeof(struct etcpmon_cmd_action) || !memchr(payload, 0, size)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Invalid Action payload fd=%d seq=%u size=%u", (int)client->fd, seq_id, size);
        return -1;
    }
    const char* action = (const char*)payload;
    int nat = !strcmp(action, "nat");
    if (!nat && strcmp(action, "nodes")) {
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Unknown Action fd=%d seq=%u action='%s'", (int)client->fd, seq_id, action);
        send_error(client, ETCPMON_ERR_INVALID_CMD, "Unknown action", seq_id);
        return 0;
    }
    struct TOPO_GROUP* group = topo_groups_get_default(server->instance->topo_groups);
    if ((nat && !server->instance->nat_det && !server->instance->stun_nat) || (!nat && !group)) {
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Action unavailable fd=%d seq=%u action=%s", (int)client->fd, seq_id, action);
        send_error(client, ETCPMON_ERR_SERVER_BUSY, nat ? "NAT/STUN service unavailable" : "BGP not available", seq_id);
        return 0;
    }
    size_t capacity = nat ? ETCPMON_ACTION_RESULT_SIZE(strlen("NAT check requested")) : ETCPMON_MAX_MSG_SIZE;
    uint8_t* buffer = u_malloc(capacity);
    if (!buffer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Action response allocation failed fd=%d seq=%u size=%zu", (int)client->fd, seq_id, capacity);
        return -1;
    }
    struct etcpmon_msg_header* header = (void*)buffer;
    struct etcpmon_rsp_action_result* result = (void*)(buffer + sizeof(*header));
    char* text = (char*)(result + 1);
    int text_len;
    if (nat) {
        if (server->instance->nat_det) nat_detection_request_check_all(server->instance->nat_det);
        stun_nat_recheck(server->instance);
        strcpy(text, "NAT check requested");
        text_len = (int)strlen(text);
    } else {
        text_len = topo_node_format_all(group, text, capacity - sizeof(*header) - sizeof(*result));
        if (text_len < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Action nodes formatting failed fd=%d seq=%u", (int)client->fd, seq_id);
            u_free(buffer);
            send_error(client, ETCPMON_ERR_SERVER_BUSY, "Node dump failed", seq_id);
            return 0;
        }
    }
    result->code = 0;
    etcpmon_build_header(header, sizeof(*result) + text_len + 1, ETCPMON_RSP_ACTION_RESULT, seq_id);
    DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "Action completed fd=%d seq=%u action=%s response_bytes=%u",
               (int)client->fd, seq_id, action, header->size);
    return control_client_send(server, client, buffer, header->size);
}

static void send_conn_list(struct control_server* server, struct control_client* client, uint8_t seq_id) {
    struct UTUN_INSTANCE* instance = server->instance;
    
    /* Count connections */
    uint8_t count = 0;
    struct ll_entry* entry = instance->connections->head;
    while (entry && count < ETCPMON_MAX_CONNECTIONS) {
        count++;
        entry = entry->next;
    }
    
    /* Build response */
    uint16_t rsp_size = ETCPMON_CONN_LIST_SIZE(count);
    uint8_t* buffer = (uint8_t*)u_malloc(rsp_size);
    if (!buffer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to allocate connection list buffer");
        client->failed = 1;
        return;
    }
    
    struct etcpmon_msg_header* hdr = (struct etcpmon_msg_header*)buffer;
    etcpmon_build_header(hdr, sizeof(struct etcpmon_rsp_conn_list) + 
                              count * sizeof(struct etcpmon_conn_info),
                         ETCPMON_RSP_CONN_LIST,
                         seq_id);
    
    struct etcpmon_rsp_conn_list* rsp = (struct etcpmon_rsp_conn_list*)(buffer + sizeof(*hdr));
    rsp->count = count;
    
    struct etcpmon_conn_info* info = (struct etcpmon_conn_info*)(buffer + sizeof(*hdr) + sizeof(*rsp));
    entry = instance->connections->head;
    for (uint8_t i = 0; i < count && entry; i++) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        info[i].peer_node_id = ce->conn->peer_node_id;
        strncpy(info[i].name, ce->conn->log_name, ETCPMON_MAX_CONN_NAME - 1);
        info[i].name[ETCPMON_MAX_CONN_NAME - 1] = '\0';
        entry = entry->next;
    }
    
    /* Log and send response */
    if (server->log_file) {
        log_hex_data(server->log_file, "TX", buffer, rsp_size);
        fprintf(server->log_file, "%llu: [LOG] Sent RSP_CONN_LIST count=%d\n",
               (unsigned long long)get_timestamp_ms(), count);
        fflush(server->log_file);
    }
    
    control_client_send(server, client, buffer, rsp_size);
}

static void send_socket_list(struct control_server* server, struct control_client* client, uint8_t seq_id) {
    struct UTUN_INSTANCE* instance = server->instance;

    if (!instance || !instance->etcp_sockets) {
        uint16_t rsp_size = ETCPMON_SOCKET_LIST_SIZE(0);
        uint8_t* buffer = (uint8_t*)u_malloc(rsp_size);
        if (!buffer) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to allocate empty socket list fd=%d", (int)client->fd);
            client->failed = 1;
            return;
        }

        struct etcpmon_msg_header* hdr = (struct etcpmon_msg_header*)buffer;
        etcpmon_build_header(hdr, sizeof(struct etcpmon_rsp_socket_list),
                             ETCPMON_RSP_SOCKET_LIST, seq_id);

        struct etcpmon_rsp_socket_list* rsp = (struct etcpmon_rsp_socket_list*)(buffer + sizeof(*hdr));
        rsp->count = 0;

        control_client_send(server, client, buffer, rsp_size);
        return;
    }

    uint8_t count = 0;
    struct ETCP_SOCKET* sock = instance->etcp_sockets;
    while (sock && count < ETCPMON_MAX_CONNECTIONS) {
        if (!sock->probe_only) {
            if (ETCPMON_SOCKET_LIST_SIZE(count + 1u) > ETCPMON_MAX_MSG_SIZE) {
                DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Socket list exceeds protocol limit: count>%u next_size=%zu max_size=%u",
                           count, ETCPMON_SOCKET_LIST_SIZE(count + 1u), ETCPMON_MAX_MSG_SIZE);
                send_error(client, ETCPMON_ERR_SERVER_BUSY, "Socket list exceeds protocol limit", seq_id);
                return;
            }
            count++;
        }
        sock = sock->next;
    }

    uint16_t rsp_size = ETCPMON_SOCKET_LIST_SIZE(count);
    uint8_t* buffer = (uint8_t*)u_malloc(rsp_size);
    if (!buffer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to allocate socket list buffer");
        client->failed = 1;
        return;
    }

    struct etcpmon_msg_header* hdr = (struct etcpmon_msg_header*)buffer;
    etcpmon_build_header(hdr, sizeof(struct etcpmon_rsp_socket_list) +
                               count * sizeof(struct etcpmon_socket_info),
                         ETCPMON_RSP_SOCKET_LIST,
                         seq_id);

    struct etcpmon_rsp_socket_list* rsp = (struct etcpmon_rsp_socket_list*)(buffer + sizeof(*hdr));
    rsp->count = count;

    struct etcpmon_socket_info* info = (struct etcpmon_socket_info*)(buffer + sizeof(*hdr) + sizeof(*rsp));
    sock = instance->etcp_sockets;
    for (uint8_t i = 0; i < count && sock; i++) {
        while (sock && sock->probe_only) sock = sock->next;
        if (!sock) break;
        strncpy(info[i].name, sock->name, ETCPMON_MAX_CONN_NAME - 1);
        info[i].name[ETCPMON_MAX_CONN_NAME - 1] = '\0';
        info[i].is_ipv6 = (sock->interface_addr.ss_family == AF_INET6) ? 1 : 0;
        if (sock->interface_addr.ss_family == AF_INET) {
            struct sockaddr_in* sin = (struct sockaddr_in*)&sock->interface_addr;
            inet_ntop(AF_INET, &sin->sin_addr, info[i].ip, sizeof(info[i].ip));
            info[i].port = ntohs(sin->sin_port);
        } else if (sock->interface_addr.ss_family == AF_INET6) {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&sock->interface_addr;
            inet_ntop(AF_INET6, &sin6->sin6_addr, info[i].ip, sizeof(info[i].ip));
            info[i].port = ntohs(sin6->sin6_port);
        } else {
            strncpy(info[i].ip, "unknown", sizeof(info[i].ip) - 1);
            info[i].ip[sizeof(info[i].ip) - 1] = '\0';
            info[i].port = 0;
        }
        info[i].config_type = sock->type;
        info[i].mapping = sock->address_profile.mapping; info[i].inbound = sock->address_profile.inbound;
        info[i].source = sock->address_profile.source;
        info[i].is_tcp = sock->is_tcp; info[i].generation = sock->generation;
        
        /* Общий внешний адрес подтверждён измерениями владельца. */
        info[i].nat_ip[0] = '\0';
        info[i].nat_port = 0;
        if (sock->nat_addr.ss_family == AF_INET) {
            struct sockaddr_in* sin = (struct sockaddr_in*)&sock->nat_addr;
            inet_ntop(AF_INET, &sin->sin_addr, info[i].nat_ip, sizeof(info[i].nat_ip));
            info[i].nat_port = ntohs(sin->sin_port);
        } else if (sock->nat_addr.ss_family == AF_INET6) {
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&sock->nat_addr;
            inet_ntop(AF_INET6, &sin6->sin6_addr, info[i].nat_ip, sizeof(info[i].nat_ip));
            info[i].nat_port = ntohs(sin6->sin6_port);
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "socket_list to gui: sock=%s config=%u mapping=%s inbound=%s external=%s:%u",
                   sock->name, sock->type, address_mapping_name(sock->address_profile.mapping),
                   address_inbound_name(sock->address_profile.inbound),
                   info[i].nat_ip[0] ? info[i].nat_ip : "none", info[i].nat_port);

        sock = sock->next;
    }

    if (server->log_file) {
        log_hex_data(server->log_file, "TX", buffer, rsp_size);
        fprintf(server->log_file, "%llu: [LOG] Sent RSP_SOCKET_LIST count=%d\n",
                (unsigned long long)get_timestamp_ms(), count);
        fflush(server->log_file);
    }

    control_client_send(server, client, buffer, rsp_size);
}

static void send_metrics(struct control_server* server, struct control_client* client, uint8_t seq_id) {
    struct UTUN_INSTANCE* instance = server->instance;
    
    /* Find selected connection */
    struct ETCP_CONN* conn = instance_find_conn(instance, client->selected_peer_id);
    if (!conn) {
        send_error(client, ETCPMON_ERR_INVALID_CONN, "Connection not found", seq_id);
        return;
    }
    
    /* Count links */
    uint8_t links_count = 0;
    struct ETCP_LINK* link = conn->links;
    while (link && links_count < ETCPMON_MAX_LINKS) {
        links_count++;
        link = link->next;
    }
    
    /* Build response */
    uint16_t rsp_size = ETCPMON_METRICS_SIZE(links_count);
    uint8_t* buffer = (uint8_t*)u_malloc(rsp_size);
    if (!buffer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to allocate metrics buffer");
        client->failed = 1;
        return;
    }
    
    struct etcpmon_msg_header* hdr = (struct etcpmon_msg_header*)buffer;
    etcpmon_build_header(hdr, sizeof(struct etcpmon_rsp_metrics) + 
                              links_count * sizeof(struct etcpmon_link_metrics),
                         ETCPMON_RSP_METRICS,
                         seq_id);
    
    struct etcpmon_rsp_metrics* rsp = (struct etcpmon_rsp_metrics*)(buffer + sizeof(*hdr));
    
    /* Fill ETCP metrics */
    rsp->etcp.peer_node_id = conn->peer_node_id;
    rsp->etcp.rtt_last = conn->rtt_last;
    rsp->etcp.rtt_avg_10 = conn->rtt_avg_10;
    rsp->etcp.jitter = conn->jitter;
    rsp->etcp.bytes_sent_total = conn->bytes_sent_total;
    rsp->etcp.retrans_count = conn->retransmissions_count;
    rsp->etcp.ack_count = conn->ack_packets_count;
    rsp->etcp.unacked_bytes = conn->unacked_bytes;
    rsp->etcp.optimal_inflight = conn->optimal_inflight;
    rsp->etcp.links_count = links_count;

    /* Queue metrics */
    rsp->etcp.input_queue_bytes = (uint32_t)queue_total_bytes(conn->input_queue);
    rsp->etcp.input_queue_packets = (uint32_t)queue_entry_count(conn->input_queue);
    rsp->etcp.input_send_q_bytes = (uint32_t)queue_total_bytes(conn->input_send_q);
    rsp->etcp.input_send_q_packets = (uint32_t)queue_entry_count(conn->input_send_q);
    rsp->etcp.input_wait_ack_bytes = (uint32_t)queue_total_bytes(conn->input_wait_ack);
    rsp->etcp.input_wait_ack_packets = (uint32_t)queue_entry_count(conn->input_wait_ack);
    rsp->etcp.ack_q_bytes = rsp->etcp.ack_q_packets = 0;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
        rsp->etcp.ack_q_bytes += (uint32_t)queue_total_bytes(link->ack_q);
        rsp->etcp.ack_q_packets += (uint32_t)queue_entry_count(link->ack_q);
    }
    rsp->etcp.recv_q_bytes = (uint32_t)queue_total_bytes(conn->recv_q);
    rsp->etcp.recv_q_packets = (uint32_t)queue_entry_count(conn->recv_q);
    rsp->etcp.output_queue_bytes = (uint32_t)queue_total_bytes(conn->output_queue);
    rsp->etcp.output_queue_packets = (uint32_t)queue_entry_count(conn->output_queue);

    /* Error counters */
    rsp->etcp.reinit_count = conn->reinit_count;
    rsp->etcp.reset_count = conn->reset_count;
    rsp->etcp.pkt_format_errors = (conn->links && conn->links->conn) ? (uint32_t)conn->links->conn->pkt_format_errors : 0;

    /* Timer flags */
    rsp->etcp.retrans_timer_active = 0;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next)
        if (link->retrans_timer) rsp->etcp.retrans_timer_active = 1;
    rsp->etcp.ack_resp_timer_active = (conn->ack_resp_timer != NULL) ? 1 : 0;
    
    /* Connection IDs */
    rsp->etcp.next_tx_id = conn->next_tx_id;
    rsp->etcp.last_rx_id = conn->last_rx_id;
    rsp->etcp.last_delivered_id = conn->last_delivered_id;
    rsp->etcp.rx_ack_till = conn->rx_ack_till;
    
    /* input_wait_ack queue state */
    rsp->etcp.wait_ack_cb_suspended = (uint8_t)conn->input_wait_ack->callback_suspended;
    rsp->etcp.wait_ack_cb_set = (conn->input_wait_ack->callback != NULL) ? 1 : 0;
    rsp->etcp.wait_ack_resume_timeout = (conn->input_wait_ack->resume_work.ua != NULL) ? 1 : 0;
    
    /* Normalizer */
    if (conn->normalizer) {
        struct PKTNORM* norm = (struct PKTNORM*)conn->normalizer;
        rsp->etcp.norm_input_pkts = (uint32_t)queue_entry_count(norm->input);
        rsp->etcp.norm_input_bytes = (uint32_t)queue_total_bytes(norm->input);
        rsp->etcp.norm_output_pkts = (uint32_t)queue_entry_count(norm->output);
        rsp->etcp.norm_output_bytes = (uint32_t)queue_total_bytes(norm->output);
        rsp->etcp.norm_alloc_errors = norm->alloc_errors;
        rsp->etcp.norm_logic_errors = norm->logic_errors;
        rsp->etcp.norm_frag_size = norm->frag_size;
        rsp->etcp.norm_data_ptr = norm->data_ptr;
        rsp->etcp.norm_data_size = norm->data_size;
        rsp->etcp.norm_in_total_pkts = norm->in_total_pkts;
        rsp->etcp.norm_in_total_bytes = norm->in_total_bytes;
        rsp->etcp.norm_out_total_pkts = norm->out_total_pkts;
        rsp->etcp.norm_out_total_bytes = norm->out_total_bytes;
    } else {
        rsp->etcp.norm_input_pkts = 0;
        rsp->etcp.norm_input_bytes = 0;
        rsp->etcp.norm_output_pkts = 0;
        rsp->etcp.norm_output_bytes = 0;
        rsp->etcp.norm_alloc_errors = 0;
        rsp->etcp.norm_logic_errors = 0;
        rsp->etcp.norm_frag_size = 0;
        rsp->etcp.norm_data_ptr = 0;
        rsp->etcp.norm_data_size = 0;
        rsp->etcp.norm_in_total_pkts = 0;
        rsp->etcp.norm_in_total_bytes = 0;
        rsp->etcp.norm_out_total_pkts = 0;
        rsp->etcp.norm_out_total_bytes = 0;
    }

    /* ACK debug counters */
    rsp->etcp.cnt_ack_hit_inf = conn->cnt_ack_hit_inf;
    rsp->etcp.cnt_ack_hit_sndq = conn->cnt_ack_hit_sndq;
    rsp->etcp.cnt_ack_miss = conn->cnt_ack_miss;
    rsp->etcp.cnt_link_wait = conn->cnt_link_wait;
    rsp->etcp.rx_dup_count = conn->rx_dup_count;
    rsp->etcp.tx_dup_count = conn->tx_dup_count;
    rsp->etcp.tx_state = conn->tx_state;
    for (int i = 0; i < 8; i++) {
        rsp->etcp.debug[i] = conn->debug[i];
    }

    /* System resources */
    if (instance->ua && instance->ua->timeout_heap) {
        rsp->etcp.active_timeouts = (uint32_t)timeout_heap_get_size(instance->ua->timeout_heap);
    } else {
        rsp->etcp.active_timeouts = 0;
    }

    rsp->etcp.busy_memory_blocks = (uint32_t)(u_get_allocated_count() - memory_pool_get_total_free_blocks());
    
    /* TUN отсутствует в режиме прокси; маршруты и BGP работают независимо. */
    memset(&rsp->tun, 0, sizeof(rsp->tun));
    if (instance->tun) {
        rsp->tun.bytes_read = instance->tun->bytes_read;
        rsp->tun.bytes_written = instance->tun->bytes_written;
        rsp->tun.packets_read = instance->tun->packets_read;
        rsp->tun.packets_written = instance->tun->packets_written;
        rsp->tun.read_errors = instance->tun->read_errors;
        rsp->tun.write_errors = instance->tun->write_errors;
        
        /* TUN queues */
        rsp->tun.tun_in_q_packets = (uint32_t)queue_entry_count(instance->tun->input_queue);
        rsp->tun.tun_in_q_bytes = (uint32_t)queue_total_bytes(instance->tun->input_queue);
        rsp->tun.tun_out_q_packets = (uint32_t)queue_entry_count(instance->tun->output_queue);
        rsp->tun.tun_out_q_bytes = (uint32_t)queue_total_bytes(instance->tun->output_queue);
    }

    /* Статистика маршрутизации не зависит от наличия TUN. */
    rsp->tun.routed_packets = instance->routed_packets;
    rsp->tun.dropped_packets = instance->dropped_packets;
    if (instance->rt) {
        rsp->tun.rt_count = (uint32_t)instance->rt->count;
        rsp->tun.rt_local = (uint32_t)instance->rt->stats.local_routes;
        rsp->tun.rt_learned = (uint32_t)instance->rt->stats.learned_routes;
    }
    struct TOPO_GROUP* group = topo_groups_get_default(instance->topo_groups);
    if (group) {
        rsp->tun.rt_bgp_senders = (uint32_t)queue_entry_count(group->senders_list);
        rsp->tun.rt_bgp_nodes = (uint32_t)queue_entry_count(group->nodes);
    }
    
    /* Fill router congestion metrics — aggregate over all router_conns */
    memset(&rsp->router, 0, sizeof(rsp->router));
    if (instance->router_conns) {
        uint32_t rtt_cnt = 0;
        struct ll_entry* re = instance->router_conns->head;
        while (re) {
            struct ETCP_ROUTER_CONN* rc = (struct ETCP_ROUTER_CONN*)re;
            rsp->router.total_inflight += (uint32_t)queue_entry_count(rc->inflight_q);
            if (rc->send_q) rsp->router.total_send_q += (uint32_t)queue_entry_count(rc->send_q);
            if (rc->recv_q) rsp->router.total_recv_q += (uint32_t)queue_entry_count(rc->recv_q);
            rsp->router.pkts_sent     += rc->c_pkts_sent;
            rsp->router.pkts_send_err += rc->c_pkts_send_err;
            rsp->router.pkts_rcvd     += rc->c_pkts_rcvd;
            rsp->router.ack_sent      += rc->c_ack_sent;
            rsp->router.ack_recv      += rc->c_ack_recv;
            rsp->router.dup_dropped   += rc->c_dup_dropped;
            rsp->router.oob_dropped   += rc->c_oob_dropped;
            rsp->router.stale_ack     += rc->c_stale_ack;
            rsp->router.sign_fail     += rc->c_sign_fail;
            if (rc->rtt > 0) {
                if (rc->rtt > rsp->router.rtt_last) rsp->router.rtt_last = rc->rtt;
                rsp->router.rtt_avg += rc->rtt;
                rtt_cnt++;
            }
            {
                uint32_t j = rc->rtt_jitter / 32768;
                if (j > rsp->router.jitter) rsp->router.jitter = j;
            }
            if (rc->minrtt_window_count > 0) {
                if (rsp->router.minrtt == 0 || rc->minrtt < rsp->router.minrtt)
                    rsp->router.minrtt = rc->minrtt;
            }
            re = re->next;
        }
        if (rtt_cnt > 0) rsp->router.rtt_avg /= rtt_cnt;
        rsp->router.total_conns = (uint32_t)queue_entry_count(instance->router_conns);
    }
    
    /* Fill link metrics */
    struct etcpmon_link_metrics* link_info = (struct etcpmon_link_metrics*)(buffer + sizeof(*hdr) + sizeof(*rsp));
    link = conn->links;
    for (uint8_t i = 0; i < links_count && link; i++) {
        memset(&link_info[i], 0, sizeof(link_info[i]));
        link_info[i].local_link_id = link->local_link_id;
        link_info[i].status = link->link_status;
        link_info[i].encrypt_errors = (uint32_t)link->encrypt_errors;
        link_info[i].decrypt_errors = (uint32_t)link->decrypt_errors;
        link_info[i].send_errors = (uint32_t)link->send_errors;
        link_info[i].recv_errors = (uint32_t)link->recv_errors;
        link_info[i].total_encrypted = link->total_encrypted;
        link_info[i].total_decrypted = link->total_decrypted;
        link_info[i].bandwidth = link->bandwidth;
        link_info[i].nat_changes_count = link->nat_changes_count;
        link_info[i].rtt_last = link->rtt_last;
        link_info[i].rtt_avg10 = link->bbr ? link->bbr->min_rtt_us / 100 : 0;
        link_info[i].tt_last = link->tt_last;
        link_info[i].init_timer_active = (link->init_timer != NULL) ? 1 : 0;
        link_info[i].keepalive_timer_active = (link->keepalive_timer != NULL) ? 1 : 0;
        link_info[i].shaper_timer_active = (link->shaper_timer != NULL) ? 1 : 0;
        link_info[i].keepalive_sent = (uint32_t)link->keepalive_sent_count;
        link_info[i].keepalive_recv = (uint32_t)link->keepalive_recv_count;
        link_info[i].inflight_bytes = link->inflight_bytes;
        link_info[i].inflight_packets = link->inflight_packets;
        link_info[i].inflight_lim_bytes = link->inflight_lim_bytes;

        /* BBR fields */
        if (link->bbr) {
            link_info[i].bbr_mode            = link->bbr->mode;
            link_info[i].bbr_cycle_idx       = link->bbr->cycle_idx;
            link_info[i].bbr_full_bw_reached = link->bbr->full_bw_reached;
            link_info[i].bbr_loss_in_round   = link->bbr->loss_in_round;
            link_info[i].bbr_pacing_rate     = link->bbr_pacing_rate;
            link_info[i].bbr_min_rtt_us      = link->bbr->min_rtt_us;
            link_info[i].bbr_pacing_gain     = link->bbr->pacing_gain;
            link_info[i].bbr_inflight_hi     = link->bbr->inflight_hi;
            link_info[i].bbr_inflight_lo     = link->bbr->inflight_lo;
           /* bw_hi/lo: convert from BW_UNIT scale (pkts/usec<<24) to bytes/sec */
            uint32_t max_bw = link->bbr->bw_hi[0] > link->bbr->bw_hi[1] ? link->bbr->bw_hi[0] : link->bbr->bw_hi[1];
            link_info[i].bbr_bw_hi = max_bw ? (uint32_t)((uint64_t)max_bw * link->mtu * 1000000ULL / 16777216ULL) : 0;
            link_info[i].bbr_bw_lo = link->bbr->bw_lo != ~0U ? (uint32_t)((uint64_t)link->bbr->bw_lo * link->mtu * 1000000ULL / 16777216ULL) : 0;
        }
        link = link->next;
    }
    
    /* Log and send response */
    if (server->log_file) {
        log_hex_data(server->log_file, "TX", buffer, rsp_size);
        fprintf(server->log_file, "%llu: [LOG] Sent RSP_METRICS rtt=%u bytes=%llu links=%d tun=%d "
                "routes=%u local=%u learned=%u bgp_senders=%u bgp_nodes=%u\n",
               (unsigned long long)get_timestamp_ms(),
               rsp->etcp.rtt_last,
               (unsigned long long)rsp->etcp.bytes_sent_total,
               links_count, instance->tun != NULL, rsp->tun.rt_count, rsp->tun.rt_local, rsp->tun.rt_learned,
               rsp->tun.rt_bgp_senders, rsp->tun.rt_bgp_nodes);
        fflush(server->log_file);
    }
    
    control_client_send(server, client, buffer, rsp_size);
}

static void send_error(struct control_client* client, uint8_t error_code, const char* msg, uint8_t seq_id) {
    struct control_server* server = client->server;
    uint16_t msg_len = (uint16_t)strlen(msg);
    uint16_t rsp_size = ETCPMON_ERROR_SIZE(msg_len);

    uint8_t* buffer = (uint8_t*)u_malloc(rsp_size);
    if (!buffer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to allocate control error fd=%d seq=%u", (int)client->fd, seq_id);
        client->failed = 1;
        return;
    }

    struct etcpmon_msg_header* hdr = (struct etcpmon_msg_header*)buffer;
    etcpmon_build_header(hdr, sizeof(struct etcpmon_rsp_error) + msg_len + 1,
                         ETCPMON_RSP_ERROR,
                         seq_id);

    struct etcpmon_rsp_error* rsp = (struct etcpmon_rsp_error*)(buffer + sizeof(*hdr));
    rsp->error_code = error_code;
    memcpy(buffer + sizeof(*hdr) + sizeof(*rsp), msg, msg_len + 1);

    /* Log error response */
    if (server && server->log_file) {
        log_hex_data(server->log_file, "TX", buffer, rsp_size);
        fprintf(server->log_file, "%llu: [LOG] Sent RSP_ERROR code=%d msg='%s'\n",
               (unsigned long long)get_timestamp_ms(), error_code, msg);
        fflush(server->log_file);
    }

    control_client_send(server, client, buffer, rsp_size);
}

static void send_debug_config(struct control_server* server, struct control_client* client, uint8_t seq_id) {
    uint8_t cat_count = 0;
    char names_buf[512];
    names_buf[0] = '\0';
    size_t off = 0;
    for (int i = 1; i < DEBUG_CATEGORY_COUNT; i++) {
        const char* name = debug_get_category_name(i);
        size_t name_len = strlen(name);
        if (off + name_len + 2 > sizeof(names_buf)) break;
        if (cat_count > 0) { names_buf[off++] = ','; }
        memcpy(names_buf + off, name, name_len); off += name_len;
        cat_count++;
    }
    names_buf[off] = '\0';
    uint16_t names_len = (uint16_t)(off + 1);

    uint16_t payload_size = 2 + names_len + cat_count;
    uint16_t rsp_size = sizeof(struct etcpmon_msg_header) + payload_size;
    uint8_t* buffer = (uint8_t*)u_malloc(rsp_size);
    if (!buffer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to allocate debug config buffer");
        client->failed = 1;
        return;
    }

    struct etcpmon_msg_header* hdr = (struct etcpmon_msg_header*)buffer;
    etcpmon_build_header(hdr, payload_size, ETCPMON_RSP_DEBUG_CONFIG, seq_id);

    uint8_t* pay = buffer + sizeof(*hdr);
    pay[0] = (uint8_t)g_debug_config.level;
    pay[1] = cat_count;
    memcpy(pay + 2, names_buf, names_len);
    for (uint8_t i = 0; i < cat_count; i++) pay[2 + names_len + i] = (uint8_t)g_debug_config.category_levels[i + 1];

    if (server->log_file) {
        fprintf(server->log_file, "%llu: [LOG] Sent RSP_DEBUG_CONFIG global=%d categories=%d\n",
               (unsigned long long)get_timestamp_ms(), g_debug_config.level, cat_count);
        fflush(server->log_file);
    }

    control_client_send(server, client, buffer, rsp_size);
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */



/* ============================================================================
 * Node Info Helpers (send_single_node_info, send_node_list, notify_change)
 * ============================================================================ */

static void send_single_node_info(struct control_server* server, struct control_client* client, struct TOPO_GROUP_NODE* nq) {
    if (!server || !nq) return;
    struct TOPO_NODE* ni = topo_node_registry_find(server->instance->topo_groups, nq->node_id);
    if (!ni) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Node snapshot has no registry entry fd=%d node=%016llx", (int)client->fd,
                    (unsigned long long)nq->node_id);
        client->failed = 1;
        return;
    }
    uint8_t buf[TOPO_NODE_WIRE_MAX_SIZE + 2];
    int ser_len = topo_node_serialize(ni, nq, TOPO_FLAG_SEND_SUBNETS, buf + 2, sizeof(buf) - 2, 0);
    if (ser_len < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to serialize node info node=%016llx", (unsigned long long)nq->node_id);
        client->failed = 1;
        return;
    }
    buf[0] = CTRL_BGP_CMD; buf[1] = CTRL_BGP_SUBCMD;
    size_t data_size = 2 + (size_t)ser_len;

    size_t np_size = sizeof(struct etcpmon_msg_header) + data_size;
    uint8_t* buffer = (uint8_t*)u_malloc(np_size);
    if (!buffer) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to allocate node info buffer");
        client->failed = 1;
        return;
    }

    struct etcpmon_msg_header* hdr = (struct etcpmon_msg_header*)buffer;
    etcpmon_build_header(hdr, (uint16_t)data_size, ETCPMON_RSP_NODE_INFO, 0);
    memcpy(buffer + sizeof(*hdr), buf, data_size);

    control_client_send(server, client, buffer, (uint16_t)np_size);
}

static void control_server_send_node_list(struct control_server* server, struct control_client* client, uint8_t seq_id) {
    struct TOPO_GROUP* group = topo_groups_get_default(server->instance->topo_groups);
    if (!group) {
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Node subscription unavailable fd=%d seq=%u", (int)client->fd, seq_id);
        send_error(client, ETCPMON_ERR_SERVER_BUSY, "BGP not available", seq_id);
        return;
    }
    size_t count = (group->nodes ? queue_entry_count(group->nodes) : 0) + (group->local_node != NULL);
    if (count > UINT32_MAX / sizeof(uint64_t)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Node snapshot too large fd=%d nodes=%zu", (int)client->fd, count);
        client->failed = 1;
        return;
    }
    uint64_t* ids = count ? u_malloc(count * sizeof(*ids)) : NULL;
    if (count && !ids) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Node snapshot allocation failed fd=%d nodes=%zu", (int)client->fd, count);
        client->failed = 1;
        return;
    }
    size_t used = 0;
    if (group->local_node) ids[used++] = group->local_node->node_id;
    for (struct ll_entry* e = group->nodes ? group->nodes->head : NULL; e; e = e->next) {
        if (used >= count) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Node snapshot count mismatch fd=%d count=%zu", (int)client->fd, count);
            u_free(ids); client->failed = 1;
            return;
        }
        ids[used++] = ((struct TOPO_GROUP_NODE*)e)->node_id;
    }
    u_free(client->snapshot_ids);
    client->snapshot_ids = ids; client->snapshot_count = used; client->snapshot_index = 0;
    client->snapshot_active = 1; client->snapshot_seq = seq_id; client->subscribed_nodes = 1;
    DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "Node snapshot started fd=%d seq=%u nodes=%zu id_bytes=%zu",
               (int)client->fd, seq_id, used, used * sizeof(*ids));
}

/* Продолжение producer после опустошения TX; за один вызов создаётся максимум один кадр. */
static void control_output_ready(struct ll_queue* queue, void* arg) {
    struct control_client* client = arg;
    struct control_server* server = client->server;
    if (client->failed) { close_client(server, client); return; }
    if (queue->head) {
        if (control_wait_output(client) < 0) close_client(server, client);
        return;
    }
    struct TOPO_GROUP* group = topo_groups_get_default(server->instance->topo_groups);
    if ((client->snapshot_active || client->dirty_count) && !group) {
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Subscribed UTUN group disappeared fd=%d", (int)client->fd);
        close_client(server, client);
        return;
    }
    while (client->snapshot_active && client->snapshot_index < client->snapshot_count && !queue->head && !client->failed) {
        uint64_t id = client->snapshot_ids[client->snapshot_index++];
        struct TOPO_GROUP_NODE* node = group->local_node && group->local_node->node_id == id ?
                                      group->local_node : topo_node_find_by_id(group, id);
        if (node) send_single_node_info(server, client, node);
        else DEBUG_DEBUG(DEBUG_CATEGORY_CONTROL, "Snapshot skips removed node fd=%d node=%016llx", (int)client->fd,
                         (unsigned long long)id);
    }
    if (client->snapshot_active && client->snapshot_index == client->snapshot_count && !queue->head && !client->failed) {
        uint8_t* buffer = u_malloc(sizeof(struct etcpmon_msg_header));
        if (!buffer) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to allocate node list end fd=%d", (int)client->fd);
            client->failed = 1;
        } else {
            etcpmon_build_header((void*)buffer, 0, ETCPMON_RSP_NODE_INFO_END, client->snapshot_seq);
            DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "Node snapshot queued fd=%d seq=%u nodes=%zu",
                       (int)client->fd, client->snapshot_seq, client->snapshot_count);
            client->snapshot_active = 0;
            u_free(client->snapshot_ids); client->snapshot_ids = NULL;
            client->snapshot_count = 0; client->snapshot_index = 0;
            control_client_send(server, client, buffer, sizeof(struct etcpmon_msg_header));
        }
    }
    while (!client->snapshot_active && client->dirty_count && !queue->head && !client->failed) {
        uint64_t id = client->dirty_ids[0];
        int removed = client->dirty_removed[0];
        client->dirty_count--;
        memmove(client->dirty_ids, client->dirty_ids + 1, client->dirty_count * sizeof(*client->dirty_ids));
        memmove(client->dirty_removed, client->dirty_removed + 1, client->dirty_count * sizeof(*client->dirty_removed));
        struct TOPO_GROUP_NODE* node = group->local_node && group->local_node->node_id == id ?
                                      group->local_node : topo_node_find_by_id(group, id);
        if (!removed && node) send_single_node_info(server, client, node);
        else {
            size_t size = sizeof(struct etcpmon_msg_header) + sizeof(id);
            uint8_t* buffer = u_malloc(size);
            if (!buffer) {
                DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to allocate node removal fd=%d", (int)client->fd);
                client->failed = 1;
                break;
            }
            etcpmon_build_header((void*)buffer, sizeof(id), ETCPMON_RSP_NODE_REMOVED, 0);
            memcpy(buffer + sizeof(struct etcpmon_msg_header), &id, sizeof(id));
            control_client_send(server, client, buffer, (uint16_t)size);
        }
    }
    if (client->failed) { close_client(server, client); return; }
    if (queue->head) return;
    if (handle_client_data(server, client) < 0) return;
    if (queue->head || client->snapshot_active || client->dirty_count) return;
    if (client->read_paused) {
        if (uasync_set_socket_read(server->ua, client->socket_id, 1) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Failed to resume control reads fd=%d", (int)client->fd);
            close_client(server, client);
            return;
        }
        client->read_paused = 0;
        DEBUG_DEBUG(DEBUG_CATEGORY_CONTROL, "Control reads resumed fd=%d buffered=%u", (int)client->fd, client->recv_len);
    }
    if (client->timer_is_tx && control_client_arm_timer(client, 0) < 0) close_client(server, client);
}

/* Сохраняет только последнее событие узла. Ограниченный набор не растёт от повторов одного ID. */
static int control_mark_node(struct control_client* client, uint64_t id, int removed) {
    size_t index = 0;
    while (index < client->dirty_count && client->dirty_ids[index] != id) index++;
    if (index == CONTROL_NODE_DIRTY_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_CONTROL, "Control subscriber overwhelmed fd=%d dirty_nodes=%zu queued_bytes=%zu",
                   (int)client->fd, client->dirty_count, queue_total_bytes(client->output_queue));
        return -1;
    }
    if (index == client->dirty_count) client->dirty_count++;
    client->dirty_ids[index] = id; client->dirty_removed[index] = removed != 0;
    return control_wait_output(client);
}

void control_server_notify_node_change(struct control_server* server, struct TOPO_GROUP_NODE* node) {
    if (!server || !node) return;
    struct control_client* client = server->clients;
    while (client) {
        struct control_client* next = client->next;
        if (client->subscribed_nodes && control_mark_node(client, node->node_id, 0) < 0) close_client(server, client);
        client = next;
    }
}

void control_server_notify_node_removed(struct control_server* server, uint64_t node_id) {
    if (!server) return;
    struct control_client* client = server->clients;
    while (client) {
        struct control_client* next = client->next;
        if (client->subscribed_nodes && control_mark_node(client, node_id, 1) < 0) close_client(server, client);
        client = next;
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

void control_server_process_updates(struct control_server* server) {
    if (!server) return;
    
    /* Process any pending data for all clients */
    struct control_client* client = server->clients;
    while (client) {
        struct control_client* next = client->next;
        
//        if (!client->connected) {
//            close_client(server, client);
//        } else if (client->recv_len > 0) {
            handle_client_data(server, client);
//        }
        
        client = next;
    }
}

uint32_t control_server_get_client_count(const struct control_server* server) {
    return server ? server->client_count : 0;
}
