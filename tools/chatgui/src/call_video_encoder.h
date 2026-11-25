/* Кодер видеотрека звонка; только video worker, без камеры, транспорта и таймеров.
 * Смена bitrate ожидает ближайший IDR (период <= 1с по capture PTS). Новый encoder
 * получает поток после полного drain старого; completion выдаётся после проверки нового IDR. Приоритет GPU, затем CPU;
 * явный backend фиксирует выбор для теста/замены. Completion передаётся в uasync
 * независимо от admission видео. PTS, generation и frame_id принадлежат владельцу.
 * open получает согласованный call_video_format; соответствие level/размера/FPS
 * проверяет общий модуль, фактический bitstream — LiveVideoEncoder. CAPS проверяет владелец. */
#pragma once
#include "live_video_encoder.h"
#include "../../../src/call/call_video.h"
#include <future>

struct CallVideoRateCompletion {
    uint64_t generation = 0, request = 0;
    uint32_t bitrate = 0; // request != 0, bitrate == 0 — отказ; request == 0 — нет результата.
};

class CallVideoEncoder {
public:
    // true — открыт проверенный кандидат; при отказе прежний encoder остаётся, причина в error().
    // HEVC TX — только Main tier; format заранее сверяет с CAPS владелец.
    bool open(const call_video_format& format, uint32_t bitrate, const QString& backend = {});
    void close(); // Ждёт фоновой подготовки, отбрасывает буферизованный выход/pending-команду и очищает error().
    // true — одна команда принята; actual completion приходит из encode на границе KEY.
    bool requestBitrate(uint64_t generation, uint64_t request, uint32_t bitrate);
    // Пустое image забирает готовые кадры без нового input (pts игнорируется).
    // На false output пуст; completion нужно обработать и на ошибке. PTS готового выхода сохраняет capture PTS.
    bool encode(const QImage& image, uint64_t pts, bool forceKey, QList<LiveEncodedFrame>& output, CallVideoRateCompletion& completion);
    uint32_t bitrate() const { return m_encoder ? m_config.bitrate : 0; }
    unsigned pendingFrames() const { return m_encoder ? m_encoder->pendingFrames() : 0; }
    QString error() const { return m_error; }
private:
    struct Prepared {
        std::unique_ptr<LiveVideoEncoder> encoder;
        QString error;
        qint64 elapsed = 0;
    };
    std::unique_ptr<LiveVideoEncoder> m_encoder;
    LiveVideoConfig m_config;
    call_video_format m_format{};
    QString m_error;
    uint64_t m_request = 0, m_lastRequest = 0, m_lastPts = 0, m_lastKey = 0, m_lastOutputPts = 0, m_switchPts = 0;
    qint64 m_backgroundMs = 0;
    uint32_t m_pendingBitrate = 0;
    bool m_haveFrame = false, m_haveInput = false, m_forceKey = false, m_keyPending = false, m_switching = false;
    // Отдельный контекст готовится в фоне; worker забирает его только после завершения open.
    std::future<Prepared> m_prepared;
};
