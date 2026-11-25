#include "stream_playback_audio.h"
#include "stream_audio_meter.h"
#include "soundtouch/SoundTouch.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>

extern "C" {
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/opus_codec.h"
}

namespace {
constexpr unsigned rate = 48000, framesPerPacket = 960, maxFrames = 8192, mapCapacity = 1024, pcmCapacity = 128;
struct Span {
    double first, last, ptsFirst, ptsLast;
    Span(double begin = 0, double end = 0, double ptsBegin = 0, double ptsEnd = 0)
        : first(begin), last(end), ptsFirst(ptsBegin), ptsLast(ptsEnd) {}
};
double mapped(const Span& span, double position) {
    if (span.last <= span.first) return span.ptsLast;
    double fraction = std::max(0.0, std::min(1.0, (position - span.first) / (span.last - span.first)));
    return span.ptsFirst + fraction * (span.ptsLast - span.ptsFirst);
}
}

struct stream_playback_audio {
    struct Pcm {
        int16_t samples[framesPerPacket * 2];
        Span spans[8];
        unsigned frames = 0, count = 0;
    } ready[pcmCapacity];
    std::atomic<uint64_t> readyWrite{0}, readyRead{0}, preparedFrames{0}, consumedFrames{0};
    std::atomic<double> consumedGain{0};
    stream_audio_meter* meter = nullptr;
    std::atomic<int64_t> fedEndUs{-1}; // Исходный конец последнего полностью обработанного Opus-пакета.
    unsigned readyOffset = 0; // Только consumer; producer переиспользует слот после readyRead.store(release).
    double preparedOutput = 0, preparedGain = 0; // Только producer.
    opus_codec_decoder_t* opus = nullptr;
    soundtouch::SoundTouch stretch;
    Span input[mapCapacity], output[mapCapacity];
    unsigned inputHead = 0, inputCount = 0, outputHead = 0, outputCount = 0;
    std::atomic<unsigned> bufferFrames{0};
    int tempo = 1000;
    int64_t startPts = 0, expectedPts = -1, endPts = 0; // endPts публикуется через preparedEnd.
    double expectedOutput = 0, producedOutput = 0, lastPts = 0;
    uint64_t written = 0, lastAudioFrame = 0; // Consumer: граница последнего PCM до возможной тишины.
    std::atomic<bool> finished{false}, failed{false}, preparedEnd{false};
    std::atomic<bool> buffering{true}; // Consumer ждёт порог PCM при старте и после фактической нехватки.
    bool reportedBuffering = true; // Только producer: переходы логируются вне hardware callback.
    int16_t pcm[framesPerPacket * 2];
    float samples[maxFrames * 2];
};

/* Между сменами темпа соответствие линейно; точки сохраняют старый темп уже поставленного PCM. */
static bool appendOutput(stream_playback_audio* a, const Span& span) {
    if (a->outputCount) {
        auto& previous = a->output[(a->outputHead + a->outputCount - 1) % mapCapacity];
        double previousSlope = (previous.ptsLast - previous.ptsFirst) / (previous.last - previous.first);
        double slope = (span.ptsLast - span.ptsFirst) / (span.last - span.first);
        if (std::abs(previous.last - span.first) < 0.000001 && std::abs(previous.ptsLast - span.ptsFirst) < 0.001
            && std::abs(previousSlope - slope) < 0.000001) {
            previous.last = span.last; previous.ptsLast = span.ptsLast; return true;
        }
    }
    if (a->outputCount == mapCapacity) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback audio output map overflow written=%llu", (unsigned long long)a->written);
        a->failed = true; return false;
    }
    a->output[(a->outputHead + a->outputCount++) % mapCapacity] = span;
    return true;
}

