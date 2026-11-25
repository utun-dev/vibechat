// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
// Исключение для Wintun описано в LICENSING в корне проекта.
// utun.c - Main application for utun VPN tunnel
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "config_parser.h"
#include "config_updater.h"
#include "etcp_connections.h"
#include "transport_layer/call_ring.h"
#include "tun_if.h"
#include "secure_channel.h"
#include "route_lib.h"
#include "utun_instance.h"
#include "db_schema.h"
#include "chat/invite_build.h"
#include "chat/invite_link.h"
#include "chat/chat_core.h"
#include "kanban/kanban.h"
#include "u_async.h"
#include "debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>

#include <getopt.h>
#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include "../lib/mem.h"

#ifdef _WIN32
#include <windows.h>
#define SECURITY_WIN32
#include <security.h>
#include <sddl.h>
#endif
/*

Архитектура:
main -> init utun instnaces -> mainloop()

utun instances: можно stop() - он ывзывает закрытие всех etcp (etcp_close) которые вызовут закрытие подключений), вызывает закрытие всех etcp сокетов



*/



// Global wakeup pipe write fd for signal handler
static int g_wakeup_pipe_write_fd = -1;

struct startup_result { int ready; struct db_async_result result; };
static void startup_done(void* data, const struct db_async_result* result) {
    struct startup_result* state = data; state->result = *result; state->ready = 1;
}
static void core_ready(void* data, struct UTUN_INSTANCE* inst, const struct db_async_result* result) {
    (void)inst; startup_done(data, result);
}
/* Только внешний main startup, вне callbacks: продолжаем poll, пока отдельный SQL worker готовит сервис. */
static int startup_wait(struct UASYNC* ua, struct startup_result* state) {
    while (!state->ready) uasync_poll(ua, 50);
    if (state->result.status) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "service startup failed status=%d rc=%d: %s",
        state->result.status, state->result.sqlite_code, state->result.message);
    return state->result.status ? -1 : 0;
}
static void server_link_ready(void* data, int result, const char* link) {
    (void)data;
    if (!result && link) fprintf(stderr, "Chatserver link: %s\n", link);
    else DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "cannot build configured chatserver link result=%d", result);
}

#define DEFAULT_CONFIG "utun.conf"
#define DEFAULT_PIDFILE "/var/run/utun.pid"

// Command line arguments
typedef struct {
    char *config_file;
    char *pid_file;
    char *log_file;
    char *debug_config;
    int foreground;
    int help;
} cmd_args_t;

// Global state





// Extract destination IPv4 address from packet
static uint32_t get_dest_ip(const uint8_t *packet, size_t len) {
    if (len < 20) return 0; // Minimum IPv4 header size
    // Check IP version (first nibble)
    uint8_t version = (packet[0] >> 4) & 0x0F;
    if (version != 4) return 0;
    // Destination IP is at offset 16
    uint32_t dest_ip;
    memcpy(&dest_ip, packet + 16, 4);
    return dest_ip;
}


// Parse command line arguments
static void parse_args(int argc, char *argv[], cmd_args_t *args) {
    memset(args, 0, sizeof(*args));
    args->config_file = DEFAULT_CONFIG;
#ifdef _WIN32
    args->pid_file = NULL;  // Windows: no default PID file
#else
    args->pid_file = DEFAULT_PIDFILE;
#endif
    args->log_file = NULL;
    args->debug_config = NULL;
    args->foreground = 0;
    args->help = 0;
    
    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"pidfile", required_argument, 0, 'p'},
        {"log", required_argument, 0, 'l'},
        {"debug", required_argument, 0, 'd'},
        {"foreground", no_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "c:p:l:d:fh", 
                              long_options, &option_index)) != -1) {
        switch (opt) {
            case 'c':
                args->config_file = optarg;
                break;
            case 'p':
                args->pid_file = optarg;
                break;
            case 'l':
                args->log_file = optarg;
                break;
            case 'f':
                args->foreground = 1;
                break;
            case 'd':
                args->debug_config = optarg;
                break;
            case 'h':
                args->help = 1;
                break;
            default:
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Unknown option: %c", opt);
                exit(1);
        }
    }
}

// Print usage
static void print_usage(const char *progname) {
    printf("Usage: %s [OPTIONS]\n", progname);
    printf("Secure VPN tunnel over UDP with TUN interface\n\n");
    printf("Options:\n");
    printf("  -c, --config FILE    Configuration file (default: %s)\n", DEFAULT_CONFIG);
#ifdef _WIN32
    printf("  -p, --pidfile FILE   PID file (default: none, use -p to specify)\n");
#else
    printf("  -p, --pidfile FILE   PID file (default: %s)\n", DEFAULT_PIDFILE);
#endif
    printf("  -l, --log FILE       Log file (default: utun.log)\n");
    printf("  -d, --debug CONFIG   Debug configuration (e.g., \"etcp:debug,routing:info\")\n");
    printf("  -f, --foreground     Run in foreground (don't daemonize)\n");
    printf("  -h, --help           Show this help\n");
    printf("\nExamples:\n");
    printf("  %s -c myconfig.conf\n", progname);
    printf("  %s --config server.conf\n", progname);
}

