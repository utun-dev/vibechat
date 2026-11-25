/*
 * standby.c — режим standby с duty-cycle (активный/неактивный интервалы)
 *
 * Синглтон (одна instance на процесс — как instance_lite.c).
 * Таймеры только через uasync. Интервалы читаются лениво из chat_setting,
 * поэтому их изменение через GUI применяется на границе следующей фазы.
 *
 * Duty-cycle:
 *   standby включён → фаза ACTIVE на standby_active_sec, затем SLEEP на
 *   standby_sleep_sec, и так по кругу, пока standby не выключат.
 *
 * Досрочное пробуждение:
 *   в фазе SLEEP первые standby_min_sleep_sec секунд спим безусловно;
 *   после них standby_notify_network_activity() (внешняя сетевая активность)
 *   пробуждает досрочно — следующий цикл как обычно.
 *
 * Список ожидающих (standby_wait) — двусвязный список, элементы из memory_pool.
 * В SLEEP ожидающие будятся фазовым таймером после анонса ACTIVE пирам.
 * При выходе из standby они будятся после восстановления режима линков.
 */
#include "standby.h"
#include "chat_setting.h"
#include "utun_instance.h"
#include "debug_config.h"
#include "memory_pool.h"

#define STANDBY_TB_PER_SEC 10000   /* timebase = 0.1ms → 1 сек = 10000 TB */

#define PHASE_ACTIVE 0
#define PHASE_SLEEP  1

struct standby_wait_entry {
    struct standby_wait_entry* prev;
    struct standby_wait_entry* next;
    void* timer_id;                 /* handle uasync_set_timeout */
    void (*cb)(void* arg);
    void* arg;
    int dispatching;               /* отцеплен для пробуждения; cancel только снимает cb */
};

static struct UASYNC* g_ua = NULL;
static struct UTUN_INSTANCE* g_inst = NULL;   /* для чтения chat_setting (per-instance) */
static int  g_enabled = 0;
static int  g_phase = PHASE_ACTIVE;
static uint64_t g_phase_start_tb = 0;   /* начало текущей фазы */
static uint64_t g_phase_end_tb = 0;     /* дедлайн текущей фазы */
static uint64_t g_min_sleep_end_tb = 0; /* до этой точки спим безусловно */
static void* g_phase_timer = NULL;
static void (*g_switch_cb)(int enabled, void* arg) = NULL;
static void* g_switch_arg = NULL;
static void (*g_phase_cb)(int sleeping, void* arg) = NULL;
static void* g_phase_cb_arg = NULL;

static struct memory_pool* g_wait_pool = NULL;
static struct standby_wait_entry* g_wait_head = NULL;
static struct standby_wait_entry* g_wait_tail = NULL;

/* счётчики для диагностики */
static uint32_t g_cnt_wakes_timer = 0;
static uint32_t g_cnt_wakes_early = 0;
static uint32_t g_cnt_sleeps      = 0;
static uint32_t g_cnt_net_events  = 0;
static uint32_t g_cnt_net_ignored = 0;

static int standby_active_sec(void) {
    return chat_setting_get_int(g_inst, "standby_active_sec", 2);
}

static int standby_sleep_sec(void) {
    return chat_setting_get_int(g_inst, "standby_sleep_sec", 60);
}

static int standby_min_sleep_sec(void) {
    int m = chat_setting_get_int(g_inst, "standby_min_sleep_sec", 15);
    if (m < 0) m = 0;
    int s = standby_sleep_sec();
    if (m > s) m = s;
    return m;
}

/* override интервалов (мс) для тестов: 0 = использовать chat_setting */
static int g_override = 0;
static int g_active_ms = 0;
static int g_sleep_ms = 0;
static int g_min_sleep_ms = 0;

static int standby_active_tb(void) {
    return g_override ? g_active_ms * 10 : standby_active_sec() * STANDBY_TB_PER_SEC;
}

static int standby_sleep_tb(void) {
    return g_override ? g_sleep_ms * 10 : standby_sleep_sec() * STANDBY_TB_PER_SEC;
}

static int standby_min_sleep_tb(void) {
    int m = g_override ? g_min_sleep_ms * 10 : standby_min_sleep_sec() * STANDBY_TB_PER_SEC;
    if (m < 0) m = 0;
    int s = standby_sleep_tb();
    if (m > s) m = s;
    return m;
}

/* ── фазовый таймер ── */

static void standby_phase_timer_cb(void* arg);

