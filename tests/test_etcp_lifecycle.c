/* Lifecycle regressions: real ETCP queues/pools/event loop, no network required. */
#include <stdio.h>
#include <stdlib.h>
#include "etcp.h"
#include "etcp_api.h"
#include "node_conn_direct.h"
#include "utun_instance.h"
#include "config_parser.h"
#include "topo_group.h"
#include "topo_node.h"
#include "topo_recovery.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__func__,__LINE__,#x); exit(1); } } while (0)
static int deleted, victim_called, release_on_delete;
static void victim(struct ETCP_CONN* c, int ev, void* arg) { (void)c; (void)ev; (void)arg; victim_called++; }
static void remove_next(struct ETCP_CONN* c, int ev, void* arg) {
    (void)ev; (void)arg; etcp_conn_remove_cbk(c, victim, NULL); etcp_conn_remove_cbk(c, remove_next, NULL);
}
static void on_delete(struct ETCP_CONN* c, int ev, void* arg) {
    (void)ev; (void)arg; deleted++;
    if (release_on_delete) etcp_conn_ref_free(c);
    CHECK(!c->free_scheduled); // phase 1 still owns the object
    etcp_connection_close(c);
}
static void close_twice(struct ETCP_CONN* c, int ev, void* arg) {
    (void)ev; (void)arg;
    etcp_connection_close(c); void* token = c->close_token; CHECK(token);
    etcp_connection_close(c); CHECK(c->close_token == token);
    struct ll_entry* e = queue_entry_new(0);
    CHECK(etcp_send(c,e) == -1); queue_entry_free(e);
}
static struct NODE_CONN_DIRECT *owner1, *owner2;
static int owner1_calls, owner2_calls, closed_calls, action;
static void owner_cb(struct NODE_CONN_DIRECT* h, enum ncd_event event, void* arg) {
    int which = (int)(intptr_t)arg;
    if (event == NCD_EVENT_CLOSED) { CHECK(node_conn_direct_get_conn(h) == NULL); closed_calls++; return; }
    if (which == 1) owner1_calls++; else owner2_calls++;
    if (which == 2 && event == NCD_EVENT_DOWN) {
        if (action >= 1) { node_conn_direct_force_close(owner1); owner1 = NULL; }
        if (action >= 2) { node_conn_direct_force_close(h); owner2 = NULL; }
    }
}
static struct ETCP_CONN* ready_conn(struct UTUN_INSTANCE* inst) {
    struct ETCP_CONN* c = etcp_connection_create(inst,"ncd_lifecycle"); CHECK(c);
    c->crypto_ctx.peer_public_key[0] = 42; c->peer_node_id = 42; c->links_up = 1; etcp_conn_ready(c);
    queue_set_callback(c->send_input_q,NULL,NULL); // физического линка нет; проверяем lifecycle управляющих отправок
    return c;
}
static void ncd_cases(struct UTUN_INSTANCE* inst) {
    for (int scenario=0; scenario<9; scenario++) {
        owner1 = owner2 = NULL; owner1_calls = owner2_calls = closed_calls = 0; action = 0;
        struct ETCP_CONN* c = ready_conn(inst);
        CHECK(node_conn_direct_open(inst,42,owner_cb,(void*)1,&owner1,NULL) == NCD_REUSED);
        CHECK(node_conn_direct_open(inst,42,owner_cb,(void*)2,&owner2,NULL) == NCD_REUSED);
        CHECK(c->ref_count == 1); // одна ссылка на entry, не на handle
        if (scenario < 2) {
            etcp_connection_close(c); uasync_poll(inst->ua,0);
            CHECK(closed_calls == 2 && owner1_calls == 1 && owner2_calls == 1); // DOWN, без запоздалого UP
            CHECK(!inst->ncd_registry && !node_conn_direct_get_conn(owner1));
            struct ETCP_CONN* fresh = ready_conn(inst);
            struct NODE_CONN_DIRECT* h = NULL;
            CHECK(node_conn_direct_open(inst,42,NULL,NULL,&h,NULL) == NCD_REUSED);
            CHECK(node_conn_direct_get_conn(h) == fresh);
            if (scenario) { node_conn_direct_force_close(owner1); node_conn_direct_force_close(owner2); }
            else { node_conn_direct_close(owner1); node_conn_direct_close(owner2); }
            node_conn_direct_force_close(h);
        } else if (scenario < 4) {
            action = scenario - 1; c->links_up = 0;
            etcp_cbk_fire(c,ETCP_CBK_EVENT_DOWN);
            CHECK(owner2_calls == 1 && owner1_calls == 0);
            if (owner2) node_conn_direct_force_close(owner2);
        } else if (scenario == 4) {
            node_conn_direct_close(owner1); node_conn_direct_close(owner2);
            CHECK(c->fin_wait && inst->ncd_registry);
            struct NODE_CONN_DIRECT* h = NULL;
            CHECK(node_conn_direct_open(inst,42,owner_cb,(void*)1,&h,NULL) == NCD_REUSED);
            CHECK(!c->fin_wait); uasync_poll(inst->ua,0); CHECK(owner1_calls == 1 && !owner2_calls);
            node_conn_direct_force_close(h);
        } else if (scenario == 5) {
            node_conn_direct_force_close(owner1); node_conn_direct_force_close(owner2);
            uasync_poll(inst->ua,0); CHECK(!owner1_calls && !owner2_calls && !closed_calls);
        } else if (scenario == 8) {
            struct ETCP_SOCKET sock = {0}; sock.instance = inst;
            struct ETCP_LINK* link = u_calloc(1,sizeof(*link)); CHECK(link);
            link->etcp = c; link->conn = &sock; link->initialized = link->link_status = 1;
            c->links = link; action = 2;
            CHECK(ncd_remove_socket_links(inst,&sock) == 1); CHECK(!owner1 && !owner2);
        } else if (scenario == 7) {
            uasync_poll(inst->ua,0); CHECK(owner1_calls == 1 && owner2_calls == 1);
            etcp_conn_reinit_id(c,"lifecycle reset",c->reset_id);
            CHECK(c->links_up == 1 && owner1_calls == 1 && owner2_calls == 1);
            CHECK(!c->close_requested && node_conn_direct_get_conn(owner1) == c);
            etcp_conn_ready(c); CHECK(!c->reinit_pending);
            node_conn_direct_force_close(owner1); node_conn_direct_force_close(owner2);
        } else {
            c->links_up = 0; etcp_cbk_fire(c,ETCP_CBK_EVENT_DOWN);
            uasync_poll(inst->ua,0); CHECK(owner1_calls == 1 && owner2_calls == 1); // pending initial UP cancelled
            node_conn_direct_force_close(owner1); node_conn_direct_force_close(owner2);
        }
        uasync_poll(inst->ua,0);
        CHECK(inst->ncd_registry == NULL && inst->connections->count == 0);
        printf("PASS NCD scenario %d\n",scenario+1);
    }
}
static void recovery_cases(struct UTUN_INSTANCE* inst) {
    struct TOPO_GROUPS groups = {0}; struct TOPO_GROUP group = {0}; struct TOPO_GROUP_NODE node = {0};
    groups.instance = inst; inst->topo_groups = &groups; group.instance = inst; node.node_id = 43;
    groups.node_registry = queue_new(inst->ua,16,0,8,"recovery_registry");
    for (int scenario=0;scenario<3;scenario++) {
        struct TOPO_NODE* ni = u_calloc(1,sizeof(*ni)); CHECK(ni); ni->node_id = node.node_id;
        CHECK(topo_node_registry_store(&groups,ni) == ni && ni->group_ref_count == 1);
        topo_recovery_add_node(&group,ni,0,100); topo_recovery_add_node(&group,ni,0,100);
        CHECK(ni->group_ref_count == 2 && group.recovery);
        if (scenario == 1) { topo_recovery_add_node(&group,ni,0,50); CHECK(ni->group_ref_count == 2); }
        topo_node_registry_unref(&groups,node.node_id); CHECK(topo_node_registry_find(&groups,node.node_id) == ni);
        if (scenario < 2) topo_recovery_cancel_all(&group);
        else { topo_recovery_start(&group); topo_recovery_cancel_all(&group); }
        uasync_poll(inst->ua,0);
        CHECK(!group.recovery && !groups.node_registry->count && !inst->ncd_registry && !inst->connections->count);
        printf("PASS recovery ownership scenario %d\n",scenario+1);
    }
    queue_free(groups.node_registry); inst->topo_groups = NULL;
}
static int late_events[4];
static void late_up_cb(struct NODE_CONN_DIRECT* h, enum ncd_event ev, void* arg) {
    (void)h; (void)arg; late_events[ev]++;
}
static void ncd_late_up_case(struct UTUN_INSTANCE* inst) {
    struct ETCP_CONN* c = ready_conn(inst); c->links_up = 0;
    struct NODE_CONN_DIRECT* h = NULL;
    CHECK(node_conn_direct_open(inst,42,late_up_cb,NULL,&h,NULL) == NCD_REUSED);
    uasync_poll(inst->ua,10); CHECK(late_events[NCD_EVENT_TIMEOUT] == 1 && !late_events[NCD_EVENT_UP]);
    c->links_up = 1;
    etcp_cbk_fire(c,ETCP_CBK_EVENT_INIT); etcp_cbk_fire(c,ETCP_CBK_EVENT_UP);
    CHECK(late_events[NCD_EVENT_UP] == 1); // INIT + UP должны дать ровно одно уведомление
    c->links_up = 0; etcp_cbk_fire(c,ETCP_CBK_EVENT_DOWN);
    c->links_up = 1; etcp_cbk_fire(c,ETCP_CBK_EVENT_UP);
    CHECK(late_events[NCD_EVENT_DOWN] == 1 && late_events[NCD_EVENT_UP] == 2);
    node_conn_direct_force_close(h); uasync_poll(inst->ua,0);
    CHECK(!inst->ncd_registry && !inst->connections->count);
    puts("PASS NCD late UP after timeout and subsequent reconnect");
}
struct subscription_case;
struct subscription_arg { struct subscription_case *test; unsigned index; };
struct subscription_case {
    struct UTUN_INSTANCE *inst;
    struct ETCP_CONN *conn;
    struct ETCP_LINK *link;
    struct ETCP_SOCKET socket;
    struct subscription_arg args[3];
    unsigned kind, calls[3];
};
static void subscription_created(struct ETCP_CONN *conn, void *arg);
static void subscription_status(struct ETCP_CONN *conn, int status, void *arg);
static void subscription_link(struct ETCP_CONN *conn, struct ETCP_LINK *link, int old_state, int old_status, void *arg);
static void subscription_socket(struct ETCP_SOCKET *socket, int event, void *arg);
static void subscription_update(struct subscription_case *test, unsigned index, int add) {
    void *arg = &test->args[index];
    switch (test->kind) {
    case 0:
        if (add) etcp_add_new_conn_cbk(test->inst, subscription_created, arg);
        else etcp_remove_new_conn_cbk(test->inst, subscription_created, arg);
        break;
    case 1:
        if (add) etcp_add_conn_status_cbk(test->inst, subscription_status, arg);
        else etcp_remove_conn_status_cbk(test->inst, subscription_status, arg);
        break;
    case 2:
        if (add) etcp_add_link_status_cbk(test->inst, subscription_link, arg);
        else etcp_remove_link_status_cbk(test->inst, subscription_link, arg);
        break;
    case 3:
        if (add) etcp_add_socket_cbk(test->inst, subscription_socket, arg, ETCP_SOCKET_EVENT_STATUS_CHANGED);
        else etcp_remove_socket_cbk(test->inst, subscription_socket, arg);
        break;
    }
}
static void subscription_fire(struct subscription_case *test) {
    switch (test->kind) {
    case 0: {
        struct ETCP_CONN *created = etcp_connection_create(test->inst, "nested subscription"); CHECK(created);
        etcp_connection_close(created);
        break;
    }
    case 1: etcp_fire_conn_status(test->conn, ETCP_CONN_STATUS_NEW); break;
    case 2: etcp_fire_link_status_cbk(test->link, -1, 0); break;
    case 3: etcp_socket_cbk_fire(&test->socket, ETCP_SOCKET_EVENT_STATUS_CHANGED); break;
    }
}
static void subscription_called(void *arg) {
    struct subscription_arg *entry = arg;
    struct subscription_case *test = entry->test;
    test->calls[entry->index]++;
    if (entry->index != 1) return;
    CHECK(test->inst->etcp_callbacks_depth > 0);
    subscription_update(test, 0, 0); subscription_update(test, 1, 0);
    subscription_update(test, 2, 1);
    subscription_fire(test);
    CHECK(test->calls[0] == 0 && test->calls[1] == 1 && test->calls[2] == 1);
}
static void subscription_created(struct ETCP_CONN *conn, void *arg) { (void)conn; subscription_called(arg); }
static void subscription_status(struct ETCP_CONN *conn, int status, void *arg) {
    (void)conn; if (status == ETCP_CONN_STATUS_NEW) subscription_called(arg);
}
static void subscription_link(struct ETCP_CONN *conn, struct ETCP_LINK *link, int old_state, int old_status, void *arg) {
    (void)conn; (void)link; (void)old_state; (void)old_status; subscription_called(arg);
}
static void subscription_socket(struct ETCP_SOCKET *socket, int event, void *arg) {
    (void)socket; (void)event; subscription_called(arg);
}
static void subscription_cases(struct UTUN_INSTANCE *inst) {
    for (unsigned kind = 0; kind < 4; kind++) {
        struct subscription_case test = {.inst = inst, .kind = kind};
        test.conn = etcp_connection_create(inst, "subscription lifetime"); CHECK(test.conn);
        if (kind == 2) { test.link = etcp_link_new(test.conn, NULL, NULL, 0); CHECK(test.link); }
        test.socket.instance = inst;
        for (unsigned i = 0; i < 3; i++) { test.args[i].test = &test; test.args[i].index = i; }
        subscription_update(&test, 0, 1); subscription_update(&test, 1, 1);
        subscription_fire(&test);
        CHECK(!inst->etcp_callbacks_depth && !test.calls[0] && test.calls[1] == 1 && test.calls[2] == 1);
        subscription_update(&test, 2, 0);
        CHECK(!inst->new_conn_cbks && !inst->conn_status_cbks && !inst->link_status_cbks && !inst->socket_cbks);
        etcp_connection_close(test.conn); uasync_drain_immediate(inst->ua); uasync_poll(inst->ua, 0);
        CHECK(!inst->connections->count);
        printf("PASS callback chain %u: remove self/next, add and nested dispatch\n", kind);
    }
}

