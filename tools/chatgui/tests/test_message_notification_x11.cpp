/* Ручная проверка overlay на X11/XWayland: нет родительского окна и привязки WM к одному рабочему столу. */
#include "../src/message_notification.h"
#include "../../../lib/debug_config.h"
#include <QtTest>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

class NotificationDesktopTest : public QObject {
    Q_OBJECT
    Display* m_display = nullptr;

    bool isDesktopOverlay(Window window) {
        XWindowAttributes attributes{};
        if (!XGetWindowAttributes(m_display, window, &attributes)) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "notification desktop test: no attributes window=0x%lx", window);
            return false;
        }
        Window root = 0, parent = 0;
        Window* children = nullptr;
        unsigned int childCount = 0;
        if (!XQueryTree(m_display, window, &root, &parent, &children, &childCount)) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "notification desktop test: no parent info window=0x%lx", window);
            return false;
        }
        if (children) XFree(children);
        Atom type = None;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char* data = nullptr;
        const Atom atom = XInternAtom(m_display, "_NET_CLIENT_LIST", False);
        const int status = XGetWindowProperty(m_display, root, atom, 0, 4096, False,
                                              XA_WINDOW, &type, &format, &count, &remaining, &data);
        bool managed = false;
        if (status == Success && type == XA_WINDOW && format == 32 && data) {
            const auto* windows = reinterpret_cast<const Window*>(data);
            for (unsigned long i = 0; i < count; ++i) if (windows[i] == window) { managed = true; break; }
        }
        if (data) XFree(data);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "notification desktop test: window=0x%lx mapped=%d override_redirect=%d root_parent=%d managed=%d",
                   window, attributes.map_state, int(attributes.override_redirect), int(parent == root), int(managed));
        return attributes.map_state == IsViewable && attributes.override_redirect && parent == root && !managed;
    }

private slots:
    void initTestCase() {
        debug_config_init();
        debug_set_level(DEBUG_LEVEL_DEBUG);
        m_display = XOpenDisplay(nullptr);
        QVERIFY(m_display);
    }
    void cleanupTestCase() { if (m_display) XCloseDisplay(m_display); }

    void allDesktopsOnFirstShowUpdateAndReshow() {
        MessageNotification notification;
        notification.showMessage("42", "Alice", "Desktop test", {});
        const Window window = Window(notification.winId());
        QTest::qWait(200); // Проверяем состояние после обработки MapRequest оконным менеджером.
        QVERIFY(isDesktopOverlay(window));
        QVERIFY(!notification.parentWidget());
        QVERIFY(notification.windowFlags() & Qt::WindowDoesNotAcceptFocus);
        notification.showMessage("dm:7", "Bob", "Desktop test", {});
        QCOMPARE(Window(notification.winId()), window);
        QVERIFY(isDesktopOverlay(window));

        notification.dismiss("test-reshow");
        QTest::qWait(50);
        notification.showMessage("43", "Charlie", "Desktop test", {});
        QTest::qWait(200);
        QVERIFY(isDesktopOverlay(Window(notification.winId())));
        notification.dismiss("test-complete");
    }
};

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("DISPLAY")) return 77;
    qputenv("QT_QPA_PLATFORM", "xcb");
    QApplication application(argc, argv);
    NotificationDesktopTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_message_notification_x11.moc"
