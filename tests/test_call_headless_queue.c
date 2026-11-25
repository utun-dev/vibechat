/* Компонентная проверка очереди: реальные TCP/uasync/Opus и независимый reference decoder.
 * Закрытый media callback вызывается напрямую; полноценный звонок покрывает test_call_headless.
 * Поля очереди только наблюдаются. send-wrapper задаёт EAGAIN/short write выбранному локальному fd. */
#include "../src/call/call_headless.c"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "headless-queue:%d %s", __LINE__, #x); abort(); } } while (0)
#define WIRE_CAP (128 * 1024)
#define TEST_CALL_ID 42

static socket_t observed_fd = SOCKET_INVALID;
static int blocked;
static size_t first_short, write_chunk, sent_bytes;
static unsigned errors, drop_warnings, closed_drops, destroyed_drops;
extern ssize_t __real_send(int fd, const void* data, size_t len, int flags);

/* Успешная часть short write реально передаётся TCP, её хвост не подменяется. */
ssize_t __wrap_send(int fd, const void* data, size_t len, int flags) {
    if (fd != observed_fd) return __real_send(fd, data, len, flags);
    if (blocked) { errno = EAGAIN; return -1; }
    int block_after = first_short != 0;
    if (first_short && len > first_short) len = first_short;
    if (write_chunk && len > write_chunk) len = write_chunk;
    ssize_t result = __real_send(fd, data, len, flags);
    if (result > 0) {
        sent_bytes += (size_t)result;
        if (block_after) { CHECK((size_t)result == len); first_short = 0; blocked = 1; }
    }
    return result;
}

/* Проверяем также отдельный учёт PCM-потерь и наличие диагностики. */
static void capture_log(int level, const char* category, const char* message) {
    if (!category || strcmp(category, "call")) return;
    if (level == DEBUG_LEVEL_ERROR) ++errors;
    if (strstr(message, "PCM frame dropped")) ++drop_warnings;
    const char* dropped = strstr(message, "pcm_drop=");
    if (dropped && strstr(message, "audio client closed")) CHECK(sscanf(dropped, "pcm_drop=%u", &closed_drops) == 1);
    if (dropped && strstr(message, "audio socket destroyed")) CHECK(sscanf(dropped, "pcm_drop=%u", &destroyed_drops) == 1);
}

struct queue_fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE inst;
    struct call_audio* ca;
    struct call_audio_client* cli;
    socket_t peer;
    opus_codec_encoder_t* encoder;
    opus_codec_decoder_t* reference;
    uint8_t *wire, *expected;
    size_t wire_len, expected_len;
    unsigned sequence;
};

