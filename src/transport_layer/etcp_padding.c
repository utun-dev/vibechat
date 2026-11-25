#include "etcp_padding.h"
#include "etcp.h"
#include "utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/u_async.h"
#include <stdlib.h>

static unsigned padding_random(unsigned min, unsigned max) {
    return min + (unsigned)rand() % (max - min + 1);
}

static void padding_generate(struct ETCP_PADDING* padding) {
    padding->min = padding_random(0, 30);
    padding->max = padding->min + padding_random(32, 128);
}

static void padding_timer_cb(void* arg);

static int padding_refresh(struct ETCP_PADDING* padding) {
    padding_generate(padding);
    unsigned seconds = padding_random(50, 300);
    padding->next_refresh_tb = get_time_tb() + (uint64_t)seconds * 10000;
    padding->timer = uasync_set_timeout(padding->ua, seconds * 10000, padding, padding_timer_cb, "etcp_padding");
    if (!padding->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] padding=%p: refresh timer allocation failed", padding->name, (void*)padding);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] padding=%p: range=%u..%u refresh_in=%us",
                padding->name, (void*)padding, padding->min, padding->max, seconds);
    return 0;
}

static void padding_timer_cb(void* arg) {
    struct ETCP_PADDING* padding = arg;
    padding->timer = NULL;
    padding_refresh(padding);
}

int etcp_padding_start(struct ETCP_PADDING* padding, struct UASYNC* ua, const char* name) {
    if (!padding || !ua || padding->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "padding: invalid start padding=%p ua=%p", (void*)padding, (void*)ua);
        return -1;
    }
    padding->ua = ua;
    padding->name = name && name[0] ? name : "instance";
    return padding_refresh(padding);
}

void etcp_padding_stop(struct ETCP_PADDING* padding) {
    if (!padding) return;
    if (padding->timer) {
        uasync_cancel_timeout(padding->ua, padding->timer);
        padding->timer = NULL;
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] padding=%p: refresh timer cancelled", padding->name, (void*)padding);
    }
    padding->ua = NULL;
    padding->next_refresh_tb = 0;
}

int etcp_padding_size(const struct ETCP_PADDING* padding, size_t available) {
    struct ETCP_PADDING temporary = {0};
    if (!padding) { padding_generate(&temporary); padding = &temporary; }
    if (padding->min > padding->max || padding->max > ETCP_PADDING_MAX || available < padding->min) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "padding: invalid range/capacity min=%u max=%u available=%zu",
                    padding->min, padding->max, available);
        return -1;
    }
    unsigned max = available < padding->max ? (unsigned)available : padding->max;
    return padding_random(padding->min, max);
}

const struct ETCP_PADDING* etcp_padding_for_peer(struct UTUN_INSTANCE* inst, const uint8_t* public_key) {
    if (!inst || !public_key) return NULL;
    struct ETCP_CONN* conn = instance_find_conn_by_key(inst, public_key);
    if (conn && !conn->close_requested) return &conn->padding;
    return &inst->padding;
}
