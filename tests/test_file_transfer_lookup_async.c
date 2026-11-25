#define _GNU_SOURCE
/* Асинхронный provider: copied REQUEST, повторы/лимиты, timeout/revoke/destroy и поздние callbacks. */
#include "../src/media_delivery/file_transfer.c"
#include <assert.h>
#include <unistd.h>

static struct ETCP_ROUTER_CONN route;
static uint8_t sdk_peer[32];
static uint64_t sdk_node;
static int allow = 1, refused, total, immediate, oom;
static char source[1024];
static struct { file_transfer_lookup_ready_fn ready; void* request; uint8_t object[16]; } pending[64];
struct ETCP_ROUTER_CONN* __wrap_etcp_router_conn_get_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t service) {
    (void)inst; assert(group == 123 && service == 59 && !memcmp(ed, sdk_peer, 32)); return &route;
}
int __wrap_etcp_router_conn_send_q_has_room(const struct ETCP_ROUTER_CONN* conn) {
    assert(conn == &route); return 1;
}
int __wrap_group_policy_member_keys(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, int active, uint8_t x[32], uint8_t ed[32]) {
    (void)inst; assert(!strcmp(channel, "123") && node == sdk_node && active && !x && ed);
    memcpy(ed, sdk_peer, 32); return 0;
}
int __wrap_etcp_router_conn_send(struct ETCP_ROUTER_CONN* conn, const uint8_t* data, size_t size, int mode) {
    (void)conn; (void)mode; assert(size); if (data[0] == FT_REFUSE) refused++; return 0;
}
void __wrap_etcp_router_set_max_inflight(struct ETCP_ROUTER_CONN* conn, uint16_t value) { (void)conn; (void)value; }
int __wrap_conn_mgr_open(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], conn_mgr_cb_t cb, void* arg,
    struct CONN_MGR_HANDLE** handle) { (void)inst; (void)group; (void)peer; (void)cb; (void)arg; *handle = NULL; return 0; }
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (oom && strstr(where, "file_transfer.c:")) { oom = 0; return NULL; }
    return __real_u_calloc_impl(count, size, where);
}
static int allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32]) {
    (void)inst; assert(group == 123 && !memcmp(ed, sdk_peer, 32)); return allow;
}
static void lookup(void* arg, uint64_t group, const uint8_t peer[32], const uint8_t object[16], uint64_t size,
    file_transfer_lookup_ready_fn ready, void* request) {
    (void)arg; assert(group == 123 && !memcmp(peer, sdk_peer, 32) && size == 12 && total < 64);
    int n = total++; pending[n].ready = ready; pending[n].request = request; memcpy(pending[n].object, object, 16);
    if (immediate) { pending[n].request = NULL; ready(request, 0, source); }
}
static void complete(int index, int error) {
    assert(index >= 0 && index < total && pending[index].request);
    void* request = pending[index].request; pending[index].request = NULL;
    pending[index].ready(request, error, error ? NULL : source);
}
static void request(struct file_transfer* ft, int id, uint64_t offset) {
    uint8_t data[48] = {0}; data[0] = id; data[16] = id;
    uint64_t size = 12; memcpy(data + 32, &size, 8); memcpy(data + 40, &offset, 8);
    ft_serve(ft, 123, sdk_peer, data); memset(data, 0xee, sizeof(data));
}
static struct file_transfer* create(struct UTUN_INSTANCE* inst) {
    struct file_transfer* ft = file_transfer_create_service_async(inst, 59, lookup, allowed, NULL); assert(ft); return ft;
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); size_t allocated = u_get_allocated_count();
    uint8_t seed[32] = {43}, public_x[32], private_x[32];
    assert(sc_identity_from_seed(seed, public_x, private_x, sdk_peer) == SC_OK);
    sdk_node = sc_derive_node_id_from_pubkey(public_x);
    const char* temporary = getenv("TMPDIR"); assert(temporary);
    char directory[768]; snprintf(directory, sizeof(directory), "%s/ft-lookup-XXXXXX", temporary); assert(mkdtemp(directory));
    snprintf(source, sizeof(source), "%s/file", directory); FILE* file = fopen(source, "wb");
    assert(file && fwrite("file-payload", 1, 12, file) == 12 && !fclose(file));
    struct UASYNC* ua = uasync_create(); assert(ua); struct UTUN_INSTANCE inst = {.ua=ua, .node_id=42};
    assert(!etcp_router_init(&inst)); route.send_q = queue_new(ua, 16, 0, 0, "lookup test queue"); assert(route.send_q);
    struct file_transfer* ft = create(&inst);
    request(ft, 1, 0); assert(total == 1 && ft->lookup_count == 1 && ft->lookup_slots == 1 && !ft->count);
    assert(pending[0].object[0] == 1); complete(0, 0); assert(ft->count == 1 && !ft->lookup_count && !ft->lookup_slots);
    assert(ft->ops->tid[0] == 1 && ft->ops->object[0] == 1 && !ft->ops->offset);
    request(ft, 1, 4); assert(total == 2 && ft->count == 1 && ft->lookup_count == 1 && !ft->lookup_slots);
    request(ft, 1, 7); assert(total == 2); complete(1, 0); assert(ft->count == 1 && ft->ops->offset == 7);
    request(ft, 1, 7); assert(total == 3); complete(2, -1); assert(!ft->count && refused == 1);
    immediate = 1; request(ft, 2, 0); immediate = 0;
    assert(ft->count == 1 && !ft->lookup_count); uint8_t object[16] = {2}; file_transfer_cancel_object(ft, object); assert(!ft->count);
    request(ft, 3, 0); object[0] = 3; file_transfer_cancel_object(ft, object); assert(!ft->lookup_count); complete(4, 0); assert(!ft->count);
    request(ft, 4, 0); allow = 0; file_transfer_revalidate_members(ft, 123); allow = 1;
    assert(!ft->lookup_count); complete(5, 0); assert(!ft->count);
    request(ft, 5, 0); struct ft_lookup* expiring = ft->lookups;
    uasync_cancel_timeout(ua, expiring->timer); expiring->timer = NULL; ft_lookup_timeout(expiring);
    assert(!ft->lookup_count && refused == 2); complete(6, 0); assert(!ft->count);
    request(ft, 6, 0); file_transfer_destroy(ft); ft = create(&inst); complete(7, 0);
    assert(!ft->lookup_count && !ft->count);
    int first = total;
    for (int n = 0; n < FT_MAX_OPS; n++) request(ft, 10 + n, 0);
    assert(total == first + FT_MAX_OPS && ft->lookup_count == FT_MAX_OPS && ft->lookup_slots == FT_MAX_OPS);
    request(ft, 30, 0); assert(total == first + FT_MAX_OPS && refused == 3);
    complete(first, 0); assert(ft->count == 1 && ft->lookup_slots == FT_MAX_OPS - 1);
    request(ft, 10, 2); assert(total == first + FT_MAX_OPS + 1 && ft->lookup_count == FT_MAX_OPS);
    complete(total - 1, 0); assert(ft->count == 1 && ft->ops->offset == 2);
    file_transfer_destroy(ft);
    for (int n = 1; n < FT_MAX_OPS; n++) complete(first + n, 0);
    ft = create(&inst); oom = 1; int before = total; request(ft, 31, 0);
    assert(!oom && total == before && !ft->lookup_count && !ft->count && refused == 4);
    request(ft, 32, 0); struct ft_lookup* terminal = ft->lookups; ft_lookup_cancel_tid(ft, 123, sdk_peer, terminal->request);
    assert(!ft->lookup_count); complete(total - 1, 0); assert(!ft->count);
    file_transfer_destroy(ft);
    for (int n = 0; n < total; n++) assert(!pending[n].request);
    queue_free(route.send_q); route.send_q = NULL; etcp_router_destroy(&inst); uasync_destroy(ua, 0);
    assert(!unlink(source) && !rmdir(directory)); assert(u_get_allocated_count() == allocated);
    puts("Async file lookup: copied input, retry/limits, sync completion, timeout/revoke/destroy: OK"); return 0;
}
