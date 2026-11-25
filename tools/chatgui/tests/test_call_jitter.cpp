// Сохранность FIFO, десятисекундный burst, догон при продолжающемся входе и повторное накопление.
#include "call/call_jitter.h"
#include "call/call_jitter_window.h"
#include "call/call_playout_policy.h"
#include "call/call_history_store.h"
#include "call/call_audio.h"
#include "call/call.h"
#include "utun_instance.h"
#include "../../../lib/opus_codec.h"
#include "../../../lib/debug_config.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures;
static UTUN_INSTANCE audio_owner;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
struct decoder { std::vector<int> seen; };
static int decode(void* arg, const uint8_t* enc, int len, int16_t* pcm, int cap) {
    auto* d = static_cast<decoder*>(arg);
    int id = -1;
    if (len == sizeof(id)) std::memcpy(&id, enc, sizeof(id));
    d->seen.push_back(id);
    for (int i = 0; i < cap; i++) pcm[i] = (int16_t)(8000 * std::sin(2 * 3.141592653589793 * 440 * i / 48000));
    return cap;
}
struct broken_decoder { int attempts = 0; };
static int decode_bad(void* arg, const uint8_t*, int, int16_t*, int) {
    ++static_cast<broken_decoder*>(arg)->attempts;
    return -1;
}
static void push(call_jitter* j, int id, int64_t received_ms = -1) {
    call_jitter_push(j, (const uint8_t*)&id, sizeof(id), (uint32_t)(id * 20), received_ms < 0 ? id * 20 : received_ms, nullptr, 1);
}

static void test_network_recovery() {
    for (int block : {240, 480, 777, 960}) {
        decoder d;
        auto* j = call_jitter_create(decode, &d, 1000);
        std::vector<int16_t> output(block);
        int next = 0, largest_reserve = 0;
        uint32_t stable_underruns = 0;
        call_jitter_diagnostics state{};
        for (int64_t time = 0; time < 30 * 48000; time += block) {
            // Один перебой 80мс, затем приходит накопленная пачка и продолжается ровная доставка.
            bool stalled = time >= 48000 && time < 48000 + 3840;
            while (!stalled && next * 960ll <= time) push(j, next++, time / 48);
            int n = call_jitter_pull(j, output.data(), block);
            if (n < block) call_jitter_silence(j, block - n);
            call_jitter_get_diagnostics(j, &state, 0);
            largest_reserve = std::max(largest_reserve, state.reserve_ms);
            if (time < 3 * 48000) stable_underruns = state.underruns;
            else CHECK(state.underruns == stable_underruns);
        }
        CHECK(largest_reserve >= 60 && state.reserve_ms >= 20 && state.reserve_ms <= 40);
        CHECK(state.delay_mode == 0 && state.phase_samples == 0 && state.tempo_ppm == 1000000);
        CHECK(state.underruns <= 2);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "recovery block=%d max_reserve=%dms final=%dms phase=%u underruns=%u",
                   block, largest_reserve, state.reserve_ms, state.phase_samples, state.underruns);
        call_jitter_destroy(j);
    }
}

