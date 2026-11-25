/* media_crypto — AES-256-CCM сообщений и файлов без БД, транспорта и владения файлами.
 * Ключ 32 байта, nonce 13 байт, tag 16 байт; AAD нет. UUID новый для каждой подготовки
 * с данным ключом/автором; сетевые повторы используют уже готовый ciphertext.
 * FILE-функции вызываются в worker и работают с текущих позиций до EOF источника.
 * Caller открывает отдельные src/dst, закрывает их и публикует результат только после
 * успешной проверки и durable сохранения. Здесь есть fflush(dst), но нет close/fsync/rename.
 * Ключи, буферы и FILE заимствованы на время вызова. int-функции: 0 = успех, -1 = ошибка. */
#ifndef MEDIA_CRYPTO_H
#define MEDIA_CRYPTO_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MEDIA_CIPHER_KEY_SIZE 32
#define MEDIA_CIPHER_NONCE_SIZE 13
#define MEDIA_CIPHER_TAG_SIZE 16
#define MEDIA_CIPHER_PLAIN_MAX 65535u
#define MEDIA_CIPHER_CHUNK_SIZE 32768u
/* Первые 13 байт SHA256("utun-media-file-v2" с NUL || author_ed25519:32 || id:16 || part:u64LE).
 * Поля author/id/nonce обязательны, проверок аргументов нет.
 * part — номер порции с нуля; разным plaintext с тем же ключом нужен новый id. */
void media_cipher_nonce(const uint8_t author[32], const uint8_t id[16], uint64_t part, uint8_t nonce[MEDIA_CIPHER_NONCE_SIZE]);
/* plain_len 0..65535, plain=NULL допустим только при нулевой длине; out вмещает plain_len+16.
 * out/key/nonce/out_len обязательны; успех даёт ciphertext||tag и его длину.
 * После проверки аргументов *out_len=0 до успеха; при неверных аргументах выходы не меняются. */
int media_ccm_encrypt(const uint8_t key[32], const uint8_t nonce[13], const uint8_t* plain,
                      size_t plain_len, uint8_t* out, size_t* out_len);
/* cipher_len 16..65551, out вмещает cipher_len-16 байт; все указатели обязательны.
 * Только rc=0 разрешает использовать plaintext. При ошибке содержимое out не определено;
 * после проверки аргументов *out_len=0 до успешной проверки tag. При неверных аргументах выходы не меняются. */
int media_ccm_decrypt(const uint8_t key[32], const uint8_t nonce[13], const uint8_t* cipher,
                      size_t cipher_len, uint8_t* out, size_t* out_len);
/* Порции по 32768 plaintext-байт, каждая с отдельным tag; пустой файл тоже даёт один tag.
 * Все указатели обязательны, src!=dst; файлы должны обозначать разные данные.
 * При корректных аргументах plain_size/hash обнуляются и выдаются только при успехе:
 * фактический размер plaintext и SHA256 всего ciphertext, без внешнего заголовка контейнера.
 * Ошибка может оставить частичный dst; выходные FILE не закрываются. */
int media_encrypt_stream(const uint8_t key[32], const uint8_t author[32], const uint8_t id[16],
                         FILE* src, FILE* dst, uint64_t* plain_size, uint8_t hash[32]);
/* Те же требования к аргументам/файлам; plain_size/hash берутся из проверенного manifest.
 * Проверяет каждый tag, точный размер (включая EOF) и полный hash ciphertext.
 * dst должен быть собственным временным файлом: до общей проверки в него уже записываются порции.
 * Частичный/непроверенный dst при ошибке отбрасывает вызывающий. */
int media_decrypt_stream(const uint8_t key[32], const uint8_t author[32], const uint8_t id[16],
                         uint64_t plain_size, const uint8_t hash[32], FILE* src, FILE* dst);
#ifdef __cplusplus
}
#endif
#endif
