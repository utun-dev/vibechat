/* Формат v2: magic:8, crypto_UUID:16, plain_size:8BE, meta_size:2BE, body_hash:32,
 * encrypted_meta, encrypted_file. Metadata nonce имеет part=UINT64_MAX.
 * Полный подписанный hash защищает заголовок вместе с обеими частями ciphertext. */
#include "encrypted_file.h"
#include "media_crypto.h"
#include "../transport_layer/secure_channel.h"
#include "media_async.h"
#include "../../lib/platform_compat.h"
#include "../../lib/debug_config.h"
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define EF_HEADER 66
#define EF_META (32 + ATTACHMENT_WIRE_MAX)

/* Одно ограничение поля для отправителя и аутентифицированных метаданных reader. */
int encrypted_file_validate_type(const char* type) {
    size_t length = type ? strnlen(type, 32) : 0;
    if (!length || length == 32) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "encrypted file: invalid content type length=%zu present=%d allowed=1..31", length, !!type);
        return -1;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned byte = (unsigned char)type[i];
        if (byte < 33 || byte > 126 || byte == '"' || byte == '\\') {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "encrypted file: invalid content type byte index=%zu value=0x%02x length=%zu", i, byte, length);
            return -1;
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "encrypted file: content type validated length=%zu", length);
    return 0;
}

/* Подготовить контейнер в эксклюзивном temp; конечный target не открывается для записи. */
int encrypted_file_write(const char* source, const char* target, struct file_io_temp* output, const uint8_t key[32],
                         const uint8_t author[32], const uint8_t id[16], const struct attachment_info* info, const char* type) {
    if (!output || output->file || output->temporary[0]) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "encrypted file: write requires inactive output owner"); return -1;
    }
    if (encrypted_file_validate_type(type)) return -1;
    FILE* in = NULL; FILE* out = NULL;
    int rc = -1;
    uint8_t header[EF_HEADER] = {0}, meta[EF_META] = {0}, cipher[EF_META + 16], nonce[13], hash[32];
    size_t length = 0;
    uint64_t size = 0;
    const char* reason = "invalid arguments";
    if (!source || !target || !key || !author || !id || attachment_validate(info)) goto done;
    reason = "source/target alias or invalid file type";
    if (file_io_paths(source, target)) goto done;
    reason = "cannot open source";
    in = file_io_open_read(source);
    if (!in) goto done;
    reason = "metadata encoding/encryption failed";
    int n = attachment_encode(info, meta + 32, ATTACHMENT_WIRE_MAX);
    if (n < 0) goto done;
    memcpy(meta, type, strlen(type));
    media_cipher_nonce(author, id, UINT64_MAX, nonce);
    if (media_ccm_encrypt(key, nonce, meta, (size_t)n + 32, cipher, &length)) goto done;
    reason = "cannot create private output";
    if (file_io_temp_open(output, target, FILE_IO_DURABLE)) goto done;
    out = output->file;
    reason = "container write/encryption failed";
    memcpy(header, "UTENC002", 8); memcpy(header + 8, id, 16);
    header[32] = (uint8_t)(length >> 8); header[33] = (uint8_t)length;
    if (fwrite(header, 1, sizeof(header), out) != sizeof(header) || fwrite(cipher, 1, length, out) != length ||
        media_encrypt_stream(key, author, id, in, out, &size, hash)) goto done;
    for (unsigned i = 0; i < 8; i++) header[24 + i] = (uint8_t)(size >> (56 - i * 8));
    memcpy(header + 34, hash, 32);
    reason = "cannot rewrite container header";
    if (fseeko(out, 0, SEEK_SET) || fwrite(header, 1, sizeof(header), out) != sizeof(header)) goto done;
    reason = "source close or output durable finish failed";
    rc = 0;
done:
    OPENSSL_cleanse(meta, sizeof(meta));
    if (in && fclose(in)) rc = -1;
    if (!rc) rc = file_io_temp_finish(output);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "encrypted file: preparation rejected source=%s target=%s reason=%s",
                    source ? source : "?", target ? target : "?", reason);
        file_io_temp_discard(output);
    } else DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "encrypted file: prepared author=%s bytes=%llu temp=%s",
                     sc_public_key_hex(author, (char[65]){0}), (unsigned long long)size, output->temporary);
    return rc;
}

/* Проверить подписанный hash, метаданные и все порции до разрешения публикации. */
int encrypted_file_read(const char* source, const char* target, struct file_io_temp* output, const uint8_t key[32], const uint8_t author[32],
                        const uint8_t id[16], const uint8_t hash[32], struct attachment_info* info, char type[32]) {
    if (!output || output->file || output->temporary[0]) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "encrypted file: read requires inactive output owner"); return -1;
    }
    FILE* in = NULL; FILE* out = NULL;
    struct attachment_info result;
    char result_type[32];
    int rc = -1;
    uint8_t actual[32], header[EF_HEADER], meta[EF_META], cipher[EF_META + 16], nonce[13];
    size_t length = 0;
    uint64_t size = 0;
    const char* reason = "invalid arguments";
    if (!source || !target || !key || !author || !id || !hash || !info || !type) goto done;
    reason = "source/target alias or invalid file type";
    if (file_io_paths(source, target)) goto done;
    reason = "cannot hash ciphertext";
    if (ma_sha256_file(source, actual)) goto done;
    reason = "ciphertext does not match signed manifest hash";
    if (CRYPTO_memcmp(actual, hash, 32)) goto done;
    reason = "cannot open ciphertext or truncated header";
    in = file_io_open_read(source);
    if (!in || fread(header, 1, sizeof(header), in) != sizeof(header)) goto done;
    reason = "unsupported container format or mismatched UUID";
    if (memcmp(header, "UTENC002", 8) || memcmp(header + 8, id, 16)) goto done;
    size_t encrypted_size = ((size_t)header[32] << 8) | header[33];
    reason = "invalid metadata length or truncated metadata";
    if (encrypted_size < 32 + 11 + 16 || encrypted_size > sizeof(cipher) ||
        fread(cipher, 1, encrypted_size, in) != encrypted_size) goto done;
    media_cipher_nonce(author, id, UINT64_MAX, nonce);
    reason = "metadata authentication failed";
    if (media_ccm_decrypt(key, nonce, cipher, encrypted_size, meta, &length)) goto done;
    reason = "invalid attachment metadata";
    if (length < 32 || attachment_decode(meta + 32, length - 32, &result)) goto done;
    reason = "invalid content type";
    if (encrypted_file_validate_type((const char*)meta)) goto done;
    memcpy(result_type, meta, 32);
    for (unsigned i = 0; i < 8; i++) size = (size << 8) | header[24 + i];
    reason = "cannot open temporary plaintext";
    if (file_io_temp_open(output, target, FILE_IO_DURABLE)) goto done;
    out = output->file;
    reason = "file authentication, size or body hash failed";
    if (media_decrypt_stream(key, author, id, size, header + 34, in, out)) goto done;
    reason = "close or durable file sync failed";
    rc = 0;
done:
    OPENSSL_cleanse(meta, sizeof(meta));
    if (in && fclose(in)) rc = -1;
    if (!rc) rc = file_io_temp_finish(output);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "encrypted file: rejected source=%s target=%s reason=%s",
                    source ? source : "?", target ? target : "?", reason);
        file_io_temp_discard(output);
    } else {
        *info = result; memcpy(type, result_type, sizeof(result_type));
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "encrypted file: verified author=%s bytes=%llu temp=%s",
                   sc_public_key_hex(author, (char[65]){0}), (unsigned long long)size, output->temporary);
    }
    return rc;
}
