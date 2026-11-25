// radio_headless.h — headless-аудио-сокет рации (PCM, без тактирования).
//
// Отдельный TCP-сокет, через который headless-клиент (CLI/скрипт) обменивается
// готовым PCM с ядром рации. Ядро кодирует PCM→Opus (TX), принимает Opus от пиров,
// микширует их в общий PCM (RX, простой микшер без time-stretch — тактирование на
// стороне клиента, как в call_headless). Управление рацией/PTT — через фреймы
// PTT_START/PTT_STOP и команды radio_on/radio_off на control-сокете.
//
// Бинарный фрейм (обе стороны):
//   [type:1][group_ed25519:32][len:2 BE][data:len]
//   type = 0x01 HELLO     (клиент → ядро: привязаться к ключу группы, data пусто)
//   type = 0x02 PTT_START (клиент → ядро: radio_talk_begin, data пусто)
//   type = 0x03 PTT_STOP  (клиент → ядро: radio_talk_end, data пусто)
//   type = 0x04 FRAME     (PCM int16 mono 48k, 960 сэмплов = 1920 байт)
//          клиент → ядро = PCM захвата (кодируется и флудится подписчикам);
//          ядро → клиент = микшированный PCM всех говорящих
#ifndef RADIO_HEADLESS_H
#define RADIO_HEADLESS_H

#include <stdint.h>

struct UASYNC;
struct UTUN_INSTANCE;

#define RADIO_AUDIO_HELLO      0x01
#define RADIO_AUDIO_PTT_START  0x02
#define RADIO_AUDIO_PTT_STOP   0x03
#define RADIO_AUDIO_FRAME      0x04
#define RADIO_AUDIO_LEN_OFF    33   /* type(1) + group_ed25519(32) */
#define RADIO_AUDIO_HDR_SIZE   (RADIO_AUDIO_LEN_OFF + 2)
#define RADIO_AUDIO_PCM_FRAME_BYTES (960 * (int)sizeof(int16_t))  /* 1920 — PCM int16 mono 48k 20мс */

#ifdef __cplusplus
extern "C" {
#endif

int  radio_headless_init(struct UASYNC* ua, struct UTUN_INSTANCE* inst,
                         const char* bind_ip, int port);
void radio_headless_destroy(struct UTUN_INSTANCE* inst);
// port=0 при init выбирает ОС; 0 для остановленной службы или при ошибке.
uint16_t radio_headless_get_port(struct UTUN_INSTANCE* inst);

#ifdef __cplusplus
}
#endif
#endif /* RADIO_HEADLESS_H */
