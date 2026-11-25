/* Настоящие null/PulseAudio devices и Qt timers; wrappers инъецируют сбои либо параметры ручного замера. */
#include "../src/audio_device.h"
#include "../src/call_audio_engine.h"
#include "../src/radio_audio_engine.h"
#include "../src/audiorecorder.h"
#include "../src/sound_manager.h"
#include <QCoreApplication>
#include <QThread>
#include <QFile>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>
#include <mutex>
extern "C" {
#include "utun_instance.h"
#include "call/call_audio.h"
#include "radio/radio_audio.h"
#include "radio/radio.h"
#include "media_async/attachment_send.h"
#include "opus_codec.h"
#include "u_async.h"
#include "debug_config.h"
}

#define REQUIRE(condition) do { if (!(condition)) qFatal("audio recovery line %d: %s", __LINE__, #condition); } while (0)
static UTUN_INSTANCE instance{};
static const QString radioGroup(64, QLatin1Char('7'));
static const QString faultRadioGroup(64, QLatin1Char('8'));
static const QString firstBufferGroup(64, QLatin1Char('a'));
static const QString secondBufferGroup(64, QLatin1Char('b'));
static std::map<ma_context*, int> live;
static std::vector<ma_device*> devices;
static int starts, captureStarts, playbackStarts, contextStarts, failContexts;
static bool failStart, failContext, failDevice;
static bool pulseIntegration;
static bool latencyProbe;
static unsigned probeCaptureFrames, probePlaybackFrames;
static std::mutex deviceMutex;
static std::map<ma_device*, ma_device_data_proc> captureCallbacks;
static std::atomic<int> microphoneFault{0}, captureCloses{0}; // 1: нет callback, 2: nullptr, 3: frames=0.
static std::atomic<bool> slowCaptureClose{false};
static std::atomic<unsigned> idleReferences{0}, missingReferences{0};

static void referenceDiagnostic(int, const char*, const char* message) {
    if (std::strstr(message, "voice-io: reference session=") && std::strstr(message, "state=idle_silence")) ++idleReferences;
    if (std::strstr(message, "voice-io: session=") && std::strstr(message, "missing_ref=")) ++missingReferences;
}

static void microphoneData(ma_device* device, void* out, const void* in, ma_uint32 frames) {
    ma_device_data_proc callback;
    { std::lock_guard guard(deviceMutex); callback = captureCallbacks.at(device); }
    int fault = microphoneFault.load();
    if (fault == 1) return;
    callback(device, out, fault == 2 ? nullptr : in, fault == 3 ? 0 : frames);
}

extern "C" UTUN_INSTANCE* gui_bridge_get_inst() { return &instance; }
extern "C" int gui_bridge_is_uasync_ready() { return instance.ua != nullptr; }
extern "C" uint64_t gui_bridge_radio_route(const char* channel) {
    const QString group = QString::fromLatin1(channel);
    if (group == radioGroup) return 7;
    if (group == faultRadioGroup) return 8;
    if (group == firstBufferGroup) return 71;
    if (group == secondBufferGroup) return 73;
    return 0;
}
extern "C" void gui_bridge_post_uasync_fn(void (*fn)(void*), void* arg) { uasync_post(instance.ua, fn, arg); }
extern "C" int gui_bridge_post_bound_fn(void (*fn)(void*), void* arg, UTUN_INSTANCE** binding) {
    if (!instance.ua || !fn || !arg || !binding) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "audio recovery bridge: rejected bound command ua=%p callback=%p arg=%p binding=%p",
            static_cast<void*>(instance.ua), reinterpret_cast<void*>(fn), arg, static_cast<void*>(binding));
        return -1;
    }
    *binding = &instance;
    gui_bridge_post_uasync_fn(fn, arg);
    return 0;
}
extern "C" ma_result __real_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context*);
extern "C" void __real_ma_context_uninit(ma_context*);
extern "C" ma_result __real_ma_device_init(ma_context*, const ma_device_config*, ma_device*);
extern "C" ma_result __real_ma_device_start(ma_device*);
extern "C" void __real_ma_device_uninit(ma_device*);
extern "C" int __real_audio_context_failed(ma_context*);
extern "C" int __real_call_send_media(UTUN_INSTANCE*, uint64_t, const uint8_t*, int, const call_capture_time*);
extern "C" int __wrap_call_send_media(UTUN_INSTANCE* inst, uint64_t id, const uint8_t* data, int size, const call_capture_time* timing) {
    return latencyProbe ? 0 : __real_call_send_media(inst, id, data, size, timing);
}

