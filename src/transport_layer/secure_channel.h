/* secure_channel — криптографические операции транспорта, без сокетов и сетевого handshake.
 * Инициализировать локальные X25519-ключи → init_ctx → set_peer_public_key → encrypt/decrypt.
 * ctx заимствует SC_MYKEYS до конца использования; состояние счётчиков и stream-объекты имеют одного владельца.
 * Пакеты: AES-128-CCM над plaintext+CRC32; выход = nonce(13)+ciphertext+tag(16).
 * Каждый nonce создаёт OpenSSL CSPRNG; при ошибке RNG encrypt возвращает ошибку с нулевой длиной выхода.
 * Отдельные API дают обфускацию ключа, потоковый AES-CTR и Ed25519; CTR сам по себе не аутентифицирует данные.
 * Возврат SC_OK / отрицательный SC_ERR_*; caller владеет входными/выходными буферами. */
#ifndef SECURE_CHANNEL_H
#define SECURE_CHANNEL_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stddef.h>

// Размеры ключей
#define SC_PRIVKEY_SIZE 32
#define SC_PUBKEY_SIZE 32
#define SC_HASH_SIZE 32
#define SC_NONCE_SIZE 13  // Длина nonce в пакетном формате этого модуля
#define SC_SHARED_SECRET_SIZE SC_HASH_SIZE
#define SC_SESSION_KEY_SIZE 16
#define SC_TAG_SIZE 16
#define SC_CRC32_SIZE 4

// Обфускация pubkey при передаче: salt(8) + XOR ключа с SHA256(salt || peer_pubkey).
#define SC_PUBKEY_ENC_SALT_SIZE 8
#define SC_PUBKEY_ENC_SIZE (SC_PUBKEY_SIZE + SC_PUBKEY_ENC_SALT_SIZE)

// Коды возврата
#define SC_OK 0
#define SC_ERR_INVALID_ARG -1
#define SC_ERR_CRYPTO -2
#define SC_ERR_NOT_INITIALIZED -3
#define SC_ERR_AUTH_FAILED -4
#define SC_ERR_CRC_FAILED -5

#define SC_PEER_PUBKEY_BIN 0
#define SC_PEER_PUBKEY_HEX 1

// Типы
typedef int sc_status_t;
typedef struct secure_channel sc_context_t;

struct SC_MYKEYS {
    /* Локальные ключи */
    uint8_t private_key[SC_PRIVKEY_SIZE];
    uint8_t public_key[SC_PUBKEY_SIZE];
  };

// Контекст защищенного канала
struct secure_channel {
    struct SC_MYKEYS* pk;                    // заимствованная локальная пара до конца использования ctx
    /* Ключ пира (после key exchange) */
    uint8_t peer_public_key[SC_PUBKEY_SIZE];
    uint8_t session_key[SC_SESSION_KEY_SIZE];  /* Derived session key */

    uint8_t initialized;
    uint8_t peer_key_set;
    uint8_t session_ready;
    uint64_t tx_counter;                     // успешные encrypt; nonce создаётся независимо через CSPRNG
    uint64_t rx_counter;                     // успешные decrypt после проверки CCM tag и CRC
};

// Функции инициализации
sc_status_t sc_init_ctx(sc_context_t *ctx, struct SC_MYKEYS *mykeys);
sc_status_t sc_generate_keypair(struct SC_MYKEYS *keys);
/* Единая идентичность устройства или CHAT-группы. Секрет на диске — seed Ed25519.
 * Восстановить из seed или сгенерировать seed с нулевым битом знака публичного Ed25519.
 * x/x_private/ed — выходы по 32 байта. SC_OK при успехе, иначе SC_ERR_*.
 * Из некорректного seed/при отказе crypto идентичность не публиковать. */
