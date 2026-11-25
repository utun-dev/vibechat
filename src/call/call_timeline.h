/* Карта SoundTouch → PTS → устройство. Встроенные кольца, без аллокаций в DSP callback.
 * feed/source/read — один playback worker. read/silence/progress/clock синхронизирует
 * владелец с читателями clock; source/feed общих выходных данных не касаются.
 * reset — после остановки доступа. Source Reset обнуляет только локальную карту DSP.
 * Ограничения не меняют PCM: ошибка карты делает clock недостоверным до reset. */
#ifndef CALL_TIMELINE_H
#define CALL_TIMELINE_H
#include "call_clock.h"
#include "soundtouch/SourceSpan.h"
#include <array>
#include <cstddef>

class CallTimeline {
public:
    // Начать пустую карту локальной эпохи; сбрасывает ошибки, позиции, PTS и аппаратный якорь.
    void reset(uint64_t epoch);
    // Зарегистрировать 1..8192 входных сэмплов 48кГц до callback source; INT64_MIN/epoch=0 не дают валидный PTS.
    void feed(int64_t pts_us, unsigned samples, uint64_t source_epoch);
    // Callback происхождения DSP/CallDelay: Copy/Mix должны описывать непрерывный выход из зарегистрированного входа.
    void source(const soundtouch::SourceSpan& span);
    // Перенести только реально извлечённый PCM на шкалу устройства; повторно учитывать его в silence нельзя.
    void read(unsigned samples);
    // Дописать локальную тишину/тоны, удерживая последний PTS. До первого валидного источника PTS остаётся недостоверным.
    void silence(unsigned samples);
    // Якорь локальных часов: position — абсолютная PCM-позиция от reset, at_us — срок её вывода,
    // received_us — время наблюдения. Чужая epoch игнорируется; регрессия позиции/received или position>written
    // запрещает часы до reset. Будущий at_us учитывает очередь устройства; measured — аппаратная метка или оценка.
    void progress(uint64_t epoch, uint64_t position, int64_t at_us, int64_t received_us, bool measured);
    // Приостановить якорь своей эпохи до progress, сохранив PCM/PTS/позиции; прежние ошибки не сбрасывает.
    void suspend_device(uint64_t epoch);
    // Снимок с продвижением/очисткой карты. Якорь старше 250мс, suspend, ошибки карты или неизвестный PTS дают valid=0.
    // PTS не регрессирует: откат до 100мс удерживается, больший откат делает снимок недостоверным.
    call_clock_snapshot clock(int64_t now_us);
private:
    struct Span {
        uint64_t first = 0, epoch = 0; // Первый индекс участка в его кольце и поколение удалённого захвата.
        unsigned count = 0; // Число сэмплов участка.
        double pts = 0, end = 0; // PTS начала и конца, мкс; равны при удержании локальной тишины.
        bool valid = false; // Есть достоверная шкала одного источника.
        Span() = default;
        Span(uint64_t f, uint64_t e, unsigned n, double p, double q, bool v)
            : first(f), epoch(e), count(n), pts(p), end(q), valid(v) {}
    };
    template<size_t N> struct Ring {
        std::array<Span, N> data{}; // Фиксированное кольцо; push при заполнении возвращает false.
        size_t head = 0, count = 0; // Начало и число действующих участков.
        Span& front() { return data[head]; }
        const Span& back() const { return data[(head + count - 1) % N]; }
        void pop() { head = (head + 1) % N; --count; }
        void clear() { head = count = 0; }
        bool push(const Span& s) {
            if (count == N) return false;
            data[(head + count++) % N] = s; return true;
        }
    };
    Ring<256> input; // >5с входа; SoundTouch звонка держит лишь несколько блоков.
    Ring<1024> ready; // Произведённый DSP-выход, ещё не учтённый read.
    Ring<2048> output; // PCM устройства до подтверждённого продвижения; overflow запрещает часы до reset.
    uint64_t inputWritten = 0, generated = 0, consumed = 0, written = 0, epoch = 0; // Вход/DSP/прочитано/устройство/локальная эпоха.
    uint64_t anchorPosition = 0, minimumPosition = 0; // Последний device-якорь и монотонная нижняя граница сыгранного PCM.
    int64_t anchorTime = 0, anchorReceived = 0, lastPts = -1; // Срок/приём якоря и последний валидный PTS; -1 — ещё нет PTS.
    double endPts = 0; // Конец последнего записанного участка, удерживаемый silence.
    uint64_t endEpoch = 0; // Поколение источника конца output.
    int sourceError = 0; // Отложенная ошибка DSP-карты; логируется при read.
    // Необратимые до reset ошибки output/device; наличие/тип якоря; валидность конца output.
    bool outputFailed = false, deviceFailed = false, haveAnchor = false, measured = false, endValid = false;
    static double at(const Span& s, double position);
    const Span* findInput(uint64_t position) const;
    void appendOutput(const Span& s);
};
#endif
