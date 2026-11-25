// test_serialize.c - Unit tests for serialize library (with linked list + error cases)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include "../lib/serialize.h"

#define TEST(name) do { \
    printf("TEST: %-40s ", name); fflush(stdout); \
} while(0)
#define PASS() do { puts("PASS"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); exit(1); } while(0)

#pragma pack(push, 1)
typedef struct TestNode {
    uint32_t value;
    struct TestNode* next;
} TestNode;

typedef struct {
    uint8_t id;
    uint16_t port;
    uint32_t ip;
    uint64_t big_val;
    int8_t signed_val;
    int32_t signed32;
    uint8_t name_len;
    char* name;
    uint8_t addr_cnt;
    uint8_t* addrs;
    TestNode* list;          // linked list (type 6)
} TestStruct;
#pragma pack(pop)

static const struct SerializeField test_fields[] = {
    {offsetof(TestStruct, id),        1, SERIALIZE_TYPE_FIXED,      0},
    {offsetof(TestStruct, port),      2, SERIALIZE_TYPE_FIXED,      0},
    {offsetof(TestStruct, ip),        4, SERIALIZE_TYPE_FIXED,      0},
    {offsetof(TestStruct, big_val),   8, SERIALIZE_TYPE_FIXED,      0},
    {offsetof(TestStruct, signed_val),1, SERIALIZE_TYPE_FIXED,      0},
    {offsetof(TestStruct, signed32),  4, SERIALIZE_TYPE_FIXED,      0},
    {offsetof(TestStruct, name),      1, SERIALIZE_TYPE_ARRAY_U8,   offsetof(TestStruct, name_len)},
    {offsetof(TestStruct, addrs),     1, SERIALIZE_TYPE_ARRAY_U8,   offsetof(TestStruct, addr_cnt)},
    {offsetof(TestStruct, list),      sizeof(TestNode), SERIALIZE_TYPE_LINKED, offsetof(TestNode, next)}
};

static const struct SerializeSchema test_schema = {
    .field_count = sizeof(test_fields) / sizeof(test_fields[0]),
    .struct_size = sizeof(TestStruct),
    .max_size    = 0,
    .header_len  = 4,
    .fields      = test_fields
};

/* ------------------------------------------------------------------ */
static void test_alloc_roundtrip(void) {
    uint8_t* buf = NULL;
    size_t len;
   
    TestStruct orig = {
        .id = 1,
        .port = 443,
        .ip = 0x01020304,
        .big_val = 100,
        .signed_val = 10,
        .signed32 = 5000,
        .list = NULL
    };
    orig.name = strdup("hello");
    orig.name_len = 5;
    orig.addrs = u_malloc(8);
    orig.addrs[0] = 192; orig.addrs[1] = 168; orig.addrs[2] = 1; orig.addrs[3] = 1;
    orig.addrs[4] = 10;  orig.addrs[5] = 0;   orig.addrs[6] = 0; orig.addrs[7] = 1;
    orig.addr_cnt = 8;

    // === linked list ===
    TestNode* n1 = u_malloc(sizeof(TestNode));
    TestNode* n2 = u_malloc(sizeof(TestNode));
    n1->value = 100;
    n1->next  = n2;
    n2->value = 200;
    n2->next  = NULL;
    orig.list = n1;

    uint8_t header[] = {0x01, 0x02, 0x03, 0x04};
    TEST("serialize_encode with header + linked list");
    int r = serialize_encode(&orig, &test_schema, header, &buf, &len);
    if (r != SERIALIZE_ERR_OK) FAIL("serialize_encode failed");
    PASS();
   
    TestStruct* restored = NULL;
    TEST("serialize_decode (data without header)");
    uint8_t* data_ptr = buf + test_schema.header_len;
    r = serialize_decode(data_ptr, len - test_schema.header_len, &test_schema, (void**)&restored);
    if (r != SERIALIZE_ERR_OK) FAIL("serialize_decode failed");
    PASS();
   
    TEST("check restored not NULL");
    if (!restored) FAIL("restored is NULL");
    PASS();
   
    TEST("check fixed fields");
    if (restored->id != orig.id || restored->port != orig.port) FAIL("fixed fields mismatch");
    PASS();
   
    TEST("check asciiz");
    if (!restored->name || strcmp(restored->name, "hello") != 0) FAIL("name mismatch");
    PASS();
   
    TEST("check array");
    if (!restored->addrs || restored->addrs[0] != 192) FAIL("addrs mismatch");
    PASS();
   
    TEST("check name_len / addr_cnt");
    if (restored->name_len != 5 || restored->addr_cnt != 8) FAIL("len fields mismatch");
    PASS();
   
    TEST("check linked list");
    if (!restored->list || restored->list->value != 100) FAIL("list head mismatch");
    if (!restored->list->next || restored->list->next->value != 200) FAIL("list next mismatch");
    if (restored->list->next->next != NULL) FAIL("list tail not NULL");
    PASS();
   
    TEST("serialize_free (full structure with list)");
    serialize_free(&test_schema, (void**)&restored);
    if (restored) FAIL("ptr should be NULL after free");
    PASS();
   
    u_free(buf);

    // очистка orig
    free(orig.name);
    u_free(orig.addrs);
    u_free(n1);
    u_free(n2);
}

static void test_decode_error(void) {
    uint8_t* buf = NULL;
    size_t len;
   
    TestStruct orig = { .id = 42, .port = 1234, .list = NULL };
    orig.name = strdup("error_test");
    orig.name_len = 10;
    orig.addrs = u_malloc(2);
    orig.addrs[0] = 0xAA; orig.addrs[1] = 0xBB;
    orig.addr_cnt = 2;

    uint8_t header[] = {0xAA, 0xBB, 0xCC, 0xDD};
    int r = serialize_encode(&orig, &test_schema, header, &buf, &len);
    if (r != SERIALIZE_ERR_OK) FAIL("encode for error test failed");
    PASS(); // не выводим, просто подготовка

    TestStruct* restored = NULL;
    uint8_t* data_ptr = buf + test_schema.header_len;
    size_t short_len = (len - test_schema.header_len) / 2; // намеренно неполный буфер

    TEST("serialize_decode on short buffer (should free internally)");
    r = serialize_decode(data_ptr, short_len, &test_schema, (void**)&restored);
    if (r != SERIALIZE_ERR_BUF) FAIL("expected SERIALIZE_ERR_BUF on truncated data");
    if (restored != NULL) FAIL("restored must be NULL after error (free was called)");
    PASS();

    u_free(buf);
    free(orig.name);
    u_free(orig.addrs);
}

static void test_free_null_and_double(void) {
    TestStruct* p = NULL;
    TEST("serialize_free on NULL pointer");
    serialize_free(&test_schema, (void**)&p); // должно ничего не делать
    if (p != NULL) FAIL("NULL pointer was changed");
    PASS();

    // уже освобождённая структура
    TestStruct* dummy = u_malloc(sizeof(TestStruct));
    memset(dummy, 0, sizeof(TestStruct));
    TEST("serialize_free after already freed");
    serialize_free(&test_schema, (void**)&dummy);
    if (dummy != NULL) FAIL("pointer not set to NULL");
    PASS();
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("=== Serialize Library Tests (linked list + error handling) ===\n\n");
   
    test_alloc_roundtrip();
    test_decode_error();
    test_free_null_and_double();
   
    printf("\n=== All tests passed ===\n");
    return 0;
}