extern "C" ma_result __wrap_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context* context) {
    ++contextStarts;
    if (failContexts > 0) { --failContexts; return MA_ERROR; }
    ma_backend backend = pulseIntegration ? ma_backend_pulseaudio : ma_backend_null;
    return __real_ma_context_init(&backend, 1, nullptr, context);
}
extern "C" void __wrap_ma_context_uninit(ma_context* context) {
    REQUIRE(live[context] == 0);
    __real_ma_context_uninit(context);
}
extern "C" ma_result __wrap_ma_device_init(ma_context* context, const ma_device_config* config, ma_device* device) {
    if (failDevice) { failDevice = false; return MA_ERROR; }
    ma_device_config requested = *config;
    if (config->deviceType == ma_device_type_capture) {
        { std::lock_guard guard(deviceMutex); captureCallbacks[device] = config->dataCallback; }
        requested.dataCallback = microphoneData;
    }
    // Второй виртуальный вход: отдельный null device с тем же аппаратным драйвером.
    if (context->backend == ma_backend_null && config->deviceType == ma_device_type_capture &&
        config->capture.pDeviceID && config->capture.pDeviceID->nullbackend == 1) requested.capture.pDeviceID = nullptr;
    if (latencyProbe) {
        requested.periodSizeInMilliseconds = 0;
        requested.periodSizeInFrames = config->deviceType == ma_device_type_capture ? probeCaptureFrames : probePlaybackFrames;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "latency-probe: override type=%d period_frames=%u rate=%u",
                   config->deviceType, requested.periodSizeInFrames, requested.sampleRate);
    }
    ma_result result = __real_ma_device_init(context, &requested, device);
    { std::lock_guard guard(deviceMutex);
      if (result == MA_SUCCESS) { ++live[context]; devices.push_back(device); }
      else captureCallbacks.erase(device); }
    return result;
}
extern "C" ma_result __wrap_ma_device_start(ma_device* device) {
    ++starts;
    if (device->type == ma_device_type_capture) {
        ++captureStarts;
        // Некоторые backends вызывают data callback ещё до возврата start.
        std::vector<int16_t> input(1200 * device->capture.channels, 1000);
        if (!pulseIntegration) device->onData(device, nullptr, input.data(), 1200);
    } else ++playbackStarts;
    if (failStart) { failStart = false; return MA_ERROR; }
    return __real_ma_device_start(device);
}
extern "C" void __wrap_ma_device_uninit(ma_device* device) {
    auto context = device->pContext;
    bool capture = device->type == ma_device_type_capture;
    if (capture && slowCaptureClose) QThread::msleep(700);
    __real_ma_device_uninit(device);
    std::lock_guard guard(deviceMutex);
    REQUIRE(live[context] > 0);
    --live[context];
    devices.erase(std::find(devices.begin(), devices.end(), device));
    captureCallbacks.erase(device);
    if (capture) captureCloses++;
}
extern "C" int __wrap_audio_context_failed(ma_context* context) {
    if (failContext) { failContext = false; return 1; }
    return __real_audio_context_failed(context);
}

static void pump(int milliseconds) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        if (latencyProbe && instance.ua) uasync_poll(instance.ua, 0);
        QThread::msleep(1);
    }
}

static void require_output_idle(SoundManager* manager) {
    QElapsedTimer wait; wait.start();
    while (manager->outputAvailable() && wait.elapsed() < 2000) pump(20);
    REQUIRE(!manager->outputAvailable());
    qInfo("output idle: closed after wait=%lldms", (long long)wait.elapsed());
}

/* Общий playback засыпает без источников и не теряет позицию приостановленного PCM. */
static void output_idle(SoundManager* manager) {
    REQUIRE(!manager->outputAvailable());
    std::vector<int16_t> first(48000 * 2, 1000), second(48000 * 2, -1000);
    auto firstId = manager->playRawPcm(first.data(), (int)first.size(), 48000, 1);
    REQUIRE(firstId && manager->outputAvailable());
    pump(100);
    auto secondId = manager->playRawPcm(second.data(), (int)second.size(), 48000, 1);
    REQUIRE(secondId && secondId != firstId);
    manager->stopPcm(firstId); pump(700);
    REQUIRE(manager->outputAvailable() && manager->isPcmPlaying(secondId));

    manager->pausePcm(secondId);
    uint64_t paused = manager->pcmCursor(secondId);
    require_output_idle(manager);
    REQUIRE(manager->pcmCursor(secondId) == paused);
    manager->resumePcm(secondId);
    REQUIRE(manager->outputAvailable());
    pump(100);
    REQUIRE(manager->pcmCursor(secondId) > paused);
    manager->stopPcm(secondId); require_output_idle(manager);

    std::vector<int16_t> shortPcm(4800, 500);
    auto shortId = manager->playRawPcm(shortPcm.data(), (int)shortPcm.size(), 48000, 1);
    REQUIRE(shortId && manager->outputAvailable());
    require_output_idle(manager);
    REQUIRE(!manager->isPcmPlaying(shortId));
    manager->stopPcm(shortId);
    int previousStarts = playbackStarts;
    pump(500);
    REQUIRE(playbackStarts == previousStarts && !manager->outputAvailable());
    qInfo("output idle: startup, multiple PCM, pause/resume, natural end and no spontaneous reopen PASS");
}

/* Включённая рация без RX не держит output; входящая серия будит его без сброса jitter. */
static void radio_output_idle(SoundManager* manager, RadioAudioEngine* radio) {
    debug_level_t previous = g_debug_config.category_levels[DEBUG_CATEGORY_AEC];
    debug_set_category_level(DEBUG_CATEGORY_AEC, DEBUG_LEVEL_INFO);
    debug_set_log_hook(referenceDiagnostic);
    idleReferences = missingReferences = 0;
    REQUIRE(radio->start(radioGroup) == 0);
    require_output_idle(manager);
    pump(100); REQUIRE(idleReferences > 0);
    unsigned missing = missingReferences.load();
    radio->talkBegin(); pump(150);
    REQUIRE(!manager->outputAvailable() && radio->captureAvailable() && missingReferences == missing);
    radio->talkEnd();

    auto* encoder = opus_codec_encoder_create(48000, 1); REQUIRE(encoder);
    int16_t speech[960]; std::fill_n(speech, 960, 1200);
    uint8_t opus[256]; int len = opus_codec_encode(encoder, speech, 960, opus, sizeof(opus)); REQUIRE(len > 0);
    opus_codec_encoder_destroy(encoder);
    const uint8_t source_ed[32] = {0xd2, 4};
    for (uint16_t seq = 0; seq < 8; ++seq)
        radio_audio_on_frame(&instance, 7, source_ed, 1, seq, 0, opus, len, nullptr);
    REQUIRE(radio_audio_output_needed());
    for (int i = 0; i < 100 && !manager->outputAvailable(); ++i) pump(20);
    REQUIRE(manager->outputAvailable());
    radio_audio_on_frame(&instance, 7, source_ed, 1, 8, 1, nullptr, 0, nullptr);
    for (int i = 0; i < 200 && manager->outputAvailable(); ++i) pump(20);
    REQUIRE(!manager->outputAvailable());
    pump(100); REQUIRE(idleReferences >= 2);
    radio->stop();
    debug_set_log_hook(nullptr);
    debug_set_category_level(DEBUG_CATEGORY_AEC, previous);
    qInfo("radio output: idle AEC, local PTT without missing reference, incoming wake and FIN drain PASS");
}

