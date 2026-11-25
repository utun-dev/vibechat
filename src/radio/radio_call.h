/* Адресный вызов участника в общую рацию CHAT-канала.
 * Ядро владеет сигналингом, GUI — рингтоном и включением обычной рации.
 * После INCOMING приложение вызывает READY; первый ответ ожидается строго 2с.
 * WAIT_DECISION не запускает UI получателя; CONTINUE разрешает начало входящего.
 * Уже слушающее устройство тоже получает INCOMING и рингтон.
 * ANSWER сообщает решение пользователя; SELECTED разрешает включение рации.
 * ACCEPT отправляется после локального включения рации. FINISHED завершает только
 * приглашение, а не рацию. Все функции и события выполняются в uasync-потоке. */
#ifndef RADIO_CALL_H
#define RADIO_CALL_H

#include <stdint.h>

struct UTUN_INSTANCE;

#ifdef __cplusplus
extern "C" {
#endif

enum radio_call_state {
    RADIO_CALL_OUTGOING = 1, RADIO_CALL_INCOMING, RADIO_CALL_RINGING, RADIO_CALL_FINISHED, RADIO_CALL_WAIT_DECISION,
    RADIO_CALL_ANSWERING, RADIO_CALL_SELECTED
};
enum radio_call_reason {
    RADIO_CALL_NONE, RADIO_CALL_ACCEPTED, RADIO_CALL_DECLINED, RADIO_CALL_BUSY,
    RADIO_CALL_CANCELLED, RADIO_CALL_UNAVAILABLE, RADIO_CALL_NO_ANSWER, RADIO_CALL_AUDIO_ERROR, RADIO_CALL_PHONE_BUSY,
    RADIO_CALL_POLICY
};
enum radio_call_action {
    RADIO_CALL_START = 1, RADIO_CALL_READY, RADIO_CALL_ACCEPT, RADIO_CALL_DECLINE,
    RADIO_CALL_CANCEL, RADIO_CALL_FAIL, RADIO_CALL_REJECT_BUSY, RADIO_CALL_CONTINUE, RADIO_CALL_ANSWER
};

/* Таймауты в timebase 0.1мс; повторы не сдвигают первоначальный deadline. */
#define RADIO_CALL_RESPONSE_TB 20000
#define RADIO_CALL_RING_TB 600000
#define RADIO_CALL_RETRY_TB 3000
#define RADIO_CALL_CACHE_TB 700000

/* init: 0/-1; destroy отменяет таймер и удаляет контекст до остановки групп. */
int radio_call_init(struct UTUN_INSTANCE* inst);
void radio_call_destroy(struct UTUN_INSTANCE* inst);
/* После commit состава: завершить отозванные ветки приглашений и снять их повторы; group=0 — все группы. */
void radio_call_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group);
/* Есть незавершённое приглашение, независимо от роли. */
int radio_call_busy(struct UTUN_INSTANCE* inst);

/* START: ненулевые id/group и peer (Ed25519); остальные действия используют только id.
 * READY подтверждает показ входящего вызова; ACCEPT — готовность местной рации.
 * Если её start ещё ждёт READ настроек, ACCEPT ждёт результата до2с без блокировки uasync;
 * off/ошибка READ/истечение ожидания дают AUDIO_ERROR. Повтор ACCEPT не продлевает deadline.
 * Ошибки и результаты приходят через CHAT_EVT_RADIO_CALL, по одному FINISHED на попытку. */
struct radio_call_request {
    struct UTUN_INSTANCE* inst; /* заимствован, запрос не должен пережить остановку ядра */
    uint64_t id;              /* идентификатор попытки */
    uint8_t group[32];        /* Ed25519 CHAT-группы */
    uint8_t peer[32];         /* Ed25519 удалённого участника; используется только в START */
    int action;               /* radio_call_action */
    int devices;              /* START: вызвать devgroup peer, иначе только peer. */
};
void radio_call_execute(const struct radio_call_request* req);
/* Выполняет запрос и освобождает выделенный через u_malloc аргумент. */
void radio_call_trampoline(void* arg);

#ifdef __cplusplus
}
#endif
#endif
