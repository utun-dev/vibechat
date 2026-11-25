/* Общий для desktop/Android SpeexDSP-фильтр; API и владение буферами описаны в speex_ns.h. */
#include "speex_ns.h"
#include "debug_config.h"
#include "mem.h"

#include <math.h>
#include <string.h>
#include <speex/speex_preprocess.h>

struct speex_ns {
    SpeexPreprocessState* state[2]; /* Независимые состояния микрофонных каналов. */
    int channels, suppress_db;      /* Формат и отрицательный предел подавления в dB. */
    int pending, finished;          /* Накопленные interleaved отсчёты и завершение записи. */
    int16_t buffer[1920];           /* Кадр записи; потоковый режим использует буфер вызывающего. */
    uint64_t frames;                /* Обработанные блоки, включая завершающие нули. */
    unsigned measured_frames;      /* Кадры текущего диагностического интервала. */
    double input_energy, output_energy; /* Энергия до/после NS; только при media=debug. */
};

/* Сконфигурировать только спектральное подавление шума, без второго AGC/VAD/AEC. */
struct speex_ns* speex_ns_create(int channels, int suppress_db) {
    if (channels < 1 || channels > 2 || suppress_db < SPEEX_NS_MIN_DB || suppress_db > SPEEX_NS_MAX_DB) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: invalid configuration channels=%d suppress=%ddB", channels, suppress_db);
        return NULL;
    }
    struct speex_ns* ns = u_calloc(1, sizeof(*ns));
    if (!ns) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: state allocation failed channels=%d", channels); return NULL; }
    ns->channels = channels;
    for (int channel = 0; channel < channels; ++channel) {
        ns->state[channel] = speex_preprocess_state_init(SPEEX_NS_FRAME_SAMPLES, 48000);
        if (!ns->state[channel]) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: preprocess allocation failed channel=%d", channel);
            speex_ns_destroy(ns); return NULL;
        }
        int on = 1, off = 0;
        if (speex_preprocess_ctl(ns->state[channel], SPEEX_PREPROCESS_SET_DENOISE, &on) ||
            speex_preprocess_ctl(ns->state[channel], SPEEX_PREPROCESS_SET_AGC, &off) ||
            speex_preprocess_ctl(ns->state[channel], SPEEX_PREPROCESS_SET_DEREVERB, &off)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: preprocess configuration failed channel=%d", channel);
            speex_ns_destroy(ns); return NULL;
        }
        /* VAD и echo_state по умолчанию выключены; SET_VAD сам печатает upstream warning даже для 0. */
    }
    if (speex_ns_set_level(ns, suppress_db)) { speex_ns_destroy(ns); return NULL; }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "speex-ns: created rate=48000 frame=960 channels=%d suppress=%ddB AGC=0 VAD=0 echo=0",
               channels, suppress_db);
    return ns;
}

/* Общая очистка работает и при частично созданных каналах. */
void speex_ns_destroy(struct speex_ns* ns) {
    if (!ns) return;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "speex-ns: destroyed channels=%d suppress=%ddB frames=%llu pending=%d finished=%d",
               ns->channels, ns->suppress_db, (unsigned long long)ns->frames, ns->pending, ns->finished);
    for (int channel = 0; channel < ns->channels; ++channel)
        if (ns->state[channel]) speex_preprocess_state_destroy(ns->state[channel]);
    u_free(ns);
}

/* Сила меняется одновременно для обоих каналов в потоке владельца. */
int speex_ns_set_level(struct speex_ns* ns, int suppress_db) {
    if (!ns || suppress_db < SPEEX_NS_MIN_DB || suppress_db > SPEEX_NS_MAX_DB) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: invalid level=%ddB state=%p", suppress_db, (void*)ns); return -1;
    }
    for (int channel = 0; channel < ns->channels; ++channel) {
        if (speex_preprocess_ctl(ns->state[channel], SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress_db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: SET_NOISE_SUPPRESS failed channel=%d value=%d", channel, suppress_db);
            return -1;
        }
    }
    ns->suppress_db = suppress_db;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "speex-ns: level=%ddB channels=%d", suppress_db, ns->channels);
    return 0;
}

