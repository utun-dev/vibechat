/* Реальные ветки miniaudio на null backend: аппаратный звук и сеть не нужны. */
#include "../transport/audio_diagnostics.h"
#include "../../../lib/speex_aec.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t observed_unfilled, observed_no_progress, observed_holes;
static uint32_t callback_count;
static void test_event(const void* device, int kind, int stage, int64_t value, int64_t detail) {
    if (kind == AUDIO_DIAG_UNFILLED) observed_unfilled += (uint64_t)value;
    if (kind == AUDIO_DIAG_NO_PROGRESS) observed_no_progress += (uint64_t)value;
    if (kind == AUDIO_DIAG_HOLE) observed_holes += (uint64_t)value;
    audio_diag_event(device, kind, stage, value, detail);
}
#define MA_DIAGNOSTIC_EVENT(device, kind, stage, value, detail) test_event(device, kind, stage, value, detail)
#define MA_DIAGNOSTIC_LOG(log, level, message) audio_diag_log(log, level, message)
#define MA_PULSE_WAIT_TIMEOUT_MS 20
#define MINIAUDIO_IMPLEMENTATION
#include "../../../lib/miniaudio.h"

static void callback(ma_device* device, void* out, const void* in, ma_uint32 count) {
    (void)device; (void)in;
    callback_count++;
    if (out) for (ma_uint32 i = 0; i < count; i++) ((int16_t*)out)[i] = 12000;
}

static void starvation(int duplex) {
    ma_backend backend = ma_backend_null;
    ma_context context;
    ma_device device;
    ma_device_config config = ma_device_config_init(duplex ? ma_device_type_duplex : ma_device_type_playback);
    config.capture.format = config.playback.format = ma_format_s16;
    config.capture.channels = config.playback.channels = 1;
    config.sampleRate = 48000; config.periodSizeInFrames = 480; config.dataCallback = callback;
    assert(ma_context_init(&backend, 1, NULL, &context) == MA_SUCCESS);
    assert(ma_device_init(&context, &config, &device) == MA_SUCCESS);
    if (duplex) assert(ma_duplex_rb_init(ma_format_s16, 1, 48000, 48000, 480, NULL, &device.duplexRB) == MA_SUCCESS);
    /* Worker не запускаем: стенд синхронно вызывает backend-обработчик. */
    ma_device__set_state(&device, ma_device_state_started);
    audio_diag_label(&device, duplex ? 1 : 2, duplex ? "test-duplex" : "test-playback");
    int16_t input[480] = {0}, output[480];
    observed_unfilled = 0; callback_count = 0;
    uint64_t untouched = 0;
    for (int tick = 0; tick < 20; tick++) {
        if (duplex && tick < 3) assert(ma_device_handle_backend_data_callback(&device, NULL, input, 480) == MA_SUCCESS);
        for (int i = 0; i < 480; i++) output[i] = 2345;
        assert(ma_device_handle_backend_data_callback(&device, output, NULL, 480) == MA_SUCCESS);
        for (int i = 0; i < 480; i++) untouched += output[i] == 2345;
    }
    assert(untouched == observed_unfilled);
    assert(untouched == 0 && callback_count >= 20);
    printf("starvation duplex=%d unfilled=%llu callback_count=%u PASS\n", duplex,
           (unsigned long long)untouched, callback_count);
    ma_device__set_state(&device, ma_device_state_stopped);
    if (duplex) ma_duplex_rb_uninit(&device.duplexRB);
    ma_device_uninit(&device); ma_context_uninit(&context);
}

