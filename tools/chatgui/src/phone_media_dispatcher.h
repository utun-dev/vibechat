/* GUI-диспетчер телефона: begin запрашивает release, ready подтверждает его фактическое завершение.
 * Отмена не отменяет release. Один владелец по call ID; тайм-аут завершает звонок, но сохраняет запрет медиа.
 * QObject parent должен пережить callbacks. Все операции выполняются в GUI-потоке. */
#pragma once
#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>
#include "audio_capture_lease.h"
#include "debug_config.h"

class PhoneMediaDispatcher : public QObject {
public:
    explicit PhoneMediaDispatcher(QObject* parent, int preparationTimeoutMs = 10000) : QObject(parent), timeoutMs(preparationTimeoutMs) {
        timer.setInterval(50);
        connect(&timer, &QTimer::timeout, this, [this] { advance(); });
    }
    ~PhoneMediaDispatcher() override { timer.stop(); }
    bool blocked() const { return id != 0; }
    void enter(quint64 call, std::function<void()> begin, std::function<bool()> ready,
               std::function<void()> activate, std::function<void()> failed) {
        if (id == call) return;
        if (id) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "media transition busy owner=%llu request=%llu", id, call); failed(); return; }
        id = call; wanted = true; preparing = true; timedOut = false;
        released = std::move(ready); start = std::move(activate); failure = std::move(failed);
        AudioCaptureLease::phoneReserved = true;
        clock.start(); timer.start();
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "media transition begins id=%llu", id);
        begin();
    }
    void finish(quint64 call, std::function<bool()> ready, std::function<void()> restore) {
        if (id != call) return;
        wanted = false; phoneReleased = std::move(ready); restored = std::move(restore); timer.start();
    }
private:
    QTimer timer;
    QElapsedTimer clock;
    quint64 id = 0;
    bool wanted = false, preparing = false, timedOut = false;
    int timeoutMs;
    std::function<bool()> released, phoneReleased;
    std::function<void()> start, failure, restored;
    void advance() {
        if (preparing && !released()) {
            if (!timedOut && clock.elapsed() >= timeoutMs) {
                timedOut = true; wanted = false;
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "media release timeout id=%llu; owners retained", id);
                failure();
            }
            return;
        }
        if (preparing) {
            preparing = false;
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "media released id=%llu elapsed_ms=%lld", id, clock.elapsed());
            if (wanted) { timer.stop(); start(); return; }
        }
        if (!wanted && phoneReleased && phoneReleased()) {
            auto restore = std::move(restored);
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "media mode restored id=%llu", id);
            timer.stop(); id = 0; AudioCaptureLease::phoneReserved = false;
            released = {}; phoneReleased = {}; start = {}; failure = {};
            if (restore) restore();
        }
    }
};
