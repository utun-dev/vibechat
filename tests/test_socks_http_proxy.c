// test_socks_http_proxy.c — Comprehensive SOCKS5 + HTTP proxy integration test
// 4 workers: socks_ipv4, socks_domain, http_proxy, https_connect
// Files: 1KB, 100KB, 1MB, 10MB — deterministic content, byte-by-byte verification
// HTTP + HTTPS (self-signed cert) + POST/echo + HEAD + OPTIONS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <poll.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"
#include "../src/proxy/socks_proxy.h"
#include "../src/proxy/tcp_proxy_client.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/ll_queue.h"
#include "../lib/mem.h"
#include "../lib/memory_pool.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_api.h"
#include "etcp_router.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "../src/tun_if.h"

#define TIMEOUT_MS 300000
#define WORKERS     4

enum { W_SOCKS_IPV4, W_SOCKS_DOMAIN, W_HTTP_PROXY, W_HTTPS_CONNECT };

static const int file_sizes[] = { 1024, 100*1024, 1024*1024, 10*1024*1024 };
static const char* file_names[] = { "f_1k.bin", "f_100k.bin", "f_1m.bin", "f_10m.bin" };
static const int num_files = 4;

#ifndef _WIN32
static struct UTUN_INSTANCE* g_cli = NULL;
static struct UTUN_INSTANCE* g_srv = NULL;
static struct UASYNC* g_ua = NULL;
static int g_ok = 0, g_done = 0, g_phase = 0;
static int g_worker_failures;
static pid_t g_http_pid = 0, g_https_pid = 0;
static pid_t g_workers[WORKERS];
static pid_t g_workers_orig[WORKERS];
static int g_http_port = 0, g_https_port = 0, g_cli_port = 0, g_srv_port = 0;
static int g_socks_port = 0, g_http_proxy_port = 0;
static char g_tmpdir[256];
static char g_cert[512], g_key[512];
static void* g_mon_id = NULL;
static int g_file_seed = 0;

static void gen_file(const char* path, int size, int seed) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "gen_file: fopen(%s) failed\n", path); return; }
    uint8_t buf[4096];
    for (int off = 0; off < size; off += (int)sizeof(buf)) {
        int chunk = size - off; if (chunk > (int)sizeof(buf)) chunk = (int)sizeof(buf);
        for (int i = 0; i < chunk; i++) buf[i] = (uint8_t)(((off + i) ^ seed) & 0xFF);
        fwrite(buf, 1, (size_t)chunk, f);
    }
    fclose(f);
}

// Сервер сообщает готовность после listen и настройки TLS; EOF обнаруживает ошибку запуска без паузы.
static int wait_server_ready(int fd, int* port, const char* name) {
    uint64_t start = get_time_us(), deadline = start + 5000000;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int result;
    do {
        uint64_t now = get_time_us();
        result = poll(&pfd, 1, now < deadline ? (int)((deadline - now + 999) / 1000) : 0);
    } while (result < 0 && errno == EINTR);
    uint8_t assigned[2] = {0};
    ssize_t bytes = result > 0 ? read(fd, assigned, sizeof(assigned)) : -1;
    int error = result < 0 || (result > 0 && bytes < 0) ? errno : 0;
    close(fd);
    int actual = ((int)assigned[0] << 8) | assigned[1];
    if (result <= 0 || bytes != sizeof(assigned) || !actual) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "test %s server startup failed: poll=%d events=%x bytes=%ld port=%d errno=%d",
                    name, result, pfd.revents, (long)bytes, actual, error);
        return -1;
    }
    *port = actual;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "test %s server ready: port=%d elapsed=%lluus", name, *port,
               (unsigned long long)(get_time_us() - start));
    return 0;
}

