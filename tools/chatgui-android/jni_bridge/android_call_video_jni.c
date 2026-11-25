#ifdef __ANDROID__
#include <jni.h>
#include <string.h>
#include "../../../src/call/call_video_port.h"
#include "../../../src/call/call_video_playout.h"
#include "../../../src/call/call_audio.h"
#include "../../../src/call/call_video_bitstream.h"
#include "../../../lib/debug_config.h"
#include "../../../lib/mem.h"
#include "../../../lib/video_plane.h"

/* LongArray format: generation,width,height,fps,rotation,codec,level,maximum_bps,profile.
 * TX добавляет frame_id,pts_us,flags; RX также received_us (локальный MONOTONIC).
 * JNI не хранит заимствованные массивы. */
static int read_format(JNIEnv *e, jlongArray array, struct call_video_format *f, jlong values[12], int frame) {
    if (!array || (*e)->GetArrayLength(e, array) != (frame ? 12 : 9)) goto invalid;
    (*e)->GetLongArrayRegion(e, array, 0, frame ? 12 : 9, values);
    if ((*e)->ExceptionCheck(e)) return -1;
    if (values[0] <= 0 || values[1] < 16 || values[1] > 4096 || values[2] < 16 || values[2] > 4096
        || values[3] < 1 || values[3] > 60 || values[4] < 0 || values[4] > 270 || values[4] % 90
        || values[5] < 0 || values[5] > 2 || values[6] < 0 || values[6] > 255
        || values[7] < 300000 || values[7] > 4000000 || values[8] < 0 || values[8] > 2) goto invalid;
    *f = (struct call_video_format){values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8]};
    return 0;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call video JNI invalid format array"); return -1;
}
static int read_caps(JNIEnv *e, jbyteArray bytes, struct call_video_caps *caps) {
    if (!bytes || (*e)->GetArrayLength(e, bytes) != CALL_VIDEO_CAPS_BYTES) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call video JNI invalid CAPS array"); return -1;
    }
    uint8_t data[CALL_VIDEO_CAPS_BYTES]; (*e)->GetByteArrayRegion(e, bytes, 0, sizeof(data), (jbyte *)data);
    return (*e)->ExceptionCheck(e) ? -1 : call_video_caps_parse(data, sizeof(data), caps);
}
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_CallVideoNative_open(JNIEnv *e, jobject o, jlong id, jbyteArray bytes) {
    (void)o; struct call_video_caps caps;
    return !read_caps(e, bytes, &caps) && !call_video_port_open(id, &caps);
}
JNIEXPORT jbyteArray JNICALL Java_com_utun_chat_data_CallVideoNative_rotatableCaps(JNIEnv *e, jobject o, jbyteArray bytes) {
    (void)o; struct call_video_caps caps; uint8_t data[CALL_VIDEO_CAPS_BYTES];
    if (read_caps(e, bytes, &caps)) return NULL;
    for (unsigned i = 0; i < 2; i++) {
        if (caps.encode[i].width && call_video_limit_orientations(&caps.encode[i], i + 1)) return NULL;
        if (caps.decode[i].width && call_video_limit_orientations(&caps.decode[i], i + 1)) return NULL;
    }
    if (call_video_caps_encode(&caps, data)) return NULL;
    jbyteArray out = (*e)->NewByteArray(e, sizeof(data));
    if (out) (*e)->SetByteArrayRegion(e, out, 0, sizeof(data), (jbyte *)data);
    return out;
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_CallVideoNative_close(JNIEnv *e, jobject o, jlong id) {
    (void)e; (void)o; call_video_port_close(id);
}
JNIEXPORT jbyteArray JNICALL Java_com_utun_chat_data_CallVideoNative_peer(JNIEnv *e, jobject o, jlong id) {
    (void)o; struct call_video_port_status status; uint8_t bytes[CALL_VIDEO_CAPS_BYTES];
    if (call_video_port_status(id, &status) || !status.have_peer || call_video_caps_encode(&status.peer, bytes)) return NULL;
    jbyteArray out = (*e)->NewByteArray(e, sizeof(bytes));
    if (out) (*e)->SetByteArrayRegion(e, out, 0, sizeof(bytes), (jbyte *)bytes);
    return out;
}
JNIEXPORT jlongArray JNICALL Java_com_utun_chat_data_CallVideoNative_status(JNIEnv *e, jobject o, jlong id) {
    (void)o; struct call_video_port_status s;
    if (call_video_port_status(id, &s)) return NULL;
    jlong values[] = {s.key_serial, s.rate_generation, s.rate_request, s.rate_bps, s.configured_generation,
        s.error, s.minimum_bps, s.maximum_bps, s.decoded_generation, s.decoded_accepted, s.can_encode, s.tx_idle};
    jlongArray out = (*e)->NewLongArray(e, 12); if (out) (*e)->SetLongArrayRegion(e, out, 0, 12, values); return out;
}
/* Ограниченный диапазон BT.601. Цветность усредняется по блоку 2x2. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_CallVideoNative_rgbaPlane(JNIEnv *e, jobject o,
    jobject src, jint sr, jint sp, jobject dst, jint dr, jint dp, jint width, jint height, jint plane) {
    (void)o;
    if (!src || !dst || width < 2 || width > 4096 || height < 2 || height > 4096 || ((width | height) & 1)
        || plane < 0 || plane > 2 || sp < 4 || dp < 1) goto invalid;
    int step = plane ? 2 : 1;
    jlong src_end = (jlong)(width - 1) * sp + 4;
    jlong dst_end = (jlong)(width / step - 1) * dp + 1;
    if (sr < src_end || dr < dst_end || (*e)->GetDirectBufferCapacity(e, src) < (jlong)(height - 1) * sr + src_end
        || (*e)->GetDirectBufferCapacity(e, dst) < (jlong)(height / step - 1) * dr + dst_end) goto invalid;
    const uint8_t *rgba = (*e)->GetDirectBufferAddress(e, src);
    uint8_t *out = (*e)->GetDirectBufferAddress(e, dst);
    if (!rgba || !out) goto invalid;
    for (int y = 0; y < height; y += step) {
        const uint8_t *pixel = rgba + (size_t)y * sr;
        uint8_t *row = out + (size_t)(y / step) * dr;
        for (int x = 0; x < width; x += step) {
            const uint8_t *p = pixel + (size_t)x * sp;
            int r = p[0], g = p[1], b = p[2];
            if (plane) {
                const uint8_t *q = p + sr;
                r = (r + p[sp] + q[0] + q[sp] + 2) / 4;
                g = (g + p[sp + 1] + q[1] + q[sp + 1] + 2) / 4;
                b = (b + p[sp + 2] + q[2] + q[sp + 2] + 2) / 4;
            }
            int value = plane == 0 ? 16 + ((66*r + 129*g + 25*b + 128) >> 8)
                : plane == 1 ? 128 + ((-38*r - 74*g + 112*b + 128) >> 8) : 128 + ((112*r - 94*g - 18*b + 128) >> 8);
            row[(size_t)(x / step) * dp] = (uint8_t)value;
        }
    }
    return JNI_TRUE;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "Invalid screen RGBA geometry %dx%d plane=%d strides=%d/%d %d/%d", width, height, plane, sr, sp, dr, dp);
    return JNI_FALSE;
}
/* ByteBuffer.slice() задаёт первый пиксель и доступный limit; JNI не удерживает адреса. */
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_CallVideoNative_rotatePlane(JNIEnv *e, jobject o,
    jobject source, jint source_row, jint source_pixel, jobject destination, jint destination_row, jint destination_pixel,
    jint width, jint height, jint rotation) {
    (void)o;
    jlong source_size = source ? (*e)->GetDirectBufferCapacity(e, source) : -1;
    jlong destination_size = destination ? (*e)->GetDirectBufferCapacity(e, destination) : -1;
    if (source_size < 0 || destination_size < 0 || source_row <= 0 || source_pixel <= 0
        || destination_row <= 0 || destination_pixel <= 0 || width <= 0 || height <= 0 || rotation < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video rotate JNI invalid planes size=%lld/%lld geometry=%dx%d rotation=%d",
            (long long)source_size, (long long)destination_size, width, height, rotation);
        return JNI_FALSE;
    }
    struct video_plane src = {(*e)->GetDirectBufferAddress(e, source), source_size, source_row, source_pixel};
    struct video_plane dst = {(*e)->GetDirectBufferAddress(e, destination), destination_size, destination_row, destination_pixel};
    return !video_plane_rotate(&src, &dst, width, height, rotation);
}
JNIEXPORT jlongArray JNICALL Java_com_utun_chat_data_CallVideoNative_choose(
    JNIEnv *e, jobject o, jbyteArray local, jbyteArray remote, jlongArray desired) {
    (void)o; struct call_video_caps sender, receiver; struct call_video_format f; jlong values[12];
    if (read_caps(e, local, &sender) || read_caps(e, remote, &receiver) || read_format(e, desired, &f, values, 0)
        || call_video_choose(&sender, &receiver, &f) <= 0) return NULL;
    values[5] = f.codec; values[6] = f.level; values[8] = f.profile;
    jlongArray out = (*e)->NewLongArray(e, 9); if (out) (*e)->SetLongArrayRegion(e, out, 0, 9, values); return out;
}
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_CallVideoNative_format(
    JNIEnv *e, jobject o, jlong id, jlongArray array, jint bps) {
    (void)o; struct call_video_format f; jlong values[12];
    return !read_format(e, array, &f, values, 0) && !call_video_port_format(id, &f, bps);
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_CallVideoNative_applied(
    JNIEnv *e, jobject o, jlong id, jlong generation, jlong request, jint bps) {
    (void)e; (void)o; call_video_port_applied(id, generation, request, bps);
}
JNIEXPORT jint JNICALL Java_com_utun_chat_data_CallVideoNative_send(
    JNIEnv *e, jobject o, jlong id, jlongArray meta, jbyteArray bytes) {
    (void)o; struct call_video_frame f = {0}; jlong values[12];
    if (read_format(e, meta, &f.format, values, 1) || !bytes) return -1;
    jsize size = (*e)->GetArrayLength(e, bytes);
    if (size <= 0 || size > CALL_VIDEO_MAX_FRAME || values[9] <= 0 || values[10] < 0 || values[11] < 0 || values[11] > 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call video JNI invalid frame metadata/size=%d", size); return -1;
    }
    jbyte *data = (*e)->GetByteArrayElements(e, bytes, NULL); if (!data) return -1;
    f.call_id = id; f.frame_id = values[9]; f.pts_us = values[10]; f.flags = values[11];
    f.size = f.length = size; f.data = (uint8_t *)data;
    int result = call_video_port_send(&f, 1); (*e)->ReleaseByteArrayElements(e, bytes, data, JNI_ABORT); return result;
}
JNIEXPORT jbyteArray JNICALL Java_com_utun_chat_data_CallVideoNative_pull(JNIEnv *e, jobject o, jlong id, jlongArray meta) {
    (void)o;
    if (!meta || (*e)->GetArrayLength(e, meta) != 13) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call video JNI invalid pull metadata"); return NULL; }
    struct call_video_frame f;
    if (call_video_port_pull(id, &f) != 1) return NULL;
    jlong values[] = {f.format.generation, f.format.width, f.format.height, f.format.fps, f.format.rotation,
        f.format.codec, f.format.level, f.format.maximum_bps, f.format.profile, f.frame_id, f.pts_us, f.flags, f.received_us};
    jbyteArray out = (*e)->NewByteArray(e, f.size);
    if (out) { (*e)->SetByteArrayRegion(e, out, 0, f.size, (jbyte *)f.data); (*e)->SetLongArrayRegion(e, meta, 0, 13, values); }
    else call_video_port_request_key(id);
    u_free((void *)f.data); return out;
}
/* До join handle использует только RX worker; после join caller может завершить отложенный destroy. */
JNIEXPORT jlong JNICALL Java_com_utun_chat_data_CallVideoNative_playoutCreate(JNIEnv *e, jobject o, jlong id) {
    (void)e; (void)o;
    struct call_video_playout *p = u_malloc(sizeof(*p));
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video playout allocation failed"); return 0; }
    call_video_playout_init(p, id); return (jlong)(uintptr_t)p;
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_CallVideoNative_playoutDestroy(JNIEnv *e, jobject o, jlong handle) {
    (void)e; (void)o; u_free((void *)(uintptr_t)handle);
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_CallVideoNative_playoutReset(JNIEnv *e, jobject o, jlong handle) {
    (void)e; (void)o; struct call_video_playout *p = (void *)(uintptr_t)handle;
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video playout reset without handle"); return; }
    call_video_playout_init(p, p->call_id);
}
JNIEXPORT jlong JNICALL Java_com_utun_chat_data_CallVideoNative_playoutStep(JNIEnv *e, jobject o, jlong handle,
    jlong pts, jlong received, jlong now, jint interval_us, jint local_queued, jint reserve) {
    (void)e; (void)o; struct call_video_playout *p = (void *)(uintptr_t)handle;
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video playout poll without handle"); return 5000; }
    struct call_video_playout_frame f = {pts, received, interval_us};
    struct call_clock_snapshot clock = {0}; call_audio_get_clock(p->call_id, now, &clock);
    struct call_video_port_status status = {0}; call_video_port_status(p->call_id, &status);
    int action = call_video_playout_poll(p, pts >= 0 ? &f : NULL, now, &clock,
        status.rx_frames + (local_queued > 0 ? (unsigned)local_queued : 0), reserve > 0 ? (unsigned)reserve : 1);
    if (action == CALL_VIDEO_PLAYOUT_SHOW) return 0;
    if (action != CALL_VIDEO_PLAYOUT_WAIT) return -action;
    int64_t wait = p->deadline_us - now;
    return wait < 100 ? 100 : wait > 5000 ? 5000 : wait;
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_CallVideoNative_playoutPresented(JNIEnv *e, jobject o,
    jlong handle, jlong pts, jlong now) {
    (void)e; (void)o; struct call_video_playout *p = (void *)(uintptr_t)handle;
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video presented without playout handle"); return; }
    call_video_playout_presented(p, pts, now);
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_CallVideoNative_requestKey(JNIEnv *e, jobject o, jlong id) {
    (void)e; (void)o; call_video_port_request_key(id);
}
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_CallVideoNative_range(JNIEnv *e, jobject o, jlong id, jint minimum, jint maximum) {
    (void)e; (void)o; return !call_video_port_range(id, minimum, maximum);
}
JNIEXPORT void JNICALL Java_com_utun_chat_data_CallVideoNative_decoded(JNIEnv *e, jobject o, jlong id, jlong generation, jboolean accepted) {
    (void)e; (void)o; call_video_port_decoded(id, generation, accepted);
}
JNIEXPORT jlongArray JNICALL Java_com_utun_chat_data_CallVideoNative_inspect(JNIEnv *e, jobject o, jint codec, jbyteArray bytes) {
    (void)o;
    if (!bytes) return NULL;
    jsize size = (*e)->GetArrayLength(e, bytes);
    if (size < 9 || size > CALL_VIDEO_MAX_FRAME) { DEBUG_WARN(DEBUG_CATEGORY_CALL, "video inspect JNI size=%d", size); return NULL; }
    jbyte *data = (*e)->GetByteArrayElements(e, bytes, NULL); if (!data) return NULL;
    struct call_video_bitstream s;
    int result = call_video_inspect_key(codec, (uint8_t *)data, size, &s);
    (*e)->ReleaseByteArrayElements(e, bytes, data, JNI_ABORT);
    if (result) return NULL;
    jlong values[] = {s.width, s.height, s.level, s.profile, s.dpb};
    jlongArray out = (*e)->NewLongArray(e, 5); if (out) (*e)->SetLongArrayRegion(e, out, 0, 5, values); return out;
}
JNIEXPORT jbyteArray JNICALL Java_com_utun_chat_data_CallVideoNative_probeKey(
    JNIEnv *e, jobject o, jint codec, jint profile, jint width, jint height, jint fps) {
    (void)o; uint8_t bytes[8192]; int size = call_video_probe_key(codec, profile, width, height, fps, bytes, sizeof(bytes));
    if (size < 0) return NULL;
    jbyteArray out = (*e)->NewByteArray(e, size); if (out) (*e)->SetByteArrayRegion(e, out, 0, size, (jbyte *)bytes); return out;
}
JNIEXPORT jboolean JNICALL Java_com_utun_chat_data_CallVideoNative_supported(
    JNIEnv *e, jobject o, jbyteArray local, jbyteArray remote, jlongArray array) {
    (void)o; struct call_video_caps sender, receiver; struct call_video_format f; jlong values[12];
    return !read_caps(e, local, &sender) && !read_caps(e, remote, &receiver) && !read_format(e, array, &f, values, 0)
        && call_video_format_supported(&sender, &receiver, &f);
}
#endif
