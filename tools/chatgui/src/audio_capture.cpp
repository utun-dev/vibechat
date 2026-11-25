#include "audio_capture.h"
#include "audio_device.h"
#include "sound_manager.h"
#include "debug_config.h"
#include <array>
#include <atomic>
#include <thread>
#include <vector>
#ifdef _WIN32
#include "platform_compat.h"
#include <objbase.h>
#endif

struct AudioCapture::Session {
    struct Slot {
        AudioCapture* client = nullptr; // Только GUI.
        std::atomic<AudioCapture*> active{nullptr};
        std::atomic<unsigned> readers{0};
    };
    AudioDevice device;
    QString key;
    std::array<Slot, 2> consumers;
    bool streamProfile = false, retiring = false;
    int requestedChannels = 1, channels = 1;
    uint64_t tag = 0;
    QElapsedTimer clock;
    qint64 prepareAt = 0;

    explicit Session(QString input) : key(std::move(input)) { clock.start(); }
    ~Session() { stop(); }

    void detach(Slot& slot) {
        slot.active.store(nullptr);
        // seq_cst: callback либо увидит nullptr, либо завершится до освобождения клиента.
        while (slot.readers.load()) std::this_thread::yield();
        if (slot.client->m_prepared) {
            slot.client->m_prepared = false;
            slot.client->m_stopped();
        }
    }
    void stop() {
        device.close(); // join до stopped: workers больше не получат PCM старого формата.
        for (auto& slot : consumers) if (slot.client) detach(slot);
    }
    void prepare() {
        if (clock.elapsed() < prepareAt) return;
        prepareAt = clock.elapsed() + 250;
        for (auto& slot : consumers) {
            auto* client = slot.client;
            if (!client || client->m_prepared) continue;
            if (!client->m_ready(channels, device.device()->capture.internalChannels)) {
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "capture: consumer start failed role=%s channels=%d",
                            client->m_role == Role::Stream ? "stream" : "radio", channels);
                continue;
            }
            client->m_prepared = true;
            slot.active.store(client);
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "capture: attached role=%s profile=%s channels=%d rate=48000",
                       client->m_role == Role::Stream ? "stream" : "radio", streamProfile ? "stream" : "radio", channels);
        }
    }
    void recover() {
        if (device.needsRecovery()) {
            if (device.resizingBuffer()) {
                for (auto& slot : consumers) if (slot.client) detach(slot);
                device.closeAsync();
            } else stop();
            device.retryLater();
        }
        if (device.initialized()) { prepare(); return; }
        auto* context = SoundManager::instance()->context();
        if (!context || !device.retryReady()) return;
        ma_device_id id;
        bool selected = SoundManager::deviceId(key, &id);
        if (!key.isEmpty() && !selected) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "capture: selected input unavailable; retaining selection");
            device.retryLater(); return;
        }
        for (int count = requestedChannels; count >= 1; --count) {
            auto config = ma_device_config_init(ma_device_type_capture);
            config.capture.format = ma_format_s16; config.capture.channels = count; config.sampleRate = 48000;
            config.capture.pDeviceID = selected ? &id : nullptr;
            if (!streamProfile) AudioDevice::requestLowLatency(context, config);
            auto callback = [this](const void* in, void*, unsigned frames) {
                Frame frame{static_cast<const int16_t*>(in), frames, channels, device.callbackTimeUs(),
                            device.route(), device.timingMeasured(), device.captureMissing()};
                // Трансляция получает исходный PCM первой; оба callback только копируют данные в свои очереди.
                for (auto& slot : consumers) {
                    slot.readers.fetch_add(1);
                    if (auto* client = slot.active.load()) client->m_callback(frame);
                    slot.readers.fetch_sub(1);
                }
            };
            if (!device.init(context, config, streamProfile ? "stream-capture" : "radio-capture", tag, callback)) continue;
            channels = count; prepareAt = 0; prepare();
            if (device.start()) {
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "capture: opened profile=%s channels=%d requested=%d input=%s",
                           streamProfile ? "stream" : "radio", channels, requestedChannels, qPrintable(key));
                return;
            }
            stop();
        }
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "capture: cannot open selected input profile=%s", streamProfile ? "stream" : "radio");
    }
};

