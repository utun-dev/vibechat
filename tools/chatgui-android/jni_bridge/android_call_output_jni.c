/* AAudio I/O без дополнительной PCM-очереди. Kotlin сериализует операции одного handle.
 * write передаёт по аппаратному периоду: blocking pacing AAudio держит заданный запас,
 * NON_BLOCKING способен заполнить всю ёмкость MMAP независимо от рабочего bufferSize. */
#include <aaudio/AAudio.h>
#include <jni.h>
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>
#include "debug_config.h"
#include "mem.h"
#include "call/call_audio.h"

struct call_output {
    AAudioStream* stream;
    int burst;
    int error;
};

static int64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void output_error(const char* operation, int result) {
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "AAudio %s result=%d reason=%s", operation, result, AAudio_convertResultToText(result));
}

JNIEXPORT jlong JNICALL Java_com_utun_chat_data_CallOutputNative_open(JNIEnv* env, jobject self, jint device) {
    (void)env; (void)self;
    /* Речевые атрибуты появились в API 28; libutun загружается также на API 26. */
    void (*set_usage)(AAudioStreamBuilder*, aaudio_usage_t) = dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setUsage");
    void (*set_content)(AAudioStreamBuilder*, aaudio_content_type_t) = dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setContentType");
    if (!set_usage || !set_content) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "AAudio unavailable: voice attributes missing"); return 0;
    }
    AAudioStreamBuilder* builder = NULL;
    int result = AAudio_createStreamBuilder(&builder);
    if (result < 0) { output_error("builder", result); return 0; }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    if (device > 0) AAudioStreamBuilder_setDeviceId(builder, device);
    set_usage(builder, AAUDIO_USAGE_VOICE_COMMUNICATION);
    set_content(builder, AAUDIO_CONTENT_TYPE_SPEECH);
    AAudioStream* stream = NULL;
    result = AAudioStreamBuilder_openStream(builder, &stream);
    int deleted = AAudioStreamBuilder_delete(builder);
    if (deleted < 0) output_error("builder delete", deleted);
    if (result < 0) { output_error("open", result); return 0; }
    int burst = AAudioStream_getFramesPerBurst(stream);
    int performance = AAudioStream_getPerformanceMode(stream);
    bool (*is_mmap)(AAudioStream*) = dlsym(RTLD_DEFAULT, "AAudioStream_isMMapUsed");
    int accepted = performance == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY && burst > 0 && burst <= 480 &&
        AAudioStream_getSampleRate(stream) == 48000 && AAudioStream_getChannelCount(stream) == 1 &&
        AAudioStream_getFormat(stream) == AAUDIO_FORMAT_PCM_I16 &&
        (device <= 0 || AAudioStream_getDeviceId(stream) == device);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "AAudio candidate accepted=%d device=%d requested=%d rate=%d channels=%d "
        "performance=%d sharing=%d mmap=%d burst=%d capacity=%d", accepted, AAudioStream_getDeviceId(stream), device,
        AAudioStream_getSampleRate(stream), AAudioStream_getChannelCount(stream), performance,
        AAudioStream_getSharingMode(stream), is_mmap ? is_mmap(stream) : -1, burst, AAudioStream_getBufferCapacityInFrames(stream));
    if (accepted) {
        result = AAudioStream_setBufferSizeInFrames(stream, 960);
        if (result <= 0) { output_error("buffer", result); accepted = 0; }
    }
    if (accepted) {
        result = AAudioStream_requestStart(stream);
        if (result < 0) { output_error("start", result); accepted = 0; }
    }
    if (!accepted) {
        result = AAudioStream_close(stream);
        if (result < 0) output_error("close rejected", result);
        return 0;
    }
    struct call_output* output = u_calloc(1, sizeof(*output));
    if (!output) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "AAudio allocation failed");
        result = AAudioStream_close(stream);
        if (result < 0) output_error("close allocation failure", result);
        return 0;
    }
    output->stream = stream; output->burst = burst;
    return (jlong)(intptr_t)output;
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_CallOutputNative_write(
    JNIEnv* env, jobject self, jlong handle, jshortArray input, jint offset, jint count) {
    (void)self;
    struct call_output* output = (struct call_output*)(intptr_t)handle;
    if (!output || !input || offset < 0 || count <= 0 || count > 960 || offset > (*env)->GetArrayLength(env, input) - count) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "AAudio invalid write offset=%d count=%d", offset, count); return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }
    if (output->error) return output->error;
    int16_t pcm[960];
    (*env)->GetShortArrayRegion(env, input, offset, count, pcm);
    if ((*env)->ExceptionCheck(env)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "AAudio JNI PCM copy failed"); return AAUDIO_ERROR_INTERNAL; }
    int accepted = 0;
    int64_t deadline = monotonic_ns() + 100000000;
    while (accepted < count) {
        int64_t timeout = deadline - monotonic_ns();
        if (timeout <= 0) break;
        int n = count - accepted < output->burst ? count - accepted : output->burst;
        int result = AAudioStream_write(output->stream, pcm + accepted, n, timeout);
        if (result < 0) { output_error("write", result); output->error = result; return accepted ? accepted : result; }
        if (!result) break;
        accepted += result;
    }
    return accepted;
}

