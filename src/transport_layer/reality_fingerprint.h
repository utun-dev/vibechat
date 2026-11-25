// reality_fingerprint.h — статические отпечатки (fingerprints) для REALITY-камуфляжа
//
// Отпечаток описывает статический скелет ClientHello/ServerHello: список cipher
// suites, supported groups, signature algorithms, ALPN и выбранный cipher suite
// для ServerHello. Билдер (reality.c) подставляет динамические поля (Random,
// SessionId, key_share, SNI) в этот скелет.
#ifndef REALITY_FINGERPRINT_H
#define REALITY_FINGERPRINT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct reality_fingerprint {
    int            id;                // REALITY_FP_OPENSSL (0), единственный текущий профиль
    const char    *name;               // "openssl", заимствованная неизменяемая строка
    const uint16_t *cipher_suites;      // список, терминирован 0x0000
    const uint16_t *supported_groups;   // список, терминирован 0x0000
    const uint16_t *sig_algs;           // список, терминирован 0x0000
    const char    *const *alpn;         // NULL-терминированный список
    uint16_t       server_cipher;       // выбранный cipher suite для ServerHello
};

// Возвращает общий неизменяемый static-профиль по id или NULL для неизвестного id; не освобождать.
const struct reality_fingerprint *reality_fingerprint_get(int id);

#ifdef __cplusplus
}
#endif
#endif // REALITY_FINGERPRINT_H
