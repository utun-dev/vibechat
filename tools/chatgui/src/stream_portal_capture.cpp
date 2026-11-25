#include "stream_portal_capture.h"
#include "debug_config.h"
#include "platform_compat.h"
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusUnixFileDescriptor>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QTimer>
#include <QUuid>
#include <QVideoFrameFormat>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <cstring>
#include <mutex>

namespace {
const QString service = "org.freedesktop.portal.Desktop", path = "/org/freedesktop/portal/desktop";
const QString screenCast = "org.freedesktop.portal.ScreenCast", requestInterface = "org.freedesktop.portal.Request";
const QString sessionInterface = "org.freedesktop.portal.Session";
QString token() { return "utun_screen_" + QUuid::createUuid().toString(QUuid::Id128); }
}

struct StreamPortalCapture::Impl {
    enum State { Idle, Checking, Creating, Selecting, Starting, Opening, Streaming, Failed } state = Idle;
    QString session, request;
    uint cursorMode = 2;
    QTimer timer;
    pw_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook coreHook{}, streamHook{};
    std::unique_ptr<QSocketNotifier> notifier;
    QVideoFrameFormat format;
    int bytesPerPixel = 4;
    uint64_t frames = 0;
};

StreamPortalCapture::StreamPortalCapture(QObject* parent) : QObject(parent), m(std::make_unique<Impl>()) {
    m->timer.setSingleShot(true);
    connect(&m->timer, &QTimer::timeout, this, [this] { fail(QString("Timed out at capture stage %1").arg(m->state)); });
    auto* watcher = new QDBusServiceWatcher(service, QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForUnregistration, this);
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] { fail("ScreenCast portal disappeared"); });
}
StreamPortalCapture::~StreamPortalCapture() { close(); }

void StreamPortalCapture::start(bool showCursor) {
    if (m->state != Impl::Idle) { fail("Capture has already been started"); return; }
    m->state = Impl::Checking; m->cursorMode = showCursor ? 2 : 1; m->timer.start(30000);
    auto message = QDBusMessage::createMethodCall(service, path, "org.freedesktop.DBus.Properties", "GetAll");
    message.setArguments({screenCast});
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        QDBusPendingReply<QVariantMap> reply = *watcher; watcher->deleteLater();
        if (m->state != Impl::Checking) return;
        if (reply.isError()) { fail(reply.error().message()); return; }
        const auto properties = reply.value();
        uint modes = properties.value("AvailableCursorModes").toUInt();
        if (!(properties.value("AvailableSourceTypes").toUInt() & 1) || !(modes & m->cursorMode)) {
            fail(QString("Portal does not support monitor capture with cursor mode %1 (available=%2)").arg(m->cursorMode).arg(modes)); return;
        }
        auto bus = QDBusConnection::sessionBus();
        QString sessionToken = token();
        m->session = path + "/session/" + bus.baseService().mid(1).replace('.', '_') + "/" + sessionToken;
        if (!bus.connect(service, m->session, sessionInterface, "Closed", this, SLOT(sessionClosed()))) {
            fail("Cannot subscribe to ScreenCast session closure"); return;
        }
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "Wayland capture portal ready version=%u cursor_mode=%u available=%u",
            properties.value("version").toUInt(), m->cursorMode, modes);
        m->state = Impl::Creating; request("CreateSession", {}, {{"session_handle_token", sessionToken}});
    });
}

/* Подписка предшествует вызову: Response может прийти раньше ответа метода D-Bus. */
void StreamPortalCapture::request(const QString& method, QVariantList args, QVariantMap options) {
    auto bus = QDBusConnection::sessionBus();
    QString requestToken = token();
    m->request = path + "/request/" + bus.baseService().mid(1).replace('.', '_') + "/" + requestToken;
    if (!bus.connect(service, m->request, requestInterface, "Response", this, SLOT(response(uint,QVariantMap,QDBusMessage)))) {
        fail("Cannot subscribe to ScreenCast response"); return;
    }
    options["handle_token"] = requestToken; args << options;
    auto message = QDBusMessage::createMethodCall(service, path, screenCast, method); message.setArguments(args);
    if (m->state == Impl::Starting) m->timer.stop(); else m->timer.start(30000); // Выбор пользователя не ограничен таймаутом.
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "Wayland capture request=%s path=%s", qPrintable(method), qPrintable(m->request));
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, expected = m->request] {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher; watcher->deleteLater();
        if (m->request != expected || m->state == Impl::Failed) return;
        if (reply.isError()) { fail(reply.error().message()); return; }
        if (reply.value().path() != expected) fail("Unexpected ScreenCast request path");
    });
}

