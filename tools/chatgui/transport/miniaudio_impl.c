#include "audio_diagnostics.h"
#ifdef __linux__
#include <dlfcn.h>
#include <time.h>
static void audio_diag_pulse_init(void* device);
static void audio_timing_begin(void* device, unsigned int frames, int capture);
static void audio_timing_end(void);
static void audio_capture_hole(uint64_t offset);
#define MA_AUDIO_TIMING_BEGIN(device, frames, capture) audio_timing_begin(device, frames, capture)
#define MA_AUDIO_TIMING_END() audio_timing_end()
#define MA_AUDIO_CAPTURE_HOLE(offset) audio_capture_hole(offset)
#define MA_DIAGNOSTIC_DEVICE_INIT(device) audio_diag_pulse_init(device)
#endif
#if defined(__linux__) || defined(_WIN32)
#define MA_DIAGNOSTIC_EVENT(device, kind, stage, value, detail) audio_diag_event(device, kind, stage, value, detail)
#define MA_DIAGNOSTIC_LOG(log, level, message) audio_diag_log(log, level, message)
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "../../../lib/miniaudio.h"

#ifdef __linux__
#ifdef MA_SUPPORT_PULSEAUDIO
static void audio_diag_pulse_underflow(ma_pa_stream* stream, void* userdata) {
    (void)stream;
    ma_device__on_notification(ma_device_notification_init(userdata, ma_device_notification_type_playback_underrun));
}
static void audio_diag_pulse_overflow(ma_pa_stream* stream, void* userdata) {
    ma_device* device = userdata;
    if (stream == device->pulse.pStreamCapture)
        ma_device__on_notification(ma_device_notification_init(device, ma_device_notification_type_capture_overrun));
    else audio_diag_event(userdata, AUDIO_DIAG_BACKEND_OVERFLOW, AUDIO_DIAG_WRITE, 1, 0);
}
static void audio_diag_pulse_started(ma_pa_stream* stream, void* userdata) {
    (void)stream;
    audio_diag_event(userdata, AUDIO_DIAG_BACKEND_STARTED, AUDIO_DIAG_WRITE, 1, 0);
}

static double audio_buffer_ms(ma_uint32 bytes, ma_uint32 bytes_per_second) {
    return bytes == (ma_uint32)-1 ? -1.0 : (double)bytes * 1000 / bytes_per_second;
}

/* После init, до запуска worker: размеры PA относятся к внутреннему PCM, не к client PCM. */
static void audio_diag_pulse_buffer(ma_device* device, int capture) {
    ma_pa_stream* stream = capture ? device->pulse.pStreamCapture : device->pulse.pStreamPlayback;
    if (!stream) return;
    const ma_pa_buffer_attr* attr = ((ma_pa_stream_get_buffer_attr_proc)device->pContext->pulse.pa_stream_get_buffer_attr)(stream);
    ma_format format = capture ? device->capture.internalFormat : device->playback.internalFormat;
    ma_uint32 channels = capture ? device->capture.internalChannels : device->playback.internalChannels;
    ma_uint32 rate = capture ? device->capture.internalSampleRate : device->playback.internalSampleRate;
    ma_uint32 bpf = ma_get_bytes_per_frame(format, channels);
    if (!attr || !rate || !bpf) {
        audio_diag_log(ma_device_get_log(device), MA_LOG_LEVEL_WARNING, "PulseAudio buffer snapshot unavailable");
        return;
    }
    ma_log_postf(ma_device_get_log(device), MA_LOG_LEVEL_INFO,
                 "[PulseAudio] dev=%p %s PCM format=%s channels=%u rate=%u bpf=%u\n",
                 (void*)device, capture ? "capture" : "playback", ma_get_format_name(format), channels, rate, bpf);
    ma_log_postf(ma_device_get_log(device), MA_LOG_LEVEL_INFO,
                 "[PulseAudio] dev=%p %s buffer_ms tlength=%.3f fragsize=%.3f minreq=%.3f maxlength=%.3f prebuf=%.3f (-1=default)\n",
                 (void*)device, capture ? "capture" : "playback", audio_buffer_ms(attr->tlength, rate * bpf),
                 audio_buffer_ms(attr->fragsize, rate * bpf), audio_buffer_ms(attr->minreq, rate * bpf),
                 audio_buffer_ms(attr->maxlength, rate * bpf), audio_buffer_ms(attr->prebuf, rate * bpf));
}
#endif

