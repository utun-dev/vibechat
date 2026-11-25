/*
 * chat_setting.c — chat-related settings: validation, storage, on-the-fly apply
 *
 * Используется и из парсера конфига (headless), и из GUI (через bridge).
 * Состояние per-instance (struct chat_setting_state).
 */

#include "chat_setting.h"
#include "chat_local.h"
#include "../call/call.h"
#include "../call/call_video_rate.h"
#include "../stream/stream.h"
#include "../utun_instance.h"
#include "../../lib/debug_config.h"
#include "../../lib/audio_compressor.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define CS_ID DEBUG_CATEGORY_CHAT_SYNC

/* ─── типы настроек ─── */

#define CHAT_SETTING_INT    0
#define CHAT_SETTING_BOOL   1
#define CHAT_SETTING_STRING 2

struct chat_setting_def {
    const char* name;
    int         type;
    int         default_val;
    int         min_val;
    int         max_val;
};

/* ─── реестр всех известных настроек ─── */

static const struct chat_setting_def g_setting_defs[] = {
    {"stream_subblocks",           CHAT_SETTING_INT,  8,   1, 255},
    {"stream_rs_redundant",        CHAT_SETTING_INT,  0,   0, 254},
    {"stream_stall_timeout_ms",    CHAT_SETTING_INT, 500, 100, 60000},
    {"stream_max_live_lag_ms",     CHAT_SETTING_INT, 10000, 1000, 60000},
    {"stream_regular_limit",      CHAT_SETTING_INT, STREAM_REGULAR_LIMIT, 1, 4096},
    {"stream_temporary_limit",    CHAT_SETTING_INT, STREAM_TEMP_LIMIT, 1, 1024},
    {"stream_background_limit",   CHAT_SETTING_INT, STREAM_BACKGROUND_LIMIT, 1, 1024},
    {"stream_peer_temporary_limit", CHAT_SETTING_INT, STREAM_PEER_TEMP_LIMIT, 1, 1024},
    {"stream_peer_limit",         CHAT_SETTING_INT, STREAM_PEER_LIMIT, 1, 1024},
    {"stream_control_limit",      CHAT_SETTING_INT, STREAM_CONTROL_LIMIT, 8, 4096},
    {"stream_extra_receive_limit", CHAT_SETTING_INT, STREAM_EXTRA_RECEIVE_LIMIT, 0, 254},
    {"storage_autoload",            CHAT_SETTING_BOOL, 1,   0, 1},
    {"opus_codec_preset",           CHAT_SETTING_INT,  1,   0, 2},
    {"radio_opus_mode",             CHAT_SETTING_INT,  0,   0, 1},
    {"radio_opus_bitrate",          CHAT_SETTING_INT,  32, 16, 128},
    {"radio_burst_frames",          CHAT_SETTING_INT,  4,   1, 10}, /* Opus-кадров в пакете; со следующего PTT/VAD BEGIN. */
    {"radio_capture_stereo",        CHAT_SETTING_INT,  0,   0, 1},
    {"compressor_enabled",          CHAT_SETTING_BOOL, 1,   0, 1},
    {"compressor_max_gain_db",      CHAT_SETTING_INT,  20,  0, 60},
    {"compressor_rise_rate",        CHAT_SETTING_INT,  2,   1, 100},
    {"radio_compressor_enabled",    CHAT_SETTING_BOOL, 1,   0, 1},
    {"radio_aec_enabled",           CHAT_SETTING_BOOL, 1,   0, 1}, /* Независимое эхоподавление рации; raw PCM или desktop VoiceAudioIo. */
    {"radio_compressor_max_gain_db",CHAT_SETTING_INT,  20,  1, 60},
    {"radio_auto_mode",             CHAT_SETTING_INT,  1,   1, 3}, /* 1=VAD, 2=уровень, 3=VAD И уровень */
    {"radio_level_on_dbfs",         CHAT_SETTING_INT, -35, -90, 0},
    {"radio_level_off_dbfs",        CHAT_SETTING_INT, -45, -90, 0},
    {"radio_auto_enabled",           CHAT_SETTING_BOOL, 0,   0, 1},
    {"radio_vad_compressor_max_gain_db", CHAT_SETTING_INT, 10, 1, 60},
    {"radio_vad_threshold",         CHAT_SETTING_INT,  50, 10, 95},   /* порог вероятности речи, % */
    {"radio_auto_hangover_ms",       CHAT_SETTING_INT,  200, 50, 2000},/* тишина до авто-отбоя */
    {"media_download_max_peers",    CHAT_SETTING_INT,  3,   1, 16},
    {"storage_backfill_days",       CHAT_SETTING_INT,  7,   1, 365},
    {"dm_media_ttl_sec",            CHAT_SETTING_INT, 604800, 1, 31536000}, /* TTL custody M: 7 суток */
    {"dm_media_storage_mb",         CHAT_SETTING_INT, 1024, 1, 1048576}, /* включая зарезервированные загрузки */
    {"dm_media_delete_on_open",     CHAT_SETTING_BOOL, 1, 0, 1}, /* 0: до загрузки всеми адресатами; 1: до открытия */
    {"standby_active_sec",         CHAT_SETTING_INT,  2,   1, 3600},
    {"standby_sleep_sec",          CHAT_SETTING_INT,  60,  1, 86400},
    {"standby_min_sleep_sec",      CHAT_SETTING_INT,  15,  1, 86400},
    {"standby_keepalive_ms",       CHAT_SETTING_INT,  30000, 1000, 600000},
    {"join_policy",                CHAT_SETTING_INT,  1,   0, 2},  /* 0=autojoin, 1=ask, 2=deny */
    {"whisper_enabled",            CHAT_SETTING_BOOL, 0,   0, 1},
    {"whisper_model_path",         CHAT_SETTING_STRING, 0, 0, 0},
    {"group_autoconnect",          CHAT_SETTING_BOOL, 1,   0, 1},
    {"aec_enabled",                CHAT_SETTING_BOOL, 1,   0, 1},  /* AEC (SpeexDSP) в звонке, desktop GUI */
    {"noise_suppression_enabled",  CHAT_SETTING_BOOL, 1,   0, 1},  /* Выключение полностью обходит SpeexDSP NS. */
    {"noise_suppression_db",       CHAT_SETTING_INT, -20, -40, -1},
    {"call_jitter_max_reserve_ms", CHAT_SETTING_INT, 1000, 500, 2000},
    {"incoming_call_policy",      CHAT_SETTING_INT, 1, 0, 31}, /* CALL_POLICY_*; публикация по умолчанию разрешена */
    {"call_video_bitrate_range_kbps", CHAT_SETTING_STRING, 0, 0, 0},
    {"call_compressor_enabled",    CHAT_SETTING_BOOL, 1,   0, 1},
    {"call_compressor_max_gain_db",CHAT_SETTING_INT,  20,  1, 60},
    {"member_files_enabled",       CHAT_SETTING_BOOL, 1,   0, 1},
    {"member_files_path",          CHAT_SETTING_STRING, 0, 0, 0},
    {"member_files_user_quota_mb", CHAT_SETTING_INT, 1024, 1, 1048576},
    {"member_files_upload_ttl_sec", CHAT_SETTING_INT, 3600, 60, 604800},
    {"bluetooth_invites_enabled",   CHAT_SETTING_BOOL, 0,   0, 1},
    {"local_invites_enabled",       CHAT_SETTING_BOOL, 1,   0, 1},
    {"local_broadcasts_enabled",    CHAT_SETTING_BOOL, 1,   0, 1},
};