// Write PID file
static int write_pidfile(const char *pidfile) {
    if (!pidfile) return -1;
    
    FILE *fp = fopen(pidfile, "w");
    if (!fp) {
        perror("fopen pidfile");
        return -1;
    }
    
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    return 0;
}

// Remove PID file
static void remove_pidfile(const char *pidfile) {
    if (pidfile) {
        unlink(pidfile);
    }
}

// Daemonize process
static int daemonize(int* ready_fd) {
#ifdef _WIN32
    // Windows doesn't support fork/setsid, just return success
    (void)ready_fd;
    return 0;
#else
    int notify[2];
    if (pipe(notify) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Daemon startup pipe failed: %s", strerror(errno)); return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        int error = errno; close(notify[0]); close(notify[1]);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Daemon fork failed: %s", strerror(error));
        return -1;
    }
    
    if (pid > 0) {
        /* Родитель сообщает успех только после инициализации дочернего процесса. */
        close(notify[1]);
        uint8_t ready = 0; ssize_t count;
        do { count = read(notify[0], &ready, sizeof(ready)); } while (count < 0 && errno == EINTR);
        close(notify[0]);
        if (count != 1 || ready != 1) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Daemon initialization failed; see log file");
        exit(count == 1 && ready == 1 ? 0 : 1);
    }
    close(notify[0]); *ready_fd = notify[1];
    
    // Child becomes session leader
    if (setsid() < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Daemon setsid failed: %s", strerror(errno));
        close(*ready_fd); *ready_fd = -1;
        return -1;
    }

    return 0;
#endif
}

/* После вывода startup-информации, но до запуска потоков сервисов. */
static int daemon_redirect_stdio(void) {
#ifdef _WIN32
    return 0;
#else
    fflush(stdout); fflush(stderr);
    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // Redirect to /dev/null
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        if (dup2(fd, STDIN_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to redirect file descriptors to /dev/null");
            close(fd);
            return -1;
        }
        if (fd > 2) close(fd);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to open /dev/null: %s", strerror(errno));
        return -1;
    }
    
    return 0;
#endif
}

// Open log file
static FILE* open_logfile(const char *logfile) {
    if (!logfile) return stderr;
    
    FILE *fp = fopen(logfile, "a");
    if (!fp) {
        perror("fopen logfile");
        return stderr;
    }
    
    setvbuf(fp, NULL, _IOLBF, 0);
    return fp;
}


// Main function

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_reload = 0;
struct UASYNC* main_ua = NULL;

static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_shutdown = 1;
    } 
#ifndef _WIN32
    else if (sig == SIGHUP) {
        g_reload = 1;
    }
#endif
    if (main_ua) {
        uasync_wakeup(main_ua);
    }
}

/* Дамп кольцевого трейса вызовов при краше, затем переброс сигнала по умолчанию
 * (чтобы остался core dump). Форматирование в callring_dump не выделяет память. */
#ifndef _WIN32
static void crash_handler(int sig) {
    callring_dump();
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

void test_tmr(void* arg) {
  struct UASYNC* ua = (struct UASYNC*)arg;
  uasync_set_timeout(ua, 10000, ua, test_tmr, "utun_test");
//  DEBUG_INFO(DEBUG_CATEGORY_ETCP, "tick ...");
  }

int main(int argc, char *argv[]) {
    cmd_args_t args;
    int ready_fd = -1;
    parse_args(argc, argv, &args);
    
    if (args.help) {
        print_usage(argv[0]);
        return 0;
    }

//    socket_platform_init();
    
    // Initialize global debug system early if configured from command line

    debug_config_init();
//    debug_set_level(DEBUG_LEVEL_TRACE);
    debug_set_level(DEBUG_LEVEL_ERROR);
//    debug_set_categories(DEBUG_CATEGORY_ALL & ~DEBUG_CATEGORY_SYS & ~DEBUG_CATEGORY_TIMERS & ~DEBUG_CATEGORY_CRYPTO & ~DEBUG_CATEGORY_ETCP & ~DEBUG_CATEGORY_CONNECTION);  // Enable all except uasync
//    debug_set_categories(DEBUG_CATEGORY_ALL & ~DEBUG_CATEGORY_SYS & ~DEBUG_CATEGORY_TIMERS);// 
    debug_enable_function_name(1);

    if (args.debug_config) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "Apply debug CLI args");
        debug_parse_config(args.debug_config);
    }
    else {
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "No debug CLI args - set all");
    }
    
    // Enable file logging (default: utun.log, truncate on start)
    const char* log_file = args.log_file ? args.log_file : "utun.log";
    debug_enable_file_output(log_file, 1);

#ifdef _WIN32
    // Check for administrator privileges
    {
        BOOL is_admin = FALSE;
        PSID administrators_group = NULL;
        SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
        
        if (AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                       DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                       &administrators_group)) {
            CheckTokenMembership(NULL, administrators_group, &is_admin);
            FreeSid(administrators_group);
        }
        
        if (!is_admin) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Administrator privileges required. Please run utun.exe as Administrator");
            return 1;
        }
    }