static void test_audio_clock() {
    const uint64_t id = 987;
    CHECK(call_audio_start(id) == 0);
    auto* encoder = opus_codec_encoder_create(48000, 1);
    CHECK(encoder);
    int delay = 0; CHECK(opus_codec_encoder_delay(encoder, &delay) == 0);
    int16_t voice[960], output[480]; uint8_t packet[512];
    int sent = 0, valid = 0;
    uint64_t written = 0;
    int64_t previous = -1;
    call_clock_snapshot clock{};
    CHECK(call_audio_get_clock(id, 0, &clock) == 0 && !clock.valid);
    uint64_t epoch = clock.epoch;
    // Начальный burst заставляет настоящий jitter менять темп; PTS не зависит от send_ts.
    for (int tick = 0; tick < 4000; tick++) {
        int count = tick == 0 ? 30 : (tick % 2 == 0 ? 1 : 0);
        for (int i = 0; i < count; i++, sent++) {
            for (int s = 0; s < 960; s++) voice[s] = (int16_t)(8000 * std::sin((sent * 960ll + s) * 0.09));
            int len = opus_codec_encode(encoder, voice, 960, packet, sizeof(packet)); CHECK(len > 0);
            call_capture_time timing{1000000 + sent * 20000ll, 9, (uint32_t)delay};
            call_audio_on_media(&audio_owner, id, (uint16_t)sent, (uint32_t)(get_time_tb() / 10), packet, len, &timing, 1);
        }
        CHECK(call_audio_pull_pcm(id, output, 480) == 480); written += 480;
        int64_t now = tick * 10000ll;
        call_audio_playback_progress(id, epoch, written, now, now, 1);
        CHECK(call_audio_get_clock(id, now, &clock) == 0 && clock.written_samples == written);
        call_clock_snapshot nonblocking{};
        CHECK(call_audio_try_clock(id, now, &nonblocking) == 1);
        CHECK(nonblocking.valid == clock.valid && nonblocking.pts_us == clock.pts_us && nonblocking.epoch == clock.epoch);
        CHECK(nonblocking.rate_ppm == clock.rate_ppm && nonblocking.written_samples == clock.written_samples);
        if (clock.valid) {
            valid++;
            CHECK(clock.source_epoch == 9 && clock.pts_us >= previous);
            CHECK(clock.pts_us <= 1000000 + sent * 20000ll);
            CHECK(clock.pts_us > 1000000 + sent * 20000ll - 700000);
            previous = clock.pts_us;
        }
    }
    CHECK(valid > 3900);
    // Underrun и местные тоны продолжают device-position, но удерживают source-PTS.
    int64_t held = -1;
    for (int tick = 4000; tick < 4300; tick++) {
        CHECK(call_audio_pull_pcm(id, output, 480) == 480); written += 480;
        call_audio_playback_progress(id, epoch, written, tick * 10000ll, tick * 10000ll, 1);
        CHECK(call_audio_get_clock(id, tick * 10000ll, &clock) == 0 && clock.valid);
        if (tick == 4200) held = clock.pts_us;
        if (tick > 4200) CHECK(clock.pts_us == held);
    }
    call_audio_reset_io(id, 1);
    CHECK(call_audio_get_clock(id, 43000000, &clock) == 0 && clock.epoch != epoch && !clock.valid);
    call_audio_playback_progress(id, epoch, written, 43000000, 43000000, 1);
    CHECK(call_audio_get_clock(id, 43000000, &clock) == 0 && !clock.valid);
    call_audio_stop(); opus_codec_encoder_destroy(encoder);
    CHECK(call_audio_try_clock(id, 43000000, &clock) == -1 && !clock.valid);
    std::printf("audio clock: real Opus, burst/tempo, device progress, local tones and reset checked\n");
}
static void test_packet_arrival_sampling() {
    decoder d; auto *j = call_jitter_create(decode, &d, 1000);
    for (unsigned packet = 0; packet < 4; packet++) {
        for (int frame = 0; frame < 3; frame++) {
            int id = packet * 3 + frame;
            call_jitter_push(j, (const uint8_t*)&id, sizeof(id), packet * 60,
                             1000 + packet * 60 + frame, nullptr, frame == 0);
        }
    }
    call_jitter_diagnostics diag{}; call_jitter_get_diagnostics(j, &diag, 0);
    CHECK(diag.arrival_spread_ms == 0);
    int id = 12;
    call_jitter_push(j, (const uint8_t*)&id, sizeof(id), 240, 1400, nullptr, 1);
    call_jitter_get_diagnostics(j, &diag, 0);
    CHECK(diag.arrival_spread_ms == 160);
    call_jitter_destroy(j);
}

static void test_startup_policy() {
    call_playout_policy p;
    CHECK(p.reserve_ms == 200);
    for (int t = 0; t < 10000; t += 80) p.observe(t, 20, 1000, false);
    CHECK(p.reserve_ms == 200); // Нет проигрывания — нет доказательства устойчивости.
    p.recover(10000, 1000); CHECK(p.reserve_ms == 220);
    for (int t = 10000; t < 15000; t += 80) p.observe(t, 20, 1000, true);
    CHECK(p.reserve_ms == 220);
    p.observe(15100, 20, 1000, true); CHECK(p.reserve_ms == 200);
    p.observe(15101, 350, 1000, true); CHECK(p.reserve_ms == 370);
    p.observe(15102, 20, 1000, true); CHECK(p.reserve_ms == 370);

    for (bool abrupt : {false, true}) {
    decoder d; auto* j = call_jitter_create(decode, &d, 1000);
    int next = 0, packets = 0, batch = 4, nominal = 80, due = nominal;
    int16_t pcm[480]; bool played = false; uint32_t previous_underruns = 0;
    for (int now = 0; now < 20000; now += 10) {
        if (now >= due) {
            for (int i = 0; i < batch; i++, next++)
                call_jitter_push(j, (uint8_t*)&next, sizeof(next), nominal, now, nullptr, i == 0);
            ++packets;
            if (abrupt && packets == 60) batch = 10;
            if (!abrupt && packets % 20 == 0 && batch < 10) ++batch;
            nominal += batch * 20; due = nominal + (packets % 4 == 0 ? 120 : 0);
        }
        int n = call_jitter_pull(j, pcm, 480);
        played |= n > 0;
        if (n < 480) call_jitter_silence(j, 480 - n);
        call_jitter_diagnostics state{}; call_jitter_get_diagnostics(j, &state, 0);
        if (state.underruns != previous_underruns) {
            printf("startup shortage: now=%d next=%d batch=%d due=%d depth=%d encoded=%d reserve=%d tempo=%d\n",
                   now, next, batch, due, state.depth_ms, state.encoded_ms, state.reserve_ms, state.tempo_x100);
            CHECK(state.underruns == 0); previous_underruns = state.underruns;
        }
        if (now < 5000) CHECK(state.reserve_ms >= 200);
    }
    CHECK(played);
    CHECK(previous_underruns == 0);
    call_jitter_destroy(j);
    }
    puts("startup policy: cold start, held reserve, packet size change and jitter PASS");
}

