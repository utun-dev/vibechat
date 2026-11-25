/*
 * headless_main.c — headless CLI entry point for chatgui-android
 *
 * Runs the uTun core without UI. Opens a JSON control socket for management.
 * Used for debugging the network stack on Linux before Android integration.
 *
 * Usage:
 *   ./headless -p <control_port> -l <logfile>
 *
 * Config is read via callbacks (set by utun_config_set_provider before init).
 * For headless, a stub config provider reads from environment variables.
 */
#include "../libutun_lite/utun_config_api.h"
#include "headless_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* ── Stub config provider (reads from ENV) ── */

static const char* env_get_string(const char* key, const char* def) {
    const char* val = getenv(key);
    return val ? val : def;
}

static int env_get_int(const char* key, int def) {
    const char* val = getenv(key);
    return val ? atoi(val) : def;
}

static const utun_config_provider_t g_env_provider = {
    .get_string = env_get_string,
    .get_int64  = NULL,
    .get_int    = env_get_int,
    .get_bool   = NULL,
};

/* ── Signal handling ── */

static volatile int g_stop = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ─── main ─── */

int main(int argc, char** argv) {
    int control_port = 9999;
    const char* bind_ip = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) control_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) fprintf(stderr, "logfile: %s\n", argv[++i]);
    }

    utun_config_set_provider(&g_env_provider);

    /* Init control socket (non-uasync for now — simple select-based poll) */
    if (headless_control_init(NULL, bind_ip, control_port) < 0) {
        fprintf(stderr, "headless: failed to init control socket\n");
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("headless: running. control socket on %s:%d\n", bind_ip, control_port);
    printf("headless: connect with: nc %s %d\n", bind_ip, control_port);

    /* Main loop */
    while (!g_stop) {
        headless_control_poll();
        usleep(10000); /* 10ms to avoid busy-loop */
    }

    headless_control_destroy();
    printf("headless: stopped.\n");
    return 0;
}