/* Прогрев перед точкой старта не создаёт слышимый PCM; частично перекрытый пакет обрезается по кадрам. */
static int feedPcm(stream_playback_audio* a, int64_t pts, const uint8_t* packet, int length) {
    int count = opus_codec_decode(a->opus, packet, length, a->pcm, framesPerPacket);
    if (count != (int)framesPerPacket) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback Opus decode failed pts=%lld result=%d", (long long)pts, count); return -1;
    }
    if (pts + 20000 <= a->startPts) return 0;
    unsigned skip = pts < a->startPts ? (unsigned)((a->startPts - pts) * rate + 999999) / 1000000 : 0;
    if (skip >= framesPerPacket) return 0;
    unsigned frames = framesPerPacket - skip;
    while (a->inputCount && a->input[a->inputHead].last <= a->preparedOutput) {
        a->inputHead = (a->inputHead + 1) % mapCapacity; a->inputCount--;
    }
    if (a->inputCount == mapCapacity) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback audio input map overflow pts=%lld", (long long)pts); return -1;
    }
    double end = a->expectedOutput + frames * 1000.0 / a->tempo;
    a->input[(a->inputHead + a->inputCount++) % mapCapacity] =
        {a->expectedOutput, end, pts + skip * 1000000.0 / rate, pts + 20000.0};
    a->expectedOutput = end;
    for (unsigned i = 0; i < frames * 2; i++) a->samples[i] = a->pcm[skip * 2 + i] / 32768.0f;
    a->stretch.putSamples(a->samples, frames);
    return 0;
}

extern "C" {

int stream_playback_audio_set_buffer_ms(struct stream_playback_audio* a, unsigned ms) {
    if (!a || !ms || ms > 1000) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback invalid prefill=%u", ms); return -1; }
    unsigned frames = rate * ms / 1000, previous = a->bufferFrames.exchange(frames);
    if (frames > previous) {
        stream_audio_meter_reset(a->meter);
        a->buffering = true;
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "playback prefill changed previous_frames=%u frames=%u", previous, frames);
    return 0;
}

struct stream_playback_audio* stream_playback_audio_create(int64_t start, unsigned bufferMs) {
    if (start < 0) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback audio invalid start=%lld", (long long)start); return nullptr; }
    if (!bufferMs || bufferMs > 1000) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback audio invalid PCM buffer=%ums range=1..1000ms", bufferMs); return nullptr;
    }
    void* memory = u_malloc(sizeof(stream_playback_audio));
    if (!memory) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback audio allocation failed"); return nullptr; }
    auto* a = new (memory) stream_playback_audio;
    a->startPts = start; a->lastPts = (double)start; a->bufferFrames = rate * bufferMs / 1000;
    a->opus = opus_codec_decoder_create(rate, 2);
    if (!a->opus) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback audio Opus initialization failed"); stream_playback_audio_destroy(a); return nullptr;
    }
    a->stretch.setSampleRate(rate); a->stretch.setChannels(2); a->stretch.setTempo(1.0);
    a->stretch.setSetting(SETTING_USE_QUICKSEEK, 1);
    a->stretch.setSetting(SETTING_SEQUENCE_MS, 20); a->stretch.setSetting(SETTING_SEEKWINDOW_MS, 10);
    a->stretch.setSetting(SETTING_OVERLAP_MS, 4); a->stretch.setSetting(SETTING_USE_AA_FILTER, 0);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM,
        "playback audio created start=%lld stereo=1 tempo=1.0..1.6 PCM_prefill_resume=%ums/%u_frames DSP_latency_us=%lld output=SPSC",
        (long long)start, bufferMs, a->bufferFrames.load(), (long long)stream_playback_audio_latency_us(a));
    return a;
}

void stream_playback_audio_set_meter(struct stream_playback_audio* a, struct stream_audio_meter* meter) {
    if (a) { stream_audio_meter_release(a->meter); a->meter = meter; }
    else stream_audio_meter_release(meter);
}

void stream_playback_audio_destroy(struct stream_playback_audio* a) {
    if (!a) return;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "playback audio destroy written=%llu produced=%.0f pts=%.0f failed=%d",
               (unsigned long long)a->written, a->producedOutput, a->lastPts, (int)a->failed.load());
    stream_audio_meter_reset(a->meter); stream_audio_meter_release(a->meter);
    opus_codec_decoder_destroy(a->opus); a->~stream_playback_audio(); u_free(a);
}