static void shared_capture(SoundManager* manager) {
    auto inputs = manager->enumerateCaptureDevices();
    REQUIRE(!inputs.empty());
    QString selected;
    for (const auto& input : inputs) if (input.isDefault) selected = input.id;
    REQUIRE(!selected.isEmpty());
    AudioCapture stream, radio;
    std::atomic<unsigned> streamFrames{0}, radioFrames{0};
    std::atomic<int64_t> streamTime{0};
    std::atomic<bool> compareTime{false};
    int streamReady = 0, radioReady = 0, streamStopped = 0, radioStopped = 0, radioChannels = 0;
    auto startStream = [&] {
        return stream.open(selected, AudioCapture::Role::Stream, 2, 1, [&](int channels, int) {
            REQUIRE(channels == 2); ++streamReady; return true;
        }, [&] { ++streamStopped; }, [&](const AudioCapture::Frame& frame) {
            REQUIRE(frame.channels == 2 && frame.timeUs > 0 && frame.pcm);
            streamTime = frame.timeUs; streamFrames += frame.frames;
        });
    };
    auto startRadio = [&] {
        return radio.open({}, AudioCapture::Role::Radio, 1, 2, [&](int channels, int) {
            radioChannels = channels; ++radioReady; return true;
        }, [&] { ++radioStopped; }, [&](const AudioCapture::Frame& frame) {
            REQUIRE(frame.channels == radioChannels && frame.pcm);
            if (compareTime) REQUIRE(frame.timeUs == streamTime.load());
            radioFrames += frame.frames;
        });
    };
    int before = captureStarts;
    REQUIRE(startStream()); REQUIRE(startRadio()); compareTime = true; pump(80);
    REQUIRE(captureStarts == before + 1 && radioChannels == 2 && streamReady == 1 && radioReady == 1);
    REQUIRE(streamFrames > 0 && radioFrames > 0);
    radio.close(); auto captured = streamFrames.load(); pump(60);
    REQUIRE(streamFrames > captured && captureStarts == before + 1 && streamStopped == 0);
    REQUIRE(startRadio()); REQUIRE(captureStarts == before + 1);
    // Detach stream не закрывает устройство и не меняет формат оставшейся рации.
    compareTime = false; stream.close(); streamTime = 0; captured = radioFrames.load(); pump(60);
    REQUIRE(radio.running() && radioFrames > captured && radioChannels == 2 && captureStarts == before + 1);
    radio.close(); REQUIRE(streamStopped == 1 && radioStopped == 2);
    // Обратный порядок: новый stream заменяет mono-профиль рации на свой stereo.
    before = captureStarts;
    REQUIRE(startRadio()); REQUIRE(radioChannels == 1);
    REQUIRE(startStream()); compareTime = true; pump(60);
    REQUIRE(captureStarts == before + 2 && radioChannels == 2 && stream.running() && radio.running());
    // Потеря устройства: обе подписки восстанавливаются одним открытием.
    for (auto* device : devices) if (device->type == ma_device_type_capture) REQUIRE(ma_device_stop(device) == MA_SUCCESS);
    stream.recover(); REQUIRE(!stream.running() && !radio.running());
    pump(300); radio.recover();
    REQUIRE(stream.running() && radio.running() && captureStarts == before + 3);
    radio.close(); stream.close();
    compareTime = false;
    // Разные выбранные устройства сохраняют независимые форматы и жизненные циклы.
    ma_device_id secondId{}; secondId.nullbackend = 1;
    QString second = QString::fromLatin1(ma_get_backend_name(manager->context()->backend)) + '/' +
        QByteArray(reinterpret_cast<const char*>(&secondId), sizeof(secondId)).toBase64();
    before = captureStarts;
    REQUIRE(startStream());
    REQUIRE(radio.open(second, AudioCapture::Role::Radio, 1, 2, [](int channels, int) { REQUIRE(channels == 1); return true; },
        [] {}, [](const AudioCapture::Frame& frame) { REQUIRE(frame.channels == 1); }));
    REQUIRE(captureStarts == before + 2);
    captured = streamFrames.load(); radio.close(); pump(60);
    REQUIRE(streamFrames > captured && captureStarts == before + 2);
    // Неверный явный выбор не заменяется default и не отбирает захват трансляции.
    REQUIRE(!radio.open(QStringLiteral("invalid-input"), AudioCapture::Role::Radio, 1, 2, [](int, int) { return true; },
        [] {}, [](const AudioCapture::Frame&) {}));
    REQUIRE(stream.running() && captureStarts == before + 2);
    radio.close(); stream.close();
    qInfo("shared capture: default/explicit alias, stream priority, attach/detach without restart, recovery PASS");
}

