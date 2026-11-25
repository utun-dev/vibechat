/*
 * test_standby.c — юнит-тест модуля standby (chatgui-android)
 *
 * Покрывает все сценарии: init, ENTER/EXIT, смену фаз ACTIVE↔SLEEP,
 * standby_wait во всех фазах, cancel, сетевую активность (игнор/раннее
 * пробуждение), clamp min_sleep и re-entry в wake_all_waiters.
 *
 * Интервалы задаются через standby_set_intervals_ms() (быстрые: 100-300мс).
 * Тестовые часы ждут ближайший таймер или заданный интервал;
 * callbacks и смену фаз выполняет настоящий uasync.
 */
#include "standby.h"
#include "debug_config.h"
#include "../../../../tests/test_clock.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); \
    } \
} while (0)

static struct UASYNC* g_ua = NULL;

/* ── колбэки для standby_wait и switch ── */

static int g_wait_calls = 0;
static void wait_cb(void* arg) {
    (void)arg;
    g_wait_calls++;
}

static int g_switch_calls = 0;
static int g_switch_last_enabled = -1;
static void switch_cb(int enabled, void* arg) {
    (void)arg;
    g_switch_calls++;
    g_switch_last_enabled = enabled;
}

/* re-entry waiter: в колбэке снова регистрирует себя (ровно один раз) */
static int g_reentry_calls = 0;
static void* g_cancel_other = NULL;
static void cancel_other_cb(void* arg) {
    (void)arg;
    standby_wait_cancel(g_cancel_other);
    g_cancel_other = NULL;
}
static void reentry_cb(void* arg) {
    (void)arg;
    g_reentry_calls++;
    if (g_reentry_calls == 1) standby_wait(NULL, reentry_cb);
}

/* ── хелперы ── */

static void reset_standby(void) {
    standby_deinit();
    standby_set_intervals_ms(200, 300, 100);  /* active=200ms sleep=300ms min_sleep=100ms */
    standby_init(g_ua);
    g_wait_calls = 0;
    g_switch_calls = 0;
    g_switch_last_enabled = -1;
    g_reentry_calls = 0;
    standby_set_switch_callback(switch_cb, NULL);
}