AudioCapture::~AudioCapture() { close(); }

bool AudioCapture::open(const QString& input, Role role, int channels, uint64_t tag, Ready ready,
                        std::function<void()> stopped, Callback callback) {
    if (closing()) { DEBUG_WARN(DEBUG_CATEGORY_CALL, "capture: previous device is still closing"); return false; }
    close();
    static std::vector<std::weak_ptr<Session>> sessions; // Только GUI; владение у подписчиков.
    QString key = input;
    if (key.isEmpty()) for (const auto& item : SoundManager::enumerateCaptureDevices()) {
        if (item.isDefault) { key = item.id; break; }
    }
    for (auto it = sessions.begin(); it != sessions.end();) {
        auto session = it->lock();
        if (!session) { it = sessions.erase(it); continue; }
        if (!session->retiring && session->key == key) m_session = session;
        ++it;
    }
    if (!m_session) { m_session = std::make_shared<Session>(key); sessions.push_back(m_session); }
    auto& session = *m_session;
    if (session.device.closing()) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "capture: shared input is resizing its buffer; new subscription deferred");
        m_session.reset(); return false;
    }
    auto& slot = session.consumers[role == Role::Stream ? 0 : 1];
    if (slot.client) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "capture: duplicate consumer role=%s", role == Role::Stream ? "stream" : "radio");
        m_session.reset(); return false;
    }
    bool takeover = role == Role::Stream && !session.streamProfile;
    if (takeover) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "capture: stream takes ownership of capture profile");
        session.stop(); session.streamProfile = true;
    }
    m_role = role; m_ready = std::move(ready); m_stopped = std::move(stopped); m_callback = std::move(callback);
    slot.client = this;
    if (role == Role::Stream || !session.streamProfile) { session.requestedChannels = channels; session.tag = tag; }
    session.prepareAt = 0; session.device.retryNow(); session.recover();
    return running();
}

std::shared_ptr<AudioCapture::Session> AudioCapture::detach() {
    if (!m_session) return {};
    auto& slot = m_session->consumers[m_role == Role::Stream ? 0 : 1];
    m_session->detach(slot); slot.client = nullptr;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "capture: detached role=%s remaining=%d", m_role == Role::Stream ? "stream" : "radio",
               int(m_session->consumers[0].client != nullptr) + int(m_session->consumers[1].client != nullptr));
    m_callback = {}; m_ready = {}; m_stopped = {};
    return std::move(m_session);
}

void AudioCapture::close() {
    if (m_closeThread.joinable()) m_closeThread.join();
    detach().reset();
}

/* Callback отсоединён в GUI; session и контекст должны жить до завершения ma_device_uninit. */
void AudioCapture::closeAsync() {
    if (!m_closeDone.load(std::memory_order_acquire)) return;
    if (m_closeThread.joinable()) m_closeThread.join();
    auto session = detach();
    if (!session) return;
    if (session.use_count() == 1) session->retiring = true;
    m_closeDone.store(false, std::memory_order_release);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "capture: asynchronous close started shared=%d", session.use_count() > 1);
    try {
        m_closeThread = std::thread([this, session = std::move(session)]() mutable {
#ifdef _WIN32
            HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(com)) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "capture: close worker COM init failed HRESULT=0x%08lx", (unsigned long)com);
#endif
            session.reset();
#ifdef _WIN32
            if (SUCCEEDED(com)) CoUninitialize();
#endif
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "capture: asynchronous close completed");
            m_closeDone.store(true, std::memory_order_release);
        });
    } catch (const std::system_error& error) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "capture: close worker failed: %s", error.what());
        m_closeDone.store(true, std::memory_order_release);
    }
}

void AudioCapture::recover() { if (m_session) m_session->recover(); }
bool AudioCapture::closing() const {
    return !m_closeDone.load(std::memory_order_acquire) || (m_session && m_session->device.closing());
}
bool AudioCapture::running() const {
    return m_session && m_prepared && m_session->device.initialized() && ma_device_is_started(m_session->device.device());
}
bool AudioCapture::captureTimedOut() const {
    return m_session && m_prepared && m_session->device.captureTimedOut();
}
