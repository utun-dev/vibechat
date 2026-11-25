/* Whisper: модель принадлежит экземпляру; файлы и распознавание обрабатывает media_async worker. */
#include "chat_whisper.h"
#include "chat_core_priv.h"
#include "chat_setting.h"
#include "../utun_instance.h"
#include "../media_async/media_async.h"
#include "../../lib/file_io.h"
#include "../../lib/mem.h"
#include "../../lib/opus_codec.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef HAVE_WHISPER
#include <whisper.h>
#endif

struct chat_whisper {
    int busy;                       /* Одна принятая задача, включая ожидающий done. */
    int closing;                    /* Отсоединён от chat_core; освобождается последним done. */
#ifdef HAVE_WHISPER
    struct whisper_context* model;  /* Доступ только worker либо после его join. */
    char model_path[512];            /* Путь успешно загруженной модели. */
#endif
};

#ifdef HAVE_WHISPER
struct wh_job {
    struct chat_whisper* owner;
    FILE* input;                    /* Открыт до передачи worker: удаление пути не подменяет файл. */
    char channel[65], model_path[512];
    uint8_t reply[32];
    chat_whisper_done_fn done;
    void* arg;
    char* text;
    int error;
};

struct wh_pcm {
    float* samples;
    size_t count, capacity;
    unsigned rate;
};

/* Единственная точка роста PCM; ограничение действует до выделения памяти. */
static int wh_reserve(struct wh_pcm* pcm, size_t count) {
    size_t maximum = (size_t)pcm->rate * (CHAT_WHISPER_MAX_DURATION_MS / 1000);
    if (!count || count > maximum) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: duration limit samples=%zu rate=%u limit_ms=%u",
            count, pcm->rate, CHAT_WHISPER_MAX_DURATION_MS); return -1;
    }
    if (count <= pcm->capacity) return 0;
    size_t capacity = pcm->capacity ? pcm->capacity * 2 : pcm->rate;
    if (capacity < count) capacity = count;
    if (capacity > maximum) capacity = maximum;
    float* next = u_realloc(pcm->samples, (uint32_t)(capacity * sizeof(float)));
    if (!next) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: PCM allocation failed samples=%zu", capacity); return -1; }
    pcm->samples = next; pcm->capacity = capacity; return 0;
}

static uint16_t wh_le16(const uint8_t* p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t wh_le32(const uint8_t* p) { return (uint32_t)wh_le16(p) | (uint32_t)wh_le16(p + 2) << 16; }

/* PCM WAV: проверяем весь RIFF, размеры/паддинг chunks и полноту данных до публикации PCM. */
static int wh_read_wav(FILE* file, long size, struct wh_pcm* pcm) {
    uint8_t header[16]; long data = 0; uint32_t bytes = 0; unsigned channels = 0;
    if (fseek(file, 0, SEEK_SET) || fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) ||
        memcmp(header + 8, "WAVE", 4) || (uint64_t)wh_le32(header + 4) + 8 != (uint64_t)size) goto invalid;
    long offset = 12;
    while (offset < size) {
        if (size - offset < 8 || fseek(file, offset, SEEK_SET) || fread(header, 1, 8, file) != 8) goto invalid;
        uint32_t length = wh_le32(header + 4);
        uint64_t next = (uint64_t)offset + 8 + length + (length & 1);
        if (next > (uint64_t)size) goto invalid;
        if (!memcmp(header, "fmt ", 4)) {
            if (pcm->rate || length < 16 || fread(header, 1, 16, file) != 16) goto invalid;
            channels = wh_le16(header + 2); pcm->rate = wh_le32(header + 4);
            if (wh_le16(header) != 1 || (channels != 1 && channels != 2) || pcm->rate < 8000 || pcm->rate > 48000 ||
                wh_le16(header + 14) != 16 || wh_le16(header + 12) != channels * 2 ||
                wh_le32(header + 8) != pcm->rate * channels * 2) goto invalid;
        } else if (!memcmp(header, "data", 4)) {
            if (data || !length) goto invalid;
            data = offset + 8; bytes = length;
        }
        offset = (long)next;
    }
    if (!data || !pcm->rate || bytes % (channels * 2)) goto invalid;
    size_t count = bytes / (channels * 2);
    if (wh_reserve(pcm, count) || fseek(file, data, SEEK_SET)) goto invalid;
    for (size_t i = 0; i < count; i++) {
        uint8_t frame[4];
        if (fread(frame, 1, channels * 2, file) != channels * 2) goto invalid;
        float sample = (int16_t)wh_le16(frame);
        if (channels == 2) sample = (sample + (int16_t)wh_le16(frame + 2)) * 0.5f;
        pcm->samples[pcm->count++] = sample / 32768.0f;
    }
    return 0;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: invalid/incomplete PCM WAV size=%ld data=%ld bytes=%u rate=%u",
        size, data, bytes, pcm->rate); return -1;
}

