// test_tcp_proxy_remote.c — 2-node TCP 1MB forward via etcp_router
// Architecture: Client_A (active uIP) ↔ socketpair ↔ B (passive, etcp) ↔ Exit (tcp_proxy_server) ↔ echo
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#endif
#include <signal.h>
#include <errno.h>
#include <time.h>

#include "../src/proxy/tcp_proxy_client.h"
#include "etcp.h"
#include "etcp_router.h"
#include "../src/proxy/tcp_proxy_server.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "../src/uip/uip.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"

#define TEST_SIZE     (1 * 1024 * 1024)
#define CHUNK_SIZE    1460
#define TIMEOUT_MS    45000

static void*        g_to_id;
static volatile int g_done, g_phase;
static pid_t        g_echo_pid;
static struct UTUN_INSTANCE* g_exit, *g_b;
static struct UASYNC* g_ua;
static int          g_pair[2];
static struct tcp_proxy_client* g_proxy_b, *g_cli;
static int          g_conn_idx, g_conn_up;
static struct uip_conn* g_uc;
static uint8_t      *g_send_buf, *g_recv_buf;
static size_t       g_sent, g_rcvd;
static int          g_ok;
static struct timespec g_t_start, g_t_end;
static double       g_elapsed;
static int          g_echo_port, g_srv_a, g_srv_b;

static void on_signal(int sig) { (void)sig; g_done = -1; }

static int alloc_port(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family=AF_INET, .sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    bind(s, (struct sockaddr*)&a, sizeof(a));
    struct sockaddr_in b; socklen_t l = sizeof(b);
    getsockname(s, (struct sockaddr*)&b, &l);
    int p = ntohs(b.sin_port);
    close(s);
    return p;
}

static void echo_server(uint16_t port) {
    signal(SIGALRM, on_signal);
    alarm(10);
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) _exit(1);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a = {.sin_family=AF_INET, .sin_port=htons(port), .sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    if (bind(srv,(struct sockaddr*)&a,sizeof(a))<0||listen(srv,1)<0){close(srv);_exit(1);}
    int cli = accept(srv,NULL,NULL);
    if (cli<0){close(srv);_exit(1);}
    uint8_t buf[65536]; ssize_t n;
    while((n=recv(cli,buf,sizeof(buf),0))>0){ssize_t s=0; while(s<n){ssize_t r=send(cli,buf+s,n-s,0); if(r<0)goto done; s+=r;}}
done: close(cli); close(srv); _exit(0);
}

static void start_test(void) {
    struct tcp_proxy_client_mapping_config m = {.local_port=9090,.remote_ip="127.0.0.1",.remote_port=g_echo_port};
    g_proxy_b = tcp_proxy_client_create(g_b, g_ua, "tun_tcp", "10.99.0.1", 0, 0, &m, 1, g_a->my_ed25519_pubkey, 0, NULL, 0, NULL);
    if (!g_proxy_b) { printf("[FAIL] proxy_b create\n"); g_done=-1; return; }
    g_b->tcp_proxy_client = g_proxy_b;

    g_cli = tcp_proxy_client_create(NULL, g_ua, "tun_tcp", "10.99.0.1", 0, 0, NULL, 0, 0, 0, NULL, 0, NULL);
    if (!g_cli) { printf("[FAIL] cli create\n"); g_done=-1; return; }

    g_conn_idx = tcp_proxy_active_open(g_cli, "10.99.0.100", 9090);
    if (g_conn_idx < 0) { printf("[FAIL] active_open\n"); g_done=-1; return; }
    g_uc = &uip_conns[g_conn_idx];
    g_phase = 1;
}

static void poll_test(void) {
    if (g_phase == 1) {
        if (g_uc->tcpstateflags == UIP_ESTABLISHED) { clock_gettime(CLOCK_MONOTONIC,&g_t_start); g_phase=2; }
        else if (g_uc->tcpstateflags == UIP_CLOSED) { printf("[FAIL] handshake\n"); g_done=-1; }
        return;
    }
    if (g_phase == 2) {
        while (g_sent < TEST_SIZE) { size_t c = TEST_SIZE - g_sent; if (c > CHUNK_SIZE) c = CHUNK_SIZE;
            if (tcp_proxy_active_send(g_cli,g_conn_idx,g_send_buf+g_sent,c)!=0) break; g_sent += c; }
        g_phase = 3; return;
    }
    if (g_phase == 3) { if (tcp_proxy_active_send_done(g_cli,g_conn_idx)) g_phase=4; return; }
    if (g_phase == 4) {
        ssize_t n = tcp_proxy_active_recv(g_cli,g_conn_idx,g_recv_buf+g_rcvd,TEST_SIZE-g_rcvd);
        if (n > 0) g_rcvd += n;
        if (g_rcvd >= TEST_SIZE) {
            clock_gettime(CLOCK_MONOTONIC,&g_t_end);
            g_elapsed = (g_t_end.tv_sec-g_t_start.tv_sec)+(g_t_end.tv_nsec-g_t_start.tv_nsec)/1e9;
            g_ok = (memcmp(g_send_buf,g_recv_buf,TEST_SIZE)==0);
            g_done = 1;
        }
    }
}

