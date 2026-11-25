#include "call_delay.h"
#include "../../lib/debug_config.h"
#include <algorithm>
#include <cmath>

using soundtouch::SourceKind;
using soundtouch::SourceError;
using soundtouch::SourceSpan;

// Android собирает общий голосовой стек в C++11: константам, переданным по ссылке, нужны определения.
constexpr unsigned CallDelay::Rate;
constexpr unsigned CallDelay::Frame;
constexpr unsigned CallDelay::Capacity;
constexpr unsigned CallDelay::Context;

static int16_t to_pcm(float value) {
    return (int16_t)std::lrintf(std::max(-32768.0f, std::min(32767.0f, value * 32768.0f)));
}

CallDelay::CallDelay(Input reader, void* readerContext, soundtouch::SourceCallback sink, void* sinkContext)
    : input(reader), inputContext(readerContext), source(sink), sourceContext(sinkContext) {
    st.setSampleRate(Rate); st.setChannels(1); st.setTempo(1);
    st.setSetting(SETTING_USE_QUICKSEEK, 1);
    st.setSetting(SETTING_SEQUENCE_MS, 20); st.setSetting(SETTING_SEEKWINDOW_MS, 10);
    st.setSetting(SETTING_OVERLAP_MS, 4); st.setSetting(SETTING_USE_AA_FILTER, 0);
    if (!input || !source || !st.setSourceCallback([](void* p, const SourceSpan& s) {
            static_cast<CallDelay*>(p)->onSource(s);
        }, this)) fail("initialization");
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call_delay: initialized direct_max=1.02 stretch_max=1.6 enter=120ms exit=40ms");
}

void CallDelay::fail(const char* reason) {
    if (failed) return;
    failed = true;
    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call_delay: %s mode=%d input=%llu cursor=%llu generated=%llu consumed=%llu spans=%u",
                reason, (int)mode, (unsigned long long)written, (unsigned long long)cursor,
                (unsigned long long)generated, (unsigned long long)consumed, spanCount);
}

/* Reset принадлежит аудиосессии, а не переключению алгоритма. */
void CallDelay::reset() {
    st.clear(); st.setTempo(1);
    spanHead = spanCount = trim = 0;
    written = cursor = generated = consumed = stBase = stFed = entry = queuedInput = deviceSamples = 0;
    phaseGoal = 0;
    playedSource = cutPhase = 0; tempo = 1;
    for (auto& b : buckets) b = {};
    firstSpan = lastCopy = false; mode = Mode::Direct;
    cuts = entered = exited = 0;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call_delay: reset failed=%d", failed);
}

CallDelay::Stats CallDelay::stats() const {
    Stats result;
    result.mode = mode; result.tempo = tempo; result.pending = std::max(0.0, written - playedSource);
    result.cuts = cuts; result.entered = entered; result.exited = exited;
    result.phase = (unsigned)((cursor % Frame + Frame - deviceSamples % Frame) % Frame);
    return result;
}

void CallDelay::silence(unsigned count) { deviceSamples += count; }

/* Сохраняем контекст и ещё не выданный исходный хвост даже после передачи в DSP. */
bool CallDelay::fill() {
    uint64_t keep = cursor > Context ? cursor - Context : 0;
    if (written - keep + Frame > Capacity) { fail("raw history overflow"); return false; }
    int16_t pcm[Frame];
    unsigned count = input(inputContext, pcm, Frame);
    if (!count) return false;
    if (count > Frame) { fail("invalid input count"); return false; }
    queuedInput -= std::min<uint64_t>(queuedInput, count);
    for (unsigned i = 0; i < count; ++i) raw[(written + i) % Capacity] = pcm[i] / 32768.0f;
    written += count;
    return true;
}

/* Общая выходная нумерация сохраняется при любых переходах. */
void CallDelay::emit(SourceSpan s) {
    if (failed) return;
    if (!s.count || spanCount == spans.size()) { fail("source queue overflow"); return; }
    s.output = generated;
    spans[(spanHead + spanCount++) % spans.size()].value = s;
    generated += s.count;
    source(sourceContext, s);
}