/* Только init-поток: backend-callbacks остаются короткими наблюдателями. */
static void audio_diag_pulse_init(void* device) {
#ifdef MA_SUPPORT_PULSEAUDIO
    ma_device* dev = (ma_device*)device;
    ma_pa_stream_set_suspended_callback_proc underflow, overflow, started;
    if (dev->pContext->backend != ma_backend_pulseaudio) return;
    audio_diag_pulse_buffer(dev, 1);
    audio_diag_pulse_buffer(dev, 0);
#ifndef MA_NO_RUNTIME_LINKING
    underflow = (ma_pa_stream_set_suspended_callback_proc)dlsym(dev->pContext->pulse.pulseSO, "pa_stream_set_underflow_callback");
    overflow = (ma_pa_stream_set_suspended_callback_proc)dlsym(dev->pContext->pulse.pulseSO, "pa_stream_set_overflow_callback");
    started = (ma_pa_stream_set_suspended_callback_proc)dlsym(dev->pContext->pulse.pulseSO, "pa_stream_set_started_callback");
#else
    underflow = pa_stream_set_underflow_callback;
    overflow = pa_stream_set_overflow_callback;
    started = pa_stream_set_started_callback;
#endif
    if (!underflow || !overflow || !started) {
        audio_diag_log(ma_device_get_log(dev), MA_LOG_LEVEL_WARNING, "PulseAudio xrun/started callbacks unavailable");
        return;
    }
    if (dev->pulse.pStreamCapture) overflow((ma_pa_stream*)dev->pulse.pStreamCapture, audio_diag_pulse_overflow, dev);
    if (dev->pulse.pStreamPlayback) {
        underflow((ma_pa_stream*)dev->pulse.pStreamPlayback, audio_diag_pulse_underflow, dev);
        overflow((ma_pa_stream*)dev->pulse.pStreamPlayback, audio_diag_pulse_overflow, dev);
        started((ma_pa_stream*)dev->pulse.pStreamPlayback, audio_diag_pulse_started, dev);
    }
    audio_diag_log(ma_device_get_log(dev), MA_LOG_LEVEL_INFO, "PulseAudio xrun/started observers registered");
#else
    (void)device;
#endif
}
#endif

/* Вызывается только владельцем общего контекста (GUI), вне device callbacks. */
int audio_context_failed(ma_context* context) {
#ifdef MA_SUPPORT_PULSEAUDIO
    if (context && context->backend == ma_backend_pulseaudio) {
        int result = ((ma_pa_mainloop_iterate_proc)context->pulse.pa_mainloop_iterate)((ma_pa_mainloop*)context->pulse.pMainLoop, 0, NULL);
        ma_pa_context_state_t state = ((ma_pa_context_get_state_proc)context->pulse.pa_context_get_state)((ma_pa_context*)context->pulse.pPulseContext);
        if (result < 0 || state == MA_PA_CONTEXT_FAILED || state == MA_PA_CONTEXT_TERMINATED) {
            ma_log_postf(ma_context_get_log(context), MA_LOG_LEVEL_ERROR, "[PulseAudio] Shared context failed iterate=%d state=%d errno=%d.\n",
                         result, state, ((ma_pa_context_errno_proc)context->pulse.pa_context_errno)((ma_pa_context*)context->pulse.pPulseContext));
            return 1;
        }
    }
#endif
    return 0;
}