static void test_render_readiness() {
    const uint64_t id = 990;
    CHECK(call_audio_start(id) == 0);
    int16_t pcm[960] = {}; call_audio_render state{}; call_clock_snapshot clock{};
    for (int i = 0; i < 10; i++) {
        CHECK(call_audio_render_pcm(id, pcm, 960, &state) == 0);
        CHECK(state.phase == CALL_AUDIO_WAITING && state.speech_samples == 0);
    }
    CHECK(call_audio_get_clock(id, 0, &clock) == 0 && clock.written_samples == 0);
    auto* enc = opus_codec_encoder_create(48000, 1); CHECK(enc);
    uint8_t wire[512]; int len = opus_codec_encode(enc, pcm, 960, wire, sizeof(wire)); CHECK(len > 0);
    for (int i = 0; i < 12; i++) call_audio_on_media(&audio_owner, id, i, 0, wire, len, nullptr, 1);
    CHECK(call_audio_render_pcm(id, pcm, 960, &state) == 960);
    CHECK(state.phase == CALL_AUDIO_SPEECH && state.speech_samples == 960); // Закодированная тишина тоже настоящая речь.
    CHECK(call_audio_get_clock(id, 0, &clock) == 0 && clock.written_samples == 960);
    call_audio_release(id); opus_codec_encoder_destroy(enc);
}

static unsigned buffer_logs;
static void capture_buffer_log(int level, const char* category, const char* message) {
    if (std::strstr(message, "audio buffers call=")) {
        CHECK(level == DEBUG_LEVEL_DEBUG && !std::strcmp(category, "call"));
        CHECK(std::strstr(message, "missing_ms=") && std::strstr(message, "wire_Bps="));
        CHECK(std::strstr(message, "required_ms=") && std::strstr(message, "headroom_ms="));
        CHECK(std::strstr(message, "headroom_min_max_ms="));
        CHECK(std::strstr(message, "aec_over_under_total=")); // Конец строки тоже дошёл до выхода.
        ++buffer_logs;
    }
}

static void test_audio_buffer_logs() {
    CHECK(call_audio_start(900) == 0);
    uint64_t base = get_time_us() / 1000;
    auto previous = g_debug_config.category_levels[DEBUG_CATEGORY_CALL];
    debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_DEBUG); debug_set_log_hook(capture_buffer_log);
    buffer_logs = 0;
    for (unsigned offset : {200, 250, 350, 400}) {
        call_history_tx tx{}; tx.at_ms = base + offset;
        audio_owner.call_audio_ops->record_tx(900, &tx);
    }
    CHECK(buffer_logs == 2); // 50мс тики не превращаются в 20 строк в секунду.
    debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_INFO);
    call_history_tx tx{}; tx.at_ms = base + 600; audio_owner.call_audio_ops->record_tx(900, &tx);
    CHECK(buffer_logs == 2);
    debug_set_log_hook(nullptr); debug_set_category_level(DEBUG_CATEGORY_CALL, previous);
    call_audio_stop();
}