void StreamPortalCapture::response(uint code, const QVariantMap& results, const QDBusMessage& message) {
    if (m->state == Impl::Failed || m->request.isEmpty() || message.path() != m->request) return;
    auto bus = QDBusConnection::sessionBus();
    bus.disconnect(service, m->request, requestInterface, "Response", this, SLOT(response(uint,QVariantMap,QDBusMessage)));
    m->request.clear(); m->timer.stop();
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "Wayland capture response stage=%d code=%u", m->state, code);
    if (code) { fail(code == 1 ? "Screen selection cancelled" : QString("ScreenCast request failed: %1").arg(code)); return; }
    if (m->state == Impl::Creating) {
        if (results.value("session_handle").toString() != m->session) { fail("Unexpected ScreenCast session path"); return; }
        m->state = Impl::Selecting;
        request("SelectSources", {QVariant::fromValue(QDBusObjectPath(m->session))},
            {{"types", uint(1)}, {"multiple", false}, {"cursor_mode", m->cursorMode}});
    } else if (m->state == Impl::Selecting) {
        m->state = Impl::Starting;
        request("Start", {QVariant::fromValue(QDBusObjectPath(m->session)), QString()}, {});
    } else if (m->state == Impl::Starting) {
        const auto streams = results.value("streams").value<QDBusArgument>();
        if (streams.currentSignature() != "a(ua{sv})") { fail("Invalid ScreenCast streams signature"); return; }
        streams.beginArray();
        if (streams.atEnd()) { fail("ScreenCast returned no monitor"); return; }
        uint node = 0; QVariantMap properties;
        streams.beginStructure(); streams >> node >> properties; streams.endStructure();
        bool extra = !streams.atEnd(); streams.endArray();
        if (!node || extra) { fail("ScreenCast must return exactly one monitor"); return; }
        openRemote(node, properties.value("pipewire-serial").toString());
    } else fail("Unexpected ScreenCast response");
}

void StreamPortalCapture::openRemote(uint node, const QString& serial) {
    m->state = Impl::Opening; m->timer.start(30000);
    auto message = QDBusMessage::createMethodCall(service, path, screenCast, "OpenPipeWireRemote");
    message.setArguments({QVariant::fromValue(QDBusObjectPath(m->session)), QVariantMap{}});
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, node, serial] {
        QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher; watcher->deleteLater();
        if (m->state != Impl::Opening) return;
        if (reply.isError()) { fail(reply.error().message()); return; }
        openPipeWire(reply.value().fileDescriptor(), node, serial);
    });
}

