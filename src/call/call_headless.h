// call_headless.h — headless-аудио-сокет для P2P-звонков (PCM, без тактирования)
//
// Отдельный TCP-сокет, через который headless-клиент (CLI/скрипт) обменивается
// ГОТОВЫМ PCM с ядром звонка. Ядро кодирует PCM→Opus (TX) и декодирует Opus→PCM
// (RX); тактирование и джиттер — на стороне клиента. Сигналинг идёт по
// control-сокету (chat_headless_control), аудио — по этому сокету.
//
// Бинарный фрейм (обе стороны):
//   [type:1][call_id:8][len:2][data:len]
//   type = 0x01 HELLO (клиент → ядро: привязать соединение к call_id, data пусто)
//   type = 0x02 FRAME (PCM int16 mono 48k, 960 сэмплов = 1920 байт)
//          клиент → ядро = PCM захвата (кодируется и шлётся пиру);
//          ядро → клиент = PCM воспроизведения (декодированный Opus пира)
// call_id хранится в порядке байтов хоста, len — старший байт первым; PCM — native int16_t.
// Это отдельный сервис с кодеками instance, без DSP/jitter движка call_audio.h.
// HELLO выбирает call_id для приёма; передаваемый FRAME содержит call_id своего звонка.
// До16 TCP-клиентов; если несколько выбрали один звонок, RX получает первый в списке (последний подключённый).
// Encoder/decoder создаются лениво и заменяются при смене call_id. Отправка требует ACTIVE в call.c.
// Все вызовы и callbacks выполняются в потоке ua; клиент обеспечивает темп и воспроизведение.
// Исходящая очередь принимает header+PCM целиком; при переполнении отбрасывается целый кадр с WARN/счётчиком.
// Частичная отправка продолжается перед следующим кадром. Буферы ввода/вывода по64КиБ на клиента;
// parser ждёт полный фрейм, неизвестный type/len>2048 закрывает соединение, неверный размер PCM отбрасывается.
// Закрытые клиенты освобождаются таймером каждые500мс; до очистки занимают слот лимита16.
#ifndef CALL_HEADLESS_H
#define CALL_HEADLESS_H

#include <stdint.h>

struct UASYNC;
struct UTUN_INSTANCE;

#define CALL_AUDIO_HELLO     0x01
#define CALL_AUDIO_FRAME     0x02
#define CALL_AUDIO_HDR_SIZE  11   /* type(1) + call_id(8) + len(2) */
#define CALL_AUDIO_PCM_FRAME_BYTES (960 * (int)sizeof(int16_t))  /* 1920 — PCM int16 mono 48k 20мс */

#ifdef __cplusplus
extern "C" {
#endif

/* Запустить IPv4/TCP listener bind_ip:port и зарегистрировать приём Opus в call.c.
 * ua — тот же loop, что inst->ua; inst должен иметь call_init. port=0 выбирает ОС.
 * Один работающий listener на inst; повтор init не меняет адрес. 0 = успех, -1 = ошибка.
 * При ошибке вызывающий выполняет destroy, включая частично созданный контекст. */
int  call_headless_init(struct UASYNC* ua, struct UTUN_INSTANCE* inst,
                        const char* bind_ip, int port);
/* Снять callback, отменить таймер и освободить listener/клиентов/кодеки до call_destroy и destroy ua. NULL допустим. */
void call_headless_destroy(struct UTUN_INSTANCE* inst);
// port=0 при init выбирает ОС; 0 для остановленной службы или при ошибке.
uint16_t call_headless_get_port(struct UTUN_INSTANCE* inst);

#ifdef __cplusplus
}
#endif
#endif /* CALL_HEADLESS_H */