/* Начальный Copy обрезается только по уже выданному контексту, не по новому звуку. */
void CallDelay::onSource(const SourceSpan& local) {
    if (local.kind == SourceKind::Reset) return;
    if (local.kind == SourceKind::Invalid) { fail("SoundTouch source invalid"); return; }
    SourceSpan s = local;
    s.input0 += stBase; s.input1 += stBase;
    if (firstSpan) {
        if (s.kind != SourceKind::Copy || s.input0 > entry || s.input0 + s.count <= entry) {
            fail("SoundTouch entry does not cover boundary"); return;
        }
        trim = (unsigned)(entry - s.input0);
        s.input0 += trim; s.count -= trim;
        firstSpan = false;
    }
    lastCopy = s.kind == SourceKind::Copy;
    if (lastCopy) cursor = s.input0 + s.count;
    emit(s);
}

/* Глубина учитывает фактическое происхождение прочитанного PCM, а не текущий tempo. */
void CallDelay::consume(unsigned count) {
    while (count && !failed) {
        if (!spanCount) { fail("source queue underflow"); return; }
        const auto& s = spans[spanHead].value;
        unsigned take = (unsigned)std::min<uint64_t>(count, s.output + s.count - consumed);
        consumed += take; count -= take;
        double offset = (double)(consumed - s.output);
        playedSource = s.input0 + offset;
        if (s.kind == SourceKind::Mix) playedSource += ((double)s.input1 - s.input0) * offset / s.count;
        if (consumed == s.output + s.count) { spanHead = (spanHead + 1) % spans.size(); --spanCount; }
    }
}

/* История перед границей компенсирует стартовый сдвиг интерполятора; величина берётся из SourceSpan. */
void CallDelay::beginStretch() {
    st.clear(); st.setTempo(tempo);
    entry = cursor; stBase = cursor - Context; stFed = stBase;
    firstSpan = true; lastCopy = false; trim = 0; mode = Mode::Stretch;
    ++entered;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call_delay: direct->stretch source=%llu pending=%.2fms tempo=%.4f",
                (unsigned long long)cursor, stats().pending / 48, tempo);
}

/* Готовый выход обязательно заканчивается Copy. Хвост raw начинается с его точного конца. */
void CallDelay::endStretch() {
    if (st.numSamples() || spanCount || (!firstSpan && !lastCopy)) { fail("unsafe stretch exit"); return; }
    st.clear(); mode = Mode::Direct; cutPhase = 0;
    tempo = std::min(tempo, 1.02); ++exited;
    int64_t offset = (int64_t)cursor - (int64_t)deviceSamples;
    phaseGoal = (int64_t)std::ceil((double)offset / Frame) * Frame;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call_delay: stretch->direct source=%llu recovered=%.2fms tempo=%.4f",
                (unsigned long long)cursor, (written - cursor) / 48.0, tempo);
}

/* Подаём небольшие порции, чтобы готовый выход и история оставались ограниченными. */
bool CallDelay::pump() {
    while (!st.numSamples() && !failed) {
        if (stFed == written && !fill()) return false;
        float pcm[Frame];
        unsigned count = (unsigned)std::min<uint64_t>(Frame, written - stFed);
        for (unsigned i = 0; i < count; ++i) pcm[i] = raw[(stFed + i) % Capacity];
        stFed += count;
        st.putSamples(pcm, count);
        if (trim) {
            if (st.receiveSamples(trim) != trim) { fail("missing entry context"); return false; }
            trim = 0;
        }
    }
    return !failed;
}

/* Сэмплы удаляются по одному; после ограничения запасом долг удаления не накапливается. */
unsigned CallDelay::direct(int16_t* out, unsigned count, unsigned reserve, unsigned remaining) {
    unsigned done = 0, run = 0;
    uint64_t first = cursor;
    auto finish = [&] {
        if (run) { emit({SourceKind::Copy, SourceError::None, 0, first, 0, run}); consume(run); run = 0; }
    };
    while (done < count && !failed) {
        if (cursor == written && !fill()) break;
        if (cutPhase >= 1) {
            cutPhase -= std::floor(cutPhase);
            uint64_t available = written - cursor + queuedInput;
            int64_t offset = (int64_t)cursor - (int64_t)deviceSamples;
            if (offset < phaseGoal && available > (uint64_t)reserve + remaining - done
                && (written - cursor > 1 || fill())) {
                finish(); ++cursor; ++cuts;
            }
        }
        if (!run) first = cursor;
        out[done++] = to_pcm(raw[cursor++ % Capacity]); ++run; ++deviceSamples;
        if ((int64_t)cursor - (int64_t)deviceSamples < phaseGoal) cutPhase += std::min(.02, tempo - 1);
        else cutPhase = 0;
    }
    finish();
    return done;
}