/* Контейнер voice_file_encode: native-endian header/lengths, 20 ms interleaved Opus и обязательный terminator. */
static int wh_read_opus(FILE* file, struct wh_pcm* pcm) {
    uint32_t magic, rate; uint16_t channels, frame; opus_codec_decoder_t* decoder = NULL;
    const char* stage = "header";
    if (fseek(file, 0, SEEK_SET) || fread(&magic, 4, 1, file) != 1 || fread(&rate, 4, 1, file) != 1 ||
        fread(&channels, 2, 1, file) != 1 || fread(&frame, 2, 1, file) != 1 || magic != 0x5355504f ||
        (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) ||
        (channels != 1 && channels != 2) || frame != rate / 50) goto invalid;
    pcm->rate = rate; stage = "decoder"; decoder = opus_codec_decoder_create((int)rate, channels);
    if (!decoder) goto invalid;
    while (1) {
        uint16_t length; uint8_t packet[4000]; int16_t samples[1920];
        stage = "packet length";
        if (fread(&length, 2, 1, file) != 1) goto invalid;
        if (!length) {
            stage = "terminator";
            if (!pcm->count || fgetc(file) != EOF || ferror(file)) goto invalid;
            opus_codec_decoder_destroy(decoder); return 0;
        }
        stage = "packet";
        if (length > sizeof(packet) || fread(packet, 1, length, file) != length ||
            wh_reserve(pcm, pcm->count + frame)) goto invalid;
        int decoded = opus_codec_decode(decoder, packet, length, samples, frame);
        stage = "decode";
        if (decoded != frame) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: Opus decode result=%d expected_samples=%u", decoded, frame); goto invalid;
        }
        for (unsigned i = 0; i < frame; i++) {
            float sample = samples[i * channels];
            if (channels == 2) sample = (sample + samples[i * channels + 1]) * 0.5f;
            pcm->samples[pcm->count++] = sample / 32768.0f;
        }
    }
invalid:
    if (decoder) opus_codec_decoder_destroy(decoder);
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: invalid/incomplete voice Opus stage=%s samples=%zu", stage, pcm->count); return -1;
}

/* Указанный путь обязателен; без настройки используются стандартные места поиска. */
static int wh_model_path(struct UTUN_INSTANCE* inst, char out[512]) {
    const char* setting = chat_setting_get(inst, "whisper_model_path");
    if (setting && *setting) {
        if (strlen(setting) >= 512) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: model path too long"); return -1; }
        memcpy(out, setting, strlen(setting) + 1); return 0;
    }
    const char* home = getenv("HOME"); char local[512];
    int length = home ? snprintf(local, sizeof(local), "%s/.local/share/utun/models/ggml-base.bin", home) : -1;
    const char* paths[] = {"ggml-base.bin", "/usr/share/utun/models/ggml-base.bin", local};
    for (unsigned i = 0; i < 3; i++) {
        if (i == 2 && (length < 0 || (size_t)length >= sizeof(local))) continue;
        struct file_io_info info;
        if (!file_io_inspect(paths[i], 1, &info) && info.type == FILE_IO_REGULAR) {
            memcpy(out, paths[i], strlen(paths[i]) + 1); return 0;
        }
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: model not found; set whisper_model_path"); return -1;
}

/* Worker — единственный пользователь модели; готовность сохраняется только после успешной загрузки. */
static void wh_work(void* arg) {
    struct wh_job* job = arg; struct chat_whisper* owner = job->owner;
    struct wh_pcm pcm = {0}; float* mono = NULL; const char* stage = "audio size";
    if (fseek(job->input, 0, SEEK_END)) goto done;
    long size = ftell(job->input);
    if (size < 12 || size > 128 * 1024 * 1024 || fseek(job->input, 0, SEEK_SET)) goto done;
    uint8_t magic[4]; stage = "audio decode";
    if (fread(magic, 1, 4, job->input) != 4) goto done;
    if ((!memcmp(magic, "RIFF", 4) ? wh_read_wav(job->input, size, &pcm) : wh_read_opus(job->input, &pcm))) goto done;
    size_t count = ((uint64_t)pcm.count * 16000 + pcm.rate - 1) / pcm.rate;
    stage = "resample allocation"; mono = u_malloc((uint32_t)(count * sizeof(float)));
    if (!mono) goto done;
    for (size_t i = 0; i < count; i++) {
        uint64_t position = (uint64_t)i * pcm.rate;
        size_t index = position / 16000;
        float first = pcm.samples[index], next = index + 1 < pcm.count ? pcm.samples[index + 1] : first;
        mono[i] = first + (next - first) * (float)(position % 16000) / 16000.0f;
    }
    u_free(pcm.samples); pcm.samples = NULL;
    stage = "model load";
    if (!owner->model || strcmp(owner->model_path, job->model_path)) {
        struct whisper_context_params params = whisper_context_default_params(); params.use_gpu = 0;
        struct whisper_context* model = whisper_init_from_file_with_params(job->model_path, params);
        if (!model) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: model load failed path=%s", job->model_path); goto done; }
        if (owner->model) whisper_free(owner->model);
        owner->model = model; memcpy(owner->model_path, job->model_path, strlen(job->model_path) + 1);
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "whisper: model loaded path=%s", job->model_path);
    }
    struct whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.language = "ru"; params.n_threads = 4; params.no_timestamps = 1; params.single_segment = 1;
    params.print_progress = params.print_realtime = params.print_timestamps = params.print_special = 0;
    stage = "recognition";
    int result = whisper_full(owner->model, params, mono, (int)count);
    if (result) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: recognition failed result=%d samples=%zu", result, count); goto done; }
    int segments = whisper_full_n_segments(owner->model); size_t bytes = 0;
    stage = "text length"; if (segments < 0) goto done;
    for (int i = 0; i < segments; i++) {
        const char* text = whisper_full_get_segment_text(owner->model, i);
        if (!text || strlen(text) > CHAT_WHISPER_MAX_TEXT - bytes) goto done;
        bytes += strlen(text);
    }
    stage = "text allocation"; job->text = u_malloc((uint32_t)bytes + 1);
    if (!job->text) goto done;
    size_t offset = 0;
    for (int i = 0; i < segments; i++) {
        const char* text = whisper_full_get_segment_text(owner->model, i); size_t length = strlen(text);
        memcpy(job->text + offset, text, length); offset += length;
    }
    job->text[offset] = 0; job->error = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "whisper: recognized ch=%s samples16k=%zu bytes=%zu", job->channel, count, bytes);
