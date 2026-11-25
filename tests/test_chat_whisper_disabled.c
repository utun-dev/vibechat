/* Отключённая сборка не должна возвращать ложный успех при повторном запросе. */
#include "../src/chat/chat_whisper.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_setting.h"
#include "../src/utun_instance.h"
#include "../src/media_async/media_async.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include <assert.h>

static unsigned calls;
static void completed(void* arg, const char* channel, const char* text, int error, const uint8_t id[32]) {
    (void)arg; assert(!strcmp(channel, "1") && !text && error == -1 && id); calls++;
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); size_t baseline = u_get_allocated_count();
    struct chat_core_ctx cc = {.initialized = 1}; struct UTUN_INSTANCE inst = {.chat_core = &cc, .chat_started = 1};
    inst.ua = uasync_create(); inst.media_async = media_async_create(); assert(inst.ua && inst.media_async);
    assert(!chat_setting_set(&inst, "whisper_enabled", "1")); uint8_t id[32] = {0};
    for (unsigned i = 0; i < 3; i++) chat_whisper_transcribe(&inst, "unused", "1", id, completed, NULL);
    assert(calls == 3 && !cc.whisper); chat_whisper_destroy(&inst); chat_whisper_destroy(&inst);
    media_async_destroy(inst.media_async); uasync_destroy(inst.ua, 0); assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "PASS whisper disabled:3 retries,3 failures,no owner/leak"); return 0;
}