#endif

    /* Fork до event loop, SQLite, сокетов и workers: фоновые потоки в ребёнка не переходят. */
    if (!args.foreground && daemonize(&ready_fd) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Failed to daemonize before core initialization"); return 1;
    }

    srand(time(NULL));
    // Create uasync instance
    struct UASYNC* ua = uasync_create();
    main_ua=ua;
    if (!ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Failed to create uasync instance");
        return 1;
    }

//    uasync_set_timeout(ua, 10000, ua, test_tmr);

#ifdef _WIN32
    // Check for wintun.dll before enabling TUN
    if (access("wintun.dll", F_OK) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "wintun.dll not found. Please ensure wintun.dll is in the same directory as utun.exe");
        uasync_destroy(ua, 0);
        return 1;
    }
#endif

    // Игнорировать SIGPIPE: send() в закрытый TCP-сокет (reality-relay/STCP под сканерами)
    // не должен молча убивать демон — ошибки EPIPE обрабатываются по errno в коде.
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    /* Дампить кольцевой трейс при краше (до core dump). */
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS,  crash_handler);
#endif

    // Enable TUN initialization for VPN functionality
    utun_instance_set_tun_init_enabled(1);

    // Create and initialize instance
    struct utun_config* config = config_ensure_keys(args.config_file) < 0 ? NULL : parse_config(args.config_file);
    struct startup_result database = {0};
    struct UTUN_INSTANCE* instance = config ? utun_instance_create_from_config_async(ua, config, core_ready, &database) : NULL;
    if (!instance) {
        free_config(config);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Failed to create UTUN instance");
        if (*db_schema_last_error()) fprintf(stderr, "%s\n", db_schema_last_error());
        uasync_destroy(ua, 0); main_ua = NULL;
        return 1;
    }
    if (startup_wait(ua, &database)) {
        if (*instance->database_error) fprintf(stderr, "%s\n", instance->database_error);
        utun_instance_destroy(instance); uasync_destroy(ua, 0); main_ua = NULL; return 1;
    }
    
    // Print config for debugging
    if (args.foreground) {
        print_config(instance->config);
    }
    if (!args.foreground && daemon_redirect_stdio() < 0) {
        utun_instance_destroy(instance); uasync_destroy(ua, 0); main_ua = NULL;
        return 1;
    }
    
    // Initialize all components (TUN, routing, connections)
    int start_failed = utun_core_start(instance) < 0 || (instance->topo_groups && utun_service_start(instance) < 0);
    if (!start_failed && instance->config->global.chatserver_enabled) {
        struct startup_result chat = {0}; chat_service_start_async(instance, startup_done, &chat);
        start_failed = startup_wait(ua, &chat) < 0 || kanban_service_start(instance) < 0;
    }
    if (start_failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Failed to initialize instance");
        utun_instance_destroy(instance);
        uasync_destroy(ua, 0); main_ua = NULL;
        return 1;
    }
    if (args.foreground && instance->config->global.chatserver_enabled && instance->config->global.chatserver_has_add_key) {
        if (!chat_server_link_start(instance, server_link_ready, NULL))
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "cannot queue configured chatserver link");
    }
    
    // Note: TUN socket is registered in tun_init(), ETCP sockets in init_connections()
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifndef _WIN32
    signal(SIGHUP, signal_handler);
#endif
    
    // Write PID file (only if specified)
    if (args.pid_file && write_pidfile(args.pid_file) < 0) {
        utun_instance_destroy(instance);
        uasync_destroy(ua, 0); main_ua = NULL;
        return 1;
    }

#ifndef _WIN32
    if (ready_fd >= 0) {
        uint8_t ready = 1; ssize_t count;
        do { count = write(ready_fd, &ready, sizeof(ready)); } while (count < 0 && errno == EINTR);
        int error = errno;
        close(ready_fd); ready_fd = -1;
        if (count != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Failed to notify daemon launcher: %s", strerror(error));
            utun_instance_destroy(instance); uasync_destroy(ua, 0); main_ua = NULL;
            remove_pidfile(args.pid_file); return 1;
        }
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "Daemon initialized pid=%ld; core and workers created after fork", (long)getpid());
    }
#endif

    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Run mainloop");
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Run mainloop");

    while (instance && instance->running) {
        uasync_poll(ua, 100);
        
        if (g_shutdown) {
            instance->running = 0;
            break;
        }
        
        if (g_reload) {
            instance = utun_instance_reload(instance, ua, args.config_file);
            if (!instance) {
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Reload failed");
                break;
            }
            g_reload = 0;
            DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Config reloaded successfully (sockets/clients/links compared)");
        }
    }

    // Cleanup
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Shutdown");
    if (instance) {
        utun_instance_destroy(instance);
    }

    // Destroy uasync instance after instance is destroyed
    if (ua) {
        uasync_destroy(ua, 0);
    }

    u_report_unfreed_blocks();

    remove_pidfile(args.pid_file);
    socket_platform_cleanup();
    return 0;
}