/* Сначала создаёт новый таймер; при ошибке сохраняет прежнюю фазу и её таймер. */
static int standby_arm_phase(int phase) {
    int dur_tb = (phase == PHASE_ACTIVE) ? standby_active_tb() : standby_sleep_tb();
    uint64_t now = get_time_tb();
    void* timer = uasync_set_timeout(g_ua, dur_tb, NULL, standby_phase_timer_cb, "standby_phase");
    if (!timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "standby: failed to arm phase=%d duration=%dms", phase, dur_tb / 10);
        return -1;
    }
    if (g_phase_timer) uasync_cancel_timeout(g_ua, g_phase_timer);
    g_phase_timer = timer;
    g_phase = phase;
    g_phase_start_tb = now;
    g_phase_end_tb = now + (uint64_t)dur_tb;
    g_min_sleep_end_tb = phase == PHASE_SLEEP ? now + (uint64_t)standby_min_sleep_tb() : 0;
    return 0;
}

static void standby_wake_all_waiters(void);

static void standby_phase_timer_cb(void* arg) {
    (void)arg;
    g_phase_timer = NULL;
    if (!g_enabled) return;
    uint64_t elapsed = get_time_tb() - g_phase_start_tb;
    /* переармить фазу ДО фазового колбэка: колбэк читает g_phase_end_tb
       (ka_active_remaining_01s), иначе он увидит старый (уже истёкший) дедлайн. */
    if (standby_arm_phase(g_phase == PHASE_ACTIVE ? PHASE_SLEEP : PHASE_ACTIVE) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "standby: phase timer lost; returning to normal mode");
        if (g_inst) utun_set_client_activity(g_inst, 1); else standby_set_enabled(NULL, 0);
        return;
    }
    if (g_phase == PHASE_SLEEP) {
        g_cnt_sleeps++;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL,
                   "standby: SLEEP (active done in %d.%ds, sleep=%dms min_sleep=%dms)",
                   (int)(elapsed / STANDBY_TB_PER_SEC), (int)((elapsed % STANDBY_TB_PER_SEC) / 1000),
                   standby_sleep_tb() / 10, standby_min_sleep_tb() / 10);
        if (g_phase_cb) g_phase_cb(1, g_phase_cb_arg);
    } else {
        g_cnt_wakes_timer++;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "standby: WAKE (timer), slept %d.%ds",
                   (int)(elapsed / STANDBY_TB_PER_SEC), (int)((elapsed % STANDBY_TB_PER_SEC) / 1000));
        if (g_phase_cb) g_phase_cb(0, g_phase_cb_arg);
        standby_wake_all_waiters();
    }
}

/* ── список ожидающих ── */

static struct standby_wait_entry* wait_entry_alloc(void) {
    if (!g_wait_pool) {
        g_wait_pool = memory_pool_init(sizeof(struct standby_wait_entry), "standby_pool");
        if (!g_wait_pool) return NULL;
    }
    return (struct standby_wait_entry*)memory_pool_alloc(g_wait_pool);
}

static void wait_entry_free(struct standby_wait_entry* e) {
    memory_pool_free(g_wait_pool, e);
}

static void wait_list_remove(struct standby_wait_entry* e) {
    if (e->prev) e->prev->next = e->next; else g_wait_head = e->next;
    if (e->next) e->next->prev = e->prev; else g_wait_tail = e->prev;
    e->prev = e->next = NULL;
}

static void wait_list_push(struct standby_wait_entry* e) {
    e->prev = g_wait_tail;
    e->next = NULL;
    if (g_wait_tail) g_wait_tail->next = e; else g_wait_head = e;
    g_wait_tail = e;
}

/* Отменяет все wait-таймеры и вызывает их колбэки немедленно.
 * Сначала отцепляем весь список — новые регистрации в колбэках
 * (standby_wait) попадут в свежий список и не будут задеты этим проходом. */
static void standby_wake_all_waiters(void) {
    struct standby_wait_entry* list = g_wait_head;
    g_wait_head = g_wait_tail = NULL;
    for (struct standby_wait_entry* e = list; e; e = e->next) {
        e->dispatching = 1;
        if (e->timer_id) {
            uasync_cancel_timeout(g_ua, e->timer_id);
            e->timer_id = NULL;
        }
    }
    while (list) {
        struct standby_wait_entry* e = list;
        list = e->next;
        void (*cb)(void*) = e->cb;
        void* arg = e->arg;
        wait_entry_free(e);
        if (cb) cb(arg);
    }
}