// При накоплении нет доступного звука; после полного pull запас равен реальному остатку, без отрицательных значений.
static void test_audio_headroom() {
    decoder d; auto* j = call_jitter_create(decode, &d, 1000); CHECK(j);
    int16_t pcm[960]; call_jitter_diagnostics diag{};
    for (int i = 0; i < 5; ++i) push(j, i);
    CHECK(call_jitter_pull(j, pcm, 960) == 0);
    call_jitter_get_diagnostics(j, &diag, 0);
    CHECK(diag.depth_ms == 100 && diag.required_ms == 220 && diag.headroom_ms == 0);
    for (int i = 5; i < 11; ++i) push(j, i);
    CHECK(call_jitter_pull(j, pcm, 960) == 960);
    call_jitter_get_diagnostics(j, &diag, 0);
    CHECK(diag.required_ms == 220 && diag.headroom_ms == 200);
    CHECK(call_jitter_pull(j, pcm, 960) == 960);
    call_jitter_get_diagnostics(j, &diag, 0);
    CHECK(diag.required_ms == 20 && diag.headroom_ms == 180);
    int n = 960;
    for (int i = 0; i < 20 && n == 960; ++i) n = call_jitter_pull(j, pcm, 960);
    CHECK(n >= 0 && n < 960); // При исчерпании возможен короткий последний блок; доступный запас уже нулевой.
    call_jitter_get_diagnostics(j, &diag, 0); CHECK(diag.headroom_ms == 0);
    for (int i = 11; i < 16; ++i) push(j, i);
    CHECK(call_jitter_pull(j, pcm, 960) == 0);
    call_jitter_get_diagnostics(j, &diag, 0);
    CHECK(diag.depth_ms == 100 && diag.required_ms == 240 && diag.headroom_ms == 0);
    for (int i = 16; i < 23; ++i) push(j, i);
    CHECK(call_jitter_pull(j, pcm, 960) == 960);
    call_jitter_get_diagnostics(j, &diag, 0); CHECK(diag.headroom_ms == 220);
    call_jitter_destroy(j);
    puts("audio headroom: startup/rebuffer pause=0, resumed PCM remainder and depletion PASS");
}

static void test_audio_history() {
    CallHistoryStore history;
    call_audio_history snapshot;
    history.receive(0, 100, 200, 100, 0, 0, 0); // Подготовка не получает веса тишины и доступного запаса.
    history.receive(10, 100, 200, 100, 480, 240, 0);
    history.receive(20, 200, 200, 100, 480, 0, 190);
    history.receive(10020, 300, 200, 100, 960, 0, 280);
    history.transmit({10020, 100, 300, 250});
    history.snapshot(10020, snapshot);
    CHECK(snapshot.rx_count == 2 && snapshot.tx_count == 1);
    CHECK(snapshot.rx[0].low_ms == 100 && snapshot.rx[0].high_ms == 200);
    CHECK(snapshot.rx[0].headroom_low_ms == 0 && snapshot.rx[0].headroom_high_ms == 190);
    CHECK(snapshot.rx[1].headroom_low_ms == 280 && snapshot.rx[1].headroom_high_ms == 280);
    CHECK(std::abs(snapshot.observed_ms - 30) < .001);
    CHECK(std::abs(snapshot.missing_ms - 2.5) < .001);
    CHECK(std::abs(snapshot.histogram[2] - 1.0 / 6) < .001);
    CHECK(std::abs(snapshot.histogram[6] - 2.0 / 3) < .001);
    CHECK(snapshot.tx[0].queued_bytes + snapshot.tx[0].inflight_bytes == 400); // Транспорт не складывается повторно.
    call_history_interval interval; call_history_tx latest;
    history.take_log(interval, latest);
    CHECK(interval.pulls == 4 && interval.samples == 1920 && interval.missing_samples == 240);
    CHECK(interval.low_ms == 100 && interval.high_ms == 300 && interval.last_pull_ms == 10020);
    CHECK(interval.headroom_low_ms == 0 && interval.headroom_high_ms == 280);
    CHECK(latest.queued_bytes == 100 && latest.inflight_bytes == 300);
    history.take_log(interval, latest);
    CHECK(!interval.pulls && !interval.samples && !interval.missing_samples && interval.last_pull_ms == 10020);
    history.snapshot(10020, snapshot);
    CHECK(snapshot.rx_count == 2 && std::abs(snapshot.observed_ms - 30) < .001); // Логгер не потребляет историю UI.
    history.snapshot(40020, snapshot);
    CHECK(!snapshot.rx_count && !snapshot.tx_count && snapshot.observed_ms == 0);
    for (int t = 50000; t < 90000; t += 50) history.receive(t, 4000, 200, 100, 2400, 0, 3950);
    history.snapshot(89950, snapshot);
    CHECK(snapshot.rx_count == 600 && snapshot.rx[0].at_ms == 60000);
    CHECK(snapshot.rx[0].headroom_low_ms == 3950 && snapshot.rx[599].headroom_high_ms == 3950);
    CHECK(std::abs(snapshot.histogram[60] - 1) < .001);
    CallHistoryStore traffic;
    for (unsigned i = 0; i <= 20; ++i) traffic.transmit({1000 + i * 50, 0, 0, 0, 42, 7, i * 100});
    traffic.snapshot(2000, snapshot);
    CHECK(snapshot.tx[0].wire_bytes_per_sec == -1);
    CHECK(snapshot.tx[20].wire_bytes_per_sec == 2000);
    traffic.transmit({2050, 0, 0, 0, 43, 8, 900000}); traffic.snapshot(2050, snapshot);
    CHECK(snapshot.tx[21].wire_bytes_per_sec == -1); // Смена пути не выглядит скачком трафика.
    traffic.transmit({2100, 0, 0, 0, 43, 8, 900100}); traffic.snapshot(2100, snapshot);
    CHECK(snapshot.tx[22].wire_bytes_per_sec == 2000);
    traffic.transmit({2400, 0, 0, 0, 43, 8, 900200}); traffic.snapshot(2400, snapshot);
    CHECK(snapshot.tx[23].wire_bytes_per_sec == -1); // Пропущенные измерения не интерполируются.
    traffic.transmit({2450, 0, 0, 0, 43, 8, 10}); traffic.snapshot(2450, snapshot);
    CHECK(snapshot.tx[24].wire_bytes_per_sec == -1);
    traffic.transmit({2500, 0, 0, 0, 43, 8, 110}); traffic.snapshot(2500, snapshot);
    CHECK(snapshot.tx[25].wire_bytes_per_sec == 2000); // После сброса счётчика начинаем новый интервал.
    CallHistoryStore scrolling;
    uint64_t sent = 0;
    for (unsigned i = 0; i <= 700; ++i) {
        if (i % 7 == 0) sent += 700;
        scrolling.transmit({i * 50, 0, 0, 0, 42, 7, sent});
    }
    scrolling.snapshot(35000, snapshot);
    const auto retained = snapshot.tx[40]; // Точка с полным секундным средним, после оборота кольца.
    CHECK(retained.wire_bytes_per_sec > 0);
    scrolling.snapshot(retained.at_ms + CALL_HISTORY_MS - 1, snapshot);
    CHECK(snapshot.tx[0].at_ms == retained.at_ms);
    CHECK(snapshot.tx[0].wire_bytes_per_sec == retained.wire_bytes_per_sec); // Левый край не переписывает прошлое.
    puts("audio history: time weights, startup exclusion, gaps, overflow bin and ring expiry PASS");
}

