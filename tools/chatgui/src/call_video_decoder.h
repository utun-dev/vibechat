/* Decoder видеотрека: один RX worker владеет всем экземпляром, submit/receive не потокобезопасны.
 * Полный KEY проверяется общим C verifier; FFmpeg дополнительно проверяет PTS/profile/level/геометрию выхода.
 * Новый формат сначала завершает старый через EOS. Готовые изображения сохраняет владелец.
 * capabilities — отдельная блокирующая worker-проба с процессным кэшем, mutex и отменой. */
#pragma once
#include "../../../src/call/call_video.h"
#include <QImage>
#include <memory>
#include <atomic>

// Один RX worker. Независимые submit/receive сохраняют PTS при задержанном выходе.
// Automatic использует проверенный capabilities() GPU, затем четыре CPU slice-потока.
class CallVideoDecoder {
public:
    enum class Backend { Automatic, HardwareOnly, SoftwareOnly };
    explicit CallVideoDecoder(Backend backend = Backend::Automatic);
    ~CallVideoDecoder();
    // 1 — принят/получен, 0 — занят/ещё не готов, -1 — ошибка с подробностями в логе.
    // При submit=0 владелец сохраняет исходный frame. При 1 encoded bytes скопированы в FFmpeg,
    // metadata сохранена до receive; очередь до восьми входов. receive отдаёт metadata без data и владеющий QImage.
    int submit(const call_video_frame& frame);
    int receive(call_video_frame& frame, QImage& image);
    int finish(); // EOS: 1 принят, 0 нужно сначала получить выход, -1 ошибка.
    unsigned pendingFrames() const;
    unsigned pipelineFrames() const; // Минимальный входной pipeline, измеренный отдельной paced пробой.
    bool hardware() const;
    QString error() const;
    void useSoftware(); // После реального отказа устройства: следующий KEY открывает CPU, без повторной GPU пробы.
    void close(); // Отбрасывает pending inputs, освобождает FFmpeg/GPU; выбор backend и текст ошибки сохраняются.
    // running отменяет ожидание lock/пробу; завершённый кэш может вернуться сразу, его не очищает close().
    static call_video_caps capabilities(const std::atomic<bool>* running = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
