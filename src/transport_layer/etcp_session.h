/* etcp_session — подтверждение эпох одного ETCP_CONN, чтобы старые DATA не попадали в новый поток.
 * HELLO/CHALLENGE/CONFIRM отправляются по инициализированным линкам, вне надёжного потока/normalizer.
 * Обработчик вызывается после проверки защиты пакета; принадлежность сессии проверяет accept_data.
 * Состояние и retry-таймер принадлежат ETCP_CONN. Все вызовы — его uasync-поток; перед teardown нужен cancel. */
#ifndef ETCP_SESSION_H
#define ETCP_SESSION_H
#include <stddef.h>
#include <stdint.h>
struct ETCP_CONN;

#define ETCP_SESSION_DATA 0x09
#define ETCP_SESSION_HELLO 0x0a
#define ETCP_SESSION_CHALLENGE 0x0b
#define ETCP_SESSION_CONFIRM 0x0c
#define ETCP_SESSION_HEADER_SIZE 17
#define ETCP_SESSION_CONTROL_SIZE 25

/* Запустить обязательное подтверждение: повтор через 500мс, общий срок 10с от start.
 * OOM control-пакета оставляет повтор активным; OOM таймера или истечение срока закрывает соединение. */
void etcp_session_start(struct ETCP_CONN* conn);
/* Предложить ненулевую эпоху пира для challenge; это ещё не разрешает приём DATA новой эпохи. */
void etcp_session_observe(struct ETCP_CONN* conn, uint64_t peer_epoch);
/* Отменить retry и сбросить candidate/cookie; подтверждённые reset_id/peer_reset_id сохраняются. */
void etcp_session_cancel(struct ETCP_CONN* conn);
/* 1 — control распознан (в том числе отброшен как ошибочный), 0 — не control. Буфер остаётся у вызывающего. */
int etcp_session_receive(struct ETCP_CONN* conn, const uint8_t* data, size_t len);
/* Записать 17-байтный заголовок code/sender/target; epochs — big-endian, cookie/body добавляет вызывающий. */
void etcp_session_encode(uint8_t* data, uint8_t code, uint64_t sender, uint64_t target);
/* 1 — DATA принадлежит готовой текущей сессии, 0 — ошибочная/старая. Заголовок не снимает, поток не обрабатывает. */
int etcp_session_accept_data(struct ETCP_CONN* conn, const uint8_t* data, size_t len);
#endif
