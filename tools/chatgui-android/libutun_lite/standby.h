/*
 * standby.h — режим standby с duty-cycle (активный/неактивный интервалы)
 *
 * Для chatgui-android: когда приложение в фоне, вместо постоянной работы
 * узел периодически просыпается: ACTIVE (X сек) → SLEEP (Y сек) → ACTIVE ...
 *
 * Потребители периодической работы используют standby_wait(): колбэк
 * в SLEEP срабатывает на старте следующего ACTIVE; в ACTIVE ставится отложенно.
 * Выход из standby (приложение вернулось в форграунд) будит ожидающих немедленно.
 * Singleton: все API, включая getters/cancel, выполняются в одном uasync-потоке.
 * TB = 0.1мс; SLEEP управляет сетевым duty-cycle, не останавливает event loop.
 */
#ifndef STANDBY_H
#define STANDBY_H

#include "u_async.h"

#ifdef __cplusplus
extern "C" {
#endif

struct UTUN_INSTANCE;

/* Привязать к живому uasync. Вызывать после create; перед повторной привязкой обязательно deinit. */
void standby_init(struct UASYNC* ua);

/* Отмена таймеров, освобождение пула и очистка подписок (без вызова колбэков). */
void standby_deinit(void);

/* Применяет режим после подготовки фазового таймера, затем вызывает switch-callback.
 * При выключении будит ожидающих после switch-callback. Повторная команда ничего не перезапускает.
 * Вызывается из utun_set_client_activity в uasync-потоке; 0 — успех, <0 — режим не изменён. */
int standby_set_enabled(struct UTUN_INSTANCE* inst, int enabled);
int  standby_is_enabled(void);

/* Колбэк, вызываемый при каждом переключении standby-режима. */
void standby_set_switch_callback(void (*cb)(int enabled, void* arg), void* arg);

/* Остаток текущего SLEEP в TB (0 = ACTIVE/standby выключен/дедлайн истёк). */
int  standby_get_sleep_tb(void);

/* Сколько TB осталось активного режима (0 = спим или standby выключен). */
int  standby_get_active_remaining_tb(void);

/* Полная настроенная длительность SLEEP в TB, в том числе при выключенном standby. */
int  standby_get_sleep_duration_tb(void);

/* Время до гарантированного пробуждения (timebase 0.1мс): остаток SLEEP, либо
 * остаток ACTIVE + полная длительность SLEEP. 0 = standby выключен. */
int  standby_get_wake_deadline_tb(void);

/* Период keepalive в спячке (мс), из standby_keepalive_ms, минимум 100мс. */
int  standby_get_keepalive_ms(void);

/* Колбэк смены фазы duty-cycle: sleeping=1 — вошли в SLEEP, 0 — вышли в ACTIVE. */
void standby_set_phase_callback(void (*cb)(int sleeping, void* arg), void* arg);

/* При SLEEP ждёт следующего ACTIVE, в ACTIVE вызывает cb отложенно через таймер 0.
 * В SLEEP ждёт фазового события после анонса ACTIVE; при выходе cb вызывается после восстановления линков.
 * При выключенном standby — call_soon без отменяемого handle; при OOM cb может выполниться синхронно.
 * NULL означает отсутствие handle, а не обязательно ошибку. arg заимствуется до cb/cancel/deinit.
 * Handle действителен до первого cb/cancel/deinit, после этого использовать его нельзя. cb не должен быть NULL. */
void* standby_wait(void* arg, void (*cb)(void* arg));
/* Отменить ещё ожидающий callback, не вызывая его; NULL допустим. */
void  standby_wait_cancel(void* handle);

/* Уведомление о внешней сетевой активности (Android ConnectivityManager).
 * В фазе SLEEP после min_sleep пробуждает досрочно; иначе игнорируется.
 * Вызывается на uasync-потоке. */
void  standby_notify_network_activity(void);

/* Диагностика: входящий пакет во время SLEEP-фазы (фазу НЕ меняет).
 * kind — транспорт/тип пакета (строка), who — идентификатор отправителя. */
void  standby_log_rx(const char* kind, const char* who);

/* Переопределить интервалы (мс), минуя chat_setting. Для тестов.
 * active_ms/sleep_ms <= 0 → вернуться к chat_setting.
 * min_sleep_ms ограничивается 0..sleep_ms. Текущий фазовый таймер не переустанавливается;
 * override сохраняется между deinit/init, сбрасывать явно. */
void  standby_set_intervals_ms(int active_ms, int sleep_ms, int min_sleep_ms);

#ifdef __cplusplus
}
#endif
#endif /* STANDBY_H */
