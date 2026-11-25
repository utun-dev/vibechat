#include "call_timeline.h"
#include "../../lib/debug_config.h"
#include <algorithm>
#include <cmath>

static constexpr double sampleUs = 1000000.0 / 48000;

double CallTimeline::at(const Span& s, double position) {
    return s.pts + (s.end - s.pts) * (position - s.first) / s.count;
}

void CallTimeline::reset(uint64_t newEpoch) {
    input.clear(); ready.clear(); output.clear();
    inputWritten = generated = consumed = written = anchorPosition = minimumPosition = 0;
    epoch = newEpoch; anchorTime = anchorReceived = 0; lastPts = -1;
    endPts = 0; endEpoch = 0; sourceError = 0;
    outputFailed = deviceFailed = haveAnchor = measured = endValid = false;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "audio timeline reset epoch=%llu", (unsigned long long)epoch);
}

/* Входная история нужна только DSP; готовые PTS уже не зависят от её кольца. */
void CallTimeline::feed(int64_t pts, unsigned samples, uint64_t sourceEpoch) {
    if (!samples || samples > 8192 || inputWritten > UINT64_MAX - samples
        || (pts != INT64_MIN && (pts < -1000000 || pts > INT64_MAX / 2))) {
        if (!sourceError) sourceError = 1;
        return;
    }
    if (input.count == input.data.size()) input.pop();
    bool valid = pts != INT64_MIN && sourceEpoch != 0;
    double first = valid ? (double)pts : 0;
    input.push({inputWritten, sourceEpoch, samples, first, first + samples * sampleUs, valid});
    inputWritten += samples;
}

const CallTimeline::Span* CallTimeline::findInput(uint64_t position) const {
    for (size_t i = 0; i < input.count; i++) {
        const auto& s = input.data[(input.head + i) % input.data.size()];
        if (position >= s.first && position - s.first < s.count) return &s;
    }
    return nullptr;
}

/* Callback DSP: только фиксированные массивы; диагностика ошибки откладывается до read(). */
void CallTimeline::source(const soundtouch::SourceSpan& s) {
    using soundtouch::SourceKind;
    if (s.kind == SourceKind::Reset) {
        input.clear(); ready.clear(); inputWritten = generated = consumed = 0; sourceError = 0;
        return;
    }
    if (sourceError) return;
    if (s.kind == SourceKind::Invalid) { sourceError = 2 + (int)s.error; return; }
    if (!s.count || s.output != generated || s.count > UINT64_MAX - generated
        || s.input0 > UINT64_MAX - s.count || s.input1 > UINT64_MAX - s.count) {
        sourceError = 10; return;
    }
    for (unsigned offset = 0; offset < s.count;) {
        const Span* a = findInput(s.input0 + offset);
        const Span* b = s.kind == SourceKind::Mix ? findInput(s.input1 + offset) : a;
        if (!a || !b) { sourceError = 11; return; }
        unsigned count = std::min<uint64_t>(s.count - offset, a->first + a->count - s.input0 - offset);
        if (s.kind == SourceKind::Mix) count = std::min<uint64_t>(count, b->first + b->count - s.input1 - offset);
        double begin = at(*a, s.input0 + offset), end = at(*a, s.input0 + offset + count);
        bool valid = a->valid;
        if (s.kind == SourceKind::Mix) {
            double other = at(*b, s.input1 + offset), otherEnd = at(*b, s.input1 + offset + count);
            // Внутри непрерывного входного участка обе шкалы имеют один шаг 1/48000.
            begin += (other - begin) * offset / s.count;
            end += (otherEnd - end) * (offset + count) / s.count;
            valid = valid && b->valid && a->epoch == b->epoch;
            // Большой разрыв источника не превращаем в вымышленный промежуточный PTS.
            if (std::abs(other - at(*a, s.input0 + offset)) > 100000) valid = false;
        }
        if (!ready.push({s.output + offset, a->epoch, count, begin, end, valid})) { sourceError = 12; return; }
        offset += count;
    }
    generated += s.count;
}

void CallTimeline::appendOutput(const Span& s) {
    if (outputFailed) return;
    // Источники без PTS и длинные локальные паузы не расходуют по записи на каждый pull.
    if (output.count) {
        Span& previous = output.data[(output.head + output.count - 1) % output.data.size()];
        bool sameHold = previous.pts == previous.end && s.pts == s.end && previous.end == s.pts;
        if (previous.first + previous.count == s.first && previous.epoch == s.epoch && previous.valid == s.valid
            && (!s.valid || sameHold) && previous.count <= UINT32_MAX - s.count) {
            previous.count += s.count; previous.end = s.end;
            endPts = s.end; endEpoch = s.epoch; endValid = s.valid;
            return;
        }
    }
    if (!output.push(s)) {
        outputFailed = true;
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio timeline output overflow epoch=%llu written=%llu anchor=%llu spans=%zu",
                    (unsigned long long)epoch, (unsigned long long)written, (unsigned long long)anchorPosition, output.count);
        return;
    }
    endPts = s.end; endEpoch = s.epoch; endValid = s.valid;
}