#define CHAT_SETTING_COUNT (sizeof(g_setting_defs) / sizeof(g_setting_defs[0]))
typedef char chat_setting_capacity_check[CHAT_SETTING_COUNT <= CHAT_SETTING_MAX_SETTINGS ? 1 : -1];

/* ─── state helpers ─── */

static void chat_setting_reset_defaults(struct chat_setting_state* s) {
    memset(s, 0, sizeof(*s));
    for (size_t i = 0; i < CHAT_SETTING_COUNT; i++) {
        s->values[i] = g_setting_defs[i].default_val;
        snprintf(s->int_text[i], sizeof(s->int_text[i]), "%d", s->values[i]);
    }
    s->initialized = 1;
}

static int chat_setting_find(const char* name) {
    if (!name) return -1;
    for (size_t i = 0; i < CHAT_SETTING_COUNT; i++)
        if (strcmp(g_setting_defs[i].name, name) == 0) return (int)i;
    return -1;
}

/* ─── state-level API ─── */

int chat_setting_set_state(struct chat_setting_state* s, const char* name, const char* value) {
    if (!s || !name || !value || !*name) {
        DEBUG_ERROR(CS_ID, "chat_setting: invalid state/name/value argument"); return -2;
    }

    int idx = chat_setting_find(name);
    if (idx < 0) {
        DEBUG_ERROR(CS_ID, "chat_setting: unknown setting '%s'", name);
        return -1;
    }

    const struct chat_setting_def* def = &g_setting_defs[idx];
    int v;

    if (def->type == CHAT_SETTING_STRING) {
        if (strcmp(name, "call_video_bitrate_range_kbps") == 0) {
            uint32_t minimum, maximum;
            if (call_video_rate_parse_range(value, &minimum, &maximum)) {
                DEBUG_ERROR(CS_ID, "chat_setting: '%s' invalid bitrate range '%s'", name, value); return -2;
            }
        }
        if (strlen(value) >= sizeof(s->string_val[0])) {
            DEBUG_ERROR(CS_ID, "chat_setting: '%s' string too long bytes=%zu limit=%zu", name, strlen(value), sizeof(s->string_val[0]) - 1);
            return -2;
        }
        if (!s->initialized) chat_setting_reset_defaults(s);
        for (int i = 0; i < s->string_count; i++) {
            if (s->string_idx[i] == idx) {
                if (strcmp(s->string_val[i], value) == 0) return 0;
                snprintf(s->string_val[i], sizeof(s->string_val[i]), "%s", value);
                DEBUG_INFO(CS_ID, "chat_setting: %s = %s", name, value);
                return 0;
            }
        }
        if (s->string_count >= CHAT_SETTING_STRING_MAX) {
            DEBUG_ERROR(CS_ID, "chat_setting: string storage full for '%s'", name);
            return -2;
        }
        s->string_idx[s->string_count] = idx;
        snprintf(s->string_val[s->string_count], sizeof(s->string_val[0]), "%s", value);
        s->string_count++;
        DEBUG_INFO(CS_ID, "chat_setting: %s = %s", name, value);
        return 0;
    }

    if (def->type == CHAT_SETTING_BOOL) {
        if (strcmp(value, "0") == 0 || strcmp(value, "no") == 0 || strcmp(value, "false") == 0) v = 0;
        else if (strcmp(value, "1") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "true") == 0) v = 1;
        else { DEBUG_ERROR(CS_ID, "chat_setting: '%s' invalid bool '%s' (expect 0/1)", name, value); return -2; }
    } else {
        char* end = NULL;
        long lv = strtol(value, &end, 10);
        if (end == value || *end != '\0' || lv < def->min_val || lv > def->max_val) {
            DEBUG_ERROR(CS_ID, "chat_setting: '%s' invalid int '%s' (range %d..%d)", name, value, def->min_val, def->max_val);
            return -2;
        }
        v = (int)lv;
    }

    if (!s->initialized) chat_setting_reset_defaults(s);
    if (s->values[idx] == v) return 0;

    s->values[idx] = v;
    snprintf(s->int_text[idx], sizeof(s->int_text[idx]), "%d", v);
    DEBUG_INFO(CS_ID, "chat_setting: %s = %s", name, value);
    return 0;
}

