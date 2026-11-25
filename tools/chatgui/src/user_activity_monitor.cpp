#include "user_activity_monitor.h"
#include "debug_ui.h"

#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <cmath>
#include <limits>

#ifdef UTUN_ACTIVITY_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif
#ifdef UTUN_ACTIVITY_KIDLETIME
#include <KIdleTime>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#elif defined(Q_OS_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#elif defined(UTUN_ACTIVITY_X11)
#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#undef KeyPress
#undef KeyRelease
#endif

UserActivityMonitor::UserActivityMonitor(QObject* parent) : QObject(parent) {
    m_pollTimer.setInterval(250);
    connect(&m_pollTimer, &QTimer::timeout, this, &UserActivityMonitor::pollIdleTime);
}

UserActivityMonitor::~UserActivityMonitor() {
    stop();
#ifdef UTUN_ACTIVITY_X11
    if (m_display) XCloseDisplay(static_cast<Display*>(m_display));
#endif
    GUI_DEBUG("activity: monitor destroyed");
}

void UserActivityMonitor::start() {
    stop();
    m_watching = true;
    m_elapsed.start();
    qApp->installEventFilter(this);
    const QString platform = QGuiApplication::platformName();
    const bool wayland = platform.startsWith("wayland") || qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland";
#ifdef UTUN_ACTIVITY_DBUS
    if (wayland && !m_gnomeUnavailable) {
        m_useGnome = true;
        m_pollTimer.start();
        pollIdleTime();
        return;
    }
#endif
    if (wayland) { startWaylandFallback(); return; }
    if (systemIdleTime() >= 0) {
        GUI_DEBUG("activity: watching system input platform=%s poll_ms=%d", qPrintable(platform), m_pollTimer.interval());
        m_pollTimer.start();
    } else useLocalActivity();
}

void UserActivityMonitor::stop() {
    ++m_generation;
    m_pollTimer.stop();
    if (m_watching && qApp) qApp->removeEventFilter(this);
#ifdef UTUN_ACTIVITY_KIDLETIME
    if (m_useKIdleTime) KIdleTime::instance()->stopCatchingResumeEvent();
    disconnect(m_kIdleConnection);
#endif
    m_watching = false;
    m_dbusPending = false;
    m_useGnome = false;
    m_useKIdleTime = false;
}

bool UserActivityMonitor::eventFilter(QObject*, QEvent* event) {
    if (!m_watching || !event->spontaneous()) return false;
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TabletMove:
    case QEvent::TabletPress:
        GUI_DEBUG("activity: application input event=%d elapsed_ms=%lld", int(event->type()), (long long)m_elapsed.elapsed());
        stop();
        emit activity();
        break;
    default:
        break;
    }
    return false;
}

void UserActivityMonitor::checkIdleTime(qint64 idleMs, qint64 elapsedMs) {
    if (!m_watching || idleMs < 0 || idleMs >= elapsedMs) return;
    GUI_DEBUG("activity: system input idle_ms=%lld elapsed_ms=%lld", (long long)idleMs, (long long)elapsedMs);
    stop();
    emit activity();
}

void UserActivityMonitor::pollIdleTime() {
#ifdef UTUN_ACTIVITY_DBUS
    if (m_useGnome) {
        if (m_dbusPending) return;
        auto message = QDBusMessage::createMethodCall("org.gnome.Mutter.IdleMonitor", "/org/gnome/Mutter/IdleMonitor/Core",
                                                     "org.gnome.Mutter.IdleMonitor", "GetIdletime");
        message.setAutoStartService(false);
        auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 1000), this);
        const quint64 generation = m_generation;
        const qint64 elapsedMs = m_elapsed.elapsed();
        m_dbusPending = true;
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation, elapsedMs] {
            QDBusPendingReply<qulonglong> reply = *watcher;
            watcher->deleteLater();
            if (!m_watching || generation != m_generation) {
                GUI_DEBUG("activity: ignored obsolete GNOME reply generation=%llu current=%llu watching=%d",
                          (unsigned long long)generation, (unsigned long long)m_generation, int(m_watching));
                return;
            }
            m_dbusPending = false;
            if (reply.isError()) {
                GUI_WARN("activity: GNOME idle monitor unavailable: %s", qPrintable(reply.error().message()));
                m_gnomeUnavailable = true;
                m_useGnome = false;
                m_pollTimer.stop();
                startWaylandFallback();
                return;
            }
            if (!m_gnomeReadyLogged) {
                GUI_DEBUG("activity: GNOME system idle monitor ready");
                m_gnomeReadyLogged = true;
            }
            if (reply.value() <= qulonglong(std::numeric_limits<qint64>::max())) checkIdleTime(qint64(reply.value()), elapsedMs);
            else GUI_WARN("activity: GNOME idle time out of range: %llu", (unsigned long long)reply.value());
        });
        return;
    }