/* Один uasync на поток, свежий listener/клиент/кодеки для каждого сценария. */
static void fixture_start(struct queue_fixture* f) {
    memset(f, 0, sizeof(*f));
    observed_fd = SOCKET_INVALID; blocked = 0; first_short = write_chunk = sent_bytes = 0;
    errors = drop_warnings = closed_drops = destroyed_drops = 0;
    f->ua = uasync_create(); CHECK(f->ua);
    f->inst.ua = f->ua;
    CHECK(call_headless_init(f->ua, &f->inst, "127.0.0.1", 0) == 0);
    f->peer = socket(AF_INET, SOCK_STREAM, 0); CHECK(f->peer != SOCKET_INVALID);
    struct sockaddr_in addr = {0}; addr.sin_family = AF_INET;
    addr.sin_port = htons(call_headless_get_port(&f->inst)); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(f->peer, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    CHECK(socket_set_nonblocking(f->peer) == 0);
    uint64_t id = TEST_CALL_ID;
    uint8_t hello[CALL_AUDIO_HDR_SIZE] = {CALL_AUDIO_HELLO}; memcpy(hello + 1, &id, 8);
    CHECK(send(f->peer, hello, sizeof(hello), 0) == sizeof(hello));
    f->ca = ca_of(&f->inst);
    uint64_t deadline = get_time_tb() + 20000;
    while (!f->ca->clients || f->ca->clients->call_id != id) {
        CHECK(get_time_tb() < deadline); uasync_poll(f->ua, 10);
    }
    f->cli = f->ca->clients; observed_fd = f->cli->fd;
    f->encoder = opus_codec_encoder_create(CA_SAMPLE_RATE, 1); CHECK(f->encoder);
    opus_codec_encoder_bitrate_set(f->encoder, CA_BITRATE);
    f->reference = opus_codec_decoder_create(CA_SAMPLE_RATE, 1); CHECK(f->reference);
    f->wire = u_malloc(WIRE_CAP); f->expected = u_malloc(WIRE_CAP); CHECK(f->wire && f->expected);
}

/* Reference decoder видит и отброшенные кадры: его Opus-состояние совпадает с принимающим. */
static void emit_frame(struct queue_fixture* f, int retained) {
    int16_t pcm[CA_FRAME_SAMPLES], decoded[CA_FRAME_SAMPLES];
    for (int i = 0; i < CA_FRAME_SAMPLES; ++i) pcm[i] = (int16_t)(((int)f->sequence * 73 + i * 31) % 6000 - 3000);
    ++f->sequence;
    uint8_t opus[256];
    int len = opus_codec_encode(f->encoder, pcm, CA_FRAME_SAMPLES, opus, sizeof(opus)); CHECK(len > 0);
    CHECK(opus_codec_decode(f->reference, opus, len, decoded, CA_FRAME_SAMPLES) == CA_FRAME_SAMPLES);
    if (retained) {
        CHECK(f->expected_len + CALL_AUDIO_HDR_SIZE + sizeof(decoded) <= WIRE_CAP);
        uint8_t* out = f->expected + f->expected_len;
        uint64_t id = TEST_CALL_ID;
        out[0] = CALL_AUDIO_FRAME; memcpy(out + 1, &id, 8);
        out[9] = (uint8_t)(sizeof(decoded) >> 8); out[10] = (uint8_t)sizeof(decoded);
        memcpy(out + CALL_AUDIO_HDR_SIZE, decoded, sizeof(decoded));
        f->expected_len += CALL_AUDIO_HDR_SIZE + sizeof(decoded);
    }
    ca_media_recv(&f->inst, TEST_CALL_ID, opus, len, f->ca);
}

/* Ждём дренажа сервера и всех реально переданных TCP-байтов, затем сравниваем весь поток. */
static void collect(struct queue_fixture* f) {
    uint64_t deadline = get_time_tb() + 20000;
    while (f->cli->send_len || f->wire_len < sent_bytes) {
        CHECK(get_time_tb() < deadline); uasync_poll(f->ua, 10);
        CHECK(f->wire_len < WIRE_CAP);
        ssize_t n = recv(f->peer, f->wire + f->wire_len, WIRE_CAP - f->wire_len, 0);
        if (n > 0) f->wire_len += (size_t)n;
        else CHECK(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }
    CHECK(!f->cli->send_len && !f->cli->send_offset);
    CHECK(f->wire_len == f->expected_len);
    CHECK(memcmp(f->wire, f->expected, f->wire_len) == 0);
}

/* Закрытие с непустой очередью тоже обязано освободить все ресурсы. */
static void fixture_stop(struct queue_fixture* f) {
    call_headless_destroy(&f->inst);
    socket_close_wrapper(f->peer); observed_fd = SOCKET_INVALID;
    opus_codec_encoder_destroy(f->encoder); opus_codec_decoder_destroy(f->reference);
    u_free(f->wire); u_free(f->expected);
    uasync_poll(f->ua, 0);
    CHECK(f->ua->timer_alloc_count == f->ua->timer_free_count);
    CHECK(f->ua->socket_alloc_count == f->ua->socket_free_count);
    uasync_destroy(f->ua, 0);
}

struct queue_case {
    const char* name;
    unsigned frames, retained;
    size_t first, chunk;
    int blocked, offset_overflow, destroy_pending, invalid;
};

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_DEBUG);
    debug_set_log_hook(capture_log);
    const struct queue_case cases[] = {
        {"normal", 8, 8, 0, 0, 0, 0, 0, 0},
        {"frame_boundary", 34, 33, 0, 0, 1, 0, 0, 0},
        {"overflow", 40, 33, 0, 0, 1, 0, 0, 0},
        {"partial_header", 40, 33, 5, 0, 0, 0, 0, 0},
        {"partial_pcm", 40, 33, 113, 0, 0, 0, 0, 0},
        {"exact_fit", 34, 34, 118, 0, 0, 0, 0, 0},
        {"exact_overflow", 35, 34, 118, 0, 0, 0, 0, 0},
        {"chunked_write", 8, 8, 5, 127, 0, 0, 0, 0},
        {"offset_overflow", 33, 33, 0, 0, 1, 1, 0, 0},
        {"destroy_pending", 8, 8, 0, 0, 1, 0, 1, 0},
        {"invalid_output", 0, 0, 0, 0, 0, 0, 0, 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const struct queue_case* c = &cases[i];
        size_t allocated = u_get_allocated_count();
        struct queue_fixture f; fixture_start(&f);
        blocked = c->blocked; first_short = c->first; write_chunk = c->chunk;
        if (c->invalid) {
            uint8_t byte = 0;
            ca_write_frame(f.cli, CALL_AUDIO_FRAME, TEST_CALL_ID, &byte, CA_PCM_FRAME_BYTES + 1);
            ca_write_frame(f.cli, CALL_AUDIO_FRAME, TEST_CALL_ID, NULL, CA_PCM_FRAME_BYTES);
            CHECK(!sent_bytes && !f.cli->send_len);
        }
        for (unsigned frame = 0; frame < c->frames; ++frame) emit_frame(&f, frame < c->retained);
        CHECK(sent_bytes + (size_t)(f.cli->send_len - f.cli->send_offset) == f.expected_len);
        CHECK(f.cli->send_len <= AUDIO_SEND_BUF_SIZE);
        if (c->first == 118) CHECK(f.cli->send_len == AUDIO_SEND_BUF_SIZE && !f.cli->send_offset);
        unsigned dropped = c->frames - c->retained;
        if (c->offset_overflow) {
            blocked = 0; first_short = 118;
            uint64_t deadline = get_time_tb() + 20000;
            while (first_short) { CHECK(get_time_tb() < deadline); uasync_poll(f.ua, 10); }
            CHECK(blocked && f.cli->send_offset == 118);
            int queued = f.cli->send_len;
            emit_frame(&f, 0); ++dropped;
            CHECK(f.cli->send_len == queued && f.cli->send_offset == 118);
        }
        if (!c->destroy_pending) {
            blocked = 0; collect(&f);
            emit_frame(&f, 1); collect(&f); /* Следующий кадр корректен и после пропусков/дренажа. */
        } else CHECK(!sent_bytes && f.cli->send_len > 0);
        size_t received = f.wire_len;
        fixture_stop(&f);
        CHECK(closed_drops == dropped && destroyed_drops == dropped);
        CHECK(errors == (c->invalid ? 2U : 0U));
        CHECK(dropped ? drop_warnings >= 1 && drop_warnings <= dropped : drop_warnings == 0);
        CHECK(u_get_allocated_count() == allocated);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "headless queue: %s PASS frames=%u retained=%u dropped=%u wire=%zu allocations=%zu->%zu",
                   c->name, c->frames, c->retained, dropped, received, allocated, u_get_allocated_count());
    }
    debug_set_log_hook(NULL);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "headless queue: 11/11 PASS");
    return 0;
}
