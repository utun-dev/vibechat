/* Часы сетевых тестов с реальными сокетами: init → test_clock_poll/run_for/mainloop → destroy ресурсов → finish.
 * Linux/ELF: нужны test_clock_sources и test_clock_ldflags из tests/Makefile.am; при пустом I/O продвигается offset часов.
 * Наличие UTUN_TEST_REAL_TIME с любым значением выключает ускорение; вне Linux функции используют реальные ожидания.
 * TB = 0.1мс. Управление только из потока теста, вне callbacks; вложенные интервалы/ускоренные poll запрещены.
 * Offset действует на весь бинарник, включая чтение часов другими потоками; отдельные uasync он не изолирует. */
#ifndef TEST_CLOCK_H
#define TEST_CLOCK_H

#include "../lib/u_async.h"

/* Перед первым uasync: max_step_tb>0 ограничивает один искусственный шаг; неверный аргумент завершает тест через abort.
 * Менять шаг можно между poll. Повторный init сохраняет offset и счётчики. Категория debug включается на INFO. */
void test_clock_init(int max_step_tb);
// В Linux включить тот же offset для CLOCK_REALTIME/gettimeofday (TTL/retry_at); до создания инстансов, обратно не выключается.
void test_clock_sync_wall_time(void);
/* Обычный uasync_poll с ускорением только при timeout_tb>0. Шаг ограничен max_step, ближайшим timer и текущим run_for.
 * Сам timeout_tb не ограничивает размер искусственного шага; прямой uasync_poll не включает ускорение ожидания. */
void test_clock_poll(struct UASYNC* ua, int timeout_tb);
/* Один poll до ближайшего timer или заданного монотонного дедлайна в мкс. Дедлайн должен быть ненулевым.
 * При ускорении переопределяет max_step на один вызов. В реальном режиме действует timeout_tb. */
void test_clock_poll_until(struct UASYNC* ua, int timeout_tb, uint64_t deadline_us);
/* Обычный test_clock_poll с неблокирующей проверкой сокетов в ускоренном режиме. Использовать, когда тесту
 * не нужно дать время на I/O в другом event loop/потоке. При реальном режиме действует timeout_tb. */
void test_clock_poll_local_io(struct UASYNC* ua, int timeout_tb);
// Poll по 100 TB до ua->stop; ua обязателен. Не меняет флаг stop и не уничтожает event loop.
void test_clock_mainloop(struct UASYNC* ua);
// Обслуживать I/O/callbacks до монотонного дедлайна duration_tb>=0; 0 не выполняет poll, вложенный run_for запрещён.
void test_clock_run_for(struct UASYNC* ua, int duration_tb);
/* Как run_for, но в ускоренном режиме проверяет сокеты без реального ожидания.
 * Для интервалов, в которых необходимые сетевые события создаются в том же event loop. */
void test_clock_run_for_local_io(struct UASYNC* ua, int duration_tb);
/* Дойти до дедлайна ближайшего неотменённого timer и выполнить poll(0), обслуживая также другие события.
 * ua и хотя бы один живой timer обязательны; пустая heap/переполнение интервала завершают тест через abort. */
void test_clock_next_timer(struct UASYNC* ua);
// Та же граница timer, с неблокирующей проверкой сокетов в ускоренном режиме.
void test_clock_next_timer_local_io(struct UASYNC* ua);
// Только вывести статистику; не сбрасывает offset/режим/счётчики. Вызывать после уничтожения ресурсов стенда.
void test_clock_finish(void);

#endif