static int start_http_server(int* port, const char* dir) {
    char script[512]; snprintf(script, sizeof(script), "%s/_srv.py", dir);
    FILE* sf = fopen(script, "w");
    if (!sf) return -1;
    fprintf(sf,
        "import sys,os,ssl,threading\n"
        "from http.server import HTTPServer,SimpleHTTPRequestHandler\n"
        "class H(SimpleHTTPRequestHandler):\n"
        " def do_POST(self):\n"
        "  if self.path=='/echo':\n"
        "   n=int(self.headers.get('Content-Length',0))\n"
        "   b=self.rfile.read(n)\n"
        "   self.send_response(200)\n"
        "   self.send_header('Content-Type','application/octet-stream')\n"
        "   self.send_header('Content-Length',str(len(b)))\n"
        "   self.end_headers();self.wfile.write(b)\n"
        "  else:self.send_response(405);self.end_headers()\n"
        " def do_OPTIONS(self):\n"
        "  self.send_response(200)\n"
        "  self.send_header('Allow','GET,HEAD,POST,OPTIONS')\n"
        "  self.send_header('Content-Length','0')\n"
        "  self.end_headers()\n"
        "port=int(sys.argv[1])\n"
        "os.chdir(sys.argv[2])\n"
        "httpd=HTTPServer(('127.0.0.1',port),H)\n"
        "ready=int(sys.argv[3]);os.write(ready,httpd.server_port.to_bytes(2,'big'));os.close(ready)\n"
        "httpd.serve_forever()\n"
    );
    fclose(sf);

    int ready[2];
    if (pipe(ready) < 0) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "test HTTP ready pipe: %s", strerror(errno)); return -1; }
    pid_t pid = fork();
    if (pid < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "test HTTP server fork: %s", strerror(errno));
        close(ready[0]); close(ready[1]); return -1;
    }
    if (pid == 0) {
        close(ready[0]);
        char ready_str[16]; snprintf(ready_str, sizeof(ready_str), "%d", ready[1]);
        execlp("python3", "python3", script, "0", dir, ready_str, (char*)NULL);
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "test HTTP python exec: %s", strerror(errno));
        _exit(1);
    }
    close(ready[1]);
    g_http_pid = pid;
    return wait_server_ready(ready[0], port, "HTTP");
}

static int start_https_server(int* port, const char* dir, const char* cert, const char* key) {
    char script[512]; snprintf(script, sizeof(script), "%s/_srv_https.py", dir);
    FILE* sf = fopen(script, "w");
    if (!sf) return -1;
    fprintf(sf,
        "import sys,os,ssl,threading,traceback\n"
        "from http.server import HTTPServer,SimpleHTTPRequestHandler\n"
        "class H(SimpleHTTPRequestHandler):\n"
        " def do_GET(self):\n"
        "  try:\n   super().do_GET()\n"
        "  except Exception as e:\n"
        "   sys.stderr.write(f'HTTPS_ERR GET {self.path}: {e}\\n{traceback.format_exc()}\\n')\n"
        " def do_POST(self):\n"
        "  if self.path=='/echo':\n"
        "   n=int(self.headers.get('Content-Length',0))\n"
        "   b=self.rfile.read(n)\n"
        "   self.send_response(200)\n"
        "   self.send_header('Content-Type','application/octet-stream')\n"
        "   self.send_header('Content-Length',str(len(b)))\n"
        "   self.end_headers();self.wfile.write(b)\n"
        "  else:self.send_response(405);self.end_headers()\n"
        " def do_OPTIONS(self):\n"
        "  self.send_response(200)\n"
        "  self.send_header('Allow','GET,HEAD,POST,OPTIONS')\n"
        "  self.send_header('Content-Length','0')\n"
        "  self.end_headers()\n"
        "class S(HTTPServer):\n"
        " def shutdown_request(self,request):\n"
        "  if os.environ.get('UTUN_TEST_TLS_ABRUPT'):\n"
        "   return super().shutdown_request(request)\n"
        "  try:\n"
        "   request.settimeout(2.0)\n"
        "   plain=request.unwrap();plain.close()\n"
        "  except (ssl.SSLError,OSError) as e:\n"
        "   sys.stderr.write(f'HTTPS_SHUTDOWN_ERR: {e}\\n')\n"
        "   request.close()\n"
        "os.chdir(sys.argv[3])\n"
        "ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)\n"
        "ctx.load_cert_chain(sys.argv[1],sys.argv[2])\n"
        "httpd=S(('127.0.0.1',int(sys.argv[4])),H)\n"
        "httpd.socket=ctx.wrap_socket(httpd.socket,server_side=True)\n"
        "httpd.timeout=None\n"
        "ready=int(sys.argv[5]);os.write(ready,httpd.server_port.to_bytes(2,'big'));os.close(ready)\n"
        "httpd.serve_forever()\n"
    );
    fclose(sf);

    int ready[2];
    if (pipe(ready) < 0) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "test HTTPS ready pipe: %s", strerror(errno)); return -1; }
    pid_t pid = fork();
    if (pid < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "test HTTPS server fork: %s", strerror(errno));
        close(ready[0]); close(ready[1]); return -1;
    }
    if (pid == 0) {
        close(ready[0]);
        char ready_str[16]; snprintf(ready_str, sizeof(ready_str), "%d", ready[1]);
        execlp("python3", "python3", script, cert, key, dir, "0", ready_str, (char*)NULL);
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "test HTTPS python exec: %s", strerror(errno));
        _exit(1);
    }
    close(ready[1]);
    g_https_pid = pid;
    return wait_server_ready(ready[0], port, "HTTPS");
}

