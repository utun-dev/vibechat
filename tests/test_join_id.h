#ifndef TEST_JOIN_ID_H
#define TEST_JOIN_ID_H
#include <assert.h>
#include "chat/member_sync.h"
#include "transport_layer/secure_channel.h"

/* Независимо восстановить хеш удостоверения из открытых ключей тестового участника и группы. */
static inline uint8_t* test_join_id(const uint8_t gx[32], const uint8_t ged[32], const uint8_t ed[32], const uint8_t x[32], uint8_t id[32]) {
    uint8_t body[MEMBER_JOIN_MSG_SIZE];
    assert(member_sync_build_join_msg(gx, ged, ed, x, body, sizeof(body)) == sizeof(body));
    assert(!member_sync_join_id(body, id));
    return id;
}
#define TEST_JOIN_ID(gx, ged, ed, x) test_join_id(gx, ged, ed, x, (uint8_t[32]){0})
#endif
