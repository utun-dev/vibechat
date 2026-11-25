/* Общий desktop-захват PCM s16/48кГц для трансляции и рации. Объект — GUI-подписка, Session владеет AudioDevice;
 * один выбранный вход открывается один раз, в сессии не более одного потребителя каждой Role.
 * Stream переоткрывает прежний radio-профиль под свои параметры; они сохраняются, пока живёт сессия, даже после ухода Stream.
 * Управление и ready/stopped — GUI-поток, Callback — аппаратный поток, где допустимо только быстро скопировать PCM.
 * ready/stopped подготавливают/освобождают потребителя и не должны реентерабельно менять подписки.
 * close/closeAsync отсоединяют свой callback с ожиданием его окончания; оставшийся потребитель сохраняет устройство. */
#pragma once
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>

class AudioCapture {
public:
    enum class Role { Stream, Radio }; // Два отдельных слота одной сессии; приоритет параметров у Stream.
    struct Frame {
        const int16_t* pcm; // Заимствованный interleaved PCM на время callback; возможен nullptr при отсутствии данных.
        unsigned frames; // Число PCM-фреймов на канал, не число отдельных int16_t.
        int channels; // Фактические каналы подписки после fallback, частота всегда 48000Гц.
        int64_t timeUs; // Монотонное время первого физического PCM-фрейма в микросекундах, измеренное либо оценённое.
        uint64_t route; // Идентификатор физического маршрута из диагностики backend.
        bool measured, missing; // Достоверность аппаратного времени и признак отсутствующих данных (не цифровой тишины).
    };
    /* Фактические выходные/native каналы; true разрешает Callback, false оставляет подписку для повтора prepare через recover.
     * Может вызываться до запуска backend; успешная подготовка сама по себе не гарантирует поступление PCM. */
    using Ready = std::function<bool(int channels, int nativeChannels)>;
    /* Заимствованный Frame нужно обработать синхронно; PCM/ссылку на аргумент после callback не хранить. */
    using Callback = std::function<void(const Frame&)>;
    AudioCapture() = default;
    ~AudioCapture(); // close с ожиданием собственного close worker; SoundManager context ещё должен быть жив.
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;
    /* Заменяет прежнюю подписку: input — ID SoundManager, пустой при open выбирает известный default текущего списка.
     * channels — запрос 1/2 с fallback до mono, tag — диагностический ID сессии. Все три callbacks обязательны.
     * ready/stopped запускают/останавливают ресурсы потребителя, stopped следует только после успешного ready.
     * Возвращает running() после первой попытки: false может оставить subscribed() для периодического recover.
     * Дублирующая Role/закрывающийся общий backend не получают подписку; closing() блокирует замену старой. */
    bool open(const QString& input, Role role, int channels, uint64_t tag, Ready ready,
              std::function<void()> stopped, Callback callback);
    void close(); // Join собственного close worker, detach и синхронное закрытие последнего backend; повтор безопасен.
    /* Detach и stopped выполняются сразу в GUI; освобождение последней Session передаётся собственному worker.
     * Пока worker занят, повтор ничего не делает; объект и SoundManager context должны пережить его завершение. */
    void closeAsync();
    bool closing() const; // Собственный close worker либо переоткрытие общего backend для подстройки буфера.
    void recover(); // GUI периодически повторяет открытие/подготовку с учётом backoff и восстановления устройства.
    /* Подписка может существовать без работающей аппаратуры; running требует также ready и запущенное устройство. */
    bool subscribed() const { return bool(m_session); }
    bool running() const;
    bool captureTimedOut() const; // Подготовленная подписка не получила настоящий PCM >=1с; сама её не закрывает.
private:
    struct Session;
    std::shared_ptr<Session> detach(); // Отключает свой слот/колбэк, вызывает stopped, отдаёт владение Session без её закрытия.
    std::shared_ptr<Session> m_session; // Общая Session выбранного входа; статический реестр хранит только weak_ptr.
    std::thread m_closeThread; // Временно владеет отсоединённой Session при closeAsync.
    std::atomic<bool> m_closeDone{true}; // Worker отпустил Session; thread может ещё требовать join.
    Ready m_ready; // GUI-подготовка потребителя при открытии/восстановлении.
    std::function<void()> m_stopped; // GUI-остановка подготовленного потребителя после завершения callback.
    Callback m_callback; // Аппаратная доставка PCM после успешного ready.
    Role m_role = Role::Radio; // Выбранный слот текущей подписки.
    bool m_prepared = false; // ready успешно выполнен и должен быть уравновешен stopped.
};
