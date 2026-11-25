/* TLS-изоляция, полный ring после uint32 wrap и надёжная запись аварийного дампа. */
#include "../src/transport_layer/call_ring.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int fault_write;
extern ssize_t __real_write(int, const void*, size_t);
ssize_t __wrap_write(int fd, const void* data, size_t length) {
    if (fault_write == 1) { fault_write = 2; errno = EINTR; return -1; }
    if (fault_write == 2 && length > 3) length = 3;
    if (fault_write == 3 && fd != 2) { fault_write = 0; errno = EIO; return -1; }
    return __real_write(fd, data, length);
}

static void* worker(void* arg) {
    uintptr_t seed = (uintptr_t)arg;
    assert(!g_callring_idx && !g_callring_count);
    for (unsigned i = 0; i < 50000; i++) callring_put(CR_LINK_NEW, seed, i);
    assert(g_callring_idx == 50000 && g_callring_count == CALLRING_SIZE);
    for (unsigned i = 0; i < CALLRING_SIZE; i++) assert(g_callring[i].a == seed);
    return NULL;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    callring_put(CR_LINK_NEW, 42, 43);
    pthread_t first, second;
    assert(!pthread_create(&first, NULL, worker, (void*)(uintptr_t)1));
    assert(!pthread_create(&second, NULL, worker, (void*)(uintptr_t)2));
    assert(!pthread_join(first, NULL) && !pthread_join(second, NULL));
    assert(g_callring_idx == 1 && g_callring_count == 1 && g_callring[0].a == 42);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS call ring: independent state for two writers and owner thread");

    g_callring_idx = UINT32_MAX - 7; g_callring_count = 0;
    for (unsigned i = 0; i < 2048; i++) callring_put(CR_LINK_NEW, i, UINTPTR_MAX);
    fault_write = 1; callring_dump(); fault_write = 0;
    char path[96]; snprintf(path, sizeof(path), "/tmp/utun_callring.%d", (int)getpid());
    FILE* file = fopen(path, "r"); assert(file);
    char line[192], expected[192];
    assert(fgets(line, sizeof(line), file));
    snprintf(expected, sizeof(expected), "callring dump idx=%u\n", g_callring_idx);
    assert(!strcmp(line, expected));
    for (unsigned i = 1024; i < 2048; i++) {
        snprintf(expected, sizeof(expected), "LINK_NEW a=0x%x b=0x%" PRIxPTR "\n", i, UINTPTR_MAX);
        assert(fgets(line, sizeof(line), file) && !strcmp(line, expected));
    }
    assert(!fgets(line, sizeof(line), file)); assert(!fclose(file));
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS call ring: last 1024 records after counter wrap, full pointer width, EINTR and short writes");

    FILE* fallback = tmpfile(); assert(fallback);
    int original = dup(2); assert(original >= 0 && dup2(fileno(fallback), 2) == 2);
    fault_write = 3; callring_dump();
    assert(dup2(original, 2) == 2); close(original);
    rewind(fallback);
    assert(fgets(line, sizeof(line), fallback) && strstr(line, "dump write failed"));
    assert(fgets(line, sizeof(line), fallback) && strstr(line, "callring dump idx="));
    for (unsigned i = 0; i < CALLRING_SIZE; i++) assert(fgets(line, sizeof(line), fallback));
    assert(!fgets(line, sizeof(line), fallback)); fclose(fallback);
    assert(!unlink(path));
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS call ring: failed dump file continues on stderr");
    return 0;
}