static void pulse_recovery(SoundManager* manager, const QString& control) {
    REQUIRE(manager->context()->backend == ma_backend_pulseaudio && !manager->outputAvailable());
    instance.ua = uasync_create(); REQUIRE(instance.ua);
    REQUIRE(radio_init_instance(&instance) == 0);
    REQUIRE(call_audio_init(&instance) == 0 && radio_audio_init(&instance) == 0);
    auto* call = CallAudioEngine::instance(); auto* radio = RadioAudioEngine::instance();
    REQUIRE(radio->start(radioGroup) == 0);
    require_output_idle(manager); // Радио включено, входящей речи нет: sink-input отсутствует.
    QFile idleReady(control + "/idle_ready"); REQUIRE(idleReady.open(QIODevice::WriteOnly)); idleReady.close();
    REQUIRE(call->start(42) == 0);
    QString separate;
    for (const auto& output : manager->enumeratePlaybackDevices()) {
        ma_device_id id{};
        if (manager->deviceId(output.id, &id) && !strcmp(id.pulse, "utun_test_voice")) separate = output.id;
    }
    REQUIRE(!separate.isEmpty()); call->setPlaybackDevice(separate);
    AudioRecorder recorder; recorder.init(); recorder.startRecording();
    std::vector<int16_t> pcm(48000 * 20, 1000);
    auto pcmId = manager->playRawPcm(pcm.data(), (int)pcm.size(), 48000, 1);
    for (int i = 0; i < 100 && recorder.durationMs() < 300; ++i) pump(50);
    REQUIRE(recorder.durationMs() >= 300 && manager->pcmCursor(pcmId) > 0);
    QFile routesReady(control + "/routes_ready"); REQUIRE(routesReady.open(QIODevice::WriteOnly)); routesReady.close();
    for (int i = 0; i < 100 && !QFile::exists(control + "/moved"); ++i) pump(50);
    REQUIRE(QFile::exists(control + "/moved"));
    pump(500);
    int outputs = 0;
    for (auto* device : devices) if (device->type == ma_device_type_playback) ++outputs;
    REQUIRE(outputs == 1); // Сервер переместил default stream на voice sink: получился один микшер.
    qInfo("PulseAudio route move: call/radio/media merged into one output PASS");
    for (auto* device : devices) if (device->type == ma_device_type_playback) REQUIRE(ma_device_stop(device) == MA_SUCCESS);
    pump(1000); outputs = 0;
    uint64_t wantedRoute = UINT64_C(14695981039346656037);
    for (const char* name = "utun_test_voice"; *name; ++name) { wantedRoute ^= (unsigned char)*name; wantedRoute *= UINT64_C(1099511628211); }
    bool foundWanted = false;
    for (auto* device : devices) if (device->type == ma_device_type_playback) {
        ++outputs;
        if (static_cast<AudioDevice*>(device->pUserData)->route() == wantedRoute) foundWanted = true;
    }
    REQUIRE(foundWanted && outputs >= 1 && outputs <= 2); // Сервер может восстановить прежний move для default stream.
    qInfo("PulseAudio output recovery: desired call route preserved outputs=%d PASS", outputs);
    int duration = recorder.durationMs();
    auto cursor = manager->pcmCursor(pcmId);
    QFile ready(control + "/ready"); REQUIRE(ready.open(QIODevice::WriteOnly)); ready.close();
    for (int i = 0; i < 100 && manager->context(); ++i) pump(50);
    REQUIRE(!manager->context());
    QFile closed(control + "/closed"); REQUIRE(closed.open(QIODevice::WriteOnly)); closed.close();
    for (int i = 0; i < 150 && (!manager->context() || recorder.durationMs() <= duration + 200); ++i) pump(50);
    REQUIRE(manager->context() && recorder.durationMs() > duration + 200 && manager->pcmCursor(pcmId) > cursor);
    REQUIRE(call->isActive() && call->callId() == 42 && radio->isActive() && radio->groupId() == radioGroup);
    qInfo("PulseAudio progress: capture=%d -> %dms playback=%llu -> %llu frames", duration, recorder.durationMs(),
          (unsigned long long)cursor, (unsigned long long)manager->pcmCursor(pcmId));
    call->stop(); radio->stop(); recorder.stopRecording(); manager->stopPcm(pcmId);
    pump(900);
    REQUIRE(!manager->outputAvailable());
    QFile idleClosed(control + "/idle_closed"); REQUIRE(idleClosed.open(QIODevice::WriteOnly)); idleClosed.close();
    call_audio_destroy(&instance); radio_audio_destroy(&instance); radio_destroy_instance(&instance);
    uasync_destroy(instance.ua, 0); instance.ua = nullptr;
    manager->shutdown();
    REQUIRE(devices.empty());
    qInfo("real PulseAudio: idle stream removal and capture/playback recovery after server restart PASS");
}

/* Ручной стенд: настоящий I/O и voice workers, без сети и сохранения микрофонного PCM.
 * --latency capture_frames playback_frames seconds [call|radio|output] [log]; 0 означает backend default. */