struct create_close_case { unsigned created, published, deleted, status; };
static void cancel_created_connection(struct ETCP_CONN *conn, void *arg) {
    struct create_close_case *test = arg;
    test->created++;
    etcp_connection_close(conn);
    CHECK(conn->close_requested && !conn->delete_complete);
}
static void observe_created_connection(struct ETCP_CONN *conn, int status, void *arg) {
    struct create_close_case *test = arg;
    if (status == ETCP_CONN_STATUS_DELETE) test->deleted++;
    if (status != ETCP_CONN_STATUS_NEW) return;
    test->published++;
    if (test->status) etcp_connection_close(conn);
}
static void constructor_close_cases(struct UTUN_INSTANCE *inst) {
    for (unsigned status = 0; status < 2; status++) {
        struct create_close_case test = {.status = status};
        if (!status) etcp_add_new_conn_cbk(inst, cancel_created_connection, &test);
        etcp_add_conn_status_cbk(inst, observe_created_connection, &test);
        CHECK(etcp_connection_create(inst, "cancel creation") == NULL);
        CHECK(test.deleted == 1 && test.created == !status && test.published == status);
        CHECK(!inst->connections->count);
        if (!status) etcp_remove_new_conn_cbk(inst, cancel_created_connection, &test);
        etcp_remove_conn_status_cbk(inst, observe_created_connection, &test);
        uasync_drain_immediate(inst->ua); uasync_poll(inst->ua, 0);
        printf("PASS connection creation cancelled from %s callback\n", status ? "NEW status" : "new connection");
    }
}

