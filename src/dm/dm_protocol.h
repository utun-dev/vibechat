/* dm_protocol — единый разбор тела DEVICE/GROUPS и сертификатов адресатов.
 * DEVICE допускает пустой content type. GROUPS
 * ставит старший бит data_len и помещает контекст перед ciphertext.
 * Тело подписывается целиком, включая снимок; parser не проверяет подпись автора.
 * Указатели view заимствуют входной буфер. Encode_read работает в SQL worker по копии identity;
 * instance-вариант предназначен для синхронного RAM/legacy-вызова в uasync. */
#ifndef UTUN_DM_PROTOCOL_H
#define UTUN_DM_PROTOCOL_H

#include "dm_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DM_MSG_AUTHOR_OFF 24
#define DM_MSG_CT_LEN_OFF (DM_MSG_AUTHOR_OFF + 32)
#define DM_MSG_FIXED_HDR (DM_MSG_CT_LEN_OFF + 1)
#define DM_TARGET_PROOF_SIZE 97 /* side:u8,ed:32,member_sig:64; X25519 выводится из Ed25519. */
#define DM_TARGET_ED_OFF 1
#define DM_TARGET_SIG_OFF 33
#define DM_CONTEXT_VERSION 2
/* Общий префикс подтверждений текста и файла. После адресов идут digest либо UUID и digest. */
#define DM_FACT_AUTHOR_OFF 24
#define DM_FACT_RECIPIENT_OFF (DM_FACT_AUTHOR_OFF + 32)
#define DM_FACT_DATA_OFF (DM_FACT_RECIPIENT_OFF + 32)
#define DM_ACK_HASH_OFF DM_FACT_DATA_OFF
#define DM_ACK_SIG_OFF (DM_ACK_HASH_OFF + 32)
#define DM_RECEIPT_SIZE (DM_ACK_SIG_OFF + 64)
#define DM_FILE_FACT_ID_OFF DM_FACT_DATA_OFF
#define DM_FILE_FACT_HASH_OFF (DM_FILE_FACT_ID_OFF + 16)
#define DM_FILE_FACT_SIG_OFF (DM_FILE_FACT_HASH_OFF + 32)
#define DM_MEDIA_RECEIPT_SIZE (DM_FILE_FACT_SIG_OFF + 64)
#define DM_ACK_DEVICE "DMACK002"
#define DM_ACK_GROUPS "DMACKG02"
#define DM_FILE_RECEIVED_DEVICE "DMFILE02"
#define DM_FILE_RECEIVED_GROUPS "DMFILEG2"
#define DM_FILE_TAKEN_DEVICE "DMTAKE02"
#define DM_FILE_TAKEN_GROUPS "DMTAKEG2"

/* Производный номер для локального SQL; 0 при неверном Ed25519. */
uint64_t dm_wire_node_id(const uint8_t ed[32]);
#define DM_CONTEXT_FLAG 0x8000u /* Старший бит uint16 data_len; фактическая длина не более1040. */
#define DM_CONTEXT_MAX (1 + 2 * DM_PARTY_WIRE_SIZE + 2 + DM_TARGET_MAX * DM_TARGET_PROOF_SIZE)
#define DM_BODY_MAX (DM_MSG_FIXED_HDR + 255 + 2 + DM_CONTEXT_MAX + 1024 + 16 + 64)
#define DM_FILE_BODY_MAX (DM_BODY_MAX - 255 + 4 + 56)

struct dm_message_view {
    uint64_t conv, seq, timestamp;        /* Числовые поля префикса, native-endian. */
    uint8_t author_ed[32], author_x[32];  /* Подписанный адрес и производный транспортный ключ. */
    uint64_t author;                       /* Производный номер для локальных SQL-ключей. */
    enum dm_mode mode;
    struct dm_party parties[2];           /* Только GROUPS: сторона автора, другая сторона. */
    uint16_t target_count;                /* GROUPS: снимок включает автора. DEVICE: 0. */
    const uint8_t* targets;               /* target_count сертификатов по DM_TARGET_PROOF_SIZE. */
    const uint8_t* content_type;          /* Не NUL-строка, ровно content_type_size байт. */
    uint8_t content_type_size;
    const uint8_t* ciphertext;            /* ciphertext_size байт, включая CCM tag. */
    uint16_t ciphertext_size;
    const uint8_t* media;                 /* UUID/size/hash:56 либо NULL. */
    const uint8_t* signature;             /* Подпись автора:64. */
    size_t size;                         /* Длина тела; parser допускает хвост после него. */
};

/* 0/-1: проверить границы/форму тела и вернуть view. Выход не меняется при ошибке.
 * Проверка точной длины, подписей и адресата — обязанность принимающего модуля. */
int dm_message_parse(const uint8_t* body, size_t available, struct dm_message_view* out);

/* 0/-1: GROUPS проверяет ID беседы, уникальный snapshot в порядке Ed25519, обе стороны,
 * наличие автора на стороне 0 и подписи удостоверений. DEVICE — пустая операция:
 * ID/подпись конкретной пары проверяет dm_core. X25519 вычисляется из Ed25519. */
int dm_message_verify_targets(const struct dm_message_view* view);

/* GROUPS: найти node в снимке, 0/1 — сторона, -1 — отсутствует. При успехе
 * x/ed (если не NULL) получают по 32 байта. Для DEVICE всегда -1. */
int dm_message_target_key(const struct dm_message_view* view, const uint8_t ed[32]);
int dm_message_target(const struct dm_message_view* view, uint64_t node, uint8_t x[32], uint8_t ed[32]);
/* 0/-1: прочитать node/side по индексу GROUPS-snapshot, без проверки подписи. */
int dm_message_target_at(const struct dm_message_view* view, unsigned index, struct dm_target* out);

/* 0/-1: сформировать контекст GROUPS (между data_len с флагом и ciphertext).
 * VERSION=2:u8, parties:2*65,count:LE16,sorted certificates:count*97.
 * Сторона 0 должна включать текущего отправителя. out меняется только при успехе;
 * DM_CONTEXT_MAX достаточно. Источник включается, неизвестные устройства не ищутся. */
int dm_context_encode(struct UTUN_INSTANCE* inst, const struct dm_party parties[2],
    uint8_t* out, size_t capacity, size_t* length);
/* SQL/worker: тот же контекст по db и копии identity, без runtime.
 * SQLITE_OK либо положительный SQLite-код; out/length меняются только при успехе.
 * Caller задаёт согласованную транзакцию чтения/записи; все SQL statements закрываются до возврата. */
int dm_context_encode_read(struct sqlite3* db, const struct dm_store_identity* identity, const struct dm_party parties[2],
    uint8_t* out, size_t capacity, size_t* length);

#ifdef __cplusplus
}
#endif
#endif
