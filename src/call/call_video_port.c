#include "call_video_port.h"
#include "call_video_playout.h"
#include "call_video_transport.h"
#include "call_priv.h"
#include "../chat/chat_setting.h"
#include "../utun_instance.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* Вне port: memset при detach не должен конкурировать с атомарным reader таймера. */
static uint64_t received_id, received_count;
static struct {
    struct UTUN_INSTANCE *inst;
    struct posted_task *pending;
    uint64_t id, bound;
    struct call_video_caps local;
    struct call_video_port_status status;
    struct call_video_format format;
    uint32_t bps, applied_bps;
    uint64_t applied_generation, applied_request;
    int opening, configure, completed, resume, key;
    int range;
    uint32_t minimum, maximum;
    struct call_video_frame tx[CALL_VIDEO_PORT_TX_FRAMES], rx[8];
    unsigned tx_count;
    int tx_processing;
    int64_t tx_wait_started;
    unsigned tx_retries;
    unsigned count, bytes;
    int64_t rx_wait_started; // Диагностика backpressure; свободное место сразу доступно producer.
    void (*rx_wake)(void *);
    void *rx_wake_arg;
    uint64_t decoded_generation;
    int decoded_pending, decoded_accepted;
} port;

static void process(void *arg);
static int schedule(void) {
    if (!port.inst) { DEBUG_WARN(DEBUG_CATEGORY_CALL, "video port: core detached"); return -1; }
    if (port.pending) return 0;
    struct posted_task *task = u_calloc(1, sizeof(*task));
    if (!task) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port task allocation failed"); return -1; }
    task->callback = process; port.pending = task; uasync_post_reserved(port.inst->ua, task); return 0;
}
static void clear_frames(void) {
    for (unsigned i = 0; i < port.tx_count; ++i) u_free((void *)port.tx[i].data);
    port.tx_count = 0;
    port.tx_wait_started = 0; port.tx_retries = 0;
    for (unsigned i = 0; i < port.count; ++i) u_free((void *)port.rx[i].data);
    port.count = port.bytes = 0;
    port.rx_wait_started = 0;
}
static void peer(void *arg, const struct call_video_caps *caps) {
    pthread_mutex_lock(&lock);
    if (port.id == (uint64_t)(uintptr_t)arg) {
        port.status.peer = *caps; port.status.have_peer = 1;
    }
    pthread_mutex_unlock(&lock);
}
static int ready(void *arg, uint32_t bytes) {
    pthread_mutex_lock(&lock);
    int result = 0;
    if (port.id == (uint64_t)(uintptr_t)arg) {
        result = port.count < 8 && bytes <= 4u * 1024u * 1024u - port.bytes;
        if (port.rx_wait_started && result) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video port RX resumed call=%llu wait_us=%lld frames=%u",
                (unsigned long long)port.id, (long long)(call_video_playout_now_us() - port.rx_wait_started), port.count);
            port.rx_wait_started = 0;
        }
        if (!result && !port.rx_wait_started) {
            port.rx_wait_started = call_video_playout_now_us();
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "video port RX full call=%llu frames=%u bytes=%u next_bytes=%u; retain until space",
                (unsigned long long)port.id, port.count, port.bytes, bytes);
        }
    }
    pthread_mutex_unlock(&lock); return result;
}
static void frame(void *arg, const struct call_video_frame *f) {
    int64_t received = call_video_playout_now_us();
    pthread_mutex_lock(&lock);
    if (port.id == (uint64_t)(uintptr_t)arg && port.count < 8 && f->size <= 4u * 1024u * 1024u - port.bytes) {
        uint8_t *data = u_malloc(f->size);
        if (data) {
            memcpy(data, f->data, f->size); port.rx[port.count] = *f;
            port.rx[port.count].received_us = received; port.rx[port.count++].data = data; port.bytes += f->size;
            __atomic_add_fetch(&received_count, 1, __ATOMIC_RELEASE);
            if (port.rx_wake) port.rx_wake(port.rx_wake_arg);
        } else { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port RX allocation failed bytes=%u", f->size); port.key = 1; schedule(); }
    } else { DEBUG_WARN(DEBUG_CATEGORY_CALL, "video port RX changed during delivery"); port.key = 1; schedule(); }
    pthread_mutex_unlock(&lock);
}
static void need_key(void *arg) {
    pthread_mutex_lock(&lock);
    if (port.id == (uint64_t)(uintptr_t)arg) ++port.status.key_serial;
    pthread_mutex_unlock(&lock);
}
static void bitrate(void *arg, uint64_t generation, uint64_t request, uint32_t bps) {
    pthread_mutex_lock(&lock);
    if (port.id == (uint64_t)(uintptr_t)arg) {
        port.status.rate_generation = generation; port.status.rate_request = request; port.status.rate_bps = bps;
    }
    pthread_mutex_unlock(&lock);
}
static void decoded(void *arg, uint64_t generation, int accepted) {
    pthread_mutex_lock(&lock);
    if (port.id == (uint64_t)(uintptr_t)arg) {
        port.status.decoded_generation = generation; port.status.decoded_accepted = accepted;
    }
    pthread_mutex_unlock(&lock);
}
static void writable(void *arg, int admit) {
    pthread_mutex_lock(&lock);
    if (port.id == (uint64_t)(uintptr_t)arg) {
        port.status.can_encode = admit;
        if (port.tx_count && !port.tx_processing) schedule();
    }
    pthread_mutex_unlock(&lock);
}
static const struct call_video_ops ops = {peer, ready, frame, need_key, bitrate, decoded, writable};

/* Снимок команды под mutex; API транспорта вызывается без mutex, callbacks могут войти обратно. */
static void process(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    port.pending = NULL;
    struct UTUN_INSTANCE *inst = port.inst;
    uint64_t id = port.id, old = port.bound;
    int opening = port.opening, configure = port.configure, completed = port.completed, resume = port.resume, key = port.key;
    int range = port.range;
    int decoded_pending = port.decoded_pending, decoded_accepted = port.decoded_accepted;
    uint64_t decoded_generation = port.decoded_generation;
    uint32_t minimum = port.minimum, maximum = port.maximum;
    struct call_video_caps caps = port.local;
    struct call_video_format format = port.format;
    uint32_t bps = port.bps, applied_bps = port.applied_bps;
    uint64_t generation = port.applied_generation, request = port.applied_request;
    struct call_video_frame tx[CALL_VIDEO_PORT_TX_FRAMES];
    unsigned tx_count = port.tx_count;
    memcpy(tx, port.tx, tx_count * sizeof(*tx)); port.tx_count = 0; port.tx_processing = tx_count != 0;
    port.opening = port.configure = port.completed = port.resume = port.key = 0;
    port.range = 0;
    port.decoded_pending = 0;
    if (opening) port.bound = id;
    pthread_mutex_unlock(&lock);
    int result = 0, sent = 1;
    unsigned sent_count = 0;
    /* Команды и завершение сессии исполняются одним uasync-потоком: после этой
     * проверки сессия не может завершиться посреди исполнения снимка. */
    struct call_session *session = inst && id ? call_session_find(inst->call, id) : NULL;
    int active = session && session->state == CALL_ST_ACTIVE;
    if (inst && opening && old && old != id) {
        struct call_session *previous = call_session_find(inst->call, old);
        if (previous && previous->state == CALL_ST_ACTIVE) {
            struct call_video_caps empty = {0}; call_video_configure(inst, old, &empty, NULL, NULL);
        }
    }
    if (inst && id && !active) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video port cancelled stale commands call=%llu state=%d tx_frames=%u",
                    (unsigned long long)id, session ? session->state : -1, tx_count);
        // Проверка id внутри close сохраняет порт следующего звонка, открытый worker во время обработки.
        call_video_port_close(id);
    }
    if (inst && active) {
        if (range) {
            char value[32]; snprintf(value, sizeof(value), "%u:%u", minimum / 1000, maximum / 1000);
            if (chat_setting_set(inst, "call_video_bitrate_range_kbps", value) < 0)
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port bitrate range rejected value=%s", value);
        }
        call_video_rate_parse_range(chat_setting_get(inst, "call_video_bitrate_range_kbps"), &minimum, &maximum);
        if (opening && id) result = call_video_configure(inst, id, &caps, &ops, (void *)(uintptr_t)id);
        if (id && configure) result = call_video_configure_rate(inst, id, &format, bps, format.maximum_bps);
        if (id && completed) call_video_rate_applied(inst, id, generation, request, applied_bps);
        if (id && resume) call_video_resume_receive(inst, id);
        if (id && key) call_video_request_key(inst, id);
        if (id && decoded_pending) call_video_decode_result(inst, id, decoded_generation, decoded_accepted);
        for (; sent_count < tx_count; ++sent_count) {
            sent = call_video_send_frame(inst, &tx[sent_count]);
            if (sent == CALL_VIDEO_SEND_NEEDS_KEY) { sent = 1; continue; }
            if (sent != 1) break;
        }
        if (sent_count < tx_count && sent < 0)
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "video port TX chain failed call=%llu accepted=%u unsent=%u result=%d "
                "first_unsent=%llu last=%llu", (unsigned long long)id, sent_count, tx_count - sent_count, sent,
                (unsigned long long)tx[sent_count].frame_id, (unsigned long long)tx[tx_count - 1].frame_id);
    }
    pthread_mutex_lock(&lock);
    port.tx_processing = 0;
    if (port.id == id) {
        port.status.minimum_bps = minimum; port.status.maximum_bps = maximum;
        if (result < 0) { port.status.error = 1; DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port configuration failed call=%llu", (unsigned long long)id); }
        else if (configure) { port.status.configured_generation = format.generation; port.status.error = 0; }
        if (!sent && active) {
            port.tx_count = tx_count - sent_count;
            memcpy(port.tx, tx + sent_count, port.tx_count * sizeof(*tx));
            if (!port.tx_wait_started) {
                port.tx_wait_started = call_video_playout_now_us();
                DEBUG_WARN(DEBUG_CATEGORY_CALL, "video port TX paused call=%llu retained=%u first=%llu; chain preserved",
                    (unsigned long long)id, port.tx_count, (unsigned long long)port.tx[0].frame_id);
            }
            port.tx_retries++;
        } else if (port.tx_wait_started) {
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "video port TX resumed call=%llu wait_us=%lld retries=%u result=%d",
                (unsigned long long)id, (long long)(call_video_playout_now_us() - port.tx_wait_started), port.tx_retries, sent);
            port.tx_wait_started = 0; port.tx_retries = 0;
        }
        if (sent < 0) ++port.status.key_serial;
    }
    unsigned release = !sent && active && port.id == id ? sent_count : tx_count;
    for (unsigned i = 0; i < release; ++i) u_free((void *)tx[i].data);
    pthread_mutex_unlock(&lock);
}
int call_video_port_attach(struct UTUN_INSTANCE *inst) {
    pthread_mutex_lock(&lock);
    int result = inst && !port.inst ? 0 : -1;
    if (!result) { memset(&port, 0, sizeof(port)); port.inst = inst; DEBUG_INFO(DEBUG_CATEGORY_CALL, "video port attached"); }
    else DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port attach conflict");
    pthread_mutex_unlock(&lock); return result;
}
void call_video_port_detach(struct UTUN_INSTANCE *inst) {
    pthread_mutex_lock(&lock);
    if (port.inst == inst) {
        __atomic_store_n(&received_id, 0, __ATOMIC_RELEASE);
        if (port.pending) uasync_cancel_post(inst->ua, port.pending);
        clear_frames(); memset(&port, 0, sizeof(port));
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "video port detached; queues and pending task released");
    }
    pthread_mutex_unlock(&lock);
}
int call_video_port_open(uint64_t id, const struct call_video_caps *caps) {
    if (!id || !call_video_caps_valid(caps)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port invalid open"); return -1; }
    pthread_mutex_lock(&lock);
    int result = schedule();
    if (!result) {
        if (port.id != id) {
            __atomic_store_n(&received_id, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&received_count, 0, __ATOMIC_RELEASE);
            clear_frames(); port.status = (struct call_video_port_status){0};
            port.rx_wake = NULL; port.rx_wake_arg = NULL;
            port.configure = port.completed = port.resume = port.key = port.range = port.decoded_pending = 0;
        }
        port.id = id; port.local = *caps; port.opening = 1;
        __atomic_store_n(&received_id, id, __ATOMIC_RELEASE);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "video port open requested call=%llu", (unsigned long long)id);
    }
    pthread_mutex_unlock(&lock); return result;
}
void call_video_port_close(uint64_t id) {
    pthread_mutex_lock(&lock);
    if (port.id == id) {
        __atomic_store_n(&received_id, 0, __ATOMIC_RELEASE);
        port.rx_wake = NULL; port.rx_wake_arg = NULL;
        clear_frames(); port.id = 0; port.opening = 1;
        port.configure = port.completed = port.resume = port.key = port.range = port.decoded_pending = 0;
        if (port.inst) schedule();
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "video port close requested call=%llu", (unsigned long long)id);
    }
    pthread_mutex_unlock(&lock);
}
int call_video_port_status(uint64_t id, struct call_video_port_status *out) {
    pthread_mutex_lock(&lock);
    int result = id && id == port.id && port.inst && out ? 0 : -1;
    if (!result) {
        *out = port.status; out->tx_idle = !port.tx_count && !port.tx_processing;
        out->can_encode = out->can_encode && out->tx_idle;
        out->rx_frames = port.count;
    }
    pthread_mutex_unlock(&lock); return result;
}
uint64_t call_video_port_received(uint64_t id) {
    if (!id || __atomic_load_n(&received_id, __ATOMIC_ACQUIRE) != id) return 0;
    uint64_t count = __atomic_load_n(&received_count, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&received_id, __ATOMIC_ACQUIRE) == id ? count : 0;
}
int call_video_port_set_wake(uint64_t id, void (*wake)(void *), void *arg) {
    pthread_mutex_lock(&lock);
    int bound = id && id == port.id && port.inst;
    if (bound) { port.rx_wake = wake; port.rx_wake_arg = arg; }
    pthread_mutex_unlock(&lock); return bound;
}
int call_video_port_format(uint64_t id, const struct call_video_format *format, uint32_t bps) {
    if (!call_video_format_valid(format)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port invalid format"); return -1; }
    pthread_mutex_lock(&lock);
    int result = id && id == port.id ? schedule() : -1;
    if (!result) { port.format = *format; port.bps = bps; port.configure = 1; port.status.error = 0; }
    pthread_mutex_unlock(&lock); return result;
}
int call_video_port_applied(uint64_t id, uint64_t generation, uint64_t request, uint32_t bps) {
    pthread_mutex_lock(&lock);
    int result = id && id == port.id ? schedule() : -1;
    if (!result) { port.applied_generation = generation; port.applied_request = request; port.applied_bps = bps; port.completed = 1; }
    pthread_mutex_unlock(&lock); return result;
}
int call_video_port_send(const struct call_video_frame *frames, unsigned count) {
    if (!frames || !count || count > CALL_VIDEO_PORT_TX_FRAMES) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port invalid TX batch count=%u", count); return -1;
    }
    uint32_t bytes = 0;
    for (unsigned i = 0; i < count; ++i) {
        const struct call_video_frame *f = &frames[i];
        if (!f->data || !f->size || f->size > CALL_VIDEO_MAX_FRAME || f->size > CALL_VIDEO_PORT_TX_BYTES - bytes
            || f->offset || f->length != f->size
            || f->call_id != frames[0].call_id || !call_video_same_format(&f->format, &frames[0].format)
            || (i && (f->frame_id <= frames[i - 1].frame_id || f->pts_us <= frames[i - 1].pts_us))) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port invalid TX batch frame=%u count=%u bytes=%u", i, count, bytes); return -1;
        }
        bytes += f->size;
    }
    pthread_mutex_lock(&lock);
    int result = frames[0].call_id && frames[0].call_id == port.id && port.inst ? 0 : -1;
    if (!result && !port.tx_count && !port.tx_processing) {
        unsigned copied = 0;
        for (; copied < count; ++copied) {
            uint8_t *data = u_malloc(frames[copied].size);
            if (!data) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port TX allocation failed frame=%u bytes=%u", copied, bytes); break; }
            memcpy(data, frames[copied].data, frames[copied].size); port.tx[copied] = frames[copied]; port.tx[copied].data = data;
        }
        if (copied == count && !schedule()) {
            port.tx_count = count; result = 1;
            if (count > 1) DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video port TX batch accepted call=%llu frames=%u bytes=%u",
                (unsigned long long)port.id, count, bytes);
        } else {
            for (unsigned i = 0; i < copied; ++i) u_free((void *)port.tx[i].data);
            result = -1;
        }
    }
    if (result == 0) DEBUG_TRACE(DEBUG_CATEGORY_CALL,
        "video port TX retry call=%llu frames=%u first=%llu pending=%u processing=%d",
        (unsigned long long)frames[0].call_id, count, (unsigned long long)frames[0].frame_id, port.tx_count, port.tx_processing);
    pthread_mutex_unlock(&lock); return result;
}
int call_video_port_pull(uint64_t id, struct call_video_frame *out) {
    pthread_mutex_lock(&lock);
    int result = 0;
    if (id && id == port.id && port.count && out) {
        *out = port.rx[0]; port.bytes -= out->size; --port.count;
        memmove(port.rx, port.rx + 1, port.count * sizeof(port.rx[0])); port.resume = 1; schedule(); result = 1;
    }
    pthread_mutex_unlock(&lock); return result;
}
void call_video_port_request_key(uint64_t id) {
    pthread_mutex_lock(&lock);
    if (id && id == port.id) { port.key = 1; schedule(); }
    pthread_mutex_unlock(&lock);
}
int call_video_port_range(uint64_t id, uint32_t minimum, uint32_t maximum) {
    if (minimum < 300000 || maximum > 4000000 || minimum > maximum || minimum % 1000 || maximum % 1000) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port invalid range min=%u max=%u", minimum, maximum); return -1;
    }
    pthread_mutex_lock(&lock);
    int result = id && id == port.id ? schedule() : -1;
    if (!result) { port.minimum = minimum; port.maximum = maximum; port.range = 1; }
    pthread_mutex_unlock(&lock); return result;
}
int call_video_port_decoded(uint64_t id, uint64_t generation, int accepted) {
    if (!generation || (accepted != 0 && accepted != 1)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video port invalid decoder result"); return -1; }
    pthread_mutex_lock(&lock);
    int result = id && id == port.id ? schedule() : -1;
    if (!result) { port.decoded_generation = generation; port.decoded_accepted = accepted; port.decoded_pending = 1; }
    pthread_mutex_unlock(&lock); return result;
}
