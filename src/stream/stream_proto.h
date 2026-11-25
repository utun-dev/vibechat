/* Общие форматы и результаты stream; числовые wire-поля — big-endian, без native-структур.
 * Части RS имеют непроверенный внешний заголовок; подписанная пачка находится внутри RS.
 * Данные ETCP-выдачи (handle/сессия/путь) добавляются снаружи и в подпись не входят.
 * seq/subblock берутся из 9-байтного заголовка RS; seq повторяется в подписанной пачке и сверяется после decode.
 * stream_part1 и stream_batch занимают по 12 байтов без NUL. Внешние поля до RS — 95 байт,
 * подписанный batch-header — 103, затем содержимое, Ed25519(64) и нулевое RS-дополнение.
 * block_size в descriptor — максимум только RS; полный размер части дополнительно включает 95 байт. */
#ifndef STREAM_PROTO_H
#define STREAM_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../lib/reed_solomon.h"
#include "../transport_layer/secure_channel.h"

#define STREAM_BLOCK_DOMAIN "stream_part2"
#define STREAM_BLOCK_DOMAIN_SIZE 12
#define STREAM_BLOCK_GROUP_OFF 12
#define STREAM_BLOCK_SOURCE_OFF 44
#define STREAM_BLOCK_EPOCH_OFF 76
#define STREAM_BLOCK_N_OFF 84
#define STREAM_BLOCK_R_OFF 85
#define STREAM_BLOCK_SIZE_OFF 86
#define STREAM_BLOCK_TIMESTAMP_OFF 88
#define STREAM_BLOCK_LENGTH_OFF 90
#define STREAM_BLOCK_FLAGS_OFF 94
#define STREAM_BLOCK_RS_OFF 95
#define STREAM_BLOCK_SEQ_OFF STREAM_BLOCK_RS_OFF
#define STREAM_BLOCK_SUB_OFF (STREAM_BLOCK_RS_OFF + 8)
#define STREAM_BLOCK_OVERHEAD STREAM_BLOCK_RS_OFF
#define STREAM_BATCH_DOMAIN "stream_batch"
#define STREAM_BATCH_SEQ_OFF STREAM_BLOCK_RS_OFF
#define STREAM_BATCH_HEADER (STREAM_BATCH_SEQ_OFF + 8)
#define STREAM_BATCH_OVERHEAD (STREAM_BATCH_HEADER + SC_SIGN_SIZE)
#define STREAM_BLOCK_DEFAULT_SIZE 1300
#define STREAM_RS_DEFAULT_SIZE (STREAM_BLOCK_DEFAULT_SIZE - STREAM_BLOCK_OVERHEAD)
#define STREAM_DEFAULT_N 8
#define STREAM_DEFAULT_R 0
#define STREAM_DEFAULT_INPUT_CAPACITY \
    ((STREAM_DEFAULT_N - STREAM_DEFAULT_R) * (STREAM_RS_DEFAULT_SIZE - RS_HEADER_SIZE) - STREAM_BATCH_OVERHEAD)
#define STREAM_BLOCK_MAX_SIZE (RS_MAX_BLOCK_SIZE + STREAM_BLOCK_OVERHEAD)
#define STREAM_BLOCK_TIME_RESET 0x01
#define STREAM_BUFFER_BYTES (32u * 1024u * 1024u)
#define STREAM_SOURCE_FLUSH_TB 1000 /* 100 мс, timebase 0.1 мс */
#define STREAM_LATE_MS 500
#define STREAM_TIMESTAMP_PERIOD_MS 65536u

struct stream_address {
    uint8_t ed[SC_PUBKEY_SIZE]; /* Ed25519; нули означают отсутствие адреса. */
};

static inline struct stream_address stream_address_read(const uint8_t ed[SC_PUBKEY_SIZE]) {
    struct stream_address address;
    memcpy(address.ed, ed, sizeof(address.ed));
    return address;
}

static inline int stream_address_cmp(struct stream_address a, struct stream_address b) {
    return memcmp(a.ed, b.ed, sizeof(a.ed));
}

static inline int stream_address_equal(struct stream_address a, struct stream_address b) {
    return stream_address_cmp(a, b) == 0;
}

static inline int stream_address_set(struct stream_address address) {
    const struct stream_address empty = {{0}};
    return !stream_address_equal(address, empty);
}

#ifndef __cplusplus
#define STREAM_ADDRESS_TEXT(address) sc_public_key_hex((address).ed, (char[65]){0})
#endif

struct stream_descriptor {
    uint64_t group_id;       /* Локальный индекс CHAT-группы. */
    uint8_t group_key[SC_PUBKEY_SIZE]; /* Ed25519 группы в кодограммах и подписях. */
    struct stream_address source; /* Ed25519 источника, проверяемый по членству. */
    uint64_t source_epoch;   /* Ненулевой случайный ID запуска; параметры неизменны внутри эпохи. */
    uint16_t block_size; /* Максимальный размер RS-кодограммы; фактический размер задан в каждой пачке. */
    uint8_t n;              /* Всего частей 1..255; первые n-r систематические. */
    uint8_t r;              /* Проверочных частей 0..n-1; нужны любые n-r различных частей. */
};

enum stream_result {
    STREAM_OK = 0,
    STREAM_DUPLICATE = 1,
    STREAM_WAIT = 2,
    STREAM_GAP = 3,
    STREAM_EXPIRED = 4,
    STREAM_REJECTED = 5, /* Пачка восстановлена, но подпись/подписанные метаданные не прошли проверку. */
    STREAM_MORE = 6, /* Бюджет работы исчерпан; можно продолжить отложенным callback без новых данных. */
    STREAM_LOCAL_BUSY = 7, /* Местный ресурс занят; повтор после освобождения, без штрафа удалённому узлу. */
    STREAM_ERR_ARGUMENT = -1,
    STREAM_ERR_MEMORY = -2,
    STREAM_ERR_FORMAT = -3,
    STREAM_ERR_SIGNATURE = -4,
    STREAM_ERR_CONFLICT = -5,
    STREAM_ERR_STATE = -6,
    STREAM_ERR_CRYPTO = -7,
    STREAM_ERR_FEC = -8,
    STREAM_ERR_NOT_MEMBER = -9,
    STREAM_ERR_CAPACITY = -10 /* k=n-r превышает общий предел постоянных сетевых слотов; это не временная занятость. */
};

/* Читать bytes (1..8) доступных байтов BE; проверки указателя/длины нет, выравнивание не требуется. */
static inline uint64_t stream_read_be(const uint8_t *data, unsigned bytes) {
    uint64_t result = 0;
    for (unsigned i = 0; i < bytes; i++) result = (result << 8) | data[i];
    return result;
}

/* Записать младшие bytes (1..8) байтов value в доступный диапазон data; границы проверяет caller. */
static inline void stream_write_be(uint8_t *data, uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++) data[bytes - i - 1] = (uint8_t)(value >> (8 * i));
}

#endif