sc_status_t sc_identity_from_seed(const uint8_t seed[32], uint8_t x[32], uint8_t x_private[32], uint8_t ed[32]);
sc_status_t sc_identity_generate(uint8_t x[32], uint8_t x_private[32], uint8_t ed[32], uint8_t seed[32]);
/* Создать только поля для конфигурации: публичный X25519 и seed Ed25519. Возврат SC_OK/SC_ERR_*. */
sc_status_t sc_identity_generate_config(uint8_t public_x[32], uint8_t seed[32]);
/* Загрузить seed и сверить X25519 из HEX-конфига. Выходы действительны только при SC_OK. */
sc_status_t sc_identity_load(const char* public_hex, const char* seed_hex, struct SC_MYKEYS* keys, uint8_t ed[32], uint8_t seed[32]);
/* Получить X25519 из канонического Ed25519. При SC_ERR_* выход x сохраняется. scope задаёт контекст лога. */
sc_status_t sc_identity_public_x(const char* scope, const uint8_t ed[32], uint8_t x[32]);
/* По публичным данным проверить канонический Ed25519 и соответствие X25519.
 * scope — контекст диагностики. SC_ERR_AUTH_FAILED означает недействительную привязку. */
sc_status_t sc_identity_check(const char* scope, const uint8_t x[32], const uint8_t ed[32]);
sc_status_t sc_init_local_keys(struct SC_MYKEYS *mykeys, const char *public_key, const char *private_key);
sc_status_t sc_set_peer_public_key(sc_context_t *ctx, const uint8_t *peer_public_key, int mode);// mode: 0-bin 1-hex key format
sc_status_t sc_compute_public_key_from_private(const uint8_t *private_key, uint8_t *public_key);

/* Выходные длины только заполняются, не задают capacity! Для encrypt выделить plaintext_len+33 байта,
 * для decrypt — ciphertext_len-33. Пустой plaintext запрещён; decrypt проверяет CCM tag и CRC.
 * Проверка повторного посещения/эпохи транспорта находится выше этого модуля. */
sc_status_t sc_encrypt(sc_context_t *ctx, const uint8_t *plaintext,  size_t plaintext_len,  uint8_t *ciphertext, size_t *ciphertext_len);
sc_status_t sc_decrypt(sc_context_t *ctx, const uint8_t *ciphertext, size_t ciphertext_len, uint8_t *plaintext,  size_t *plaintext_len);

// SHA256-based transcode: XOR data with SHA256(key)
sc_status_t sc_sha_transcode(const uint8_t *key, size_t key_len, uint8_t *data, size_t data_len);

/* output[32] = pubkey[32] XOR SHA256(salt[8] || peer_pubkey[32]); все аргументы обязательны, бинарные.
 * Для снятия маски peer_pubkey — собственный ключ получателя, pubkey — принятый обфусцированный ключ.
 * SC_OK/SC_ERR_INVALID_ARG. Второй вычисляемый в реализации SHA256 не участвует в 32-байтной маске. */
sc_status_t sc_obfuscate_pubkey(const uint8_t *salt, const uint8_t *peer_pubkey, const uint8_t *pubkey, uint8_t *output);

// --- Streaming cipher (AES-128-CTR, confidentiality only) ---

#define SC_STREAM_NONCE_SIZE 12

/* Неизменяемый снимок CTR; копируется в задания, не содержит EVP/ссылок на соединение. */
struct sc_stream_key {
    uint8_t key[SC_SESSION_KEY_SIZE]; // AES-128 ключ направления.
    uint8_t iv[16];                  // Начальный 128-битный счётчик, до прибавления смещения.
};

struct sc_stream_state {
    void *ectx;                   // EVP_CIPHER_CTX*
    struct sc_stream_key key;      // Снимок для независимой обработки участков.
    uint64_t offset;               // Число уже обработанных байтов, включая handshake/padding.
    uint8_t initialized;
};

/* Производный ключ, соль отправителя и направление задают CTR IV. state — нулевой или после cleanup. */
sc_status_t sc_stream_init(const uint8_t session_key[SC_SESSION_KEY_SIZE], struct sc_stream_state *state,
                           uint32_t stream_id, const uint8_t salt[SC_PUBKEY_ENC_SALT_SIZE]);
sc_status_t sc_stream_xor(struct sc_stream_state *state, uint8_t *data, size_t data_len);
/* XOR участка на абсолютном offset без изменения key. Буфер принадлежит caller; SC_OK/SC_ERR_*.
 * Допускает невыровненные участки; каждый вызов имеет свой EVP-контекст. Не переиспользовать TX-диапазоны. */
sc_status_t sc_stream_xor_at(const struct sc_stream_key *key, uint64_t offset, uint8_t *data, size_t len);
void sc_stream_cleanup(struct sc_stream_state *state);