/* Только device callback того же PA mainloop: запрос использует готовый timing snapshot без roundtrip. */
int64_t audio_stream_latency_us(ma_device* device, int capture, int* valid) {
    *valid = 0;
#ifdef MA_SUPPORT_PULSEAUDIO
    if (device && device->pContext->backend == ma_backend_pulseaudio && device->pContext->pulse.pa_stream_get_latency) {
        ma_pa_stream* stream = capture ? device->pulse.pStreamCapture : device->pulse.pStreamPlayback;
        ma_uint64 latency = 0; int negative = 0;
        if (stream && ((ma_pa_stream_get_latency_proc)device->pContext->pulse.pa_stream_get_latency)(stream, &latency, &negative) == 0) {
            *valid = 1;
            return negative ? -(int64_t)latency : (int64_t)latency;
        }
    }
#endif
    return 0;
}

/* PA device name читается в его mainloop; GUI получает только непрозрачный идентификатор маршрута. */
uint64_t audio_stream_route(ma_device* device, int capture) {
#ifdef MA_SUPPORT_PULSEAUDIO
    if (device && device->pContext->backend == ma_backend_pulseaudio) {
        ma_pa_stream* stream = capture ? device->pulse.pStreamCapture : device->pulse.pStreamPlayback;
        const char* name = stream ? ((ma_pa_stream_get_device_name_proc)device->pContext->pulse.pa_stream_get_device_name)(stream) : NULL;
        if (name && *name) {
            uint64_t hash = UINT64_C(14695981039346656037);
            for (; *name; ++name) { hash ^= (unsigned char)*name; hash *= UINT64_C(1099511628211); }
            return hash;
        }
    }
#else
    (void)device; (void)capture;
#endif
    return 0;
}

/* Reporter копирует сообщение в bounded очередь; DEBUG/file I/O в аппаратном потоке не выполняются. */
void audio_stream_log_route(ma_device* device, int capture) {
#ifdef MA_SUPPORT_PULSEAUDIO
    if (device && device->pContext->backend == ma_backend_pulseaudio) {
        ma_pa_stream* stream = capture ? device->pulse.pStreamCapture : device->pulse.pStreamPlayback;
        const char* name = stream ? ((ma_pa_stream_get_device_name_proc)device->pContext->pulse.pa_stream_get_device_name)(stream) : NULL;
        if (name) {
            char message[320];
            snprintf(message, sizeof(message), "PulseAudio physical %s route: %.255s", capture ? "capture" : "playback", name);
            audio_diag_log(ma_device_get_log(device), MA_LOG_LEVEL_INFO, message);
        }
    }
#else
    (void)device; (void)capture;
#endif
}

#ifdef __linux__
/* Один backend dispatch может породить несколько client callbacks при конвертации.
 * Начало snapshot фиксируется один раз; offset растёт по фактически выданному client PCM. */
static _Thread_local struct {
    ma_device* device;
    int valid;
    int64_t first_us;
    uint64_t frames;
    uint64_t hole_offset;
    int missing, hole_pending;
} audio_timing;

