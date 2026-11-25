// route_crypto.h — пред/пост-обработка SVC_ROUTE пакетов (шифрование + подпись)
// Автономный модуль: из одного пакета формирует другой, пригодный для отправки/обработки.
//
//   encode(): вызывается в начале функции отправки.
//     CHAT: [SVC_ROUTE_HDR][plaintext] → [SVC_ROUTE_HDR][encrypt(plaintext || signature)]
//   decode(): вызывается перед вызовом сервисного коллбэка.
//     [SVC_ROUTE_HDR][body выбранного формата] → [SVC_ROUTE_HDR][plaintext]
//
// CHAT подписывает заголовок и открытый payload, затем шифрует payload вместе с подписью.
// Получатель сначала расшифровывает, затем проверяет автора; до этого данные не выдаются.
// UTUN сохраняет свой формат: шифрование payload, затем наружная подпись заголовка/шифртекста.
// Формат выбирает вызывающий по локальному типу группы, а не отправитель пакета.
// Список пройденных узлов добавляет/снимает etcp_router снаружи этого формата.
// При ошибке декодирования (плохая подпись / не удалось расшифровать) пакет дропается.
// Все вызовы — в потоке ядра; локальные и переданные проверенные ключи должны быть готовы.
// Вход заимствован и не изменяется; при успехе *out принадлежит вызывающему (u_free).
// При ошибке out/out_len не устанавливаются. Буфер выделяется и при mode=0/отсутствии crypto-флагов.
#ifndef ROUTE_CRYPTO_H
#define ROUTE_CRYPTO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include "etcp_router.h"

struct UTUN_INSTANCE;

enum route_crypto_format {
    ROUTE_CRYPTO_CHAT, /* Подпись открытого заголовка/payload внутри шифрованного блока. */
    ROUTE_CRYPTO_UTUN  /* Прежний формат сервисов UTUN, подпись снаружи. */
};

// Флаги трансформации (bitmask), совпадают с битами SVC_ROUTE_HDR.flags
#define ROUTE_CRYPTO_ENCRYPT  ROUTER_FLAG_ENCRYPTED  // 0x04
#define ROUTE_CRYPTO_SIGN     ROUTER_FLAG_SIGNED     // 0x08

// Кодировать пакет: base = [SVC_ROUTE_HDR][plaintext payload].
// format — тип wire; mode — битовая маска ROUTE_CRYPTO_SIGN / ROUTE_CRYPTO_ENCRYPT.
// Codec допускает отдельные операции; router требует обе для любого сетевого CHAT-пакета.
// hdr.src_ed — локальный ключ; исходные crypto-флаги очищены.
// peer_x — проверенный X25519 получателя, обязателен при ENCRYPT; при SIGN без ENCRYPT допускается NULL.
// Возвращает новый буфер (u_malloc) с финальным пакетом. 0 = успех, <0 = ошибка.
int route_crypto_encode(struct UTUN_INSTANCE* inst, enum route_crypto_format format, const uint8_t peer_x[32],
                        const uint8_t* base, size_t base_len, int mode,
                        uint8_t** out, size_t* out_len);

// Декодировать пакет: wire = [SVC_ROUTE_HDR][body], format задаёт порядок расшифровки/проверки.
// Проверяет подпись и/или расшифровывает. Через *rx_flags сообщает, что было на wire
// (ROUTE_CRYPTO_SIGN / ROUTE_CRYPTO_ENCRYPT). Возвращает [SVC_ROUTE_HDR][plaintext] с очищенными флагами.
// Проверенные ключи передаёт вызывающий: для CHAT — из MEMBERS выбранной группы.
// peer_x обязателен при ENCRYPT, peer_ed при SIGN; для неиспользуемого ключа допускается NULL.
// rx_flags обязателен; может быть заполнен и при ошибке.
// 0 = успех, <0 = ошибка (пакет дропается).
int route_crypto_decode(struct UTUN_INSTANCE* inst, enum route_crypto_format format, const uint8_t peer_x[32], const uint8_t peer_ed[32],
                        const uint8_t* wire, size_t wire_len,
                        uint8_t** out, size_t* out_len, uint8_t* rx_flags);

#ifdef __cplusplus
}
#endif
#endif // ROUTE_CRYPTO_H