static void monitor(void* arg) {
    (void)arg;
    if (g_done) return;
    if (g_phase == 0 && !g_conn_up) {
        int bup=0, eup=0;
        if (g_b) { struct ll_entry* e=g_b->connections->head;
          while (e) { struct conn_queue_entry* ce=(struct conn_queue_entry*)e->data;
            struct ETCP_CONN*c=ce->conn;
            for (struct ETCP_LINK*l=c->links;l;l=l->next) if (l->initialized&&c->crypto_ctx.initialized) bup=1;
            e=e->next; } }
        if (g_exit) { struct ll_entry* e=g_exit->connections->head;
          while (e) { struct conn_queue_entry* ce=(struct conn_queue_entry*)e->data;
            struct ETCP_CONN*c=ce->conn;
            for (struct ETCP_LINK*l=c->links;l;l=l->next) if (l->initialized&&c->crypto_ctx.initialized) eup=1;
            e=e->next; } }
        if (bup && eup) { g_conn_up=1; start_test(); }
    }
    if (g_conn_up && !g_done) poll_test();
    if (!g_done) g_to_id = uasync_set_timeout(g_ua, 5, NULL, monitor, "mon");
}

static void test_timeout(void* arg) {
    (void)arg;
    if (!g_done) { printf("[FAIL] timeout phase=%d sent=%zu rcvd=%zu\n",g_phase,g_sent,g_rcvd); g_done=-1; }
}

static const char* cfg_exit(int srv_port) { static char b[1024]; snprintf(b,sizeof(b),
    "[global]\n"
    "my_private_key=38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68\n"
    "my_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
    "tun_ip=10.99.0.1/24\ntun_ifname=tun99\n"
    "[server:s1]\naddr=127.0.0.1:%d\ntype=public\n[allowed_keys]\nallow_all=1\n[tcp_proxy_server]\nenabled=yes\n", srv_port); return b; }

static const char* cfg_b(int srv_port, int cli_port) { static char b[1024]; snprintf(b,sizeof(b),
    "[global]\n"
    "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
    "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
    "tun_ip=10.99.0.2/24\ntun_ifname=tun98\n"
    "[server:s1]\naddr=127.0.0.1:%d\ntype=public\n[client:c1]\nkeepalive=1\nlink=s1:127.0.0.1:%d\n"
    "peer_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
    "[tcp_proxy_server]\nenabled=yes\n", srv_port, cli_port); return b; }

int main(void) {
    printf("[SKIP] Active connections API removed, test skipped\n");
    return 0;
#if 0
    printf("=== test_tcp_proxy_remote ===\n");
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_categories(DEBUG_CATEGORY_ALL);
    utun_instance_set_tun_init_enabled(0);
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal);
    srand((unsigned)time(NULL));

    g_send_buf = malloc(TEST_SIZE); g_recv_buf = malloc(TEST_SIZE);
    if (!g_send_buf || !g_recv_buf) { printf("[FAIL] malloc\n"); return 1; }
    for (size_t i = 0; i < TEST_SIZE; i++) g_send_buf[i] = (uint8_t)(rand() & 0xFF);

    g_echo_port = alloc_port(); g_srv_a = alloc_port(); g_srv_b = alloc_port();
    if (!g_echo_port || !g_srv_a || !g_srv_b) { printf("[FAIL] alloc_port\n"); return 1; }

    g_echo_pid = fork();
    if (g_echo_pid == 0) echo_server(g_echo_port);
    usleep(50000);

    g_ua = uasync_create();
    if (!g_ua) { printf("[FAIL] uasync\n"); goto done; }
    g_exit = utun_instance_create_from_str(g_ua, cfg_exit(g_srv_a));
    g_b = utun_instance_create_from_str(g_ua, cfg_b(g_srv_b, g_srv_a));
    if (!g_exit || !g_b) { printf("[FAIL] create\n"); goto done; }
    if (utun_instance_init(g_exit) < 0 || utun_instance_init(g_b) < 0) { printf("[FAIL] init\n"); goto done; }
    for (int i = 0; i < 50; i++) uasync_poll(g_ua, 10);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, g_pair) < 0) { perror("pair"); goto done; }

    g_to_id = uasync_set_timeout(g_ua, 50, NULL, monitor, "mon");
    void* to_id = uasync_set_timeout(g_ua, TIMEOUT_MS*10, NULL, test_timeout, "to");
    while (!g_done) uasync_poll(g_ua, 10);
    if (to_id) uasync_cancel_timeout(g_ua, to_id);

    if (g_ok) printf("[PASS] test_tcp_proxy_remote — 1MB in %.2fs (%.2f MB/s)\n", g_elapsed, (TEST_SIZE/1e6)/g_elapsed);
    else if (g_done == -1) printf("[FAIL] test_tcp_proxy_remote\n");

done:
    if (g_to_id) uasync_cancel_timeout(g_ua, g_to_id);
    if (g_cli) tcp_proxy_client_destroy(g_cli);
    if (g_proxy_b) tcp_proxy_client_destroy(g_proxy_b);
    if (g_exit) { g_exit->running=0; utun_instance_destroy(g_exit); }
    if (g_b) { g_b->running=0; utun_instance_destroy(g_b); }
    if (g_ua) uasync_destroy(g_ua, 0);
    if (g_echo_pid) { kill(g_echo_pid, SIGTERM); waitpid(g_echo_pid, NULL, 0); }
    free(g_send_buf); free(g_recv_buf);
    return g_ok ? 0 : 1;
#endif
}