void StreamPortalCapture::openPipeWire(int fd, uint node, const QString& serial) {
    static std::once_flag initialized; std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
    m->loop = pw_loop_new(nullptr);
    if (!m->loop) { fail("Cannot create PipeWire loop"); return; }
    pw_loop_enter(m->loop);
    m->context = pw_context_new(m->loop, nullptr, 0);
    if (!m->context) { fail("Cannot create PipeWire context"); return; }
    int ownedFd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (ownedFd < 0) { fail(QString("Cannot duplicate PipeWire fd: %1").arg(strerror(errno))); return; }
    m->core = pw_context_connect_fd(m->context, ownedFd, nullptr, 0); // PipeWire закрывает fd также при ошибке.
    if (!m->core) { fail(QString("Cannot connect PipeWire: %1").arg(strerror(errno))); return; }
    static const pw_core_events coreEvents = { .version = PW_VERSION_CORE_EVENTS,
        .error = [](void* data, uint32_t id, int, int result, const char* error) {
            static_cast<StreamPortalCapture*>(data)->fail(QString("PipeWire object=%1 error=%2: %3").arg(id).arg(result).arg(error));
        }
    };
    pw_core_add_listener(m->core, &m->coreHook, &coreEvents, this);
    auto* props = pw_properties_new(PW_KEY_NODE_NAME, "vibechat-screen", PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
    if (!props) { fail("Cannot allocate PipeWire stream properties"); return; }
    if (!serial.isEmpty()) pw_properties_set(props, PW_KEY_TARGET_OBJECT, serial.toUtf8().constData());
    m->stream = pw_stream_new(m->core, "vibechat-screen", props);
    if (!m->stream) { fail("Cannot create PipeWire stream"); return; }
    static const pw_stream_events events = { .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = [](void* data, pw_stream_state, pw_stream_state state, const char* error) {
            auto* self = static_cast<StreamPortalCapture*>(data);
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "Wayland PipeWire state=%s error=%s", pw_stream_state_as_string(state), error ? error : "");
            if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
                self->fail(error ? QString::fromUtf8(error) : QStringLiteral("PipeWire stream disconnected"));
        },
        .param_changed = [](void* data, uint32_t id, const spa_pod* param) {
            if (id != SPA_PARAM_Format || !param) return;
            auto* self = static_cast<StreamPortalCapture*>(data); spa_video_info_raw info{};
            if (spa_format_video_raw_parse(param, &info) < 0) { self->fail("Invalid PipeWire video format"); return; }
            QVideoFrameFormat::PixelFormat pixel = QVideoFrameFormat::Format_Invalid;
            self->m->bytesPerPixel = 4;
            switch (info.format) {
            case SPA_VIDEO_FORMAT_BGR: pixel = QVideoFrameFormat::Format_BGRX8888; self->m->bytesPerPixel = 3; break;
            case SPA_VIDEO_FORMAT_RGB: pixel = QVideoFrameFormat::Format_RGBX8888; self->m->bytesPerPixel = 3; break;
            case SPA_VIDEO_FORMAT_BGRx: pixel = QVideoFrameFormat::Format_BGRX8888; break;
            case SPA_VIDEO_FORMAT_BGRA: pixel = QVideoFrameFormat::Format_BGRA8888; break;
            case SPA_VIDEO_FORMAT_RGBx: pixel = QVideoFrameFormat::Format_RGBX8888; break;
            case SPA_VIDEO_FORMAT_RGBA: pixel = QVideoFrameFormat::Format_RGBA8888; break;
            default: self->fail(QString("Unsupported PipeWire pixel format %1").arg(info.format)); return;
            }
            if (!info.size.width || !info.size.height || info.size.width > 16384 || info.size.height > 16384) {
                self->fail(QString("Invalid PipeWire frame size %1x%2").arg(info.size.width).arg(info.size.height)); return;
            }
            self->m->format = QVideoFrameFormat(QSize(info.size.width, info.size.height), pixel);
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "Wayland PipeWire format=%u size=%ux%u fps=%u/%u cursor=%u",
                info.format, info.size.width, info.size.height, info.framerate.num, info.framerate.denom, self->m->cursorMode);
            uint8_t buffer[512]; spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            const spa_pod* params[] = {
                static_cast<spa_pod*>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                    SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)))),
                static_cast<spa_pod*>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                    SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header))))
            };
            int result = pw_stream_update_params(self->m->stream, params, 2);
            if (result < 0) self->fail(QString("PipeWire buffer negotiation failed: %1").arg(result));
        },
        .process = [](void* data) { static_cast<StreamPortalCapture*>(data)->processFrame(); }
    };
    pw_stream_add_listener(m->stream, &m->streamHook, &events, this);
    uint8_t buffer[512]; spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_rectangle size{1920, 1080}, minimum{1, 1}, maximum{16384, 16384};
    const spa_pod* format = static_cast<spa_pod*>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(7, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGB, SPA_VIDEO_FORMAT_BGR),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&size, &minimum, &maximum)));
    int result = pw_stream_connect(m->stream, PW_DIRECTION_INPUT, serial.isEmpty() ? node : PW_ID_ANY,
        pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), &format, 1);
    if (result < 0) { fail(QString("Cannot connect screen stream: %1").arg(result)); return; }
    m->notifier = std::make_unique<QSocketNotifier>(pw_loop_get_fd(m->loop), QSocketNotifier::Read);
    connect(m->notifier.get(), &QSocketNotifier::activated, this, [this] {
        if (m->state == Impl::Failed) return;
        int result = pw_loop_iterate(m->loop, 0);
        if (result < 0 && result != -EINTR) fail(QString("PipeWire dispatch failed: %1").arg(result));
    });
    m->state = Impl::Streaming; m->timer.start(8000);
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "Wayland capture opened node=%u serial=%s cursor_mode=%u", node, qPrintable(serial), m->cursorMode);
}

