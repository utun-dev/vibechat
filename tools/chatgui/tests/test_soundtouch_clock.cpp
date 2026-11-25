/* Проверяем позицию по самому сигналу: каждый четырёхсэмпловый фрагмент уникален.
 * Сравниваем номинальный feed/tempo с фактическим потреблением входа SoundTouch. */
#include "soundtouch/SoundTouch.h"
#include "../../../lib/debug_config.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "clock test line=%d: %s", __LINE__, #x); exit(1); } } while (0)

struct Span { double first, last, sourceFirst, sourceLast; };
struct Trace {
    std::vector<soundtouch::SourceSpan> spans;
    unsigned resets = 0, invalid = 0;
    soundtouch::SourceError reason = soundtouch::SourceError::None;
};
static void source_event(void* context, const soundtouch::SourceSpan& s) {
    auto& trace = *static_cast<Trace*>(context);
    if (s.kind == soundtouch::SourceKind::Reset) { trace.spans.clear(); trace.resets++; return; }
    if (s.kind == soundtouch::SourceKind::Invalid) { trace.invalid++; trace.reason = s.error; return; }
    CHECK(!trace.invalid && s.count);
    CHECK(s.output == (trace.spans.empty() ? 0 : trace.spans.back().output + trace.spans.back().count));
    trace.spans.push_back(s);
}

static void configure(soundtouch::SoundTouch& st, unsigned channels) {
    st.setSampleRate(48000); st.setChannels(channels);
    st.setSetting(SETTING_USE_QUICKSEEK, 1); st.setSetting(SETTING_SEQUENCE_MS, 20);
    st.setSetting(SETTING_SEEKWINDOW_MS, 10); st.setSetting(SETTING_OVERLAP_MS, 4);
    st.setSetting(SETTING_USE_AA_FILTER, 0);
}

/* Независимая проверка: восстановить каждый сэмпл по карте и сравнить с реальным DSP.
 * Второй SoundTouch получает тот же вход без callback: выход обязан совпадать побитово. */
static void check_exact(unsigned channels, unsigned block) {
    soundtouch::SoundTouch tracked, plain;
    configure(tracked, channels); configure(plain, channels);
    Trace trace;
    CHECK(tracked.setSourceCallback(source_event, &trace));
    std::vector<float> source, output;
    uint32_t random = 17;
    for (unsigned tick = 0; tick < 3000; tick++) {
        size_t start = source.size();
        for (unsigned i = 0; i < block * channels; i++) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            source.push_back((int32_t)(random & 0xffffff) / 16777216.0f - 0.5f);
        }
        double tempo = tick % 2 ? 1.0 : 1.6;
        tracked.setTempo(tempo); plain.setTempo(tempo);
        tracked.putSamples(source.data() + start, block); plain.putSamples(source.data() + start, block);
        // Оставляем готовый выход до следующего put, затем читаем произвольными частями.
        if (tick % 3 == 0) continue;
        for (;;) {
            float a[1554], b[1554];
            unsigned count = tracked.receiveSamples(a, 777), other = plain.receiveSamples(b, 777);
            CHECK(count == other && !std::memcmp(a, b, count * channels * sizeof(float)));
            if (!count) break;
            output.insert(output.end(), a, a + count * channels);
        }
    }
    unsigned mixed = 0, copied = 0;
    for (const auto& s : trace.spans) {
        for (unsigned i = 0; i < s.count && (s.output + i) * channels < output.size(); i++) {
            CHECK((s.input0 + i + 1) * channels <= source.size());
            for (unsigned ch = 0; ch < channels; ch++) {
                float expected = source[(s.input0 + i) * channels + ch];
                if (s.kind == soundtouch::SourceKind::Mix) {
                    CHECK((s.input1 + i + 1) * channels <= source.size());
                    float weight = (float)i / s.count;
                    expected = expected * (1.0f - weight) + source[(s.input1 + i) * channels + ch] * weight;
                }
                float actual = output[(s.output + i) * channels + ch];
                if (std::abs(actual - expected) >= 0.000002f)
                    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "source mismatch channels=%u block=%u kind=%d out=%llu input0=%llu input1=%llu"
                        " count=%u i=%u channel=%u actual=%.9g expected=%.9g", channels, block, (int)s.kind,
                        (unsigned long long)s.output, (unsigned long long)s.input0, (unsigned long long)s.input1,
                        s.count, i, ch, actual, expected);
                CHECK(std::abs(actual - expected) < 0.000002f);
                if (s.kind == soundtouch::SourceKind::Copy) CHECK(actual == expected);
            }
        }
        if (s.kind == soundtouch::SourceKind::Mix) mixed++; else copied++;
    }
    CHECK(mixed > 100 && copied > 100 && trace.resets == 1 && !trace.invalid);
    tracked.clear(); plain.clear();
    CHECK(trace.resets == 2 && trace.spans.empty());
    tracked.putSamples(source.data(), 4000); plain.putSamples(source.data(), 4000);
    CHECK(!trace.spans.empty() && trace.spans.front().output == 0 && trace.spans.front().input0 == 0);
    CHECK(!tracked.setSourceCallback(source_event, &trace) && trace.reason == soundtouch::SourceError::LateAttach);
    trace.invalid = 0;
    tracked.setRate(1.01);
    CHECK(trace.invalid == 1 && trace.reason == soundtouch::SourceError::Profile);
    tracked.setRate(1.0); tracked.flush();
    CHECK(trace.invalid == 1); // Invalid однократный, восстановление только через clear.
    tracked.clear(); trace.invalid = 0;
    tracked.putSamples(source.data(), 4000);
    tracked.flush();
    CHECK(trace.invalid == 1 && trace.reason == soundtouch::SourceError::Flush);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS exact source spans channels=%u block=%u copy=%u mix=%u PCM bit-identical",
               channels, block, copied, mixed);
}
static double position(const std::vector<Span>& spans, size_t& index, double output) {
    while (index + 1 < spans.size() && spans[index].last <= output) index++;
    const auto& s = spans[index];
    return s.sourceFirst + (s.sourceLast - s.sourceFirst) * (output - s.first) / (s.last - s.first);
}