int stream_playback_audio_feed(struct stream_playback_audio* a, int64_t pts, const uint8_t* packet, size_t length) {
    if (!a || !packet || !length || length > 1275 || pts < 0 || pts > INT64_MAX - 20000 || a->finished || a->failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback audio invalid feed pts=%lld length=%zu", (long long)pts, length); return -1;
    }
    if (a->expectedPts >= 0 && pts < a->expectedPts - 10000) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "playback audio late packet pts=%lld expected=%lld", (long long)pts, (long long)a->expectedPts);
        return 1;
    }
    int64_t difference = a->expectedPts < 0 ? 0 : pts - a->expectedPts;
    if (difference >= 110000) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "playback audio discontinuity pts=%lld expected=%lld gap_us=%lld",
                   (long long)pts, (long long)a->expectedPts, (long long)difference); return -2;
    }
    int missing = a->expectedPts < 0 ? 0 : (int)((difference + 10000) / 20000);
    int64_t effectivePts = a->expectedPts < 0 ? pts : a->expectedPts + missing * 20000;
    if (effectivePts > INT64_MAX - 20000) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback audio normalized PTS overflow pts=%lld", (long long)effectivePts); return -1;
    }
    for (int i = 0; i < missing; i++) if (feedPcm(a, a->expectedPts + i * 20000, nullptr, 0) < 0) return -1;
    if (feedPcm(a, effectivePts, packet, (int)length) < 0) return -1;
    a->expectedPts = effectivePts + 20000;
    int result = stream_playback_audio_prepare(a);
    if (!result) a->fedEndUs.store(pts + 20000, std::memory_order_release);
    return result;
}

void stream_playback_audio_set_tempo(struct stream_playback_audio* a, int tempo) {
    if (!a || tempo < 1000 || tempo > 1600) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback audio invalid tempo=%d", tempo); return;
    }
    if (tempo == a->tempo) return;
    a->tempo = tempo; a->stretch.setTempo(tempo / 1000.0);
}

int stream_playback_audio_needs_input(const struct stream_playback_audio* a) {
    return a && !a->finished && !a->failed && stream_playback_audio_buffered(a) < a->bufferFrames;
}

unsigned stream_playback_audio_buffered(const struct stream_playback_audio* a) {
    if (!a) return 0;
    uint64_t consumed = a->consumedFrames.load(std::memory_order_acquire);
    uint64_t prepared = a->preparedFrames.load(std::memory_order_acquire);
    return (unsigned)(prepared > consumed ? prepared - consumed : 0);
}

int stream_playback_audio_buffering(const struct stream_playback_audio* a) {
    return a && !a->finished.load(std::memory_order_acquire) && a->buffering.load(std::memory_order_acquire);
}

int64_t stream_playback_audio_latency_us(struct stream_playback_audio* a) {
    return a ? ((int64_t)a->stretch.getSetting(SETTING_INITIAL_LATENCY) * 1000000 + rate - 1) / rate : 0;
}