// --- Streaming Ed25519 signature ---
// SHA-512 инкрементально (EVP_DigestUpdate) + Ed25519 подпись хеша (one-shot EVP_DigestSign).
// Память O(1), не зависит от размера данных.
// Перед первым использованием state = {0}; активный state имеет одного владельца и не копируется.

#define SC_SIGN_SIZE 64  // Ed25519 signature size

struct sc_stream_sign_state {
    void *md_ctx;    // EVP_MD_CTX* — SHA-512 аккумулятор
    void *pkey;      // EVP_PKEY* — Ed25519 ключ
    uint8_t initialized; // 1 — ресурсы готовы; 0 — пустое состояние
    uint8_t is_sign; // В активном состоянии 1=sign, 0=verify; после cleanup обнуляется
};

/* init — только для пустого state; ошибка не меняет state. При корректных входных аргументах
 * повторный init активного state возвращает SC_ERR_INVALID_ARG. Оба init: SC_OK / отрицательный SC_ERR_*.
 * Подпись использует seed Ed25519 устройства; ключ нужен только на время init. */
sc_status_t sc_stream_sign_init(const uint8_t seed[32], struct sc_stream_sign_state *state);
/* ed25519_pubkey — обязательный бинарный ключ[32], копируется OpenSSL во внутренний ресурс state. */
sc_status_t sc_stream_sign_verify_init(struct sc_stream_sign_state *state, const uint8_t *ed25519_pubkey);
/* Добавляет data[len] в активный state; len=0 — no-op, допускает NULL. SC_OK / отрицательный SC_ERR_*.
 * При ошибке EVP update владелец должен вызвать cleanup; продолжение хеширования не гарантируется. */
sc_status_t sc_stream_sign_update(struct sc_stream_sign_state *state, const uint8_t *data, size_t len);
/* sig_len задаёт capacity sig_out (не менее 64), при успехе получает длину 64. SC_OK / отрицательный SC_ERR_*.
 * NULL аргументы, недостаточная capacity и неверный режим оставляют state и выходные буферы без изменений.
 * После начала финализации освобождает sign state при любом результате; sig_out/sig_len действительны при SC_OK. */
sc_status_t sc_stream_sign_final(struct sc_stream_sign_state *state, uint8_t *sig_out, size_t *sig_len);
/* Проверяет sig[sig_len] для накопленного хеша в режиме verify; обычная подпись имеет длину 64.
 * SC_OK / отрицательный SC_ERR_*; NULL аргументы/неверный режим сохраняют state,
 * после начала финализации state освобождается при любом результате, включая неверную подпись. */
sc_status_t sc_stream_sign_verify(struct sc_stream_sign_state *state, const uint8_t *sig, size_t sig_len);
/* Освобождает ресурсы и обнуляет все поля; допускает NULL, пустой state и повторные вызовы. */
void sc_stream_sign_cleanup(struct sc_stream_sign_state *state);

sc_status_t sc_ed25519_sign(const uint8_t privkey[32], const uint8_t* msg, size_t msg_len, uint8_t sig_out[64]);
/* Проверить raw Ed25519 подпись: SC_OK — верна, SC_ERR_AUTH_FAILED — не совпала;
 * SC_ERR_CRYPTO — отказ allocation/init/операции, SC_ERR_INVALID_ARG — NULL вход.
 * Ошибку операции нельзя трактовать как доказательство повреждения подписанных данных. */
sc_status_t sc_ed25519_verify(const uint8_t pubkey[32], const uint8_t* msg, size_t msg_len, const uint8_t sig[64]);

/* SHA256(public_key[32]): первые 8 байт memcpy в uint64_t хоста, старший бит обнуляется.
 * public_key — бинарный X25519-ключ; при NULL/отказе SHA256 логирует ошибку и возвращает 0. */
uint64_t sc_derive_node_id_from_pubkey(const uint8_t *public_key);

/* Публичный ключ в 64 HEX для диагностики. При key=NULL возвращает "-", буфер принадлежит caller. */
const char* sc_public_key_hex(const uint8_t key[32], char out[65]);

int sc_hex_to_binary(const char *hex_str, uint8_t *binary, size_t binary_len);


#ifdef __cplusplus
}
#endif
#endif // SECURE_CHANNEL_H