struct link_close_case { struct ETCP_LINK *neighbor; unsigned events, nested; };
static void cancel_created_link(struct ETCP_CONN *conn, struct ETCP_LINK *link, int old_state, int old_status, void *arg) {
    (void)old_status;
    unsigned action = (unsigned)(uintptr_t)arg;
    if (old_state != -1) return;
    if (action == 1) etcp_link_close(link);
    else etcp_connection_close(conn);
}
static void link_constructor_close_cases(struct UTUN_INSTANCE *inst) {
    for (unsigned action = 1; action <= 2; action++) {
        struct ETCP_CONN *conn = etcp_connection_create(inst, "cancel link"); CHECK(conn);
        CHECK(etcp_conn_ref_take(conn) == 0);
        etcp_add_link_status_cbk(inst, cancel_created_link, (void*)(uintptr_t)action);
        CHECK(etcp_link_new(conn, NULL, NULL, 0) == NULL);
        CHECK(!conn->links);
        etcp_remove_link_status_cbk(inst, cancel_created_link, (void*)(uintptr_t)action);
        etcp_connection_close(conn); etcp_conn_ref_free(conn);
        uasync_drain_immediate(inst->ua); uasync_poll(inst->ua, 0);
        CHECK(!inst->connections->count);
        printf("PASS link creation cancelled by closing %s\n", action == 1 ? "link" : "connection");
    }
}
static void close_neighbor(struct ETCP_CONN *conn, struct ETCP_LINK *link, int old_state, int old_status, void *arg) {
    (void)conn; (void)old_state; (void)old_status;
    struct link_close_case *test = arg;
    test->events++;
    CHECK(link->closing);
    etcp_link_close(link);
    if (test->neighbor && test->neighbor != link) {
        struct ETCP_LINK *neighbor = test->neighbor;
        test->neighbor = NULL; test->nested++;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Closing neighbor from teardown callback: current=%u neighbor=%u",
            link->local_link_id, neighbor->local_link_id);
        etcp_link_close(neighbor);
    }
}
static void neighbor_close_case(struct UTUN_INSTANCE *inst) {
    struct ETCP_CONN *conn = etcp_connection_create(inst, "neighbor close"); CHECK(conn);
    CHECK(etcp_conn_ref_take(conn) == 0);
    CHECK(etcp_link_new(conn, NULL, NULL, 0));
    struct link_close_case test = {.neighbor = etcp_link_new(conn, NULL, NULL, 0)}; CHECK(test.neighbor);
    etcp_add_link_status_cbk(inst, close_neighbor, &test);
    etcp_connection_close(conn);
    CHECK(!conn->links && test.events == 2 && test.nested == 1 && conn->delete_complete);
    etcp_remove_link_status_cbk(inst, close_neighbor, &test);
    etcp_conn_ref_free(conn); uasync_drain_immediate(inst->ua); uasync_poll(inst->ua, 0);
    CHECK(!inst->connections->count);
    puts("PASS nested close: current and neighbor links, no cached next pointer");
}