static void standby_wait_timer_cb(void* arg) {
    struct standby_wait_entry* e = (struct standby_wait_entry*)arg;
    e->timer_id = NULL;
    wait_list_remove(e);
    void (*cb)(void*) = e->cb;
    void* user_arg = e->arg;
    wait_entry_free(e);
    cb(user_arg);
}

/* ── публичный API ── */

void standby_init(struct UASYNC* ua) {
    g_ua = ua;
    g_inst = NULL;
    g_enabled = 0;
    g_phase = PHASE_ACTIVE;
    g_phase_timer = NULL;
    g_cnt_wakes_timer = g_cnt_wakes_early = 0;
    g_cnt_sleeps = g_cnt_net_events = g_cnt_net_ignored = 0;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "standby: init (active=%dms sleep=%dms min_sleep=%dms)",
               standby_active_tb() / 10, standby_sleep_tb() / 10, standby_min_sleep_tb() / 10);
}

void standby_deinit(void) {
    if (g_phase_timer && g_ua) {
        uasync_cancel_timeout(g_ua, g_phase_timer);
        g_phase_timer = NULL;
    }
    while (g_wait_head) {
        struct standby_wait_entry* e = g_wait_head;
        if (e->timer_id && g_ua) uasync_cancel_timeout(g_ua, e->timer_id);
        wait_list_remove(e);
        wait_entry_free(e);
    }
    if (g_wait_pool) {
        memory_pool_destroy(g_wait_pool);
        g_wait_pool = NULL;
    }
    g_enabled = 0;
    g_phase = PHASE_ACTIVE;
    g_phase_start_tb = g_phase_end_tb = g_min_sleep_end_tb = 0;
    g_ua = NULL;
    g_inst = NULL;
    g_switch_cb = NULL;
    g_switch_arg = NULL;
    g_phase_cb = NULL;
    g_phase_cb_arg = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "standby: deinit (wakes_timer=%u wakes_early=%u sleeps=%u net=%u ignored=%u)",
               g_cnt_wakes_timer, g_cnt_wakes_early, g_cnt_sleeps, g_cnt_net_events, g_cnt_net_ignored);
}

int standby_set_enabled(struct UTUN_INSTANCE* inst, int enabled) {
    if (!g_ua || (inst && inst->ua != g_ua)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "standby: set_enabled(%d) ua=%p instance_ua=%p", enabled,
                    (void*)g_ua, (void*)(inst ? inst->ua : NULL));
        return -1;
    }
    enabled = !!enabled;
    if (inst) g_inst = inst;
    if (g_enabled == enabled) return 0;

    if (enabled) {
        if (standby_arm_phase(PHASE_ACTIVE) < 0) return -1;
        g_enabled = 1;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "standby: ENTER (active=%dms sleep=%dms min_sleep=%dms)",
                   standby_active_tb() / 10, standby_sleep_tb() / 10, standby_min_sleep_tb() / 10);
    } else {
        g_enabled = 0;
        if (g_phase_timer) {
            uasync_cancel_timeout(g_ua, g_phase_timer);
            g_phase_timer = NULL;
        }
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "standby: EXIT (wakes_timer=%u wakes_early=%u sleeps=%u net=%u ignored=%u)",
                   g_cnt_wakes_timer, g_cnt_wakes_early, g_cnt_sleeps, g_cnt_net_events, g_cnt_net_ignored);
    }

    if (g_switch_cb) g_switch_cb(enabled, g_switch_arg);
    if (!enabled) standby_wake_all_waiters();
    return 0;
}

int standby_is_enabled(void) {
    return g_enabled;
}

void standby_set_switch_callback(void (*cb)(int enabled, void* arg), void* arg) {
    g_switch_cb = cb;
    g_switch_arg = arg;
}

int standby_get_sleep_tb(void) {
    if (!g_enabled) return 0;
    if (g_phase == PHASE_ACTIVE) return 0;
    uint64_t now = get_time_tb();
    if (now >= g_phase_end_tb) return 0;
    return (int)(g_phase_end_tb - now);
}

int standby_get_active_remaining_tb(void) {
    if (!g_enabled) return 0;
    if (g_phase == PHASE_SLEEP) return 0;
    uint64_t now = get_time_tb();
    if (now >= g_phase_end_tb) return 0;
    return (int)(g_phase_end_tb - now);
}

int standby_get_sleep_duration_tb(void) {
    return standby_sleep_tb();
}

