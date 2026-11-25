// call_ring.c — статический кольцевой трейс вызовов (диагностика UAF в teardown)
#include "call_ring.h"
#include "../lib/platform_compat.h"
#include <inttypes.h>
#include <stdio.h>
#include <errno.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#endif

__thread callring_entry_t g_callring[CALLRING_SIZE];
__thread uint32_t g_callring_idx = 0;
__thread uint32_t g_callring_count = 0;

/* Никакого logger/malloc: путь вызывается из аварийного обработчика. */
static void cr_write(int* fd, const char* data, size_t len) {
    while (len) {
        ssize_t n = write(*fd, data, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (*fd == 2) return;
            close(*fd); *fd = 2;
            static const char error[] = "callring: dump write failed, continuing on stderr\n";
            ssize_t ignored = write(2, error, sizeof(error) - 1); (void)ignored;
            continue;
        }
        data += n; len -= (size_t)n;
    }
}

static const char *cr_name(uint32_t code) {
    switch (code) {
    case CR_LINK_NEW:        return "LINK_NEW";
    case CR_LINK_CLOSE_IN:   return "LINK_CLOSE_IN";
    case CR_LINK_CLOSE_TCP:  return "LINK_CLOSE_TCP";
    case CR_LINK_CLOSE_FREE: return "LINK_CLOSE_FREE";
    case CR_TCPCB_IN:        return "TCPCB_IN";
    case CR_TCPCB_FREE:      return "TCPCB_FREE";
    case CR_SRV_ONLINK:      return "SRV_ONLINK";
    case CR_SLINK_CLOSE:     return "SLINK_CLOSE";
    case CR_SLINK_IMPL:      return "SLINK_IMPL";
    case CR_SLINK_ONSTCP:    return "SLINK_ONSTCP";
    case CR_CONN_SETONCLOSE: return "CONN_SETONCLOSE";
    case CR_CONN_DOCLOSE_IN: return "CONN_DOCLOSE_IN";
    case CR_CONN_DOCLOSE_CB: return "CONN_DOCLOSE_CB";
    case CR_CONN_FREE:       return "CONN_FREE";
    case CR_CCLOSE_IN:       return "CCLOSE_IN";
    case CR_CFREE_RES:       return "CFREE_RES";
    case CR_NCD_DEFER:       return "NCD_DEFER";
    default:                 return "?";
    }
}

/* Дамп последних записей кольцевого буфера в /tmp/utun_callring.<pid> (и stderr
 * при ошибке). Форматирование только через snprintf в стековый буфер — в
 * обработчике сигнала не выделяет память. */
void callring_dump(void) {
    char buf[192];
    int fd = -1;

#if defined(_WIN32)
    fd = 2;
#else
    {
        char path[96];
        snprintf(path, sizeof(path), "/tmp/utun_callring.%d", (int)getpid());
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd < 0) {
        fd = 2;
        static const char error[] = "callring: cannot open dump file, using stderr\n";
        ssize_t ignored = write(2, error, sizeof(error) - 1); (void)ignored;
    }
#endif

    int n = snprintf(buf, sizeof(buf), "callring dump idx=%u\n", g_callring_idx);
    if (n > 0 && (size_t)n < sizeof(buf)) cr_write(&fd, buf, (size_t)n);

    uint32_t total = g_callring_count;
    uint32_t start = g_callring_idx - total;
    for (uint32_t k = 0; k < total; k++) {
        const callring_entry_t *e = &g_callring[(start + k) % CALLRING_SIZE];
        n = snprintf(buf, sizeof(buf), "%s a=0x%" PRIxPTR " b=0x%" PRIxPTR "\n", cr_name(e->code), e->a, e->b);
        if (n > 0 && (size_t)n < sizeof(buf)) cr_write(&fd, buf, (size_t)n);
    }

#if !defined(_WIN32)
    if (fd != 2) close(fd);
#endif
}
