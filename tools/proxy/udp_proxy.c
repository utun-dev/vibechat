#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>

#include <u_async.h>
#include <socket_compat.h>
#include <debug_config.h>
#include <mem.h>

#define BUFFER_SIZE 65536

typedef struct udp_flow {
    struct sockaddr_in client_addr;
    socket_t backend_sock;
    void* socket_id;
    struct udp_flow* next;
} udp_flow_t;

typedef struct {
    uasync_t* ua;
    socket_t listen_sock;
    void* listen_id;
    struct sockaddr_in target_addr;
    char listen_ip[64];
    int listen_port;
    int loss_forward;
    int loss_back;
    int delay_min_fwd;
    int delay_max_fwd;
    int delay_min_back;
    int delay_max_back;
    udp_flow_t* flows;
    char buffer[BUFFER_SIZE];
} proxy_context_t;

static void client_read_cb(socket_t sock, void* arg);
static void backend_read_cb(socket_t sock, void* arg);

static udp_flow_t* find_flow_by_client(proxy_context_t* ctx, struct sockaddr_in* client) {
    for (udp_flow_t* f = ctx->flows; f; f = f->next) {
        if (memcmp(&f->client_addr, client, sizeof(*client)) == 0) return f;
    }
    return NULL;
}

static udp_flow_t* find_flow_by_sock(proxy_context_t* ctx, socket_t sock) {
    for (udp_flow_t* f = ctx->flows; f; f = f->next) {
        if (f->backend_sock == sock) return f;
    }
    return NULL;
}

static udp_flow_t* create_flow(proxy_context_t* ctx, struct sockaddr_in* client) {
    udp_flow_t* flow = (udp_flow_t*)u_malloc(sizeof(udp_flow_t));
    if (!flow) return NULL;
    
    memcpy(&flow->client_addr, client, sizeof(*client));
    flow->backend_sock = socket_create_udp(AF_INET);
    if (flow->backend_sock == SOCKET_INVALID) {
        u_free(flow);
        return NULL;
    }
    
    socket_set_nonblocking(flow->backend_sock);
    socket_set_reuseaddr(flow->backend_sock, 1);
    
    struct sockaddr_in baddr = {0};
    baddr.sin_family = AF_INET;
    baddr.sin_addr.s_addr = INADDR_ANY;
    baddr.sin_port = 0;
    bind((int)flow->backend_sock, (struct sockaddr*)&baddr, sizeof(baddr));
    
    flow->socket_id = uasync_add_socket_t(ctx->ua, flow->backend_sock, backend_read_cb, NULL, NULL, "udp_backend", ctx);
    if (!flow->socket_id) {
        socket_close_wrapper(flow->backend_sock);
        u_free(flow);
        return NULL;
    }
    
    flow->next = ctx->flows;
    ctx->flows = flow;
    
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "New flow: client %s:%d -> backend socket %d", 
               inet_ntoa(client->sin_addr), ntohs(client->sin_port), (int)flow->backend_sock);
    return flow;
}

static void client_read_cb(socket_t sock, void* arg) {
    proxy_context_t* ctx = (proxy_context_t*)arg;
    struct sockaddr_in client;
    socklen_t alen = sizeof(client);
    
    ssize_t n = socket_recvfrom(sock, ctx->buffer, BUFFER_SIZE, (struct sockaddr*)&client, &alen);
    if (n <= 0) return;
    
    udp_flow_t* flow = find_flow_by_client(ctx, &client);
    if (!flow) {
        flow = create_flow(ctx, &client);
        if (!flow) return;
    }
    
    if (ctx->loss_forward > 0 && (rand() % 100 < ctx->loss_forward)) {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "Forward packet dropped");
        return;
    }
    
    int delay = ctx->delay_min_fwd;
    if (ctx->delay_max_fwd > ctx->delay_min_fwd) delay += rand() % (ctx->delay_max_fwd - ctx->delay_min_fwd + 1);
    
    if (delay > 0) {
        // simple immediate for now, full timeout later
        usleep(delay * 1000); // temporary
    }
    
    socket_sendto(flow->backend_sock, ctx->buffer, n, (struct sockaddr*)&ctx->target_addr, sizeof(ctx->target_addr));
}

static void backend_read_cb(socket_t sock, void* arg) {
    proxy_context_t* ctx = (proxy_context_t*)arg;
    udp_flow_t* flow = find_flow_by_sock(ctx, sock);
    if (!flow) return;
    
    struct sockaddr_in from;
    socklen_t alen = sizeof(from);
    ssize_t n = socket_recvfrom(sock, ctx->buffer, BUFFER_SIZE, (struct sockaddr*)&from, &alen);
    if (n <= 0) return;
    
    if (ctx->loss_back > 0 && (rand() % 100 < ctx->loss_back)) {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "Back packet dropped");
        return;
    }
    
    int delay = ctx->delay_min_back;
    if (ctx->delay_max_back > ctx->delay_min_back) delay += rand() % (ctx->delay_max_back - ctx->delay_min_back + 1);
    
    if (delay > 0) {
        usleep(delay * 1000); // temporary
    }
    
    socket_sendto(ctx->listen_sock, ctx->buffer, n, (struct sockaddr*)&flow->client_addr, sizeof(flow->client_addr));
}

