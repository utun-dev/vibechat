/* Qt-обёртка общего C-кодека src/chat/invite_link.h: decodeInviteLink переводит QString в UTF-8 и копирует результат.
 * Поддерживаемую версию определяет общий кодек; ошибка возвращается в InviteData.error.
 * Успешный разбор не проверяет срок регистрации join_key/членство и не запускает подключение.
 * Структуры Qt и C имеют одинаковые имена, но различное представление: не передавать их между API. */
#pragma once

#include <QString>
#include <QByteArray>
#include <QList>
#include <cstdint>
#include "../db/db_manager.h"

#define INVITE_KIND_CHANNEL 0
#define INVITE_KIND_SERVER 1

#define INVITE_PROTO_UDP      0x01
#define INVITE_PROTO_TCP      0x02
#define INVITE_PROTO_REALITY  0x04   // флаг reality-камуфляжа поверх TCP (reality-TCP = 0x06)

#define INVITE_SNAME_MAX      64

struct InviteAddr {
    int        family;   // 4=IPv4, 6=IPv6
    QByteArray address;  // 4/16 сетевых байтов в зависимости от family.
    quint16    port;     // Порт в host byte order.
    uint8_t    socketId; // socket_id из ETCP
    uint8_t    proto;    // INVITE_PROTO_*
};

/* Reality-параметры узла (node-global: одинаковы для всех reality-сокетов). */
struct InviteReality {
    bool        has = false; // При false остальные поля не используются.
    QByteArray  shortId;      // 8 байт
    QByteArray  serverPubkey; // 32 байта
    QByteArray  version;      // 3 байта
    QString     serverName;  // UTF-8 SNI из общего кодека, преобразованный в QString.
};

struct InviteData {
    uint8_t           kind = INVITE_KIND_CHANNEL; // Тип определяет действие; секрет сервера остаётся в исходной ссылке.
    uint64_t          joinKey = 0;   // 64-битный ключ входа (генерируется инвайтером)
    QByteArray        pubkey;       // 32 байта X25519 connection-узла при успешном разборе.
    QList<InviteAddr> addrs;        // Проверенные общим кодеком адреса того же connection-узла.
    QString           error;       // Пусто при успехе; иначе текст ошибки, остальные поля не использовать.
    InviteReality     reality;     // Общие REALITY-параметры connection-узла.
};

// link передаётся целиком, без trim/исправления регистра; возвращённые Qt-поля владеют своими данными.
InviteData decodeInviteLink(const QString& link);
