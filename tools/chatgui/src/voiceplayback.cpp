#include "voiceplayback.h"
#include "sound_manager.h"

extern "C" {
#include "../../../lib/opus_codec.h"
}

#include "../../lib/debug_config.h"

#include <QFile>
#include <QDebug>
#include <QThread>
#include <QPointer>
#include <QCoreApplication>
#include <memory>

static const uint32_t OPUS_MAGIC = 0x5355504F;

int VoicePlayer::decodeOpusFile(const QString& filePath, std::vector<int16_t>& outPcm,
                               int& outSampleRate, int& outChannels, float& outDurationSec) {
    outPcm.clear(); outDurationSec = 0; outSampleRate = 0; outChannels = 0;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice: open failed path=%s reason=%s", qPrintable(filePath), qPrintable(file.errorString()));
        return -1;
    }
    uint32_t magic = 0, rate = 0;
    uint16_t channels = 0, frameSamples = 0;
    if (file.read((char*)&magic, 4) != 4 || file.read((char*)&rate, 4) != 4 ||
        file.read((char*)&channels, 2) != 2 || file.read((char*)&frameSamples, 2) != 2 || magic != OPUS_MAGIC ||
        (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) ||
        (channels != 1 && channels != 2) || frameSamples != rate / 50) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice: invalid header path=%s rate=%u channels=%u frame=%u",
                    qPrintable(filePath), rate, channels, frameSamples);
        return -1;
    }
    std::unique_ptr<opus_codec_decoder_t, decltype(&opus_codec_decoder_destroy)>
        decoder(opus_codec_decoder_create((int)rate, channels), opus_codec_decoder_destroy);
    if (!decoder) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice: decoder allocation failed"); return -1; }
    std::vector<int16_t> frame((size_t)frameSamples * channels);
    constexpr size_t maxPcmSamples = 256 * 1024 * 1024 / sizeof(int16_t);
    int frames = 0;
    for (;;) {
        uint16_t length = 0;
        if (file.read((char*)&length, 2) != 2) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice: missing end marker path=%s frame=%d", qPrintable(filePath), frames);
            return -1;
        }
        if (!length) {
            if (!frames || !file.atEnd()) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice: empty stream or trailing bytes path=%s", qPrintable(filePath));
                return -1;
            }
            break;
        }
        if (length > 4000 || outPcm.size() > maxPcmSamples - frame.size()) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice: packet/PCM limit exceeded path=%s frame=%d size=%u",
                        qPrintable(filePath), frames, length);
            return -1;
        }
        QByteArray packet = file.read(length);
        if (packet.size() != length || opus_codec_decode(decoder.get(), (const uint8_t*)packet.constData(),
                                                        length, frame.data(), frameSamples) != frameSamples) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice: truncated/invalid packet path=%s frame=%d", qPrintable(filePath), frames);
            return -1;
        }
        outPcm.insert(outPcm.end(), frame.begin(), frame.end()); frames++;
    }
    outSampleRate = (int)rate; outChannels = channels; outDurationSec = frames / 50.0f;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "voice: decoded path=%s frames=%d duration=%.2f", qPrintable(filePath), frames, outDurationSec);
    return frames;
}

static unsigned s_decodeGeneration = 0; /* Только GUI-поток. */
static bool s_decoding = false;
static uint64_t s_playbackId = 0;

void VoicePlayer::cancelPending() { s_decodeGeneration++; }
void VoicePlayer::stop() { cancelPending(); SoundManager::instance()->stopPcm(s_playbackId); s_playbackId = 0; }
bool VoicePlayer::isPlaying() { return SoundManager::instance()->isPcmPlaying(s_playbackId); }
uint64_t VoicePlayer::playbackId() { return s_playbackId; }
bool VoicePlayer::isPreparing() { return s_decoding; }

/* Декодирование и выделение PCM выполняются в worker; GUI принимает готовый буфер. */
void VoicePlayer::playOpusFile(const QString& filePath, QObject* context, std::function<void(float, uint64_t)> ready) {
    if (s_decoding) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "voice: decoder busy"); ready(0, 0); return; }
    struct Result {
        std::shared_ptr<std::vector<int16_t>> pcm = std::make_shared<std::vector<int16_t>>();
        int rate = 0, channels = 0;
        float duration = 0;
    };
    auto result = std::make_shared<Result>();
    unsigned generation = ++s_decodeGeneration;
    s_decoding = true;
    QPointer<QObject> receiver(context);
    auto* thread = QThread::create([filePath, result]() {
        try {
            if (decodeOpusFile(filePath, *result->pcm, result->rate, result->channels, result->duration) <= 0)
                result->duration = 0;
        } catch (const std::exception& error) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice: decode failed path=%s reason=%s", qPrintable(filePath), error.what());
            result->duration = 0;
        }
    });
    QObject::connect(thread, &QThread::finished, QCoreApplication::instance(), [result, receiver, ready, generation]() {
        s_decoding = false;
        if (!receiver || generation != s_decodeGeneration) return;
        if (result->duration > 0) {
            SoundManager::instance()->stopPcm(s_playbackId);
            s_playbackId = SoundManager::instance()->playRawPcm(result->pcm->data(),
                (int)(result->pcm->size() / result->channels), result->rate, result->channels, result->pcm);
            if (!s_playbackId) result->duration = 0;
        }
        ready(result->duration, s_playbackId);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