static int gen_self_signed_cert(const char* tmpdir, char* cert_out, size_t cert_sz, char* key_out, size_t key_sz) {
    snprintf(cert_out, cert_sz, "%s/cert.pem", tmpdir);
    snprintf(key_out, key_sz, "%s/key.pem", tmpdir);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s -days 1 -nodes -subj '/CN=127.0.0.1' 2>/dev/null",
             key_out, cert_out);
    int rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "[FAIL] openssl cert generation\n"); return -1; }
    return 0;
}

static char* make_cfg_server(void) {
    static char buf[1024];
    snprintf(buf, sizeof(buf),
        "[global]\ntun_enabled=no\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[server: s1]\naddr=127.0.0.1:%d\ntype=public\n"
        "[allowed_keys]\nallow_all=1\n"
        "[tcp_proxy_server]\nconnect_timeout=2000\n", g_srv_port);
    return buf;
}

static char* make_cfg_client(void) {
    static char buf[1024];
    snprintf(buf, sizeof(buf),
        "[global]\ntun_enabled=no\n"
        "my_private_key=38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68\n"
        "my_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
        "[server: s1]\naddr=127.0.0.1:%d\ntype=public\n"
        "[client: c1]\nkeepalive=1\nlink=s1:127.0.0.1:%d\n"
        "peer_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[tcp_proxy_client]\n"
        "enabled=yes\n"
        "socks_enabled=yes\n"
        "socks_addr=127.0.0.1:%d\n"
        "http_proxy_enabled=yes\n"
        "http_proxy_addr=127.0.0.1:%d\n"
        "via_node=e6a1785a9fa59bf5278e4d14c716dbe9782343092f301e02e8a6554998f13d64\n",
        g_cli_port, g_srv_port, g_socks_port, g_http_proxy_port);
    return buf;
}

static int conn_ready(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections) return 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_CONN* c = ce->conn;
        for (struct ETCP_LINK* l = c->links; l; l = l->next)
            if (l->initialized && c->crypto_ctx.initialized) return 1;
        entry = entry->next;
    }
    return 0;
}

