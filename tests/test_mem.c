/* Проверяем размеры реальных malloc и OOM без выделения гигабайт памяти. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_config.h"
#include "mem.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static bool fail_malloc;
static size_t malloc_calls;
static size_t malloc_size;

void* __real_malloc(size_t size);

void* __wrap_malloc(size_t size) {
    malloc_calls++;
    malloc_size = size;
    if (fail_malloc) {
        fail_malloc = false;
        return NULL;
    }
    return __real_malloc(size);
}

static void test_small_allocations(void) {
    const uint32_t sizes[] = {0, 1, 3, 4, 255, 256, 257, 1024};
    size_t count = u_get_allocated_count();
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        uint8_t* ptr = u_malloc(sizes[i]);
        CHECK(ptr != NULL);
        CHECK(u_get_allocated_count() == count + 1);
        memset(ptr, 0x5a, sizes[i]);
        u_check(ptr, "small allocation", LOCATION);
        u_free(ptr);
        CHECK(u_get_allocated_count() == count);
    }
    puts("PASS: zero, odd and ordinary allocation sizes");
}

static void test_oom(void) {
    size_t count = u_get_allocated_count();
    fail_malloc = true;
    CHECK(u_malloc(0) == NULL);
    CHECK(!fail_malloc);
    fail_malloc = true;
    CHECK(u_calloc(0, UINT32_MAX) == NULL);
    CHECK(!fail_malloc);
    fail_malloc = true;
    CHECK(u_calloc(UINT32_MAX, 0) == NULL);
    CHECK(!fail_malloc);
    fail_malloc = true;
    CHECK(u_realloc(NULL, 0) == NULL);
    CHECK(!fail_malloc);
    fail_malloc = true;
    CHECK(u_malloc(17) == NULL);
    CHECK(!fail_malloc);
    CHECK(u_get_allocated_count() == count);

    char* ptr = u_strdup("preserve on OOM");
    CHECK(ptr != NULL);
    fail_malloc = true;
    CHECK(u_realloc(ptr, 128) == NULL);
    CHECK(!fail_malloc);
    CHECK(strcmp(ptr, "preserve on OOM") == 0);
    CHECK(u_get_allocated_count() == count + 1);
    u_free(ptr);
    CHECK(u_get_allocated_count() == count);
    puts("PASS: OOM for zero/nonzero sizes and realloc preserves its old block");
}

static void test_large_sizes(void) {
    /* Измеряем служебный размер по запросу malloc для пустого блока. */
    void* empty = u_malloc(0);
    CHECK(empty != NULL);
    size_t overhead = malloc_size;
    u_free(empty);
    CHECK(overhead > 0 && overhead <= 1024);
    const uint32_t sizes[] = {
        UINT32_MAX - 1024, UINT32_MAX - (uint32_t)overhead, UINT32_MAX - (uint32_t)overhead + 1, UINT32_MAX - 1, UINT32_MAX
    };
    size_t count = u_get_allocated_count();
    char* old = u_strdup("preserve on large OOM");
    CHECK(old != NULL);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        for (int api = 0; api < 3; api++) {
            size_t calls = malloc_calls;
            fail_malloc = true;
            void* ptr;
            if (api == 0) ptr = u_malloc(sizes[i]);
            else if (api == 1) ptr = u_calloc(sizes[i], 1);
            else ptr = u_realloc(old, sizes[i]);
            CHECK(ptr == NULL);
            if ((uint64_t)sizes[i] + overhead > SIZE_MAX) {
                CHECK(malloc_calls == calls);
                CHECK(fail_malloc);
                fail_malloc = false;
            } else {
                CHECK(malloc_calls == calls + 1);
                CHECK(!fail_malloc);
                CHECK(malloc_size == (uint64_t)sizes[i] + overhead);
            }
            CHECK(u_get_allocated_count() == count + 1);
            CHECK(strcmp(old, "preserve on large OOM") == 0);
        }
    }
    u_free(old);
    CHECK(u_get_allocated_count() == count);
    puts("PASS: sizes near UINT32_MAX reach malloc intact or are rejected before overflow");
}

static void test_report_oom(void) {
    void* p = u_malloc(8); CHECK(p != NULL);
    size_t count = u_get_allocated_count(); fail_malloc = true;
    u_report_unfreed_blocks();
    CHECK(!fail_malloc && u_get_allocated_count() == count);
    u_free(p); CHECK(u_get_allocated_count() == count - 1);
    puts("PASS: report snapshot OOM preserves allocation list and releases its mutex");
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_WARN);
    test_small_allocations();
    test_oom();
    test_large_sizes();
    test_report_oom();
    CHECK(u_get_allocated_count() == 0);
    puts("PASS: mem regression tests");
    return 0;
}
