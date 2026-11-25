/* Профиль конкретного endpoint: изменение адреса и доступность входа независимы.
 * Измерение ETCP действует до изменения endpoint или нового результата; STUN — временная оценка того же IP/netif.
 * Профиль помогает выбирать попытки; работоспособность подтверждает транспорт. */
#ifndef ADDRESS_PROFILE_H
#define ADDRESS_PROFILE_H

#include <stdint.h>

enum address_mapping {
    ADDRESS_MAP_UNKNOWN,
    ADDRESS_MAP_REAL_IP,
    ADDRESS_MAP_EIM,
    ADDRESS_MAP_DEPENDENT
};

enum address_inbound {
    ADDRESS_IN_UNKNOWN,
    ADDRESS_IN_OPEN,
    ADDRESS_IN_CONTACT
};

enum address_profile_source { ADDRESS_SOURCE_NONE, ADDRESS_SOURCE_ETCP, ADDRESS_SOURCE_STUN };

struct address_profile {
    uint8_t mapping; /* enum address_mapping: UNKNOWN, без трансляции, EIM или зависимый mapping. */
    uint8_t inbound; /* enum address_inbound: UNKNOWN, OPEN либо нужен исходящий контакт. */
    uint8_t source; /* ETCP — измерение рабочего endpoint; STUN — предварительный EIM? с неизвестным входом. */
};

/* Постоянные строки для диагностики; неизвестное значение возвращает INVALID. */
static inline const char *address_mapping_name(uint8_t value) {
    static const char *const names[] = {"UNKNOWN", "REAL_IP", "EIM", "DEPENDENT"};
    return value <= ADDRESS_MAP_DEPENDENT ? names[value] : "INVALID";
}

static inline const char *address_inbound_name(uint8_t value) {
    static const char *const names[] = {"UNKNOWN", "OPEN", "CONTACT_REQUIRED"};
    return value <= ADDRESS_IN_CONTACT ? names[value] : "INVALID";
}

static inline const char *address_profile_source_name(uint8_t value) {
    static const char *const names[] = {"NONE", "ETCP", "STUN"};
    return value <= ADDRESS_SOURCE_STUN ? names[value] : "INVALID";
}

/* 1 — поля входят в enum и при source=NONE оба свойства UNKNOWN; 0 — иначе/NULL.
 * Проверяет представление, а не достоверность измерения или сочетания свойств. */
static inline int address_profile_valid(const struct address_profile *profile) {
    return profile && profile->mapping <= ADDRESS_MAP_DEPENDENT && profile->inbound <= ADDRESS_IN_CONTACT &&
        profile->source <= ADDRESS_SOURCE_STUN &&
        (profile->source || (!profile->mapping && !profile->inbound));
}

#endif