static void check_clock(unsigned mode) {
    constexpr unsigned frames = 3000, block = 960;
    std::vector<float> source(frames * block), output;
    std::unordered_multimap<uint32_t, unsigned> locations;
    uint32_t seed = 123456789;
    for (unsigned i = 0; i < source.size(); i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        source[i] = (int32_t)(seed & 0xffffff) / 16777216.0f - 0.5f;
        uint32_t bits; std::memcpy(&bits, &source[i], sizeof(bits)); locations.emplace(bits, i);
    }
    soundtouch::SoundTouch st;
    st.setSampleRate(48000); st.setChannels(1);
    st.setSetting(SETTING_USE_QUICKSEEK, 1); st.setSetting(SETTING_SEQUENCE_MS, 20);
    st.setSetting(SETTING_SEEKWINDOW_MS, 10); st.setSetting(SETTING_OVERLAP_MS, 4);
    st.setSetting(SETTING_USE_AA_FILTER, 0);
    std::vector<Span> nominal, consumed;
    double expected = 0, mappedEnd = 0;
    unsigned produced = 0;
    for (unsigned i = 0; i < frames; i++) {
        double triangle = std::abs((int)(i % 100) - 50) / 50.0;
        double tempo = mode == 0 ? 1.6 : mode == 1 ? 1.0 + 0.6 * triangle : (i % 2 ? 1.6 : 1.0);
        st.setTempo(tempo);
        nominal.push_back({expected, expected + block / tempo, (double)i * block, (double)(i + 1) * block});
        expected += block / tempo;
        unsigned before = st.numSamples();
        st.putSamples(source.data() + i * block, block);
        unsigned added = st.numSamples() - before;
        if (added) {
            // Середина seek-window компенсирует стартовый сдвиг WSOLA; звук не изменяем.
            double end = (i + 1) * block - st.numUnprocessedSamples() + 240.0;
            consumed.push_back({(double)produced, (double)produced + added, mappedEnd, end});
            mappedEnd = end; produced += added;
        }
        float pcm[480];
        for (;;) {
            unsigned count = st.receiveSamples(pcm, 480);
            if (!count) break;
            output.insert(output.end(), pcm, pcm + count);
        }
    }
    size_t nominalIndex = 0, consumedIndex = 0;
    unsigned matches = 0;
    double maxNominal = 0, maxConsumed = 0;
    for (unsigned i = 0; i + 4 < output.size(); i += 97) {
        uint32_t bits; std::memcpy(&bits, &output[i], sizeof(bits));
        auto found = locations.equal_range(bits);
        for (auto it = found.first; it != found.second; ++it) {
            unsigned at = it->second;
            if (at + 4 > source.size() || std::memcmp(&source[at], &output[i], 4 * sizeof(float))) continue;
            maxNominal = std::max(maxNominal, std::abs(position(nominal, nominalIndex, i) - at) / 48.0);
            maxConsumed = std::max(maxConsumed, std::abs(position(consumed, consumedIndex, i) - at) / 48.0);
            matches++; break;
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "SoundTouch clock mode=%u matches=%u nominal_error_ms=%.3f consumed_error_ms=%.3f",
               mode, matches, maxNominal, maxConsumed);
    CHECK(matches > 10000);
    CHECK(maxConsumed < 20.0);
}

int main() {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    check_clock(0); check_clock(1); check_clock(2);
    check_exact(1, 240); check_exact(1, 960); check_exact(2, 777);
    return 0;
}