/* Переносим только реально прочитанный PCM, разрезая исходные участки на границах read. */
void CallTimeline::read(unsigned samples) {
    if ((sourceError || samples > generated - consumed) && !outputFailed) {
        outputFailed = true;
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio timeline source failed epoch=%llu reason=%d generated=%llu consumed=%llu read=%u",
                    (unsigned long long)epoch, sourceError, (unsigned long long)generated, (unsigned long long)consumed, samples);
    }
    unsigned left = samples;
    while (!outputFailed && left) {
        while (ready.count && ready.front().first + ready.front().count <= consumed) ready.pop();
        if (!ready.count || ready.front().first > consumed) {
            outputFailed = true;
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio timeline missing produced range consumed=%llu", (unsigned long long)consumed);
            break;
        }
        const Span& s = ready.front();
        unsigned take = std::min<uint64_t>(left, s.first + s.count - consumed);
        appendOutput({written, s.epoch, take, at(s, consumed), at(s, consumed + take), s.valid});
        consumed += take; written += take; left -= take;
    }
    if (outputFailed) { consumed += left; written += left; }
}

void CallTimeline::silence(unsigned samples) {
    if (!samples) return;
    appendOutput({written, endEpoch, samples, endPts, endPts, endValid});
    written += samples;
}

/* at_us — время конца известного устройству участка; будущая отметка учитывает очередь backend. */
void CallTimeline::progress(uint64_t deviceEpoch, uint64_t position, int64_t atUs, int64_t receivedUs, bool isMeasured) {
    if (deviceEpoch != epoch || deviceFailed) return;
    if (position > written || atUs < 0 || receivedUs < 0 || (haveAnchor && (position < anchorPosition || receivedUs < anchorReceived))) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio clock invalid device progress epoch=%llu position=%llu written=%llu time=%lld received=%lld",
                   (unsigned long long)epoch, (unsigned long long)position, (unsigned long long)written,
                   (long long)atUs, (long long)receivedUs);
        deviceFailed = true; haveAnchor = false; return;
    }
    anchorPosition = position; anchorTime = atUs; anchorReceived = receivedUs; measured = isMeasured; haveAnchor = true;
    uint64_t pending = atUs > receivedUs ? (uint64_t)std::ceil((atUs - receivedUs) / sampleUs) : 0;
    uint64_t played = pending > position ? 0 : position - pending;
    minimumPosition = std::max(minimumPosition, played);
    while (output.count > 1 && output.front().first + output.front().count <= minimumPosition) output.pop();
}

/* Устройство меняется, но PCM/PTS, монотонная позиция и сетевой резерв сохраняются. */
void CallTimeline::suspend_device(uint64_t deviceEpoch) {
    if (deviceEpoch != epoch) return;
    anchorReceived = -1;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio clock device suspended epoch=%llu position=%llu written=%llu",
        (unsigned long long)epoch, (unsigned long long)anchorPosition, (unsigned long long)written);
}

call_clock_snapshot CallTimeline::clock(int64_t nowUs) {
    call_clock_snapshot result{};
    result.epoch = epoch; result.written_samples = written; result.pts_us = lastPts; result.measured = measured;
    if (outputFailed || deviceFailed || !haveAnchor || !output.count || anchorReceived < 0 ||
        nowUs < anchorReceived || nowUs - anchorReceived > 250000) return result;
    uint64_t pending = anchorTime > nowUs ? (uint64_t)std::ceil((anchorTime - nowUs) / sampleUs) : 0;
    uint64_t position = pending > anchorPosition ? 0 : anchorPosition - pending;
    position = std::max(position, minimumPosition);
    minimumPosition = position;
    while (output.count > 1 && output.front().first + output.front().count <= position) output.pop();
    const Span& s = output.front();
    result.played_samples = position; result.source_epoch = s.epoch;
    if (!s.valid || position < s.first || position > s.first + s.count) return result;
    int64_t pts = (int64_t)std::llround(at(s, position));
    if (pts < 0 || (lastPts >= 0 && pts < lastPts - 100000)) return result;
    result.held_back_us = pts < lastPts ? (uint32_t)(lastPts - pts) : 0;
    lastPts = std::max(pts, lastPts);
    result.pts_us = lastPts; result.valid = 1;
    result.rate_ppm = (uint32_t)std::llround(std::max(0.0, (s.end - s.pts) / (s.count * sampleUs)) * 1000000);
    return result;
}