static int worker_main(int wtype, const char* socks_proxy, const char* http_proxy,
                       const char* url_http, const char* url_https, const char* tmpdir) {
    const char* type_names[] = { "socks_ipv4", "socks_domain", "http_proxy", "https_connect" };
    const char* tname = type_names[wtype];

    char prof_path[512]; snprintf(prof_path, sizeof(prof_path), "%s/.prof_%d", tmpdir, getpid());
    FILE* prof = fopen(prof_path, "w");
    if (prof) fprintf(prof, "%s\n", tname);

    struct timespec t1, t2;
    double times_ms[num_files];

    for (int fi = 0; fi < num_files; fi++) {
        char expected[512], out[512], url[512], cmd[2048];
        snprintf(expected, sizeof(expected), "%s/%s", tmpdir, file_names[fi]);
        snprintf(out, sizeof(out), "%s/out_%d_%d_%s", tmpdir, getpid(), fi, file_names[fi]);

        const char* base = (wtype == W_HTTPS_CONNECT) ? url_https : url_http;
        snprintf(url, sizeof(url), "%s/%s", base, file_names[fi]);

        switch (wtype) {
        case W_SOCKS_IPV4:
            snprintf(cmd, sizeof(cmd), "curl -s --connect-timeout 15 --max-time 60 --socks5 %s -o %s %s 2>/dev/null",
                     socks_proxy, out, url);
            break;
        case W_SOCKS_DOMAIN:
            snprintf(cmd, sizeof(cmd), "curl -s --connect-timeout 15 --max-time 60 --socks5-hostname %s -o %s %s 2>/dev/null",
                     socks_proxy, out, url);
            break;
        case W_HTTP_PROXY:
            snprintf(cmd, sizeof(cmd), "curl -s --connect-timeout 15 --max-time 60 -x http://%s -o %s %s 2>/dev/null",
                     http_proxy, out, url);
            break;
        case W_HTTPS_CONNECT:
            snprintf(cmd, sizeof(cmd), "curl -s --connect-timeout 15 --max-time 60 --proxytunnel -x %s -k -o %s %s 2>/dev/null",
                     http_proxy, out, url);
            break;
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        int rc = system(cmd);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        if (rc != 0) { fprintf(stderr, "[FAIL] %s %s curl exit=%d\n", tname, file_names[fi], WEXITSTATUS(rc)); if (prof) fclose(prof); return 1; }

        char cmp_cmd[1024];
        snprintf(cmp_cmd, sizeof(cmp_cmd), "cmp -s %s %s", out, expected);
        if (system(cmp_cmd) != 0) {
            fprintf(stderr, "[FAIL] %s %s byte mismatch\n", tname, file_names[fi]);

            char sizecmd[1024]; snprintf(sizecmd, sizeof(sizecmd), "wc -c <%s", out);
            FILE* pf = popen(sizecmd, "r"); if (pf) { int sz = 0; fscanf(pf, "%d", &sz); pclose(pf);
                fprintf(stderr, "  downloaded: %d bytes, expected: %d bytes\n", sz, file_sizes[fi]); }
            unlink(out); if (prof) fclose(prof); return 1;
        }
        unlink(out);

        double elapsed_ms = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_nsec - t1.tv_nsec) / 1e6;
        double mbps = (file_sizes[fi] / (1024.0 * 1024.0)) / (elapsed_ms / 1000.0);
        times_ms[fi] = elapsed_ms;
        if (prof) fprintf(prof, "%s %d %.0f %.1f\n", file_names[fi], file_sizes[fi], elapsed_ms, mbps);
    }

    {
        double tot_ms = 0; int tot_bytes = 0;
        for (int fi = 0; fi < num_files; fi++) { tot_ms += times_ms[fi]; tot_bytes += file_sizes[fi]; }
        double avg_mbps = (tot_bytes / (1024.0 * 1024.0)) / (tot_ms / 1000.0);
        if (prof) { fprintf(prof, "TOTAL %d %.0f %.1f\n", tot_bytes, tot_ms, avg_mbps); fclose(prof); prof = NULL; }
    }

    if (wtype == W_HTTP_PROXY) {
        char cmd[2048], out[512], f1[512];
        int seed = (int)getpid() ^ 0x55;

        snprintf(f1, sizeof(f1), "%s/post_%d.bin", tmpdir, getpid());
        gen_file(f1, 256, seed);
        snprintf(out, sizeof(out), "%s/post_out_%d.bin", tmpdir, getpid());
        snprintf(cmd, sizeof(cmd),
                 "curl -s --connect-timeout 10 --max-time 30 -x http://%s --data-binary @%s -o %s %s/echo 2>/dev/null",
                 http_proxy, f1, out, url_http);
        if (system(cmd) != 0) { fprintf(stderr, "[FAIL] %s POST/echo curl\n", tname); unlink(f1); return 1; }
        snprintf(cmd, sizeof(cmd), "cmp -s %s %s", out, f1);
        if (system(cmd) != 0) { fprintf(stderr, "[FAIL] %s POST/echo cmp\n", tname); unlink(f1); unlink(out); return 1; }
        unlink(f1); unlink(out);

        snprintf(cmd, sizeof(cmd),
                 "curl -s -I --connect-timeout 15 --max-time 30 -x http://%s %s/f_1k.bin 2>/dev/null | head -1 | grep -q '200 OK'",
                 http_proxy, url_http);
        if (system(cmd) != 0) { fprintf(stderr, "[FAIL] %s HEAD 200\n", tname); return 1; }

        snprintf(out, sizeof(out), "%s/opt_%d.txt", tmpdir, getpid());
        snprintf(cmd, sizeof(cmd),
                 "curl -s -i --connect-timeout 15 --max-time 30 -x http://%s -X OPTIONS -o %s %s/f_1k.bin 2>/dev/null",
                 http_proxy, out, url_http);
        if (system(cmd) != 0) { fprintf(stderr, "[FAIL] %s OPTIONS curl\n", tname); return 1; }
        snprintf(cmd, sizeof(cmd), "grep -q 'Allow:' %s", out);
        if (system(cmd) != 0) { fprintf(stderr, "[FAIL] %s OPTIONS Allow\n", tname); unlink(out); return 1; }
        unlink(out);
    }

    return 0;
}

