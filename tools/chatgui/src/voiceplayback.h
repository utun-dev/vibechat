/* Один общий проигрыватель собственного контейнера voice_file: worker декодирует PCM, SoundManager проигрывает.
 * Управление — GUI с живым QCoreApplication; SoundManager должен быть инициализирован для старта PCM.
 * Одновременно декодируется один файл; cancelPending/context подавляют доставку результата, но не останавливают worker.
 * decodeOpusFile — отдельное синхронное API без GUI: возвращает число 20мс пакетов, а не число PCM-фреймов. */
#pragma once

#include <QString>
#include <vector>
#include <cstdint>
#include <functional>
class QObject;


class VoicePlayer {
public:
    /* context — живой получатель в GUI; непустой ready(durationSec,id) вызывается в GUI, при busy — синхронно.
     * durationSec=0 при ошибке/busy; id при ошибке может сохранять прежнюю сессию и не является признаком успеха.
     * Успешное декодирование заменяет прежнюю сессию; уничтожение context/отмена подавляют callback и запуск PCM. */
    static void playOpusFile(const QString& filePath, QObject* context, std::function<void(float, uint64_t)> ready);
    static void cancelPending(); // Только инвалидирует результат; busy остаётся до доставки finished в GUI.
    static void stop(); // Также отменяет pending, удаляет свою PCM-сессию из SoundManager и обнуляет id.
    static bool isPlaying(); // Состояние своей PCM-сессии, не worker и не всех звуков приложения.
    static uint64_t playbackId(); // Текущий handle; может оставаться ненулевым после конца/внешней остановки PCM.
    static bool isPreparing(); // Декодирование/ожидание обработки finished в GUI, включая отменённый запрос.
    /* Рабочий поток: строгий контейнер 8/12/16/24/48 кГц, mono/stereo, пакеты по 20мс и обязательный end marker.
     * Успех: >0 пакетов, interleaved s16 в outPcm, частота/каналы/секунды в остальных out-параметрах; лимит PCM 256MiB.
     * Ошибка формата/I/O/декодера: -1, числовые out=0; outPcm может содержать уже декодированный префикс, его не использовать. */
    static int decodeOpusFile(const QString& filePath, std::vector<int16_t>& outPcm,
                                int& outSampleRate, int& outChannels, float& outDurationSec);
};
