#include "chat_join_port.h"
#include "chat_core.h"
#include "../routing_layer/topo_group.h"
#include "chat_sync.h"
#include "invite_link.h"
#include "../utun_instance.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <pthread.h>
#include <string.h>

struct join_command {
    struct posted_task post; /* первый член: uasync освобождает всю команду */
    struct join_command* next;
    struct chat_join_port* port;
    uint64_t request;
    char channel[65];
    uint8_t peer[32];
    size_t len;
    int completed;
    char link[];
};
struct chat_join_port {
    pthread_mutex_t lock;
    unsigned refs;
    uint64_t active_request;
    struct UTUN_INSTANCE* inst; /* только callback в uasync обращается к instance */
    struct UASYNC* ua; /* жив до close; producer публикует под lock */
    struct join_command* commands;
};

struct chat_join_port* chat_join_port_create(struct UTUN_INSTANCE* inst) {
    struct chat_join_port* p = u_calloc(1, sizeof(*p));
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "join port allocation failed"); return NULL; }
    if (pthread_mutex_init(&p->lock, NULL)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "join port mutex initialization failed"); u_free(p); return NULL;
    }
    p->refs = 1; p->inst = inst; p->ua = inst->ua; return p;
}
void chat_join_port_retain(struct chat_join_port* p) {
    if (!p) return;
    pthread_mutex_lock(&p->lock); p->refs++; pthread_mutex_unlock(&p->lock);
}
void chat_join_port_release(struct chat_join_port* p) {
    if (!p) return;
    pthread_mutex_lock(&p->lock); unsigned refs = --p->refs; pthread_mutex_unlock(&p->lock);
    if (!refs) { pthread_mutex_destroy(&p->lock); u_free(p); }
}
static void execute(void* arg) {
    struct join_command* command = arg;
    struct chat_join_port* p = command->port;
    pthread_mutex_lock(&p->lock);
    struct join_command** link = &p->commands;
    while (*link && *link != command) link = &(*link)->next;
    if (*link) *link = command->next;
    struct UTUN_INSTANCE* inst = p->inst;
    if (!command->completed && inst && !p->active_request) p->active_request = command->request;
    pthread_mutex_unlock(&p->lock);
    if (!command->completed && inst) {
        uint64_t channel = topo_groups_channel_slot(inst->topo_groups, command->channel);
        int result = command->channel[0] ? chat_sync_offer_link(inst, command->request, channel, command->link, command->len)
                                    : chat_sync_join_link(inst, command->request, command->link, command->len);
        if (result) {
            chat_join_port_complete(p, command->request);
            chat_sync_join_result(inst, command->request, channel, command->peer, result);
        }
    }
    chat_join_port_release(p); /* память command освободит uasync */
}
static int submit(struct chat_join_port* p, uint64_t request, const char* link, size_t len, const char* offer) {
    if (!p || !request || !link || !len || len >= INVITE_LINK_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "join command invalid or port unavailable"); return CHAT_CONNECT_INVALID;
    }
    struct InviteData invite;
    if (invite_link_decode(link, len, &invite, NULL, 0)) return CHAT_CONNECT_INVALID;
    if (!offer && invite.kind != INVITE_KIND_CHANNEL) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "server link requires selecting a local channel to add"); return CHAT_CONNECT_INVALID;
    }
    struct join_command* command = u_calloc(1, sizeof(*command) + len);
    if (!command) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "join command allocation failed"); return CHAT_CONNECT_ERROR; }
    command->post.callback = execute; command->post.arg = command; command->port = p;
    command->request = request;
    if (offer) strcpy(command->channel, offer);
    memcpy(command->peer, invite.pubkey, sizeof(command->peer));
    command->len = len; memcpy(command->link, link, len);
    pthread_mutex_lock(&p->lock);
    if (!p->inst) {
        pthread_mutex_unlock(&p->lock); u_free(command);
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "join command rejected: port closed"); return CHAT_CONNECT_CANCELLED;
    }
    if (p->active_request == request) {
        pthread_mutex_unlock(&p->lock); u_free(command);
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "join request already active"); return CHAT_CONNECT_BUSY;
    }
    for (struct join_command* c = p->commands; c; c = c->next) if (c->request == request) {
        pthread_mutex_unlock(&p->lock); u_free(command);
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "duplicate pending join request"); return CHAT_CONNECT_BUSY;
    }
    p->refs++; command->next = p->commands; p->commands = command;
    uasync_post_reserved(p->ua, &command->post);
    pthread_mutex_unlock(&p->lock);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "join command queued request=%llu", (unsigned long long)request);
    return 0;
}
void chat_join_port_close(struct chat_join_port* p) {
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    struct UTUN_INSTANCE* inst = p->inst;
    struct UASYNC* ua = p->ua;
    struct join_command* commands = p->commands;
    p->commands = NULL; p->inst = NULL; p->ua = NULL;
    pthread_mutex_unlock(&p->lock);
    while (commands) {
        struct join_command* c = commands; commands = c->next;
        c->completed = 1;
        uint64_t request = c->request, channel = topo_groups_channel_slot(inst->topo_groups, c->channel);
        uint8_t peer[32]; memcpy(peer, c->peer, sizeof(peer));
        /* Успешная отмена снимает ссылку команды; уже начатый execute снимает её самостоятельно. */
        if (uasync_cancel_post(ua, &c->post) == ERR_OK) chat_join_port_release(p);
        chat_sync_join_result(inst, request, channel, peer, CHAT_CONNECT_CANCELLED);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "join port closed");
}

void chat_join_port_complete(struct chat_join_port* p, uint64_t request) {
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    if (p->active_request == request) p->active_request = 0;
    pthread_mutex_unlock(&p->lock);
}

int chat_join_port_submit(struct chat_join_port* p, uint64_t request, const char* link, size_t len) {
    return submit(p, request, link, len, 0);
}
int chat_join_port_offer(struct chat_join_port* p, uint64_t request, const char* channel, const char* link, size_t len) {
    if (!chat_channel_valid(channel)) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "offer missing channel"); return CHAT_CONNECT_INVALID; }
    return submit(p, request, link, len, channel);
}
