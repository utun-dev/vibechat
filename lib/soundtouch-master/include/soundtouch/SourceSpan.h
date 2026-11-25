/* uTun: происхождение PCM без изменения DSP. Индексы — сэмплы НА КАНАЛ от clear().
 * Copy: out[i] происходит из input0+i. Mix: источники input0+i и input1+i,
 * вес второго i/count, первого 1-i/count. Это описание происхождения, не PCM.
 * Reset начинает новую шкалу. Invalid запрещает её использование до clear().
 * Callback синхронный: нельзя вызывать SoundTouch повторно, блокировать поток или
 * сохранять ссылку на событие. Ошибки передаются через Invalid; логирует потребитель. */
#ifndef SOUNDTOUCH_SOURCE_SPAN_H
#define SOUNDTOUCH_SOURCE_SPAN_H
#include <cstdint>

namespace soundtouch {
enum class SourceKind { Reset, Copy, Mix, Invalid };
enum class SourceError { None, Profile, Reconfigure, Flush, LateAttach };
struct SourceSpan {
    SourceKind kind;
    SourceError error;
    uint64_t output, input0, input1;
    uint32_t count;
    SourceSpan(SourceKind k, SourceError e = SourceError::None, uint64_t out = 0,
               uint64_t first = 0, uint64_t second = 0, uint32_t length = 0)
        : kind(k), error(e), output(out), input0(first), input1(second), count(length) {}
};
using SourceCallback = void (*)(void*, const SourceSpan&);
}
#endif
