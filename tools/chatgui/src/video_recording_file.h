/* Последовательная запись пакетов LiveVideoEncoder/Opus в MKV; все методы — один worker.
 * open задаёт выходной файл, первый KEY создаёт дорожки. finish публикует готовый файл без перекодирования,
 * удаляя резервную звуковую дорожку, если звук ни разу не включался. Деструктор удаляет незавершённый spool. */
#pragma once
#include "live_video_encoder.h"
#include <memory>
class VideoRecordingFile {
public:
    VideoRecordingFile();
    ~VideoRecordingFile();
    static bool supported(); // Есть Matroska muxer в текущей сборке FFmpeg.
    bool open(const QString& path, const LiveVideoConfig& config, int audioDelaySamples = 0); // Путь должен быть новым; false — error().
    bool video(const LiveEncodedFrame& frame, qint64 durationUs = 0); // KEY содержит length/config, как в LiveVideoEncoder.
    bool audio(const QByteArray& packet, qint64 ptsUs); // Готовые stereo Opus 48 кГц, 20 мс; только после первого KEY.
    bool finish(bool keepAudio); // Закрыть spool, remux в path; false не оставляет файл для отправки.
    QString error() const; // Последняя ошибка с контекстом операции и FFmpeg.
    qint64 bytes() const; // Записанные байты spool, только worker.
private:
    struct Impl;
    std::unique_ptr<Impl> m; // Владеет mux context, временным spool и путём публикации.
};
