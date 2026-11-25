// radio_proto.h — wire-формат и константы рации (PTT walkie-talkie мультикаст).
//
// Транспорт: raw ETCP (etcp_bind/etcp_send), первый байт кодограммы = ETCP_ID_RADIO.
// Пакет флудится по senders_list CHAT-группы с hop-list против циклов и
// RPF-обрезанием ветвей без подписчиков (узел с radio==1 в NODEINFO).
//
// Формат payload (после dgram[0] = ETCP_ID_RADIO):
//   [group_key:32][src_ed25519:32][stream_id:2][seq:2][timestamp:8][flags:1][hop_count:1][count:1]
//   [hop_list:32*hop_count] затем count раз [length:2][opus:length], затем [signature:64].
//   Ed25519 автора подписывает ETCP_ID_RADIO, group, src, stream, seq, timestamp, flags, count и все length/opus.
//   hop_count/hop_list не подписываются: ретранслятор дописывает путь и сохраняет подпись автора.
//   Подписчик проверяет подпись по действующим MEMBERS своей группы до выдачи кадров/FIN.
//   timestamp — микросекунды часов отправителя, строго растут между пачками и передачами, даже при коррекции NTP назад.
//   Получатель хранит последний timestamp отдельно для каждого автора в группе и принимает только больший.
//   Часы получателя не сравниваются; повтор и опоздавшая пачка отбрасываются. Состояние живёт до закрытия группы.
//   Чистый транзит не проверяет прикладные права автора. Числа в порядке байтов хоста.
//   DATA: flags=0, count=1..10; FIN: flags=1, count=0, без тела, после отправки хвоста DATA.
//   stream_id — идентификатор передачи, seq — номер первого кадра пачки; каждый кадр увеличивает seq.
//   FIN использует следующий seq. Пересылка сохраняет timestamp; транзитный кэш дублей использует (src,timestamp).
#ifndef RADIO_PROTO_H
#define RADIO_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_FLAG_FIN 0x01

#define RADIO_KEY_SIZE  32
#define RADIO_MAX_HOPS  16
#define RADIO_MAX_OPUS  512   /* макс. Opus-кадр (стерео 128кбит/с 20мс = 320 байт + запас) */
#define RADIO_MAX_BURST 10
#define RADIO_MAX_BODY (RADIO_MAX_BURST * (2 + RADIO_MAX_OPUS))
#define RADIO_SIGNATURE_SIZE 64
#define RADIO_MAX_PACKET_SIZE (1 + RADIO_HDR_SIZE + RADIO_MAX_HOPS * RADIO_KEY_SIZE + RADIO_MAX_BODY + RADIO_SIGNATURE_SIZE)

#define RADIO_HDR_SIZE  79   /* group(32)+src(32)+stream(2)+seq(2)+timestamp(8)+flags(1)+hop_count(1)+count(1) */

#define RADIO_OFF_GROUP   0
#define RADIO_OFF_SRC     32
#define RADIO_OFF_STREAM  64
#define RADIO_OFF_SEQ     66
#define RADIO_OFF_TIMESTAMP 68
#define RADIO_OFF_FLAGS   76
#define RADIO_OFF_HOPS    77
#define RADIO_OFF_COUNT   78
#define RADIO_OFF_HOPLIST 79

#ifdef __cplusplus
}
#endif
#endif /* RADIO_PROTO_H */