static void monitor(void* arg) {
    (void)arg;
    g_mon_id = NULL;
    if (g_done) return;

    if (g_phase == 0) {
        if (conn_ready(g_cli) && conn_ready(g_srv) &&
            etcp_router_peer_reachable(g_cli, TOPO_GROUP_UTUN, g_cli->tcp_proxy_client->via_ed, ETCP_RT_ID_TCP_PROXY_SERVER)) {
            printf("  ETCP connected, launching %d workers...\n", WORKERS); fflush(stdout);
            g_phase = 1;

            char url_http[128];  snprintf(url_http,  sizeof(url_http),  "http://127.0.0.1:%d", g_http_port);
            char url_https[128]; snprintf(url_https, sizeof(url_https), "https://127.0.0.1:%d", g_https_port);
            char socks[64];      snprintf(socks, sizeof(socks), "127.0.0.1:%d", g_socks_port);
            char hproxy[64];     snprintf(hproxy, sizeof(hproxy), "127.0.0.1:%d", g_http_proxy_port);

            // Для socks_domain worker'а: используем localhost вместо 127.0.0.1 чтобы тестировать atyp=3 (DNS через SOCKS)
            char url_http_local[128];
            snprintf(url_http_local, sizeof(url_http_local), "http://localhost:%d", g_http_port);

            struct {
                int type;
                const char* socks;
                const char* hproxy;
                const char* url_h;
                const char* url_s;
            } wcfg[WORKERS] = {
                { W_SOCKS_IPV4,  socks, NULL,   url_http,      NULL },
                { W_SOCKS_DOMAIN,socks, NULL,   url_http_local, NULL },
                { W_HTTP_PROXY,  NULL,  hproxy, url_http,      NULL },
                { W_HTTPS_CONNECT,NULL, hproxy,  NULL,          url_https },
            };

            for (int i = 0; i < WORKERS; i++) {
                pid_t pid = fork();
                if (pid < 0) { printf("[FAIL] fork worker %d: %s\n", i, strerror(errno)); g_done = -1; return; }
                if (pid == 0) {
                    int rc = worker_main(wcfg[i].type, wcfg[i].socks, wcfg[i].hproxy,
                                         wcfg[i].url_h, wcfg[i].url_s, g_tmpdir);
                    _exit(rc);
                }
                g_workers[i] = pid;
                g_workers_orig[i] = pid;
            }
        }
    }

    if (g_phase == 1) {
        int done_count = 0;
        for (int i = 0; i < WORKERS; i++) {
            if (g_workers[i] <= 0) { done_count++; continue; }
            int status; pid_t r = waitpid(g_workers[i], &status, WNOHANG);
            if (r == 0) continue;
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "test worker=%d pid=%d failed: waitpid=%d status=%d errno=%d",
                            i, (int)g_workers[i], (int)r, r > 0 ? status : -1, r < 0 ? errno : 0);
                g_worker_failures++;
            }
            g_workers[i] = 0; done_count++;
        }
        if (done_count >= WORKERS) {
            if (g_worker_failures) { printf("[FAIL] %d/%d workers failed\n", g_worker_failures, WORKERS); g_done = -1; }
            else { g_ok = 1; g_done = 1; }

            printf("\n=== PROFILING ===\n");
            printf("%-14s | %-9s %-9s %-9s %-9s | %-11s\n",
                   "Worker", "1KB", "100KB", "1MB", "10MB", "Avg MB/s");
            printf("%s\n", "-----------------------------------------------------------------");
            for (int i = 0; i < WORKERS; i++) {
                if (g_workers[i] > 0) continue;
                char pp[512]; snprintf(pp, sizeof(pp), "%s/.prof_%d", g_tmpdir, (int)((long)g_workers_orig[i]));
                FILE* prf = fopen(pp, "r");
                if (!prf) continue;
                char tname[64] = "?"; { char buf[128]; if (fgets(buf, sizeof(buf), prf)) sscanf(buf, "%63s", tname); }
                double speeds[4] = {0}; char fn[64];
                for (int fi = 0; fi < num_files; fi++) {
                    double mbps; char buf[128];
                    if (fgets(buf, sizeof(buf), prf)) {
                        int sz2; double em2; int n = sscanf(buf, "%63s %d %lf %lf", fn, &sz2, &em2, &mbps);
                        if (n >= 4) speeds[fi] = mbps;
                    }
                }
                double total_mbps = 0; { char buf[128]; while (fgets(buf, sizeof(buf), prf)) {
                    int tb; double tm, tm2; if (sscanf(buf, "TOTAL %d %lf %lf", &tb, &tm, &tm2) == 3) { total_mbps = tm2; break; }
                }}
                fclose(prf);
                printf("%-14s | %5.1f MB/s %5.1f MB/s %5.1f MB/s %5.1f MB/s | %5.1f MB/s\n",
                       tname, speeds[0], speeds[1], speeds[2], speeds[3], total_mbps);
            }
            printf("\n"); fflush(stdout);
        }
    }

    if (!g_done) g_mon_id = uasync_set_timeout(g_ua, 1000, NULL, monitor, "mon");
}

