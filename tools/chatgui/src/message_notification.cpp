#include "message_notification.h"
#include "user_activity_monitor.h"
#include "debug_ui.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QHideEvent>
#include <QCloseEvent>

MessageNotification::MessageNotification()
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus)
    , m_activity(new UserActivityMonitor(this))
{
    setObjectName("messageNotification");
    // X11/XWayland: отдельное overlay-окно не принадлежит рабочему столу главного окна и не забирает фокус.
    if (QGuiApplication::platformName() == "xcb") setWindowFlag(Qt::X11BypassWindowManagerHint);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_X11NetWmWindowTypeNotification);
#ifdef Q_OS_MACOS
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
#endif
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setStyleSheet("#messageNotification { background: palette(window); border: 1px solid palette(mid); border-radius: 8px; }"
                  "#messageNotification QLabel { color: palette(window-text); border: none; }"
                  "#messageNotification QPushButton { color: palette(window-text); background: transparent; border: none; padding: 4px; }"
                  "#messageNotification QPushButton:hover { background: palette(light); }");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 10, 10, 14);
    auto* heading = new QHBoxLayout();
    auto* title = new QLabel(QCoreApplication::translate("ChatGui", "Incoming message"), this);
    title->setAttribute(Qt::WA_TransparentForMouseEvents);
    heading->addWidget(title, 1);
    auto* closeButton = new QPushButton(QStringLiteral("✕"), this);
    closeButton->setObjectName("notificationCloseButton");
    closeButton->setToolTip(QCoreApplication::translate("ChatGui", "Close"));
    closeButton->setFocusPolicy(Qt::NoFocus);
    heading->addWidget(closeButton);
    layout->addLayout(heading);
    m_body = new QLabel(this);
    m_body->setTextFormat(Qt::PlainText);
    m_body->setWordWrap(true);
    m_body->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(m_body);
    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(10000);
    m_hideTimer.setTimerType(Qt::PreciseTimer);
    connect(closeButton, &QPushButton::clicked, this, [this] { dismiss("close-button"); });
    connect(&m_hideTimer, &QTimer::timeout, this, [this] { dismiss("activity-timeout"); });
    connect(m_activity, &UserActivityMonitor::activity, this, [this] {
        if (!isVisible() || m_hideTimer.isActive()) return;
        GUI_DEBUG("notification: first activity target=%s hide_in_ms=%d", qPrintable(m_target), m_hideTimer.interval());
        m_hideTimer.start();
    });
    GUI_DEBUG("notification: initialized platform=%s hide_after_activity_ms=%d all_desktop_overlay=%d",
              qPrintable(QGuiApplication::platformName()), m_hideTimer.interval(), int(QGuiApplication::platformName() == "xcb"));
    if (QGuiApplication::platformName().startsWith("wayland"))
        GUI_WARN("notification: native Wayland controls desktop, placement and stacking; remapping on current desktop is best effort");
}

MessageNotification::~MessageNotification() {
    m_activity->stop();
    GUI_DEBUG("notification: destroyed target=%s visible=%d timer_active=%d",
              qPrintable(m_target), int(isVisible()), int(m_hideTimer.isActive()));
}

void MessageNotification::showMessage(const QString& target, const QString& sender, const QString& channel,
                                     const QRect& trayGeometry, QScreen* preferredScreen) {
    QScreen* screen = trayGeometry.isEmpty() ? nullptr : QGuiApplication::screenAt(trayGeometry.center());
    if (!screen) screen = preferredScreen;
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen || screen->availableGeometry().isEmpty()) {
        GUI_ERROR("notification: no available screen target=%s", qPrintable(target));
        dismiss("screen-unavailable");
        return;
    }
    const QRect area = screen->availableGeometry();
    const bool replacing = isVisible();
    if (QGuiApplication::platformName().startsWith("wayland") && internalWinId()) {
        hide();
        destroy();
        GUI_DEBUG("notification: recreated Wayland surface for current desktop target=%s", qPrintable(target));
    }
    m_hideTimer.stop();
    m_activity->stop();
    m_target = target;
    m_body->setText(QCoreApplication::translate("ChatGui", "New message from %1").arg(sender) + '\n' + channel);
    setFixedWidth(qMin(360, qMax(1, area.width() - 24)));
    adjustSize();
    int x = area.right() - width() - 11, y = area.bottom() - height() - 11;
    if (!trayGeometry.isEmpty()) {
        x = trayGeometry.center().x() - width() / 2;
        y = trayGeometry.center().y() < area.center().y() ? trayGeometry.bottom() + 8 : trayGeometry.top() - height() - 8;
    }
    x = qBound(area.left(), x, qMax(area.left(), area.right() - width() + 1));
    y = qBound(area.top(), y, qMax(area.top(), area.bottom() - height() + 1));
    move(x, y);
    show();
    m_activity->start();
    GUI_INFO("notification: %s target=%s screen=%s position=%d,%d tray_anchor=%d waiting_for_activity",
             replacing ? "updated" : "shown", qPrintable(target), qPrintable(screen->name()), x, y, int(!trayGeometry.isEmpty()));
}

void MessageNotification::dismiss(const char* reason) {
    if (isVisible()) GUI_INFO("notification: hidden target=%s reason=%s", qPrintable(m_target), reason);
    m_hideTimer.stop();
    m_activity->stop();
    hide();
}

void MessageNotification::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        const QString target = m_target;
        dismiss("open-chat");
        emit activated(target);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void MessageNotification::hideEvent(QHideEvent* event) {
    m_hideTimer.stop();
    m_activity->stop();
    QWidget::hideEvent(event);
}

void MessageNotification::closeEvent(QCloseEvent* event) {
    dismiss("close-event");
    event->ignore();
}
