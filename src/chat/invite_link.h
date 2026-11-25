/*
 * invite_link.h — invite-ссылки utun:// для каналов
 *
 * Версия 0x08: utun:// + base64(version | kind | реквизиты | reality | блоки pubkey/addrs).
 * CHANNEL: join_key | inviter_pubkey32; SERVER: add_key[32], без полей канала.
 * Проверка общая для всех клиентов. ID/port — big-endian.
 * Сам кодек не подключает узел, не регистрирует join_key и не проверяет подпись/членство.
 * decode: 0/-1; encode: длина base64 без utun:// и NUL/-1; serialize_addrs: число байтов/-1.
 * Буферы предоставляет вызывающий; encode завершает строку NUL.
 */
#ifndef INVITE_LINK_H
#define INVITE_LINK_H

#include <stdint.h>
#include <stddef.h>

#define INVITE_LINK_VERSION     0x08
#define INVITE_KIND_CHANNEL     0
#define INVITE_KIND_SERVER      1
#define INVITE_ADD_KEY_SIZE     32
#define INVITE_LINK_MAX         1416 /* Максимальная ссылка с NUL: 32 IPv6, REALITY и ключ инвайтера. */
#define INVITE_PUBKEY_SIZE      32
#define INVITE_ADDR_MAX         32
#define INVITE_SNAME_MAX        64

#define INVITE_PROTO_UDP        0x01
#define INVITE_PROTO_TCP        0x02
#define INVITE_PROTO_REALITY    0x04   /* флаг reality-камуфляжа поверх TCP (reality-TCP = 0x06) */

#ifdef __cplusplus
extern "C" {
#endif

struct InviteAddr {
    int      family;             /* 4 или 6, не AF_INET/AF_INET6 */
    uint8_t  address[16];        /* сетевые байты IP; для IPv4 используются первые 4 */
    uint8_t  socketId;           /* ID сокета узла подключения */
    uint8_t  proto;              /* INVITE_PROTO_* */
    uint16_t port;              /* значение порта в порядке байтов хоста */
};

/* Reality-параметры узла (node-global: одинаковы для всех reality-сокетов). */
struct InviteReality {
    uint8_t  has;                 /* 1 = reality присутствует в ссылке */
    uint8_t  short_id[8];         /* идентификатор REALITY-сервера */
    uint8_t  server_pubkey[32];   /* публичный ключ REALITY-сервера */
    uint8_t  version[3];          /* версия REALITY */
    char     server_name[INVITE_SNAME_MAX]; /* SNI, NUL-строка до 63 байт */
};

struct InviteData {
    uint8_t kind;              /* INVITE_KIND_*; SERVER не разрешает обычный JOIN */
    uint8_t add_key[INVITE_ADD_KEY_SIZE]; /* постоянный секрет добавления сервера; у CHANNEL нули */
    uint64_t join_key;          /* Ключ хранится у пригласившего до закрытия окна либо до истечения срока. */
    uint8_t inviter_pubkey[32]; /* Ed25519 пригласившего для BGP и проверки сквозного JOIN; у SERVER нули. */
    uint8_t  pubkey[INVITE_PUBKEY_SIZE]; /* X25519 узла подключения */
    uint8_t  addrCount;         /* число используемых элементов addrs, не больше INVITE_ADDR_MAX */
    struct   InviteAddr addrs[INVITE_ADDR_MAX]; /* адреса узла подключения */
    struct   InviteReality reality; /* параметры при reality.has != 0 */
};

/* Проверить поля и адреса перед encode/использованием; 0/-1, без побочных действий. */
int invite_link_validate(const struct InviteData* data);

/* Разобрать link_len байт ссылки; out принадлежит вызывающему и не изменяется при ошибке.
 * 0/-1; error_buf необязателен, при ошибке получает NUL-строку, если error_buf_size > 0.
 * Успех гарантирует обязательные поля и согласованные блоки. */
int invite_link_decode(const char* link, size_t link_len, struct InviteData* out,
                       char* error_buf, size_t error_buf_size);

/* Записать полную ссылку с NUL. Требуются непустой pubkey и 1..INVITE_ADDR_MAX адресов.
 * Возврат: длина base64 без префикса utun:// и NUL либо -1; при ошибке out не использовать. */
int invite_link_encode(const struct InviteData* data, char* out, size_t out_size);

/* Подготовить бинарное представление адресов, без base64 и без счётчика:
 * [reality_has:1][при наличии: short_id:8,pubkey:32,version:3,sni_len:1,sni]
 * затем addrCount записей [family:1,socketId:1,proto:1,IP:4/16,port:2BE].
 * data должен содержать не больше INVITE_ADDR_MAX адресов и корректные NUL-строки.
 * Возврат: записанные байты либо -1 (нет аргумента/буфер мал); данные копируются в buf. */
int invite_serialize_addrs(const struct InviteData* data, uint8_t* buf, size_t buf_size);
/* Прочитать публичные адресные подсказки. При успехе меняет только адреса и REALITY. */
int invite_deserialize_addrs(const uint8_t* buf, size_t len, struct InviteData* out);

#ifdef __cplusplus
}
#endif
#endif /* INVITE_LINK_H */