static void test_timeout(void* arg) {
    (void)arg;
    if (!g_done) { printf("[FAIL] timeout (%ds)\n", TIMEOUT_MS/1000); g_done = -1; }
}

// Нагрузка замещает каждый завершившийся поток сразу; ядро продолжает poll в основном потоке.
static int rolling_load(struct tcp_proxy_client* proxy) {
    int seconds = 10;
    const char* duration = getenv("UTUN_PROXY_LOAD_SECONDS");
    if (duration) {
        char* end;
        long value = strtol(duration, &end, 10);
        if (*end || value < 1 || value > 300) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "invalid UTUN_PROXY_LOAD_SECONDS"); return -1; }
        seconds = (int)value;
    }
    char socks_port[8], http_port[8], time_arg[8];
    snprintf(socks_port, sizeof(socks_port), "%d", g_socks_port);
    snprintf(http_port, sizeof(http_port), "%d", g_http_proxy_port);
    snprintf(time_arg, sizeof(time_arg), "%d", seconds);
    pid_t child = fork();
    if (child < 0) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "rolling load fork: %s", strerror(errno)); return -1; }
    if (child == 0) {
        execlp("python3", "python3", PROXY_LOAD_SCRIPT, socks_port, http_port, time_arg, (char*)NULL);
        _exit(127);
    }

    unsigned peak_opening = 0;
    int peak_streams = 0, peak_queue = 0, status = 0;
    size_t baseline = u_get_allocated_count() - memory_pool_get_total_free_blocks(), peak_blocks = baseline;
    uint64_t deadline = get_time_tb() + (2 * seconds + 45) * 10000ULL;
    pid_t done;
    for (;;) {
        do { done = waitpid(child, &status, WNOHANG); } while (done < 0 && errno == EINTR);
        if (done != 0 || get_time_tb() >= deadline) break;
        uasync_poll(g_ua, 20);
        int streams = proxy->socks_conn_count + proxy->http_conn_count;
        struct ETCP_ROUTER_CONN* route = etcp_router_conn_lookup(g_cli, TOPO_GROUP_UTUN, proxy->via_ed, ETCP_RT_ID_TCP_PROXY_SERVER);
        int queued = route ? queue_entry_count(route->send_q) : 0;
        size_t blocks = u_get_allocated_count() - memory_pool_get_total_free_blocks();
        if (streams > peak_streams) peak_streams = streams;
        if (queued > peak_queue) peak_queue = queued;
        if (proxy->opening > peak_opening) peak_opening = proxy->opening;
        if (blocks > peak_blocks) peak_blocks = blocks;
    }
    if (done == 0) { kill(child, SIGKILL); waitpid(child, NULL, 0); }
    if (done != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "rolling load failed pid=%ld result=%ld status=%d", (long)child, (long)done, status);
        return -1;
    }
    deadline = get_time_tb() + 50000;
    while ((proxy->socks_conn_count || proxy->http_conn_count || proxy->terminal_count || g_srv->tcp_proxy_server.conn_count) &&
           get_time_tb() < deadline) uasync_poll(g_ua, 20);
    size_t after = u_get_allocated_count() - memory_pool_get_total_free_blocks();
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "rolling load: peak streams=%d opening=%u send_q=%d live blocks=%zu/%zu/%zu; remaining=%d/%d/%u/%d",
               peak_streams, peak_opening, peak_queue, baseline, peak_blocks, after,
               proxy->socks_conn_count, proxy->http_conn_count, proxy->terminal_count, g_srv->tcp_proxy_server.conn_count);
    if (peak_streams < TCP_PROXY_CLIENT_OPEN_LIMIT || peak_opening > TCP_PROXY_CLIENT_OPEN_LIMIT ||
        peak_queue > ROUTER_MAX_SEND_Q_PACKETS + TCP_PROXY_CLIENT_CONTROL_RESERVE ||
        proxy->socks_conn_count || proxy->http_conn_count || proxy->opening || proxy->terminal_count || g_srv->tcp_proxy_server.conn_count) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "rolling load admission, queue or cleanup invariant failed"); return -1;
    }
    return 0;
}