int main() {
    test_audio_history();
    test_audio_headroom();
    test_startup_policy();
    test_packet_arrival_sampling();
    CHECK(call_audio_init(&audio_owner) == 0);
    test_audio_buffer_logs();
    test_render_readiness();
    {
        call_jitter_window w;
        w.observe(0, 100); w.observe(500, 400);
        CHECK(w.low == 100 && w.high == 400 && w.reserve == 300);
        w.observe(1000, 2000); CHECK(w.reserve == 1000);
        w.observe(10000, 2000); CHECK(w.low == 2000 && w.reserve == 20);
        w.observe(30000, 80); CHECK(w.low == 80 && w.high == 80 && w.reserve == 20);
        for (int cap : {500, 2000}) {
            call_jitter_window limited;
            limited.max_reserve = cap;
            limited.observe(0, 0); limited.observe(500, 5000);
            CHECK(limited.reserve == cap);
        }
    }
    {
        call_jitter_window arrival;
        // Разные часы и переполнение uint32 не создают джиттер при ровной доставке.
        uint32_t sent = UINT32_MAX - 39;
        for (int i = 0; i < 100; i++) arrival.observe_arrival(900000 + i * 20, sent + (uint32_t)(i * 20));
        CHECK(arrival.high == 0 && arrival.low == 0 && arrival.reserve == 20);
        arrival.observe_arrival(902200, sent + 2000); // задержка доставки +200мс
        CHECK(arrival.reserve == 200);
        arrival.observe_arrival(902220, sent + 2220); // очередь пришла пачкой
        CHECK(arrival.reserve == 200);
        arrival.observe_arrival(920000, sent + 20000); // старый burst ушёл из окна
        CHECK(arrival.reserve == 20);
        // Небольшой джиттер больше минимума тоже учитывается, без прежнего порога 60мс.
        arrival.observe_arrival(920055, sent + 20020);
        CHECK(arrival.reserve == 35);
        arrival.observe_arrival(931055, sent + 31020);
        CHECK(arrival.reserve == 20);
    }
    debug_set_level(DEBUG_LEVEL_WARN);
    debug_enable_console(1);
    test_audio_clock();
    test_network_recovery();
    int16_t pcm[480];
    int depth, tempo;
    uint32_t dropped, under;
    {
        decoder d;
        auto* j = call_jitter_create(decode, &d, 1000);
        CHECK(j);
        for (int i = 0; i < 9; i++) {
            push(j, i);
            CHECK(call_jitter_pull(j, pcm, 480) == 0);
            CHECK(d.seen.empty()); // До200мс не декодируем и не расходуем накопленный звук.
        }
        call_jitter_get_stats(j, &depth, nullptr, nullptr, &under, nullptr);
        CHECK(depth == 180 && under == 0);
        push(j, 9);
        CHECK(call_jitter_pull(j, pcm, 480) == 0); // 200мс защищённого запаса плюс следующий блок.
        push(j, 10);
        CHECK(call_jitter_pull(j, pcm, 480) == 480);
        call_jitter_diagnostics diagnostic{};
        call_jitter_get_diagnostics(j, &diagnostic, 0);
        CHECK(diagnostic.resumed == 1 && diagnostic.pcm_samples == 480 && diagnostic.reserve_ms == 200);
        CHECK(diagnostic.encoded_ms >= 0 && diagnostic.processing_ms >= 0 && diagnostic.arrival_spread_ms == 0);
        call_jitter_get_diagnostics(j, &diagnostic, 0);
        CHECK(diagnostic.resumed == 1); // Снимок playback не потребляет события для uasync.
        call_jitter_get_diagnostics(j, &diagnostic, 1);
        CHECK(diagnostic.resumed == 1);
        call_jitter_get_diagnostics(j, &diagnostic, 0);
        CHECK(diagnostic.resumed == 0);
        call_jitter_range range;
        for (int i = 11; i < 511; i++) push(j, i, 10220); // 10с сетевой задержки, затем пачка
        call_jitter_get_stats(j, nullptr, nullptr, nullptr, nullptr, &range);
        CHECK(range.reserve_ms == 1000);
        int next = 511, highest = 100;
        for (int tick = 0; tick < 6000; tick++) {
            if (tick % 2 == 0) push(j, next++, 10220 + tick * 10); // непрерывный live
            call_jitter_pull(j, pcm, 480);
            call_jitter_get_stats(j, &depth, &tempo, &dropped, &under, nullptr);
            if (tempo > highest) highest = tempo;
        }
        call_jitter_get_stats(j, nullptr, nullptr, nullptr, nullptr, &range);
        CHECK(range.reserve_ms == 20 && range.max_ms >= 10000);
        CHECK(range.min_ms <= range.max_ms);
        CHECK(highest >= 150);
        CHECK(depth < 200);
        CHECK(tempo <= 110);
        CHECK(dropped == 0);
        for (size_t i = 0; i < d.seen.size(); i++) CHECK(d.seen[i] == (int)i);
        for (int i = 0; i < 100; i++) call_jitter_pull(j, pcm, 480);
        size_t before = d.seen.size();
        // После опустошения ждём полный блок сверх сетевого запаса; скрытого накопления DSP больше нет.
        push(j, next++);
        CHECK(call_jitter_pull(j, pcm, 480) == 0);
        CHECK(d.seen.size() == before);
        for (int i = 0; i < 11; i++) push(j, next++);
        int refill = call_jitter_pull(j, pcm, 480);
        if (refill != 480) {
            call_jitter_get_stats(j, &depth, nullptr, nullptr, nullptr, &range);
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "refill check: pcm=%d remaining=%dms reserve=%dms decoded=%zu",
                        refill, depth, range.reserve_ms, d.seen.size() - before);
        }
        CHECK(refill == 480);
        CHECK(d.seen.size() > before && d.seen.size() <= before + 12); // SoundTouch может подготовить несколько кадров.
        for (size_t i = before; i < d.seen.size(); i++) CHECK(d.seen[i] == (int)i);
        for (int i = 0; i < 50; i++) push(j, next++);
        CHECK(call_jitter_pull(j, pcm, 480) > 0);
        call_jitter_destroy(j);
    }
    {
        decoder d;
        auto* j = call_jitter_create(decode, &d, 1000);
        // Устройство готовилось 1.2с, но сеть всё время доставляла кадры равномерно.
        for (int i = 0; i < 60; i++) push(j, i);
        call_jitter_range range;
        call_jitter_get_stats(j, &depth, nullptr, nullptr, nullptr, &range);
        CHECK(depth == 1200 && range.reserve_ms == 200);
        CHECK(call_jitter_pull(j, pcm, 480) == 480); // первый pull сразу воспроизводит речь
        int next = 60, short_pulls = 0;
        for (int tick = 0; tick < 6000; tick++) {
            if (tick % 2 == 0) push(j, next++);
            int produced = call_jitter_pull(j, pcm, 480);
            if (produced != 480 && ++short_pulls <= 3) {
                call_jitter_diagnostics state{};
                call_jitter_get_diagnostics(j, &state, 0);
                DEBUG_ERROR(DEBUG_CATEGORY_CALL,
                            "steady delivery short pull tick=%d pcm=%d depth_before=%d encoded=%d processing=%d reserve=%d tempo=%d spread=%lld",
                            tick, produced, state.depth_ms, state.encoded_ms, state.processing_ms, state.reserve_ms,
                            state.tempo_x100, (long long)state.arrival_spread_ms);
            }
            call_jitter_get_stats(j, &depth, &tempo, &dropped, &under, &range);
            CHECK(range.reserve_ms <= 200); // догон не увеличивает сетевой резерв; начальная цель уходит постепенно.
        }
        CHECK(short_pulls == 0);
        CHECK(depth < 150 && tempo <= 110 && dropped == 0 && under == 0);
        for (size_t i = 0; i < d.seen.size(); i++) CHECK(d.seen[i] == (int)i);
        call_jitter_destroy(j);
    }
    {
        decoder d;
        auto* j = call_jitter_create(decode, &d, 1000);
        for (int i = 0; i < CALL_JITTER_MAX_FRAMES; i++) push(j, i);
        call_jitter_get_stats(j, &depth, nullptr, &dropped, nullptr, nullptr);
        CHECK(depth == 15000 && dropped == 0);
        push(j, CALL_JITTER_MAX_FRAMES);
        call_jitter_get_stats(j, &depth, nullptr, &dropped, nullptr, nullptr);
        CHECK(depth == 15000 && dropped == 1);
        CHECK(call_jitter_pull(j, pcm, 480) > 0);
        CHECK(d.seen.front() == 1);
        call_jitter_destroy(j);
    }
    for (int samples : {240, 480, 960}) {
        decoder d;
        auto* j = call_jitter_create(decode, &d, 1000);
        for (int i = 0; i < 500; i++) push(j, i);
        std::vector<int16_t> output(samples);
        int elapsed = 0, previous = 100;
        for (; elapsed < 500; ) {
            CHECK(call_jitter_pull(j, output.data(), samples) == samples);
            elapsed += samples * 1000 / 48000;
            call_jitter_get_stats(j, nullptr, &tempo, nullptr, nullptr, nullptr);
            int expected = (int)std::lround(100 + 60.0 * elapsed / 500);
            CHECK(std::abs(tempo - expected) <= 1);
            previous = tempo;
        }
        CHECK(tempo == 160);
        bool slowing = false;
        for (int tick = 0; tick < 3000 * 960 / samples; tick++) {
            call_jitter_pull(j, output.data(), samples);
            call_jitter_get_stats(j, &depth, &tempo, &dropped, nullptr, nullptr);
            int max_step = (int)std::ceil(120.0 * samples / 48000);
            CHECK(std::abs(tempo - previous) <= max_step);
            if (tempo < previous) slowing = true;
            previous = tempo;
        }
        CHECK(slowing && tempo <= 110 && dropped == 0);
        call_jitter_destroy(j);
    }
    {
        const uint64_t id = 123;
        CHECK(call_audio_start(id) == 0);
        auto* encoder = opus_codec_encoder_create(48000, 1);
        CHECK(encoder);
        int16_t voice[960];
        for (int i = 0; i < 960; i++) voice[i] = (int16_t)(8000 * std::sin(2 * 3.141592653589793 * 440 * i / 48000));
        uint8_t encoded[512];
        int len = opus_codec_encode(encoder, voice, 960, encoded, sizeof(encoded));
        CHECK(len > 0);
        // Подготовка микрофона дольше 500мс не вызывает ложный сигнал потери звука.
        for (int tick = 0; tick < 150; tick++) {
            CHECK(call_audio_pull_pcm(id, pcm, 480) == 480);
            for (auto value : pcm) CHECK(value == 0);
        }
        // Первый пакет ещё не заполнил запас PCM: продолжается тихое ожидание.
        call_audio_on_media(&audio_owner, id, 0, 0, encoded, len, nullptr, 1);
        CHECK(call_audio_pull_pcm(id, pcm, 480) == 480);
        for (auto value : pcm) CHECK(value == 0);
        for (int i = 1; i < 11; i++) call_audio_on_media(&audio_owner, id, i, 0, encoded, len, nullptr, 1);
        CHECK(call_audio_pull_pcm(id, pcm, 480) == 480);
        bool audible = false;
        for (auto value : pcm) if (value) audible = true;
        CHECK(audible);
        // После начала воспроизведения настоящий перебой по-прежнему включает тон.
        audible = false;
        for (int tick = 0; tick < 120; tick++) {
            CHECK(call_audio_pull_pcm(id, pcm, 480) == 480);
            if (tick >= 60) for (auto value : pcm) if (std::abs((int)value) > 1000) audible = true;
        }
        CHECK(audible);
        // Малого запаса недостаточно для восстановления: сигнал продолжает звучать.
        call_audio_on_media(&audio_owner, id, 11, 0, encoded, len, nullptr, 1);
        audible = false;
        for (int tick = 0; tick < 100; tick++) {
            CHECK(call_audio_pull_pcm(id, pcm, 480) == 480);
            for (auto value : pcm) if (std::abs((int)value) > 1000) audible = true;
        }
        CHECK(audible);
        for (int i = 12; i < 35; i++) call_audio_on_media(&audio_owner, id, i, 0, encoded, len, nullptr, 1);
        CHECK(call_audio_pull_pcm(id, pcm, 480) == 480);
        call_audio_begin_end(id); // явное завершение прекращает догон и включает финальный тон
        CHECK(call_audio_pull_pcm(id, pcm, 480) > 0);
        call_audio_release(id);
        opus_codec_encoder_destroy(encoder);
    }
    {
        // После PCM порог 500мс работает и для блока, который не делит его нацело.
        CHECK(call_audio_start(124) == 0);
        auto* encoder = opus_codec_encoder_create(48000, 1);
        CHECK(encoder);
        int16_t silence[960] = {};
        uint8_t encoded[512];
        int len = opus_codec_encode(encoder, silence, 960, encoded, sizeof(encoded));
        CHECK(len > 0);
        for (int i = 0; i < 11; i++) call_audio_on_media(&audio_owner, 124, i, 0, encoded, len, nullptr, 1);
        std::vector<int16_t> output(777);
        under = 0;
        for (int tick = 0; tick < 30 && !under; tick++) {
            CHECK(call_audio_pull_pcm(124, output.data(), 777) == 777);
            CHECK(call_audio_get_stats(124, nullptr, nullptr, nullptr, &under, nullptr) == 0);
        }
        CHECK(under > 0);
        // Первый недобор занимает не более 777 сэмплов; ещё 29 блоков не достигают 500мс.
        for (int tick = 0; tick < 29; tick++) {
            CHECK(call_audio_pull_pcm(124, output.data(), 777) == 777);
            for (auto value : output) CHECK(std::abs((int)value) <= 5);
        }
        bool audible = false;
        for (int tick = 0; tick < 2; tick++) {
            CHECK(call_audio_pull_pcm(124, output.data(), 777) == 777);
            for (auto value : output) if (std::abs((int)value) > 1000) audible = true;
        }
        CHECK(audible);
        call_audio_release(124);
        opus_codec_encoder_destroy(encoder);
        // Новый звонок не наследует включённый тон и отсчёт предыдущего.
        CHECK(call_audio_start(125) == 0);
        CHECK(call_audio_pull_pcm(125, output.data(), 777) == 777);
        for (auto value : output) CHECK(value == 0);
        // Завершение звучит даже у звонка, который ещё не получил первого PCM.
        call_audio_begin_end(125);
        CHECK(call_audio_pull_pcm(125, output.data(), 777) == 777);
        audible = false;
        for (auto value : output) if (value) audible = true;
        CHECK(audible);
        call_audio_release(125);
    }
    {
        broken_decoder decoder;
        auto* jitter = call_jitter_create(decode_bad, &decoder, 1000);
        for (int i = 0; i < CALL_JITTER_MAX_FRAMES; ++i) push(jitter, i);
        CHECK(call_jitter_pull(jitter, pcm, 480) == 0);
        CHECK(decoder.attempts > 0 && decoder.attempts <= 9);
        call_jitter_reset(jitter);
        int depth = 0;
        call_jitter_get_stats(jitter, &depth, nullptr, nullptr, nullptr, nullptr);
        CHECK(depth <= 1000);
        call_jitter_destroy(jitter);
    }
    call_audio_stop(); call_audio_destroy(&audio_owner);
    std::printf("call_jitter: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
