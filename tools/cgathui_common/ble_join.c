#include "ble_join.h"
#include "../../src/chat/chat_sync.h"
#include "../../src/utun_instance.h"
#include "../../src/transport_layer/secure_channel.h"
#include "../../src/local_transport/protocol.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <string.h>
#define CAT DEBUG_CATEGORY_DISCOVERY
#define JOIN_QUEUE 4
#define JOIN_PREFIX 34 /* version, kind, X25519 отправителя */
#define JOIN_BODY 61 /* domain8, X25519 получателя[32], nonce8, echo8, seq4, opcode1 */
#define JOIN_OVERHEAD (JOIN_PREFIX + JOIN_BODY + SC_NONCE_SIZE + SC_TAG_SIZE + SC_CRC32_SIZE)
#define JOIN_PAYLOAD_MAX (BLE_INVITE_FRAME_MAX - JOIN_OVERHEAD)
_Static_assert(JOIN_PAYLOAD_MAX <= CHAT_JOIN_WIRE_MAX, "BLE JOIN payload must fit CHAT_SYNC");
#define JOIN_LIFETIME_US 60000000ULL
static const uint8_t domain[8] = {'V','C','J','O','I','N',2,0};
enum { JOIN_HELLO = 1, JOIN_REPLY, JOIN_MESSAGE, JOIN_CLOSE };
struct join_link {
    ble_link_id id;
    uint64_t peer, local_nonce, peer_nonce, deadline, hello_due;
    sc_context_t crypto;
    uint32_t tx_sequence, rx_sequence;
    int ready, requested, authenticated, notified, held, releasing;
    struct ll_entry* queue[JOIN_QUEUE];
    unsigned head, count;
};
struct ble_join {
    struct UTUN_INSTANCE* inst;
    struct ble_transport* radio;
    struct chat_join_carrier provider;
    ble_link_id (*connect)(void*, const uint8_t key[32]);
    void (*wake)(void*);
    void* arg;
    struct join_link links[BLE_LINKS_MAX];
};
static struct join_link* find_link(struct ble_join* b, uint64_t id, int handle) {
    for (int i = 0; i < BLE_LINKS_MAX; ++i)
        if (id && b->links[i].id && (handle ? b->links[i].local_nonce == id : b->links[i].id == id)) return &b->links[i];
    return NULL;
}
/* При сбросе nonce меняется; поздние сообщения не могут возобновить завершённую сессию. */
static void reset(struct ble_join* b, struct join_link* l, int notify) {
    uint64_t handle = l->local_nonce, peer = l->peer; ble_link_id id = l->id; int ready = l->ready;
    for (unsigned i = 0; i < JOIN_QUEUE; ++i) if (l->queue[i]) {
        queue_dgram_free(l->queue[i]); queue_entry_free(l->queue[i]);
    }
    OPENSSL_cleanse(l, sizeof(*l)); l->id = id; l->peer = peer; l->ready = ready;
    if (notify && handle) chat_sync_join_carrier_closed(b->inst, handle);
}
static int start(struct ble_join* b, struct join_link* l, const uint8_t key[32]) {
    if (l->local_nonce) {
        if (!memcmp(l->crypto.peer_public_key, key, 32)) return 0;
        DEBUG_WARN(CAT, "BLE JOIN key changed inside GATT link=%llu", (unsigned long long)l->id); return -1;
    }
    uint64_t nonce;
    if (RAND_bytes((uint8_t*)&nonce, sizeof(nonce)) != 1 || !nonce || find_link(b, nonce, 1) ||
        sc_init_ctx(&l->crypto, &b->inst->my_keys) != SC_OK || sc_set_peer_public_key(&l->crypto, key, SC_PEER_PUBKEY_BIN) != SC_OK) {
        DEBUG_ERROR(CAT, "BLE JOIN nonce/crypto initialization failed link=%llu", (unsigned long long)l->id); return -1;
    }
    l->local_nonce = nonce; l->deadline = get_time_us() + JOIN_LIFETIME_US; l->held = 1;
    DEBUG_INFO(CAT, "BLE JOIN session started link=%llu peer=%016llx handle=%016llx",
               (unsigned long long)l->id, (unsigned long long)l->peer, (unsigned long long)nonce);
    return 0;
}
/* Одна bounded очередь шифрованных целых INVITE; фрагментацией владеет ble_transport. */
static int enqueue(struct ble_join* b, struct join_link* l, int opcode, const uint8_t* data, size_t len) {
    if (l->count == JOIN_QUEUE || len > JOIN_PAYLOAD_MAX || !l->local_nonce || l->tx_sequence == UINT32_MAX) {
        DEBUG_WARN(CAT, "BLE JOIN TX limit link=%llu queued=%u bytes=%zu", (unsigned long long)l->id, l->count, len); return -1;
    }
    uint8_t plain[JOIN_BODY + JOIN_PAYLOAD_MAX];
    memcpy(plain, domain, sizeof(domain)); memcpy(plain + 8, l->crypto.peer_public_key, 32);
    local_put64(plain + 40, l->local_nonce); local_put64(plain + 48, opcode == JOIN_HELLO ? 0 : l->peer_nonce);
    uint32_t sequence = opcode >= JOIN_MESSAGE ? l->tx_sequence + 1 : 0, wire = htonl(sequence);
    memcpy(plain + 56, &wire, 4); plain[60] = (uint8_t)opcode;
    if (len) memcpy(plain + JOIN_BODY, data, len);
    struct ll_entry* e = ll_alloc_lldgram(JOIN_OVERHEAD + len);
    if (!e) { DEBUG_ERROR(CAT, "BLE JOIN TX allocation failed"); return -1; }
    e->dgram[0] = 1; e->dgram[1] = 3; memcpy(e->dgram + 2, b->inst->my_keys.public_key, 32);
    size_t encrypted = 0;
    int result = sc_encrypt(&l->crypto, plain, JOIN_BODY + len, e->dgram + JOIN_PREFIX, &encrypted);
    OPENSSL_cleanse(plain, sizeof(plain));
    if (result != SC_OK) {
        queue_dgram_free(e); queue_entry_free(e); DEBUG_ERROR(CAT, "BLE JOIN encryption failed rc=%d", result); return -1;
    }
    e->len = JOIN_PREFIX + encrypted; l->queue[(l->head + l->count++) % JOIN_QUEUE] = e;
    if (opcode >= JOIN_MESSAGE) l->tx_sequence = sequence;
    DEBUG_DEBUG(CAT, "BLE JOIN TX queued link=%llu op=%d seq=%u bytes=%zu queued=%u",
                (unsigned long long)l->id, opcode, sequence, len, l->count);
    b->wake(b->arg); return 0;
}
static int carrier_open(void* arg, const uint8_t key[32], uint64_t* handle) {
    struct ble_join* b = arg;
    if (b->inst->utun_started) return 1;
    ble_link_id id = b->connect(b->arg, key);
    struct join_link* l = id ? find_link(b, id, 0) : NULL;
    if (!l) return 1;
    if (l->local_nonce && !l->held && !l->count) reset(b, l, 0);
    if (start(b, l, key)) {
        DEBUG_ERROR(CAT, "BLE JOIN cannot reserve link=%llu key=%s", (unsigned long long)l->id, sc_public_key_hex(key, (char[65]){0}));
        return -1;
    }
    if (l->requested) { DEBUG_WARN(CAT, "BLE JOIN already requested on link=%llu", (unsigned long long)l->id); return -1; }
    l->requested = l->held = 1; l->releasing = l->notified = 0; *handle = l->local_nonce; b->wake(b->arg); return 0;
}
static int carrier_send(void* arg, uint64_t handle, const uint8_t* data, size_t len) {
    struct ble_join* b = arg; struct join_link* l = find_link(b, handle, 1);
    if (!l || !l->authenticated || !data || len < CHAT_SYNC_BOOTSTRAP_SIZE + CHAT_SYNC_ATTEMPT_SIZE || len > JOIN_PAYLOAD_MAX ||
        data[CHAT_SYNC_TYPE_OFF] != CS_MSG_JOIN_RELAY) {
        DEBUG_WARN(CAT, "BLE JOIN send on inactive carrier handle=%016llx bytes=%zu", (unsigned long long)handle, len); return -1;
    }
    return enqueue(b, l, JOIN_MESSAGE, data, len);
}
static void carrier_close(void* arg, uint64_t handle, int success) {
    struct ble_join* b = arg; struct join_link* l = find_link(b, handle, 1);
    if (!l) return;
    DEBUG_INFO(CAT, "BLE JOIN release requested link=%llu success=%d queued=%u", (unsigned long long)l->id, success, l->count);
    if (l->authenticated && !l->releasing && enqueue(b, l, JOIN_CLOSE, NULL, 0))
        DEBUG_WARN(CAT, "BLE JOIN close could not be queued link=%llu", (unsigned long long)l->id);
    l->requested = 0; l->releasing = 1; b->wake(b->arg);
}
struct ble_join* ble_join_create(struct UTUN_INSTANCE* inst, struct ble_transport* radio,
                                ble_link_id (*connect)(void*, const uint8_t key[32]), void (*wake)(void*), void* arg) {
    if (!inst || !radio || !connect || !wake || inst->join_carrier) {
        DEBUG_ERROR(CAT, "BLE JOIN invalid/duplicate provider"); return NULL;
    }
    struct ble_join* b = u_calloc(1, sizeof(*b));
    if (!b) { DEBUG_ERROR(CAT, "BLE JOIN provider allocation failed"); return NULL; }
    b->inst = inst; b->radio = radio; b->connect = connect; b->wake = wake; b->arg = arg;
    b->provider = (struct chat_join_carrier){b, carrier_open, carrier_send, carrier_close};
    if (chat_sync_set_join_carrier(inst, &b->provider)) { u_free(b); return NULL; }
    return b;
}
void ble_join_destroy(struct ble_join* b) {
    if (!b) return;
    if (b->inst->join_carrier == &b->provider) chat_sync_set_join_carrier(b->inst, NULL);
    for (int i = 0; i < BLE_LINKS_MAX; ++i) reset(b, &b->links[i], 0);
    OPENSSL_cleanse(b, sizeof(*b)); u_free(b);
}
void ble_join_connection(struct ble_join* b, ble_link_id id, uint64_t peer, enum ble_state state) {
    if (!b) return;
    struct join_link* l = find_link(b, id, 0);
    if (state == BLE_CLOSED) { if (l) { reset(b, l, 1); l->id = 0; } return; }
    if (!l) for (int i = 0; i < BLE_LINKS_MAX; ++i) if (!b->links[i].id) { l = &b->links[i]; l->id = id; break; }
    if (!l) { DEBUG_ERROR(CAT, "BLE JOIN connection limit link=%llu", (unsigned long long)id); return; }
    l->peer = peer; l->ready = state == BLE_READY;
    if (l->ready && l->local_nonce) {
        uint8_t actual[32];
        if (ble_get_peer_key(b->radio, id, actual) != BLE_OK || memcmp(actual, l->crypto.peer_public_key, sizeof(actual))) {
            DEBUG_WARN(CAT, "BLE JOIN GATT address differs from requested key link=%llu", (unsigned long long)id);
            reset(b, l, 1);
        }
    }
}
int ble_join_holds(struct ble_join* b, ble_link_id id) {
    struct join_link* l = b ? find_link(b, id, 0) : NULL; return l && l->held;
}
int ble_join_receive(struct ble_join* b, ble_link_id id, const uint8_t* bytes, size_t len) {
    if (len < 2 || bytes[0] != 1 || bytes[1] != 3) return 0;
    struct join_link* l = find_link(b, id, 0);
    uint8_t actual[32];
    if (!l || !l->ready || b->inst->utun_started || len < JOIN_OVERHEAD ||
        len > BLE_INVITE_FRAME_MAX || ble_get_peer_key(b->radio, id, actual) != BLE_OK || memcmp(bytes + 2, actual, sizeof(actual))) {
        DEBUG_WARN(CAT, "BLE JOIN invalid envelope link=%llu bytes=%zu", (unsigned long long)id, len); return 1;
    }
    sc_context_t candidate;
    if ((l->local_nonce && memcmp(l->crypto.peer_public_key, bytes + 2, 32)) ||
        sc_init_ctx(&candidate, &b->inst->my_keys) != SC_OK || sc_set_peer_public_key(&candidate, bytes + 2, SC_PEER_PUBKEY_BIN) != SC_OK) {
        DEBUG_WARN(CAT, "BLE JOIN unexpected peer key link=%llu", (unsigned long long)id); return 1;
    }
    uint8_t plain[JOIN_BODY + JOIN_PAYLOAD_MAX]; size_t size = 0;
    int decrypted = sc_decrypt(&candidate, bytes + JOIN_PREFIX, len - JOIN_PREFIX, plain, &size);
    OPENSSL_cleanse(&candidate, sizeof(candidate));
    if (decrypted != SC_OK || size < JOIN_BODY || memcmp(plain, domain, 8) || memcmp(plain + 8, b->inst->my_keys.public_key, 32)) {
        DEBUG_WARN(CAT, "BLE JOIN authentication/domain rejected link=%llu rc=%d", (unsigned long long)id, decrypted);
        OPENSSL_cleanse(plain, sizeof(plain)); return 1;
    }
    uint64_t nonce = local_get64(plain + 40), echo = local_get64(plain + 48);
    uint32_t wire; memcpy(&wire, plain + 56, 4); uint32_t sequence = ntohl(wire); int opcode = plain[60];
    uint64_t opening_deadline = 0;
    if (l->local_nonce && (get_time_us() >= l->deadline ||
        (opcode == JOIN_HELLO && l->peer_nonce && (!l->held || !l->authenticated) && !l->count && l->peer_nonce != nonce))) {
        if (l->held && !l->authenticated && get_time_us() < l->deadline) opening_deadline = l->deadline;
        reset(b, l, 1);
    }
    if (!nonce || (l->peer_nonce && l->peer_nonce != nonce) ||
        (opcode != JOIN_HELLO && (!l->local_nonce || echo != l->local_nonce)) ||
        (opcode == JOIN_HELLO && (echo || sequence || size != JOIN_BODY)) ||
        (opcode == JOIN_REPLY && (sequence || size != JOIN_BODY || !l->requested)) ||
        (opcode >= JOIN_MESSAGE && (!l->peer_nonce || !sequence || sequence != l->rx_sequence + 1)) ||
        (opcode == JOIN_CLOSE && size != JOIN_BODY) ||
        (opcode != JOIN_HELLO && opcode != JOIN_REPLY && opcode != JOIN_MESSAGE && opcode != JOIN_CLOSE)) {
        DEBUG_WARN(CAT, "BLE JOIN stale/invalid session link=%llu op=%d seq=%u expected=%u",
                   (unsigned long long)id, opcode, sequence, l->rx_sequence + 1);
        OPENSSL_cleanse(plain, sizeof(plain)); return 1;
    }
    if (start(b, l, bytes + 2)) { OPENSSL_cleanse(plain, sizeof(plain)); return 1; }
    if (opening_deadline) l->deadline = opening_deadline; /* Смена ещё не доказанного challenge не продлевает открытие. */
    l->peer_nonce = nonce;
    DEBUG_DEBUG(CAT, "BLE JOIN RX link=%llu op=%d seq=%u bytes=%zu", (unsigned long long)id, opcode, sequence, size - JOIN_BODY);
    if (opcode == JOIN_HELLO) {
        if (enqueue(b, l, JOIN_REPLY, NULL, 0)) reset(b, l, 1);
    } else if (opcode == JOIN_REPLY) l->authenticated = 1;
    else if (opcode == JOIN_CLOSE) reset(b, l, 1);
    else {
        l->authenticated = 1; l->rx_sequence = sequence; l->held = 1;
        l->deadline = get_time_us() + JOIN_LIFETIME_US;
        chat_sync_join_carrier_receive(b->inst, l->local_nonce, l->crypto.peer_public_key, plain + JOIN_BODY, size - JOIN_BODY);
    }
    OPENSSL_cleanse(plain, sizeof(plain)); b->wake(b->arg); return 1;
}
void ble_join_run(struct ble_join* b) {
    if (!b) return;
    for (int i = 0; i < BLE_LINKS_MAX; ++i) {
        struct join_link* l = &b->links[i];
        if (!l->id || !l->local_nonce) continue;
        if (get_time_us() >= l->deadline && (l->held || l->count)) {
            DEBUG_WARN(CAT, "BLE JOIN session deadline link=%llu", (unsigned long long)l->id); reset(b, l, 1); continue;
        }
        if (!l->ready) continue;
        if (l->requested && !l->authenticated && get_time_us() >= l->hello_due && l->count < JOIN_QUEUE) {
            if (enqueue(b, l, JOIN_HELLO, NULL, 0)) { reset(b, l, 1); continue; }
            l->hello_due = get_time_us() + 500000; /* Только challenge повторяется; общий deadline не сдвигается. */
        }
        if (l->requested && l->authenticated && !l->notified) {
            l->notified = 1; chat_sync_join_carrier_ready(b->inst, l->local_nonce, l->crypto.peer_public_key);
        }
        if (l->count && ble_channel_tx_ready(b->radio, l->id, BLE_CHANNEL_INVITE)) {
            struct ll_entry* e = l->queue[l->head];
            int result = ble_channel_tx_put(b->radio, l->id, BLE_CHANNEL_INVITE, e);
            if (result == BLE_OK) { l->queue[l->head] = NULL; l->head = (l->head + 1) % JOIN_QUEUE; l->count--; }
            else if (result != BLE_WOULD_BLOCK) {
                DEBUG_WARN(CAT, "BLE JOIN TX refused link=%llu rc=%d", (unsigned long long)l->id, result); reset(b, l, 1); continue;
            }
        }
        if (l->releasing && !l->count && ble_channel_tx_ready(b->radio, l->id, BLE_CHANNEL_INVITE)) {
            l->held = l->releasing = 0;
            DEBUG_INFO(CAT, "BLE JOIN carrier drained link=%llu; known-key admission may proceed", (unsigned long long)l->id);
        }
    }
}