#endif /* !_WIN32 */

int main(void) {
    printf("=== test_socks_http_proxy ===\n"); fflush(stdout);
#ifdef _WIN32
    printf("[SKIP] test_socks_http_proxy — fork() not available on Windows\n");
    return 0;
#else
    { int fd = open("/dev/null", O_RDONLY); if (fd > 0 && fd != 0) { dup2(fd, 0); close(fd); } else if (fd < 0) { close(0); open("/dev/null", O_RDONLY); } }

    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_categories(DEBUG_CATEGORY_ALL);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));

    strcpy(g_tmpdir, "/tmp/utun_test_XXXXXX");
    if (test_mkdtemp(g_tmpdir) < 0) { printf("[FAIL] mkdtemp: %s\n", strerror(errno)); return 1; }

    if (gen_self_signed_cert(g_tmpdir, g_cert, sizeof(g_cert), g_key, sizeof(g_key)) < 0) goto cleanup;

    g_file_seed = (int)time(NULL) ^ (int)getpid();
    int total_mb = 0;
    for (int fi = 0; fi < num_files; fi++) {
        char path[512]; snprintf(path, sizeof(path), "%s/%s", g_tmpdir, file_names[fi]);
        gen_file(path, file_sizes[fi], g_file_seed + fi);
        total_mb += file_sizes[fi];
    }
    printf("  test files: 1KB, 100KB, 1MB, 10MB (total %.1fMB)\n", total_mb / (1024.0*1024.0)); fflush(stdout);

    if (start_http_server(&g_http_port, g_tmpdir) < 0) { printf("[FAIL] start http server\n"); goto cleanup; }
    if (start_https_server(&g_https_port, g_tmpdir, g_cert, g_key) < 0) { printf("[FAIL] start https server\n"); goto cleanup; }
    printf("  HTTP/HTTPS servers started\n");

    g_ua = uasync_create();
    if (!g_ua) { printf("[FAIL] uasync_create\n"); goto cleanup; }

    g_srv = utun_instance_create_from_str(g_ua, make_cfg_server());
    g_cli = utun_instance_create_from_str(g_ua, make_cfg_client());
    if (!g_srv || !g_cli) { printf("[FAIL] instance create\n"); goto cleanup; }
    test_etcp_set_peer(g_cli, "c1", g_srv, "s1");
    g_srv_port = ss_get_port(&test_etcp_bound_socket(g_srv, "s1")->local_addr);
    g_cli_port = ss_get_port(&test_etcp_bound_socket(g_cli, "s1")->local_addr);
    struct tcp_proxy_client* proxy = g_cli->tcp_proxy_client;
    if (!proxy) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "test: proxy client missing"); goto cleanup; }
    g_socks_port = socks_proxy_get_port(proxy->socks_listen);
    g_http_proxy_port = socks_proxy_get_port(proxy->http_listen);
    if (!g_socks_port || !g_http_proxy_port) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "test: proxy listeners missing"); goto cleanup; }
    printf("  ports: http=%d https=%d socks=%d http_proxy=%d udp_client=%d udp_server=%d\n",
           g_http_port, g_https_port, g_socks_port, g_http_proxy_port, g_cli_port, g_srv_port);

    if (utun_instance_init(g_srv) < 0 || utun_instance_init(g_cli) < 0) { printf("[FAIL] instance init\n"); goto cleanup; }
    printf("  waiting for ETCP...\n"); fflush(stdout);

    g_mon_id = uasync_set_timeout(g_ua, 500, NULL, monitor, "mon");
    void* to_id = uasync_set_timeout(g_ua, TIMEOUT_MS * 10, NULL, test_timeout, "to");

    while (!g_done) uasync_poll(g_ua, 50);

    if (to_id) uasync_cancel_timeout(g_ua, to_id);
    if (g_ok && rolling_load(proxy) < 0) g_ok = 0;

