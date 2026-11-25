/* JNI live media: session IDs для ядра. Playback DSP принадлежит audio worker; mixer берёт готовый PCM.
 * Только output map/clock сериализуются StreamMixedPlayback lock; release выполняет audio worker после detach. */
#ifdef __ANDROID__
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "../../../lib/debug_config.h"
#include "../../../lib/mem.h"
#include "../../../lib/strbuf.h"
#include "../../../lib/opus_codec.h"
#include "../../../lib/audio_compressor.h"
#include "../libutun_lite/stream_bridge.h"
#include "../../../src/stream/stream_media.h"
#include "../../../src/stream/stream_media_buffer.h"
#include "../../../src/stream/stream_viewer.h"
#include "../../../src/stream/stream_playback_clock.h"
#include "../../../src/stream/stream_playback_audio.h"

enum { STREAM_AUDIO_CHANNELS = 2, STREAM_AUDIO_FRAMES = 960, STREAM_AUDIO_SAMPLES = 1920 };

/* Kotlin использует тот же фильтр video и log hook, что и native-диагностика. */
JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_videoLog(JNIEnv *env, jobject obj, jboolean trace, jstring message) {
    (void)obj;
    debug_level_t level = trace ? DEBUG_LEVEL_TRACE : DEBUG_LEVEL_DEBUG;
    if (!debug_should_output(level, DEBUG_CATEGORY_VIDEO)) return;
    if (!message) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video log missing message"); return; }
    const char *text = (*env)->GetStringUTFChars(env, message, NULL);
    if (!text) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video log string access failed"); return; }
    if (trace) { DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "%s", text); }
    else { DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "%s", text); }
    (*env)->ReleaseStringUTFChars(env, message, text);
}

struct android_stream_opus {
    opus_codec_encoder_t *encoder;
    opus_codec_decoder_t *decoder;
    struct audio_compressor *compressor;
    int compressor_enabled, max_gain_db, rise_rate;
    uint64_t frames;
    uint32_t reported_clips;
};