static void print_usage(const char* prog) {
    printf("UDP Proxy - network loss/delay emulator\n\n");
    printf("Usage: %s --listen [IP:]PORT --target IP:PORT [options]\n\n", prog);
    printf("Required:\n");
    printf("  --listen [IP:]PORT     Listen IP:port (default 0.0.0.0)\n");
    printf("  --target IP:PORT       Destination IP:port\n\n");
    printf("Options:\n");
    printf("  --loss-forward N       Forward loss %% (0-100, default 0)\n");
    printf("  --loss-back N          Backward loss %% (0-100, default 0)\n");
    printf("  --delay-forward A:B    Forward delay ms range (default 0:0)\n");
    printf("  --delay-back A:B       Backward delay ms range (default 0:0)\n");
    printf("  --help                 Show this help\n\n");
    printf("Example: %s --listen 0.0.0.0:12345 --target 8.8.8.8:53 --loss-forward 5 --delay-forward 10:50\n", prog);
    exit(0);
}

int main(int argc, char* argv[]) {
    if (argc < 3) print_usage(argv[0]);
    
    srand(time(NULL));
    socket_platform_init();
    
    proxy_context_t ctx = {0};
    ctx.ua = uasync_create();
    if (!ctx.ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "uasync_create failed");
        return 1;
    }
    
    ctx.loss_forward = 0; ctx.loss_back = 0;
    ctx.delay_min_fwd = ctx.delay_max_fwd = 0;
    ctx.delay_min_back = ctx.delay_max_back = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) print_usage(argv[0]);
        if (strcmp(argv[i], "--listen") == 0 && i+1 < argc) {
            char ip[64] = "0.0.0.0"; int p = 0;
            const char* val = argv[++i];
            if (strchr(val, ':')) {
                sscanf(val, "%63[^:]:%d", ip, &p);
            } else {
                p = atoi(val);
            }
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(p);
            inet_aton(ip, &addr.sin_addr);
            strncpy(ctx.listen_ip, ip, sizeof(ctx.listen_ip)-1);
            ctx.listen_port = p;
            ctx.listen_sock = socket_create_udp(AF_INET);
            socket_set_nonblocking(ctx.listen_sock);
            socket_set_reuseaddr(ctx.listen_sock, 1);
            if (bind((int)ctx.listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "bind listen failed on %s:%d", ip, p);
                return 1;
            }
            ctx.listen_id = uasync_add_socket_t(ctx.ua, ctx.listen_sock, client_read_cb, NULL, NULL, "udp_listen", &ctx);
        } else if (strcmp(argv[i], "--target") == 0 && i+1 < argc) {
            char ip[64] = {0}; int port = 0;
            if (sscanf(argv[++i], "%63[^:]:%d", ip, &port) == 2) {
                ctx.target_addr.sin_family = AF_INET;
                ctx.target_addr.sin_port = htons(port);
                inet_aton(ip, &ctx.target_addr.sin_addr);
            }
        } else if (strcmp(argv[i], "--loss-forward") == 0 && i+1 < argc) {
            ctx.loss_forward = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--loss-back") == 0 && i+1 < argc) {
            ctx.loss_back = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--delay-forward") == 0 && i+1 < argc) {
            int mn=0,mx=0; sscanf(argv[++i], "%d:%d", &mn, &mx);
            ctx.delay_min_fwd = mn; ctx.delay_max_fwd = mx > mn ? mx : mn;
        } else if (strcmp(argv[i], "--delay-back") == 0 && i+1 < argc) {
            int mn=0,mx=0; sscanf(argv[++i], "%d:%d", &mn, &mx);
            ctx.delay_min_back = mn; ctx.delay_max_back = mx > mn ? mx : mn;
        }
    }
    
    if (ctx.listen_sock == 0 || ctx.target_addr.sin_port == 0) {
        fprintf(stderr, "Error: --listen and --target required\n");
        print_usage(argv[0]);
    }
    
    printf("UDP Proxy: listen %s:%d → %s:%d | loss %d%%/%d%% | delay %d-%d/%d-%d ms\n",
           ctx.listen_ip, ctx.listen_port,
           inet_ntoa(ctx.target_addr.sin_addr), ntohs(ctx.target_addr.sin_port),
           ctx.loss_forward, ctx.loss_back,
           ctx.delay_min_fwd, ctx.delay_max_fwd, ctx.delay_min_back, ctx.delay_max_back);
    
    uasync_mainloop(ctx.ua);
    return 0;
}