cleanup:
    for (int i = 0; i < WORKERS; i++) if (g_workers[i] > 0) { kill(g_workers[i], SIGKILL); waitpid(g_workers[i], NULL, 0); }
    if (g_mon_id) uasync_cancel_timeout(g_ua, g_mon_id);
    if (g_http_pid > 0) { kill(g_http_pid, SIGKILL); waitpid(g_http_pid, NULL, 0); }
    if (g_https_pid > 0) { kill(g_https_pid, SIGKILL); waitpid(g_https_pid, NULL, 0); }
    if (g_cli) { g_cli->running = 0; utun_instance_destroy(g_cli); }
    if (g_srv) { g_srv->running = 0; utun_instance_destroy(g_srv); }
    if (g_ua) uasync_destroy(g_ua, 0);
    if (g_ok && u_get_allocated_count()) {
        u_report_unfreed_blocks(); g_ok = 0;
    }
    if (g_ok) printf("[PASS] test_socks_http_proxy — %d downloads checked, 0 tracked blocks after shutdown\n", WORKERS * num_files);
    else printf("[FAIL] test_socks_http_proxy\n");
    { char buf[512]; snprintf(buf, sizeof(buf), "rm -rf %s", g_tmpdir); system(buf); }
    return g_ok ? 0 : 1;
#endif
}
