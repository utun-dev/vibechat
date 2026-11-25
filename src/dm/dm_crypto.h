/*
 * dm_crypto.h — криптография прямого чата (DM)
 *
 * conv_id выводится из ID двух узлов, content_key — из своего X25519-ключа и
 * публичного ключа пира. Переговоров нет; nonce сообщения зависит от conv_id/author/seq.
 * CCM и файловые примитивы предоставляет общий media_crypto.h.
 */

#ifndef DM_CRYPTO_H
#define DM_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include "../media_async/media_crypto.h"
#include "dm_identity.h"

#ifdef __cplusplus
extern "C" {
#endif


#define DM_CONTENT_KEY_SIZE  32
#define DM_NONCE_SIZE        13   /* AES-CCM требует ровно 13 байт */
#define DM_TAG_SIZE          16
#define DM_CCM_PLAIN_MAX     65535u
#define DM_MEDIA_PLAIN_SIZE  32768u

/* SHA256("utun-dm-device-v2" с NUL, затем два открытых Ed25519 в лексикографическом порядке).
 * Первые 8 байт читаются как LE, старший бит сбрасывается, ноль заменяется единицей.
 * 0 при отсутствующих или совпадающих ключах. */
uint64_t dm_derive_conv_id(const uint8_t a[32], const uint8_t b[32]);

/* content_key = SHA256(X25519(my_priv, peer_pub) || "utun_dm_content"), суффикс — 15 байт без NUL.
 * my_priv/peer_pub — по 32 байта, out — 32 байта. Возвращает 0 при успехе, -1 при ошибке.
 * При ошибке out не меняется; NULL-аргументы отклоняются. */
int dm_derive_content_key(const uint8_t my_priv[32], const uint8_t peer_pub[32],
                          uint8_t out[DM_CONTENT_KEY_SIZE]);

/* Детерминированный nonce: первые 13 байт SHA256(conv_id || author_ed || seq).
 * Все три uint64_t хешируются в порядке байтов хоста; nonce — обязательный буфер 13 байт.
 * author разделяет направления; вызывающий не должен повторять seq при том же ключе/авторе/conv_id.
 * Усечение хеша не даёт математической гарантии отсутствия коллизий. */
void dm_build_nonce(uint64_t conv_id, const uint8_t author[32], uint64_t seq, uint8_t nonce[DM_NONCE_SIZE]);

#define DM_KEY_WRAP_SIZE 80 /* HPKE X25519 enc:32 + encrypted secret:32 + GCM tag:16. */
#define DM_FILE_KEYS_SIZE (64 + 2 * DM_KEY_WRAP_SIZE) /* F_public, file enc, две упаковки F_private. */

/* Независимая HPKE-упаковка 32-байтового секрета к X25519 ключу. RFC9180 base,
 * X25519/HKDF-SHA256/AES-256-GCM. info связывает назначение/идентичность (до1024 байт).
 * 0/-1, выход не меняется при ошибке. Открытый секрет caller обязан очистить. */
int dm_key_seal(const uint8_t public_key[32], const uint8_t* info, size_t info_size,
    const uint8_t secret[32], uint8_t out[DM_KEY_WRAP_SIZE]);
int dm_key_open(const uint8_t private_key[32], const uint8_t* info, size_t info_size,
    const uint8_t wrapped[DM_KEY_WRAP_SIZE], uint8_t out[32]);

/* Создать собственную X25519 пару вложения и вывести файловый AES ключ через
 * HPKE export-only. F_private упакован независимо для parties[0] и parties[1].
 * parties идут в порядке автор/собеседник; conv/Ed25519 автора/UUID фиксируются до worker.
 * 0/-1, выходы меняются только при успехе. Файловый ключ caller очищает после работы. */
int dm_file_keys_create(uint64_t conv, const uint8_t author[32], const uint8_t id[16], const struct dm_party parties[2],
    uint8_t out[DM_FILE_KEYS_SIZE], uint8_t file_key[32]);
/* Раскрыть свою упаковку (side=0/1), проверить F_public и вывести тот же файловый
 * ключ. Собственный private_key соответствует parties[side]; 0/-1, out неизменен при ошибке. */
int dm_file_keys_open(uint64_t conv, const uint8_t author[32], const uint8_t id[16], const struct dm_party parties[2],
    unsigned side, const uint8_t private_key[32], const uint8_t keys[DM_FILE_KEYS_SIZE], uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif /* DM_CRYPTO_H */
