#include "debug_config.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <jni.h>
#include <dlfcn.h>
#include <atomic>
#include <algorithm>
#include <cstring>

/* Один worker Java владеет stream; callbacks только заполняют тишину и публикуют счётчики. */
struct Probe {
    AAudioStream* stream = nullptr;
    int channels = 0;
    std::atomic<int> error{0}, callbacks{0}, callbackFrames{0};
    int previousError = 0;
    int previousBuffer = -1;
};
static Probe probe;

static void log_hook(int level, const char*, const char* message) {
    int priority = level == DEBUG_LEVEL_ERROR ? ANDROID_LOG_ERROR : level == DEBUG_LEVEL_WARN ? ANDROID_LOG_WARN : ANDROID_LOG_INFO;
    __android_log_write(priority, "AudioProbe", message);
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); debug_enable_console(0); debug_set_log_hook(log_hook);
    return JNI_VERSION_1_6;
}

static aaudio_data_callback_result_t data_callback(AAudioStream*, void* context, void* output, int32_t frames) {
    auto* p = static_cast<Probe*>(context);
    std::memset(output, 0, size_t(frames) * p->channels * sizeof(int16_t));
    p->callbackFrames.store(frames, std::memory_order_relaxed);
    p->callbacks.fetch_add(1, std::memory_order_relaxed);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void error_callback(AAudioStream*, void* context, aaudio_result_t error) {
    static_cast<Probe*>(context)->error.store(error, std::memory_order_relaxed);
}

static bool check_result(const char* operation, aaudio_result_t result) {
    if (result >= 0) return true;
    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "probe operation=%s result=%d reason=%s", operation, result, AAudio_convertResultToText(result));
    return false;
}

/* Только после выхода callback: requestStop/close выполняет worker, не callback ошибки. */
extern "C" JNIEXPORT void JNICALL Java_com_utun_audioprobe_ProbeActivity_closeNative(JNIEnv*, jclass) {
    if (!probe.stream) return;
    check_result("stop", AAudioStream_requestStop(probe.stream));
    check_result("close", AAudioStream_close(probe.stream));
    probe.stream = nullptr;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "probe closed");
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_utun_audioprobe_ProbeActivity_openNative(
    JNIEnv*, jclass, jboolean exclusive, jint channels, jboolean callback) {
    if (probe.stream || (channels != 1 && channels != 2)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "probe invalid open stream=%p channels=%d", probe.stream, channels);
        return JNI_FALSE;
    }
    probe.channels = channels; probe.error = 0; probe.callbacks = 0; probe.callbackFrames = 0; probe.previousError = 0;
    probe.previousBuffer = -1;
    AAudioStreamBuilder* builder = nullptr;
    if (!check_result("builder", AAudio_createStreamBuilder(&builder))) return JNI_FALSE;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_VOICE_COMMUNICATION);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_SPEECH);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setChannelCount(builder, channels);
    AAudioStreamBuilder_setSharingMode(builder, exclusive ? AAUDIO_SHARING_MODE_EXCLUSIVE : AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    if (callback) AAudioStreamBuilder_setDataCallback(builder, data_callback, &probe);
    AAudioStreamBuilder_setErrorCallback(builder, error_callback, &probe);
    aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &probe.stream);
    check_result("delete builder", AAudioStreamBuilder_delete(builder));
    if (!check_result("open", opened)) return JNI_FALSE;
    if (AAudioStream_getFormat(probe.stream) != AAUDIO_FORMAT_PCM_I16 || AAudioStream_getChannelCount(probe.stream) != channels) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "probe unexpected format/channel count");
        check_result("close invalid", AAudioStream_close(probe.stream)); probe.stream = nullptr;
        return JNI_FALSE;
    }
    int burst = AAudioStream_getFramesPerBurst(probe.stream);
    int rate = AAudioStream_getSampleRate(probe.stream);
    int wanted = std::max(rate / 50, burst * 2);
    int actual = AAudioStream_setBufferSizeInFrames(probe.stream, wanted);
    check_result("buffer", actual);
    // Экспорт доступен не во всех версиях NDK; неизвестность не выдаём за отказ MMAP.
    auto is_mmap = reinterpret_cast<bool (*)(AAudioStream*)>(dlsym(RTLD_DEFAULT, "AAudioStream_isMMapUsed"));
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "probe opened exclusiveRequested=%d sharingActual=%d channels=%d rate=%d "
               "performance=%d mmap=%d burst=%d bufferRequested=%d bufferActual=%d capacity=%d device=%d",
               exclusive, AAudioStream_getSharingMode(probe.stream), channels, rate,
               AAudioStream_getPerformanceMode(probe.stream), is_mmap ? int(is_mmap(probe.stream)) : -1,
               burst, wanted, actual, AAudioStream_getBufferCapacityInFrames(probe.stream), AAudioStream_getDeviceId(probe.stream));
    if (!check_result("start", AAudioStream_requestStart(probe.stream))) {
        check_result("close failed start", AAudioStream_close(probe.stream)); probe.stream = nullptr;
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL Java_com_utun_audioprobe_ProbeActivity_writeNative(JNIEnv* env, jclass, jshortArray input) {
    int16_t samples[960]{};
    env->GetShortArrayRegion(input, 0, 960, samples);
    return AAudioStream_write(probe.stream, samples, std::min(960 / probe.channels, AAudioStream_getFramesPerBurst(probe.stream)), 100000000);
}

/* Timestamp опрашивается вне realtime callback. Java одинаково агрегирует AAudio и AudioTrack. */
extern "C" JNIEXPORT void JNICALL Java_com_utun_audioprobe_ProbeActivity_sampleNative(JNIEnv* env, jclass, jlongArray output) {
    if (!probe.stream || env->GetArrayLength(output) != 10) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "probe invalid sample"); return;
    }
    auto* s = probe.stream;
    int buffer = AAudioStream_getBufferSizeInFrames(s);
    if (buffer != probe.previousBuffer) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "probe buffer=%d capacity=%d written=%lld read=%lld", buffer,
            AAudioStream_getBufferCapacityInFrames(s), (long long)AAudioStream_getFramesWritten(s), (long long)AAudioStream_getFramesRead(s));
        probe.previousBuffer = buffer;
    }
    int64_t frame = -1, stamp = -1;
    int64_t written = AAudioStream_getFramesWritten(s);
    int result = AAudioStream_getTimestamp(s, CLOCK_MONOTONIC, &frame, &stamp);
    int error = probe.error.load(std::memory_order_relaxed);
    if (error && error != probe.previousError) {
        check_result("callback", error); probe.previousError = error;
    }
    jlong values[10] = {written, frame, stamp, result, AAudioStream_getSampleRate(s), AAudioStream_getXRunCount(s),
        AAudioStream_getDeviceId(s), error, probe.callbacks.load(), probe.callbackFrames.load()};
    env->SetLongArrayRegion(output, 0, 10, values);
}
