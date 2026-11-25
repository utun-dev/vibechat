#include "ble_fixture_key.h"
/* Private D-Bus + настоящий backend: DATA/ACK без ATT ACK, адресные INVITE indications, два клиента. */
#include "ble_backend.h"
#include "ble_identity.h"
#include "u_async.h"
#include "mem.h"
#include "debug_config.h"
#include <systemd/sd-bus.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define CHECK(v) do { if (!(v)) { fprintf(stderr, "FAIL line=%d: %s errno=%d\n", __LINE__, #v, errno); exit(1); } } while (0)
#define ROOT "/org/vibechat/ble"
#define ADAPTER "/org/bluez/hci0"
#define REMOTE ADAPTER "/dev_C"
#define REMOTE_SERVICE REMOTE "/service"
#define ROTATED ADAPTER "/dev_D"
static struct UASYNC* ua;
static sd_bus* bus;
static struct ble_transport* transport;
static char application[256];
static int ready, broadcasts, ready_links, advertisement_count, closed_links;
static int advertisement_uuid_count, advertisement_queries;
static ble_link_id links[4];
static unsigned commands, requests, ack_requests, ack_values;
static int ack_fds[2] = {-1, -1};
static int remote_resolved, remote_connected = 1;
static unsigned remote_connects;
static unsigned rotated_connects;
static uint64_t remote_identity;
static unsigned profile_callbacks, profile_reads;
static const char full_name[] = "Полное имя устройства GATT";
static char service_uuid[37];
struct reply { sd_bus_slot* slot; int done, fd, failed; uint8_t bytes[64]; size_t size; };
static void poll_all(void) {
    for (int i = 0; i < 500; i++) {
        CHECK(sd_bus_process(bus, NULL) >= 0); uasync_poll(ua, 10);
        for (int j = 0; j < 2; j++) if (ack_fds[j] >= 0) {
            uint8_t ack[8]; ssize_t n = recv(ack_fds[j], ack, sizeof(ack), MSG_DONTWAIT);
            if (n < 0) { CHECK(errno == EAGAIN); continue; }
            CHECK(n == 2); ack_values++; /* ACK notification не требует ATT confirmation. */
        }
    }
}
static int register_app(sd_bus_message* m, void* arg, sd_bus_error* e) {
    (void)arg; (void)e; snprintf(application, sizeof(application), "%s", sd_bus_message_get_sender(m));
    return sd_bus_reply_method_return(m, "");
}
static int simple(sd_bus_message* m, void* arg, sd_bus_error* e) { (void)arg; (void)e; return sd_bus_reply_method_return(m, ""); }
static int advertisement_properties(sd_bus_message* m, void* arg, sd_bus_error* e) {
    (void)arg; (void)e; CHECK(!sd_bus_message_is_method_error(m, NULL));
    CHECK(sd_bus_message_enter_container(m, 'a', "{sv}") > 0); const char *name, *type; int r;
    advertisement_uuid_count = 0; advertisement_queries++;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        CHECK(sd_bus_message_read(m, "s", &name) > 0 && sd_bus_message_peek_type(m, NULL, &type) > 0);
        CHECK(sd_bus_message_enter_container(m, 'v', type) > 0);
        CHECK(strcmp(name, "ManufacturerData")); /* Node endpoint объявляется одним service advertising. */
        if (!strcmp(name, "ServiceUUIDs")) {
            CHECK(sd_bus_message_enter_container(m, 'a', "s") > 0); const char* uuid;
            while ((r = sd_bus_message_read(m, "s", &uuid)) > 0) {
                uint64_t node; CHECK(!ble_service_uuid_decode(uuid, &node) && node == 200); advertisement_uuid_count++;
            }
            CHECK(r >= 0 && sd_bus_message_exit_container(m) >= 0);
        } else if (!strcmp(name, "Type")) {
            const char* value; CHECK(sd_bus_message_read(m, "s", &value) > 0 && !strcmp(value, "peripheral"));
        } else CHECK(sd_bus_message_skip(m, type) >= 0);
        CHECK(sd_bus_message_exit_container(m) >= 0 && sd_bus_message_exit_container(m) >= 0);
    }
    CHECK(r >= 0); return 0;
}
static int advertise(sd_bus_message* m, void* arg, sd_bus_error* e) {
    (void)arg; (void)e; advertisement_count++; const char* path;
    CHECK(sd_bus_message_read(m, "o", &path) > 0);
    CHECK(sd_bus_call_method_async(bus, NULL, application, path, "org.freedesktop.DBus.Properties", "GetAll",
        advertisement_properties, NULL, "s", "org.bluez.LEAdvertisement1") >= 0);
    return sd_bus_reply_method_return(m, "");
}
static int property(sd_bus* b, const char* p, const char* i, const char* name, sd_bus_message* m, void* arg, sd_bus_error* e) {
    (void)b; (void)p; (void)i; (void)name; (void)arg; (void)e; return sd_bus_message_append(m, "b", 1);
}
static const sd_bus_vtable adapter_vtable[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("Powered", "b", property, 0, 0),
    SD_BUS_METHOD("SetDiscoveryFilter", "a{sv}", "", simple, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("StartDiscovery", "", "", simple, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END
};
static const sd_bus_vtable gatt_vtable[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_METHOD("RegisterApplication", "oa{sv}", "", register_app, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END
};
static int power_property(sd_bus* b, const char* p, const char* i, const char* name,
                          sd_bus_message* m, void* arg, sd_bus_error* e) {
    (void)b; (void)p; (void)i; (void)arg; (void)e;
    int unavailable = getenv("BLE_TEST_POWER_UNAVAILABLE") != NULL;
    if (!strcmp(name, "SupportedFeatures"))
        return unavailable ? sd_bus_message_append(m, "as", 0) : sd_bus_message_append(m, "as", 2, "HardwareOffload", "CanSetTxPower");
    return unavailable ? sd_bus_message_append(m, "a{sv}", 0) :
        sd_bus_message_append(m, "a{sv}", 2, "MaxAdvLen", "y", 251, "MaxTxPower", "n", 4);
}
static const sd_bus_vtable adv_vtable[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_METHOD("RegisterAdvertisement", "oa{sv}", "", advertise, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("SupportedFeatures", "as", power_property, 0, 0),
    SD_BUS_PROPERTY("SupportedCapabilities", "a{sv}", power_property, 0, 0),
    SD_BUS_METHOD("UnregisterAdvertisement", "o", "", simple, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END
};
static int remote_property(sd_bus* b, const char* path, const char* i, const char* name,
                           sd_bus_message* m, void* arg, sd_bus_error* e) {
    (void)b; (void)i; (void)arg; (void)e;
    if (!strcmp(name, "MTU")) return sd_bus_message_append(m, "q", remote_resolved ? 517 : 23);
    const char* uuid = strstr(path, "/name") ? BLE_NAME_UUID : strstr(path, "/node") ? BLE_NODE_UUID :
                       strstr(path, "/rx") ? BLE_RX_UUID : strstr(path, "/tx") ? BLE_TX_UUID :
        strstr(path, "/ack") ? BLE_ACK_UUID : BLE_INVITE_UUID;
    return sd_bus_message_append(m, "s", uuid);
}
static int remote_write(sd_bus_message* m, void* arg, sd_bus_error* e) {
    (void)arg; (void)e;
    const void* data; size_t len; const char *key, *type;
    CHECK(sd_bus_message_read_array(m, 'y', &data, &len) > 0 && len > 0);
    CHECK(sd_bus_message_enter_container(m, 'a', "{sv}") > 0);
    CHECK(sd_bus_message_enter_container(m, 'e', "sv") > 0);
    CHECK(sd_bus_message_read(m, "s", &key) > 0 && !strcmp(key, "type"));
    CHECK(sd_bus_message_enter_container(m, 'v', "s") > 0);
    CHECK(sd_bus_message_read(m, "s", &type) > 0);
    int invitation = strstr(sd_bus_message_get_path(m), "/invite") != NULL;
    int ack = strstr(sd_bus_message_get_path(m), "/ack") != NULL;
    CHECK(!strcmp(type, invitation ? "request" : "command"));
    if (ack) { CHECK(len == 2); ack_requests++; } else if (invitation) requests++; else commands++;
    int r = sd_bus_reply_method_return(m, "");
    if (!invitation && !ack && len == 68 && ((const uint8_t*)data)[2] == 1) {
        sd_bus_message* signal = NULL; uint8_t accept[68] = {0, 1, 2, 0};
        fixture_ble_key(accept + 4, 3); fixture_ble_key(accept + 36, 200);
        CHECK(sd_bus_message_new_signal(bus, &signal, REMOTE_SERVICE "/tx", "org.freedesktop.DBus.Properties", "PropertiesChanged") >= 0);
        CHECK(sd_bus_message_append(signal, "s", "org.bluez.GattCharacteristic1") >= 0);
        CHECK(sd_bus_message_open_container(signal, 'a', "{sv}") >= 0);
        CHECK(sd_bus_message_open_container(signal, 'e', "sv") >= 0 && sd_bus_message_append(signal, "s", "Value") >= 0);
        CHECK(sd_bus_message_open_container(signal, 'v', "ay") >= 0 && sd_bus_message_append_array(signal, 'y', accept, sizeof(accept)) >= 0);
        for (int i = 0; i < 3; i++) CHECK(sd_bus_message_close_container(signal) >= 0);
        CHECK(sd_bus_message_append(signal, "as", 0) >= 0);
        CHECK(sd_bus_send(bus, signal, NULL) >= 0); sd_bus_message_unref(signal);
    }
    return r;
}
static int remote_state(sd_bus* b, const char* path, const char* i, const char* name,
                        sd_bus_message* m, void* arg, sd_bus_error* e) {
    (void)b; (void)i; (void)arg; (void)e;
    int rotated = !strcmp(path, ROTATED);
    if (!strcmp(name, "UUIDs")) {
        char uuid[37]; CHECK(!ble_service_uuid_encode(uuid, rotated || !remote_identity ? 3 : remote_identity));
        return sd_bus_message_append(m, "as", 1, uuid);
    }
    if (!strcmp(name, "ManufacturerData")) {
        if (!remote_identity) return sd_bus_message_append(m, "a{qv}", 1, 0xffff, "ay", 3, 'a', 'b', 'c');
        uint8_t bytes[BLE_BROADCAST_MAX]; int len = ble_identity_encode(bytes, rotated ? 3 : remote_identity, "");
        int r = sd_bus_message_open_container(m, 'a', "{qv}");
        if (r >= 0) r = sd_bus_message_open_container(m, 'e', "qv");
        if (r >= 0) r = sd_bus_message_append(m, "q", 0xffff);
        if (r >= 0) r = sd_bus_message_open_container(m, 'v', "ay");
        if (r >= 0) r = sd_bus_message_append_array(m, 'y', bytes, len);
        if (r >= 0) r = sd_bus_message_close_container(m);
        if (r >= 0) r = sd_bus_message_close_container(m);
        if (r >= 0) r = sd_bus_message_close_container(m);
        return r;
    }
    if (rotated) return sd_bus_message_append(m, "b", 0);
    return sd_bus_message_append(m, "b", !strcmp(name, "Connected") ? remote_connected : remote_resolved);
}
/* Повторный Connect существующего ACL недопустим: BlueZ может начать другой bearer (BR/EDR). */
static int remote_connect(sd_bus_message* m, void* arg, sd_bus_error* e) {
    (void)arg; (void)e;
    if (!strcmp(sd_bus_message_get_path(m), ROTATED)) {
        rotated_connects++; return sd_bus_reply_method_errorf(m, "org.bluez.Error.Failed", "ACL already exists at old address");
    }
    CHECK(!remote_connected);
    remote_connects++; remote_connected = 1;
    CHECK(sd_bus_emit_properties_changed(bus, REMOTE, "org.bluez.Device1", "Connected", NULL) >= 0);
    return sd_bus_reply_method_return(m, "");
}
static const sd_bus_vtable device_vtable[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_METHOD("Connect", "", "", remote_connect, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("Connected", "b", remote_state, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ServicesResolved", "b", remote_state, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ManufacturerData", "a{qv}", remote_state, 0, 0),
    SD_BUS_PROPERTY("UUIDs", "as", remote_state, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END
};
static int service_property(sd_bus* b, const char* path, const char* i, const char* name,
                             sd_bus_message* m, void* arg, sd_bus_error* e) {
    (void)b; (void)path; (void)i; (void)name; (void)arg; (void)e; return sd_bus_message_append(m, "s", service_uuid);
}
static const sd_bus_vtable remote_service_vtable[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("UUID", "s", service_property, 0, SD_BUS_VTABLE_PROPERTY_CONST), SD_BUS_VTABLE_END
};
static int remote_read(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)arg; (void)error; uint8_t node[BLE_PUBLIC_KEY_SIZE]; fixture_ble_key(node, 3);
    int is_node = strstr(sd_bus_message_get_path(m), "/node") != NULL;
    sd_bus_message* reply = NULL; CHECK(sd_bus_message_new_method_return(m, &reply) >= 0);
    CHECK(sd_bus_message_append_array(reply, 'y', is_node ? (const void*)node : (const void*)full_name,
                                      is_node ? sizeof(node) : strlen(full_name)) >= 0);
    CHECK(sd_bus_send(bus, reply, NULL) >= 0); sd_bus_message_unref(reply); profile_reads++; return 1;
}
static void got_profile(void* arg, const struct ble_endpoint* endpoint, const uint8_t key[BLE_PUBLIC_KEY_SIZE], const char* name) {
    (void)endpoint; uint64_t node = fixture_ble_locator(key);
    (void)arg; CHECK(node == 3 && !strcmp(name, full_name)); profile_callbacks++;
}
static const sd_bus_vtable remote_vtable[] = {
    SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("UUID", "s", remote_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MTU", "q", remote_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("ReadValue", "a{sv}", "ay", remote_read, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("StartNotify", "", "", simple, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("StopNotify", "", "", simple, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("WriteValue", "aya{sv}", "", remote_write, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END
};
static void status(void* arg, enum ble_state state, const char* reason) {
    (void)arg; (void)reason; if (state == BLE_READY) ready++;
}
static void connection(void* arg, ble_link_id id, uint64_t node, enum ble_state state, const char* reason) {
    (void)arg; (void)reason;
    if (state == BLE_READY) { CHECK(node < 4); links[node] = id; ready_links++; }
    if (state == BLE_CLOSED) closed_links++;
}
static void broadcast(void* arg, const struct ble_endpoint* ep, const uint8_t* data, size_t len, int rssi) {
    (void)arg; struct ble_identity identity; CHECK(strstr(ep->id, "dev_A") || !strcmp(ep->id, REMOTE));
    CHECK(!ble_identity_decode(&identity, data, len) && identity.node_id == 3); CHECK(rssi == -42); broadcasts++;
}
static int replied(sd_bus_message* m, void* arg, sd_bus_error* error) {
    (void)error; struct reply* reply = arg;
    reply->failed = sd_bus_message_is_method_error(m, NULL);
    if (reply->failed) { const sd_bus_error* e = sd_bus_message_get_error(m); fprintf(stderr, "reply error %s: %s\n", e->name, e->message); }
    if (!reply->failed && !strcmp(sd_bus_message_get_signature(m, 1), "hq")) {
        int fd; uint16_t mtu; CHECK(sd_bus_message_read(m, "hq", &fd, &mtu) > 0); CHECK(mtu == 23);
        reply->fd = fcntl(fd, F_DUPFD_CLOEXEC, 3); CHECK(reply->fd >= 0);
    }
    if (!reply->failed && !strcmp(sd_bus_message_get_signature(m, 1), "ay")) {
        const void* bytes; CHECK(sd_bus_message_read_array(m, 'y', &bytes, &reply->size) >= 0);
        CHECK(reply->size <= sizeof(reply->bytes)); memcpy(reply->bytes, bytes, reply->size);
    }
    reply->done = 1; return 0;
}
static int acquire(const char* device, int channel) {
    struct reply reply = {.fd=-1};
    const char* path = channel == 2 ? ROOT "/service0/ack" : channel ? ROOT "/service0/invite" : ROOT "/service0/tx";
    CHECK(sd_bus_call_method_async(bus, &reply.slot, application, path, "org.bluez.GattCharacteristic1",
        "AcquireNotify", replied, &reply, "a{sv}", 2, "device", "o", device, "mtu", "q", 23) >= 0);
    poll_all(); CHECK(reply.done && !reply.failed && reply.fd >= 0); sd_bus_slot_unref(reply.slot); return reply.fd;
}
static struct reply read_local_profile(const char* path, unsigned offset) {
    struct reply reply = {0};
    CHECK(sd_bus_call_method_async(bus, &reply.slot, application, path, "org.bluez.GattCharacteristic1",
        "ReadValue", replied, &reply, "a{sv}", 3, "device", "o", ADAPTER "/dev_A", "mtu", "q", 23,
        "offset", "q", offset) >= 0);
    poll_all(); CHECK(reply.done && !reply.failed); sd_bus_slot_unref(reply.slot); return reply;
}
static void write_value(const char* device, const uint8_t* data, size_t len, int channel) {
    struct reply reply = {0}; sd_bus_message* m = NULL;
    const char* path = channel == 2 ? ROOT "/service0/ack" : channel ? ROOT "/service0/invite" : ROOT "/service0/rx";
    CHECK(sd_bus_message_new_method_call(bus, &m, application, path, "org.bluez.GattCharacteristic1", "WriteValue") >= 0);
    CHECK(sd_bus_message_append_array(m, 'y', data, len) >= 0);
    CHECK(sd_bus_message_append(m, "a{sv}", 2, "device", "o", device, "mtu", "q", 23) >= 0);
    CHECK(sd_bus_call_async(bus, &reply.slot, m, replied, &reply, 1000000) >= 0); sd_bus_message_unref(m);
    poll_all(); CHECK(reply.done && !reply.failed); sd_bus_slot_unref(reply.slot);
}
static void announce(const char* path, int with_uuid) {
    sd_bus_message* m = NULL;
    CHECK(sd_bus_message_new_signal(bus, &m, path, "org.freedesktop.DBus.Properties", "PropertiesChanged") >= 0);
    if (with_uuid) CHECK(sd_bus_message_append(m, "sa{sv}as", "org.bluez.Device1", 2,
        "UUIDs", "as", 1, service_uuid, "RSSI", "n", -42, 0) >= 0);
    else CHECK(sd_bus_message_append(m, "sa{sv}as", "org.bluez.Device1", 1, "RSSI", "n", -42, 0) >= 0);
    unsigned before = broadcasts;
    CHECK(sd_bus_send(bus, m, NULL) >= 0); sd_bus_message_unref(m); poll_all(); CHECK(broadcasts == before + 1);
}
static void hello(const char* device, int fd, uint8_t node) {
    uint8_t data[20] = {0, 0, 1}, body[64], answer[64];
    fixture_ble_key(body, node); fixture_ble_key(body + 32, 200);
    for (unsigned i = 0; i < 4; i++) {
        data[1] = i + 1; data[3] = i * 16;
        memcpy(data + 4, body + i * 16, 16);
        write_value(device, data, sizeof(data), 0);
    }
    for (unsigned i = 0; i < 4; i++) {
        CHECK(recv(fd, data, sizeof(data), MSG_DONTWAIT) == 20);
        CHECK(data[2] == 2 && data[3] == i * 16);
        memcpy(answer + i * 16, data + 4, 16);
    }
    CHECK(!memcmp(answer, body + 32, 32) && !memcmp(answer + 32, body, 32));
    poll_all(); /* ACCEPT отправляется Notification: подтверждения от клиента нет. */
}
static struct ll_entry* frame(int value) {
    struct ll_entry* e = ll_alloc_lldgram(31); CHECK(e); e->len = 31; memset(e->dgram, value, e->len); return e;
}
int main(void) {
    CHECK(getenv("DBUS_SESSION_BUS_ADDRESS")); CHECK(!setenv("DBUS_SYSTEM_BUS_ADDRESS", getenv("DBUS_SESSION_BUS_ADDRESS"), 1));
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    size_t before = u_get_allocated_count();
    CHECK(sd_bus_open_user(&bus) >= 0); CHECK(sd_bus_request_name(bus, "org.bluez", 0) >= 0);
    sd_bus_slot *manager = NULL, *adapter = NULL, *gatt = NULL, *adv = NULL, *remote[9] = {0};
    CHECK(!ble_service_uuid_encode(service_uuid, 3));
    CHECK(sd_bus_add_object_manager(bus, &manager, "/") >= 0);
    CHECK(sd_bus_add_object_vtable(bus, &adapter, ADAPTER, "org.bluez.Adapter1", adapter_vtable, NULL) >= 0);
    CHECK(sd_bus_add_object_vtable(bus, &gatt, ADAPTER, "org.bluez.GattManager1", gatt_vtable, NULL) >= 0);
    CHECK(sd_bus_add_object_vtable(bus, &adv, ADAPTER, "org.bluez.LEAdvertisingManager1", adv_vtable, NULL) >= 0);
    CHECK(sd_bus_add_object_vtable(bus, &remote[0], REMOTE, "org.bluez.Device1", device_vtable, NULL) >= 0);
    CHECK(sd_bus_add_object_vtable(bus, &remote[6], REMOTE_SERVICE, "org.bluez.GattService1", remote_service_vtable, NULL) >= 0);
    const char* paths[] = {REMOTE_SERVICE "/rx", REMOTE_SERVICE "/tx", REMOTE_SERVICE "/invite", REMOTE_SERVICE "/ack"};
    for (int i = 0; i < 4; i++)
        CHECK(sd_bus_add_object_vtable(bus, &remote[i+1], paths[i], "org.bluez.GattCharacteristic1", remote_vtable, NULL) >= 0);
    CHECK(sd_bus_add_object_vtable(bus, &remote[7], REMOTE_SERVICE "/name", "org.bluez.GattCharacteristic1", remote_vtable, NULL) >= 0);
    CHECK(sd_bus_add_object_vtable(bus, &remote[8], REMOTE_SERVICE "/node", "org.bluez.GattCharacteristic1", remote_vtable, NULL) >= 0);
    ua = uasync_create(); CHECK(ua);
    struct ble_config config = {0}; config.node_id = 200; fixture_ble_config(&config);
    strcpy(config.name, full_name); config.callbacks.profile = got_profile;
    memcpy(config.initial_advertisement, "abc", 3); config.initial_advertisement_size = 3;
    config.callbacks.status = status; config.callbacks.connection = connection; config.callbacks.broadcast = broadcast;
    transport = ble_create(ua, &config); CHECK(transport && !ble_start(transport)); poll_all(); CHECK(ready == 1 && broadcasts == 0);
    struct ble_advertising_power power = ble_get_advertising_power(transport);
    CHECK(power.max_dbm == (getenv("BLE_TEST_POWER_UNAVAILABLE") ? 127 : 4));
    CHECK(power.can_set == (getenv("BLE_TEST_POWER_UNAVAILABLE") ? 0 : 1));
    CHECK(advertisement_count == 1); /* Один connectable advertising с UUID узла. */
    CHECK(advertisement_queries == 1 && advertisement_uuid_count == 1);
    /* Read Long при MTU23 возвращает один снимок даже при смене имени между частями. */
    struct reply part = read_local_profile(ROOT "/service0/name", 0);
    CHECK(part.size == 22 && !memcmp(part.bytes, full_name, 22));
    CHECK(!ble_set_name(transport, "Новое имя"));
    part = read_local_profile(ROOT "/service0/name", 22);
    CHECK(part.size == 22 && !memcmp(part.bytes, full_name + 22, part.size));
    part = read_local_profile(ROOT "/service0/name", 44);
    CHECK(part.size == strlen(full_name) - 44 && !memcmp(part.bytes, full_name + 44, part.size));
    part = read_local_profile(ROOT "/service0/name", 0);
    CHECK(part.size == strlen("Новое имя") && !memcmp(part.bytes, "Новое имя", part.size));
    part = read_local_profile(ROOT "/service0/node", 0);
    CHECK(part.size == 22 && !memcmp(part.bytes, config.public_key, part.size));
    part = read_local_profile(ROOT "/service0/node", 22);
    CHECK(part.size == 10 && !memcmp(part.bytes, config.public_key + 22, part.size));
    CHECK(!ble_advertise(transport, "abc", 3)); poll_all(); CHECK(advertisement_count == 1);
    CHECK(!ble_advertise(transport, "def", 3)); poll_all(); CHECK(advertisement_count == 1); /* UUID/видимость прежние. */
    announce(REMOTE, 0); /* UUID был лишь в начальном snapshot; живой RSSI делает наблюдение свежим. */
    announce(ADAPTER "/dev_A", 1);
    int fd1 = acquire(ADAPTER "/dev_A", 0), fd2 = acquire(ADAPTER "/dev_B", 0);
    int invite1 = acquire(ADAPTER "/dev_A", 1), invite2 = acquire(ADAPTER "/dev_B", 1);
    CHECK(ready_links == 0); /* Без обязательной ACK-подписки opened не наступает. */
    ack_fds[0] = acquire(ADAPTER "/dev_A", 2); ack_fds[1] = acquire(ADAPTER "/dev_B", 2);
    hello(ADAPTER "/dev_A", fd1, 1); hello(ADAPTER "/dev_B", fd2, 2); CHECK(ready_links == 2);
    CHECK(!ble_tx_put(transport, links[1], frame(0xa1))); CHECK(!ble_tx_put(transport, links[2], frame(0xb2))); poll_all();
    uint8_t data[64];
    /* Все DATA-фрагменты обоим клиентам проходят без единого ATT ACK. */
    for (int offset = 0; offset < 31; offset += 11) {
        CHECK(recv(fd1, data, sizeof(data), MSG_DONTWAIT) == (offset == 22 ? 18 : 20));
        CHECK(data[8] == offset && data[9] == 0xa1);
        CHECK(recv(fd2, data, sizeof(data), MSG_DONTWAIT) == (offset == 22 ? 18 : 20));
        CHECK(data[8] == offset && data[9] == 0xb2);
    }
    CHECK(recv(fd1, data, sizeof(data), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    CHECK(recv(fd2, data, sizeof(data), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    uint8_t incoming[] = {0, 5, 3, 0, 1, 0, 3, 0, 0, 0x11, 0x22, 0x33};
    write_value(ADAPTER "/dev_B", incoming, sizeof(incoming), 0);
    CHECK(!ble_rx_get(transport, links[1])); struct ll_entry* e = ble_rx_get(transport, links[2]); CHECK(e && e->len == 3);
    CHECK(!memcmp(e->dgram, incoming + 9, 3)); queue_dgram_free(e); queue_entry_free(e);
    write_value(ADAPTER "/dev_A", incoming + 2, sizeof(incoming) - 2, 1);
    CHECK(!ble_rx_get(transport, links[1])); CHECK(!ble_channel_rx_get(transport, links[2], BLE_CHANNEL_INVITE));
    e = ble_channel_rx_get(transport, links[1], BLE_CHANNEL_INVITE); CHECK(e && e->len == 3);
    queue_dgram_free(e); queue_entry_free(e);
    CHECK(!ble_channel_tx_put(transport, links[2], BLE_CHANNEL_INVITE, frame(0xc3))); poll_all();
    CHECK(recv(invite1, data, sizeof(data), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    CHECK(recv(fd2, data, sizeof(data), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    for (int offset = 0; offset < 31; offset += 13) {
        CHECK(recv(invite2, data, sizeof(data), MSG_DONTWAIT) == (offset == 26 ? 12 : 20));
        CHECK(data[6] == offset && data[7] == 0xc3);
        poll_all(); /* INVITE не продвигается без подтверждения; другой клиент свободен. */
        CHECK(recv(invite2, data, sizeof(data), MSG_DONTWAIT) < 0 && errno == EAGAIN);
        CHECK(!ble_tx_put(transport, links[1], frame(0xd4))); poll_all();
        for (int part = 0; part < 31; part += 11) {
            CHECK(recv(fd1, data, sizeof(data), MSG_DONTWAIT) == (part == 22 ? 18 : 20));
            CHECK(data[8] == part && data[9] == 0xd4);
        }
        /* Flow ACK последнего DATA-фрагмента, независимо от ATT confirmation канала INVITE. */
        write_value(ADAPTER "/dev_A", data, 2, 2);
        CHECK(send(invite2, "\1", 1, MSG_NOSIGNAL) == 1); poll_all();
    }
    CHECK(ack_values >= 2);
    /* Повторный AcquireNotify до HUP старого fd: новая сессия A, действующая сессия B не затронута. */
    ble_link_id old_a = links[1], old_b = links[2];
    int closed_before = closed_links;
    int old_ack = ack_fds[0]; ack_fds[0] = -1; /* poll_all не должен читать EOF закрываемой подписки. */
    int new_fd1 = acquire(ADAPTER "/dev_A", 0);
    CHECK(closed_links == closed_before + 1 && ready_links == 2);
    CHECK(recv(invite1, data, sizeof(data), MSG_DONTWAIT) == 0);
    close(old_ack);
    close(invite1); close(fd1); fd1 = new_fd1;
    invite1 = acquire(ADAPTER "/dev_A", 1);
    CHECK(ready_links == 2);
    ack_fds[0] = acquire(ADAPTER "/dev_A", 2);
    hello(ADAPTER "/dev_A", fd1, 1);
    CHECK(ready_links == 3 && links[1] != old_a && links[2] == old_b);
    CHECK(!ble_tx_put(transport, old_b, frame(0xf1))); poll_all();
    for (int offset = 0; offset < 31; offset += 11) {
        CHECK(recv(fd2, data, sizeof(data), MSG_DONTWAIT) == (offset == 22 ? 18 : 20));
        CHECK(data[8] == offset && data[9] == 0xf1);
    }
    write_value(ADAPTER "/dev_B", data, 2, 2);
    write_value(ADAPTER "/dev_A", incoming, sizeof(incoming), 0);
    e = ble_rx_get(transport, links[1]); CHECK(e && e->len == 3 && !memcmp(e->dgram, incoming + 9, 3));
    queue_dgram_free(e); queue_entry_free(e);
    close(ack_fds[0]); close(ack_fds[1]); ack_fds[0] = ack_fds[1] = -1;
    close(invite1); close(invite2);
    close(fd1); poll_all(); close(fd2); poll_all();
    /* Настоящий клиент backend: HELLO/DATA используют command, INVITE использует request. */
    struct ble_endpoint ep = {REMOTE};
    CHECK(ble_connect(transport, &ep, 3)); poll_all();
    /* Кеш UUID доступен сразу, но до ServicesResolved нельзя объявлять GATT готовым с прежним MTU23. */
    CHECK(ready_links == 3 && commands == 0 && remote_connects == 0);
    remote_resolved = 1;
    CHECK(sd_bus_emit_properties_changed(bus, REMOTE, "org.bluez.Device1", "ServicesResolved", NULL) >= 0);
    poll_all(); CHECK(ready_links == 4 && commands == 1);
    CHECK(!ble_tx_put(transport, links[3], frame(0xe5)));
    CHECK(!ble_channel_tx_put(transport, links[3], BLE_CHANNEL_INVITE, frame(0xe6)));
    poll_all(); CHECK(commands == 2 && requests == 1 && ack_requests >= 1); /* MTU517 и ACK Write Command. */
    /* Новый ACL требует ровно одного Connect; discovery ждёт ServicesResolved. */
    ble_disconnect(transport, links[3]); poll_all(); remote_connected = remote_resolved = 0;
    CHECK(ble_connect(transport, &ep, 3)); poll_all();
    CHECK(remote_connects == 1 && ready_links == 4 && commands == 2);
    remote_resolved = 1;
    CHECK(sd_bus_emit_properties_changed(bus, REMOTE, "org.bluez.Device1", "ServicesResolved", NULL) >= 0);
    poll_all(); CHECK(remote_connects == 1 && ready_links == 5 && commands == 3);
    /* Следующий GATT на уже готовом ACL сразу использует snapshot без Connect. */
    ble_disconnect(transport, links[3]); poll_all();
    CHECK(ble_connect(transport, &ep, 3)); poll_all();
    CHECK(remote_connects == 1 && ready_links == 6 && commands == 4);
    /* Новый RPA того же узла не требует второго ACL; чужой ID на старом ACL не подходит. */
    ble_disconnect(transport, links[3]); poll_all(); remote_resolved = 0; remote_identity = 2;
    CHECK(sd_bus_add_object_vtable(bus, &remote[5], ROTATED, "org.bluez.Device1", device_vtable, NULL) >= 0);
    struct ble_endpoint rotated = {ROTATED};
    CHECK(ble_connect(transport, &rotated, 3)); poll_all();
    CHECK(rotated_connects == 1 && ready_links == 6 && commands == 4);
    remote_identity = 3;
    CHECK(ble_connect(transport, &rotated, 3)); poll_all();
    CHECK(rotated_connects == 1 && remote_connects == 1 && ready_links == 6 && commands == 4);
    remote_resolved = 1;
    CHECK(sd_bus_emit_properties_changed(bus, REMOTE, "org.bluez.Device1", "ServicesResolved", NULL) >= 0);
    poll_all(); CHECK(ready_links == 7 && commands == 5 && rotated_connects == 1 && remote_connects == 1);
    unsigned closed = closed_links;
    CHECK(!ble_advertise(transport, NULL, 0)); poll_all();
    CHECK(advertisement_uuid_count == 0 && closed_links == closed && ble_tx_ready(transport, links[3]));
    ble_set_incoming(transport, 0); CHECK(!ble_advertise(transport, "abc", 3)); poll_all();
    CHECK(advertisement_uuid_count == 1 && !ble_backend_incoming(transport, &rotated));
    CHECK(profile_callbacks >= 4 && profile_reads == profile_callbacks * 2);
    ble_stop(transport); power = ble_get_advertising_power(transport); CHECK(power.max_dbm == 127 && power.can_set == -1);
    ble_destroy(transport); uasync_destroy(ua, 0);
    for (int i = 0; i < 9; i++) sd_bus_slot_unref(remote[i]);
    sd_bus_slot_unref(manager); sd_bus_slot_unref(adapter); sd_bus_slot_unref(gatt); sd_bus_slot_unref(adv); sd_bus_close_unref(bus);
    CHECK(u_get_allocated_count() == before);
    puts("BlueZ private-bus tests PASS (existing/new ACL, DATA/ACK commands, INVITE indications, two clients, teardown)");
    return 0;
}
