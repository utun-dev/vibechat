/* Источник непрозрачных байтов: одна подпись внутри FEC-пачки за ограниченный write,
 * неполная пачка сбрасывается таймером 100 мс, все блоки сначала сохраняются в кольце.
 * Один uasync-поток; источник сам владеет encoder/кольцом/таймером. NCD здесь ещё нет.
 * ready вызывается после помещения всей пачки и может читать кольцо; write/finish/destroy
 * из ready запрещены. destroy выполняется вне callbacks источника. */
#ifndef STREAM_SOURCE_H
#define STREAM_SOURCE_H

#include "stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

struct UASYNC;
struct stream_source;
typedef void (*stream_source_ready_fn)(struct stream_source *source, uint64_t seq, void *arg);

/* out заранее NULL; ua/arg заимствованы, descriptor/Ed25519 seed копируются; ready необязателен.
 * STREAM_OK либо отрицательная ошибка; при отказе out не публикуется. */
int stream_source_create(struct stream_source **out, struct UASYNC *ua, const struct stream_descriptor *descriptor,
                         const uint8_t private_key[SC_PRIVKEY_SIZE], stream_source_ready_fn ready, void *arg);
/* Отменить таймер и освободить источник; не выполняет finish и не публикует остаток. NULL допустим. */
void stream_source_destroy(struct stream_source *source);
/* Максимальная порция write/block, включая после finish; NULL даёт 0 с диагностикой. */
size_t stream_source_input_capacity(const struct stream_source *source);
/* Только заимствованное чтение/decode; вставлять блоки/освобождать это кольцо должен сам источник. */
struct stream_buffer *stream_source_buffer(struct stream_source *source);

/* length <= input_capacity: вызов кодирует не более одной пачки, не захватывая uasync большим внешним буфером.
 * accepted обязателен и обнуляется до проверок, затем получает принятую часть даже при ошибке.
 * Нулевой write допускает data=NULL и не выбирает режим, но требует живого незавершённого источника.
 * Ошибка публикации/таймера делает источник failed; его уничтожают, не повторяют принятую часть как новую трансляцию. */
int stream_source_write(struct stream_source *source, const uint8_t *data, size_t length, size_t *accepted);
/* Один непустой блок = одна немедленная FEC-пачка с минимальным размером частей, length <= input_capacity.
 * Режим выбирается первой непустой записью; побайтовый и блочный режимы не смешиваются. */
int stream_source_write_block(struct stream_source *source, const uint8_t *data, size_t length);
/* Сбросить остаток и остановить приём; повторный finish допустим, кольцо доступно до destroy.
 * Сам не создаёт подписанный сетевой END. STREAM_OK либо отрицательная ошибка; failed не восстанавливается. */
int stream_source_finish(struct stream_source *source);

#ifdef __cplusplus
}
#endif
#endif