/* Окно покрывает период пачки и вариацию доставки; размер pull не влияет на его длительность. */
double CallDelay::lowDepth(double depth, double& inventory) {
    uint64_t time = deviceSamples / (Rate / 20);
    auto& b = buckets[time % 48];
    if (b.time != time) { b.time = time; b.low = depth; b.inventory = inventory; }
    else { b.low = std::min(b.low, depth); b.inventory = std::min(b.inventory, inventory); }
    for (const auto& item : buckets) if (item.time <= time && time - item.time < observationBuckets) {
        depth = std::min(depth, item.low); inventory = std::min(inventory, item.inventory);
    }
    return depth;
}

unsigned CallDelay::pull(int16_t* out, unsigned count, uint64_t queued, unsigned reserve, unsigned observation_ms, bool protect_reserve) {
    if (failed) return 0;
    if (!out || !count) { fail("invalid pull"); return 0; }
    queuedInput = queued;
    observationBuckets = std::max(4u, std::min(48u, (observation_ms + 49) / 50 + 1)); // Один неполный bucket сверх периода.
    double depth = stats().pending + queued;
    double inventory = (double)written + queued - deviceSamples - count;
    double low = lowDepth(std::max(0.0, depth - count), inventory);
    double excess = std::max(0.0, low - reserve);
    bool replenish = protect_reserve && low + Frame < reserve && depth >= count + 80 * 48;
    bool stretch = excess >= 120 * 48 || replenish;
    if (mode == Mode::Stretch && ((!replenish && excess <= 40 * 48) || depth < count + 60 * 48)) mode = Mode::Drain;
    int64_t offset = (int64_t)cursor - (int64_t)deviceSamples;
    // Цель кратна кадру: прекращаем ускорение точно на границе, а не при произвольной глубине.
    phaseGoal = (int64_t)std::max(std::ceil((double)offset / Frame), std::floor((inventory - reserve) / Frame)) * Frame;
    double correction = (double)(phaseGoal - offset);
    double directTempo = correction > 0 ? 1 + std::min(.02, std::max(.001, correction / (2 * Rate))) : 1;
    double target = replenish ? .98 : mode == Mode::Direct && !stretch ? directTempo
        : 1 + .6 * std::max(0.0, std::min(1.0, (excess / 48 - 15) / 925));
    double step = .6 * count / (Rate * .5);
    tempo += std::max(-step, std::min(step, target - tempo));
    if (mode == Mode::Direct && !stretch) tempo = std::min(tempo, 1.02);
    st.setTempo(tempo);
    unsigned done = 0;
    while (done < count && !failed) {
        if (mode == Mode::Direct) {
            // Начало звонка сначала выдаёт контекст: ни один начальный сэмпл не теряется.
            if (stretch && cursor >= Context) { beginStretch(); continue; }
            unsigned take = std::min(count - done, Frame);
            if (stretch) {
                take = (unsigned)std::min<uint64_t>(take, Context - cursor);
                phaseGoal = (int64_t)cursor - (int64_t)deviceSamples;
            }
            unsigned n = direct(out + done, take, reserve, count - done);
            done += n;
            if (n < take) break;
        } else {
            if (!st.numSamples() && mode == Mode::Stretch && !pump()) mode = Mode::Drain;
            if (!st.numSamples()) { endStretch(); stretch = false; continue; }
            float pcm[Frame];
            unsigned n = st.receiveSamples(pcm, std::min(count - done, Frame));
            for (unsigned i = 0; i < n; ++i) out[done + i] = to_pcm(pcm[i]);
            consume(n); done += n; deviceSamples += n;
        }
    }
    return done;
}