JNIEXPORT jlong JNICALL Java_com_utun_chat_data_CallOutputNative_head(JNIEnv* env, jobject self, jlong handle) {
    (void)env; (void)self;
    struct call_output* output = (struct call_output*)(intptr_t)handle;
    if (!output) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "AAudio head without output"); return -1; }
    return AAudioStream_getFramesRead(output->stream);
}

/* head, timestamp frame/ns, underruns, buffer, capacity, device, performance, error, written. */
JNIEXPORT void JNICALL Java_com_utun_chat_data_CallOutputNative_sample(JNIEnv* env, jobject self, jlong handle, jlongArray out) {
    (void)self;
    struct call_output* output = (struct call_output*)(intptr_t)handle;
    if (!output || !out || (*env)->GetArrayLength(env, out) != 10) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "AAudio invalid sample"); return;
    }
    AAudioStream* s = output->stream;
    int64_t frame = -1, ns = -1;
    int result = AAudioStream_getTimestamp(s, CLOCK_MONOTONIC, &frame, &ns);
    if (result < 0) { frame = -1; ns = -1; }
    jlong values[10] = {AAudioStream_getFramesRead(s), frame, ns, AAudioStream_getXRunCount(s),
        AAudioStream_getBufferSizeInFrames(s), AAudioStream_getBufferCapacityInFrames(s), AAudioStream_getDeviceId(s),
        AAudioStream_getPerformanceMode(s), output->error, AAudioStream_getFramesWritten(s)};
    (*env)->SetLongArrayRegion(env, out, 0, 10, values);
}

JNIEXPORT jint JNICALL Java_com_utun_chat_data_CallOutputNative_buffer(JNIEnv* env, jobject self, jlong handle, jint frames) {
    (void)env; (void)self;
    struct call_output* output = (struct call_output*)(intptr_t)handle;
    if (!output || frames < 960) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "AAudio invalid buffer=%d", frames); return -1; }
    int result = AAudioStream_setBufferSizeInFrames(output->stream, frames);
    if (result < 0) output_error("buffer change", result);
    return result;
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_CallOutputNative_close(JNIEnv* env, jobject self, jlong handle) {
    (void)env; (void)self;
    struct call_output* output = (struct call_output*)(intptr_t)handle;
    if (!output) return;
    int result = AAudioStream_close(output->stream);
    if (result < 0) output_error("close", result);
    u_free(output);
}

JNIEXPORT void JNICALL Java_com_utun_chat_data_CallOutputNative_suspendClock(JNIEnv* env, jobject self, jlong id, jlong epoch) {
    (void)env; (void)self;
    call_audio_suspend_device(id, epoch);
}
