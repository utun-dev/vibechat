/* Политика исходящих подключений к узлу: T=TCP, U=UDP, R=REALITY.
 * connection_order_parse проверяет всю строку; 0 — успех, -1 — ошибка с логом.
 * Режим 2 содержит время этапа в секундах (1..3600). Строка wire ограничена 255 байтами. */
#ifndef CONNECTION_ORDER_H
#define CONNECTION_ORDER_H
#include <string.h>
#include "../../lib/debug_config.h"
#define CONNECTION_ORDER_DEFAULT "RUT2-2"
#define CONNECTION_ORDER_BOOTSTRAP "RUT2-1"
#define CONNECTION_ORDER_MAX 256
struct connection_order {
    unsigned protocols[3]; /* Биты: TCP=1, UDP=2, REALITY=4, в порядке попыток. */
    unsigned count, mode, seconds;
};
static inline unsigned connection_order_protocol(unsigned protocol) {
    return (protocol & 4) ? 4 : ((protocol & 2) ? 1 : 2);
}
static inline int connection_order_parse(const char* text, struct connection_order* out) {
    struct connection_order p = { {0, 0, 0}, 0, 0, 0 };
    unsigned seen = 0;
    const char* s = text;
    if (!s || !*s || strlen(s) >= CONNECTION_ORDER_MAX) goto invalid;
    while (*s == 'T' || *s == 'U' || *s == 'R') {
        unsigned bit = *s == 'T' ? 1 : (*s == 'U' ? 2 : 4);
        if (seen & bit) goto invalid;
        seen |= bit; p.protocols[p.count++] = bit; s++;
    }
    if (!p.count || *s < '1' || *s > '3') goto invalid;
    p.mode = (unsigned)(*s++ - '0');
    if (p.mode == 2) {
        if (*s != '-') goto invalid;
        s++;
        if (*s < '1' || *s > '9') goto invalid;
        while (*s >= '0' && *s <= '9') {
            p.seconds = p.seconds * 10 + (unsigned)(*s++ - '0');
            if (p.seconds > 3600) goto invalid;
        }
    }
    if (*s) goto invalid;
    if (out) *out = p;
    return 0;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "invalid connection_order='%s' (expected TUR1, TUR2-4 or TUR3)", text ? text : "(null)");
    return -1;
}
#endif
