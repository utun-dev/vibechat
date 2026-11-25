/* Подготовка stereo PCM s16/48кГц для локальной записи: удаление DC ниже 5 Гц и плавный старт за 5 мс.
 * reset вызывать при новом источнике/разрыве PCM. process меняет только значения отсчётов, без задержки и изменения длины.
 * История DC начинается с первого реального отсчёта: постоянное смещение микрофона не создаёт стартовый импульс. */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

struct RecordingAudioInput {
    static constexpr unsigned FadeSamples = 240; // 5 мс при 48 кГц; raised cosine имеет нулевой наклон на обоих концах.
    static constexpr double Pole = 0.9993457156679053; // exp(-2*pi*5/48000); слышимый диапазон почти не затрагивается.
    bool initialized = false; // История привязана к первому сохранённому PCM-фрейму.
    unsigned fadePosition = 0; // Насыщается на FadeSamples, счётчик не растёт во время длительной записи.
    double previousInput[2]{}, previousOutput[2]{}; // Независимая история DC-фильтра левого/правого каналов.

    void reset() { *this = {}; } // Новый независимый источник, включая восстановление после пропуска.
    void process(int16_t* pcm, unsigned frames) { // Собственный interleaved буфер frames*2; можно передавать произвольные порции.
        for (unsigned i = 0; i < frames; ++i) {
            if (!initialized) {
                previousInput[0] = pcm[2*i]; previousInput[1] = pcm[2*i+1]; initialized = true;
            }
            double gain = fadePosition < FadeSamples
                ? 0.5 - 0.5 * std::cos(3.141592653589793 * fadePosition / FadeSamples) : 1.0;
            if (fadePosition < FadeSamples) ++fadePosition;
            for (unsigned ch = 0; ch < 2; ++ch) {
                double input = pcm[2*i+ch];
                double output = (1 + Pole) * 0.5 * (input - previousInput[ch]) + Pole * previousOutput[ch];
                previousInput[ch] = input; previousOutput[ch] = output;
                pcm[2*i+ch] = int16_t(std::clamp(std::lround(output * gain), -32768L, 32767L));
            }
        }
    }
};
