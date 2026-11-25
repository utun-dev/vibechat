// reality.h — REALITY-style TLS ClientHello/ServerHello camouflage для STCP
//
// Модуль формирует/разбирает ClientHello и ServerHello так же, как REALITY
// (см. 3.go): аутентификация прячется в SessionId ClientHello — весь SessionId
// (32 байта) это AES-256-GCM(ciphertext(16) + tag(16)), а открытый текст
// (16 байт) несёт version(3) + reserved(1) + timestamp(4) + ShortId(8).
// Ключ AuthKey = HKDF-SHA256(X25519(eph, static), Random[0:20], "REALITY").
//
// После Hello весь STCP передаётся в защищённых TLS-подобных записях.
// Это собственный протокол с TLS-фреймированием; ключи не совместимы с TLS.
#ifndef REALITY_H
#define REALITY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "reality_record.h"

// Размеры
#define REALITY_SHORT_ID_SIZE   8
#define REALITY_VERSION_SIZE    3
#define REALITY_AUTH_KEY_SIZE   32
#define REALITY_RANDOM_SIZE     32
#define REALITY_SESSION_ID_SIZE 32
#define REALITY_TAG_SIZE        16
#define REALITY_MAX_CH_SIZE     65535   // предел нормализованного ClientHello после сборки записей
#define REALITY_MAX_SH_SIZE     512    // максимальный размер ServerHello
#define REALITY_MAX_SHORT_IDS   64
#define REALITY_SERVER_NAME_MAX 256

// Коды возврата
#define REALITY_OK          0
#define REALITY_ERR_ARG     -1
#define REALITY_ERR_CRYPTO  -2
#define REALITY_ERR_AUTH    -3
#define REALITY_ERR_FORMAT  -4

// Отпечатки (reality_fingerprint.c)
#define REALITY_FP_OPENSSL   0
#define REALITY_FP_COUNT    1

struct reality_client_config;
struct reality_server_config;

/* Одноразовая сессия: перед start обнулить, после любой ошибки вызвать cleanup.
 * Все функции — в потоке владельца; входы/выходы не должны перекрываться. */
struct reality_session {
    uint8_t ephemeral_private[32], auth_key[32], session_id[32]; // секреты Hello и ожидаемый echo SessionId
    uint8_t finished_send[32], finished_recv[32]; // ключи проверки transcript по направлениям
    struct reality_traffic send, recv; // независимые ключи и счётчики защищённых записей
    void *transcript; // EVP_MD_CTX, точные handshake-байты без заголовков записей
    uint32_t auth_timestamp; // timestamp авторизованного ClientHello для replay-кеша I/O
    uint8_t is_server, keys_ready; // роль и готовность ключей после обоих Hello
};
// После обоих Finished удаляет transcript и handshake-секреты, сохраняя send/recv; NULL допустим.
void reality_session_handshake_done(struct reality_session *s);
// Освобождает transcript и стирает весь контекст; NULL допустим, повторный вызов безопасен.
void reality_session_cleanup(struct reality_session *s);
// Старт клиента: собирает Hello и сохраняет контекст для client_accept; cap >= REALITY_MAX_CH_SIZE.
// now — Unix-секунды. 0/REALITY_ERR_*; written действителен только при успехе.
int reality_session_client_start(struct reality_session *s, const struct reality_client_config *cfg, uint32_t now,
                                  uint8_t *out, size_t cap, size_t *written);
// Старт сервера: проверяет нормализованный ClientHello и выводит ключи; cap >= REALITY_MAX_SH_SIZE.
// Проверяет время/short_id, но уникальность Hello проверяет replay-кеш reality_io; 0/REALITY_ERR_*.
int reality_session_server_start(struct reality_session *s, const struct reality_server_config *cfg, uint32_t now,
                                  const uint8_t *ch, size_t len, uint8_t *out, size_t cap, size_t *written);
// Клиент принимает один полный ServerHello после client_start и выводит traffic/Finished-ключи; 0/REALITY_ERR_*.
int reality_session_client_accept(struct reality_session *s, const uint8_t *sh, size_t len);
// Добавляет точные handshake-байты (с handshake-заголовком) в SHA256 transcript; 0/-1.
int reality_transcript_add(struct reality_session *s, const uint8_t *data, size_t len);
// Вычисляет 32 байта Finished для текущего transcript; sending выбирает свой/чужой ключ; 0/-1.
int reality_finished(struct reality_session *s, int sending, uint8_t out[32]);

// Конфигурация клиента
struct reality_client_config {
    uint8_t     server_static_pubkey[REALITY_AUTH_KEY_SIZE]; // X25519 static pubkey сервера
    uint8_t     short_id[REALITY_SHORT_ID_SIZE];              // выбранный серверный идентификатор
    uint8_t     version[REALITY_VERSION_SIZE];               // версия протокола utun
    char        server_name[REALITY_SERVER_NAME_MAX];        // непустое NUL-terminated SNI, максимум 255 байт
    int         fingerprint;                                 // REALITY_FP_*
};

