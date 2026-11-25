/* BlueZ D-Bus через sd-bus (без Qt/GLib). Один uasync-поток, без блокирующих вызовов.
 * Серверные DATA notifications и INVITE/ACK indications адресуются через AcquireNotify fd каждого клиента.
 * DATA завершается после send(fd); INVITE/ACK ждут байт 1 от BlueZ (ATT completion). */
#include "ble_backend.h"
#include "ble_identity.h"
#include "../u_async.h"
#include "../mem.h"
#include "../debug_config.h"
#include <systemd/sd-bus.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define CAT DEBUG_CATEGORY_DISCOVERY
#define ROOT "/org/vibechat/ble"
#define SERVICE ROOT "/service0"
#define RX SERVICE "/rx"
#define TX SERVICE "/tx"
#define INVITE SERVICE "/invite"
#define ACK SERVICE "/ack"
#define NAME SERVICE "/name"
#define NODE SERVICE "/node"
#define BEACON ROOT "/beacon"
#define DEVICE_IF "org.bluez.Device1"
#define CHAR_IF "org.bluez.GattCharacteristic1"
/* Верхняя граница запроса BlueZ; контроллер выбирает доступную мощность не выше неё. Только advertising. */
#define ADV_TX_POWER_MAX_DBM 20
enum object_read { OBJECT_SNAPSHOT, OBJECT_EVENT, OBJECT_SELECT_CONNECTED, OBJECT_SELECT_SERVICE };
struct bluez_link {
    struct ble_backend* backend;
    ble_link_id id;
    char device[BLE_ENDPOINT_MAX], service[BLE_ENDPOINT_MAX], rx[BLE_ENDPOINT_MAX], tx[BLE_ENDPOINT_MAX], invite[BLE_ENDPOINT_MAX], ack[BLE_ENDPOINT_MAX];
    char name[BLE_ENDPOINT_MAX], node[BLE_ENDPOINT_MAX];
    uint8_t profile_node[BLE_PUBLIC_KEY_SIZE]; int profile_read;
    sd_bus_slot *connect, *query, *subscribe, *write;
    int fd[BLE_WIRE_COUNT], outgoing, opened, sending, buffered, subscribed, channel, connected, resolved, mtu_known;
    void* io[BLE_WIRE_COUNT];
    unsigned int mtu;
    uint8_t value[BLE_VALUE_MAX];
    size_t value_size;
};
struct characteristic { struct ble_backend* backend; int tx; };
struct advertisement { struct ble_backend* backend; };
struct ble_backend {
    struct ble_transport* owner;
    struct UASYNC* ua;
    sd_bus* bus;
    sd_bus_slot *setup, *signals, *objects, *removed, *owner_watch, *service, *rx, *tx, *invite, *ack, *manager;
    sd_bus_slot *beacon, *beacon_op, *name, *node, *name_desc, *node_desc;
    struct characteristic rx_context, tx_context, invite_context, ack_context, name_context, node_context;
    struct { char device[BLE_ENDPOINT_MAX], name[BLE_NAME_MAX + 1]; } name_reads[BLE_LINKS_MAX];
    unsigned name_cursor;
    struct advertisement beacon_context;
    char service_uuid[37];
    struct { char device[BLE_ENDPOINT_MAX]; struct ble_identity identity; uint64_t seen; } peers[64];
    struct bluez_link links[BLE_LINKS_MAX];
    char adapter[BLE_ENDPOINT_MAX], adapter_name[64], daemon_owner[BLE_ENDPOINT_MAX];
    uint8_t data[BLE_BROADCAST_MAX], next_data[BLE_BROADCAST_MAX];
    size_t data_size, next_size;
    int beacon_registered, beacon_desired, beacon_action, step, failed, processing;
    void *io, *timer;
};
static void bus_update(struct ble_backend* b);
static void bus_timer(void* arg);
static int query_link(struct bluez_link* l);
static int connected(sd_bus_message* m, void* arg, sd_bus_error* error);
static void beacon_update(struct ble_backend* b);
static void fail(struct ble_backend* b, const char* where, int error) {
    if (b->failed) return;
    b->failed = 1;
    DEBUG_ERROR(CAT, "BLE BlueZ %s: %s (%d)", where, strerror(error < 0 ? -error : error), error);
    ble_backend_status(b->owner, 0, where);
}
static int response_error(sd_bus_message* m, const char* where) {
    const sd_bus_error* e = sd_bus_message_get_error(m);
    if (!e) return 0;
    DEBUG_ERROR(CAT, "BLE BlueZ %s: %s: %s", where, e->name, e->message ? e->message : ""); return 1;
}
static void bus_process(struct ble_backend* b) {
    if (b->processing || b->failed) return;
    b->processing = 1;
    int r = 0;
    for (int i = 0; i < 64; i++) { r = sd_bus_process(b->bus, NULL); if (r <= 0 || b->failed) break; }
    b->processing = 0;
    if (r < 0) { fail(b, "D-Bus disconnected", r); return; }
    bus_update(b);
    if (r > 0 && !b->failed) {
        DEBUG_DEBUG(CAT, "BLE D-Bus drain budget exhausted; continuing buffered messages");
        if (b->timer) uasync_cancel_timeout(b->ua, b->timer);
        b->timer = uasync_set_timeout(b->ua, 0, b, bus_timer, "ble_dbus_drain");
        if (!b->timer) fail(b, "D-Bus drain allocation", ENOMEM);
    }
}
static void bus_io(int fd, void* arg) { (void)fd; bus_process(arg); }
static void bus_timer(void* arg) { struct ble_backend* b = arg; b->timer = NULL; bus_process(b); }
static void bus_update(struct ble_backend* b) {
    if (b->processing || b->failed) return;
    int events = sd_bus_get_events(b->bus);
    uint64_t timeout = UINT64_MAX;
    int r = sd_bus_get_timeout(b->bus, &timeout);
    if (events < 0 || r < 0) { fail(b, "D-Bus polling failed", events < 0 ? events : r); return; }
    uasync_set_socket_read(b->ua, b->io, !!(events & POLLIN));
    uasync_set_socket_write(b->ua, b->io, !!(events & POLLOUT));
    if (b->timer) { uasync_cancel_timeout(b->ua, b->timer); b->timer = NULL; }
    if (timeout != UINT64_MAX) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
        uint64_t delay = timeout > now ? (timeout - now + 99) / 100 : 0;
        b->timer = uasync_set_timeout(b->ua, delay > INT_MAX ? INT_MAX : (int)delay, b, bus_timer, "ble_dbus");
        if (!b->timer) fail(b, "D-Bus timer allocation", ENOMEM);
    }
}
static struct bluez_link* find_link(struct ble_backend* b, ble_link_id id) {
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (b->links[i].id == id && id) return &b->links[i];
    return NULL;
}
static struct bluez_link* find_device(struct ble_backend* b, const char* device) {
    struct bluez_link* profile = NULL;
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (b->links[i].id && !strcmp(b->links[i].device, device)) {
        if (!ble_backend_profile_only(b->owner, b->links[i].id)) return &b->links[i];
        profile = &b->links[i];
    }
    return profile;
}
static struct bluez_link* alloc_link(struct ble_backend* b, ble_link_id id, const char* device, int outgoing) {
    struct bluez_link* existing = find_device(b, device);
    int metadata_owner = (outgoing && ble_backend_profile_only(b->owner, id) && existing && !existing->outgoing) ||
                         (!outgoing && existing && ble_backend_profile_only(b->owner, existing->id));
    if (strlen(device) >= BLE_ENDPOINT_MAX || (existing && !metadata_owner)) return NULL;
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (!b->links[i].id) {
        struct bluez_link* l = &b->links[i]; memset(l, 0, sizeof(*l));
        l->backend = b; l->id = id;
        for (int ch = 0; ch < BLE_WIRE_COUNT; ch++) l->fd[ch] = -1;
        l->mtu = 23; l->outgoing = outgoing; strcpy(l->device, device); return l;
    }
    return NULL;
}
static struct bluez_link* incoming(struct ble_backend* b, const char* device) {
    struct bluez_link* l = find_device(b, device);
    if (l && !ble_backend_profile_only(b->owner, l->id)) return l->outgoing ? NULL : l;
    struct ble_endpoint ep = {0};
    if (strlen(device) >= sizeof(ep.id)) return NULL;
    strcpy(ep.id, device);
    ble_link_id id = ble_backend_incoming(b->owner, &ep);
    if (!id) return NULL;
    l = alloc_link(b, id, device, 0);
    if (!l) ble_backend_closed(b->owner, id, "BlueZ link allocation failed");
    return l;
}
static void link_error(struct bluez_link* l, const char* reason) {
    DEBUG_WARN(CAT, "BLE BlueZ link=%llu failed: %s device=%s outgoing=%d connected=%d resolved=%d opened=%d",
               (unsigned long long)l->id, reason, l->device, l->outgoing, l->connected, l->resolved, l->opened);
    ble_backend_closed(l->backend->owner, l->id, reason);
}
static void notify_read(int fd, void* arg) {
    struct bluez_link* l = arg;
    uint8_t data[8]; ssize_t n = recv(fd, data, sizeof(data), MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n < 0) {
        DEBUG_ERROR(CAT, "BLE notification socket read failed link=%llu fd=%d errno=%d: %s",
                    (unsigned long long)l->id, fd, errno, strerror(errno));
        link_error(l, "notification socket read failed"); return;
    }
    if (!n) {
        DEBUG_INFO(CAT, "BLE BlueZ subscription removed link=%llu fd=%d", (unsigned long long)l->id, fd);
        ble_backend_closed(l->backend->owner, l->id, "notification subscription removed"); return;
    }
    if (n != 1 || data[0] != 1 || l->channel != BLE_WIRE_INVITE || fd != l->fd[l->channel] ||
        !l->sending || l->buffered) {
        DEBUG_WARN(CAT, "BLE indication completion invalid fd=%d expected_fd=%d bytes=%zd sending=%d buffered=%d",
                   fd, l->fd[l->channel], n, l->sending, l->buffered);
        link_error(l, "invalid indication completion"); return;
    }
    l->sending = 0;
    ble_backend_sent(l->backend->owner, l->id, 1);
}
static void notify_write(int fd, void* arg) {
    struct bluez_link* l = arg;
    if (!l->buffered) { uasync_set_socket_write(l->backend->ua, l->io[l->channel], 0); return; }
    ssize_t n = send(fd, l->value, l->value_size, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n != (ssize_t)l->value_size) {
        DEBUG_ERROR(CAT, "BLE notification socket write failed link=%llu channel=%d expected=%zu sent=%zd errno=%d",
                    (unsigned long long)l->id, l->channel, l->value_size, n, n < 0 ? errno : 0);
        link_error(l, "notification socket write failed"); return;
    }
    l->buffered = 0; uasync_set_socket_write(l->backend->ua, l->io[l->channel], 0);
    if (l->channel != BLE_WIRE_INVITE) {
        l->sending = 0;
        ble_backend_sent(l->backend->owner, l->id, 1);
    }
}
static int options(sd_bus_message* m, const char** device, uint16_t* mtu, uint16_t* offset) {
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0) return r;
    const char *key, *type;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        if ((r = sd_bus_message_read(m, "s", &key)) < 0 || (r = sd_bus_message_peek_type(m, NULL, &type)) < 0) return r;
        if ((r = sd_bus_message_enter_container(m, 'v', type)) < 0) return r;
        if (!strcmp(key, "device") && !strcmp(type, "o")) r = sd_bus_message_read(m, "o", device);
        else if (!strcmp(key, "mtu") && !strcmp(type, "q")) r = sd_bus_message_read(m, "q", mtu);
        else if (!strcmp(key, "offset") && !strcmp(type, "q")) r = sd_bus_message_read(m, "q", offset);
        else r = sd_bus_message_skip(m, type);
        if (r < 0) return r;
        sd_bus_message_exit_container(m); sd_bus_message_exit_container(m);
    }
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}
static int reject(sd_bus_message* m, const char* reason) {
    DEBUG_WARN(CAT, "BLE BlueZ request rejected: %s", reason);
    int r = sd_bus_reply_method_errorf(m, "org.bluez.Error.Failed", "%s", reason);
    if (r < 0) DEBUG_ERROR(CAT, "BLE BlueZ error reply failed: %s", strerror(-r));
    return r;
}
static int write_value(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct characteristic* ctx = arg; struct ble_backend* b = ctx->backend;
    const void* data; size_t len; const char* device = NULL; uint16_t mtu = 23, offset = 0;
    if (!sd_bus_message_get_sender(m) || strcmp(sd_bus_message_get_sender(m), b->daemon_owner)) return reject(m, "sender is not BlueZ");
    if (ctx->tx == 1 || sd_bus_message_read_array(m, 'y', &data, &len) < 0 || options(m, &device, &mtu, &offset) < 0 ||
        !device || offset || !len || len > BLE_VALUE_MAX || mtu < 23) return reject(m, "invalid WriteValue");
    struct bluez_link* l = incoming(b, device);
    if (!l) return reject(m, "connection unavailable");
    ble_link_id id = l->id;
    enum ble_wire_channel channel = ctx->tx >= 2 ? (enum ble_wire_channel)(ctx->tx - 1) : BLE_WIRE_DATA;
    ble_backend_received(b->owner, id, channel, data, len);
    if (!find_link(b, id)) return reject(m, "session rejected");
    int r = sd_bus_reply_method_return(m, "");
    if (r < 0) DEBUG_ERROR(CAT, "BLE BlueZ WriteValue reply failed link=%llu: %s", (unsigned long long)id, strerror(-r));
    return r;
}
static int acquire_notify(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct characteristic* ctx = arg; struct ble_backend* b = ctx->backend;
    const char* device = NULL; uint16_t mtu = 23, offset = 0;
    if (!sd_bus_message_get_sender(m) || strcmp(sd_bus_message_get_sender(m), b->daemon_owner)) return reject(m, "sender is not BlueZ");
    if (!ctx->tx || options(m, &device, &mtu, &offset) < 0 || !device || offset || mtu < 23) return reject(m, "invalid AcquireNotify");
    int channel = ctx->tx >= 2 ? ctx->tx - 1 : BLE_WIRE_DATA;
    struct bluez_link* l = find_device(b, device);
    /* BlueZ уже создал новую подписку, но HUP прежнего fd может ещё ждать обработки в uasync. */
    if (l && !l->outgoing && l->fd[channel] >= 0) {
        DEBUG_INFO(CAT, "BLE BlueZ subscriptions replaced link=%llu channel=%d device=%s", (unsigned long long)l->id, channel, device);
        ble_backend_closed(b->owner, l->id, "subscriptions replaced");
    }
    l = incoming(b, device);
    if (!l || l->fd[channel] >= 0) return reject(m, "subscription unavailable");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) < 0) {
        DEBUG_ERROR(CAT, "BLE notification socketpair failed: %s", strerror(errno)); return reject(m, "socketpair failed");
    }
    l->fd[channel] = pair[0]; l->mtu = mtu;
    l->io[channel] = uasync_add_socket(b->ua, l->fd[channel], notify_read, notify_write, notify_read, "ble_notify", l);
    if (!l->io[channel]) { close(pair[0]); close(pair[1]); l->fd[channel] = -1; return reject(m, "fd registration failed"); }
    uasync_set_socket_write(b->ua, l->io[channel], 0);
    int r = sd_bus_reply_method_return(m, "hq", pair[1], mtu); close(pair[1]);
    if (r < 0) {
        DEBUG_ERROR(CAT, "BLE BlueZ AcquireNotify reply failed link=%llu: %s", (unsigned long long)l->id, strerror(-r));
        link_error(l, "AcquireNotify reply failed"); return r;
    }
    DEBUG_DEBUG(CAT, "BLE BlueZ subscription link=%llu channel=%d mode=%s mtu=%u",
                (unsigned long long)l->id, channel, channel == BLE_WIRE_INVITE ? "indication" : "notification", mtu);
    if (l->fd[0] >= 0 && l->fd[1] >= 0 && l->fd[2] >= 0 && !l->opened) {
        l->opened = 1; ble_backend_opened(b->owner, l->id, mtu);
    }
    bus_update(b); return r;
}
static int get_service(sd_bus* bus, const char* path, const char* interface, const char* property,
                       sd_bus_message* reply, void* arg, sd_bus_error* error) {
    (void)bus; (void)path; (void)interface; (void)error; struct ble_backend* b = arg;
    return !strcmp(property, "UUID") ? sd_bus_message_append(reply, "s", b->service_uuid) : sd_bus_message_append(reply, "b", 1);
}
static int get_char(sd_bus* bus, const char* path, const char* interface, const char* property,
                    sd_bus_message* reply, void* arg, sd_bus_error* error) {
    (void)bus; (void)path; (void)interface; (void)error; struct characteristic* ctx = arg;
    if (!strcmp(property, "UUID")) return sd_bus_message_append(reply, "s",
        ctx->tx == 5 ? BLE_NODE_UUID : ctx->tx == 4 ? BLE_NAME_UUID : ctx->tx == 3 ? BLE_ACK_UUID :
        ctx->tx == 2 ? BLE_INVITE_UUID : ctx->tx ? BLE_TX_UUID : BLE_RX_UUID);
    if (!strcmp(property, "Service")) return sd_bus_message_append(reply, "o", SERVICE);
    if (!strcmp(property, "Flags") && ctx->tx >= 4) return sd_bus_message_append(reply, "as", 1, "read");
    if (!strcmp(property, "Flags") && ctx->tx == 2) return sd_bus_message_append(reply, "as", 2, "write", "indicate");
    if (!strcmp(property, "Flags") && ctx->tx == 3) return sd_bus_message_append(reply, "as", 2, "write-without-response", "notify");
    if (!strcmp(property, "Flags")) return sd_bus_message_append(reply, "as", 1, ctx->tx ? "notify" : "write-without-response");
    return sd_bus_message_append(reply, "b", 0); /* NotifyAcquired объявляет поддержку адресного fd API. */
}
static int read_profile(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct characteristic* ctx = arg; struct ble_backend* b = ctx->backend;
    const char* device = NULL; uint16_t mtu = 23, offset = 0;
    if (!sd_bus_message_get_sender(m) || strcmp(sd_bus_message_get_sender(m), b->daemon_owner) ||
        options(m, &device, &mtu, &offset) < 0 || !device || mtu < 23) return reject(m, "invalid profile ReadValue");
    const uint8_t* value = ble_backend_public_key(b->owner); size_t length = BLE_PUBLIC_KEY_SIZE;
    if (ctx->tx == 4) {
        unsigned slot = BLE_LINKS_MAX;
        for (unsigned i = 0; i < BLE_LINKS_MAX; i++) if (!strcmp(b->name_reads[i].device, device)) { slot = i; break; }
        if (offset == 0) {
            if (slot == BLE_LINKS_MAX) {
                slot = b->name_cursor++ % BLE_LINKS_MAX;
                if (b->name_reads[slot].device[0])
                    DEBUG_WARN(CAT, "BLE NODE_NAME snapshot capacity=%u replacing device=%s", BLE_LINKS_MAX, b->name_reads[slot].device);
            }
            snprintf(b->name_reads[slot].device, sizeof(b->name_reads[slot].device), "%s", device);
            strcpy(b->name_reads[slot].name, ble_backend_name(b->owner));
        }
        if (slot == BLE_LINKS_MAX) return reject(m, "NODE_NAME continuation without initial read");
        value = (const uint8_t*)b->name_reads[slot].name; length = strlen((const char*)value);
    }
    if (offset > length) {
        DEBUG_WARN(CAT, "BLE profile ReadValue invalid offset=%u length=%zu", offset, length);
        return sd_bus_reply_method_errorf(m, "org.bluez.Error.InvalidOffset", "Offset exceeds profile value");
    }
    size_t size = length - offset; if (size > (size_t)mtu - 1) size = mtu - 1;
    DEBUG_DEBUG(CAT, "BLE profile read characteristic=%s device=%s offset=%u bytes=%zu total=%zu",
                ctx->tx == 4 ? "NODE_NAME" : "NODE_ID", device, offset, size, length);
    sd_bus_message* reply = NULL;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r >= 0) r = sd_bus_message_append_array(reply, 'y', value + offset, size);
    if (r >= 0) r = sd_bus_send(b->bus, reply, NULL);
    sd_bus_message_unref(reply);
    if (r < 0) DEBUG_ERROR(CAT, "BLE profile reply failed: %s", strerror(-r));
    return r;
}
static int description_property(sd_bus* bus, const char* path, const char* interface, const char* property,
                                sd_bus_message* reply, void* arg, sd_bus_error* error) {
    (void)bus; (void)path; (void)interface; (void)error; struct characteristic* ctx = arg;
    if (!strcmp(property, "UUID")) return sd_bus_message_append(reply, "s", "00002901-0000-1000-8000-00805f9b34fb");
    if (!strcmp(property, "Characteristic")) return sd_bus_message_append(reply, "o", ctx->tx == 4 ? NAME : NODE);
    return sd_bus_message_append(reply, "as", 1, "read");
}
static int read_description(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct characteristic* ctx = arg;
    const char* value = ctx->tx == 4 ? "NODE_NAME" : "NODE_ID";
    const char* device = NULL; uint16_t mtu = 23, offset = 0;
    if (options(m, &device, &mtu, &offset) < 0 || offset > strlen(value)) return reject(m, "invalid description read");
    sd_bus_message* reply = NULL; int r = sd_bus_message_new_method_return(m, &reply);
    if (r >= 0) r = sd_bus_message_append_array(reply, 'y', value + offset, strlen(value) - offset);
    if (r >= 0) r = sd_bus_send(ctx->backend->bus, reply, NULL);
    sd_bus_message_unref(reply);
    if (r < 0) DEBUG_ERROR(CAT, "BLE description reply failed: %s", strerror(-r));
    return r;
}
static const sd_bus_vtable profile_table[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("UUID", "s", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("ReadValue", "a{sv}", "ay", read_profile, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END
};
static const sd_bus_vtable description_table[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("UUID", "s", description_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Characteristic", "o", description_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", description_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("ReadValue", "a{sv}", "ay", read_description, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END
};
static int get_adv(sd_bus* bus, const char* path, const char* interface, const char* property,
                   sd_bus_message* reply, void* arg, sd_bus_error* error) {
    (void)bus; (void)path; (void)interface; (void)error;
    struct advertisement* a = arg; struct ble_backend* b = a->backend;
    if (!strcmp(property, "Type")) return sd_bus_message_append(reply, "s", "peripheral");
    if (!strcmp(property, "ServiceUUIDs")) return b->data_size ? sd_bus_message_append(reply, "as", 1, b->service_uuid) :
                                                                sd_bus_message_append(reply, "as", 0);
    if (!strcmp(property, "TxPower")) {
        DEBUG_DEBUG(CAT, "BLE advertising TX power requested=%d dBm UUID=%s; actual power selected by controller",
                    ADV_TX_POWER_MAX_DBM, b->service_uuid);
        return sd_bus_message_append(reply, "n", ADV_TX_POWER_MAX_DBM);
    }
    return -EINVAL;
}
static int adv_release(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct advertisement* a = arg;
    DEBUG_WARN(CAT, "BLE advertisement released by BlueZ UUID=%s", a->backend->service_uuid);
    a->backend->beacon_registered = 0;
    ble_backend_advertising(a->backend->owner, BLE_CLOSED, "advertisement released");
    return sd_bus_reply_method_return(m, "");
}
static const sd_bus_vtable service_table[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("UUID", "s", get_service, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Primary", "b", get_service, 0, SD_BUS_VTABLE_PROPERTY_CONST), SD_BUS_VTABLE_END
};
static const sd_bus_vtable rx_table[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("UUID", "s", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("WriteValue", "aya{sv}", "", write_value, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END
};
static const sd_bus_vtable tx_table[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("UUID", "s", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("NotifyAcquired", "b", get_char, 0, 0),
    SD_BUS_METHOD("AcquireNotify", "a{sv}", "hq", acquire_notify, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END
};
static const sd_bus_vtable invite_table[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("WriteValue", "aya{sv}", "", write_value, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("UUID", "s", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", get_char, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("NotifyAcquired", "b", get_char, 0, 0),
    SD_BUS_METHOD("AcquireNotify", "a{sv}", "hq", acquire_notify, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END
};
static const sd_bus_vtable beacon_table[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("Type", "s", get_adv, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ServiceUUIDs", "as", get_adv, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("TxPower", "n", get_adv, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Release", "", "", adv_release, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END
};
static int subscribed(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct bluez_link* l = arg; l->subscribe = sd_bus_slot_unref(l->subscribe);
    if (response_error(m, "StartNotify")) { link_error(l, "subscription failed"); return 0; }
    if (++l->subscribed < BLE_WIRE_COUNT) {
        const char* path = l->subscribed == BLE_WIRE_INVITE ? l->invite : l->ack;
        int r = sd_bus_call_method_async(l->backend->bus, &l->subscribe, "org.bluez", path, CHAR_IF,
                                        "StartNotify", subscribed, l, "");
        if (r < 0) link_error(l, "control subscription failed");
        return 0;
    }
    DEBUG_INFO(CAT, "BLE BlueZ DATA configured link=%llu write=without_response receive=notification mtu=%u; PHY/interval managed by OS/peer",
               (unsigned long long)l->id, l->mtu);
    l->opened = 1; ble_backend_opened(l->backend->owner, l->id, l->mtu); return 0;
}
/* Snapshot содержит также старые объявления; только live-событие обновляет discovery/last_seen. */
static int properties(struct ble_backend* b, const char* path, const char* interface, sd_bus_message* m, enum object_read mode) {
    const char *key, *type, *uuid = NULL;
    int r, resolved = -1, connected = -1, rssi = 127, have_uuids = 0;
    uint64_t node = 0;
    uint16_t mtu = 0;
    uint8_t data[BLE_BROADCAST_MAX];
    if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        if ((r = sd_bus_message_read(m, "s", &key)) < 0 || (r = sd_bus_message_peek_type(m, NULL, &type)) < 0) return r;
        if ((r = sd_bus_message_enter_container(m, 'v', type)) < 0) return r;
        if (!strcmp(interface, DEVICE_IF) && !strcmp(key, "UUIDs") && !strcmp(type, "as")) {
            have_uuids = 1;
            if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0) return r;
            const char* service;
            while ((r = sd_bus_message_read(m, "s", &service)) > 0) {
                uint64_t id; if (!ble_service_uuid_decode(service, &id)) node = id;
            }
            if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0) return r;
        } else if (!strcmp(interface, DEVICE_IF) && !strcmp(key, "Name") && !strcmp(type, "s")) {
            r = sd_bus_message_skip(m, "s");
        } else if (!strcmp(interface, DEVICE_IF) && !strcmp(key, "RSSI") && !strcmp(type, "n")) {
            int16_t value; r = sd_bus_message_read(m, "n", &value); rssi = value;
        } else if (!strcmp(interface, DEVICE_IF) && !strcmp(type, "b") && (!strcmp(key, "Connected") || !strcmp(key, "ServicesResolved"))) {
            int value; r = sd_bus_message_read(m, "b", &value);
            if (!strcmp(key, "Connected")) connected = value; else resolved = value;
        } else if ((!strcmp(interface, CHAR_IF) || !strcmp(interface, "org.bluez.GattService1")) && !strcmp(key, "UUID") && !strcmp(type, "s")) r = sd_bus_message_read(m, "s", &uuid);
        else if (!strcmp(interface, CHAR_IF) && !strcmp(key, "MTU") && !strcmp(type, "q")) r = sd_bus_message_read(m, "q", &mtu);
        else if (!strcmp(interface, CHAR_IF) && !strcmp(key, "Value") && !strcmp(type, "ay")) {
            const void* bytes; size_t size; r = sd_bus_message_read_array(m, 'y', &bytes, &size);
            if (r >= 0) for (int i = 0; i < BLE_LINKS_MAX; i++) {
                struct bluez_link* l = &b->links[i];
                if (mode == OBJECT_EVENT && size && l->id && l->opened && l->outgoing &&
                    (!strcmp(l->tx, path) || !strcmp(l->invite, path) || !strcmp(l->ack, path)))
                    ble_backend_received(b->owner, l->id, !strcmp(l->ack, path) ? BLE_WIRE_ACK :
                        !strcmp(l->invite, path) ? BLE_WIRE_INVITE : BLE_WIRE_DATA, bytes, size);
            }
        } else r = sd_bus_message_skip(m, type);
        if (r < 0) return r;
        sd_bus_message_exit_container(m); sd_bus_message_exit_container(m);
    }
    if (r < 0) return r;
    sd_bus_message_exit_container(m);
    if (mode == OBJECT_SELECT_CONNECTED) {
        size_t prefix = strlen(b->adapter);
        if (strcmp(interface, DEVICE_IF) || connected != 1 || !node || strlen(path) >= BLE_ENDPOINT_MAX ||
            strncmp(path, b->adapter, prefix) || path[prefix] != '/' ||
            (find_device(b, path) && find_device(b, path)->outgoing &&
             !ble_backend_profile_only(b->owner, find_device(b, path)->id))) return 0;
        for (int i = 0; i < BLE_LINKS_MAX; i++) {
            struct bluez_link* l = &b->links[i];
            if (!l->id || !l->outgoing || l->opened || l->connected || l->connect || !l->query ||
                ble_backend_peer(b->owner, l->id) != node) continue;
            DEBUG_INFO(CAT, "BLE BlueZ reuse connected peer=%016llx link=%llu requested=%s connected=%s resolved=%d",
                       (unsigned long long)node, (unsigned long long)l->id, l->device, path, resolved);
            strcpy(l->device, path); l->service[0] = l->name[0] = l->node[0] = 0; l->profile_read = 0;
            l->rx[0] = l->tx[0] = l->invite[0] = l->ack[0] = 0;
            l->connected = 1; l->resolved = resolved == 1; l->mtu = 23; l->mtu_known = 0;
            break;
        }
        return 0;
    }
    if (mode == OBJECT_SELECT_SERVICE) {
        uint64_t id;
        if (!uuid || strcmp(interface, "org.bluez.GattService1") || ble_service_uuid_decode(uuid, &id)) return 0;
        for (int i = 0; i < BLE_LINKS_MAX; i++) {
            struct bluez_link* l = &b->links[i]; size_t prefix = strlen(l->device);
            if (l->id && l->outgoing && !l->opened && strlen(path) < sizeof(l->service) &&
                !strncmp(path, l->device, prefix) && path[prefix] == '/' && ble_backend_peer(b->owner, l->id) == id)
                strcpy(l->service, path);
        }
        return 0;
    }
    if (!strcmp(interface, "org.bluez.Adapter1") && !b->adapter[0]) {
        const char* name = strrchr(path, '/');
        if (strlen(path) < sizeof(b->adapter) && (!b->adapter_name[0] || (name && !strcmp(name + 1, b->adapter_name)))) strcpy(b->adapter, path);
    }
    if (!strcmp(interface, DEVICE_IF)) {
        for (int i = 0; i < BLE_LINKS_MAX; i++) {
            struct bluez_link* l = &b->links[i];
            if (!l->id || strcmp(l->device, path)) continue;
            if ((connected == 0 && (!l->outgoing || l->connected)) || (resolved == 0 && l->resolved))
                link_error(l, connected == 0 ? "device disconnected" : "GATT services invalidated");
            else if (l->outgoing && (connected >= 0 || resolved >= 0)) {
                if (connected >= 0) l->connected = connected;
                if (resolved >= 0) l->resolved = resolved;
                DEBUG_DEBUG(CAT, "BLE BlueZ readiness link=%llu connected=%d resolved=%d opened=%d",
                            (unsigned long long)l->id, l->connected, l->resolved, l->opened);
                if (l->connected && l->resolved && !l->opened && !l->query && query_link(l) < 0)
                    link_error(l, "discovery request failed");
            }
        }
        /* Snapshot запоминает UUID кешированного Device1, но не выдаёт его за свежий эфирный пакет.
         * BlueZ не обязан повторять неизменившиеся UUIDs: следующий живой RSSI/Data использует этот кеш. */
        if (((mode == OBJECT_EVENT && (rssi != 127 || have_uuids)) ||
             (mode == OBJECT_SNAPSHOT && node)) && strlen(path) < BLE_ENDPOINT_MAX) {
            int slot = -1, oldest = 0;
            for (int i = 0; i < 64; i++) {
                if (!strcmp(b->peers[i].device, path)) { slot = i; break; }
                if (b->peers[i].seen < b->peers[oldest].seen) oldest = i;
            }
            if (slot < 0 && node) slot = oldest;
            if (slot >= 0) {
                if (have_uuids && !node) { memset(&b->peers[slot], 0, sizeof(b->peers[slot])); return 0; }
                if (node && b->peers[slot].identity.node_id != node) {
                    memset(&b->peers[slot], 0, sizeof(b->peers[slot]));
                    strcpy(b->peers[slot].device, path); b->peers[slot].identity.node_id = node;
                    DEBUG_DEBUG(CAT, "BLE BlueZ service identity cached node=%016llx endpoint=%s source=%s",
                                (unsigned long long)node, path, mode == OBJECT_EVENT ? "live" : "snapshot");
                }
                struct ble_identity* identity = &b->peers[slot].identity;
                int n = ble_identity_encode(data, identity->node_id, identity->name);
                if (n > 0 && mode == OBJECT_EVENT) {
                    if (!b->peers[slot].seen)
                        DEBUG_INFO(CAT, "BLE BlueZ service discovered node=%016llx endpoint=%s",
                                   (unsigned long long)identity->node_id, path);
                    struct ble_endpoint ep = {0}; strcpy(ep.id, path); b->peers[slot].seen = get_time_us();
                    ble_backend_broadcast(b->owner, &ep, data, (size_t)n, rssi);
                }
            }
        }
    }
    if (uuid) for (int i = 0; i < BLE_LINKS_MAX; i++) {
        struct bluez_link* l = &b->links[i]; size_t prefix = strlen(l->service);
        if (!l->id || !l->outgoing || l->opened || !prefix || strncmp(path, l->service, prefix) || path[prefix] != '/' || strlen(path) >= BLE_ENDPOINT_MAX) continue;
        if (!strcmp(uuid, BLE_RX_UUID)) {
            strcpy(l->rx, path);
            if (mtu >= 23) { l->mtu = mtu; l->mtu_known = 1; }
        }
        if (!strcmp(uuid, BLE_TX_UUID)) strcpy(l->tx, path);
        if (!strcmp(uuid, BLE_INVITE_UUID)) strcpy(l->invite, path);
        if (!strcmp(uuid, BLE_ACK_UUID)) strcpy(l->ack, path);
        if (!strcmp(uuid, BLE_NAME_UUID)) strcpy(l->name, path);
        if (!strcmp(uuid, BLE_NODE_UUID)) strcpy(l->node, path);
    }
    return 0;
}
static int interfaces(struct ble_backend* b, const char* path, sd_bus_message* m, enum object_read mode) {
    int r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}"); const char* iface;
    if (r < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
        if ((r = sd_bus_message_read(m, "s", &iface)) < 0 || (r = properties(b, path, iface, m, mode)) < 0) return r;
        sd_bus_message_exit_container(m);
    }
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}
static int managed_objects(struct ble_backend* b, sd_bus_message* m, enum object_read mode) {
    int r = sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}"); const char* path;
    if (r < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
        if ((r = sd_bus_message_read(m, "o", &path)) < 0 || (r = interfaces(b, path, m, mode)) < 0) return r;
        sd_bus_message_exit_container(m);
    }
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}
static int profile_name_read(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct bluez_link* l = arg; l->query = sd_bus_slot_unref(l->query);
    if (response_error(m, "NODE_NAME ReadValue")) { link_error(l, "NODE_NAME read failed"); return 0; }
    const void* data; size_t size;
    if (sd_bus_message_read_array(m, 'y', &data, &size) < 0) { link_error(l, "invalid NODE_NAME reply"); return 0; }
    ble_link_id id = l->id;
    int result = ble_backend_profile(l->backend->owner, id, l->profile_node, sizeof(l->profile_node), data, size);
    if (result < 0 || l->id != id) return 0; /* Callback мог начать обычный connect вместо profile-only. */
    l->profile_read = 1;
    if (result > 0) return 0;
    if (query_link(l) < 0) link_error(l, "discovery after profile failed");
    return 0;
}
static int profile_node_read(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct bluez_link* l = arg; l->query = sd_bus_slot_unref(l->query);
    if (response_error(m, "NODE_ID ReadValue")) { link_error(l, "NODE_ID read failed"); return 0; }
    const void* data; size_t size;
    if (sd_bus_message_read_array(m, 'y', &data, &size) < 0 || size != BLE_PUBLIC_KEY_SIZE) {
        link_error(l, "invalid GATT public key size"); return 0;
    }
    memcpy(l->profile_node, data, sizeof(l->profile_node));
    int r = sd_bus_call_method_async(l->backend->bus, &l->query, "org.bluez", l->name, CHAR_IF,
                                    "ReadValue", profile_name_read, l, "a{sv}", 0);
    if (r < 0) link_error(l, "NODE_NAME read request failed");
    return 0;
}
static int queried(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct bluez_link* l = arg;
    /* Оставляем slot ненулевым при разборе, чтобы ServicesResolved не породил рекурсивный запрос. */
    int r = response_error(m, "GetManagedObjects") ? -EIO : managed_objects(l->backend, m, OBJECT_SELECT_CONNECTED);
    if (r >= 0) r = sd_bus_message_rewind(m, 1);
    if (r >= 0) r = managed_objects(l->backend, m, OBJECT_SELECT_SERVICE);
    if (r >= 0) r = sd_bus_message_rewind(m, 1);
    if (r >= 0) r = managed_objects(l->backend, m, OBJECT_SNAPSHOT);
    l->query = sd_bus_slot_unref(l->query);
    if (!l->id) return 0;
    if (r < 0) { link_error(l, "service discovery failed"); return 0; }
    DEBUG_DEBUG(CAT, "BLE BlueZ GATT snapshot link=%llu connected=%d resolved=%d RX=%d TX=%d INVITE=%d ACK=%d mtu=%u known=%d",
                (unsigned long long)l->id, l->connected, l->resolved, !!l->rx[0], !!l->tx[0], !!l->invite[0], !!l->ack[0], l->mtu, l->mtu_known);
    /* Connect уже подключённого dual-mode Device1 может запустить BR/EDR вместо использования готового LE. */
    if (!l->connected && !l->connect) {
        DEBUG_INFO(CAT, "BLE BlueZ Connect requested link=%llu device=%s: no existing connection", (unsigned long long)l->id, l->device);
        r = sd_bus_call_method_async(l->backend->bus, &l->connect, "org.bluez", l->device, DEVICE_IF, "Connect", connected, l, "");
        if (r < 0) {
            DEBUG_ERROR(CAT, "BLE Connect request link=%llu: %s", (unsigned long long)l->id, strerror(-r));
            link_error(l, "connect request failed");
        }
        return 0;
    }
    if (l->connected) DEBUG_DEBUG(CAT, "BLE BlueZ reusing connection link=%llu resolved=%d", (unsigned long long)l->id, l->resolved);
    if (l->connected && l->resolved && l->rx[0] && l->tx[0] && l->invite[0] && l->ack[0] && !l->subscribe && !l->opened) {
        if (!l->profile_read) {
            if (!l->name[0] || !l->node[0]) { link_error(l, "NODE_NAME/NODE_ID characteristics missing"); return 0; }
            r = sd_bus_call_method_async(l->backend->bus, &l->query, "org.bluez", l->node, CHAR_IF,
                                         "ReadValue", profile_node_read, l, "a{sv}", 0);
            if (r < 0) link_error(l, "NODE_ID read request failed");
            return 0;
        }
        if (!l->mtu_known) DEBUG_WARN(CAT, "BLE BlueZ MTU unavailable link=%llu: using ATT minimum 23", (unsigned long long)l->id);
        r = sd_bus_call_method_async(l->backend->bus, &l->subscribe, "org.bluez", l->tx, CHAR_IF, "StartNotify", subscribed, l, "");
        if (r < 0) link_error(l, "StartNotify request failed");
    }
    return 0;
}
/* Сначала snapshot: существующее LE соединение не требует повторного Device1.Connect. */
static int query_link(struct bluez_link* l) {
    if (l->query || l->subscribe || l->opened || (l->profile_read && ble_backend_profile_only(l->backend->owner, l->id))) return 0;
    int r = sd_bus_call_method_async(l->backend->bus, &l->query, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
                                     "GetManagedObjects", queried, l, "");
    if (r < 0) DEBUG_ERROR(CAT, "BLE discovery request link=%llu: %s", (unsigned long long)l->id, strerror(-r));
    return r;
}
static int connected(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct bluez_link* l = arg; l->connect = sd_bus_slot_unref(l->connect);
    if (response_error(m, "Connect")) { link_error(l, "connect failed"); return 0; }
    if (query_link(l) < 0) link_error(l, "discovery request failed");
    return 0;
}
static int signal_changed(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct ble_backend* b = arg; const char* iface;
    int r = sd_bus_message_read(m, "s", &iface);
    if (r >= 0) r = properties(b, sd_bus_message_get_path(m), iface, m, OBJECT_EVENT);
    if (r < 0) DEBUG_WARN(CAT, "BLE invalid BlueZ PropertiesChanged: %s", strerror(-r));
    return 0;
}
static int object_added(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct ble_backend* b = arg; const char* path;
    int r = sd_bus_message_read(m, "o", &path);
    if (r >= 0) r = interfaces(b, path, m, OBJECT_EVENT);
    if (r < 0) DEBUG_WARN(CAT, "BLE invalid BlueZ InterfacesAdded: %s", strerror(-r));
    return 0;
}
static int object_removed(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct ble_backend* b = arg; const char* path;
    if (sd_bus_message_read(m, "o", &path) < 0) { DEBUG_WARN(CAT, "BLE invalid InterfacesRemoved"); return 0; }
    if (!strcmp(path, b->adapter)) { fail(b, "Bluetooth adapter removed", ENODEV); return 0; }
    for (int i = 0; i < BLE_LINKS_MAX; i++) {
        struct bluez_link* l = &b->links[i];
        if (l->id && (!strcmp(path, l->device) || !strcmp(path, l->rx) || !strcmp(path, l->tx) ||
            !strcmp(path, l->invite) || !strcmp(path, l->ack))) link_error(l, "BlueZ object removed");
    }
    return 0;
}
static int owner_changed(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; const char *name, *old_owner, *new_owner;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) >= 0 && !new_owner[0]) fail(arg, "BlueZ stopped", ECONNRESET);
    return 0;
}
/* GetAll выбранного адаптера: пропущенные свойства остаются неизвестными. */
static int read_advertising_power(sd_bus_message* m, struct ble_advertising_power* power) {
    const char *key, *type;
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        if ((r = sd_bus_message_read(m, "s", &key)) < 0 || (r = sd_bus_message_peek_type(m, NULL, &type)) < 0) return r;
        if ((r = sd_bus_message_enter_container(m, 'v', type)) < 0) return r;
        if (!strcmp(key, "SupportedFeatures") && !strcmp(type, "as")) {
            if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0) return r;
            power->can_set = 0;
            const char* feature;
            while ((r = sd_bus_message_read(m, "s", &feature)) > 0)
                if (!strcmp(feature, "CanSetTxPower")) power->can_set = 1;
            if (r < 0) return r;
            if ((r = sd_bus_message_exit_container(m)) < 0) return r;
        } else if (!strcmp(key, "SupportedCapabilities") && !strcmp(type, "a{sv}")) {
            if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0) return r;
            while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
                if ((r = sd_bus_message_read(m, "s", &key)) < 0 || (r = sd_bus_message_peek_type(m, NULL, &type)) < 0) return r;
                if ((r = sd_bus_message_enter_container(m, 'v', type)) < 0) return r;
                if (!strcmp(key, "MaxTxPower") && !strcmp(type, "n")) {
                    int16_t value;
                    if ((r = sd_bus_message_read(m, "n", &value)) < 0) return r;
                    if (value < -127 || value > 20) return -ERANGE;
                    power->max_dbm = value;
                } else if ((r = sd_bus_message_skip(m, type)) < 0) return r;
                if ((r = sd_bus_message_exit_container(m)) < 0 || (r = sd_bus_message_exit_container(m)) < 0) return r;
            }
            if (r < 0) return r;
            if ((r = sd_bus_message_exit_container(m)) < 0) return r;
        } else if ((r = sd_bus_message_skip(m, type)) < 0) return r;
        if ((r = sd_bus_message_exit_container(m)) < 0 || (r = sd_bus_message_exit_container(m)) < 0) return r;
    }
    return r < 0 ? r : sd_bus_message_exit_container(m);
}
static int setup_reply(sd_bus_message* m, void* arg, sd_bus_error* error);
static int power_reply(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct ble_backend* b = arg; b->setup = sd_bus_slot_unref(b->setup);
    struct ble_advertising_power power = {127, -1};
    const sd_bus_error* e = sd_bus_message_get_error(m);
    int r = e ? -EIO : read_advertising_power(m, &power);
    if (r < 0) {
        DEBUG_WARN(CAT, "BLE advertising power capabilities unavailable: %s", e ? e->name : strerror(-r));
        power = (struct ble_advertising_power){127, -1};
    }
    ble_backend_advertising_power(b->owner, power);
    if (power.can_set != 1) DEBUG_WARN(CAT, "BLE advertising maximum TX power request may be ignored: CanSetTxPower=%d", power.can_set);
    r = sd_bus_call_method_async(b->bus, &b->setup, "org.bluez", b->adapter, "org.bluez.GattManager1",
                                 "RegisterApplication", setup_reply, b, "oa{sv}", ROOT, 0);
    if (r < 0) fail(b, "BlueZ application registration request failed", r);
    return 0;
}
static int setup_reply(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct ble_backend* b = arg; b->setup = sd_bus_slot_unref(b->setup);
    if (response_error(m, "startup")) { fail(b, "BlueZ startup failed", EIO); return 0; }
    int r = 0;
    switch (b->step++) {
    case 0:
        snprintf(b->daemon_owner, sizeof(b->daemon_owner), "%s", sd_bus_message_get_sender(m));
        r = managed_objects(b, m, OBJECT_SNAPSHOT);
        if (r < 0 || !b->adapter[0]) { fail(b, "Bluetooth adapter unavailable", r < 0 ? r : ENODEV); return 0; }
        r = sd_bus_call_method_async(b->bus, &b->setup, "org.bluez", b->adapter, "org.freedesktop.DBus.Properties",
                                     "GetAll", power_reply, b, "s", "org.bluez.LEAdvertisingManager1"); break;
    case 1:
        r = sd_bus_call_method_async(b->bus, &b->setup, "org.bluez", b->adapter, "org.bluez.Adapter1",
                                     "SetDiscoveryFilter", setup_reply, b, "a{sv}", 2, "Transport", "s", "le", "DuplicateData", "b", 1); break;
    case 2:
        r = sd_bus_call_method_async(b->bus, &b->setup, "org.bluez", b->adapter, "org.bluez.Adapter1", "StartDiscovery", setup_reply, b, ""); break;
    case 3: beacon_update(b); ble_backend_status(b->owner, 1, "BlueZ scan/GATT ready"); break;
    }
    if (r < 0) fail(b, "BlueZ setup request failed", r);
    return 0;
}
static int beacon_reply(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct ble_backend* b = arg; b->beacon_op = sd_bus_slot_unref(b->beacon_op);
    if (response_error(m, "advertisement update")) { fail(b, "advertisement update failed", EIO); return 0; }
    b->beacon_registered = b->beacon_action == 1; b->beacon_action = 0;
    DEBUG_DEBUG(CAT, "BLE broadcast registered=%d bytes=%zu", b->beacon_registered, b->data_size);
    ble_backend_advertising(b->owner, b->beacon_registered && b->data_size ? BLE_READY : BLE_CLOSED, "BlueZ discovery publication applied");
    beacon_update(b); return 0;
}
static void beacon_update(struct ble_backend* b) {
    if (b->beacon_op || b->failed) return;
    int r;
    if (b->beacon_registered) {
        if (b->beacon_desired && b->next_size == b->data_size && !memcmp(b->data, b->next_data, b->data_size)) return;
        b->beacon_action = 2;
        r = sd_bus_call_method_async(b->bus, &b->beacon_op, "org.bluez", b->adapter, "org.bluez.LEAdvertisingManager1",
                                     "UnregisterAdvertisement", beacon_reply, b, "o", BEACON);
    } else {
        if (!b->beacon_desired) return;
        b->beacon_action = 1;
        b->data_size = b->next_size; memcpy(b->data, b->next_data, b->data_size);
        r = sd_bus_call_method_async(b->bus, &b->beacon_op, "org.bluez", b->adapter, "org.bluez.LEAdvertisingManager1",
                                     "RegisterAdvertisement", beacon_reply, b, "oa{sv}", BEACON, 0);
    }
    if (r < 0) fail(b, "advertisement request failed", r);
}
struct ble_backend* ble_backend_create(struct ble_transport* owner, struct UASYNC* ua, const struct ble_config* cfg) {
    struct ble_backend* b = u_calloc(1, sizeof(*b));
    if (!b) { DEBUG_ERROR(CAT, "BLE BlueZ allocation failed"); return NULL; }
    b->owner = owner; b->ua = ua;
    if (ble_service_uuid_encode(b->service_uuid, cfg->node_id)) { u_free(b); return NULL; }
    b->beacon_desired = 1; b->next_size = cfg->initial_advertisement_size ? 1 : 0;
    b->next_data[0] = 1;
    if (cfg->adapter) snprintf(b->adapter_name, sizeof(b->adapter_name), "%s", cfg->adapter);
    b->rx_context.backend = b; b->tx_context.backend = b; b->tx_context.tx = 1;
    b->invite_context.backend = b; b->invite_context.tx = 2;
    b->ack_context.backend = b; b->ack_context.tx = 3;
    b->name_context = (struct characteristic){b, 4}; b->node_context = (struct characteristic){b, 5};
    b->beacon_context.backend = b;
    int r = sd_bus_open_system(&b->bus);
    if (r >= 0) r = sd_bus_add_object_manager(b->bus, &b->manager, ROOT);
    if (r >= 0) r = sd_bus_add_object_vtable(b->bus, &b->service, SERVICE, "org.bluez.GattService1", service_table, b);
    if (r >= 0) r = sd_bus_add_object_vtable(b->bus, &b->rx, RX, CHAR_IF, rx_table, &b->rx_context);
    if (r >= 0) r = sd_bus_add_object_vtable(b->bus, &b->tx, TX, CHAR_IF, tx_table, &b->tx_context);
    if (r >= 0) r = sd_bus_add_object_vtable(b->bus, &b->invite, INVITE, CHAR_IF, invite_table, &b->invite_context);
    if (r >= 0) r = sd_bus_add_object_vtable(b->bus, &b->ack, ACK, CHAR_IF, invite_table, &b->ack_context);
    if (r >= 0) r = sd_bus_add_object_vtable(b->bus, &b->name, NAME, CHAR_IF, profile_table, &b->name_context);
    if (r >= 0) r = sd_bus_add_object_vtable(b->bus, &b->node, NODE, CHAR_IF, profile_table, &b->node_context);
    if (r >= 0) r = sd_bus_add_object_vtable(b->bus, &b->name_desc, NAME "/description", "org.bluez.GattDescriptor1",
                                            description_table, &b->name_context);
    if (r >= 0) r = sd_bus_add_object_vtable(b->bus, &b->node_desc, NODE "/description", "org.bluez.GattDescriptor1",
                                            description_table, &b->node_context);
    if (r >= 0) r = sd_bus_add_object_vtable(b->bus, &b->beacon, BEACON, "org.bluez.LEAdvertisement1", beacon_table, &b->beacon_context);
    if (r >= 0) r = sd_bus_add_match_async(b->bus, &b->signals,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'", signal_changed, NULL, b);
    if (r >= 0) r = sd_bus_add_match_async(b->bus, &b->objects,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'", object_added, NULL, b);
    if (r >= 0) r = sd_bus_add_match_async(b->bus, &b->removed,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved'", object_removed, NULL, b);
    if (r >= 0) r = sd_bus_add_match_async(b->bus, &b->owner_watch,
        "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
        owner_changed, NULL, b);
    if (r >= 0) r = sd_bus_call_method_async(b->bus, &b->setup, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
                                             "GetManagedObjects", setup_reply, b, "");
    if (r >= 0) { b->io = uasync_add_socket(ua, sd_bus_get_fd(b->bus), bus_io, bus_io, bus_io, "ble_dbus", b); if (!b->io) r = -ENOMEM; }
    if (r < 0) { DEBUG_ERROR(CAT, "BLE BlueZ create: %s", strerror(-r)); ble_backend_destroy(b); return NULL; }
    bus_update(b); return b;
}
void ble_backend_close(struct ble_backend* b, ble_link_id id) {
    struct bluez_link* l = find_link(b, id); if (!l) return;
    l->connect = sd_bus_slot_unref(l->connect); l->query = sd_bus_slot_unref(l->query);
    l->subscribe = sd_bus_slot_unref(l->subscribe); l->write = sd_bus_slot_unref(l->write);
    for (int channel = 0; channel < BLE_WIRE_COUNT; channel++) {
        if (l->io[channel]) uasync_remove_socket(b->ua, l->io[channel]);
        if (l->fd[channel] >= 0) close(l->fd[channel]);
    }
    /* Не разрываем общий Device1 ACL: он может принадлежать другим профилям/приложениям. */
    if (l->outgoing && !b->failed) for (int channel = 0; channel < l->subscribed; channel++) {
        const char* path = channel == BLE_WIRE_ACK ? l->ack : channel == BLE_WIRE_INVITE ? l->invite : l->tx;
        int r = sd_bus_call_method_async(b->bus, NULL, "org.bluez", path, CHAR_IF, "StopNotify", NULL, NULL, "");
        if (r < 0) DEBUG_WARN(CAT, "BLE StopNotify failed: %s", strerror(-r));
    }
    memset(l, 0, sizeof(*l)); bus_update(b);
}
void ble_backend_destroy(struct ble_backend* b) {
    if (!b) return;
    b->failed = 1;
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (b->links[i].id) ble_backend_close(b, b->links[i].id);
    if (b->timer) uasync_cancel_timeout(b->ua, b->timer);
    if (b->io) uasync_remove_socket(b->ua, b->io);
    sd_bus_slot_unref(b->setup); sd_bus_slot_unref(b->signals); sd_bus_slot_unref(b->objects); sd_bus_slot_unref(b->removed);
    sd_bus_slot_unref(b->owner_watch);
    sd_bus_slot_unref(b->service); sd_bus_slot_unref(b->rx); sd_bus_slot_unref(b->tx); sd_bus_slot_unref(b->invite);
    sd_bus_slot_unref(b->ack); sd_bus_slot_unref(b->manager);
    sd_bus_slot_unref(b->name); sd_bus_slot_unref(b->node); sd_bus_slot_unref(b->name_desc); sd_bus_slot_unref(b->node_desc);
    sd_bus_slot_unref(b->beacon); sd_bus_slot_unref(b->beacon_op);
    /* Закрытие приватного bus освобождает регистрации/scan сессии в BlueZ по owner-disconnect. */
    sd_bus_close_unref(b->bus); u_free(b);
}
int ble_backend_advertise(struct ble_backend* b, const uint8_t* data, size_t len) {
    if (b->failed) return BLE_NOT_READY;
    int visible = data && len;
    if (b->next_size == (size_t)visible) {
        ble_backend_advertising(b->owner, b->beacon_registered && b->data_size ? BLE_READY : BLE_CLOSED, "discovery unchanged");
        return BLE_OK;
    }
    b->next_size = visible; b->next_data[0] = 1;
    ble_backend_advertising(b->owner, BLE_STARTING, "updating service UUID visibility");
    beacon_update(b); bus_update(b); return BLE_OK;
}
int ble_backend_connect(struct ble_backend* b, ble_link_id id, const struct ble_endpoint* ep) {
    if (b->failed || strncmp(ep->id, b->adapter, strlen(b->adapter)) || !strstr(ep->id, "/dev_")) {
        DEBUG_WARN(CAT, "BLE invalid BlueZ endpoint=%s", ep->id); return BLE_ERROR;
    }
    struct bluez_link* l = alloc_link(b, id, ep->id, 1);
    if (!l) { DEBUG_WARN(CAT, "BLE BlueZ duplicate/limit endpoint=%s", ep->id); return BLE_ERROR; }
    int r = query_link(l);
    if (r < 0) { ble_backend_close(b, id); return BLE_ERROR; }
    bus_update(b); return BLE_OK;
}
static int written(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct bluez_link* l = arg; l->write = sd_bus_slot_unref(l->write); l->sending = 0;
    ble_backend_sent(l->backend->owner, l->id, !response_error(m, "WriteValue")); return 0;
}
int ble_backend_send(struct ble_backend* b, ble_link_id id, enum ble_wire_channel channel, const uint8_t* data, size_t len) {
    struct bluez_link* l = find_link(b, id);
    if ((unsigned)channel >= BLE_WIRE_COUNT || !l || !l->opened || l->sending || !len || len > BLE_VALUE_MAX) {
        DEBUG_ERROR(CAT, "BLE invalid backend send link=%llu", (unsigned long long)id); return BLE_ERROR;
    }
    l->channel = channel;
    if (!l->outgoing) {
        memcpy(l->value, data, len); l->value_size = len; l->buffered = l->sending = 1;
        uasync_set_socket_write(b->ua, l->io[channel], 1); return BLE_OK;
    }
    sd_bus_message* m = NULL;
    const char* path = channel == BLE_WIRE_ACK ? l->ack : channel == BLE_WIRE_INVITE ? l->invite : l->rx;
    int r = sd_bus_message_new_method_call(b->bus, &m, "org.bluez", path, CHAR_IF, "WriteValue");
    if (r >= 0) r = sd_bus_message_append_array(m, 'y', data, len);
    if (r >= 0) r = sd_bus_message_append(m, "a{sv}", 1, "type", "s", channel == BLE_WIRE_INVITE ? "request" : "command");
    if (r >= 0) r = sd_bus_call_async(b->bus, &l->write, m, written, l, 10000000);
    sd_bus_message_unref(m);
    if (r < 0) { DEBUG_ERROR(CAT, "BLE WriteValue request: %s", strerror(-r)); return BLE_ERROR; }
    DEBUG_TRACE(CAT, "BLE WriteValue submitted link=%llu channel=%d type=%s bytes=%zu", (unsigned long long)id, channel,
                channel == BLE_WIRE_INVITE ? "request" : "command", len);
    l->sending = 1; bus_update(b); return BLE_OK;
}

int ble_backend_set_name(struct ble_backend* b, const char* name) { (void)b; (void)name; return BLE_OK; }