#ifdef MA_SUPPORT_PULSEAUDIO
static size_t zero_writable(const ma_pa_stream* stream) {
    (void)stream;
    /* Без внешнего stop callback обязан вернуть управление сам. */
    return 0;
}
static void no_progress(void) {
    ma_context context;
    ma_device device;
    memset(&context, 0, sizeof(context)); memset(&device, 0, sizeof(device));
    device.pContext = &context;
    device.playback.internalFormat = ma_format_s16; device.playback.internalChannels = 1;
    context.pulse.pa_stream_writable_size = (ma_proc)zero_writable;
    ma_device__set_state(&device, ma_device_state_started);
    observed_no_progress = 0;
    ma_device_on_write__pulse((ma_pa_stream*)&device, 960, &device);
    assert(observed_no_progress == 1);
    audio_diag_event(&device, AUDIO_DIAG_CLOSE, -1, 0, 0);
    puts("PulseAudio zero-progress detection PASS");
}
static unsigned int wait_cancelled, wait_unrefed;
static ma_pa_operation_state_t wait_state(const ma_pa_operation* operation) { (void)operation; return MA_PA_OPERATION_RUNNING; }
static void wait_cancel(ma_pa_operation* operation) { (void)operation; ++wait_cancelled; }
static void wait_unref(ma_pa_operation* operation) { (void)operation; ++wait_unrefed; }
static int wait_iterate(ma_pa_mainloop* loop, int block, int* result) {
    (void)loop; (void)result;
    assert(block == 0);
    return 0;
}
static void bounded_wait(void) {
    ma_context context;
    ma_timer timer;
    memset(&context, 0, sizeof(context));
    context.pulse.pa_operation_get_state = (ma_proc)wait_state;
    context.pulse.pa_operation_cancel = (ma_proc)wait_cancel;
    context.pulse.pa_operation_unref = (ma_proc)wait_unref;
    context.pulse.pa_mainloop_iterate = (ma_proc)wait_iterate;
    wait_cancelled = wait_unrefed = 0;
    ma_timer_init(&timer);
    assert(ma_wait_for_operation_and_unref__pulse(&context, NULL, (ma_pa_operation*)&context) == MA_TIMEOUT);
    assert(wait_cancelled == 1 && wait_unrefed == 1);
    assert(ma_timer_get_time_in_seconds(&timer) < 0.5);
    puts("PulseAudio deadline and callback cancellation PASS");
}

static unsigned int hole_frames, hole_drops;
static int hole_peek(ma_pa_stream* stream, const void** data, size_t* bytes) {
    (void)stream; *data = NULL; *bytes = 9600; return 0;
}
static int hole_drop(ma_pa_stream* stream) { (void)stream; ++hole_drops; return 0; }
static void hole_capture(ma_device* device, void* out, const void* in, ma_uint32 frames) {
    (void)device; (void)out;
    assert(in);
    for (ma_uint32 i = 0; i < frames; ++i) assert(((const int16_t*)in)[i] == 0);
    hole_frames += frames;
}
static void capture_hole(void) {
    ma_backend backend = ma_backend_null;
    ma_context context;
    ma_device device;
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16; config.capture.channels = 1; config.sampleRate = 48000;
    config.noFixedSizedCallback = MA_TRUE; config.dataCallback = hole_capture;
    assert(ma_context_init(&backend, 1, NULL, &context) == MA_SUCCESS);
    assert(ma_device_init(&context, &config, &device) == MA_SUCCESS);
    context.pulse.pa_stream_peek = (ma_proc)hole_peek;
    context.pulse.pa_stream_drop = (ma_proc)hole_drop;
    observed_holes = hole_frames = hole_drops = 0;
    ma_device__set_state(&device, ma_device_state_started);
    ma_device_on_read__pulse((ma_pa_stream*)&device, 9600, &device);
    assert(observed_holes == 4800 && hole_frames == 4800 && hole_drops == 1);
    ma_device__set_state(&device, ma_device_state_stopped);
    ma_device_uninit(&device); ma_context_uninit(&context);
    puts("PulseAudio capture hole preserves duration and silence PASS");
}

#endif

/* Характеризация найденной потери хвоста; после исправления ожидаем два кадра. */
static void aec_variable_chunks(void) {
    speex_aec_t* aec = speex_aec_create(48000, 960, 14400, 8);
    int16_t pcm[1920] = {0};
    assert(aec != NULL);
    speex_aec_feed_playback(aec, pcm, 1200);
    speex_aec_feed_playback(aec, pcm + 1200, 720);
    const int frames = speex_aec_delay_fill(aec);
    assert(frames == 2);
    printf("AEC characterization: input=1920 queued=%d*960; all samples preserved\n", frames);
    speex_aec_destroy(aec);
}

int main(void) {
    audio_diag_start();
    starvation(1); starvation(0);
#ifdef MA_SUPPORT_PULSEAUDIO
    no_progress();
    bounded_wait();
    capture_hole();
#endif
    aec_variable_chunks();
    audio_diag_stop();
    return 0;
}