// Конфигурация сервера
struct reality_server_config {
    uint8_t     static_privkey[REALITY_AUTH_KEY_SIZE];       // X25519 static privkey сервера
    uint8_t     short_ids[REALITY_MAX_SHORT_IDS][REALITY_SHORT_ID_SIZE]; // допустимые идентификаторы клиента
    int         short_id_count;                              // 1..REALITY_MAX_SHORT_IDS
    uint8_t     version[REALITY_VERSION_SIZE];               // принимаемая версия
    int64_t     time_window_sec;                             // допуск timestamp 0..3600 секунд; replay-кеш в instance
    int         relay_idle_timeout_sec;                      // idle-timeout релея неавторизованных (0=выкл)
    int         fingerprint;                                 // REALITY_FP_*; сервер сейчас использует профиль OpenSSL
};

// Общая конфигурация reality (секция [reality] в конфиге utun)
#define REALITY_DEST_MAX 256
struct reality_config {
    int     enabled;                                         // 1 = камуфляж включён
    char    server_name[REALITY_SERVER_NAME_MAX];            // SNI-таргет
    char    dest[REALITY_DEST_MAX];                          // "host:port" для релея (fallback)
    uint8_t short_id[REALITY_SHORT_ID_SIZE];                 // клиентский short_id
    uint8_t short_ids[REALITY_MAX_SHORT_IDS][REALITY_SHORT_ID_SIZE]; // сервер: список
    int     short_id_count;                                 // число допустимых серверных short_ids
    uint8_t public_key[REALITY_AUTH_KEY_SIZE];               // клиент: static pubkey сервера
    uint8_t private_key[REALITY_AUTH_KEY_SIZE];              // сервер: static privkey
    uint8_t has_public_key;                                 // public_key задан в конфиге
    uint8_t has_private_key;                                // private_key задан в конфиге
    uint8_t version[REALITY_VERSION_SIZE];                   // версия протокола
    int     time_window_sec;                                // допустимое расхождение Unix-времени (0..3600)
    int     relay_idle_timeout_sec;                          // idle-timeout релея неавторизованных (0=выкл)
    int     fingerprint;                                   // REALITY_FP_*
};

// Сбрасывает cfg: выключен, ключи/строки пусты; version=2.0.0, OpenSSL, time_window=30, relay_idle=5; NULL допустим.
void reality_config_set_defaults(struct reality_config *cfg);

// Сбрасывает клиентский cfg: ключ/short_id/SNI пусты, version=2.0.0, OpenSSL; NULL допустим.
void reality_client_config_set_defaults(struct reality_client_config *cfg);

// Клиент: собрать ClientHello (TLS record + handshake) с REALITY-авторизацией.
// Генерирует эфемерный X25519 и Random внутри. out_cap >= REALITY_MAX_CH_SIZE.
// Возвращает REALITY_OK / REALITY_ERR_*, в *out_len — итоговый размер.
int reality_client_hello_build(const struct reality_client_config *cfg,
                               uint8_t *out, size_t out_cap, size_t *out_len);

// То же, но с явным timestamp (unix-секунды) — для тестов антиреплей-проверки.
int reality_client_hello_build_at(const struct reality_client_config *cfg, uint32_t ts,
                                  uint8_t *out, size_t out_cap, size_t *out_len);

// Сервер: распарсить ClientHello, проверить авторизацию, собрать ServerHello.
// ch/ch_len — ровно одна полная нормализованная запись TLS с ClientHello; out_cap >= REALITY_MAX_SH_SIZE.
// Обёртка без контекста сессии: не проверяет повторы Hello и не сохраняет ключи защищённых записей.
// REALITY_OK     — авторизация валидна, out/out_len = ServerHello.
// REALITY_ERR_AUTH — авторизация не прошла (вызывающий код запускает релей).
// REALITY_ERR_*    — формат/аргумент/крипто-ошибка.
int reality_server_hello_build(const struct reality_server_config *cfg,
                               const uint8_t *ch, size_t ch_len,
                               uint8_t *out, size_t out_cap, size_t *out_len);

// То же, но с явным now (unix-секунды) для анти-реплей проверки timestamp.
// Вызывающий код передаёт синхронизированное время (ntp_time_get_seconds),
// чтобы не зависеть от сырого time(NULL) при рассинхроне системных часов.
int reality_server_hello_build_at(const struct reality_server_config *cfg, uint32_t now_sec,
                                  const uint8_t *ch, size_t ch_len,
                                  uint8_t *out, size_t out_cap, size_t *out_len);

// Сгенерировать X25519 пару ключей (для конфигурации: сервер хранит privkey,
// клиент — pubkey сервера). Возвращает REALITY_OK / REALITY_ERR_ARG / REALITY_ERR_CRYPTO.
int reality_generate_keypair(uint8_t priv[REALITY_AUTH_KEY_SIZE],
                             uint8_t pub[REALITY_AUTH_KEY_SIZE]);

// Вычислить X25519 pubkey из privkey. Возвращает REALITY_OK / REALITY_ERR_ARG / REALITY_ERR_CRYPTO.
int reality_pubkey_from_priv(const uint8_t priv[REALITY_AUTH_KEY_SIZE],
                             uint8_t pub[REALITY_AUTH_KEY_SIZE]);

#ifdef __cplusplus
}
#endif
#endif // REALITY_H
