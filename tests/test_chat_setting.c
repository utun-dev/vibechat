/* Чистое чтение настроек, независимость копий и различие validation/apply-отказов. */
#include <assert.h>
#include <string.h>
#include "../src/chat/chat_setting.h"
#include "../src/utun_instance.h"
#include "../src/stream/stream.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

static int fail_apply;
static unsigned applied;
extern int __real_chat_local_apply_settings(struct UTUN_INSTANCE*);
int __wrap_chat_local_apply_settings(struct UTUN_INSTANCE* inst) {
    applied++;
    if (fail_apply) { fail_apply = 0; return -1; }
    return __real_chat_local_apply_settings(inst);
}

static void test_reads(void) {
    struct chat_setting_state state = {0};
    assert(!chat_setting_get_state(&state, "stream_subblocks"));
    assert(chat_setting_get_int_state(&state, "stream_subblocks", 77) == 77);
    assert(!strcmp(chat_setting_get_state(&state, "call_video_bitrate_range_kbps"), "300:4000"));
    assert(!chat_setting_set_state(&state, "stream_subblocks", "8"));
    assert(chat_setting_get_int_state(&state, "radio_burst_frames", 0) == 4);
    assert(chat_setting_get_int_state(&state, "noise_suppression_enabled", 0) == 1);
    assert(chat_setting_get_int_state(&state, "noise_suppression_db", 0) == -20);
    assert(chat_setting_get_int_state(&state, "stream_stall_timeout_ms", 0) == 500);
    assert(chat_setting_get_int_state(&state, "stream_max_live_lag_ms", 0) == 10000);
    struct chat_setting_state before = state;
    const char* blocks = chat_setting_get_state(&state, "stream_subblocks");
    const char* bitrate = chat_setting_get_state(&state, "radio_opus_bitrate");
    assert(!strcmp(blocks, "8") && !strcmp(bitrate, "32") && blocks != bitrate);
    assert(!memcmp(&before, &state, sizeof(state)));
    assert(!chat_setting_get_state(&state, NULL) && !chat_setting_get_state(&state, "unknown"));
    assert(chat_setting_get_int_state(&state, NULL, 77) == 77);
    assert(chat_setting_get_int_state(&state, "member_files_path", 77) == 77);
    assert(!chat_setting_get_state(NULL, "stream_subblocks") && chat_setting_get_int_state(NULL, NULL, 77) == 77);
    assert(!chat_setting_set_state(&state, "radio_opus_bitrate", "48") && !strcmp(blocks, "8") && !strcmp(bitrate, "48"));
    struct chat_setting_state copy = state;
    assert(!chat_setting_set_state(&copy, "stream_subblocks", "12"));
    assert(!strcmp(blocks, "8") && !strcmp(chat_setting_get_state(&copy, "stream_subblocks"), "12"));
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS settings reads: const state unchanged, distinct stable values, NULL/type defaults, independent copied state");
}

