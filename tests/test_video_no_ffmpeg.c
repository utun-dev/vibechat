/* Компилировать сам модуль без FFmpeg независимо от конфигурации основной сборки. */
#undef HAVE_FFMPEG
#include "../src/video/video.c"
#include <assert.h>

static unsigned callbacks;
static void unexpected_done(void* arg, int error, const struct video_info* info) {
    (void)arg; (void)error; (void)info; callbacks++;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    struct video_info actual, zero = {0}; memset(&actual, 0x5a, sizeof(actual));
    video_ffmpeg_init();
    assert(video_probe("unused") == 0);
    assert(video_get_info("unused", &actual) == -1 && !memcmp(&actual, &zero, sizeof(actual)));
    assert(video_get_info(NULL, NULL) == -1 && video_needs_transcode(NULL) == 1);
    assert(video_transcode_start(NULL, NULL, "src", "dst", unexpected_done, NULL) == -1 && !callbacks);
    assert(video_extract_thumbnail("src", "dst", 320) == -1);
    assert(video_transcode_get_progress() == -1 && !video_transcode_is_active());
    video_codec_probe_begin(); video_codec_probe_begin();
    assert(video_codec_log_level(DEBUG_LEVEL_ERROR) == DEBUG_LEVEL_DEBUG);
    video_codec_probe_end(); assert(video_codec_log_level(DEBUG_LEVEL_WARN) == DEBUG_LEVEL_DEBUG);
    video_codec_probe_end(); assert(video_codec_log_level(DEBUG_LEVEL_ERROR) == DEBUG_LEVEL_ERROR);
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "PASS video build without FFmpeg");
    return 0;
}
