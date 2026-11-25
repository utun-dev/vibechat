/* Проверка патча FFmpeg 7.1.2 без GPU: настоящий receive_packet и AMF SetProperty на границе драйвера.
 * AllocSurface намеренно возвращает OOM после установки QP. Проверяем применение, отсутствие повторных
 * установок и повтор после отказа драйвера. Собирать с include/source и статическими библиотеками patched FFmpeg. */
#include <assert.h>
#include <wchar.h>
#include "libavcodec/amfenc.h"
#include "libavutil/opt.h"

static unsigned updates, allocations;
static const wchar_t* rejected;
static int expected;

static AMF_RESULT AMF_STD_CALL set_property(AMFComponent* component, const wchar_t* name, AMFVariantStruct value) {
    (void)component;
    assert(value.type == AMF_VARIANT_INT64 && value.int64Value == expected);
    assert(!wcscmp(name, AMF_VIDEO_ENCODER_MAX_QP) || !wcscmp(name, AMF_VIDEO_ENCODER_HEVC_MAX_QP_I)
        || !wcscmp(name, AMF_VIDEO_ENCODER_HEVC_MAX_QP_P));
    ++updates;
    return rejected && !wcscmp(name, rejected) ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL alloc_surface(AMFContext* context, AMF_MEMORY_TYPE memory, AMF_SURFACE_FORMAT format,
                                           amf_int32 width, amf_int32 height, AMFSurface** surface) {
    (void)context; (void)memory; (void)format; (void)width; (void)height; (void)surface;
    ++allocations; return AMF_OUT_OF_MEMORY;
}

static void check_encoder(const char* name, int properties) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name); assert(codec);
    AVCodecContext* context = avcodec_alloc_context3(codec); assert(context);
    AmfContext* amf = context->priv_data;
    int64_t initial = 0;
    assert(av_opt_get_int(amf, "utun_qp_max", 0, &initial) == 0 && initial == -1);
    AMFComponentVtbl component_vtable = {.SetProperty = set_property};
    AMFComponent component = {.pVtbl = &component_vtable};
    AMFContextVtbl context_vtable = {.AllocSurface = alloc_surface};
    AMFContext driver = {.pVtbl = &context_vtable};
    amf->encoder = &component; amf->context = &driver; amf->utun_qp_applied = -1;
    amf->delayed_frame = av_frame_alloc(); assert(amf->delayed_frame);
    amf->delayed_frame->buf[0] = av_buffer_alloc(1); assert(amf->delayed_frame->buf[0]);
    amf->delayed_frame->format = AV_PIX_FMT_NV12;
    AVPacket* packet = av_packet_alloc(); assert(packet);
    updates = allocations = 0; rejected = NULL; expected = 28;
    assert(av_opt_set_int(amf, "utun_qp_max", expected, 0) == 0);
    assert(ff_amf_receive_packet(context, packet) == AVERROR(ENOMEM));
    assert(updates == properties && amf->utun_qp_applied == 28 && allocations == 1);
    assert(ff_amf_receive_packet(context, packet) == AVERROR(ENOMEM));
    assert(updates == properties && allocations == 2);
    expected = 29; assert(av_opt_set_int(amf, "utun_qp_max", expected, 0) == 0);
    assert(ff_amf_receive_packet(context, packet) == AVERROR(ENOMEM));
    assert(updates == properties * 2 && amf->utun_qp_applied == 29 && allocations == 3);
    rejected = properties == 1 ? AMF_VIDEO_ENCODER_MAX_QP : AMF_VIDEO_ENCODER_HEVC_MAX_QP_P;
    expected = 30; assert(av_opt_set_int(amf, "utun_qp_max", expected, 0) == 0);
    assert(ff_amf_receive_packet(context, packet) == AVERROR_EXTERNAL);
    assert(amf->utun_qp_applied == 29 && allocations == 3);
    rejected = NULL;
    assert(ff_amf_receive_packet(context, packet) == AVERROR(ENOMEM));
    assert(updates == properties * 4 && amf->utun_qp_applied == 30 && allocations == 4);
    av_packet_free(&packet); av_frame_free(&amf->delayed_frame);
    amf->encoder = NULL; amf->context = NULL; avcodec_free_context(&context);
    av_log(NULL, AV_LOG_INFO, "Live QP native boundary passed: %s\n", name);
}

int main(void) {
    check_encoder("h264_amf", 1); check_encoder("hevc_amf", 2); return 0;
}