/* продвинуть время до ближайшего таймера (фаза переключится) */
static void run_to_next_phase(void) {
    test_clock_next_timer(g_ua);
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    test_clock_init(1000);

    g_ua = uasync_create();
    if (!g_ua) { printf("uasync_create failed\n"); return 1; }

    printf("=== standby unit test ===\n");

    /* ── 1. init ── */
    reset_standby();
    CHECK(standby_is_enabled() == 0, "init: should be disabled");
    CHECK(standby_get_sleep_tb() == 0, "init: sleep_tb should be 0");
    CHECK(standby_get_active_remaining_tb() == 0, "init: active_remaining should be 0");

    /* ── 2. ENTER ── */
    standby_set_enabled(NULL, 1);
    CHECK(standby_is_enabled() == 1, "ENTER: should be enabled");
    CHECK(standby_get_sleep_tb() == 0, "ENTER: active phase → sleep_tb 0");
    CHECK(standby_get_active_remaining_tb() > 0, "ENTER: active phase → active_remaining > 0");
    CHECK(g_switch_calls == 1 && g_switch_last_enabled == 1, "ENTER: switch_cb(1) once");

    /* ── 3. ACTIVE → SLEEP ── */
    run_to_next_phase();
    CHECK(standby_get_sleep_tb() > 0, "ACTIVE→SLEEP: sleep_tb > 0");
    CHECK(standby_get_active_remaining_tb() == 0, "ACTIVE→SLEEP: active_remaining == 0");

    /* ── 4. SLEEP → ACTIVE (по таймеру) ── */
    run_to_next_phase();
    CHECK(standby_get_sleep_tb() == 0, "SLEEP→ACTIVE: sleep_tb == 0");
    CHECK(standby_get_active_remaining_tb() > 0, "SLEEP→ACTIVE: active_remaining > 0");

    /* ── 5. EXIT ── */
    standby_set_enabled(NULL, 0);
    CHECK(standby_is_enabled() == 0, "EXIT: should be disabled");
    CHECK(standby_get_sleep_tb() == 0, "EXIT: sleep_tb == 0");
    CHECK(g_switch_calls == 2 && g_switch_last_enabled == 0, "EXIT: switch_cb(0)");

    /* ── 6. standby_wait при выключенном → сразу ── */
    reset_standby();
    standby_wait(NULL, wait_cb);
    uasync_poll(g_ua, 0);   /* обработать call_soon */
    CHECK(g_wait_calls == 1, "wait(disabled): cb fired immediately");

    /* ── 7. standby_wait в ACTIVE → будит сразу (burst, без паузы) ── */
    reset_standby();
    standby_set_enabled(NULL, 1);
    standby_wait(NULL, wait_cb);   /* ACTIVE → tb=0, immediate */
    uasync_poll(g_ua, 0);
    CHECK(g_wait_calls == 1, "wait(ACTIVE): fired immediately (burst)");
    standby_set_enabled(NULL, 0);

    /* ── 8. standby_wait в SLEEP → ждёт конца сна ── */
    reset_standby();
    standby_set_enabled(NULL, 1);
    run_to_next_phase();            /* → SLEEP */
    CHECK(standby_get_sleep_tb() > 0, "wait(SLEEP): in sleep");
    standby_wait(NULL, wait_cb);
    run_to_next_phase();           /* конец сна */
    uasync_poll(g_ua, 0);
    CHECK(g_wait_calls == 1, "wait(SLEEP): fired at sleep end");
    standby_set_enabled(NULL, 0);

    /* ── 9. standby_wait_cancel ── */
    reset_standby();
    standby_set_enabled(NULL, 1);
    run_to_next_phase();            /* → SLEEP */
    void* h = standby_wait(NULL, wait_cb);
    standby_wait_cancel(h);
    run_to_next_phase();           /* конец сна */
    uasync_poll(g_ua, 0);
    CHECK(g_wait_calls == 0, "wait_cancel: cb not called");
    standby_set_enabled(NULL, 0);

    /* ── 10. notify при standby off → игнор ── */
    reset_standby();
    standby_notify_network_activity();
    CHECK(standby_is_enabled() == 0, "notify(off): still disabled");

    /* ── 11. notify в ACTIVE → игнор ── */
    standby_set_enabled(NULL, 1);
    CHECK(standby_get_active_remaining_tb() > 0, "notify(ACTIVE): in active");
    standby_notify_network_activity();
    CHECK(standby_get_active_remaining_tb() > 0, "notify(ACTIVE): still active");
    standby_set_enabled(NULL, 0);

    /* ── 12. notify в SLEEP до min_sleep → игнор ── */
    reset_standby();
    standby_set_enabled(NULL, 1);
    run_to_next_phase();            /* → SLEEP */
    CHECK(standby_get_sleep_tb() > 0, "notify(min_sleep): in sleep");
    standby_notify_network_activity();   /* сразу — ещё < min_sleep(100ms) */
    CHECK(standby_get_sleep_tb() > 0, "notify(<min_sleep): still sleeping");

    /* ── 13. notify в SLEEP после min_sleep → раннее пробуждение ── */
    test_clock_run_for(g_ua, 2000); /* +200ms → прошло min_sleep(100ms) */
    standby_notify_network_activity();
    CHECK(standby_get_sleep_tb() == 0, "notify(>min_sleep): woke → sleep_tb 0");
    CHECK(standby_get_active_remaining_tb() > 0, "notify(>min_sleep): woke → active_remaining > 0");
    standby_set_enabled(NULL, 0);

    /* ── 14. раннее пробуждение будит waiters ── */
    reset_standby();
    standby_set_enabled(NULL, 1);
    run_to_next_phase();            /* → SLEEP */
    standby_wait(NULL, wait_cb);
    test_clock_run_for(g_ua, 2000); /* +200ms → min_sleep прошёл */
    standby_notify_network_activity();
    uasync_poll(g_ua, 0);
    CHECK(g_wait_calls == 1, "early wake: waiter fired immediately");
    standby_set_enabled(NULL, 0);

    /* ── 15. clamp min_sleep ≥ sleep → notify всегда игнор ── */
    reset_standby();
    standby_set_intervals_ms(200, 300, 500);  /* min_sleep(500) > sleep(300) → clamp */
    standby_init(g_ua);
    standby_set_switch_callback(switch_cb, NULL);
    standby_set_enabled(NULL, 1);
    run_to_next_phase();            /* → SLEEP */
    test_clock_run_for(g_ua, 2000); /* +200ms */
    standby_notify_network_activity();
    CHECK(standby_get_sleep_tb() > 0, "clamp: min_sleep==sleep → notify ignored");
    standby_set_enabled(NULL, 0);

    /* ── 16. re-entry: waiter перерегистрируется, без бесконечного цикла ── */
    reset_standby();
    standby_set_enabled(NULL, 1);
    run_to_next_phase();            /* → SLEEP */
    standby_wait(NULL, reentry_cb);
    test_clock_run_for(g_ua, 2000); /* +200ms → min_sleep прошёл */
    standby_notify_network_activity();   /* раннее пробуждение → wake_all_waiters */
    CHECK(g_reentry_calls == 1, "re-entry: wake fired waiter once (no infinite loop)");
    CHECK(standby_is_enabled() == 1, "re-entry: still enabled after early wake");
    standby_set_enabled(NULL, 0);   /* EXIT → будит перерегистрированного ещё раз */
    CHECK(g_reentry_calls == 2, "re-entry: exit fired re-registered waiter once more");

    /* ── 17. колбэк отменяет другого waiter из уже отцепленного списка ── */
    reset_standby();
    standby_set_enabled(NULL, 1);
    run_to_next_phase();
    CHECK(standby_wait(NULL, cancel_other_cb) != NULL, "cross-cancel: first waiter registered");
    g_cancel_other = standby_wait(NULL, wait_cb);
    CHECK(g_cancel_other != NULL, "cross-cancel: second waiter registered");
    standby_set_enabled(NULL, 0);
    CHECK(g_wait_calls == 0 && g_cancel_other == NULL, "cross-cancel: cancelled callback not dispatched");

    /* ── 18. даже неиспользованный standby очищает подписки при deinit ── */
    reset_standby();
    standby_deinit();
    standby_init(g_ua);
    standby_set_enabled(NULL, 1);
    CHECK(g_switch_calls == 0, "deinit: old switch callback cleared");
    standby_set_enabled(NULL, 0);

    /* ── итог ── */
    standby_deinit();
    uasync_poll(g_ua, 0);
    CHECK(g_ua->timer_alloc_count == g_ua->timer_free_count, "cleanup: all timers freed before loop destruction");
    uasync_destroy(g_ua, 0);

    printf("=== standby unit test: %d checks, %d failures ===\n", g_checks, g_failures);
    if (g_failures == 0) printf("PASS\n");
    else printf("FAIL\n");
    test_clock_finish();
    return g_failures ? 1 : 0;
}