static int read_stream_group(JNIEnv *env, jbyteArray group, struct stream_address *key) {
    if (!group || (*env)->GetArrayLength(env, group) != SC_PUBKEY_SIZE) return -1;
    (*env)->GetByteArrayRegion(env, group, 0, SC_PUBKEY_SIZE, (jbyte*)key->ed);
    return (*env)->ExceptionCheck(env) || !stream_address_set(*key) ? -1 : 0;
}
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamOpen(
    JNIEnv *env, jobject obj, jlong id, jbyteArray group, jbyteArray source, jint width, jint height, jint rotation, jboolean audio, jlong epoch, jint codec) {
    (void)obj;
    struct stream_address address = {{0}}, group_key;
    if (read_stream_group(env, group, &group_key)) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "Android invalid stream group"); return JNI_FALSE; }
    if (source) {
        if ((*env)->GetArrayLength(env, source) != SC_PUBKEY_SIZE) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android stream source length invalid"); return JNI_FALSE;
        }
        (*env)->GetByteArrayRegion(env, source, 0, SC_PUBKEY_SIZE, (jbyte*)address.ed);
        if ((*env)->ExceptionCheck(env) || !stream_address_set(address)) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android stream source key unavailable"); return JNI_FALSE;
        }
    }
    return android_stream_open((uint64_t)id, group_key, address, width, height, rotation, audio, (uint64_t)epoch, codec) == 0;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamWatch(JNIEnv *env, jobject obj, jbyteArray group) {
    (void)obj;
    struct stream_address key = {{0}};
    if (group && read_stream_group(env, group, &key)) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "Android invalid stream watch group"); return JNI_FALSE; }
    return android_stream_watch(key) == 0;
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamFeed(
    JNIEnv *env, jobject obj, jlong id, jboolean video, jlong pts, jboolean key, jbyteArray data,
    jlong revision, jint width, jint height, jint rotation, jboolean audio, jint video_codec) {
    (void)obj;
    if (!data || pts < 0 || (video && (revision <= 0 || revision > UINT32_MAX || width <= 0 || width > 4096
        || height <= 0 || height > 4096 || rotation < 0 || rotation > 270 || rotation % 90))) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream feed missing data/invalid PTS"); return -1; }
    jsize length = (*env)->GetArrayLength(env, data);
    jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);
    if (!bytes) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream feed array access failed"); return -1; }
    struct stream_media_format format = {(uint16_t)width, (uint16_t)height, (uint16_t)rotation,
        (uint8_t)video_codec, audio ? STREAM_MEDIA_OPUS_STEREO : 0, (uint32_t)revision};
    if (video && (video_codec < STREAM_MEDIA_H264 || video_codec > STREAM_MEDIA_H266)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI video codec invalid id=%d", video_codec);
        (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT); return -1;
    }
    int result = android_stream_feed((uint64_t)id, video, (uint64_t)pts, key, (const uint8_t *)bytes, (size_t)length, video ? &format : NULL);
    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
    return result;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamStop(JNIEnv *env, jobject obj, jlong id) {
    (void)env; (void)obj; android_stream_stop((uint64_t)id);
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamSetAudioEnabled(
    JNIEnv *env, jobject obj, jlong id, jboolean enabled) {
    (void)env; (void)obj;
    return android_stream_set_audio_enabled((uint64_t)id, enabled != JNI_FALSE) == 0;
}

JNIEXPORT jlongArray JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamTxStats(JNIEnv *env, jobject obj, jlong id) {
    (void)obj; struct android_stream_tx_stats s;
    if (android_stream_tx_snapshot((uint64_t)id, &s) < 0) return NULL;
    jlong values[15 + STREAM_BUFFER_POSITION_PEERS * 7] = {s.sample_us, s.video_bytes, s.audio_bytes, s.stream_bytes, s.transport_bytes,
        s.inflight_bytes, s.acked_1s, s.total_bytes, s.connections, s.buffer.first, s.buffer.head, s.buffer.total, s.buffer.count,
        s.source_overloaded, s.video_bitrate};
    for (unsigned i = 0; i < s.buffer.count; i++) {
        unsigned offset = 15 + i * 7;
        for (unsigned j = 0; j < 4; j++) values[offset + j] = (jlong)stream_read_be(s.buffer.peers[i].peer.ed + j * 8, 8);
        values[offset + 4] = s.buffer.peers[i].worst;
        values[offset + 5] = s.buffer.peers[i].best;
        values[offset + 6] = s.buffer.peers[i].subflows;
    }
    jsize count = 15 + s.buffer.count * 7;
    jlongArray result = (*env)->NewLongArray(env, count);
    if (result) (*env)->SetLongArrayRegion(env, result, 0, count, values);
    else DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI TX stats allocation failed");
    return result;
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamVideoRate(
    JNIEnv *env, jobject obj, jlong id, jboolean automatic, jint nominal, jint minimum) {
    (void)env; (void)obj;
    return android_stream_video_rate((uint64_t)id, automatic != JNI_FALSE, nominal, minimum);
}

JNIEXPORT jbyteArray JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPull(
    JNIEnv *env, jobject obj, jlong id, jlong position, jint video, jlong audio_ahead, jint records) {
    (void)obj;
    if (android_stream_read_request((uint64_t)id, position, (unsigned)video, audio_ahead, (unsigned)records) < 0) return NULL;
    size_t length = 0; uint8_t *data = android_stream_pull((uint64_t)id, &length);
    if (!data) return NULL;
    jbyteArray output = (*env)->NewByteArray(env, (jsize)length);
    if (output) (*env)->SetByteArrayRegion(env, output, 0, (jsize)length, (const jbyte *)data);
    else DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream pull array allocation failed bytes=%zu", length);
    u_free(data); return output;
}

JNIEXPORT jlongArray JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamStatus(JNIEnv *env, jobject obj, jlong id) {
    (void)obj;
    struct stream_media_status status;
    if (android_stream_get_status((uint64_t)id, &status) < 0) return NULL;
    jlong values[] = {status.ready_us, status.video_us, status.audio_us, status.start_us, (jlong)status.generation,
        status.initialized | (status.audio_enabled << 1) | (status.finished << 2) | (status.video_ended << 3) | (status.audio_ended << 4)};
    jlongArray result = (*env)->NewLongArray(env, 6);
    if (result) (*env)->SetLongArrayRegion(env, result, 0, 6, values);
    else DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI media status allocation failed");
    return result;
}

/* UTF-8 передаётся байтами: имена с supplementary Unicode не являются modified UTF-8 JNI. */
JNIEXPORT jbyteArray JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamTelemetry(JNIEnv *env, jobject obj, jlong id) {
    (void)obj;
    struct stream_viewer_telemetry t;
    if (android_stream_get_telemetry((uint64_t)id, &t) < 0) return NULL;
    struct strbuf json = strbuf_new(); int ok = strbuf_addf(&json, "{\"self\":") >= 0;
    if (ok) ok = strbuf_add_json_string(&json, t.names[t.chain.count]) >= 0;
    if (ok) ok = strbuf_addf(&json, ",\"lag_ms\":%llu,\"lag_valid\":%s,\"waiting_live\":%s,\"nodes\":[",
        (unsigned long long)t.live_lag_ms, t.live_lag_valid ? "true" : "false", t.live_waiting_key ? "true" : "false") >= 0;
    for (unsigned i = 0; ok && i < t.chain.count; i++) {
        const struct stream_telemetry_row *r = &t.chain.rows[i];
        ok = strbuf_addf(&json, "%s{\"name\":", i ? "," : "") >= 0
            && strbuf_add_json_string(&json, t.names[i]) >= 0
            && strbuf_addf(&json, ",\"us\":%llu,\"frames\":%u,\"flags\":%u,\"age\":%u}",
                (unsigned long long)r->buffered_us, r->frames, r->flags, r->age_ms) >= 0;
    }
    if (ok) ok = strbuf_addf(&json, "]}") >= 0;
    char *data = ok ? strbuf_detach(&json) : NULL; strbuf_free(&json);
    if (!data) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream telemetry JSON allocation failed"); return NULL; }
    size_t length = strlen(data);
    jbyteArray result = (*env)->NewByteArray(env, (jsize)length);
    if (result) (*env)->SetByteArrayRegion(env, result, 0, (jsize)length, (const jbyte *)data);
    else DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream telemetry array allocation failed bytes=%zu", length);
    u_free(data); return result;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamResync(JNIEnv *env, jobject obj, jlong id) {
    (void)env; (void)obj; return android_stream_resync((uint64_t)id) == 0;
}

JNIEXPORT jlong JNICALL Java_com_utun_chat_data_NativeLib_streamSignalCreate(JNIEnv *env, jobject obj, jlong id) {
    (void)env; (void)obj; return (jlong)(intptr_t)android_stream_signal_create((uint64_t)id);
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_streamSignalNotify(JNIEnv *env, jobject obj, jlong handle) {
    (void)env; (void)obj; android_stream_signal_notify((struct android_stream_signal *)(intptr_t)handle);
}
JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_streamSignalWait(JNIEnv *env, jobject obj, jlong handle, jint ms) {
    (void)env; (void)obj; return android_stream_signal_wait((struct android_stream_signal *)(intptr_t)handle, (unsigned)ms);
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_streamSignalClose(JNIEnv *env, jobject obj, jlong handle) {
    (void)env; (void)obj; android_stream_signal_destroy((struct android_stream_signal *)(intptr_t)handle);
}

struct android_stream_playback {
    struct stream_playback_clock clock;
    struct stream_playback_state state;
    struct stream_playback_audio *audio;
    int64_t position, time_us;
    atomic_int finished;
    uint64_t end_frame;
};

JNIEXPORT jlong JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPlaybackCreate(
    JNIEnv *env, jobject obj, jlong id, jlong pts, jboolean audio, jboolean low_latency) {
    (void)env; (void)obj;
    if (pts < 0) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback invalid start PTS"); return 0; }
    struct android_stream_playback *p = u_calloc(1, sizeof(*p));
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback allocation failed"); return 0; }
    stream_playback_clock_reset(&p->clock, (uint64_t)id);
    stream_playback_clock_set_mode(&p->clock, low_latency ? 0 : 1); p->position = pts; p->time_us = -1;
    atomic_init(&p->finished, 0);
    if (audio && !(p->audio = stream_playback_audio_create(pts, stream_playback_prefill_ms(p->clock.mode)))) { u_free(p); return 0; }
    if (p->audio) stream_playback_audio_set_meter(p->audio, android_stream_audio_meter((uint64_t)id));
    return (jlong)(intptr_t)p;
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_streamPlaybackBuffering(JNIEnv *env, jobject obj, jlong handle) {
    (void)env; (void)obj; struct android_stream_playback *p = (struct android_stream_playback *)(intptr_t)handle;
    return p && stream_playback_audio_buffering(p->audio) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPlaybackFeed(
    JNIEnv *env, jobject obj, jlong handle, jlong pts, jbyteArray packet) {
    (void)obj; struct android_stream_playback *p = (struct android_stream_playback *)(intptr_t)handle;
    if (!p || !p->audio || !packet) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback feed invalid handle/packet"); return -1; }
    jsize length = (*env)->GetArrayLength(env, packet); uint8_t bytes[1275];
    if (length <= 0 || length > (jsize)sizeof(bytes)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback packet size=%d", length); return -1; }
    (*env)->GetByteArrayRegion(env, packet, 0, length, (jbyte *)bytes);
    if ((*env)->ExceptionCheck(env)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback packet access failed"); return -1; }
    return stream_playback_audio_feed(p->audio, pts, bytes, (size_t)length);
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPlaybackPrepare(JNIEnv *env, jobject obj, jlong handle) {
    (void)env; (void)obj; struct android_stream_playback *p = (struct android_stream_playback *)(intptr_t)handle;
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback prepare invalid handle"); return -1; }
    if (!p->audio) return 0;
    stream_playback_audio_set_tempo(p->audio, p->clock.tempo_milli);
    return stream_playback_audio_prepare(p->audio);
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPlaybackBuffered(JNIEnv *env, jobject obj, jlong handle) {
    (void)env; (void)obj; struct android_stream_playback *p = (struct android_stream_playback *)(intptr_t)handle;
    return p && p->audio ? (jint)stream_playback_audio_buffered(p->audio) : 0;
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPlaybackPull(
    JNIEnv *env, jobject obj, jlong handle, jshortArray output, jint frames, jlong limit) {
    (void)obj; struct android_stream_playback *p = (struct android_stream_playback *)(intptr_t)handle;
    if (!p || !p->audio || !output || frames <= 0 || frames > 8192 || (*env)->GetArrayLength(env, output) < frames * 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback PCM invalid frames=%d", frames); return -1;
    }
    if (limit < 0) return 0;
    int16_t pcm[8192 * 2];
    int count = stream_playback_audio_pull(p->audio, pcm, (unsigned)frames, limit);
    if (p->finished && !p->end_frame && stream_playback_audio_empty(p->audio))
        p->end_frame = stream_playback_audio_written(p->audio) + 1; /* 0 — конец ещё не поставлен устройству. */
    if (count <= 0) return count;
    (*env)->SetShortArrayRegion(env, output, 0, count * 2, pcm);
    if ((*env)->ExceptionCheck(env)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback PCM copy failed"); return -1; }
    return count;
}

JNIEXPORT jlongArray JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPlaybackUpdate(
    JNIEnv *env, jobject obj, jlong handle, jlong now, jlong played, jlong ready, jlong guard, jboolean starved, jboolean low_latency, jboolean source_finished) {
    (void)obj; struct android_stream_playback *p = (struct android_stream_playback *)(intptr_t)handle;
    if (!p || now < 0 || played < 0 || ready < 0) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback clock invalid input"); return NULL; }
    int mode = low_latency ? 0 : 1;
    if (p->clock.mode != mode) {
        stream_playback_clock_set_mode(&p->clock, mode);
        if (p->audio) stream_playback_audio_set_buffer_ms(p->audio, stream_playback_prefill_ms(mode));
        if (!p->audio) p->time_us = now;
    }
    int64_t pending = 0;
    int audio_done = p->finished && p->end_frame && (uint64_t)played >= p->end_frame - 1;
    if (p->audio && !audio_done) {
        int64_t position = stream_playback_audio_position(p->audio, (uint64_t)played);
        if (position > p->position) p->position = position;
        pending = stream_playback_audio_pending_gain(p->audio, (uint64_t)played);
    }
    if ((!p->audio || audio_done) && p->time_us >= 0 && !starved) {
        int64_t elapsed = now - p->time_us;
        if (elapsed < 0) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback wall clock regressed"); return NULL; }
        int64_t proposed = p->position + elapsed * p->clock.tempo_milli / 1000;
        if (p->clock.prefilling) {
            stream_playback_clock_can_present(&p->clock, ready - p->position, guard, p->finished || source_finished); proposed = p->position;
        } else if (proposed > ready) { proposed = ready; starved = JNI_TRUE; }
        if (proposed > p->position) p->position = proposed;
    }
    p->time_us = now;
    if (stream_playback_audio_buffering(p->audio)) starved = JNI_TRUE;
    if (p->audio) guard += stream_playback_audio_latency_us(p->audio);
    if (stream_playback_clock_update(&p->clock, now, p->position, ready, guard, pending, starved, &p->state) < 0) return NULL;
    jlong values[] = {p->position, p->state.reserve_us, p->state.minimum_us, p->state.tempo_milli, p->state.waiting,
        p->audio ? stream_playback_audio_needs_input(p->audio) : 0,
        p->audio ? (jlong)stream_playback_audio_written(p->audio) : 0,
        p->state.guard_us, p->state.reason, p->state.correction_us, p->state.pending_gain_us, (jlong)p->clock.pauses};
    jlongArray output = (*env)->NewLongArray(env, 12);
    if (output) (*env)->SetLongArrayRegion(env, output, 0, 12, values);
    else DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback clock allocation failed");
    return output;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPlaybackSilence(
    JNIEnv *env, jobject obj, jlong handle, jint frames) {
    (void)env; (void)obj; struct android_stream_playback *p = (struct android_stream_playback *)(intptr_t)handle;
    if (!p || !p->audio || frames < 0 || frames > 8192) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI playback silence invalid frames=%d", frames); return;
    }
    stream_playback_audio_silence(p->audio, (unsigned)frames);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPlaybackFinish(JNIEnv *env, jobject obj, jlong handle) {
    (void)env; (void)obj; struct android_stream_playback *p = (struct android_stream_playback *)(intptr_t)handle;
    if (p) { stream_playback_audio_finish(p->audio); p->finished = 1; }
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPlaybackNeedsInput(JNIEnv *env, jobject obj, jlong handle) {
    (void)env; (void)obj; struct android_stream_playback *p = (struct android_stream_playback *)(intptr_t)handle;
    return p && stream_playback_audio_needs_input(p->audio);
}

JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPlaybackEmpty(JNIEnv *env, jobject obj, jlong handle) {
    (void)env; (void)obj; struct android_stream_playback *p = (struct android_stream_playback *)(intptr_t)handle;
    return !p || stream_playback_audio_empty(p->audio);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamPlaybackClose(JNIEnv *env, jobject obj, jlong handle) {
    (void)env; (void)obj; struct android_stream_playback *p = (struct android_stream_playback *)(intptr_t)handle;
    if (!p) return;
    stream_playback_audio_destroy(p->audio); u_free(p);
}

/* Opus state существует только в audio worker; encode/decode не занимают uasync. */
JNIEXPORT jlong JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamOpusCreate(JNIEnv *env, jobject obj, jboolean encode) {
    (void)env; (void)obj;
    struct android_stream_opus *codec = u_calloc(1, sizeof(*codec));
    if (!codec) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream Opus allocation failed"); return 0; }
    if (encode) {
        codec->encoder = opus_codec_encoder_create(48000, STREAM_AUDIO_CHANNELS);
        if (!codec->encoder || opus_codec_encoder_application_set(codec->encoder, OPUS_CODEC_APP_AUDIO) < 0
            || opus_codec_encoder_bitrate_set(codec->encoder, 128000) < 0) goto fail;
        codec->compressor = audio_compressor_create();
        if (!codec->compressor) goto fail;
    } else {
        codec->decoder = opus_codec_decoder_create(48000, STREAM_AUDIO_CHANNELS);
        if (!codec->decoder) goto fail;
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "JNI stream Opus created codec=%p encode=%d rate=48000 channels=2 frame=960 bitrate=128000",
               (void *)codec, encode);
    return (jlong)(intptr_t)codec;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream Opus initialization failed encode=%d", encode);
    audio_compressor_destroy(codec->compressor);
    opus_codec_encoder_destroy(codec->encoder); opus_codec_decoder_destroy(codec->decoder); u_free(codec); return 0;
}

JNIEXPORT jbyteArray JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamOpusEncode(
    JNIEnv *env, jobject obj, jlong handle, jshortArray samples, jboolean enabled, jint max_gain_db, jint rise_rate) {
    (void)obj;
    struct android_stream_opus *codec = (struct android_stream_opus *)(intptr_t)handle;
    if (!codec || !codec->encoder || !codec->compressor || !samples || (*env)->GetArrayLength(env, samples) != STREAM_AUDIO_SAMPLES) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream Opus encode invalid handle/frame"); return NULL;
    }
    if (max_gain_db < 1 || max_gain_db > 60 || rise_rate < 1 || rise_rate > 100) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream compressor invalid max_gain=%d rise=%d", max_gain_db, rise_rate); return NULL;
    }
    if (codec->compressor_enabled != !!enabled || codec->max_gain_db != max_gain_db || codec->rise_rate != rise_rate) {
        audio_compressor_config_t config = {0};
        config.sample_rate = 48000; config.channels = STREAM_AUDIO_CHANNELS; config.block_duration_ms = 20;
        config.lookback_ms = 200; config.lookahead_ms = 0; config.target_level = audio_compressor_shared_target_level();
        config.use_shared_settings = 1;
        config.max_gain_db = max_gain_db; config.rise_rate_per_sec = rise_rate; config.release_rate_per_sec = rise_rate * 5.0f;
        if (audio_compressor_configure(codec->compressor, &config) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream compressor configuration failed codec=%p", (void *)codec); return NULL;
        }
        audio_compressor_set_enabled(codec->compressor, enabled);
        codec->compressor_enabled = !!enabled; codec->max_gain_db = max_gain_db; codec->rise_rate = rise_rate; codec->reported_clips = 0;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM,
                   "JNI stream compressor applied codec=%p enabled=%d max_gain=%ddB rise=%d release=%d target=0.25 history=200ms delay=0ms",
                   (void *)codec, !!enabled, max_gain_db, rise_rate, rise_rate * 5);
    }
    int16_t pcm[STREAM_AUDIO_SAMPLES]; uint8_t packet[1275];
    (*env)->GetShortArrayRegion(env, samples, 0, STREAM_AUDIO_SAMPLES, pcm);
    if ((*env)->ExceptionCheck(env)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream PCM read failed"); return NULL; }
    int report = codec->frames % 50 == 0, input_peak = 0, output_peak = 0, stereo_difference = 0;
    if (report) {
        for (unsigned i = 0; i < STREAM_AUDIO_SAMPLES; i++) if (abs((int)pcm[i]) > input_peak) input_peak = abs((int)pcm[i]);
        for (unsigned i = 0; i < STREAM_AUDIO_FRAMES; i++) stereo_difference += abs((int)pcm[2 * i] - pcm[2 * i + 1]);
    }
    if (audio_compressor_process_frame(codec->compressor, pcm, pcm, STREAM_AUDIO_SAMPLES) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream compressor processing failed codec=%p", (void *)codec); return NULL;
    }
    int length = opus_codec_encode(codec->encoder, pcm, STREAM_AUDIO_FRAMES, packet, sizeof(packet));
    if (length < 0) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream Opus encode failed result=%d", length); return NULL; }
    codec->frames++;
    if (report) {
        for (unsigned i = 0; i < STREAM_AUDIO_SAMPLES; i++) if (abs((int)pcm[i]) > output_peak) output_peak = abs((int)pcm[i]);
        uint32_t clips = audio_compressor_clip_count(codec->compressor);
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM,
                    "JNI stream audio codec=%p frames=%llu channels=2 enabled=%d gain=%.1fdB peaks=%d->%d lrDifference=%d clips=%u packet=%d",
                    (void *)codec, (unsigned long long)codec->frames, codec->compressor_enabled,
                    audio_compressor_gain_db(codec->compressor), input_peak, output_peak, stereo_difference / STREAM_AUDIO_FRAMES, clips, length);
        if (clips != codec->reported_clips) DEBUG_WARN(DEBUG_CATEGORY_STREAM, "JNI stream audio clipped codec=%p samples=%u gain=%.1fdB",
            (void *)codec, clips - codec->reported_clips, audio_compressor_gain_db(codec->compressor));
        codec->reported_clips = clips;
    }
    jbyteArray output = (*env)->NewByteArray(env, length);
    if (output) (*env)->SetByteArrayRegion(env, output, 0, length, (const jbyte *)packet);
    else DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream Opus output allocation failed");
    return output;
}

JNIEXPORT jshortArray JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamOpusDecode(
    JNIEnv *env, jobject obj, jlong handle, jbyteArray packet) {
    (void)obj;
    struct android_stream_opus *codec = (struct android_stream_opus *)(intptr_t)handle;
    if (!codec || !codec->decoder || !packet || (*env)->GetArrayLength(env, packet) > 1275) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream Opus decode invalid handle/packet"); return NULL;
    }
    jsize length = (*env)->GetArrayLength(env, packet); uint8_t bytes[1275]; int16_t pcm[STREAM_AUDIO_SAMPLES];
    (*env)->GetByteArrayRegion(env, packet, 0, length, (jbyte *)bytes);
    if ((*env)->ExceptionCheck(env)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream Opus packet read failed"); return NULL; }
    int count = opus_codec_decode(codec->decoder, length ? bytes : NULL, length, pcm, STREAM_AUDIO_FRAMES);
    if (count != STREAM_AUDIO_FRAMES) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream Opus decode failed count=%d", count); return NULL; }
    jshortArray output = (*env)->NewShortArray(env, STREAM_AUDIO_SAMPLES);
    if (output) (*env)->SetShortArrayRegion(env, output, 0, STREAM_AUDIO_SAMPLES, pcm);
    else DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "JNI stream Opus PCM allocation failed");
    return output;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_NativeLib_nativeStreamOpusClose(JNIEnv *env, jobject obj, jlong handle) {
    (void)env; (void)obj;
    struct android_stream_opus *codec = (struct android_stream_opus *)(intptr_t)handle;
    if (!codec) return;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "JNI stream Opus release codec=%p encode=%d frames=%llu", (void *)codec,
               codec->encoder != NULL, (unsigned long long)codec->frames);
    audio_compressor_destroy(codec->compressor);
    opus_codec_encoder_destroy(codec->encoder); opus_codec_decoder_destroy(codec->decoder); u_free(codec);
}
#endif