static void audio_timing_begin(void* opaque, unsigned int frames, int capture) {
    ma_device* device = opaque;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int valid = 0;
    int64_t latency = audio_stream_latency_us(device, capture, &valid);
    ma_uint32 rate = capture ? device->capture.internalSampleRate : device->playback.internalSampleRate;
    ma_uint64 converter_delay = capture ? ma_data_converter_get_output_latency(&device->capture.converter)
                                        : ma_data_converter_get_input_latency(&device->playback.converter);
    audio_timing.device = device;
    audio_timing.missing = audio_timing.hole_pending;
    audio_timing.valid = valid && device->noFixedSizedCallback;
    int64_t time = now.tv_sec * INT64_C(1000000) + now.tv_nsec / 1000;
    if (capture) {
        time -= valid ? latency : (int64_t)frames * 1000000 / rate;
        time -= (int64_t)converter_delay * 1000000 / device->sampleRate;
        if (audio_timing.missing) time += (int64_t)audio_timing.hole_offset * 1000000 / rate;
    } else {
        time += latency + (int64_t)(converter_delay + device->playback.inputCacheRemaining) * 1000000 / device->sampleRate;
    }
    audio_timing.first_us = time; audio_timing.frames = 0;
    if (capture) {
        struct audio_capture_timing timing = {time, latency, (int64_t)converter_delay * 1000000 / device->sampleRate,
                                             frames, rate, audio_timing.valid, audio_timing.missing};
        audio_diag_capture_clock(device, &timing);
    } else {
        struct audio_playback_timing timing = {latency,
            (int64_t)(converter_delay + device->playback.inputCacheRemaining) * 1000000 / device->sampleRate,
            frames, rate, audio_timing.valid};
        audio_diag_playback_clock(device, &timing);
    }
    audio_timing.hole_pending = 0;
}
static void audio_timing_end(void) { audio_timing.device = NULL; }
static void audio_capture_hole(uint64_t offset) { audio_timing.hole_pending = 1; audio_timing.hole_offset = offset; }
#endif

#ifdef MA_SUPPORT_WASAPI
/* Только поток устройства: API-снимок не меняет PTS и не подменяет callback-estimate.
 * WASAPI заполняется после callback; время блокирующего write видно в стадии write. */
static void audio_wasapi_playback_snapshot(ma_device* device, unsigned int frames) {
    if (device->pContext->backend != ma_backend_wasapi || device->type != ma_device_type_playback) return;
    ma_IAudioClient* client = (ma_IAudioClient*)device->wasapi.pAudioClientPlayback;
    if (!client) { audio_diag_event(device, AUDIO_DIAG_ERROR, AUDIO_DIAG_WRITE, 1, MA_INVALID_OPERATION); return; }
    struct audio_wasapi_timing timing = {0};
    MA_REFERENCE_TIME latency = 0;
    timing.frames = frames; timing.rate = device->sampleRate; timing.device_rate = device->playback.internalSampleRate;
    timing.buffer_frames = device->wasapi.actualBufferSizeInFramesPlayback;
    timing.period_frames = device->playback.internalPeriodSizeInFrames; timing.periods = device->playback.internalPeriods;
    timing.mapped_frames = device->wasapi.mappedBufferPlaybackLen;
    timing.shared = device->playback.shareMode == ma_share_mode_shared; timing.fixed_callback = !device->noFixedSizedCallback;
    timing.padding_result = ma_IAudioClient_GetCurrentPadding(client, &timing.padding_frames);
    timing.latency_result = ma_IAudioClient_GetStreamLatency(client, &latency);
    if (timing.latency_result >= 0) timing.stream_latency_us = latency / 10;
    if (timing.rate) timing.converter_us = (int64_t)(ma_data_converter_get_input_latency(&device->playback.converter)
        + device->playback.inputCacheRemaining) * 1000000 / timing.rate;
    audio_diag_wasapi_playback(device, &timing);
}
#endif

int64_t audio_stream_frame_time(ma_device* device, unsigned int frames, int* valid) {
    *valid = 0;
#ifdef __linux__
    if (audio_timing.device == device) {
        int64_t time = audio_timing.first_us + (int64_t)audio_timing.frames * 1000000 / device->sampleRate;
        audio_timing.frames += frames; *valid = audio_timing.valid;
        return time;
    }
#elif defined(MA_SUPPORT_WASAPI)
    audio_wasapi_playback_snapshot(device, frames);
#else
    (void)device; (void)frames;
#endif
    return 0;
}

int audio_stream_capture_missing(ma_device* device) {
#ifdef __linux__
    return audio_timing.device == device && audio_timing.missing;
#else
    (void)device; return 0;
#endif
}
