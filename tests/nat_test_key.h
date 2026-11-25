#ifndef UTUN_TEST_NAT_KEY_H
#define UTUN_TEST_NAT_KEY_H

#include <stdint.h>
#include <string.h>

/* Синтетические адреса для проверки таблицы; идентификатор сценария занимает последние байты. */
static inline const uint8_t* nat_test_key(uint64_t value) {
    static uint8_t ed[32] = {1};
    memcpy(ed + 24, &value, sizeof(value));
    return ed;
}

#endif
