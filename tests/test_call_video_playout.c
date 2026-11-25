/* Управляемые arrival/presentation clocks: FIFO сохраняется при паузах сети/CPU/GUI. */
#include <stdlib.h>
#include "../src/call/call_video_playout.h"
#include "../lib/debug_config.h"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "playout test line=%d: %s", __LINE__, #x); exit(1); } } while (0)
static const int64_t origin = 1000000;

static void simulate(unsigned pause_ms, int cpu, int64_t quantum, int accelerated_audio) {
    struct call_video_playout p; call_video_playout_init(&p, 7);
    enum { COUNT = 480 };
    struct call_video_playout_frame frames[COUNT];
    for (int i = 0; i < COUNT; ++i) {
        int64_t arrival = origin + i * 33333ll;
        if (!cpu && arrival >= origin + 1000000 && arrival < origin + 1000000 + pause_ms * 1000ll)
            arrival = origin + 1000000 + pause_ms * 1000ll;
        frames[i] = (struct call_video_playout_frame){origin + i * 33333ll, arrival, 33333};
    }
    unsigned selected = 0, shown = 0, skipped = 0, available = 0, accelerated = 0;
    int64_t last_show = 0, ack_at = 0, recovered_lag = INT64_MAX, audio_pts = origin;
    for (int64_t now = origin; now < origin + 22000000 && shown + skipped < COUNT; now += quantum) {
        uint32_t rate = accelerated_audio && now >= origin + 1500000 && now < origin + 2500000 ? 1100000 : 1000000;
        if (now > origin) audio_pts += quantum * rate / 1000000;
        if (cpu && now >= origin + 1000000 && now < origin + 1000000 + pause_ms * 1000ll) continue;
        while (available < COUNT && frames[available].received_us <= now) ++available;
        if (ack_at && now >= ack_at) {
            call_video_playout_presented(&p, frames[selected - 1].pts_us, now);
            CHECK(!last_show || now > last_show); last_show = now; ++shown; ack_at = 0;
        }
        struct call_clock_snapshot audio = {.pts_us = audio_pts, .rate_ppm = rate,
            .valid = !(now >= origin + 4000000 && now < origin + 4100000)};
        const struct call_video_playout_frame *f = selected < available ? frames + selected : NULL;
        int action = call_video_playout_poll(&p, f, now, &audio, available - shown - skipped, 1);
        if (now >= origin + 6000000 && shown) recovered_lag = audio.pts_us - p.last_show_pts;
        if (action == CALL_VIDEO_PLAYOUT_WAIT) continue;
        if (action == CALL_VIDEO_PLAYOUT_DROP_LATE) { ++selected; ++skipped; continue; }
        CHECK(action == CALL_VIDEO_PLAYOUT_SHOW && audio.valid && !ack_at);
        CHECK(f->pts_us - audio.pts_us <= 40000);
        if (p.speed_ppm > 1050000) ++accelerated;
        ++selected; ack_at = now + 3000; // Не мгновенный GUI: кадр остаётся в общем запасе до ACK.
        CHECK(call_video_playout_poll(&p, selected < available ? frames + selected : NULL, now, &audio,
            available - shown - skipped, 1) == CALL_VIDEO_PLAYOUT_WAIT);
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "presentation recovery pause_ms=%u cpu=%d quantum_us=%lld audio_fast=%d shown=%u skipped=%u total=%u lag_us=%lld",
        pause_ms, cpu, (long long)quantum, accelerated_audio, shown, skipped, COUNT, (long long)recovered_lag);
    CHECK(shown + skipped == COUNT && accelerated);
    if (quantum < 2000) CHECK(skipped == 0); // При достаточной частоте показа ускорение сохраняет все кадры.
    CHECK(recovered_lag < (quantum < 2000 ? 100000 : 220000));
}