static void measure_latency(SoundManager* manager, int seconds, const QString& kind) {
    REQUIRE(manager->context()->backend == ma_backend_pulseaudio);
    instance.ua = uasync_create(); REQUIRE(instance.ua);
    REQUIRE(radio_init_instance(&instance) == 0);
    REQUIRE(call_audio_init(&instance) == 0 && radio_audio_init(&instance) == 0);
    auto* call = CallAudioEngine::instance(); auto* radio = RadioAudioEngine::instance();
    if (kind == "call") REQUIRE(call->start(42) == 0);
    if (kind == "radio") REQUIRE(radio->start(radioGroup) == 0);
    if (kind == "output") REQUIRE(manager->attachStream(90004, [](float*, unsigned, int64_t, uint64_t) {}));
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "latency-probe: begin kind=%s seconds=%d capture_frames=%u playback_frames=%u",
               qPrintable(kind), seconds, probeCaptureFrames, probePlaybackFrames);
    pump(seconds * 1000);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "latency-probe: end kind=%s", qPrintable(kind));
    call->stop(); radio->stop();
    if (kind == "output") manager->detachStream(90004);
    call_audio_destroy(&instance); radio_audio_destroy(&instance); radio_destroy_instance(&instance);
    uasync_destroy(instance.ua, 0); instance.ua = nullptr;
    manager->shutdown(); REQUIRE(devices.empty());
}

/* Null backend выдаёт цифровую тишину; отсутствие PCM инъецируется независимо от уровня сигнала. */
static void microphone_watchdog(SoundManager* manager) {
    instance.ua = uasync_create(); REQUIRE(instance.ua);
    REQUIRE(radio_init_instance(&instance) == 0);
    REQUIRE(call_audio_init(&instance) == 0 && radio_audio_init(&instance) == 0);
    auto* call = CallAudioEngine::instance(); auto* radio = RadioAudioEngine::instance();
    int callErrors = 0, beats = 0;
    QObject::connect(call, &CallAudioEngine::captureErrorChanged, [&] { if (!call->captureError().isEmpty()) callErrors++; });
    QTimer heartbeat; heartbeat.setInterval(10); QObject::connect(&heartbeat, &QTimer::timeout, [&] { beats++; }); heartbeat.start();
    for (int fault : {1, 2, 3}) {
        microphoneFault = 0;
        REQUIRE(call->start(42) == 0 && radio->start(radioGroup) == 0);
        pump(1300); // Более секунды корректной тишины не вызывает ошибку и не усыпляет активный voice output.
        REQUIRE(call->captureError().isEmpty() && radio->captureError().isEmpty() && manager->outputAvailable());
        REQUIRE(radio->captureAvailable()); radio->talkBegin();
        int before = captureStarts, closed = captureCloses, previousBeats = beats;
        slowCaptureClose = true; microphoneFault = fault;
        pump(1400);
        REQUIRE(!call->captureError().isEmpty() && !radio->captureError().isEmpty());
        REQUIRE(call->isActive() && radio->isActive() && !radio->captureAvailable() && !radio->isTalking());
        REQUIRE(beats - previousBeats >= 80); // uninit 700мс выполняется вне GUI.
        radio->talkBegin(); radio->setHotkeyTalking(true); REQUIRE(!radio->isTalking());
        if (fault == 3) {
            QElapsedTimer elapsed; elapsed.start(); call->stop(); radio->stop();
            REQUIRE(elapsed.elapsed() < 300); // Stop не ждёт уже закрывающийся неисправный backend.
            AudioCaptureLease exclusive(AudioCaptureLease::Mode::Exclusive);
            REQUIRE(!exclusive.acquire()); REQUIRE(call->start(44) == -1);
        }
        pump(800); REQUIRE(captureCloses == closed + 2);
        slowCaptureClose = false; microphoneFault = 0; pump(1200);
        REQUIRE(captureStarts == before); // Самопроизвольный возврат PCM не перезапускает отключённый вход.
        call->stop(); radio->stop(); pump(50);
        REQUIRE(callErrors == fault);
    }
    // Первого callback нет вообще: те же ошибка и остановка, без повторных открытий.
    microphoneFault = 1;
    REQUIRE(call->start(43) == 0 && radio->start(faultRadioGroup) == 0);
    int before = captureStarts, closed = captureCloses;
    pump(1500);
    REQUIRE(!call->captureError().isEmpty() && !radio->captureError().isEmpty());
    REQUIRE(captureStarts == before && captureCloses == closed + 2);
    call->stop(); radio->stop(); pump(50);
    call_audio_destroy(&instance); radio_audio_destroy(&instance); radio_destroy_instance(&instance);
    uasync_destroy(instance.ua, 0); instance.ua = nullptr;
    manager->shutdown(); REQUIRE(devices.empty());
    qInfo("microphone watchdog: silence, no callbacks, null PCM, zero frames, no retry, nonblocking close, RX retained PASS");
}

