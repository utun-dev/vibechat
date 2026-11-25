/* Настоящее ядро, chat stop/start и Opus/JB; без устройств и подмены состояния voice-движка. */
#define main call_fixture_main
#include "../../../tests/test_call.c"
#undef main
#include "call/call_audio.h"
#include "opus_codec.h"

#define CHECK_AUDIO(value) do { if (!(value)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio lifecycle line=%d: %s", __LINE__, #value); return 1; \
} } while (0)

static int feed_frames(uint64_t id, unsigned count) {
    int16_t pcm[960] = {0}; uint8_t mask[960] = {0};
    unsigned accepted = 0;
    for (unsigned i = 0; i < count; ++i)
        accepted += call_audio_feed_prepared_pcm(id, pcm, mask, INT64_MIN) == 0;
    return (int)accepted;
}

int main(int argc, char** argv) {
    CHECK_AUDIO(argc == 2);
    int restart = !strcmp(argv[1], "restart"), detach = !strcmp(argv[1], "detach");
    int before_chat = !strcmp(argv[1], "before_chat"), idempotent = !strcmp(argv[1], "idempotent");
    CHECK_AUDIO(restart || detach || before_chat || idempotent);
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_DEBUG);
    utun_instance_set_tun_init_enabled(0);
    size_t allocated = u_get_allocated_count();
    char directory[512] = "/tmp/utun_audio_lifecycle_XXXXXX", config[600];
    CHECK_AUDIO(!test_mkdtemp(directory) && !test_setenv("UTUN_TEST_DIR", directory, 1));
    fill_shared(); write_configs(directory);
    struct UASYNC* ua = uasync_create(); CHECK_AUDIO(ua);
    snprintf(config, sizeof(config), "%s/a.conf", directory);
    struct UTUN_INSTANCE* inst = utun_instance_create(ua, config); CHECK_AUDIO(inst);
    CHECK_AUDIO(call_audio_start_prepared(42) == -1);
    if (before_chat) CHECK_AUDIO(!call_audio_init(inst));
    CHECK_AUDIO(!utun_instance_init(inst));
    if (!before_chat) CHECK_AUDIO(!call_audio_init(inst));
    const struct call_audio_ops* provider = inst->call_audio_ops;
    CHECK_AUDIO(provider && provider->on_media && !call_audio_start_prepared(42));
    CHECK_AUDIO(feed_frames(42, 8) == 8 && feed_frames(42, 1) == 0);
    call_audio_stop();
    if (restart || detach) {
        if (detach) call_audio_destroy(inst);
        chat_service_stop(inst);
        CHECK_AUDIO(inst->core_started && !inst->chat_started && !inst->call);
        CHECK_AUDIO(inst->call_audio_ops == (detach ? NULL : provider));
        CHECK_AUDIO(!chat_service_start(inst));
        if (detach) CHECK_AUDIO(!call_audio_init(inst));
        // После обычного restart повторного init нет: новый call_ctx уже использует сохранённый provider.
    } else {
        CHECK_AUDIO(!call_audio_init(inst) && !call_audio_init(inst));
    }
    CHECK_AUDIO(inst->call_audio_ops == provider && !call_audio_start_prepared(43));
    CHECK_AUDIO(feed_frames(43, 1) == (detach ? 1 : 0));
    if (detach) {
        call_audio_stop(); call_audio_destroy(inst);
        CHECK_AUDIO(!call_audio_init(inst) && !call_audio_start_prepared(43));
    }
    uasync_poll(ua, 0); uasync_poll(ua, 0);
    CHECK_AUDIO(feed_frames(43, 8) == 8 && feed_frames(43, 1) == 0);

    opus_codec_encoder_t* encoder = opus_codec_encoder_create(48000, 1); CHECK_AUDIO(encoder);
    int16_t pcm[960] = {0}; uint8_t packet[256];
    int length = opus_codec_encode(encoder, pcm, 960, packet, sizeof(packet)); CHECK_AUDIO(length > 0);
    struct UTUN_INSTANCE foreign = {0};
    call_audio_on_media(&foreign, 43, 0, 0, packet, length, NULL, 1);
    struct call_jitter_diagnostics diagnostic = {0};
    CHECK_AUDIO(!call_audio_get_diagnostics(43, &diagnostic) && diagnostic.encoded_ms == 0);
    for (unsigned i = 0; i < 3; ++i) provider->on_media(inst, 43, i, i * 20, packet, length, NULL, 1);
    CHECK_AUDIO(!call_audio_get_diagnostics(43, &diagnostic) && diagnostic.encoded_ms == 60);
    opus_codec_encoder_destroy(encoder);
    call_audio_stop(); call_audio_destroy(inst);
    CHECK_AUDIO(!inst->call_audio_ops && call_audio_start_prepared(44) == -1);
    utun_instance_destroy(inst); uasync_poll(ua, 0); uasync_destroy(ua, 0);
    CHECK_AUDIO(u_get_allocated_count() == allocated);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio lifecycle mode=%s received_ms=60 TX_slots=8 allocations=%zu->%zu PASS",
               argv[1], allocated, u_get_allocated_count());
    return 0;
}