int standby_get_wake_deadline_tb(void) {
    if (!g_enabled) return 0;
    if (g_phase == PHASE_SLEEP) return standby_get_sleep_tb();
    /* ACTIVE: остаток активного + полная длительность предстоящего сна */
    return standby_get_active_remaining_tb() + standby_sleep_tb();
}

int standby_get_keepalive_ms(void) {
    int ms = chat_setting_get_int(g_inst, "standby_keepalive_ms", 30000);
    if (ms < 100) ms = 100;
    return ms;
}

void standby_set_phase_callback(void (*cb)(int sleeping, void* arg), void* arg) {
    g_phase_cb = cb;
    g_phase_cb_arg = arg;
}

void* standby_wait(void* arg, void (*cb)(void* arg)) {
    if (!cb) return NULL;
    if (!g_ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "standby: wait() but not initialized");
        return NULL;
    }
    if (!g_enabled) {
        /* standby выключен → будим сразу (отложенно, чтобы не было рекурсии) */
        uasync_call_soon(g_ua, arg, (timeout_callback_t)cb);
        return NULL;
    }

    struct standby_wait_entry* e = wait_entry_alloc();
    if (!e) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "standby: wait_entry_alloc failed");
        cb(arg);
        return NULL;
    }
    e->cb = cb;
    e->arg = arg;
    e->dispatching = 0;

    /* SLEEP ждёт фазового события; ACTIVE вызывает cb отложенно, без рекурсии. */
    e->timer_id = NULL;
    if (g_phase == PHASE_ACTIVE) {
        e->timer_id = uasync_set_timeout(g_ua, 0, e, standby_wait_timer_cb, "standby_wait");
        if (!e->timer_id) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "standby: uasync_set_timeout failed");
            wait_entry_free(e);
            cb(arg);
            return NULL;
        }
    }
    wait_list_push(e);
    return e;
}

void standby_wait_cancel(void* handle) {
    if (!handle) return;
    struct standby_wait_entry* e = (struct standby_wait_entry*)handle;
    if (e->dispatching) { e->cb = NULL; return; }
    if (e->timer_id && g_ua) {
        uasync_cancel_timeout(g_ua, e->timer_id);
        e->timer_id = NULL;
    }
    wait_list_remove(e);
    wait_entry_free(e);
}

/* ── внешняя сетевая активность ── */

void standby_notify_network_activity(void) {
    g_cnt_net_events++;
    if (!g_enabled) {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "standby: network activity ignored (standby off)");
        return;
    }
    if (g_phase != PHASE_SLEEP) {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "standby: network activity ignored (phase=ACTIVE)");
        return;
    }

    uint64_t now = get_time_tb();
    uint64_t slept = now - g_phase_start_tb;
    if (now < g_min_sleep_end_tb) {
        g_cnt_net_ignored++;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL,
                   "standby: network activity caught, min_sleep not elapsed (%d.%ds left)",
                   (int)((g_min_sleep_end_tb - now) / STANDBY_TB_PER_SEC),
                   (int)(((g_min_sleep_end_tb - now) % STANDBY_TB_PER_SEC) / 1000));
        return;
    }

    if (standby_arm_phase(PHASE_ACTIVE) < 0) return;
    g_cnt_wakes_early++;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL,
               "standby: WAKE early (network activity), slept %d.%ds min_sleep=%dms",
               (int)(slept / STANDBY_TB_PER_SEC), (int)((slept % STANDBY_TB_PER_SEC) / 1000),
               standby_min_sleep_tb() / 10);

    if (g_phase_cb) g_phase_cb(0, g_phase_cb_arg);
    standby_wake_all_waiters();
}

void standby_log_rx(const char* kind, const char* who) {
    if (!g_enabled || g_phase != PHASE_SLEEP) return;
    uint64_t slept = get_time_tb() - g_phase_start_tb;
    DEBUG_DEBUG(DEBUG_CATEGORY_KEEPALIVE,
               "standby: rx in SLEEP: %s from %s, slept %d.%ds",
               kind ? kind : "?", who ? who : "?",
               (int)(slept / STANDBY_TB_PER_SEC), (int)((slept % STANDBY_TB_PER_SEC) / 1000));
}

void standby_set_intervals_ms(int active_ms, int sleep_ms, int min_sleep_ms) {
    if (active_ms <= 0 || sleep_ms <= 0) {
        g_override = 0;
        return;
    }
    g_override = 1;
    g_active_ms = active_ms;
    g_sleep_ms = sleep_ms;
    g_min_sleep_ms = min_sleep_ms < 0 ? 0 : min_sleep_ms;
}
