/* Внутренний wire-формат local_transport; ручной codec, без выравнивания и native-структур.
 * Общий заголовок 108 байт (смещения):
 * 0: magic(16, строка LOCAL_MAGIC вместе с NUL), 16: version(1)=2, 17: type(1),
 * 18: длина только тела(2 BE), 20: source Ed25519(32), 52: source_epoch(8 BE),
 * 60: destination Ed25519(32), 92: destination_epoch(8 BE), 100: request(8 BE).
 * Полная UDP-датаграмма ровно HEADER_SIZE + length, максимум PACKET_MAX=1208; фрагментации модулем нет.
 * source/source_epoch всегда ненулевые, собственный source игнорируется.
 * ANNOUNCE: ник 1..127 байтов UTF-8 без NUL, destination/destination_epoch/request=0.
 * MESSAGE: произвольные 1..1100 байтов; ACK: ровно один байт enum local_delivery_status (0..2).
 * MESSAGE/ACK адресованы Ed25519/эпохе получателя и имеют request != 0; отправитель и endpoint должны быть известны.
 * Эти три типа не имеют подписи/шифрования; ACK подтверждает приём приложением, не выполнение содержимого.
 * Общий discovery-порт принимает только ANNOUNCE/ROUTE_ANNOUNCE. Остальные типы — на unicast-портах.
 * Проверка IP/входящего интерфейса выполняется до разбора заголовка. */
#ifndef LOCAL_TRANSPORT_PROTOCOL_H
#define LOCAL_TRANSPORT_PROTOCOL_H

#include "local_transport.h"
#include <string.h>

#define LOCAL_MAGIC "uTunChatLocalUDP"
#define LOCAL_VERSION 2
#define LOCAL_SOURCE_OFF 20
#define LOCAL_SOURCE_EPOCH_OFF 52
#define LOCAL_DESTINATION_OFF 60
#define LOCAL_DESTINATION_EPOCH_OFF 92
#define LOCAL_REQUEST_OFF 100
#define LOCAL_HEADER_SIZE 108
#define LOCAL_PACKET_MAX (LOCAL_HEADER_SIZE + LOCAL_TRANSPORT_DATA_MAX)
#define LOCAL_ANNOUNCE 1
#define LOCAL_MESSAGE 2
#define LOCAL_ACK 3
#define LOCAL_ROUTE_ANNOUNCE 4
#define LOCAL_ROUTE_CHALLENGE 5
#define LOCAL_ROUTE_PROOF 6

/* ROUTE_ANNOUNCE/ROUTE_PROOF: nonce(16), family(1: 4/6), IP(16, v4 с нулевым хвостом), count(1),
 * count * {socket_id(1), protocol(1), port(2 BE), generation(8 BE)}, Ed25519(64).
 * Подпись покрывает весь заголовок (включая оба Ed25519, эпохи и request)
 * и тело до подписи. ROUTE_ANNOUNCE имеет нулевые nonce/destination/destination_epoch/request;
 * ROUTE_PROOF адресован запросившему и повторяет его nonce. Для всех трёх ROUTE-типов request=0.
 * ROUTE_CHALLENGE: ровно ненулевой nonce(16), destination/epoch обязательны, подписи нет.
 * IP обязан совпасть с источником UDP, семейство — с его семейством; scope берётся из локального интерфейса.
 * count=0..32; port/generation != 0; socket_id уникальны; protocol — 1(UDP), 2(TCP), 6(TCP|REALITY).
 * Отправитель не рассылает пустой ROUTE_ANNOUNCE, но пустой PROOF допустим и очищает набор кандидатов.
 * Verify принимает подписанные адреса только от ранее известных ключей, ANNOUNCE не создаёт кандидаты.
 * Только однократный PROOF с ожидаемыми nonce/эпохами за 3с даёт кандидаты на 30с; повтор TTL не продлевает.
 * ROUTE_ANNOUNCE/ROUTE_PROOF обрабатываются только при route_offline != 0; ответ на CHALLENGE не зависит от gate.
 * Дополнительно требуется живой discovery-endpoint того же IP/UDP-порта/интерфейса.
 * Временные константы ниже — timebase (0.1мс), отсчитываются монотонно. */
#define LOCAL_ROUTE_NONCE_SIZE 16
#define LOCAL_ROUTE_HEADER_SIZE 34
#define LOCAL_ROUTE_SOCKET_SIZE 12
#define LOCAL_ROUTE_SIGNATURE_SIZE 64
#define LOCAL_ROUTE_CHALLENGE_TB 30000
#define LOCAL_ROUTE_TTL_TB 300000

/* 1 для пустого 32-байтного адреса. */
static inline int local_address_empty(const uint8_t ed[LOCAL_ADDRESS_KEY_SIZE]) {
    static const uint8_t zero[LOCAL_ADDRESS_KEY_SIZE] = {0};
    return !ed || !memcmp(ed, zero, sizeof(zero));
}

/* Записать uint64 BE в доступные 8 байтов p; проверок указателя/длины нет. */
static inline void local_put64(uint8_t* p, uint64_t value) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)value; value >>= 8; }
}
/* Прочитать uint64 BE из доступных 8 байтов p, без требований выравнивания и проверок границ. */
static inline uint64_t local_get64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = value << 8 | p[i];
    return value;
}

/* 1 = ник допустим: 1..127 байтов UTF-8 без NUL, управляющих ASCII, overlong, surrogate и кодов > U+10FFFF.
 * Иначе 0. p должен содержать len доступных байтов; завершающий NUL в len не входит. */
static inline int local_name_valid(const uint8_t* p, size_t len) {
    if (!len || len >= LOCAL_TRANSPORT_NAME_MAX) return 0;
    for (size_t i = 0; i < len;) {
        unsigned c = p[i++], value, count, minimum;
        if (c < 0x80) { if (c < 0x20 || c == 0x7f) return 0; continue; }
        if (c >= 0xc2 && c <= 0xdf) { value = c & 31; count = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { value = c & 15; count = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { value = c & 7; count = 3; minimum = 0x10000; }
        else return 0;
        if (count > len - i) return 0;
        while (count--) { c = p[i++]; if ((c & 0xc0) != 0x80) return 0; value = value << 6 | (c & 63); }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return 1;
}
#endif
