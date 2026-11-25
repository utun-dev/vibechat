/* Одна Ed25519-подпись пачки до RS, части переменного размера без отдельных подписей.
 * Контексты одного uasync-потока; descriptor и ключ источника уже проверены членством группы.
 * encode возвращает заимствованные блоки до следующего encode/destroy; ошибочные результаты не публикуют.
 * parse проверяет только формат части; подпись источника проверяется после RS в batch_verify.
 * Ключи здесь Ed25519 (32-байтный seed/public key); ID источника с ключом codec сам не связывает. */
#ifndef STREAM_BLOCK_H
#define STREAM_BLOCK_H

#include "stream_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

struct stream_encoder;

struct stream_batch_info {
    uint64_t seq;          /* Индекс пачки, UINT64_MAX запрещён. */
    uint32_t data_length;  /* Длина содержимого без batch-header/подписи/padding. */
    uint16_t timestamp_ms; /* Монотонные часы источника по модулю 65536 мс. */
    uint16_t block_size; /* Фактический размер RS-кодограммы одной пачки. */
    uint8_t flags;         /* Только STREAM_BLOCK_TIME_RESET. */
};

struct stream_block_view {
    struct stream_batch_info batch; /* Неподписанные подсказки, сверяемые после восстановления. */
    uint8_t subblock;               /* Номер RS-части 0..n-1. */
    const uint8_t *rs; /* Заимствованный диапазон batch.block_size байт. */
};

/* Проверить ненулевой ключ, n/r, RS-size 10..4096 и место хотя бы для байта содержимого.
 * STREAM_OK/STREAM_ERR_ARGUMENT; descriptor не меняется. */
int stream_descriptor_validate(const struct stream_descriptor *descriptor);
/* Максимальная полная часть: descriptor.block_size + 47; 0 при неверном descriptor. */
size_t stream_block_capacity(const struct stream_descriptor *descriptor);
/* Максимальная длина содержимого без подписанных метаданных/подписи; 0 при неверном descriptor. */
size_t stream_input_size(const struct stream_descriptor *descriptor);

/* out заранее NULL; descriptor/seed копируются, seed очищается при destroy.
 * STREAM_OK либо отрицательная ошибка, при отказе *out остаётся NULL. */
int stream_encoder_create(struct stream_encoder **out, const struct stream_descriptor *descriptor,
                          const uint8_t private_key[SC_PRIVKEY_SIZE]);
/* Освободить encoder и рабочие буферы; NULL допустим. */
void stream_encoder_destroy(struct stream_encoder *encoder);
/* Заимствованный публичный ключ до destroy, полученный из seed этого encoder; NULL для NULL encoder. */
const uint8_t *stream_encoder_public_key(const struct stream_encoder *encoder);

/* blocks — массив descriptor.n указателей; input не перекрывается с рабочими буферами encoder.
 * 0 < length <= stream_input_size; подпись входит в RS, дополнение не превышает n-r-1 байт.
 * block_length получает общий фактический размер частей. seq != UINT64_MAX.
 * При ошибке blocks/block_length не изменяются; собственная рабочая память может измениться. */
int stream_encoder_encode(struct stream_encoder *encoder, uint64_t seq, uint16_t timestamp_ms, uint8_t flags,
                          const uint8_t *input, size_t length, const uint8_t **blocks, size_t *block_length);

/* Формат, дескриптор и канонический размер; криптографической проверки части нет. view не меняется при отказе. */
int stream_block_parse(const struct stream_descriptor *descriptor, const uint8_t *wire, size_t length, struct stream_block_view *view);
/* После RS сверить подписанные поля с ожидаемой пачкой, нулевой хвост и одну подпись источника.
 * decoded_length — полный восстановленный размер с padding; содержимое начинается с STREAM_BATCH_HEADER.
 * STREAM_OK либо ERR_ARGUMENT/FORMAT/SIGNATURE. Входы не меняются; decode RS выполняет caller. */
int stream_batch_verify(const struct stream_descriptor *descriptor, const uint8_t public_key[SC_PUBKEY_SIZE],
                        const struct stream_batch_info *expected, const uint8_t *decoded, size_t decoded_length);

#ifdef __cplusplus
}
#endif
#endif