done:
    u_free(pcm.samples); u_free(mono);
    if (fclose(job->input)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: audio close failed ch=%s", job->channel); job->error = -1; }
    job->input = NULL;
    if (job->error) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: task failed ch=%s stage=%s", job->channel, stage);
}
#endif

/* Только после worker join (либо когда работы нет). */
static void wh_free(struct chat_whisper* owner) {
#ifdef HAVE_WHISPER
    if (owner->model) whisper_free(owner->model);
#endif
    u_free(owner);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "whisper: owner released");
}

#ifdef HAVE_WHISPER
/* media_async гарантирует join до done; пользовательский callback может заново вызвать transcribe/destroy. */
static void wh_done(void* arg, int error) {
    struct wh_job* job = arg; struct chat_whisper* owner = job->owner;
    if (job->input && fclose(job->input)) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: rejected input close failed");
    owner->busy = 0;
    if (error) job->error = error;
    if (owner->closing) { job->error = MEDIA_ASYNC_CANCELLED; wh_free(owner); }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "whisper: completed ch=%s error=%d", job->channel, job->error);
    job->done(job->arg, job->channel, job->error ? NULL : job->text, job->error, job->reply);
    u_free(job->text); u_free(job);
}
#endif

void chat_whisper_transcribe(struct UTUN_INSTANCE* inst, const char* path, const char* channel,
    const uint8_t reply[32], chat_whisper_done_fn done, void* arg) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !inst->chat_started || !inst->ua || !inst->media_async || !path || !*path ||
        !chat_channel_valid(channel) || !reply || !done) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: invalid context/arguments");
        if (done) done(arg, channel ? channel : "", NULL, -1, reply);
        return;
    }
    if (!chat_setting_get_int(inst, "whisper_enabled", 0)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "whisper: transcription disabled"); done(arg, channel, NULL, -1, reply); return;
    }
#ifndef HAVE_WHISPER
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "whisper: compiled without recognition support"); done(arg, channel, NULL, -1, reply);
#else
    if (cc->whisper && cc->whisper->busy) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "whisper: busy ch=%s", channel); done(arg, channel, NULL, CHAT_WHISPER_BUSY, reply); return;
    }
    struct wh_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: task allocation failed"); done(arg, channel, NULL, -1, reply); return; }
    if (wh_model_path(inst, job->model_path) || !(job->input = file_io_open_read(path))) {
        u_free(job); done(arg, channel, NULL, -1, reply); return;
    }
    if (!cc->whisper) cc->whisper = u_calloc(1, sizeof(*cc->whisper));
    if (!cc->whisper) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: owner allocation failed");
        if (fclose(job->input)) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "whisper: input close failed after owner allocation failure");
        u_free(job); done(arg, channel, NULL, -1, reply); return;
    }
    job->owner = cc->whisper; job->owner->busy = 1; job->done = done; job->arg = arg; job->error = -1;
    memcpy(job->channel, channel, strlen(channel) + 1); memcpy(job->reply, reply, 32);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "whisper: accepted ch=%s model=%s", channel, job->model_path);
    media_async_submit(inst->media_async, inst->ua, wh_work, job, wh_done, job);
#endif
}

void chat_whisper_destroy(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->whisper) return;
    struct chat_whisper* owner = cc->whisper; cc->whisper = NULL; owner->closing = 1;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "whisper: stopping busy=%d", owner->busy);
    if (!owner->busy) wh_free(owner);
}
