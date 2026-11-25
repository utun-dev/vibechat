/* Политика резерва готового PCM desktop, единственный владелец — RX worker. Сам PCM/очередь не хранит.
 * Резерв 1..4 блока по 960 фреймов/20мс (48кГц); частично сыгранный блок находится сверх этого резерва.
 * bufferMs=20/40/60/80 проверяет VoiceAudioIo::start, конструктор этого класса не валидирует аргумент.
 * Четыре места данных доступны в AudioEventQueue<Playback,12>; у звонка sample вызывается по progress вывода. */
#pragma once
#include <algorithm>
#include <cstdint>

class VoicePlaybackBuffer {
public:
    /* Сохраняет настроенный минимум bufferMs/20 и сбрасывает адаптацию. */
    explicit VoicePlaybackBuffer(unsigned bufferMs = 20) : minimumBlocks(bufferMs / 20) { reset(); }
    unsigned blocks = 1; // Текущая цель полной очереди, от настроенного минимума до четырёх блоков.
    unsigned callbackFrames = 960; // Размер последнего callback из sample, фреймов на канал.
    /* Возвращает настроенный минимум и 10с пробный интервал, забывая историю starvation и callback. */
    void reset() { blocks = minimumBlocks; callbackFrames = 960; stableSince = 0; lastGrowth = 0; lastSample = 0; probeUs = 10000000; }
    /* nowUs — монотонные мкс; frames — полный размер выходного callback, missing — нехватка хотя бы одного PCM-фрейма.
     * Минимум — max(настройка, ceil(frames/960), ограниченное 1..4). При missing рост на блок не чаще 500мс;
     * каждая такая попытка удваивает интервал снижения с 10с до 300с. Стабильный вывод убирает по одному блоку
     * после этого интервала; пауза sample >1с сбрасывает стабильность, но не адаптированный интервал. */
    void sample(int64_t nowUs, unsigned frames, bool missing) {
        callbackFrames = frames;
        unsigned minimum = std::max(minimumBlocks, std::clamp((frames + 959) / 960, 1u, 4u));
        if (!stableSince || nowUs - lastSample > 1000000) stableSince = nowUs;
        lastSample = nowUs;
        if (minimum > blocks) { blocks = minimum; stableSince = nowUs; }
        if (missing) {
            stableSince = nowUs;
            if (!lastGrowth || nowUs - lastGrowth >= 500000) {
                blocks = std::min(blocks + 1, 4u); lastGrowth = nowUs;
                probeUs = std::min(probeUs * 2, int64_t(300000000));
            }
        } else if (blocks > minimum && nowUs - stableSince >= probeUs) {
            --blocks; stableSince = nowUs;
        }
    }
private:
    unsigned minimumBlocks; // Настроенный минимум, не уменьшается адаптацией.
    int64_t stableSince = 0, lastGrowth = 0, lastSample = 0, probeUs = 10000000; // Начало стабильности/рост/опрос/интервал снижения, мкс.
};
