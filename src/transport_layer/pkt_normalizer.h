/* pkt_normalizer — упаковка кодограмм сервисов в надёжный байтовый поток ETCP и обратная сборка.
 * input принимает целые кодограммы, packer добавляет длину и делит поток под MTU;
 * unpacker читает etcp->output_queue и выдаёт целые кодограммы в output/etcp_int_recv.
 * Содержимое cmd сервисов непрозрачно для normalizer. Контекст принадлежит ETCP_CONN,
 * владеет своими очередями/частичными буферами/ожиданием backpressure; все вызовы — uasync.
 * OOM после принятия данных аварийно сбрасывает всю ETCP-сессию, сохраняя физические линки.
 * Старые данные не повторяются: владельцы после REINIT восстанавливают операции своего протокола.
 * RX до сохранения в ETCP.recv_q может быть отброшен без ACK для обычного ретрансмита.
 * Обработчики прекращают старый разбор после reset/close из callback; дополнительные OOM-очереди/таймеры не используются. */
#ifndef PKT_NORMALIZER_H
#define PKT_NORMALIZER_H

#ifdef __cplusplus
extern "C" {
#endif


#include "../lib/ll_queue.h"
#include "../lib/u_async.h"
#include <stdint.h>

/* Формат входной кодограммы: [cmd:1][данные сервиса...]; максимальная длина — PKTNORM_MAX_DGRAM_SIZE. */

/* Длина кодограммы вместе с cmd, без двухбайтового little-endian префикса. */
#define PKTNORM_MAX_DGRAM_SIZE (16u * 1024u)

// Структура для packer
struct PKTNORM {
    // public:
    struct ll_queue* input;   // Входная очередь в packer (через нее отправляем пакеты)
    struct ll_queue* output;  // Выходная очередь из unpacker (через нее принимаем пакеты)
    uint16_t tx_wait_time;

    // private:
    uasync_t* ua;             // uasync instance
    struct ETCP_CONN* etcp;

    // packer:
    uint16_t frag_size;       // размер фрагмента на которые разбивать
    uint8_t* data;            // буфер (выделяется из пула pn->etcp->instance->data_pool)
    uint16_t data_ptr;        // число заполненных байт
    uint16_t data_size;       // размер выделенной области
//    struct ll_entry sndpart;  // блок ожидающий досборки
    void* flush_timer;        // For timeout flush
    struct ll_entry* pending; // Partial processed input entry
    uint16_t pending_in_ptr;  // Pointer in pending entry

    // backpressure:
    struct queue_waiter_handle input_waiter_handle; // waiter на etcp->input_queue

    // unpacker:
    struct ll_entry* recvpart; // блок ожидающий заполнение
//    uint16_t recvpart_rem;     // сколько байт осталось собрать (0 = ждём заголовок нового пакета)

    // stats:
    uint32_t alloc_errors;
    uint32_t logic_errors;
    uint64_t in_total_pkts;
    uint64_t in_total_bytes;
    uint64_t out_total_pkts;
    uint64_t out_total_bytes;
};

// Инициализация пары
struct PKTNORM* pn_init(struct ETCP_CONN* etcp);// все что нужно (в т.ч. mtu и ua) берет из etcp

// Деинициализация пары
void pn_deinit(struct PKTNORM* pn);

// Отбросить обе очереди/частичные буферы, отменить flush и waiter без выделений памяти. NULL допустим.
// Вызывается владельцем при сбросе ETCP-потока; не запускает согласование эпох и не воспроизводит данные.
void pn_reset(struct PKTNORM* pn);

// Сброс состояния unpacker (для unpacker)
void pn_unpacker_reset_state(struct PKTNORM* pn);

// создаёт malloc data, копирует, помещает в input.
//void pn_packer_send(struct PKTNORM* pn, uint8_t* data, uint16_t len);


#ifdef __cplusplus
}
#endif
#endif // PKT_NORMALIZER_H
