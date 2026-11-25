/**
 * Reed–Solomon: кодирование и восстановление одной пачки по известным пропускам.
 * rs_init задаёт n кодограмм, r проверочных и максимальный размер каждой кодограммы.
 * Первые n-r кодограмм содержат исходные данные, остальные — проверочные.
 * Формат: seq (8 байт, big-endian), subblock (1 байт), полезные данные.
 * blocks[i] всегда соответствует subblock=i; при decode NULL обозначает пропуск.
 * Любые n-r присутствующих кодограмм восстанавливают всю исходную пачку.
 * Полученные данные и заголовки должны быть проверены транспортом на повреждения.
 * Все входные/выходные буферы принадлежат вызывающему коду и не перекрываются.
 * Кодек выделяет рабочую память только при init; один контекст используют последовательно.
 * encode принимает действительную длину, выбирает минимальный одинаковый размер частей
 * и дополняет хвост максимум n-r-1 нулями. Размер и исходную длину хранит вызывающий код.
 */
#ifndef REED_SOLOMON_H
#define REED_SOLOMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RS_HEADER_SIZE 9
#define RS_MAX_BLOCK_SIZE 4096
#define RS_MAX_BLOCKS 255

enum rs_result {
    RS_OK = 0,
    RS_ERR_INVALID_ARG = -1,
    RS_ERR_MEMORY = -2,
    RS_ERR_INSUFFICIENT = -3,
    RS_ERR_HEADER = -4,
    RS_ERR_MATRIX = -5
};

struct rs_codec;

/* Создать кодек: 1 <= n <= 255, 0 <= r < n, 10 <= max_block_size <= 4096.
 * ctx — выходной указатель; при ошибке *ctx=NULL. Старый контекст сначала освобождают.
 * Возвращает RS_OK или отрицательный rs_result.
 */
int rs_init(struct rs_codec **ctx, unsigned n, unsigned r, size_t max_block_size);

/* Освободить контекст и рабочую память; NULL допустим. */
void rs_free(struct rs_codec *ctx);

/* Максимальная длина входа encode: (n-r)*(max_block_size-RS_HEADER_SIZE).
 * Для NULL возвращает 0 и логирует ошибку.
 */
size_t rs_input_capacity(const struct rs_codec *ctx);

/* Читать 0 < length <= rs_input_capacity(ctx) байт input, записать n кодограмм.
 * blocks — n ненулевых указателей на буферы ёмкостью max_block_size каждый.
 * block_size получает RS_HEADER_SIZE+ceil(length/(n-r)); хвост за этим размером не меняется.
 * При ошибке аргументов выходные буферы и block_size не изменяются.
 */
int rs_encode(struct rs_codec *ctx, uint64_t seq, const uint8_t *input, size_t length,
              uint8_t *const blocks[], size_t *block_size);

/* Восстановить (n-r)*(block_size-RS_HEADER_SIZE) байт, включая нулевой хвост.
 * Ненулевые буферы содержат block_size байт; NULL обозначает пропуск. Размер общий для пачки.
 * output_capacity проверяется до записи; block_size не превышает максимум контекста.
 * Проверяет одинаковый seq и subblock=i у всех присутствующих кодограмм.
 * Входные буферы не изменяются. При ошибке output не изменяется.
 */
int rs_decode(struct rs_codec *ctx, const uint8_t *const blocks[], size_t block_size,
              uint8_t *output, size_t output_capacity);

#ifdef __cplusplus
}
#endif
#endif