#endif
    const qint64 elapsedMs = m_elapsed.elapsed();
    const qint64 idleMs = systemIdleTime();
    if (idleMs < 0) { m_pollTimer.stop(); useLocalActivity(); return; }
    checkIdleTime(idleMs, elapsedMs);
}

void UserActivityMonitor::startWaylandFallback() {
#ifdef UTUN_ACTIVITY_KIDLETIME
    if (QGuiApplication::platformName().startsWith("wayland")) {
        auto* idle = KIdleTime::instance();
        const int timeout = idle->addIdleTimeout(0);
        if (timeout) {
            idle->removeIdleTimeout(timeout);
            m_kIdleConnection = connect(idle, &KIdleTime::resumingFromIdle, this, [this] {
                if (!m_watching || !m_useKIdleTime) return;
                GUI_DEBUG("activity: Wayland compositor reported user activity");
                stop();
                emit activity();
            });
            m_useKIdleTime = true;
            idle->catchNextResumeEvent();
            GUI_DEBUG("activity: watching Wayland resume through KIdleTime (subject to compositor idle inhibitors)");
            return;
        }
        GUI_WARN("activity: KIdleTime has no available compositor backend");
    }
#endif
    useLocalActivity();
}

void UserActivityMonitor::useLocalActivity() {
    if (!m_localWarningLogged) {
        GUI_WARN("activity: global input unavailable platform=%s session=%s; using ChatGUI input and manual dismissal",
                 qPrintable(QGuiApplication::platformName()), qPrintable(qEnvironmentVariable("XDG_SESSION_TYPE")));
        m_localWarningLogged = true;
    }
}

qint64 UserActivityMonitor::systemIdleTime() {
    const QString platform = QGuiApplication::platformName();
    if (platform == "offscreen" || platform == "minimal") return -1;
#ifdef Q_OS_WIN
    LASTINPUTINFO info = {sizeof(info), 0};
    if (!GetLastInputInfo(&info)) { GUI_WARN("activity: GetLastInputInfo failed error=%lu", GetLastError()); return -1; }
    return DWORD(GetTickCount() - info.dwTime);
#elif defined(Q_OS_MACOS)
    const double seconds = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
    if (!std::isfinite(seconds) || seconds < 0 || seconds > double(std::numeric_limits<qint64>::max() / 1000)) {
        GUI_WARN("activity: invalid macOS idle time seconds=%g", seconds);
        return -1;
    }
    return qint64(seconds * 1000);
#elif defined(UTUN_ACTIVITY_X11)
    if (QGuiApplication::platformName() != "xcb") return -1;
    if (!m_x11Checked) {
        m_x11Checked = true;
        auto* display = XOpenDisplay(nullptr);
        int eventBase = 0, errorBase = 0;
        if (!display) { GUI_WARN("activity: cannot open X11 display"); return -1; }
        if (!XScreenSaverQueryExtension(display, &eventBase, &errorBase)) {
            GUI_WARN("activity: X11 screen saver extension unavailable");
            XCloseDisplay(display);
            return -1;
        }
        m_display = display;
    }
    if (!m_display) return -1;
    auto* display = static_cast<Display*>(m_display);
    XScreenSaverInfo info{};
    if (!XScreenSaverQueryInfo(display, DefaultRootWindow(display), &info)) {
        GUI_WARN("activity: XScreenSaverQueryInfo failed");
        return -1;
    }
    return qint64(info.idle);
#else
    return -1;
#endif
}