/* DSP принадлежит producer. Каждый слот содержит PCM и его исходные PTS, включая смены темпа. */
int stream_playback_audio_prepare(struct stream_playback_audio* a) {
    if (!a || a->failed) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback PCM prepare invalid state"); return -1; }
    bool buffering = stream_playback_audio_buffering(a);
    if (buffering != a->reportedBuffering) {
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "playback PCM state=%s start=%lld current_frames=%u prefill_frames=%u tempo=%d finished=%d",
            buffering ? "buffering" : "playing", (long long)a->startPts, stream_playback_audio_buffered(a),
            a->bufferFrames.load(), a->tempo, (int)a->finished.load());
        a->reportedBuffering = buffering;
    }
    uint64_t write = a->readyWrite.load(std::memory_order_relaxed);
    while (write - a->readyRead.load(std::memory_order_acquire) < pcmCapacity) {
        double end = a->finished ? std::round(a->expectedOutput) : std::floor(a->expectedOutput);
        unsigned count = (unsigned)std::max(0.0, std::min<double>(framesPerPacket, end - a->preparedOutput));
        count = std::min(count, a->stretch.numSamples());
        if (!count) break;
        auto& slot = a->ready[write % pcmCapacity]; slot.count = 0;
        slot.frames = a->stretch.receiveSamples(a->samples, count);
        for (unsigned i = 0; i < slot.frames * 2; i++)
            slot.samples[i] = (int16_t)std::lrint(std::max(-32768.0f, std::min(32767.0f, a->samples[i] * 32767.0f)));
        double position = a->preparedOutput, until = position + slot.frames;
        for (unsigned i = 0; i < a->inputCount && position < until; i++) {
            const auto& span = a->input[(a->inputHead + i) % mapCapacity];
            if (span.last <= position) continue;
            double next = std::min(until, span.last);
            // EOF округляется до целого выходного frame, последний PTS остаётся концом материала.
            if (a->finished && i + 1 == a->inputCount && until <= std::round(a->expectedOutput)) next = until;
            if (slot.count == 8) {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback PCM source map overflow frames=%u", slot.frames);
                a->failed = true; return -1;
            }
            double firstPts = mapped(span, position), lastPts = mapped(span, next);
            if (a->finished && i + 1 == a->inputCount && next == std::round(a->expectedOutput)) lastPts = span.ptsLast;
            slot.spans[slot.count++] = {position, next, firstPts, lastPts};
            a->preparedGain += std::max(0.0, lastPts - firstPts - (next - position) * 1000000.0 / rate);
            position = next;
        }
        if (position != until || !slot.frames) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback PCM source map missing begin=%.0f end=%.0f mapped=%.0f",
                        a->preparedOutput, until, position);
            a->failed = true; return -1;
        }
        a->preparedOutput = until;
        a->readyWrite.store(++write, std::memory_order_release);
        a->preparedFrames.store((uint64_t)until, std::memory_order_release);
    }
    if (a->finished && a->preparedOutput >= std::round(a->expectedOutput)) a->preparedEnd.store(true, std::memory_order_release);
    return 0;
}

/* Consumer копирует только готовый PCM. Ни Opus, ни SoundTouch, ни producer-lock здесь нет. */
int stream_playback_audio_pull(struct stream_playback_audio* a, int16_t* pcm, unsigned frames, int64_t limit) {
    if (!a || !pcm || frames > maxFrames || a->failed || limit < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "playback audio invalid pull frames=%u limit=%lld", frames, (long long)limit); return -1;
    }
    if (!frames) return 0;
    if (a->buffering.load(std::memory_order_relaxed)) {
        if (!a->finished.load(std::memory_order_acquire) && stream_playback_audio_buffered(a) < a->bufferFrames) {
            if (a->producedOutput > 0 && limit == INT64_MAX)
                stream_audio_meter_output(a->meter, a->fedEndUs.load(), 0, frames, 0);
            return 0;
        }
        a->buffering.store(false, std::memory_order_release);
    }
    unsigned done = 0;
    uint64_t read = a->readyRead.load(std::memory_order_relaxed);
    double consumedGain = a->consumedGain.load(std::memory_order_relaxed);
    while (done < frames && read != a->readyWrite.load(std::memory_order_acquire)) {
        const auto& slot = a->ready[read % pcmCapacity];
        double until = a->producedOutput;
        for (unsigned i = 0; i < slot.count; i++) {
            const auto& span = slot.spans[i];
            if (span.last <= a->producedOutput) continue;
            if (limit >= span.ptsLast) until = span.last;
            else if (limit > span.ptsFirst) {
                until = span.first + (limit - span.ptsFirst) * (span.last - span.first) / (span.ptsLast - span.ptsFirst); break;
            } else break;
        }
        unsigned got = std::min(frames - done, (unsigned)std::max(0.0, std::floor(until - a->producedOutput)));
        if (!got) break;
        std::memcpy(pcm + done * 2, slot.samples + a->readyOffset * 2, got * 2 * sizeof(int16_t));
        double position = a->producedOutput, end = position + got;
        for (unsigned i = 0; i < slot.count && position < end; i++) {
            const auto& span = slot.spans[i];
            if (span.last <= position) continue;
            double next = std::min(end, span.last), firstPts = mapped(span, position), lastPts = mapped(span, next);
            if (!appendOutput(a, {a->written + position - a->producedOutput,
                                  a->written + next - a->producedOutput, firstPts, lastPts})) return -1;
            consumedGain += std::max(0.0, lastPts - firstPts - (next - position) * 1000000.0 / rate);
            a->lastPts = lastPts; position = next;
        }
        a->producedOutput = end; a->written += got; done += got; a->readyOffset += got;
        a->lastAudioFrame = a->written;
        if (a->readyOffset == slot.frames) { a->readyOffset = 0; a->readyRead.store(++read, std::memory_order_release); }
    }
    a->consumedGain.store(consumedGain, std::memory_order_release);
    a->consumedFrames.store((uint64_t)a->producedOutput, std::memory_order_release);
    bool underrun = done < frames && read == a->readyWrite.load(std::memory_order_acquire) && !a->finished.load(std::memory_order_acquire);
    if (underrun) a->buffering.store(true, std::memory_order_release);
    if (a->producedOutput > 0 && limit == INT64_MAX && !a->finished)
        stream_audio_meter_output(a->meter, a->fedEndUs.load(), done, frames - done, underrun);
    if (stream_playback_audio_empty(a)) a->lastPts = (double)a->endPts;
    return (int)done;
}

