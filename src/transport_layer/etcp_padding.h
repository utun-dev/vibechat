/* etcp_padding — выбор длины случайного дополнения сетевых пакетов для instance или ETCP-соединения.
 * Обнулить ETCP_PADDING → start → size перед отправкой → stop до освобождения владельца.
 * Start выбирает диапазон длин и включает таймер его обновления; size учитывает свободное место пакета.
 * Все вызовы — в uasync-потоке владельца. Модуль выбирает длину, байты дополнения заполняет отправитель. */
#ifndef ETCP_PADDING_H
#define ETCP_PADDING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UASYNC;
struct UTUN_INSTANCE;

#define ETCP_PADDING_MAX 158

struct ETCP_PADDING {
    uint16_t min, max; // включительные границы длины дополнения, байты
    void* timer; // принадлежащий владельцу таймер обновления диапазона
    struct UASYNC* ua; // заимствованный цикл событий до stop
    const char* name; // заимствованное имя владельца для диагностики
    uint64_t next_refresh_tb; // следующий срок обновления в единицах 0.1 мс; 0 после stop
};

// ── Жизненный цикл диапазона ──

// Запустить обновление для обнулённого/остановленного состояния; name заимствован до stop. 0/-1.
int etcp_padding_start(struct ETCP_PADDING* padding, struct UASYNC* ua, const char* name);

// Отменить таймер и освободить ссылку на ua; NULL и повтор допустимы.
void etcp_padding_stop(struct ETCP_PADDING* padding);

// ── Выбор длины и диапазона владельца ──

/* Выбрать длину в байтах, не превышающую available; -1 при неверном диапазоне или нехватке места для min.
 * NULL допустим только для автономного STCP без instance: диапазон генерируется на операцию. */
int etcp_padding_size(const struct ETCP_PADDING* padding, size_t available);

// Заимствованный диапазон существующего незакрываемого ETCP_CONN, иначе instance; NULL при неверных аргументах.
const struct ETCP_PADDING* etcp_padding_for_peer(struct UTUN_INSTANCE* inst, const uint8_t* public_key);

#ifdef __cplusplus
}
#endif

#endif
