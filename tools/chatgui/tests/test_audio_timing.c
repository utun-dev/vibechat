/* PA snapshot без сервера: несколько converter callbacks принадлежат одному physical PCM-блоку. */
#define MA_DATA_CONVERTER_STACK_BUFFER_SIZE 128
#include "../transport/miniaudio_impl.c"
#include <assert.h>
#include <stdio.h>
static int64_t times[64];
static unsigned counts[64], seen;
static int capture_missing;
static unsigned overruns, underruns;
static void notification(const ma_device_notification* event) {
    if (event->type == ma_device_notification_type_capture_overrun) overruns++;
    if (event->type == ma_device_notification_type_playback_underrun) underruns++;
}
static int fake_hole(const void* stream, const void** data, size_t* bytes) {
    (void)stream; *data = NULL; *bytes = 64 * 2 * sizeof(float); return 0;
}
static int fake_drop(void* stream) { (void)stream; return 0; }
static int fake_latency(const void* stream, uint64_t* latency, int* negative) {
    (void)stream; *latency = 100000; *negative = 0; return 0;
}
static void observe(ma_device* device, void* output, const void* input, ma_uint32 frames) {
    (void)output; (void)input;
    int measured = 0;
    assert(seen < 64);
    times[seen] = audio_stream_frame_time(device, frames, &measured);
    counts[seen++] = frames;
    assert(measured && seen < 64);
    assert(audio_stream_capture_missing(device) == capture_missing);
}
static int64_t time_us(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * INT64_C(1000000) + now.tv_nsec / 1000;
}
int main(void) {
    /* Только dispatch/converter: backend union не подменяется у живого null-device. */
    ma_context context = {0}; ma_device device = {0};
    device.pContext = &context; device.type = ma_device_type_capture;
    device.sampleRate = 48000; device.noFixedSizedCallback = MA_TRUE; device.onData = observe;
    device.capture.format = ma_format_s16; device.capture.channels = 1;
    device.capture.internalFormat = ma_format_f32; device.capture.internalChannels = 2;
    assert(ma_device_set_master_volume(&device, 1) == MA_SUCCESS);
    ma_data_converter_config converter = ma_data_converter_config_init(ma_format_f32, ma_format_s16, 2, 1, 48000, 48000);
    assert(ma_data_converter_init(&converter, NULL, &device.capture.converter) == MA_SUCCESS);
    device.capture.internalSampleRate = 48000;
    context.backend = ma_backend_pulseaudio;
    context.pulse.pa_stream_get_latency = (ma_proc)fake_latency;
    device.pulse.pStreamCapture = (void*)1;
    float input[180 * 2] = {0};
    int64_t now = time_us();
    assert(ma_device_handle_backend_data_callback(&device, NULL, input, 180) == MA_SUCCESS);
    int64_t after = time_us();
    assert(seen == 3 && counts[0] == 64 && counts[1] == 64 && counts[2] == 52);
    assert(times[0] >= now - 100000 && times[0] <= after - 100000);
    for (unsigned i = 1; i < seen; ++i) {
        int64_t difference = times[i] - times[0] - (int64_t)i * 64 * 1000000 / 48000;
        assert(difference >= -1 && difference <= 1);
    }
    capture_missing = 1; seen = 0; audio_capture_hole(120);
    now = time_us();
    assert(ma_device_handle_backend_data_callback(&device, NULL, input, 64) == MA_SUCCESS);
    after = time_us();
    assert(seen == 1 && times[0] >= now - 100000 + 2500 && times[0] <= after - 100000 + 2500);
    assert(!audio_stream_capture_missing(&device)); // Hint действует только внутри dispatch.
    device.onNotification = notification;
    ma_device__set_state(&device, ma_device_state_started);
    context.pulse.pa_stream_peek = (ma_proc)fake_hole; context.pulse.pa_stream_drop = (ma_proc)fake_drop;
    seen = 0;
    ma_device_on_read__pulse(device.pulse.pStreamCapture, 64 * 2 * sizeof(float), &device);
    assert(overruns == 1 && underruns == 0 && seen == 4); // Один hole, четыре converter callbacks.
    audio_diag_pulse_overflow(device.pulse.pStreamCapture, &device);
    assert(overruns == 2 && underruns == 0);
    audio_diag_pulse_underflow(NULL, &device);
    assert(overruns == 2 && underruns == 1);
    ma_data_converter_uninit(&device.capture.converter, NULL);
    puts("audio timing: capture read index, converter offsets, hole/overrun/underrun notifications PASS");
}
