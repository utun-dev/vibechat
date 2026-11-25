/* AEAD-записи внешнего REALITY; состояние обнуляется при создании и принадлежит одному TCP-сеансу.
 * Wire: 17 03 03 | uint16_be(ciphertext_len) | AES-128-GCM(content || inner_type || zero_padding) | tag[16].
 * AAD — пять байт заголовка; nonce[12] = base_iv XOR (zero[4] || uint64_be(sequence)).
 * Каждый успешный seal/open увеличивает свой sequence; повторно передаются только готовые wire-байты.
 * Ошибка требует закрытия сеанса. KeyUpdate последним сообщением под старым ключом меняет traffic secret
 * через HKDF("uTun-reality-update"), затем key/iv и sequence=0. Направления обновляются независимо. */
#ifndef REALITY_RECORD_H
#define REALITY_RECORD_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define REALITY_INNER_MAX 16385
#define REALITY_RECORD_MAX (5 + REALITY_INNER_MAX + 16)
#ifndef REALITY_KEY_LIMIT
#define REALITY_KEY_LIMIT (UINT64_C(1) << 20)
#endif
#define REALITY_TYPE_HANDSHAKE 22
#define REALITY_TYPE_APPLICATION 23
#define REALITY_TYPE_ALERT 21

struct reality_traffic {
    uint8_t secret[32], key[16], iv[12]; // traffic secret и производные AES-128-GCM ключ/базовый nonce
    uint64_t sequence;                 // следующий sequence направления; до REALITY_KEY_LIMIT-1 включительно
    uint32_t generation;               // диагностический счётчик KeyUpdate, init=0, циклический uint32_t
};

/* HKDF-SHA256, label — NUL-строка info. Входы NULL допустимы при длине 0;
 * out/out_len ненулевые, длины ikm/salt/label должны помещаться в int OpenSSL. 0/-1.
 * Входы заимствованы, выход принадлежит caller; при ошибке выход не использовать. */
int reality_kdf(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                const char *label, uint8_t *out, size_t out_len);
/* Производит key/iv из 32 байт secret, сбрасывает sequence/generation. Все указатели обязательны; 0/-1. */
int reality_traffic_init(struct reality_traffic *t, const uint8_t secret[32]);
/* Выводит следующий secret/key/iv, сбрасывает sequence, увеличивает generation; t обязателен, 0/-1. */
int reality_traffic_update(struct reality_traffic *t);
/* Тип — HANDSHAKE/APPLICATION/ALERT; len+1+padding<=REALITY_INNER_MAX.
 * data=NULL допустим при len=0 только для APPLICATION. out вмещает 5+len+1+padding+16.
 * Успех — 0 и *written=wire length; ошибка — -1, written/sequence не меняются, out может измениться. */
int reality_record_seal(struct reality_traffic *t, uint8_t type, const uint8_t *data, size_t len,
                        size_t padding, uint8_t *out, size_t cap, size_t *written);
/* Одна полная запись, без хвоста: 22..REALITY_RECORD_MAX байт. cap>=len-21 для всей расшифровки.
 * Успех — 0, *written=длина content без inner_type/padding, *type=тип, sequence++.
 * Все выходные указатели обязательны. -1 сохраняет written/type/sequence;
 * при отказе аутентификации/inner type расшифрованная область out очищается. После ошибки закрыть сеанс. */
int reality_record_open(struct reality_traffic *t, const uint8_t *record, size_t len,
                        uint8_t *out, size_t cap, size_t *written, uint8_t *type);
#ifdef __cplusplus
}
#endif
#endif
