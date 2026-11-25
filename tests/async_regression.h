/* Linux/ELF-стенд регрессий: regression_run запускает каждый случай в fork с лимитом 2000мс реального времени.
 * Падение/зависание не скрывает остальные случаи; успешный случай дополнительно проверяется на u_* и fd-утечки.
 * Снимки fd читаются из /proc/self/fd; если proc недоступен, этот контроль не работает.
 * fault/clock/epoll работают через --wrap из async_regression_ldflags в tests/Makefile.am.
 * Состояние подмен глобально, без синхронизации; не менять его параллельно с использующими его вызовами.
 * Это отдельный стенд, его clock wrapper нельзя совмещать с wrappers test_clock.c в одном бинарнике. */
#ifndef ASYNC_REGRESSION_H
#define ASYNC_REGRESSION_H

#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/mem.h"
#include <stdio.h>
#include <sys/epoll.h>

/* Проверки вычисляют аргументы по одному разу; при отказе печатают место/условие и возвращают 1 из run.
 * CHECK_EQ сравнивает после преобразования к long long; макросы не выполняют cleanup случая. */
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; \
} } while (0)
#define CHECK_EQ(actual, expected) do { \
    long long a_ = (long long)(actual), e_ = (long long)(expected); \
    if (a_ != e_) { fprintf(stderr, "[FAIL] %s:%d: %s=%lld expected=%lld\n", \
        __FILE__, __LINE__, #actual, a_, e_); return 1; } \
} while (0)

struct regression_case {
    const char* name;       // Заимствованное имя для вывода и фильтра по подстроке, живёт до возврата regression_run.
    int (*run)(int param);  // Выполняется в дочернем процессе: 0 — успех, 77 — skip, прочие коды — ошибка.
    int param;             // Параметр конкретного случая, передаётся run без преобразования.
};

/* argc/argv — main: необязательный argv[1] фильтрует имена по подстроке; таблица cases заимствуется на вызов.
 * Возврат 0 — все выбранные прошли/skip, 1 — ошибка/timeout/signal, нет совпадений или неверные аргументы.
 * Вывод ребёнка наследует stdout/stderr; ресурсный контроль выполняется только для run=0. */
int regression_run(int argc, char** argv, const struct regression_case* cases, size_t count);
// CLOCK_MONOTONIC в мс, независимо от regression_clock_shift; ошибка часов завершает процесс через abort.
uint64_t regression_time_ms(void);
// WARN для всех категорий; наличие UTUN_TEST_DEBUG добавляет DEBUG для sys/socket/timers. Вызывает и сам runner в ребёнке.
void regression_log_init(void);

enum regression_alloc { FAULT_MALLOC = 1, FAULT_CALLOC = 2, FAULT_REALLOC = 4, FAULT_ALLOC = 7 };
/* Отказ u_malloc/u_calloc/u_realloc из mask: scope — подстрока allocation location либо NULL для всех мест.
 * nth=0 — все совпавшие выделения до reset, nth>0 — только N-е совпавшее. scope заимствуется на время подмены.
 * arm сбрасывает признак hit; отказ выдаёт NULL и [INJECT] с местом. Обычный malloc не перехватывается. */
void regression_fault_arm(int mask, const char* scope, int nth);
// Был хотя бы один отказ после последнего arm (0/1), не число отказов.
int regression_fault_hit(void);
// Только отключить allocation faults; hit, часы и прочие подмены не сбрасываются.
void regression_fault_reset(void);
// Заменить поправку gettimeofday в мс (0 отключает), не менять CLOCK_REALTIME/MONOTONIC или часы ОС.
void regression_clock_shift(int64_t milliseconds);
// Скопировать 0..64 события и один раз вернуть их следующим epoll_wait; нужен maxevents>=count. Новый вызов заменяет пакет.
void regression_epoll_events(const struct epoll_event* events, int count);
// Один отказ следующего epoll_ctl с данным EPOLL_CTL_* (ENOMEM); operation=0 снимает эту подмену.
void regression_epoll_ctl_fail(int operation);
// Один отказ следующего eventfd/epoll_create1 с errno=EMFILE.
void regression_eventfd_fail(void);
void regression_epoll_create_fail(void);

#endif