/* Управляемое время проверяет длительные интервалы и предел без минут ожидания. */
static void buffer_tuning_policy() {
    AudioBufferTuning tuning;
    tuning.begin(0, 10, 3);
    REQUIRE(tuning.observe(500, 100) == AudioBufferTuning::Unchanged && !tuning.requestedMs);
    REQUIRE(tuning.observe(1500, 100) == AudioBufferTuning::Unchanged); // Одна пачка — один сбой.
    REQUIRE(tuning.observe(7000, 1) == AudioBufferTuning::Unchanged); // Старый единичный сбой уже забыт.
    REQUIRE(tuning.observe(7200, 1) == AudioBufferTuning::Increase && tuning.requestedMs == 15);
    tuning.begin(7300, 15, 3);
    REQUIRE(tuning.observe(7500, 100) == AudioBufferTuning::Unchanged);
    REQUIRE(tuning.observe(8500, 1) == AudioBufferTuning::Unchanged);
    REQUIRE(tuning.observe(8700, 1) == AudioBufferTuning::Unchanged && tuning.requestedMs == 15);
    REQUIRE(tuning.observe(12200, 0) == AudioBufferTuning::Unchanged); // Один лишь таймер не запускает reopen.
    REQUIRE(tuning.observe(12200, 1) == AudioBufferTuning::Increase && tuning.requestedMs == 20);
    int64_t now = 12200;
    for (unsigned expected : {25u, 30u, 33u}) {
        tuning.begin(now + 100, tuning.requestedMs, 3); now += 5000;
        REQUIRE(tuning.observe(now, 1) == AudioBufferTuning::Unchanged);
        REQUIRE(tuning.observe(now + 200, 1) == AudioBufferTuning::Increase && tuning.requestedMs == expected);
        REQUIRE(tuning.requestedMs * tuning.periods <= 100); now += 200;
    }
    tuning.begin(now + 100, 33, 3); now += 5000;
    REQUIRE(tuning.observe(now, 1) == AudioBufferTuning::Unchanged);
    REQUIRE(tuning.observe(now + 200, 1) == AudioBufferTuning::Limit && tuning.requestedMs == 33);
    tuning = {}; tuning.baseMs = 20; tuning.begin(0, 10, 3); // Backend может округлить исходный запрос вниз.
    REQUIRE(tuning.observe(1100, 1) == AudioBufferTuning::Unchanged);
    REQUIRE(tuning.observe(1300, 1) == AudioBufferTuning::Increase && tuning.requestedMs == 25);
    ma_context context{}; context.backend = ma_backend_pulseaudio;
    auto config = ma_device_config_init(ma_device_type_capture); config.periodSizeInMilliseconds = 20;
    AudioDevice::requestLowLatency(&context, config);
    REQUIRE(config.periodSizeInMilliseconds == 10 && !config.periodSizeInFrames);
}

/* Та же notification, что у WASAPI/ALSA/PA; устройства и Qt recovery настоящие, backend виртуальный. */
static void inject_capture_glitch() {
    std::lock_guard guard(deviceMutex);
    for (auto* device : devices) if (device->type == ma_device_type_capture) {
        ma_device_notification event{}; event.pDevice = device; event.type = ma_device_notification_type_capture_overrun;
        device->onNotification(&event);
    }
}