static void presentation_deadline(void) {
    struct call_video_playout p; call_video_playout_init(&p, 9);
    struct call_video_playout_frame f = {origin, origin, 33333};
    struct call_clock_snapshot audio = {.pts_us = origin, .rate_ppm = 1000000, .valid = 1};
    CHECK(call_video_playout_poll(&p, &f, origin, &audio, 2, 1) == CALL_VIDEO_PLAYOUT_SHOW);
    CHECK(p.shown == 0 && p.awaiting); // Выбор кадра ещё не показ.
    call_video_playout_presented(&p, f.pts_us, origin);
    CHECK(p.shown == 1 && !p.awaiting);
    CHECK(call_video_playout_poll(&p, NULL, origin + 5000, &audio, 0, 1) == CALL_VIDEO_PLAYOUT_WAIT);
    f.pts_us += 33333; f.received_us = origin + 12000; audio.pts_us += 12000;
    CHECK(call_video_playout_poll(&p, &f, origin + 12000, &audio, 1, 1) == CALL_VIDEO_PLAYOUT_WAIT);
    int64_t deadline = p.deadline_us;
    audio.pts_us += 12000;
    CHECK(call_video_playout_poll(&p, &f, origin + 24000, &audio, 8, 1) == CALL_VIDEO_PLAYOUT_WAIT);
    CHECK(p.deadline_us == deadline); // Приход пачки и движение audio clock не двигают уже назначенный срок.
    CHECK(call_video_playout_poll(&p, &f, origin + 35000, &audio, 8, 1) == CALL_VIDEO_PLAYOUT_SHOW);
    call_video_playout_presented(&p, f.pts_us, origin + 40000);
    CHECK(p.last_show_us == origin + 40000);
    CHECK(call_video_playout_poll(&p, &f, origin + 41000, &audio, 1, 1) == CALL_VIDEO_PLAYOUT_DROP_ORDER);
    f.pts_us += 33333; audio.valid = 0;
    CHECK(call_video_playout_poll(&p, &f, origin + 200000, &audio, 1, 1) == CALL_VIDEO_PLAYOUT_WAIT);
    audio.valid = 1; audio.pts_us = origin + 200000;
    CHECK(call_video_playout_poll(&p, &f, origin + 200000, &audio, 1, 1) == CALL_VIDEO_PLAYOUT_SHOW);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS presentation clock: 12ms starvation does not collapse cadence; ACK alone advances time");
}