const char* chat_setting_get_state(const struct chat_setting_state* s, const char* name) {
    if (s && name && strcmp(name, "call_video_bitrate_range_kbps") == 0) {
        int idx = chat_setting_find(name);
        for (int i = 0; s->initialized && i < s->string_count; i++)
            if (s->string_idx[i] == idx) return s->string_val[i];
        return CALL_VIDEO_RATE_DEFAULT_RANGE;
    }
    if (!s || !s->initialized) return NULL;
    int idx = chat_setting_find(name);
    if (idx < 0) return NULL;

    const struct chat_setting_def* def = &g_setting_defs[idx];
    if (def->type == CHAT_SETTING_STRING) {
        for (int i = 0; i < s->string_count; i++) {
            if (s->string_idx[i] == idx) return s->string_val[i];
        }
        return "";
    }

    return s->int_text[idx];
}

int chat_setting_get_int_state(const struct chat_setting_state* s, const char* name, int def) {
    if (!s || !s->initialized) return def;
    int idx = chat_setting_find(name);
    return idx >= 0 && g_setting_defs[idx].type != CHAT_SETTING_STRING ? s->values[idx] : def;
}

/* ─── instance-level API ─── */

static struct chat_setting_state* cs_state(struct UTUN_INSTANCE* inst) {
    return inst ? &inst->chat_settings : NULL;
}

int chat_setting_set(struct UTUN_INSTANCE* inst, const char* name, const char* value) {
    struct chat_setting_state* s = cs_state(inst);
    if (!s) { DEBUG_ERROR(CS_ID, "chat_setting: instance is NULL"); return -2; }
    int result = chat_setting_set_state(s, name, value);
    if (!result && !strcmp(name, "radio_vad_compressor_max_gain_db") &&
        audio_compressor_set_vad_max_gain(chat_setting_get_int(inst, name, 10))) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "chat_setting: VAD max gain apply failed"); return -3;
    }
    if (!result && !strcmp(name, "opus_codec_preset") && inst->call_audio_ops && inst->call_audio_ops->set_codec_preset &&
        inst->call_audio_ops->set_codec_preset(inst, chat_setting_get_int(inst, name, 1))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "chat_setting: '%s' stored but codec apply failed; retry setting to reapply", name); return -3;
    }
    if (!result && !strcmp(name, "incoming_call_policy"))
        call_set_phone_state(inst, inst->phone_state, (unsigned)chat_setting_get_int(inst, name, CALL_POLICY_PUBLISH_PHONE));
    if (!result && (!strcmp(name, "local_invites_enabled") || !strcmp(name, "local_broadcasts_enabled")) &&
        chat_local_apply_settings(inst)) {
        DEBUG_ERROR(CS_ID, "chat_setting: '%s' stored but LAN apply failed; retry setting to reapply", name); return -3;
    }
    return result;
}

const char* chat_setting_get(struct UTUN_INSTANCE* inst, const char* name) {
    struct chat_setting_state* s = cs_state(inst);
    return s ? chat_setting_get_state(s, name) : NULL;
}

int chat_setting_get_int(struct UTUN_INSTANCE* inst, const char* name, int def) {
    struct chat_setting_state* s = cs_state(inst);
    return s ? chat_setting_get_int_state(s, name, def) : def;
}