/* Разделение stereo сохраняет независимость каналов; на кадре нет malloc/lock. */
int speex_ns_process_frame(struct speex_ns* ns, int16_t* pcm, int count) {
    if (!pcm || count <= 0 || (ns && (count != SPEEX_NS_FRAME_SAMPLES * ns->channels || ns->finished))) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: invalid frame count=%d channels=%d finished=%d",
                    count, ns ? ns->channels : 0, ns ? ns->finished : 0); return -1;
    }
    if (!ns) return 0;
    int measure = debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_MEDIA);
    if (measure) for (int i = 0; i < count; ++i) ns->input_energy += (double)pcm[i] * pcm[i];
    if (ns->channels == 1) speex_preprocess_run(ns->state[0], pcm);
    else {
        int16_t mono[SPEEX_NS_FRAME_SAMPLES];
        for (int channel = 0; channel < ns->channels; ++channel) {
            for (int i = 0; i < SPEEX_NS_FRAME_SAMPLES; ++i) mono[i] = pcm[2 * i + channel];
            speex_preprocess_run(ns->state[channel], mono);
            for (int i = 0; i < SPEEX_NS_FRAME_SAMPLES; ++i) pcm[2 * i + channel] = mono[i];
        }
    }
    ++ns->frames;
    if (measure) {
        for (int i = 0; i < count; ++i) ns->output_energy += (double)pcm[i] * pcm[i];
        if (++ns->measured_frames == 50) {
            double scale = (double)50 * count * 32768.0 * 32768.0;
            DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "speex-ns: signal channels=%d suppress=%ddB frames=%llu input=%.1fdBFS output=%.1fdBFS",
                        ns->channels, ns->suppress_db, (unsigned long long)ns->frames,
                        10 * log10(fmax(ns->input_energy / scale, 1e-12)), 10 * log10(fmax(ns->output_energy / scale, 1e-12)));
            ns->measured_frames = 0; ns->input_energy = ns->output_energy = 0;
        }
    }
    return 0;
}

/* Адаптер произвольных чанков записи; получатель вызывается только для готового PCM. */
int speex_ns_push(struct speex_ns* ns, const int16_t* pcm, int count, speex_ns_output_fn output, void* arg) {
    if (!pcm || count <= 0 || !output || (ns && (count % ns->channels || ns->finished))) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: invalid push count=%d channels=%d finished=%d",
                    count, ns ? ns->channels : 0, ns ? ns->finished : 0); return -1;
    }
    if (!ns) {
        if (!output(arg, pcm, count)) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: bypass output rejected count=%d", count); return -1;
    }
    int frame = SPEEX_NS_FRAME_SAMPLES * ns->channels;
    while (count) {
        int take = frame - ns->pending;
        if (take > count) take = count;
        memcpy(ns->buffer + ns->pending, pcm, (size_t)take * sizeof(*pcm));
        ns->pending += take; pcm += take; count -= take;
        if (ns->pending != frame) continue;
        ns->pending = 0;
        if (speex_ns_process_frame(ns, ns->buffer, frame)) return -1;
        if (output(arg, ns->buffer, frame)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: record output rejected count=%d", frame); return -1;
        }
    }
    return 0;
}

/* WOLA хранит предыдущий кадр: последний нулевой вход выдаёт остаток без обрезания конца записи. */
int speex_ns_finish(struct speex_ns* ns, speex_ns_output_fn output, void* arg) {
    if (!ns || ns->finished) return 0;
    if (!output) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: finish without output"); return -1; }
    int frame = SPEEX_NS_FRAME_SAMPLES * ns->channels, tail = ns->pending;
    if (tail) {
        memset(ns->buffer + tail, 0, (size_t)(frame - tail) * sizeof(int16_t));
        ns->pending = 0;
        if (speex_ns_process_frame(ns, ns->buffer, frame)) return -1;
        if (output(arg, ns->buffer, frame)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: partial output rejected count=%d", frame); return -1;
        }
    }
    if (ns->frames) {
        memset(ns->buffer, 0, (size_t)frame * sizeof(int16_t));
        if (speex_ns_process_frame(ns, ns->buffer, frame)) return -1;
        if (output(arg, ns->buffer, tail ? tail : frame)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "speex-ns: final output rejected count=%d", tail ? tail : frame); return -1;
        }
    }
    ns->finished = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "speex-ns: finished channels=%d partial_samples=%d frames=%llu",
                ns->channels, tail, (unsigned long long)ns->frames);
    return 0;
}