static void adaptive_buffers(SoundManager* manager) {
    buffer_tuning_policy();
    instance.ua = uasync_create(); REQUIRE(instance.ua);
    REQUIRE(radio_init_instance(&instance) == 0);
    REQUIRE(call_audio_init(&instance) == 0 && radio_audio_init(&instance) == 0);
    auto* call = CallAudioEngine::instance(); auto* radio = RadioAudioEngine::instance();
    REQUIRE(call->start(70) == 0 && radio->start(firstBufferGroup) == 0);
    pump(1300);
    std::vector<unsigned> initial;
    for (auto* device : devices) if (device->type == ma_device_type_capture) initial.push_back(device->capture.internalPeriodSizeInFrames);
    std::sort(initial.begin(), initial.end()); REQUIRE(initial.size() == 2);
    int before = captureStarts;
    inject_capture_glitch(); pump(250); REQUIRE(captureStarts == before);
    inject_capture_glitch(); pump(600); REQUIRE(captureStarts == before + 2);
    std::vector<unsigned> grown;
    for (auto* device : devices) if (device->type == ma_device_type_capture) grown.push_back(device->capture.internalPeriodSizeInFrames);
    std::sort(grown.begin(), grown.end()); REQUIRE(grown.size() == 2);
    REQUIRE(grown[0] == initial[0] + 240 && grown[1] == initial[1] + 240);
    REQUIRE(call->captureError().isEmpty() && radio->captureError().isEmpty() && radio->captureAvailable());
    pump(4500); slowCaptureClose = true;
    inject_capture_glitch(); pump(250); inject_capture_glitch(); pump(250);
    QElapsedTimer elapsed; elapsed.start();
    AudioCapture incoming;
    REQUIRE(!incoming.open({}, AudioCapture::Role::Stream, 2, 75, [](int, int) { return true; }, [] {}, [](const auto&) {}));
    REQUIRE(!incoming.subscribed() && elapsed.elapsed() < 300);
    elapsed.restart(); call->stop(); radio->stop();
    REQUIRE(elapsed.elapsed() < 300); // Stop не ждёт выполняющееся закрытие для изменения буфера.
    pump(900); slowCaptureClose = false;
    REQUIRE(call->start(72) == 0 && radio->start(secondBufferGroup) == 0); pump(100);
    std::vector<unsigned> reset;
    for (auto* device : devices) if (device->type == ma_device_type_capture) reset.push_back(device->capture.internalPeriodSizeInFrames);
    std::sort(reset.begin(), reset.end()); REQUIRE(reset == initial);
    call->stop(); radio->stop(); pump(50);
    call_audio_destroy(&instance); radio_audio_destroy(&instance); radio_destroy_instance(&instance);
    uasync_destroy(instance.ua, 0); instance.ua = nullptr;
    manager->shutdown(); REQUIRE(devices.empty());
    qInfo("adaptive buffers: startup, isolated faults, cooldown, limit, actual reopen, async stop, new session baseline PASS");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    latencyProbe = argc >= 2 && std::strcmp(argv[1], "--latency") == 0;
    int seconds = 0;
    QString kind = argc >= 6 ? QString::fromLocal8Bit(argv[5]) : "call";
    if (latencyProbe) {
        REQUIRE(argc >= 5 && argc <= 7);
        bool captureOk, playbackOk, durationOk;
        probeCaptureFrames = QString::fromLocal8Bit(argv[2]).toUInt(&captureOk);
        probePlaybackFrames = QString::fromLocal8Bit(argv[3]).toUInt(&playbackOk);
        seconds = QString::fromLocal8Bit(argv[4]).toInt(&durationOk);
        REQUIRE(captureOk && playbackOk && durationOk && seconds > 0 && seconds <= 3600);
        REQUIRE(probeCaptureFrames <= 4800 && probePlaybackFrames <= 4800);
        REQUIRE(kind == "call" || kind == "radio" || kind == "output");
    }
    pulseIntegration = latencyProbe || (argc == 3 && std::strcmp(argv[1], "--pulse-integration") == 0);
    debug_config_init(); debug_set_level(pulseIntegration ? DEBUG_LEVEL_INFO : DEBUG_LEVEL_WARN);
    if (latencyProbe) {
        debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_TRACE);
        debug_set_category_level(DEBUG_CATEGORY_AEC, DEBUG_LEVEL_DEBUG);
        if (argc == 7) { debug_enable_file_output(argv[6], 1); REQUIRE(g_debug_config.file_output); }
    }
    auto* manager = SoundManager::instance();
    REQUIRE(manager->init() && manager->context() && !manager->outputAvailable());
    pump(700);
    REQUIRE(!manager->outputAvailable());
    if (argc == 2 && std::strcmp(argv[1], "--microphone-watchdog") == 0) { microphone_watchdog(manager); return 0; }
    if (argc == 2 && std::strcmp(argv[1], "--buffer-tuning") == 0) { adaptive_buffers(manager); return 0; }
    if (latencyProbe) { measure_latency(manager, seconds, kind); return 0; }
    if (pulseIntegration) { pulse_recovery(manager, QString::fromLocal8Bit(argv[2])); return 0; }
    output_idle(manager);
    ma_device_id id{};
    auto outputs = manager->enumeratePlaybackDevices();
    REQUIRE(!outputs.empty() && SoundManager::deviceId(outputs.front().id, &id));
    REQUIRE(!SoundManager::deviceId(outputs.front().id + '!', &id));
    shared_capture(manager);
    {
        AudioDevice device;
        auto config = ma_device_config_init(ma_device_type_capture);
        config.capture.format = ma_format_s16; config.capture.channels = 1; config.sampleRate = 48000;
        REQUIRE(device.init(manager->context(), config, "test-partial-start", 0, [](const void*, void*, unsigned int) {}));
        int count = live[manager->context()];
        failStart = true;
        REQUIRE(!device.start() && !device.initialized());
        REQUIRE(live[manager->context()] == count - 1);
        failDevice = true;
        REQUIRE(!device.init(manager->context(), config, "test-init-failure", 0, [](const void*, void*, unsigned int) {}));
        REQUIRE(!device.initialized() && live[manager->context()] == count - 1);
    }
    {
        AudioRecorder first, second;
        first.init(); second.init();
        first.configureNoiseSuppression(false, -20); // Первый — прямой проход, второй — NS по умолчанию.
        auto inputs = manager->enumerateCaptureDevices();
        REQUIRE(!inputs.empty());
        first.setCaptureDevice(inputs.front().id);
        int previous = captureStarts;
        failStart = true;
        first.startRecording(); second.startRecording();
        REQUIRE(captureStarts == previous + 3); // Выбранный input не стартовал: default запущен после полного uninit.
        pump(60);
        first.stopRecording(); second.stopRecording();
        attachment_send_req a{}, b{};
        REQUIRE(first.takeRecording(&a) && second.takeRecording(&b));
        REQUIRE(a.pcm_count >= 1200 && b.pcm_count >= 1200);
        REQUIRE(a.pcm[0] == 1000 && b.pcm[0] > -100 && b.pcm[0] < 100);
        REQUIRE(int(a.pcm_count * 1000 / 48000) == first.durationMs());
        REQUIRE(b.pcm_count >= 2160 && int((b.pcm_count - 960) * 1000 / 48000) == second.durationMs());
        a.pcm_release(a.pcm_owner); b.pcm_release(b.pcm_owner);
        second.configureNoiseSuppression(false, -30);
        second.startRecording(); pump(60); second.stopRecording();
        attachment_send_req bypass{}; REQUIRE(second.takeRecording(&bypass));
        REQUIRE(bypass.pcm_count >= 1200 && bypass.pcm[0] == 1000);
        REQUIRE(int(bypass.pcm_count * 1000 / 48000) == second.durationMs());
        bypass.pcm_release(bypass.pcm_owner);
    }
    {
        AudioRecorder meter; meter.init(); meter.startRecording(true); pump(60); meter.stopRecording();
        attachment_send_req request{};
        REQUIRE(meter.durationMs() > 0 && !meter.takeRecording(&request));
        REQUIRE(!request.pcm && !request.pcm_owner && !request.compressor);
    }
    instance.ua = uasync_create(); REQUIRE(instance.ua);
    REQUIRE(radio_init_instance(&instance) == 0);
    REQUIRE(call_audio_init(&instance) == 0 && radio_audio_init(&instance) == 0);
    auto* call = CallAudioEngine::instance(); auto* radio = RadioAudioEngine::instance();
    radio_output_idle(manager, radio);
    REQUIRE(call->start(42) == 0);
    pump(60);
    ma_device* playback = nullptr;
    for (auto* device : devices)
        if (device->type == ma_device_type_playback && device->playback.format == ma_format_f32 && device->playback.channels == 2)
            playback = device;
    REQUIRE(playback);
    int previousCapture = captureStarts, previousPlayback = playbackStarts;
    REQUIRE(ma_device_stop(playback) == MA_SUCCESS);
    pump(800);
    REQUIRE(captureStarts == previousCapture && playbackStarts > previousPlayback);
    REQUIRE(call->callId() == 42 && call->isActive());
    REQUIRE(radio->start(radioGroup) == 0);
    AudioRecorder recorder; recorder.init(); recorder.startRecording();
    std::vector<int16_t> notification(48000 * 10, 1000);
    auto pcmId = manager->playRawPcm(notification.data(), (int)notification.size(), 48000, 1);
    pump(60);
    auto cursor = manager->pcmCursor(pcmId);
    REQUIRE(cursor > 0);
    int previousContext = contextStarts;
    failContext = true; failContexts = 2;
    pump(1800);
    REQUIRE(contextStarts >= previousContext + 3 && manager->context());
    REQUIRE(call->callId() == 42 && radio->groupId() == radioGroup && recorder.isRecording());
    REQUIRE(manager->pcmCursor(pcmId) > cursor && manager->isPcmPlaying(pcmId));
    call->stop(); radio->stop(); recorder.stopRecording();
    int previousStarts = starts;
    pump(600);
    REQUIRE(starts == previousStarts); // Отложенное recovery не воскрешает остановленную сессию.
    REQUIRE(call->start(43) == 0);
    call->beginEnd(43);
    REQUIRE(call->start(43) == 0);
    pump(600);
    REQUIRE(call->isActive() && call->callId() == 43); // Старый end timer не закрывает новое поколение того же call ID.
    call->stop();
    failContext = true; failContexts = 1;
    recorder.startRecording();
    pump(220);
    REQUIRE(!manager->context());
    recorder.stopRecording();
    previousCapture = captureStarts;
    pump(1000);
    REQUIRE(manager->context() && captureStarts == previousCapture);

    // Частота источника и управление сессией независимы от соседнего проигрывателя.
    std::vector<int16_t> pcm24(24000 * 2, 2000), pcm48(48000 * 2, -1000);
    auto player24 = manager->playRawPcm(pcm24.data(), 48000, 24000, 1);
    auto player48 = manager->playRawPcm(pcm48.data(), 96000, 48000, 1);
    REQUIRE(player24 && player48 && player24 != player48);
    pump(150);
    uint64_t cursor24 = manager->pcmCursor(player24), cursor48 = manager->pcmCursor(player48);
    REQUIRE(cursor24 > 1000 && cursor24 < 6000 && cursor48 > cursor24 * 1.8 && cursor48 < cursor24 * 2.2);
    manager->pausePcm(player24);
    cursor24 = manager->pcmCursor(player24);
    pump(80);
    uint64_t pausedAfter = manager->pcmCursor(player24), neighbourAfter = manager->pcmCursor(player48);
    qInfo("PCM pause: paused=%llu->%llu neighbour=%llu->%llu", (unsigned long long)cursor24, (unsigned long long)pausedAfter,
          (unsigned long long)cursor48, (unsigned long long)neighbourAfter);
    REQUIRE(pausedAfter == cursor24 && neighbourAfter > cursor48);
    REQUIRE(manager->seekPcm(player24, 12000));
    REQUIRE(manager->pcmCursor(player24) == 12000 && !manager->isPcmPlaying(player24));
    manager->resumePcm(player24);
    // Пауза синхронизируется с уже начатым чтением графа, сосед продолжает играть.
    for (int i = 0; i < 50; ++i) {
        pump(1);
        manager->pausePcm(player24);
        uint64_t paused = manager->pcmCursor(player24);
        pump(1);
        REQUIRE(manager->pcmCursor(player24) == paused);
        manager->resumePcm(player24);
    }
    manager->stopPcm(player48);
    pump(60);
    REQUIRE(manager->pcmCursor(player24) > 12000 && manager->pcmCursor(player48) == 0);
    manager->stopPcm(player24);

    // Предел TX действует до помещения задачи в uasync; старое поколение не отправляется после stop/start.
    uasync_poll(instance.ua, 0);
    REQUIRE(call_audio_start(99) == 0);
    int16_t silence[960]{};
    for (int i = 0; i < 100; ++i) call_audio_feed_pcm(99, silence, 960, INT64_MIN);
    int pending = 0;
    for (auto* task = instance.ua->posted_tasks_head; task; task = task->next) ++pending;
    REQUIRE(pending == 8);
    call_audio_stop();
    REQUIRE(call_audio_start(100) == 0);
    for (int i = 0; i < 100; ++i) call_audio_feed_pcm(100, silence, 960, INT64_MIN);
    pending = 0;
    for (auto* task = instance.ua->posted_tasks_head; task; task = task->next) ++pending;
    REQUIRE(pending == 8); // Новый звонок также учитывает ещё не освобождённые задачи старого.
    call_audio_stop();
    uasync_poll(instance.ua, 0);
    REQUIRE(!instance.ua->posted_tasks_head);
    REQUIRE(radio_audio_start(8, 1) == 0);
    radio_audio_talk_begin(8); radio_audio_talk_end(8); radio_audio_stop();
    REQUIRE(instance.ua->posted_tasks_head);
    // Завершение ядра освобождает ещё не выполненные PTT-задачи вместе с аргументами.
    call_audio_destroy(&instance); radio_audio_destroy(&instance); radio_destroy_instance(&instance);
    uasync_destroy(instance.ua, 0); instance.ua = nullptr;
    manager->shutdown();
    REQUIRE(devices.empty());
    qInfo("audio recovery: partial start, independent streams, two recorders, context restart, cancellation, bounded TX PASS");
    return 0;
}