static void queued_capture_gap(void) {
    const int64_t gaps[] = {500000, 2264933, 10000000};
    const unsigned fps[] = {15, 30, 60};
    for (unsigned i = 0; i < 3; ++i) for (unsigned j = 0; j < 3; ++j) {
        struct call_video_playout p; call_video_playout_init(&p, 11);
        unsigned interval = 1000000 / fps[j];
        struct call_video_playout_frame first = {origin, origin, interval}, next = {origin + gaps[i], origin, interval};
        struct call_clock_snapshot audio = {.pts_us = next.pts_us + 1000000, .valid = 1, .rate_ppm = 1000000};
        CHECK(call_video_playout_poll(&p, &first, origin, &audio, 2, 1) == CALL_VIDEO_PLAYOUT_SHOW);
        call_video_playout_presented(&p, first.pts_us, origin);
        CHECK(call_video_playout_poll(&p, &next, origin, &audio, 1, 1) == CALL_VIDEO_PLAYOUT_WAIT);
        CHECK(call_video_playout_poll(&p, &next, origin + interval + 2000, &audio, 1, 1) == CALL_VIDEO_PLAYOUT_SHOW);
        CHECK(p.last_pts == next.pts_us && p.source_gaps == 1);
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS queued capture gaps: 0.5/2.265/10s at 15/30/60fps, no repeated wait");
}

static void slowing_with_queue(void) {
    struct call_video_playout p; call_video_playout_init(&p, 12);
    struct call_clock_snapshot audio = {.pts_us = origin, .valid = 1, .rate_ppm = 1000000};
    struct call_video_playout_frame f = {origin, origin, 33333};
    uint32_t fast = 0; unsigned shows = 0;
    for (int64_t now = origin; now < origin + 2000000; now += 1000) {
        audio.pts_us = now;
        unsigned queue = now < origin + 1000000 ? 8 : 2;
        int action = call_video_playout_poll(&p, &f, now, &audio, queue, 1);
        if (action == CALL_VIDEO_PLAYOUT_SHOW) {
            call_video_playout_presented(&p, f.pts_us, now);
            f.pts_us += 33333; ++shows;
        }
        if (now == origin + 999000) fast = p.speed_ppm;
    }
    CHECK(shows && fast > 1500000 && p.speed_ppm < 1050000);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS queue governor: speed_ppm=%u -> %u as total frames fall 8 -> 2", fast, p.speed_ppm);
}

static void render_latency_change(void) {
    struct call_video_playout p; call_video_playout_init(&p, 13);
    struct call_video_playout_frame f = {origin, origin, 33333};
    struct call_clock_snapshot audio = {.valid = 1, .rate_ppm = 1000000};
    int64_t now = origin;
    for (unsigned i = 0; i < 240; ++i) {
        f.pts_us = origin + i * 33333ll; audio.pts_us = f.pts_us + 100000;
        int action = call_video_playout_poll(&p, &f, now, &audio, 2, 1);
        if (action == CALL_VIDEO_PLAYOUT_WAIT) {
            now = p.deadline_us;
            CHECK(call_video_playout_poll(&p, &f, now, &audio, 2, 1) == CALL_VIDEO_PLAYOUT_SHOW);
        } else CHECK(action == CALL_VIDEO_PLAYOUT_SHOW);
        now += i < 120 ? 3000 : 9000;
        call_video_playout_presented(&p, f.pts_us, now);
        ++now;
    }
    int64_t phase = p.last_show_us - (origin + 239 * 33333ll);
    CHECK(phase >= 0 && phase < 15000);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS render latency 3->9ms: phase_us=%lld; no accumulated per-frame GUI delay", (long long)phase);
}

/* Вход 60 fps, показ ограничен 60 Hz: после паузы ускориться физически нельзя.
 * Каждый вход считается decoded; теряется лишь показ, ACK остаётся единственным владельцем часов. */
static void limited_display(unsigned pause_ms) {
    enum { COUNT = 600 };
    struct call_video_playout p; call_video_playout_init(&p, 14);
    unsigned consumed = 0, shown = 0, skipped = 0;
    int64_t ack_at = 0, pending_pts = 0, recovered = INT64_MAX;
    for (int64_t now = origin; now < origin + 12000000 && consumed < COUNT; now += 1000) {
        unsigned available = (unsigned)((now - origin) * 60 / 1000000) + 1;
        if (available > COUNT) available = COUNT;
        if (ack_at && now >= ack_at) {
            call_video_playout_presented(&p, pending_pts, now); ++shown; ack_at = 0;
        }
        if (now >= origin + 1000000 && now < origin + 1000000 + pause_ms * 1000ll) continue;
        if (consumed >= available) continue;
        struct call_video_playout_frame f = {origin + consumed * 1000000ll / 60, origin, 16666};
        struct call_clock_snapshot audio = {.pts_us = now, .rate_ppm = 1000000, .valid = 1};
        const int64_t before = p.last_show_us, deadline = p.last_deadline;
        int action = call_video_playout_poll(&p, &f, now, &audio, available - consumed + (ack_at != 0), 1);
        if (action == CALL_VIDEO_PLAYOUT_SHOW) {
            ++consumed; pending_pts = f.pts_us;
            ack_at = origin + ((now - origin) * 60 / 1000000 + 1) * 1000000 / 60;
        } else if (action == CALL_VIDEO_PLAYOUT_DROP_LATE) {
            ++consumed; ++skipped;
            CHECK(p.last_show_us == before && p.last_deadline == deadline && !p.awaiting);
        } else CHECK(action == CALL_VIDEO_PLAYOUT_WAIT);
        if (now > origin + 6000000) recovered = now - f.pts_us;
    }
    if (ack_at) { call_video_playout_presented(&p, pending_pts, ack_at); ++shown; }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "60Hz recovery pause_ms=%u decoded=%u shown=%u skipped=%u lag_us=%lld",
        pause_ms, consumed, shown, skipped, (long long)recovered);
    CHECK(consumed == COUNT && shown + skipped == COUNT);
    if (!pause_ms) CHECK(skipped == 0);
    else CHECK(skipped > 0 && recovered < 130000);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    const unsigned pauses[] = {50, 100, 200, 500};
    for (unsigned i = 0; i < sizeof(pauses) / sizeof(pauses[0]); ++i)
        for (int cpu = 0; cpu <= 1; ++cpu) {
            simulate(pauses[i], cpu, 1000, 0);
            simulate(pauses[i], cpu, 15625, 1);
        }
    presentation_deadline(); queued_capture_gap(); slowing_with_queue(); render_latency_change();
    limited_display(0); limited_display(50); limited_display(100); limited_display(500); return 0;
}
