/* SIGPIPE остаётся с действием по умолчанию: оба пути STCP обязаны вернуть ошибку закрытого пира. */
#include "stcp.h"
#include "../lib/platform_compat.h"
#include "../lib/mem.h"
#include <signal.h>
#include <sys/wait.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL %s:%d: %s", __func__, __LINE__, #x); exit(1); } } while (0)

static void check_closed_peer(int buffered) {
    int sockets[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(socket_close_wrapper(sockets[1]) == 0);
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct stcp_conn conn = {.ua = ua, .sock = sockets[0], .state = STCP_STATE_DATA};
    uint8_t* bytes = u_malloc(16); CHECK(bytes); memset(bytes, 0, 16);
    if (buffered) {
        conn.send_buf = bytes; conn.send_len = 16;
        stcp_write_cb(conn.sock, &conn);
        CHECK(conn.close_err == EPIPE && conn.close_call_soon);
        uasync_drain_immediate(ua);
        CHECK(conn.state == STCP_STATE_CLOSED && !conn.send_buf);
    } else {
        CHECK(stcp_send_raw(&conn, bytes, 16) < 0 && socket_get_error() == EPIPE);
        u_free(bytes); stcp_conn_do_close(&conn, EPIPE);
    }
    uasync_poll(ua, 0); CHECK(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    for (int buffered = 0; buffered < 2; buffered++) {
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) {
            CHECK(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
            size_t baseline = u_get_allocated_count();
            check_closed_peer(buffered); CHECK(u_get_allocated_count() == baseline);
            exit(0);
        }
        int status; CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    return 0;
}