void stream_playback_audio_silence(struct stream_playback_audio* a, unsigned frames) {
    if (!a || !frames) return;
    if (!appendOutput(a, {(double)a->written, (double)(a->written + frames), a->lastPts, a->lastPts})) return;
    a->written += frames;
}

int64_t stream_playback_audio_position(struct stream_playback_audio* a, uint64_t played) {
    if (!a) return -1;
    if (played > a->written) played = a->written;
    while (a->outputCount > 1 && a->output[a->outputHead].last <= played) {
        a->outputHead = (a->outputHead + 1) % mapCapacity; a->outputCount--;
    }
    // Округлённый PCM мог быть опубликован до finish; точный EOF передаётся отдельной immutable границей.
    if (stream_playback_audio_empty(a) && played >= a->lastAudioFrame) return a->endPts;
    if (!a->outputCount) return a->startPts;
    return (int64_t)std::llround(mapped(a->output[a->outputHead], (double)played));
}

int64_t stream_playback_audio_pending_gain(struct stream_playback_audio* a, uint64_t played) {
    if (!a) return 0;
    if (stream_playback_audio_empty(a) && played >= a->lastAudioFrame) return 0;
    double gain = std::max(0.0, a->preparedGain - a->consumedGain.load(std::memory_order_acquire));
    for (unsigned i = 0; i < a->outputCount; i++) {
        const auto& span = a->output[(a->outputHead + i) % mapCapacity];
        double begin = std::max(span.first, (double)played);
        if (span.last <= begin) continue;
        gain += std::max(0.0, span.ptsLast - mapped(span, begin) - (span.last - begin) * 1000000.0 / rate);
    }
    for (unsigned i = 0; !a->preparedEnd.load(std::memory_order_relaxed) && i < a->inputCount; i++) {
        const auto& span = a->input[(a->inputHead + i) % mapCapacity];
        double begin = std::max(span.first, a->preparedOutput);
        if (span.last <= begin) continue;
        gain += std::max(0.0, span.ptsLast - mapped(span, begin) - (span.last - begin) * 1000000.0 / rate);
    }
    return (int64_t)std::ceil(gain);
}

uint64_t stream_playback_audio_written(const struct stream_playback_audio* a) { return a ? a->written : 0; }

int stream_playback_audio_starved(const struct stream_playback_audio* a, uint64_t played) {
    if (!a || a->finished) return 0;
    if (played >= a->written || !a->outputCount) return 1;
    for (unsigned i = 0; i < a->outputCount; i++) {
        const auto& span = a->output[(a->outputHead + i) % mapCapacity];
        if (span.last <= played) continue;
        return span.ptsLast == span.ptsFirst;
    }
    return 1;
}

void stream_playback_audio_finish(struct stream_playback_audio* a) {
    if (!a || a->finished) return;
    a->endPts = std::max(a->startPts, a->expectedPts);
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "playback audio finish pts=%lld expected_frames=%.6f prepared_frames=%.0f",
        (long long)a->endPts, a->expectedOutput, a->preparedOutput);
    a->stretch.flush(); a->finished = true; stream_playback_audio_prepare(a);
}

int stream_playback_audio_empty(const struct stream_playback_audio* a) {
    return !a || (a->preparedEnd.load(std::memory_order_acquire) && !stream_playback_audio_buffered(a));
}
}
