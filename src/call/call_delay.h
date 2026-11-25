/* Обработка задержки PCM 48кГц mono: прямое ускорение и SoundTouch с общей шкалой.
 * Один playback worker. input возвращает до capacity сэмплов, 0 — вход исчерпан;
 * до возврата input владелец регистрирует PTS этих сэмплов в своей карте времени.
 * source сообщает происхождение выхода в абсолютных индексах от reset(). Локальные
 * Reset SoundTouch наружу не выходят. Владелец сам сбрасывает карту при reset().
 * queued — ещё не декодированный вход, reserve — сетевой запас, оба в сэмплах.
 * Переключение сохраняет хвост исходного PCM; ускорение учитывается отдельно.
 * Вход в SoundTouch при избытке >=120мс, выход <=40мс; низкий запас дополнительно
 * принуждает выход. Прямой режим ускоряет не более чем на 2%, без замедления.
 * Его цель — целая разность входных/выходных кадров по 960 сэмплов. Нижняя огибающая
 * запаса подавляет пилу пакетов; её окно задаёт владелец по периоду доставки.
 * При protect_reserve дефицит восполняется через SoundTouch до0.98x; Direct не замедляется.
 * При нехватке входа ускорение не расходует reserve: фазу доводим после восстановления.
 * Это выравнивание границ PCM, не синхронизация физических часов двух аудиоустройств.
 * Внутренние очереди ограничены, размеры pull произвольные, heap в нашем hot path нет. */
#ifndef CALL_DELAY_H
#define CALL_DELAY_H

#include "soundtouch/SoundTouch.h"
#include <array>
#include <cstdint>

class CallDelay {
public:
    using Input = unsigned (*)(void*, int16_t*, unsigned);
    enum class Mode { Direct, Stretch, Drain }; // Прямая выдача, подача в SoundTouch, выдача готового DSP-хвоста.
    struct Stats {
        Mode mode = Mode::Direct;
        double tempo = 1, pending = 0; // Заданный темп и ещё не выданная длительность исходного PCM в сэмплах.
        uint64_t cuts = 0, entered = 0, exited = 0; // Удалённые Direct-сэмплы и переходы в/из SoundTouch с последнего reset.
        unsigned phase = 0; // В Direct: ошибка относительно 20мс выхода; 0 — границы совпадают.
    };
    // Оба callback обязательны; контексты заимствованы до уничтожения. После создания проверить valid().
    CallDelay(Input input, void* input_context, soundtouch::SourceCallback source, void* source_context);
    CallDelay(const CallDelay&) = delete;
    CallDelay& operator=(const CallDelay&) = delete;
    // Ошибка обработки необратима для объекта; reset не восстанавливает valid, нужен новый экземпляр.
    bool valid() const { return !failed; }
    // Выдать 0..count PCM-сэмплов, без дополнения тишиной; queued не включает уже принятый этим объектом PCM.
    // output/count должны быть валидны; нехватка input даёт короткую выдачу, ошибка делает valid()==false.
    // observation_ms — окно нижней огибающей; при пакетном входе должно покрывать период пачки и джиттер.
    // protect_reserve включает мягкое замедление до0.98x при недостаточном запасе (без изменения высоты голоса).
    unsigned pull(int16_t* output, unsigned count, uint64_t queued, unsigned reserve,
                  unsigned observation_ms = 200, bool protect_reserve = false);
    // Удалить PCM/DSP-хвост, счётчики и фазу; внешнюю PTS-карту владелец сбрасывает отдельно.
    void reset();
    void silence(unsigned count); // Тишина устройства тоже сдвигает выходную фазу.
    // Снимок единственного worker; pending — исходные сэмплы, ещё не выданные, не размер готового DSP-выхода.
    Stats stats() const;

private:
    static constexpr unsigned Rate = 48000, Frame = 960, Capacity = 8192, Context = 16;
    Input input; // Заимствованный reader исходного PCM.
    void* inputContext;
    soundtouch::SourceCallback source; // Заимствованный получатель происхождения выходного PCM.
    void* sourceContext;
    soundtouch::SoundTouch st; // DSP только режима Stretch; готовый выход дочитывается в Drain.
    std::array<float, Capacity> raw{}; // Кольцо исходного PCM, включая контекст перед границей Direct/Stretch.
    struct Span { soundtouch::SourceSpan value{soundtouch::SourceKind::Copy}; };
    std::array<Span, 32> spans{}; // Произведение DSP/Direct, ещё не учтённое consume.
    unsigned spanHead = 0, spanCount = 0; // Начало/число действующих spans.
    uint64_t written = 0, cursor = 0, generated = 0, consumed = 0; // Принятый вход, позиция raw, произведённый/выданный выход.
    uint64_t stBase = 0, stFed = 0, entry = 0, queuedInput = 0; // Начало DSP-шкалы, переданный вход, граница входа, внешний FIFO.
    uint64_t deviceSamples = 0; // Выданный PCM плюс объявленная локальная тишина.
    int64_t phaseGoal = 0; // Целевая разность cursor-deviceSamples, кратная Frame.
    double playedSource = 0, tempo = 1, cutPhase = 0; // Позиция источника на выходе, темп, дробный долг удаления Direct.
    struct Bucket { uint64_t time = UINT64_MAX; double low = 0, inventory = 0; } buckets[48]; // До 2.4с, шаг 50мс.
    unsigned observationBuckets = 4; // Действующая часть окна, 4..48 интервалов.
    unsigned trim = 0; // Уже выданный Direct-контекст, который нужно убрать с начала SoundTouch.
    bool failed = false, firstSpan = false, lastCopy = false; // Необратимая ошибка; ожидаем первый DSP-span; хвост — Copy.
    Mode mode = Mode::Direct; // Текущий способ выдачи PCM.
    uint64_t cuts = 0, entered = 0, exited = 0; // Счётчики Direct-удалений и переходов режима с последнего reset.

    bool fill();
    bool pump();
    void beginStretch();
    void endStretch();
    void emit(soundtouch::SourceSpan span);
    void onSource(const soundtouch::SourceSpan& span);
    void consume(unsigned count);
    unsigned direct(int16_t* output, unsigned count, unsigned reserve, unsigned remaining);
    double lowDepth(double depth, double& inventory);
    void fail(const char* reason);
};
#endif