static void test_validation(void) {
    const char* names[] = {"unknown", "stream_subblocks", "stream_subblocks", "stream_subblocks", "stream_subblocks",
        "stream_subblocks", "stream_subblocks", "local_invites_enabled", "call_video_bitrate_range_kbps", "", NULL};
    const char* values[] = {"1", "0", "256", "", "  ", "8x", "99999999999999999999999999999", "maybe", "4000:300", "1", "1"};
    for (unsigned initialized = 0; initialized < 2; initialized++) {
        struct chat_setting_state state = {0};
        if (initialized) assert(!chat_setting_set_state(&state, "stream_subblocks", "9"));
        const struct chat_setting_state before = state;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            assert(chat_setting_set_state(&state, names[i], values[i]) == (i == 0 ? -1 : -2));
            assert(!memcmp(&before, &state, sizeof(state)));
        }
        assert(chat_setting_set_state(&state, "stream_subblocks", NULL) == -2);
        assert(!memcmp(&before, &state, sizeof(state)));
        char oversized[257]; memset(oversized, 'a', sizeof(oversized)); oversized[256] = 0;
        assert(chat_setting_set_state(&state, "member_files_path", oversized) == -2);
        assert(!memcmp(&before, &state, sizeof(state)));
    }
    struct chat_setting_state state = {0};
    const char* booleans[] = {"0", "no", "false", "1", "yes", "true"};
    for (size_t i = 0; i < sizeof(booleans) / sizeof(booleans[0]); i++) {
        assert(!chat_setting_set_state(&state, "local_invites_enabled", booleans[i]));
        assert(chat_setting_get_int_state(&state, "local_invites_enabled", 77) == (i >= 3));
    }
    char maximum[256]; memset(maximum, 'b', sizeof(maximum)); maximum[255] = 0;
    assert(!chat_setting_set_state(&state, "member_files_path", maximum));
    assert(!strcmp(chat_setting_get_state(&state, "member_files_path"), maximum));
    assert(!chat_setting_set_state(&state, "member_files_path", ""));
    assert(!strcmp(chat_setting_get_state(&state, "member_files_path"), ""));
    assert(!chat_setting_set_state(&state, "radio_level_on_dbfs", "-90"));
    assert(!strcmp(chat_setting_get_state(&state, "radio_level_on_dbfs"), "-90"));
    assert(!chat_setting_set_state(&state, "radio_burst_frames", "1"));
    assert(!chat_setting_set_state(&state, "radio_burst_frames", "10"));
    assert(chat_setting_set_state(&state, "radio_burst_frames", "0") == -2);
    assert(chat_setting_set_state(&state, "radio_burst_frames", "11") == -2);
    assert(chat_setting_get_int_state(&state, "radio_burst_frames", 0) == 10);
    assert(!chat_setting_set_state(&state, "noise_suppression_db", "-40"));
    assert(!chat_setting_set_state(&state, "noise_suppression_db", "-1"));
    assert(chat_setting_set_state(&state, "noise_suppression_db", "0") == -2);
    assert(chat_setting_set_state(&state, "noise_suppression_db", "-41") == -2);
    assert(chat_setting_get_int_state(&state, "noise_suppression_db", 0) == -1);
    assert(!chat_setting_set_state(&state, "noise_suppression_enabled", "0"));
    assert(chat_setting_get_int_state(&state, "noise_suppression_enabled", -1) == 0);
    assert(!chat_setting_set_state(&state, "stream_stall_timeout_ms", "100"));
    assert(!chat_setting_set_state(&state, "stream_stall_timeout_ms", "60000"));
    assert(chat_setting_set_state(&state, "stream_stall_timeout_ms", "99") == -2);
    assert(chat_setting_set_state(&state, "stream_stall_timeout_ms", "60001") == -2);
    assert(chat_setting_get_int_state(&state, "stream_stall_timeout_ms", 0) == 60000);
    assert(!chat_setting_set_state(&state, "stream_max_live_lag_ms", "1000"));
    assert(!chat_setting_set_state(&state, "stream_max_live_lag_ms", "60000"));
    assert(chat_setting_set_state(&state, "stream_max_live_lag_ms", "999") == -2);
    assert(chat_setting_set_state(&state, "stream_max_live_lag_ms", "60001") == -2);
    assert(chat_setting_get_int_state(&state, "stream_max_live_lag_ms", 0) == 60000);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS settings validation: invalid values preserve zero/initialized state, bool aliases, signed range,255-byte path and clearing");
}

static void test_runtime_apply(void) {
    struct UTUN_INSTANCE inst; memset(&inst, 0, sizeof(inst));
    fail_apply = 1;
    assert(chat_setting_set(&inst, "local_broadcasts_enabled", "0") == -3 && applied == 1 && !fail_apply);
    assert(chat_setting_get_int(&inst, "local_broadcasts_enabled", 77) == 0);
    assert(!chat_setting_set(&inst, "local_broadcasts_enabled", "0") && applied == 2);
    assert(chat_setting_set(&inst, "local_broadcasts_enabled", "invalid") == -2 && applied == 2);
    assert(!chat_setting_set(&inst, "local_invites_enabled", "1") && applied == 3);
    assert(chat_setting_set(NULL, "local_invites_enabled", "1") == -2);
    assert(!chat_setting_get(NULL, "stream_subblocks") && chat_setting_get_int(NULL, NULL, 77) == 77);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS settings apply: desired value survives LAN failure, distinct error, identical retry reapplies, validation does not apply");
}

static void test_stream_limits(void) {
    struct UTUN_INSTANCE inst = {0}; struct stream_limits limits;
    stream_limits_get(&inst, &limits);
    assert(limits.regular == 192 && limits.temporary == 64 && limits.background == 16 && limits.peer_temporary == 16);
    assert(limits.peers == 64 && limits.control == 128 && limits.extra_receive == 16);
    assert(!chat_setting_set(&inst, "stream_regular_limit", "256"));
    assert(!chat_setting_set(&inst, "stream_temporary_limit", "4"));
    assert(!chat_setting_set(&inst, "stream_background_limit", "8"));
    assert(!chat_setting_set(&inst, "stream_peer_temporary_limit", "6"));
    stream_limits_get(&inst, &limits);
    assert(limits.regular == 256 && limits.temporary == 4 && limits.background == 4 && limits.peer_temporary == 4);
    assert(!chat_setting_set(&inst, "stream_temporary_limit", "64"));
    stream_limits_get(&inst, &limits); assert(limits.background == 8 && limits.peer_temporary == 6);
    assert(chat_setting_set(&inst, "stream_regular_limit", "0") == -2);
    assert(chat_setting_set(&inst, "stream_regular_limit", "4097") == -2);
    assert(chat_setting_set(&inst, "stream_temporary_limit", "1025") == -2);
    assert(chat_setting_set(&inst, "stream_control_limit", "7") == -2);
    assert(!chat_setting_set(&inst, "stream_extra_receive_limit", "0"));
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS stream budgets: defaults, live settings, child limits independent of INI order, invalid values rejected");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); size_t baseline = u_get_allocated_count();
    test_reads(); test_validation(); test_runtime_apply(); test_stream_limits();
    assert(u_get_allocated_count() == baseline); return 0;
}
