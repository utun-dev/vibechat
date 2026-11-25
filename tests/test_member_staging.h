#ifndef TEST_MEMBER_STAGING_H
#define TEST_MEMBER_STAGING_H

/* Для тестов, включающих member_sync.c. Подать обычную страницу без автомата Merkle. */
static int member_stage_page(void* ctx, void* state, uint64_t prefix, int after_valid, const merkle_key_t* after,
    const merkle_key_t* next, int more, const uint8_t* data, size_t len) {
    struct merkle_sync_sql_request request = {
        .pull=state, .prefix=prefix, .after_valid=after_valid, .after=*after, .next=*next, .more=more, .length=len
    };
    if (len > sizeof(request.data)) return MT_ERR_DATA;
    memcpy(request.data, data, len);
    return member_stage_request(ctx, &request);
}

#endif