/* Два уже аутентифицированных транспорта с одним числовым ID моделируют коллизию ETCP.
 * Hold и закрытие одного владельца должны сохранить второй conn. */
static void ncd_collision_case(struct UTUN_INSTANCE* inst) {
    struct ETCP_CONN* a = ready_conn(inst);
    struct ETCP_CONN* b = etcp_connection_create(inst, "ncd collision"); CHECK(b);
    b->peer_node_id = a->peer_node_id; b->crypto_ctx.peer_public_key[0] = 43;
    b->links_up = 1; etcp_conn_ready(b);
    a->initialized = b->initialized = 1;
    struct NODE_CONN_DIRECT *ha = NULL, *hb = NULL;
    CHECK(!node_conn_direct_hold(a, &ha) && !node_conn_direct_hold(b, &hb));
    CHECK(node_conn_direct_get_conn(ha) == a && node_conn_direct_get_conn(hb) == b);
    node_conn_direct_force_close(ha);
    CHECK(node_conn_direct_get_conn(hb) == b && node_conn_direct_handle_count(b) == 1);
    node_conn_direct_force_close(hb);
    uasync_drain_immediate(inst->ua); uasync_poll(inst->ua, 0);
    CHECK(!inst->connections->count);
}

static void registry_index_cases(struct UTUN_INSTANCE *inst) {
    struct ETCP_CONN *first = etcp_connection_create(inst, "first indexed"); CHECK(first);
    struct ETCP_CONN *second = etcp_connection_create(inst, "second indexed"); CHECK(second);
    struct ll_entry *entry = first->conn_queue_entry, *other = second->conn_queue_entry;
    first->peer_node_id = second->peer_node_id = 1;
    first->crypto_ctx.peer_public_key[0] = second->crypto_ctx.peer_public_key[0] = 1;
    size_t allocated = u_get_allocated_count();
    CHECK(etcp_conn_index_peer(first) == 0 && first->conn_queue_entry == entry);
    CHECK(first->state == 0 && instance_find_conn(inst, 1) == first && inst->connections->count == 2);
    CHECK(etcp_conn_index_peer(first) == 0 && u_get_allocated_count() == allocated);
    CHECK(etcp_conn_index_peer(second) < 0 && second->conn_queue_entry == other);
    CHECK(((struct conn_queue_entry*)other->data)->peer_node_id == 0 && instance_find_conn(inst, 1) == first);
    etcp_conn_queue_set_ready(first); etcp_conn_queue_set_ready(first);
    CHECK(first->state == 1 && first->conn_queue_entry == entry && u_get_allocated_count() == allocated);
    second->crypto_ctx.peer_public_key[0] = 2;
    CHECK(!etcp_conn_index_peer(second) && !instance_find_conn(inst, 1));
    CHECK(instance_find_conn_by_key(inst, first->crypto_ctx.peer_public_key) == first);
    CHECK(instance_find_conn_by_key(inst, second->crypto_ctx.peer_public_key) == second);
    second->peer_node_id = 2;
    CHECK(etcp_conn_index_peer(second) == 0 && second->conn_queue_entry == other && instance_find_conn(inst, 2) == second);
    /* Отказ put освобождает entry: close не должен повторно трогать эту память. */
    queue_set_size_limit(inst->connections, 1);
    second->peer_node_id = 3;
    CHECK(etcp_conn_index_peer(second) < 0 && !second->conn_queue_entry && inst->connections->count == 1);
    CHECK(!instance_find_conn(inst, 2) && !instance_find_conn(inst, 3) && instance_find_conn(inst, 1) == first);
    queue_set_size_limit(inst->connections, -1);
    etcp_connection_close(second); etcp_connection_close(first);
    uasync_drain_immediate(inst->ua); uasync_poll(inst->ua, 0);
    CHECK(!inst->connections->count);
    puts("PASS peer registry: stable entry, idempotent ready, duplicate rejection, failed put cleanup");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    const char* log = getenv("LIFECYCLE_TEST_LOG");
    if (log) { debug_enable_file_output(log,1); debug_set_category_level(DEBUG_CATEGORY_CONNECTION,DEBUG_LEVEL_DEBUG); }
    size_t baseline = u_get_allocated_count();
    struct UTUN_INSTANCE inst = {0}; struct utun_config cfg = {0};
    inst.ua = uasync_create(); inst.config = &cfg;
    inst.connections = queue_new(inst.ua,16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE,"test_connections");
    for (int scenario=0; scenario<4; scenario++) {
        deleted = victim_called = 0; release_on_delete = scenario == 0;
        struct ETCP_CONN* c = etcp_connection_create(&inst,"lifecycle"); CHECK(c);
        etcp_conn_add_cbk(c,on_delete,NULL,ETCP_CBK_EVENT_DELETE);
        if (scenario < 2) CHECK(etcp_conn_ref_take(c) == 0);
        if (scenario == 2) {
            etcp_conn_add_cbk(c,close_twice,NULL,ETCP_CBK_EVENT_REINIT);
            etcp_cbk_fire(c,ETCP_CBK_EVENT_REINIT);
            CHECK(c->close_requested && deleted == 0);
        }
        if (scenario == 3) {
            etcp_conn_add_cbk(c,victim,NULL,ETCP_CBK_EVENT_REINIT);
            etcp_conn_add_cbk(c,remove_next,NULL,ETCP_CBK_EVENT_REINIT);
            etcp_cbk_fire(c,ETCP_CBK_EVENT_REINIT); CHECK(!victim_called);
        }
        etcp_connection_close(c); CHECK(deleted == 1 && c->delete_complete && !c->close_token);
        if (scenario == 1) {
            uasync_poll(inst.ua,0); CHECK(c->ref_count == 1 && !c->free_scheduled);
            etcp_conn_ref_free(c); CHECK(c->free_scheduled);
        }
        uasync_poll(inst.ua,0); CHECK(inst.connections->count == 0);
        printf("PASS lifecycle scenario %d\n",scenario+1);
    }
    ncd_cases(&inst); ncd_late_up_case(&inst); recovery_cases(&inst); neighbor_close_case(&inst);
    constructor_close_cases(&inst); link_constructor_close_cases(&inst); subscription_cases(&inst);
    ncd_collision_case(&inst);
    registry_index_cases(&inst);
    queue_free(inst.connections); uasync_poll(inst.ua,0);
    CHECK(inst.ua->timer_alloc_count == inst.ua->timer_free_count);
    uasync_destroy(inst.ua,0); CHECK(u_get_allocated_count() == baseline);
    puts("All ETCP lifecycle scenarios passed"); return 0;
}