/* Копируем только последний кадр; PipeWire немедленно получает обратно каждый взятый буфер. */
void StreamPortalCapture::processFrame() {
    if (m->state == Impl::Failed) return;
    pw_buffer* buffer = nullptr;
    while (auto* next = pw_stream_dequeue_buffer(m->stream)) {
        if (buffer) pw_stream_queue_buffer(m->stream, buffer);
        buffer = next;
    }
    if (!buffer) return;
    auto recycle = qScopeGuard([&] { pw_stream_queue_buffer(m->stream, buffer); });
    auto* raw = buffer->buffer;
    if (raw->n_datas != 1 || !raw->datas[0].data || !raw->datas[0].chunk) { fail("PipeWire frame is not mapped packed RGB"); return; }
    const auto& data = raw->datas[0]; const auto& chunk = *data.chunk;
    if (!chunk.size) return; // Статичный экран может прислать пустой буфер.
    if (chunk.flags & SPA_CHUNK_FLAG_CORRUPTED) { DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "Wayland capture discarded corrupted frame"); return; }
    int width = m->format.frameWidth(), height = m->format.frameHeight();
    int rowBytes = width * m->bytesPerPixel;
    uint64_t stride = chunk.stride > 0 ? chunk.stride : rowBytes;
    uint64_t required = height > 0 ? (height - 1) * stride + rowBytes : 0;
    if (width <= 0 || height <= 0 || chunk.stride < 0 || stride < uint64_t(rowBytes) || chunk.offset > data.maxsize
        || required > data.maxsize - chunk.offset || required > chunk.size) {
        fail(QString("Invalid PipeWire buffer size=%1x%2 stride=%3 offset=%4 bytes=%5 capacity=%6")
            .arg(width).arg(height).arg(chunk.stride).arg(chunk.offset).arg(chunk.size).arg(data.maxsize)); return;
    }
    QVideoFrame frame(m->format);
    if (!frame.map(QVideoFrame::WriteOnly)) { fail("Cannot allocate captured video frame"); return; }
    for (int y = 0; y < height; y++) {
        auto* target = frame.bits(0) + y * frame.bytesPerLine(0);
        const auto* source = static_cast<const uint8_t*>(data.data) + chunk.offset + y * stride;
        if (m->bytesPerPixel == 4) std::memcpy(target, source, rowBytes);
        else for (int x = 0; x < width; x++) { std::memcpy(target + x * 4, source + x * 3, 3); target[x * 4 + 3] = 255; }
    }
    frame.unmap();
    auto* header = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(raw, SPA_META_Header, sizeof(spa_meta_header)));
    if (header && header->pts >= 0) frame.setStartTime(header->pts / 1000);
    if (!m->frames++) {
        m->timer.stop();
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "Wayland capture first frame size=%dx%d stride=%llu PTS=%lld cursor_mode=%u",
            width, height, (unsigned long long)stride, (long long)frame.startTime(), m->cursorMode);
    }
    emit frameReady(frame);
}

void StreamPortalCapture::sessionClosed() { fail("Screen sharing stopped by the system"); }

void StreamPortalCapture::fail(const QString& reason) {
    if (m->state == Impl::Failed) return;
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "Wayland capture failed stage=%d reason=%s", m->state, qPrintable(reason));
    m->state = Impl::Failed; m->timer.stop();
    // Уничтожение pw_stream запрещено внутри его process/state callbacks.
    QTimer::singleShot(0, this, [this, reason] {
        close(); emit errorOccurred(QCoreApplication::translate("ChatGui", "Screen capture failed: %1").arg(reason));
    });
}

void StreamPortalCapture::close() {
    m->state = Impl::Failed; m->timer.stop(); m->notifier.reset();
    if (m->stream) { spa_hook_remove(&m->streamHook); pw_stream_destroy(m->stream); m->stream = nullptr; }
    if (m->core) { spa_hook_remove(&m->coreHook); pw_core_disconnect(m->core); m->core = nullptr; }
    if (m->context) { pw_context_destroy(m->context); m->context = nullptr; }
    if (m->loop) { pw_loop_leave(m->loop); pw_loop_destroy(m->loop); m->loop = nullptr; }
    auto bus = QDBusConnection::sessionBus();
    if (!m->request.isEmpty()) {
        bus.disconnect(service, m->request, requestInterface, "Response", this, SLOT(response(uint,QVariantMap,QDBusMessage)));
        auto message = QDBusMessage::createMethodCall(service, m->request, requestInterface, "Close");
        message.setAutoStartService(false); bus.asyncCall(message); m->request.clear();
    }
    if (!m->session.isEmpty()) {
        bus.disconnect(service, m->session, sessionInterface, "Closed", this, SLOT(sessionClosed()));
        auto message = QDBusMessage::createMethodCall(service, m->session, sessionInterface, "Close");
        message.setAutoStartService(false); bus.asyncCall(message);
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "Wayland capture closed session=%s frames=%llu", qPrintable(m->session), (unsigned long long)m->frames);
        m->session.clear();
    }
}
